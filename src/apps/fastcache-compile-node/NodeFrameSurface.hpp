// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "FrameEndpoint.hpp"
#include "Responders.hpp"

#include <FastCache/Core/Logger.hpp>

#include <expected>
#include <memory>
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
/// The worker's dedicated port has not gone: `WorkerServer` still accepts on it, and
/// retiring it is a step of its own. What changed is that a compile can now arrive here
/// too, spending the same slot accounting through `CompileResponder`.
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

    /// Bind the listener and start serving it.
    /// @param io The loop this surface accepts and answers on.
    /// @param cfg What the operator asked for; the surface row resolves the address.
    /// @param logger Where to announce the bound address.
    /// @return Nothing, or why it could not be served.
    [[nodiscard]] std::expected<void, std::string> Bind(NodeIoLoop& io, NodeConfig const& cfg, ILogger& logger);

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
///   - **Success carrying nothing** — deliberately no listener, because this node has
///     no component for any verb family at all, or because a DEFAULT address was
///     already taken. The node continues; a line has already been logged.
///   - **An error** — the operator NAMED an address, or asked for a scheduler, and it
///     could not be served, so startup must stop.
///
/// **The tolerated case narrowed when the surfaces merged (#290), and that is a
/// consequence worth stating rather than discovering.** A default cache port already
/// held by a `fastcached` on the same machine was, and still is, a warning: the
/// launcher reaches that daemon instead and the build works. But this listener now
/// also carries the scheduler verbs, and a scheduler that is not listening is the
/// "silently cannot work" shape -- so `--serve-scheduler` makes the same failure
/// fatal. One flag decides it, rather than the bind failure being judged twice by two
/// components that would each have been right about half of it.
///
/// Judged on the PROVENANCE bit, never on the value: comparing against the default
/// reads `--listen-node=127.0.0.1:6674` as a convenience nobody asked for, and the
/// node came up healthy serving no cache (#286).
///
/// @param io The loop this surface accepts and answers on.
/// @param cfg What the operator asked for.
/// @param cache Answers the cache verbs, or nullptr when this node holds no tier.
/// @param scheduler Answers the scheduler verbs, or nullptr when it does not schedule.
/// @param compile Answers the compile verbs, or nullptr when this node runs no worker.
///        In this binary it is never null -- a node compiles, that is what it is -- so
///        the "no component at all" outcome below is now reachable only from a test.
///        The predicate stays honest rather than being narrowed to the two that can
///        still be absent: a component this function stopped asking about is a listener
///        opened for verbs nobody answers.
/// @param logger Where the bound address, or the tolerated failure, is announced.
/// @return The surface, a null surface meaning "carry on without one", or the reason.
[[nodiscard]] std::expected<std::unique_ptr<NodeFrameSurface>, std::string> StartNodeSurfaceOrExplain(
    NodeIoLoop& io,
    NodeConfig const& cfg,
    IFrameResponder* cache,
    IFrameResponder* scheduler,
    IFrameResponder* compile,
    ILogger& logger);

} // namespace FastCache::Node
