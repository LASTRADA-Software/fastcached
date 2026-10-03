// SPDX-License-Identifier: Apache-2.0
#include "NodeFirewall.hpp"
#include "NodeIdentity.hpp"
#include "NodeStateFiles.hpp"

#include <FastCache/Cluster/FleetEndpoints.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <utility>

#include <core/Ranges.hpp>

namespace FastCache::Node
{

namespace
{
    /// A surface's protocol, as the firewall spells it.
    struct ProtocolMapping
    {
        SurfaceProtocol surface;   ///< The surface table's spelling.
        FirewallProtocol firewall; ///< The firewall's.
    };

    constexpr auto ProtocolMappings = std::to_array<ProtocolMapping>({
        { .surface = SurfaceProtocol::Tcp, .firewall = FirewallProtocol::Tcp },
        { .surface = SurfaceProtocol::Udp, .firewall = FirewallProtocol::Udp },
    });

    /// A surface's port kind, as the firewall spells the ports a rule admits.
    struct PortKindMapping
    {
        SurfacePortKind surface;   ///< Who chooses the endpoint's port.
        FirewallPortKind firewall; ///< Which local ports the rule admits for it.
    };

    // A port the kernel chooses is one no port-scoped rule can name, so its rule admits every
    // local port -- still scoped to this program and its service, so it reaches only the sockets
    // of that protocol this program opens, which for UDP is discovery alone.
    constexpr auto PortKindMappings = EnumTable<SurfacePortKind, PortKindMapping> { {
        { .surface = SurfacePortKind::Fixed, .firewall = FirewallPortKind::Fixed },
        { .surface = SurfacePortKind::KernelChosen, .firewall = FirewallPortKind::Any },
    } };

    static_assert(RowsInEnumeratorOrder(PortKindMappings, [](PortKindMapping const& row) { return row.surface; }),
                  "every SurfacePortKind needs a firewall spelling, at its own index");
} // namespace

std::optional<FirewallProtocol> FirewallProtocolOf(SurfaceProtocol protocol) noexcept
{
    if (auto const* const mapping = core::findOrNull(ProtocolMappings, protocol, &ProtocolMapping::surface))
        return mapping->firewall;
    return std::nullopt;
}

FirewallPortKind FirewallPortKindOf(SurfacePortKind kind) noexcept
{
    return PortKindMappings[static_cast<std::size_t>(kind)].firewall;
}

std::vector<FirewallRule> NodeFirewallRules(NodeConfig const& cfg, std::filesystem::path const& program)
{
    std::vector<FirewallRule> rules;
    for (auto const& row: NodeSurfaceTable())
    {
        // Every row has one -- `NodeFirewall_test.cpp` walks the table to say so.
        auto const protocol = FirewallProtocolOf(row.protocol);
        if (!protocol.has_value())
            continue;
        for (auto const& endpoint: row.Resolve(cfg))
        {
            // Brackets off first: `[::1]` is loopback, spelled for a URL.
            if (IsLoopbackHost(HostOfEndpoint(endpoint.host)))
                continue;
            auto const surface =
                endpoint.role.empty() ? std::string { row.name } : std::format("{}-{}", row.name, endpoint.role);
            auto const localPort =
                FirewallLocalPort { .kind = FirewallPortKindOf(endpoint.portKind), .number = endpoint.port };
            rules.push_back(FirewallRule { .name = FirewallRuleName(cfg.serviceName, surface, *protocol, localPort),
                                           .group = FirewallGroupFor(cfg.serviceName),
                                           .program = program,
                                           .serviceName = cfg.serviceName,
                                           .protocol = *protocol,
                                           .localPort = localPort,
                                           .remoteAddresses = cfg.firewallAllow,
                                           .bindHost = endpoint.host });
        }
    }
    return rules;
}

StateDirectoryFormationReader::StateDirectoryFormationReader(NodeConfig const& cfg):
    _cfg { cfg }
{
}

std::expected<KeptFormation, std::string> StateDirectoryFormationReader::Read() const
{
    auto const directory = ChosenStateDirectory(_cfg);
    if (!directory.has_value())
        return std::unexpected { std::string { "this node has no state directory to keep its formation record in" } };
    Cluster::FileFormationStore const store { directory->path };
    Cluster::FleetEndpointsFile endpoints { directory->path };
    return ReadKeptFormation(store, endpoints).transform_error([&directory](std::string const& error) {
        return error + StateFileUnreadableHint(directory->path / Cluster::FormationRecordFileName);
    });
}

std::expected<NodeConfig, std::string> ShapedForService(NodeConfig const& cfg, IKeptFormationReader const& formation)
{
    auto kept = formation.Read();
    if (!kept.has_value())
        return std::unexpected { std::move(kept).error() };
    auto shaped = cfg;
    if (auto applied = ApplyFormation(shaped, ProspectiveRecord(*kept), kept->remembered); !applied.has_value())
        return std::unexpected { std::move(applied).error() };
    return shaped;
}

ServiceControlResult InstallWithServiceFirewall(std::function<ServiceControlResult()> const& registerService,
                                                NodeConfig const& cfg,
                                                std::filesystem::path const& program,
                                                std::string_view serviceName,
                                                IKeptFormationReader const& formation,
                                                IFirewall* firewall)
{
    auto registered = registerService();
    if (registered.outcome != ServiceControlOutcome::Done)
        return registered;
    auto const refused = [&registered](std::string_view why) {
        return ServiceControlResult {
            .outcome = ServiceControlOutcome::Failed,
            .message = std::format("{}\nrefusing the install: {}. No firewall rule was opened, since the service's "
                                   "surfaces follow that configuration; the service stays registered and refuses to "
                                   "start for the same reason. Fix it, then run --install-service again",
                                   registered.message,
                                   why),
        };
    };
    auto const shaped = ShapedForService(cfg, formation);
    if (!shaped.has_value())
        return refused(shaped.error());
    if (auto const rejection = NodeInstallRejection(*shaped))
        return refused(*rejection);
    return WithRegistrationFirewall(std::move(registered), firewall, serviceName, NodeFirewallRules(*shaped, program));
}

} // namespace FastCache::Node
