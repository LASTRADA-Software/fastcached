// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "DashboardEvent.hpp"
#include "DashboardLoop.hpp"
#include "DashboardRung.hpp"
#include "LiveEventSource.hpp"
#include "LivePipedView.hpp"
#include "LiveStats.hpp"
#include "StatsSource.hpp"
#include "TerminalEvents.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Platform/StopSignal.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cli
{

/// @file LiveSession.hpp
/// One `live-stats` session: composed from parts `main` acquires, run, and drained.

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
///
/// **Asked once per session, never per frame** (#134 §9.12). What a terminal can draw is settled
/// by asking it -- a DA1 reply, a mode report -- and a frame that asked again would interleave
/// those queries with its own output; a terminal does not grow Sixel mid-session either. So a
/// composed session decides its rung once, and asks this for the rung's view once, before the
/// loop's first event: before, because an interactive session's first event is the terminal's
/// opening `Resize`, whose tick draws a frame. A seam rather than a call, so that "once" is
/// COUNTED by a test instead of being a property of where a line happens to sit.
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
    /// @return Its view, or null for a rung this build has no view for -- which refuses the
    ///         session by name rather than drawing it through some other rung's view.
    [[nodiscard]] virtual std::unique_ptr<IDashboardView> For(RenderRung rung) = 0;
};

/// Everything a session is composed from. `main` acquires each part; nothing here does.
struct LiveSessionParts
{
    LivePlan plan {};                            ///< What was admitted.
    IReactor* reactor { nullptr };               ///< Where the session runs.
    IStatsGatherer* gatherer { nullptr };        ///< What each sample asks, until one fails.
    IStatsDialer* dialer { nullptr };            ///< What re-dials after a failed sample.
    SampleReader reader { nullptr };             ///< What a sample says: the subject's reader.
    IClock* clock { nullptr };                   ///< What samples are stamped with: `SteadyClock`.
    IExecutor* samplePool { nullptr };           ///< Where a gather blocks.
    IExecutor* stopWaiter { nullptr };           ///< Where the stop wait blocks: its own thread.
    IExecutor* terminalPool { nullptr };         ///< Where terminal reads block: its own thread.
    IFrameSink* sink { nullptr };                ///< Where frames go.
    bool interactive { false };                  ///< `StandardStreamsAreInteractive()`, asked by `main`.
    ITerminalAcquisition* terminals { nullptr }; ///< Asked only when interactive.
    IStopSignalInstaller* stops { nullptr };     ///< Asked only when not.
    IRungViews* views { nullptr };               ///< What each rung draws through.
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
/// **The source is closed on every way the loop ends**, an exception included, because the
/// source drains only after a close and a terminal that went away on its own closes nothing.
///
/// @param parts What to compose from.
/// @param source Where the running source is kept, the caller's: a drain on another thread
///        still waits on it after this coroutine has returned. Left empty when the session was
///        refused before starting.
/// @return How the session ended, or the refusal that kept it from starting.
[[nodiscard]] Task<std::expected<LiveSessionRun, Answer>> RunComposedSession(LiveSessionParts parts,
                                                                             std::optional<LiveEventSource>* source);

/// The stderr line for a closed session whose source did not drain within its bound.
///
/// A sample that is out has an age, and the age is the part an operator can act on: a scrape
/// out for five seconds against a one-second timeout says the timeout is not being honoured.
/// With no sample out something else did not finish, and inventing an age for it would send
/// them to the wrong place. Pure, so the wording is tested apart from the drain.
/// @param endpoint Where the samples were asked.
/// @param sampleAge How long the sample still out had been out, or nullopt when none was.
/// @return The line, naming what is being abandoned.
[[nodiscard]] std::string DescribeAbandonment(std::string_view endpoint, std::optional<Duration> sampleAge);

/// Wait for a closed session's source to drain, within @p bound.
///
/// **`DrainWithin` -- this tree's one bounded shutdown drain -- on the CALLING thread, which
/// must not be the reactor's.** The reactor has to keep running for a returning sample to land,
/// and a poll on it would stall exactly that; on a thread with nothing else to do at shutdown it
/// is how every other drain here waits.
///
/// **On an abandonment the caller ends the process WITHOUT unwinding**, after restoring what it
/// changed, flushing, and writing the line: a sample still inside `Gather()` holds the gatherer,
/// the pool and the connections the caller's stack owns, and returning destroys them under that
/// thread. It exits with the code the session already earned, which this never touches.
/// @param source The closed source.
/// @param endpoint Where the samples were asked.
/// @param bound The ceiling, and how often to look.
/// @param wait The drain's clock and its sleep; injected so a test spends no real time. Its
///        clock must count from the reactor clock's epoch, which `steady_clock` does.
/// @return Nothing when the source drained, or the abandonment line when the bound was spent.
[[nodiscard]] std::optional<std::string> DrainSession(LiveEventSource const& source,
                                                      std::string_view endpoint,
                                                      DrainBound bound,
                                                      IDrainWait& wait);

/// The views this build draws through: the `Piped` rung's record stream, in `--format`.
///
/// **The interactive rungs answer null**, which refuses an interactive session by name. Drawing a
/// terminal session through the piped view instead would change the output's shape under a
/// script copied from a run at that terminal, which is the silent fall back §1.6 rules out. The
/// Sixel, Unicode and ASCII views are the panels' own and take their rows here when they land.
class StandardRungViews final: public IRungViews
{
  public:
    /// @param render The `--format` and `--absent` the operator asked for.
    /// @param project What a row reports.
    StandardRungViews(RenderOptions render, FigureProjection project);

    [[nodiscard]] std::unique_ptr<IDashboardView> For(RenderRung rung) override;

  private:
    RenderOptions _render;
    FigureProjection _project;
};

/// Everything `main` acquires for a session, and nothing a session decides.
///
/// **The reactor must already be running, on a thread that is not the caller's**:
/// `RunLiveStatsSession` blocks its caller until the session ends and then drains on it, and
/// a drain on the reactor's own thread would stall the sample it is waiting for.
struct LiveSessionSeat
{
    IReactor* reactor { nullptr };               ///< Running on a thread of its own.
    IClock* clock { nullptr };                   ///< What samples are stamped with: `SteadyClock`.
    IStatsDialer* dialer { nullptr };            ///< What re-dials the endpoint after a failed sample.
    IExecutor* samplePool { nullptr };           ///< Where a gather blocks.
    IExecutor* stopWaiter { nullptr };           ///< Where the stop wait blocks.
    IExecutor* terminalPool { nullptr };         ///< Where terminal reads block.
    IFrameSink* sink { nullptr };                ///< Where frames go: stdout.
    bool streamsInteractive { false };           ///< `StandardStreamsAreInteractive()`.
    RenderOptions render {};                     ///< The `--format` and `--absent` asked for.
    ITerminalAcquisition* terminals { nullptr }; ///< Asked only for an interactive session.
    IStopSignalInstaller* stops { nullptr };     ///< Asked only for a piped one.
    IRungViews* views { nullptr };               ///< What each rung draws through.
    IDrainWait* drainWait { nullptr };           ///< The drain's clock and sleep.
    DrainBound drainBound {};                    ///< How long a closed session may take to drain.

    /// Where the running source is kept: the caller's.
    ///
    /// On an abandonment the process ends without unwinding, so the source must not live in
    /// a frame this returns from. On any other ending the caller destroys it after stopping
    /// the reactor, because an object a reactor owns dies with that reactor stopped.
    std::optional<LiveEventSource>* source { nullptr };
};

/// How a session verb ended, from `main`'s side.
///
/// TRANSMITTED/PERSISTED: no. Private; enumerators may be inserted.
enum class SessionEndKind : std::uint8_t
{
    Refused,   ///< Nothing ran: `answer` is reported like any verb's.
    Ran,       ///< The session ran and drained: report `answer`'s remarks and exit with its outcome.
    Abandoned, ///< The drain gave up: restore, flush, write `line`, and end with `answer`'s outcome.
    Last,
};

/// What `main` does once a session verb returns.
struct SessionEnding
{
    SessionEndKind kind { SessionEndKind::Refused }; ///< Which of the three.
    Answer answer {};                                ///< The refusal, or the outcome and remarks earned.
    std::string line {};                             ///< `Abandoned` only: the stderr line.
};

/// Run `live-stats`: admit, compose, run to the end, drain.
///
/// **Blocks the calling thread until the session ends, and drains on it** -- see
/// `LiveSessionSeat`. Admission refuses exactly as `LiveStatsVerb` does. A session is
/// interactive only when the standard streams are AND the format is `human`: the format does
/// not change because a stream is a terminal (§1.6), so `--format=json` at a terminal streams
/// NDJSON rather than drawing.
///
/// An exception from the loop is rethrown here, on the caller's thread, once the source has
/// drained.
/// @param context The invocation.
/// @param seat What `main` acquired.
/// @return How it ended.
[[nodiscard]] SessionEnding RunLiveStatsSession(VerbContext const& context, LiveSessionSeat const& seat);

} // namespace FastCache::Cli
