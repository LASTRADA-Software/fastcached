// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeFrameSurface.hpp"
#include "NodeIoLoop.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <format>
#include <optional>
#include <utility>

namespace FastCache::Node
{

std::expected<void, std::string> NodeFrameSurface::Bind(
    NodeIoLoop& io, NodeConfig const& cfg, std::optional<int> inherited, IMetricsSink& metrics, ILogger& logger)
{
    // Two factories over one constructor, and which one runs is decided by whether a
    // supervisor handed a descriptor over -- never by a flag. The environment says
    // it, and it says so unambiguously; a flag would be a second author of a fact the
    // process can already observe.
    //
    // On the ordinary path: the surface, not an address. Where a bare port lands is
    // the row's answer -- loopback on a worker, the wildcard on a node that schedules
    // -- so the address bound here, the one an install-time refusal judges and the one
    // `--print-surfaces` prints are one computation rather than three that agree
    // today. Under activation there is no such computation to do: the unit bound the
    // port, and `--advertise` -- mandatory there, and refused at startup when absent
    // -- is the only thing that can say where clients should go.
    auto started =
        inherited.has_value()
            ? FrameEndpoint::StartAdopted(
                  io, NodeSurface::Node, *inherited, HostOfEndpoint(AdvertisedEndpoint(cfg)), _responder, metrics, logger)
            : FrameEndpoint::Start(io, NodeSurface::Node, cfg, _responder, metrics, logger);
    if (!started.has_value())
        return std::unexpected { started.error() };

    _endpoint = std::move(*started);
    return {};
}

std::expected<std::unique_ptr<NodeFrameSurface>, std::string> StartNodeSurfaceOrExplain(NodeIoLoop& io,
                                                                                        NodeConfig const& cfg,
                                                                                        IFrameResponder* cache,
                                                                                        IFrameResponder* scheduler,
                                                                                        IFrameResponder* compile,
                                                                                        std::optional<int> inherited,
                                                                                        IMetricsSink& metrics,
                                                                                        ILogger& logger)
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
    if (cache == nullptr && scheduler == nullptr && compile == nullptr)
    {
        logger.Logf(LogLevel::Info, "no cache tier, no scheduler and no worker; serving no 0xFC port");
        return std::unique_ptr<NodeFrameSurface> {};
    }
    // Asked BEFORE the row, because under activation the row answers about a flag
    // that configured nothing. An operator who enables the `.socket` unit and leaves
    // `--listen-node` empty has not asked for a closed port -- the unit is the port --
    // so reading the row first would decline a handoff that had already happened and
    // leave the descriptor unserved.
    if (!inherited.has_value() && RowFor(NodeSurface::Node).Resolve(cfg).empty())
    {
        // **The row is the authority on WHETHER, and it cannot say WHY.** That is the
        // division this function's header states, and there is now exactly one way for
        // the row to answer nothing: an empty `--listen-node`.
        //
        // The second reason is GONE rather than unhandled. It used to be "neither a
        // cache tier nor a scheduler", which was a real state while a worker had a
        // compile port of its own to fall back on. Stage 3 retires that port, so a
        // dispatched compile arrives here and the row is served on every node that
        // runs -- see the row's own comment. A branch for it would be one no
        // configuration reaches, saying compiles are served somewhere that no longer
        // exists.
        logger.Logf(LogLevel::Info, "--listen-node is empty; serving no 0xFC port");
        return std::unique_ptr<NodeFrameSurface> {};
    }

    auto surface = std::make_unique<NodeFrameSurface>(cache, scheduler, compile);
    auto bound = surface->Bind(io, cfg, inherited, metrics, logger);
    if (bound.has_value())
        return surface;

    // **Fatal, whoever named the address and whatever else this node runs** -- and
    // WHY is the row's, not this function's (#352). `RowFor(NodeSurface::Node)`
    // carries the reasoning; repeating it here would put it back in the two places
    // the ticket is about, and the copy a maintainer editing this function reads is
    // the one no guard checks.
    //
    // What belongs here is what the row cannot say. A taken DEFAULT port used to be
    // tolerated, and correctly: the worker had a compile port of its own to fall back
    // to, so what was lost was a cache tier nobody had asked for. #290 stage 3 retired
    // that port, which deleted the branch's PREMISE rather than showing the branch
    // wrong -- so the provenance bit stopped deciding this and `--serve-scheduler`
    // stopped being the one flag that escalated it. Both were answering "is this port
    // load-bearing", and since the merge the answer is yes unconditionally.
    //
    // And the sentence below, which names the REMEDY rather than the diagnosis:
    // "cannot bind" is a wall at three in the morning, and the operator needs to be
    // told what almost certainly holds the port and that this node does not need it
    // (#229). Two readers, two texts -- the row answers a maintainer's "why is this
    // fatal", this answers an operator's "what do I do now".
    auto judged = JudgeBindFailure(
        RowFor(NodeSurface::Node),
        std::format("--listen-node: {}. this node opens exactly one 0xFC port, so without it there is nowhere for a "
                    "dispatched compile to arrive -- and it would still register with --scheduler and advertise an "
                    "address nothing answers, which every client meets as a failed connection and a silent local "
                    "compile. the usual cause is a fastcached holding that port on this machine: stop it, or give "
                    "--listen-node a port of its own. a node needs no daemon beside it -- it answers every verb the "
                    "daemon does, its cache verbs included",
                    bound.error()),
        logger);
    if (!judged.has_value())
        return std::unexpected { std::move(judged).error() };

    // Tolerated. Unreachable while this row is `Refuse`, and written rather than
    // asserted because the row is what decides it: a build that flips the column
    // must not also need this branch written.
    return std::unique_ptr<NodeFrameSurface> {};
}

} // namespace FastCache::Node
