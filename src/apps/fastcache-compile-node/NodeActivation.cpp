// SPDX-License-Identifier: Apache-2.0
#include "NodeActivation.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <format>

namespace FastCache::Node
{

NodeRefusal UnreadableActivatedBindRefusal()
{
    // The one case the socket cannot answer: it will not report an IP address and port -- a UNIX or
    // other non-IP socket, most often -- so its port is unknown, and the node surface would hold a
    // bind no row resolves while every endpoint derived from it is a guess that registers,
    // heartbeats and sends every client to an address nothing answers. Named whatever `--advertise`
    // says: the advertised HOST does not make the socket a TCP one this node can serve.
    return Refusal(NodeRefusalCause::HandedOverListeners,
                   "the socket a supervisor handed over does not report an IP address and port (a UNIX or other "
                   "non-IP socket?), so this node cannot serve it or say where clients reach it; give the socket "
                   "unit an IP ListenStream= (a port, or an address and a port)");
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
