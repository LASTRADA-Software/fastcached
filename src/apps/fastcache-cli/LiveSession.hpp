// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "DashboardEvent.hpp"
#include "DashboardLoop.hpp"
#include "DashboardRung.hpp"
#include "LiveEventSource.hpp"
#include "LiveStats.hpp"
#include "StatsSource.hpp"
#include "TerminalEvents.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Platform/StopSignal.hpp>

#include <atomic>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
/// Acquires the terminal an interactive session draws on.
///
/// A seam over `MakeTerminalEvents` then `StartTerminal`, so a composition is testable without
/// a terminal and so "a run with no terminal never acquires one" is COUNTED rather than read off
/// an `if`.
class ITerminalAcquisition
{
  public:
    ITerminalAcquisition() = default;
    ITerminalAcquisition(ITerminalAcquisition const&) = delete;
    ITerminalAcquisition(ITerminalAcquisition&&) = delete;
    ITerminalAcquisition& operator=(ITerminalAcquisition const&) = delete;
    ITerminalAcquisition& operator=(ITerminalAcquisition&&) = delete;
    virtual ~ITerminalAcquisition() = default;

    /// Acquire the terminal and learn what it can draw.
    /// @param pool Where acquiring and each blocking read run: a thread of its own.
    /// @param resumeOn Where the caller resumes.
    /// @return The started terminal, or why it could not be acquired.
    [[nodiscard]] virtual Task<std::expected<StartedTerminal, std::string>> Acquire(IExecutor* pool,
                                                                                    IExecutor* resumeOn) = 0;
};

/// Installs the stop request a session with no terminal ends on.
class IStopSignalInstaller
{
  public:
    IStopSignalInstaller() = default;
    IStopSignalInstaller(IStopSignalInstaller const&) = delete;
    IStopSignalInstaller(IStopSignalInstaller&&) = delete;
    IStopSignalInstaller& operator=(IStopSignalInstaller const&) = delete;
    IStopSignalInstaller& operator=(IStopSignalInstaller&&) = delete;
    virtual ~IStopSignalInstaller() = default;

    /// Install it; the disposition returns when the result is destroyed.
    /// @return The installed signal, or why none could be installed.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IStopSignal>, std::string> Install() = 0;
};

/// The view each rung draws through.
class IRungViews
{
  public:
    IRungViews() = default;
    IRungViews(IRungViews const&) = delete;
    IRungViews(IRungViews&&) = delete;
    IRungViews& operator=(IRungViews const&) = delete;
    IRungViews& operator=(IRungViews&&) = delete;
    virtual ~IRungViews() = default;

    /// @param rung The rung decided for this session.
    /// @return Its view; never null.
    [[nodiscard]] virtual std::unique_ptr<IDashboardView> For(RenderRung rung) = 0;
};

/// Everything a session is composed from. `main` acquires each part; nothing here does.
struct LiveSessionParts
{
    LivePlan plan {};                            ///< What was admitted.
    IReactor* reactor { nullptr };               ///< Where the session runs.
    IStatsGatherer* gatherer { nullptr };        ///< What each sample asks.
    IExecutor* samplePool { nullptr };           ///< Where a gather blocks.
    IExecutor* stopWaiter { nullptr };           ///< Where the stop wait blocks: its own thread.
    IExecutor* terminalPool { nullptr };         ///< Where terminal reads block: its own thread.
    IFrameSink* sink { nullptr };                ///< Where frames go.
    bool interactive { false };                  ///< `StandardStreamsAreInteractive()`, asked by `main`.
    ITerminalAcquisition* terminals { nullptr }; ///< Asked only when interactive.
    IStopSignalInstaller* stops { nullptr };     ///< Asked only when not.
    IRungViews* views { nullptr };               ///< What each rung draws through.
};

/// What a composed session leaves behind that must outlive it: the source a drain waits on, and
/// the stop signal whose disposition returns only once nothing waits on it.
///
/// Owned by the caller rather than by the session's coroutine, because the drain runs AFTER the
/// coroutine has returned, on another thread.
struct LiveSessionHold
{
    std::unique_ptr<IStopSignal> stop {};  ///< Destroyed after the drain; restores the disposition.
    std::optional<LiveEventSource> source; ///< Empty when the session was refused before starting.
    std::atomic<bool> drained { false };   ///< Set on the reactor once `source` has drained.
};

/// How a composed session ended, when it ran at all.
struct LiveSessionRun
{
    DashboardExit exit {};               ///< What the loop returned.
    std::vector<std::string> remarks {}; ///< What an operator should be told beside it.
};

/// Compose and run one `live-stats` session.
///
/// **Interactive**: acquire the terminal (on its own pool), decide the rung from what it
/// reported, and listen to it; a terminal that cannot be acquired REFUSES the session naming
/// why, and never falls back to piped output -- that would change the output's shape under a
/// script somebody wrote by copying an interactive run. **Not interactive**: acquire no
/// terminal at all, install the stop request instead, and draw on the `Piped` rung.
///
/// **The source is closed on every way the loop ends**, an exception included, because
/// `Drained()` completes only after a close and a terminal that went away on its own closes
/// nothing. Then the caller drains `hold` off the reactor.
/// @param parts What to compose from.
/// @param hold Where the parts that outlive the session are kept; the caller's.
/// @return How the session ended, or the refusal that kept it from starting.
[[nodiscard]] Task<std::expected<LiveSessionRun, Answer>> RunComposedSession(LiveSessionParts parts, LiveSessionHold* hold);

[[nodiscard]] SessionEnding DrainSession(std::atomic<bool> const& drained,
                                         LiveEventSource const& source,
                                         Outcome earned,
                                         std::string_view endpoint,
                                         DrainBound bound,
                                         IDrainWait& wait);

} // namespace FastCache::Cli
