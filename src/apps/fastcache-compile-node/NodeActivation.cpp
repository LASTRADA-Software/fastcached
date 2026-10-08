// SPDX-License-Identifier: Apache-2.0
#include "NodeActivation.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <format>

namespace FastCache::Node
{

NodeRefusal UnreadableActivatedBindRefusal()
{
    // The text the start gave before the bind was read off the socket, kept for the one case that
    // still cannot be answered: a socket that will not say where it listens leaves this node with
    // nothing to advertise but a guess, and a guess registers, heartbeats and sends every client to
    // an address nothing answers.
    return Refusal(NodeRefusalCause::HandedOverListeners,
                   "--advertise is required under socket activation: the socket unit owns the "
                   "port, so this worker cannot know what address clients should use");
}

std::expected<void, NodeRefusal> AdoptActivatedBind(NodeConfig& cfg, BoundEndpoint const& bound)
{
    // A socket listening on port 0 or on no address is no socket a client can reach, and the Node
    // surface would hold a bind no row resolves -- so nothing is adopted, and the start says why
    // rather than advertising `--listen-node`'s default port, which the unit does not serve.
    if (bound.host.empty() || bound.port == 0)
        return std::unexpected { Refusal(NodeRefusalCause::HandedOverListeners,
                                         std::format("the socket a supervisor handed over reports it is bound to "
                                                     "'{}' port {}, which is no address a client can dial",
                                                     bound.host,
                                                     bound.port)) };
    cfg.nodeListen = FormatHostPort(bound.host, bound.port);
    return {};
}

std::expected<void, NodeRefusal> AdoptActivatedBind(NodeConfig& cfg, std::optional<BoundEndpoint> const& bound)
{
    if (!bound.has_value())
        return std::unexpected { UnreadableActivatedBindRefusal() };
    return AdoptActivatedBind(cfg, *bound);
}

} // namespace FastCache::Node
