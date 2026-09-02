// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "Responders.hpp"

#include <FastCache/Core/Logger.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>

namespace FastCache::Node
{

class NodeIoLoop;
struct NodeConfig;

/// This node's one `0xFC` listener, and the router in front of it.
///
/// The two used to be four things: a cache surface, a scheduler surface, a worker's
/// own accept loop, and no router at all -- because the listener a frame arrived on WAS
/// the routing decision. One listener cannot decide that by existing, so
/// `MergedResponder` decides it per frame and this owns the pair (#290).
///
/// The worker's dedicated port is gone (#290 stage 3): every compile arrives here too,
/// spending the same slot accounting through `CompileResponder`.
///
/// Owned as one object for the reason `CacheTier` and `SchedulerTier` are: the two
/// members form a reference chain whose declaration order is load-bearing and silently
/// so. The endpoint holds a reference to the responder, so the responder is declared
/// first and destroyed last.
class NodeFrameSurface
{
  public:
    /// @param cache Answers the cache verbs, or nullptr when this node holds no tier.
    /// @param scheduler Answers the scheduler verbs and owns the credential, or
    ///        nullptr when this node does not schedule.
    /// @param compile Answers the compile verbs, or nullptr when this node runs no
    ///        worker. All three must outlive this.
    NodeFrameSurface(IFrameResponder* cache, IFrameResponder* scheduler, IFrameResponder* compile) noexcept:
        _responder { cache, scheduler, compile }
    {
    }

    ~NodeFrameSurface() = default;

    NodeFrameSurface(NodeFrameSurface const&) = delete;
    NodeFrameSurface& operator=(NodeFrameSurface const&) = delete;
    NodeFrameSurface(NodeFrameSurface&&) = delete;
    NodeFrameSurface& operator=(NodeFrameSurface&&) = delete;

    /// Bind the listener, or adopt one a supervisor handed over, and start serving.
    /// @param io The loop this surface accepts and answers on.
    /// @param cfg What the operator asked for; the surface row resolves the address,
    ///        except under socket activation, where the unit chose it and `cfg` can
    ///        only supply the `--advertise` host clients are told to dial.
    /// @param inherited A descriptor a supervisor already bound and listened, or
    ///        `std::nullopt` for the ordinary path. Ownership passes to the listener
    ///        built from it, including when that fails.
    /// @param metrics Where the endpoint's own at-capacity refusal is counted.
    /// @param logger Where to announce the bound address.
    /// @return Nothing, or why it could not be served.
    [[nodiscard]] std::expected<void, std::string> Bind(
        NodeIoLoop& io, NodeConfig const& cfg, std::optional<int> inherited, IMetricsSink& metrics, ILogger& logger);

    /// The address this node's `0xFC` listener bound.
    /// @return The endpoint, or an empty string before `Bind` succeeded.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        static std::string const none;
        return _endpoint == nullptr ? none : _endpoint->BoundEndpoint();
    }

    /// What routes each frame to the component owning its verb family.
    /// @return The router.
    [[nodiscard]] MergedResponder& Responder() noexcept
    {
        return _responder;
    }

  private:
    // Declaration order IS construction order, and the endpoint below holds a
    // reference to the responder above it.
    MergedResponder _responder;
    std::unique_ptr<FrameEndpoint> _endpoint;
};

/// Open this node's `0xFC` listener, or say why there is none.
///
/// Three outcomes, the same shape `StartCacheTierOrExplain` has and for the same
/// reason -- `main.cpp` is in no test target, and the judgement below is worth stating
/// once and testing:
///
///   - **A surface** — it is serving.
///   - **Success carrying nothing** — deliberately no listener, because the operator
///     emptied `--listen-node`, or because this node has no component for any verb
///     family at all. Both are configurations, not failures; a line has been logged.
///   - **An error** — an address that was asked for could not be served, so startup
///     must stop.
///
/// **What a bind failure DOES is the row's answer, not this function's** (#352):
/// `RowFor(NodeSurface::Node)` states the policy and the reasoning behind it, and
/// this header deliberately does not restate either. A copy here would go stale the
/// moment the column moved, and no build would notice.
///
/// What stays here is what the row cannot say: #290 stage 3 is why the question is
/// open at all. A taken DEFAULT port used to be tolerated, on reasoning that was
/// sound while the worker had a compile port of its OWN to fall back to. There is one
/// 0xFC port now, so `nodeListenExplicit` and `--serve-scheduler` stopped deciding
/// this -- both were answering "is this port load-bearing". The provenance bit is
/// still what `--install-service` emits on, which is where #286 needed it.
///
/// @param io The loop this surface accepts and answers on.
/// @param cfg What the operator asked for.
/// @param cache Answers the cache verbs, or nullptr when this node holds no tier.
/// @param scheduler Answers the scheduler verbs, or nullptr when it does not schedule.
/// @param compile Answers the compile verbs, or nullptr when this node runs no worker.
///        In this binary it is never null -- a node compiles, that is what it is -- so
///        the "no component at all" outcome below is reachable only from a test. The
///        predicate stays honest rather than being narrowed to the two that can still
///        be absent: a component this function stopped asking about is a listener
///        opened for verbs nobody answers.
///
///        **A worker with no tier and no scheduler still opens this port**, because
///        since #290 stage 3 it is the only place its compiles can arrive. That is the
///        row's answer rather than this one's: `NodeSurfaceTable()` resolves the node
///        surface unconditionally, so `--print-surfaces` names the same address this
///        binds.
/// @param metrics Where the endpoint's own at-capacity refusal is counted.
/// @param logger Where the bound address, or the tolerated failure, is announced.
/// @return The surface, a null surface meaning "carry on without one", or the reason.
[[nodiscard]] std::expected<std::unique_ptr<NodeFrameSurface>, std::string> StartNodeSurfaceOrExplain(
    NodeIoLoop& io,
    NodeConfig const& cfg,
    IFrameResponder* cache,
    IFrameResponder* scheduler,
    IFrameResponder* compile,
    std::optional<int> inherited,
    IMetricsSink& metrics,
    ILogger& logger);

} // namespace FastCache::Node
