// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliAnswer.hpp"
#include "CliEndpoint.hpp"
#include "DashboardEvent.hpp"
#include "DashboardGlyphs.hpp"
#include "DashboardLoop.hpp"
#include "DashboardRung.hpp"
#include "LiveEventSource.hpp"
#include "LivePipedView.hpp"
#include "LiveStats.hpp"
#include "LiveSubscriber.hpp"
#include "SixelEncoder.hpp"
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
    /// @param plan What was admitted: the subject whose panel is drawn, and the interval it states.
    /// @param address Where the stream is read, as a frame names it.
    /// @return Its view, or null for a rung this build has no view for -- which refuses the
    ///         session by name rather than drawing it through some other rung's view.
    [[nodiscard]] virtual std::unique_ptr<IDashboardView> For(RenderRung rung,
                                                              LivePlan const& plan,
                                                              std::string_view address) = 0;
};

/// Where a piped session's remarks go as they happen: stderr, in `main`.
class IRemarkSink
{
  public:
    IRemarkSink() = default;
    IRemarkSink(IRemarkSink const&) = delete;
    IRemarkSink(IRemarkSink&&) = delete;
    IRemarkSink& operator=(IRemarkSink const&) = delete;
    IRemarkSink& operator=(IRemarkSink&&) = delete;
    virtual ~IRemarkSink() = default;

    /// Tell the operator @p line now. Called on the reactor's thread.
    /// @param line One remark, without a newline.
    virtual void Remark(std::string_view line) = 0;
};

/// Tells an operator why samples fail: once per reason, never once per sample.
///
/// Forwards a session's events unchanged, and remarks on a sample that read nothing when it is the
/// first such sample since a reading -- or of the run -- or when its reason differs from the one
/// before it. A stream of absent rows then says why exactly as often as the why changes: a daemon
/// that is down for an hour is one line, not one per sample, and a daemon that recovers and fails
/// again is told about again.
///
/// **The reason is the decision's own account, never a second one**: `SampleReading::note` for a
/// `Sample` its reader could not read, asked of the very reader the loop is given (it is pure, so
/// both ask one question and get one answer), and `DashboardEvent::note` for a `SampleFailed`. It is
/// remarked verbatim, on one line, so each producer's note is a sentence that stands on its own.
class FailureRemarks final: public IDashboardEventSource
{
  public:
    /// @param events What to forward; outlives this.
    /// @param reader The session's reader.
    /// @param sink Where a remark goes; outlives this.
    FailureRemarks(IDashboardEventSource* events, SampleReader reader, IRemarkSink* sink) noexcept;

    [[nodiscard]] Task<DashboardEvent> Next() override;
    void Close() noexcept override;

  private:
    /// Remark on @p event when it is a failure worth one.
    /// @param event What is about to be forwarded.
    void Observe(DashboardEvent const& event);

    IDashboardEventSource* _events;
    SampleReader _reader;
    IRemarkSink* _sink;

    /// Why the previous sample read nothing, or nullopt when it read something or none was taken.
    std::optional<std::string> _failing {};
};

/// Everything a session is composed from. `main` acquires each part; nothing here does.
struct LiveSessionParts
{
    LivePlan plan {};                            ///< What was admitted.
    Endpoint endpoint {};                        ///< Where the session subscribes: `--addr`.
    std::string dashboardToken {};               ///< What a fleet subscription presents; empty for none.
    IReactor* reactor { nullptr };               ///< Where the session runs.
    ILiveSubscription* subscription { nullptr }; ///< The stream every reading arrives on.
    SampleReader reader { nullptr };             ///< What a sample says: the subject's reader.
    IClock* clock { nullptr };                   ///< What readings are stamped with: `SteadyClock`.
    IExecutor* streamPool { nullptr };           ///< Where a dial or a stream read blocks.
    IExecutor* stopWaiter { nullptr };           ///< Where the stop wait blocks: its own thread.
    IExecutor* terminalPool { nullptr };         ///< Where terminal reads block: its own thread.
    IFrameSink* sink { nullptr };                ///< Where a piped session's frames go; never an interactive one's.
    IRemarkSink* remarks { nullptr };            ///< Where a piped session's remarks go as they happen.
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
/// reported, listen to it, and draw every frame through ITS presenter -- on the alternate screen,
/// never to `parts.sink`; a terminal that cannot be acquired REFUSES the session naming why, and
/// never falls back to piped output -- that would change the output's shape under a script
/// somebody wrote by copying an interactive run. **Not interactive**: acquire no terminal at all,
/// install the stop request instead, and draw on the `Piped` rung to `parts.sink`.
///
/// **Why samples fail is remarked once per reason** (`FailureRemarks`): on `parts.remarks` as it
/// happens for a piped session, whose stderr is free, and into the run's remarks for an interactive
/// one, whose stderr is the screen being drawn on -- they are written once the terminal is back.
///
/// **The source is closed on every way the loop ends**, an exception included, because the
/// source drains only after a close and a terminal that went away on its own closes nothing.
///
/// @param parts What to compose from.
/// @param source Where the running source is kept, the caller's: a drain on another thread
///        still waits on it after this coroutine has returned. Left empty when the session was
///        refused before starting.
/// @param restore Where the acquired terminal's restore handle is kept, the caller's, set the
///        moment the terminal is acquired: an abandonment reaches for it after this coroutine
///        has returned, and a loop that threw returned nothing to carry it in. Left null when
///        no terminal was acquired.
/// @return How the session ended, or the refusal that kept it from starting.
[[nodiscard]] Task<std::expected<LiveSessionRun, Answer>> RunComposedSession(LiveSessionParts parts,
                                                                             std::optional<LiveEventSource>* source,
                                                                             std::shared_ptr<ITerminalRestore>* restore);

/// The stderr line for a closed session whose source did not drain within its bound.
///
/// A read that is out has an age, and the age is the part an operator can act on: closing the
/// session half-closes the stream, a node answers that by closing, and a read still out seconds
/// later says the node never did -- nor did the idle bound its grant set. With no read out
/// something else did not finish, and inventing an age for it would send them to the wrong place.
/// Pure, so the wording is tested apart from the drain.
/// @param endpoint Where the stream was read.
/// @param readAge How long the dial or read still out had been out, or nullopt when none was.
/// @return The line, naming what is being abandoned.
[[nodiscard]] std::string DescribeAbandonment(std::string_view endpoint, std::optional<Duration> readAge);

/// Wait for a closed session's source to drain, within @p bound.
///
/// **`DrainWithin` -- this tree's one bounded shutdown drain -- on the CALLING thread, which
/// must not be the reactor's.** The reactor has to keep running for a returning read to land,
/// and a poll on it would stall exactly that; on a thread with nothing else to do at shutdown it
/// is how every other drain here waits.
///
/// **On an abandonment the caller ends the process WITHOUT unwinding**, after restoring what it
/// changed, flushing, and writing the line: a read still inside `ILiveSubscription::Read` holds the
/// subscription, the pool and the connection the caller's stack owns, and returning destroys them
/// under that thread. It exits with the code the session already earned, which this never touches.
/// @param source The closed source; its `StreamingEndpoint()` is what the line names.
/// @param bound The ceiling, and how often to look.
/// @param wait The drain's clock and its sleep; injected so a test spends no real time. Its
///        clock must count from the reactor clock's epoch, which `steady_clock` does.
/// @return Nothing when the source drained, or the abandonment line when the bound was spent.
[[nodiscard]] std::optional<std::string> DrainSession(LiveEventSource const& source, DrainBound bound, IDrainWait& wait);

/// The views this build draws through: the subject's figures as a record stream on the `Piped`
/// rung, in `--format`, and the subject's panel on every interactive rung.
///
/// **A subject with nothing for a rung answers null**, which refuses the session by name. Drawing a
/// terminal session through the piped view instead would change the output's shape under a
/// script copied from a run at that terminal, which is the silent fall back §1.6 rules out.
///
/// **How wide text is arrives here, and every panel is laid out through it**: `main` binds
/// `TerminalCellWidth`, a test binds its fake. It is not looked up per view, so one session has one
/// opinion about where a terminal's right edge is.
class StandardRungViews final: public IRungViews
{
  public:
    /// @param render The `--format` and `--absent` the operator asked for.
    /// @param cellWidth How many cells text occupies on the terminal a panel is drawn on; not null.
    /// @param sixel What draws an image on the Sixel rung, or null for a build or session with none.
    StandardRungViews(RenderOptions render, CellWidth cellWidth, ISixelEncoder* sixel);

    [[nodiscard]] std::unique_ptr<IDashboardView> For(RenderRung rung,
                                                      LivePlan const& plan,
                                                      std::string_view address) override;

  private:
    RenderOptions _render;
    CellWidth _cellWidth;
    ISixelEncoder* _sixel;
};

/// Everything `main` acquires for a session, and nothing a session decides.
///
/// **The reactor must already be running, on a thread that is not the caller's**:
/// `RunLiveStatsSession` blocks its caller until the session ends and then drains on it, and
/// a drain on the reactor's own thread would stall the read it is waiting for.
struct LiveSessionSeat
{
    IReactor* reactor { nullptr };               ///< Running on a thread of its own.
    Endpoint endpoint {};                        ///< Where the session subscribes: `--addr`.
    std::string dashboardToken {};               ///< From `--dashboard-token-file`; empty for none.
    IClock* clock { nullptr };                   ///< What readings are stamped with: `SteadyClock`.
    ILiveSubscription* subscription { nullptr }; ///< The stream, dialled with this invocation's credential.
    IExecutor* streamPool { nullptr };           ///< Where a dial or a stream read blocks.
    IExecutor* stopWaiter { nullptr };           ///< Where the stop wait blocks.
    IExecutor* terminalPool { nullptr };         ///< Where terminal reads block.
    IFrameSink* sink { nullptr };                ///< Where a piped session's frames go: stdout.
    IRemarkSink* remarks { nullptr };            ///< Where a piped session's remarks go as they happen: stderr.
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

    /// `Abandoned` only: what puts the terminal back, or null when the session acquired none.
    ///
    /// **Null is the whole answer for a piped session, not a missing part**: it changed no
    /// terminal mode, so there is nothing to put back and nothing stands in for the handle. An
    /// interactive session's events are never destroyed on this ending -- the process ends without
    /// unwinding -- so this is the only thing that leaves the alternate screen and raw mode.
    std::shared_ptr<ITerminalRestore> restore {};
};

/// How a process whose session was abandoned ends: the three things `main` does, as a seam.
///
/// A seam because the ORDER is the property, and `main` is in no test target: the terminal is
/// put back before anything is written, so the stderr line lands on the operator's own screen
/// rather than on an alternate screen about to vanish, and a flush in raw mode cannot mangle it.
class IAbandonedExit
{
  public:
    IAbandonedExit() = default;
    IAbandonedExit(IAbandonedExit const&) = delete;
    IAbandonedExit(IAbandonedExit&&) = delete;
    IAbandonedExit& operator=(IAbandonedExit const&) = delete;
    IAbandonedExit& operator=(IAbandonedExit&&) = delete;
    virtual ~IAbandonedExit() = default;

    /// Flush what the session already wrote to stdout.
    virtual void Flush() = 0;

    /// Tell the operator: the answer's remarks, then what was abandoned.
    /// @param answer The outcome and remarks earned.
    /// @param line The abandonment line.
    virtual void Say(Answer const& answer, std::string_view line) = 0;

    /// End the process without unwinding. Returns only in a test.
    /// @param code The exit code the session earned.
    virtual void Exit(int code) = 0;
};

/// End a process whose session was abandoned: restore the terminal, flush, say why, exit.
///
/// **`RestoreNow` first, whenever there is a terminal**, and it is safe while a terminal read is
/// still parked on the pool, which is the usual state here. A piped session has no handle and
/// skips that step, and only that one.
/// @param ending An `Abandoned` ending.
/// @param exit How this process flushes, speaks and ends.
void EndAbandonedSession(SessionEnding const& ending, IAbandonedExit& exit);

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
