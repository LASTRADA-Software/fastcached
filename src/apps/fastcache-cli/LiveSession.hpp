// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "DashboardEvent.hpp"
#include "DashboardLoop.hpp"
#include "LiveEventSource.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Clock.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache::Cli
{

/// @file LiveSession.hpp
/// One `live-stats` session: decide how it draws, once, then run the loop.

/// How a session draws, decided from what the terminal can do.
///
/// **Asked once per session, never per frame** (#134 §9.12). What a terminal can do is
/// settled by asking it -- a DA1 reply, a mode report -- and a frame that asked again would
/// interleave those queries with the frames it draws, once per frame. A terminal does not
/// grow Sixel mid-session, so the answer is not worth re-asking either.
/// A seam rather than a call, so that "once" is COUNTED by a test instead of being a
/// property of where a line happens to sit.
///
/// Production's implementation maps the capabilities the started terminal reported
/// through the rung chooser to the view for that rung. A run with no terminal has one rung
/// and decides it the same way, which keeps the count honest for both compositions.
class IViewLadder
{
  public:
    IViewLadder() = default;
    IViewLadder(IViewLadder const&) = delete;
    IViewLadder(IViewLadder&&) = delete;
    IViewLadder& operator=(IViewLadder const&) = delete;
    IViewLadder& operator=(IViewLadder&&) = delete;
    virtual ~IViewLadder() = default;

    /// Decide how this session draws.
    /// @return The view every frame of the session is drawn through; never null.
    [[nodiscard]] virtual std::unique_ptr<IDashboardView> Decide() = 0;
};

/// Run one session: decide the view, then run the loop over @p events with it.
///
/// Pointers rather than references, because this is a coroutine and its frame outlives
/// the call that created it. None may be null.
/// @param events Where the session's events come from.
/// @param ladder How the session draws; asked exactly once, before the first event.
/// @param sink Where frames go.
/// @param limits What bounds the session.
/// @return How the session ended.
[[nodiscard]] Task<DashboardExit> RunLiveSession(IDashboardEventSource* events,
                                                 IViewLadder* ladder,
                                                 IFrameSink* sink,
                                                 DashboardLimits limits);

/// How the process leaves once a session has ended and its source has been drained, or has
/// not been.
struct SessionEnding
{
    /// The exit code the session earned (#134 §1.7). **The drain never changes it**: a session
    /// that showed readings and then could not wait out one last scrape still showed readings.
    int exitCode { 0 };

    /// What to write to stderr before ending the process WITHOUT unwinding, or nullopt when the
    /// source drained and the ordinary path returns.
    ///
    /// Without unwinding, because a sample still inside `Gather()` holds the gatherer, the pool
    /// and the connections the caller's stack owns: returning destroys them under that thread,
    /// which is a use-after-free presenting as a rare crash on exit. The bound is spent; say
    /// what is abandoned and end the process.
    std::optional<std::string> abandonment {};
};

/// Decide how the process leaves, from how the drain ended.
///
/// Pure, so the decision is tested apart from the one call that acts on it.
/// @param drain How the drain ended.
/// @param earned The session's outcome, which decides the exit code whatever the drain did.
/// @param endpoint Where the samples were asked, as the abandonment line should name it.
/// @param sampleAge How long the sample still out had been out, or nullopt when none was.
/// @return The exit code, and the abandonment line when there is one.
[[nodiscard]] SessionEnding DecideSessionEnding(DrainResult drain,
                                                Outcome earned,
                                                std::string_view endpoint,
                                                std::optional<Duration> sampleAge);

/// Wait for a closed session's source to drain, within @p bound, and decide how the process leaves.
///
/// **`DrainWithin` -- this tree's one bounded shutdown drain -- on the CALLING thread, which
/// must not be the reactor's.** The reactor has to keep running for a returning sample to land,
/// and a poll on it would stall exactly that; on a thread with nothing else to do at shutdown it
/// is how every other drain here waits.
/// @param drained Set on the reactor once `source.Drained()` has resumed.
/// @param source The closed source, asked how long a sample still out has been out.
/// @param earned The session's outcome.
/// @param endpoint Where the samples were asked.
/// @param bound The ceiling, and how often to look.
/// @param wait The drain's clock and its sleep; injected so a test spends no real time.
/// @return How the process leaves.
[[nodiscard]] SessionEnding DrainSession(std::atomic<bool> const& drained,
                                         LiveEventSource const& source,
                                         Outcome earned,
                                         std::string_view endpoint,
                                         DrainBound bound,
                                         IDrainWait& wait);

} // namespace FastCache::Cli
