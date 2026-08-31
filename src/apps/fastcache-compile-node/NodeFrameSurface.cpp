// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeFrameSurface.hpp"
#include "NodeIoLoop.hpp"
#include "NodeSurfaces.hpp"

#include <format>
#include <utility>

namespace FastCache::Node
{

std::expected<void, std::string> NodeFrameSurface::Bind(NodeIoLoop& io, NodeConfig const& cfg, ILogger& logger)
{
    // The surface, not an address. Where a bare port lands is the row's answer --
    // loopback on a worker, the wildcard on a node that schedules -- so the address
    // bound here, the one an install-time refusal judges and the one
    // `--print-surfaces` prints are one computation rather than three that agree
    // today.
    auto started = FrameEndpoint::Start(io, NodeSurface::Node, cfg, _responder, logger);
    if (!started.has_value())
        return std::unexpected { started.error() };

    _endpoint = std::move(*started);
    return {};
}

std::expected<std::unique_ptr<NodeFrameSurface>, std::string> StartNodeSurfaceOrExplain(
    NodeIoLoop& io, NodeConfig const& cfg, IFrameResponder* cache, IFrameResponder* scheduler, ILogger& logger)
{
    // **Whether** this surface is served is the row's answer -- the same one
    // `--print-surfaces` reads -- so a worksheet cannot name a port this function then
    // declines to bind. **Which sentence** an operator gets is this function's own,
    // because a row resolving to nothing cannot say which of two reasons applied.
    //
    // Asked of the components that were actually BUILT rather than of the flags that
    // asked for them, which is the same rule `NodeCapacityOf` holds: a `--cache-dir`
    // that would not open has already stopped startup, and a tier the row expects but
    // that does not exist would leave this listener answering `UnimplementedVerb` to
    // every FETCH behind an open port.
    if (cache == nullptr && scheduler == nullptr)
    {
        logger.Logf(LogLevel::Info, "no cache tier and no scheduler; serving no 0xFC port");
        return std::unique_ptr<NodeFrameSurface> {};
    }
    if (RowFor(NodeSurface::Node).Resolve(cfg).empty())
    {
        // Reachable only by emptying the flag: every other way for the row to resolve
        // to nothing is a node with neither component, which the branch above caught.
        logger.Logf(LogLevel::Info, "--listen-node is empty; serving no 0xFC port");
        return std::unique_ptr<NodeFrameSurface> {};
    }

    auto surface = std::make_unique<NodeFrameSurface>(cache, scheduler);
    if (auto bound = surface->Bind(io, cfg, logger); bound.has_value())
        return surface;
    else
    {
        // Fatal when the operator NAMED the address, and fatal when they asked for a
        // scheduler whatever they named: the reasons are on `StartNodeSurfaceOrExplain`
        // in the header. A named address is a promise; a scheduler nobody can dial is a
        // fleet that looks configured and is not.
        if (cfg.nodeListenExplicit)
            return std::unexpected { std::format("--listen-node {}", bound.error()) };
        if (cfg.serveScheduler)
            return std::unexpected { std::format("--serve-scheduler needs the node port: {}", bound.error()) };

        // Never silent. The launcher will reach whatever else holds that port -- very
        // likely the daemon -- so the build still works, but "the cache quietly did
        // less than you configured" is the failure mode this codebase keeps a list
        // about.
        logger.Logf(
            LogLevel::Warn, "default node endpoint {}: {}; continuing without a 0xFC port", cfg.nodeListen, bound.error());
        return std::unique_ptr<NodeFrameSurface> {};
    }
}

} // namespace FastCache::Node
