// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Distributed/TicketVerifier.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

/// @file NodeAudience.hpp
/// Which endpoints a machine ticket may name to be spent at this node.

/// This node's own endpoints as a ticket audience names them: the advertised host and port (read per
/// call -- `--advertise` is reloadable), this machine's host name, the `Node` surface's resolved
/// ports, and any address `ILocalityOracle` calls this machine.
///
/// **Per call rather than captured**, because the advertised endpoint moves on a reload and a ticket
/// minted for the new one must be spendable the moment the registration names it. The decision is
/// `Distributed::AudienceMatches`, which also refuses loopback and wildcard hosts whatever this
/// node calls itself -- so nothing here can widen that rule.
class NodeAudience final: public Distributed::IAudience
{
  public:
    /// @param advertised Where this node tells other machines to dial it; must outlive this.
    /// @param hostName This machine's host name, as another machine may resolve it.
    /// @param ports The ports this node's `0xFC` surface is bound to.
    /// @param locality Which addresses are this machine's; must outlive this.
    NodeAudience(Cc::IAdvertisedEndpointSource const& advertised,
                 std::string hostName,
                 std::vector<std::uint16_t> ports,
                 ILocalityOracle const& locality) noexcept:
        _advertised { advertised },
        _hostName { std::move(hostName) },
        _ports { std::move(ports) },
        _locality { locality }
    {
    }

    /// The ports a ticket's audience may name for this node: those the `Node` surface binds under
    /// @p cfg, as `NodeSurfaceTable()` resolves them -- the same row `--print-surfaces` prints, so
    /// the audience and the listener cannot disagree about which ports are this node's.
    /// @param cfg The configuration this node started with.
    /// @return The resolved ports; empty when the node serves no `0xFC` surface.
    [[nodiscard]] static std::vector<std::uint16_t> PortsOf(NodeConfig const& cfg)
    {
        auto ports = std::vector<std::uint16_t> {};
        for (auto const& endpoint: RowFor(NodeSurface::Node).Resolve(cfg))
            ports.push_back(endpoint.port);
        return ports;
    }

    /// @copydoc Distributed::IAudience::Matches
    [[nodiscard]] bool Matches(std::string_view audience) const override
    {
        auto own = Distributed::OwnAudience { .names = {}, .ports = _ports };
        if (!_hostName.empty())
            own.names.push_back(_hostName);
        if (auto advertised = ParseDialEndpoint(_advertised.Current()); advertised.has_value())
        {
            own.names.push_back(std::move(advertised->first));
            own.ports.push_back(advertised->second);
        }
        return Distributed::AudienceMatches(audience, own, _locality);
    }

  private:
    Cc::IAdvertisedEndpointSource const& _advertised;
    std::string _hostName;
    std::vector<std::uint16_t> _ports;
    ILocalityOracle const& _locality;
};

} // namespace FastCache::Node
