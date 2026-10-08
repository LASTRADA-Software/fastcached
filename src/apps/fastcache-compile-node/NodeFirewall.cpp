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
    // of that protocol this program opens: for UDP, discovery's, and the route probe's, which only
    // connects and never receives (`ctest -R udp-opener`).
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

StateDirectoryFormationReader::StateDirectoryFormationReader(std::optional<std::filesystem::path> directory):
    _directory { std::move(directory) }
{
}

std::expected<KeptFormation, std::string> StateDirectoryFormationReader::Read() const
{
    if (!_directory.has_value())
        return std::unexpected { std::string { "this node has no state directory to keep its formation record in" } };
    auto const& directory = *_directory;
    Cluster::FileFormationStore const store { directory };
    Cluster::FleetEndpointsFile endpoints { directory };
    return ReadKeptFormation(store, endpoints).transform_error([&directory](std::string const& error) {
        return error + StateFileUnreadableHint(directory / Cluster::FormationRecordFileName);
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
                                                std::function<ServiceControlResult()> const& removeService,
                                                NodeConfig const& cfg,
                                                std::filesystem::path const& program,
                                                std::string_view serviceName,
                                                IKeptFormationReader const& formation,
                                                IFirewall* firewall)
{
    auto registered = registerService();
    if (!Registered(registered.outcome))
        return registered;

    // A registration THIS install created is removed again, so the SCM starts no service the
    // install refused -- the MSI starts the node straight after it, whatever it answered. On
    // launchd that is not quite so: `LaunchdInstall` kickstarts a Created auto-start job BEFORE
    // this judgement, so the job runs briefly until it is booted out here, and that run refuses
    // to start for the same reason the install was refused. One it only RE-APPLIED is an
    // upgrade's and stays, refusing to start for the same reason. The refusal is the verdict's
    // ending (@p outcome) unless the removal itself did not go through, which leaves a
    // registration nobody asked for: that is a failure, and says how to remove it.
    auto const refused = [&](ServiceControlOutcome outcome, std::string_view why) {
        auto registration = RefusedRegistration::Kept;
        if (registered.outcome == ServiceControlOutcome::Created)
            registration =
                NoneRemains(removeService().outcome) ? RefusedRegistration::Removed : RefusedRegistration::NotRemoved;
        // A rule's sentence may end in its own full stop; the one added here is the only one.
        auto const sentence = why.ends_with('.') ? why.substr(0, why.size() - 1) : why;
        auto const refusal = std::format("refusing the install: {}. No firewall rule was opened, since the service's "
                                         "surfaces follow that configuration",
                                         sentence);
        return ServiceControlResult {
            .outcome = registration == RefusedRegistration::NotRemoved ? ServiceControlOutcome::Failed : outcome,
            .message = std::format("{}\n{}", registered.message, RefusedInstallMessage(refusal, serviceName, registration)),
        };
    };
    // A record that could not be read may be read at the next attempt; a configuration the rules
    // refuse is refused again, a decision.
    auto const shaped = ShapedForService(cfg, formation);
    if (!shaped.has_value())
        return refused(ServiceControlOutcome::Failed, shaped.error());
    if (auto const rejection = NodeInstallRejection(*shaped))
        return refused(ServiceControlOutcome::Declined, *rejection);
    return WithRegistrationFirewall(std::move(registered), firewall, serviceName, NodeFirewallRules(*shaped, program));
}

ServiceControlResult InstallNodeService(NodeConfig const& merged,
                                        NodeConfig const& registration,
                                        IConfigPathProbe const& probe,
                                        std::filesystem::path const& program,
                                        std::function<ServiceControlResult(ServiceSpec const&)> const& install,
                                        std::function<ServiceControlResult(ServiceSpec const&)> const& uninstall,
                                        IFirewall* firewall)
{
    auto const spec = MakeNodeServiceSpec(program, registration, probe);
    StateDirectoryFormationReader const formation { RegisteredStateDirectory(registration, probe) };
    return InstallWithServiceFirewall([&install, &spec] { return install(spec); },
                                      [&uninstall, &spec] { return uninstall(spec); },
                                      merged,
                                      spec.exePath,
                                      spec.serviceName,
                                      formation,
                                      firewall);
}

} // namespace FastCache::Node
