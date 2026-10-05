// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Logger.hpp>

#include <chrono>
#include <cstdint>
#include <string>

#include <core/async/Task.hpp>
#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/NetError.hpp>
#include <core/platform/Clock.hpp>

/// @file AcceptLoopReporter.hpp
/// How every accept loop in this tree carries out `core::net::AcceptErrorPolicy`'s verdicts.
///
/// **What a failed accept MEANS is core-cpp's** (`<core/net/AcceptPolicy.hpp>`, graduated from this
/// tree at 0.6.0): only a closed or dead listener ends a loop; one connection's failure is accepted
/// past, exhaustion is backed off on, and a failure nothing classifies is backed off on without end
/// and reported as the loop being degraded if it persists. **What it is REPORTED as is this tree's**,
/// and it is one class rather than five copies, because the five loops -- the node's 0xFC surface,
/// the Raft peer server, the admin surface, the daemon's cache binds and its Windows acceptor
/// threads -- must agree: a line at the same level, and one entry in the process's one registry
/// (`core::net::AcceptLoopHealth`) that `/healthz` and the node's conditions read.

namespace FastCache
{

/// What an accept loop does after one failed accept.
///
/// Private to the accept loops and their tests: never stored or sent.
enum class AcceptLoopNext : std::uint8_t
{
    AcceptAgain, ///< Accept again, after `AcceptLoopStep::delay`.
    End,         ///< The loop ends: its listener was closed.
    EndAndClose, ///< The loop ends on a dead listener, which it CLOSES, so its port refuses rather than queues.
};

/// One failed accept's answer, for the loop to carry out.
struct AcceptLoopStep
{
    std::chrono::milliseconds delay {}; ///< How long to wait before accepting again; zero is at once.
    AcceptLoopNext next {};             ///< What the loop does.
};

/// One accept loop's reporting: the policy's state, and where its lines and conditions go.
///
/// One per loop, owned by the loop's frame or thread; not thread-safe, since one loop asks it.
class AcceptLoopReporter
{
  public:
    /// @param logName The loop's name in its log lines (`raft: peer`, `admin`, `cache 0.0.0.0:6674`).
    /// @param surface The loop's name in the registry, the one `/healthz` and the conditions print.
    /// @param logger Where the lines go; must outlive this.
    /// @param health The process's registry; must outlive this.
    AcceptLoopReporter(std::string logName, std::string surface, ILogger& logger, core::net::AcceptLoopHealth& health);

    /// Answer one failed accept: log what is due, record a condition that changed, and say what next.
    ///
    /// A listener closed, or found dead, under a loop its owner was NOT stopping is reported as the
    /// loop giving up: the surface stopped accepting while it was meant to serve, which is what the
    /// registry exists to say. While its owner IS stopping, any accept that ends the loop is the stop
    /// arriving -- a dead handle included, which is how Windows answers an accept on a handle the
    /// teardown just closed -- so it is no news; and a loop that was degraded says it stopped, or the
    /// registry would go on showing a surface that was shut down as degraded.
    /// @param error What the accept answered.
    /// @param now The loop's clock.
    /// @param stopping Whether the loop's owner is shutting it down.
    /// @return What the loop does next.
    [[nodiscard]] AcceptLoopStep OnError(core::net::NetError const& error,
                                         core::platform::SteadyTimePoint now,
                                         bool stopping);

    /// One accept succeeded: the backoff starts again, and a degraded loop reports its recovery.
    /// @param now The loop's clock.
    void OnAccepted(core::platform::SteadyTimePoint now);

    /// The loop ended for its owner's reason, seen before an accept failed: a degraded loop says so.
    void OnLoopEnded();

  private:
    /// Log @p line at @p level and record it in the registry as @p kind.
    void Report(core::net::AcceptLoopEventKind kind, LogLevel level, std::string line, core::net::NetError error);

    core::net::AcceptErrorPolicy _policy;
    std::string _logName;
    std::string _surface;
    ILogger& _logger;
    core::net::AcceptLoopHealth& _health;
    bool _ended {}; ///< Whether the end was reported already, so a second report does not repeat it.
};

/// Wait out an accept backoff on whatever timer the loop has.
///
/// A loop on a reactor SUSPENDS on the reactor's own timer, which is its clock and a test loop's
/// too; a loop on a thread of its own with a blocking listener -- the admin surface -- BLOCKS
/// through its injected wait. Never a bare `sleep_for` on a reactor, which would stall every
/// connection the reactor carries for the length of the backoff.
/// @param reactor The loop's reactor, or nullptr for a loop that owns its thread.
/// @param blocking How a loop without a reactor waits; a pointer, since a coroutine
///        frame does not keep a reference parameter's referent alive.
/// @param delay How long.
/// @return A task that completes once the delay has passed.
[[nodiscard]] core::async::Task<void> WaitOutBackoff(core::net::EventLoop* reactor,
                                                     IDrainWait* blocking,
                                                     std::chrono::milliseconds delay);

} // namespace FastCache
