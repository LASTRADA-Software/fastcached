// SPDX-License-Identifier: Apache-2.0
#include "CliFormat.hpp"
#include "DashboardPanel.hpp"
#include "LiveSession.hpp"
#include "TerminalCapabilities.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <semaphore>
#include <string>
#include <string_view>
#include <utility>

#include <core/async/ResumeOn.hpp>

namespace FastCache::Cli
{

namespace
{
    /// Closes a source when it goes out of scope, however that happens.
    class CloseOnExit
    {
      public:
        explicit CloseOnExit(LiveEventSource* source) noexcept:
            _source { source }
        {
        }

        CloseOnExit(CloseOnExit const&) = delete;
        CloseOnExit(CloseOnExit&&) = delete;
        CloseOnExit& operator=(CloseOnExit const&) = delete;
        CloseOnExit& operator=(CloseOnExit&&) = delete;

        ~CloseOnExit()
        {
            _source->Close();
        }

      private:
        LiveEventSource* _source;
    };

    /// What the reactor hands back to the thread waiting for a session.
    struct SessionResult
    {
        std::optional<std::expected<LiveSessionRun, Answer>> run {}; ///< How it ended, when it did not throw.
        std::exception_ptr failure {};                               ///< What it threw, when it did.
        std::shared_ptr<ITerminalRestore> restore {};                ///< The acquired terminal's, or null.
    };

    /// Remarks kept for the end of a session: an interactive one, whose stderr is its screen.
    class KeptRemarks final: public IRemarkSink
    {
      public:
        /// @param into Where they are kept; outlives this.
        explicit KeptRemarks(std::vector<std::string>* into) noexcept:
            _into { into }
        {
        }

        void Remark(std::string_view line) override
        {
            _into->emplace_back(line);
        }

      private:
        std::vector<std::string>* _into;
    };

    /// @p text as one line: each run of line breaks is `; `, and none leads or trails.
    ///
    /// A reason can carry a server's whole body -- a follower's `503` names its leader in several
    /// comment lines -- and a remark is one line on stderr, where a line break would read as a
    /// second remark.
    /// @param text The reason.
    /// @return The line, or the words saying there was none.
    [[nodiscard]] std::string OneLine(std::string_view text)
    {
        auto line = std::string {};
        auto broken = false;
        for (auto const ch: text)
        {
            if (ch == '\n' || ch == '\r')
            {
                broken = !line.empty();
                continue;
            }
            if (broken)
                line += "; ";
            broken = false;
            line += ch;
        }
        return line.empty() ? std::string { "no reason was given" } : line;
    }

    /// A session that never started.
    /// @param answer Why.
    /// @return The ending.
    [[nodiscard]] SessionEnding RefusedEnding(Answer answer)
    {
        return SessionEnding {
            .kind = SessionEndKind::Refused, .answer = std::move(answer), .line = {}, .restore = nullptr
        };
    }

    /// Run a composed session on its reactor, and release @p done when it is over.
    ///
    /// A `core::async::DetachedTask` because nothing on the reactor awaits it: the waiter is a thread. The
    /// catch is what keeps that thread from waiting forever -- a detached coroutine that let an
    /// exception out would terminate without releasing anything -- and the exception is carried
    /// to the waiter rather than swallowed.
    /// @param parts What to compose from; its reactor is where this runs.
    /// @param source Where the running source is kept.
    /// @param out Where the result goes.
    /// @param done Released once `out` is written.
    /// @return The detached task.
    core::async::DetachedTask RunOnReactor(LiveSessionParts parts,
                                           std::optional<LiveEventSource>* source,
                                           SessionResult* out,
                                           std::binary_semaphore* done)
    {
        co_await core::async::ResumeOn { *parts.reactor };
        try
        {
            out->run = co_await RunComposedSession(std::move(parts), source, &out->restore);
        }
        catch (...)
        {
            out->failure = std::current_exception();
        }
        done->release();
    }
} // namespace

core::async::Task<std::expected<LiveSessionRun, Answer>> RunComposedSession(LiveSessionParts parts,
                                                                            std::optional<LiveEventSource>* source,
                                                                            std::shared_ptr<ITerminalRestore>* restore)
{
    auto run = LiveSessionRun {};
    auto const& subject = LiveSubjectTable[static_cast<std::size_t>(parts.plan.subject)];
    auto sourceParts = LiveSourceParts { .reactor = parts.reactor,
                                         .subscription = parts.subscription,
                                         .subject = subject.wire,
                                         .endpoint = parts.endpoint,
                                         .dashboardToken = std::move(parts.dashboardToken),
                                         .pool = parts.streamPool,
                                         .clock = parts.clock,
                                         .interval = parts.plan.interval,
                                         .terminal = nullptr,
                                         .frames = nullptr,
                                         .stop = nullptr,
                                         .stopWaiter = nullptr };
    auto rung = RenderRung::Piped;

    // No subject is refused here for want of a panel: every row names one (`EverySubjectHasAPanel`),
    // so acquiring the terminal is never spent on a session that then cannot draw its subject.
    if (parts.interactive)
    {
        auto started = co_await parts.terminals->Acquire(parts.terminalPool, parts.reactor);
        // `Local`, not `Refused`: nothing at the server declined anything, and the remedy is on
        // this machine. A script reading 4 would go and look at the node.
        if (!started.has_value())
            co_return std::unexpected(
                Concluded(Outcome::Local,
                          std::format("cannot draw live-stats on this terminal: {}; run it with its output "
                                      "redirected for one line per sample instead",
                                      started.error())));
        rung = ChooseRenderRung(started->capabilities);
        // Kept apart from the events, and before anything else can go wrong: see the parameter.
        *restore = std::move(started->restore);
        sourceParts.terminal = std::move(started->events);
        // The source owns the presenter beside the events, and releases it first: see
        // `LiveSourceParts::frames`.
        sourceParts.frames = std::move(started->frames);
        if (sourceParts.frames == nullptr)
        {
            // Never drawn to stdout instead: that terminal is in raw mode on the alternate screen.
            // Closed first, and released, as a terminal nothing will read must be.
            sourceParts.terminal->Close();
            sourceParts.terminal.reset();
            co_return std::unexpected(Concluded(Outcome::Local,
                                                "cannot draw live-stats on this terminal: it was started with "
                                                "nowhere to draw frames; run it with its output redirected for one "
                                                "line per sample instead"));
        }
    }
    else if (auto installed = parts.stops->Install(); installed.has_value())
    {
        sourceParts.stop = *std::move(installed);
        sourceParts.stopWaiter = parts.stopWaiter;
    }
    else
    {
        // Not a refusal: without it Ctrl-C still ends the process, with the platform's own
        // status rather than the one the samples earned, and that is worth a sentence rather
        // than a session nobody can start.
        run.remarks.push_back(std::format("Ctrl-C will end this run without its exit status: {}", installed.error()));
    }

    // Once, and before the first event: see `IRungViews`.
    auto const view = parts.views->For(rung, parts.plan, EndpointText(parts.endpoint));
    if (view == nullptr)
    {
        // Released before refusing, closed first as its contract asks: nothing reads it, and an
        // acquired terminal left in raw mode is the most visible way this command can fail.
        sourceParts.frames.reset();
        if (sourceParts.terminal != nullptr)
        {
            sourceParts.terminal->Close();
            sourceParts.terminal.reset();
        }
        if (rung == RenderRung::Piped)
            co_return std::unexpected(
                Concluded(Outcome::Local, std::format("live-stats {} has no record to stream in this build", subject.key)));
        co_return std::unexpected(Concluded(Outcome::Local,
                                            std::format("cannot draw live-stats {} on this terminal: this build has "
                                                        "no panel for it; run it with its output redirected for one "
                                                        "line per sample instead",
                                                        subject.key)));
    }

    source->emplace(std::move(sourceParts));
    auto const closeOnExit = CloseOnExit { &**source };
    auto kept = KeptRemarks { &run.remarks };
    assert((parts.interactive || parts.remarks != nullptr) && "a piped session names where its remarks go");
    auto remarking = FailureRemarks { &**source, parts.reader, parts.interactive ? &kept : parts.remarks };
    // An interactive session draws on the terminal's presenter and never on the pipe.
    auto* const terminalFrames = (*source)->Frames();
    auto* const sink = terminalFrames != nullptr ? terminalFrames : parts.sink;
    run.exit =
        co_await RunDashboard(&remarking, parts.reader, view.get(), sink, DashboardLimits { .samples = parts.plan.samples });
    co_return run;
}

FailureRemarks::FailureRemarks(IDashboardEventSource* events, SampleReader reader, IRemarkSink* sink) noexcept:
    _events { events },
    _reader { reader },
    _sink { sink }
{
}

core::async::Task<DashboardEvent> FailureRemarks::Next()
{
    auto event = co_await _events->Next();
    Observe(event);
    co_return event;
}

void FailureRemarks::Close() noexcept
{
    _events->Close();
}

void FailureRemarks::Observe(DashboardEvent const& event)
{
    auto reason = std::optional<std::string> {};
    switch (event.kind)
    {
        case DashboardEventKind::Sample:
            if (auto reading = _reader(event); reading.outcome != Outcome::Affirmative)
                reason = std::move(reading.note);
            break;
        case DashboardEventKind::SampleFailed:
            reason = event.note;
            break;
        case DashboardEventKind::Tick:
        case DashboardEventKind::Key:
        case DashboardEventKind::Resize:
        case DashboardEventKind::Detached:
        case DashboardEventKind::StopRequested:
        case DashboardEventKind::Last:
            // Not a sample: whether samples are failing has not changed.
            return;
    }

    // The reason verbatim: each producer writes a sentence that stands alone, naming who refused or
    // where the stream broke. A prefix of this class's own misnamed the commonest reason -- a refusal
    // is an answer, not a sample that read nothing.
    if (reason.has_value() && reason != _failing)
        _sink->Remark(OneLine(*reason));
    _failing = std::move(reason);
}

std::string DescribeAbandonment(std::string_view endpoint, std::optional<core::platform::SteadyDuration> readAge)
{
    if (!readAge.has_value())
        return std::format("gave up waiting for the live-stats session to finish: no read of the stream from {} was "
                           "out, so a terminal read or the stop watch did not end",
                           endpoint);
    return std::format("gave up waiting for the stream from {} to close: its read had been out for {} ms; the exit "
                       "status is what the session had already earned",
                       endpoint,
                       std::chrono::duration_cast<std::chrono::milliseconds>(*readAge).count());
}

std::optional<std::string> DrainSession(LiveEventSource const& source, DrainBound bound, IDrainWait& wait)
{
    if (DrainWithin([&source] { return !source.IsDrained(); }, bound, wait) == DrainResult::Drained)
        return std::nullopt;
    auto const since = source.ReadOutstandingSince();
    return DescribeAbandonment(source.StreamingEndpoint(),
                               since.has_value() ? std::optional<core::platform::SteadyDuration> { wait.Now() - *since }
                                                 : std::nullopt);
}

StandardRungViews::StandardRungViews(RenderOptions render, CellWidth cellWidth, ISixelEncoder* sixel):
    _render { std::move(render) },
    _cellWidth { cellWidth },
    _sixel { sixel }
{
    assert(_cellWidth != nullptr && "a panel is laid out through the one width function the session is handed");
}

std::unique_ptr<IDashboardView> StandardRungViews::For(RenderRung rung, LivePlan const& plan, std::string_view address)
{
    auto const& row = LiveSubjectTable[static_cast<std::size_t>(plan.subject)];
    if (rung == RenderRung::Piped)
        return row.figures == nullptr
                   ? nullptr
                   : std::make_unique<PipedRecordView>(_render.format, _render.absentOverride, row.figures);

    // Never null: `EverySubjectHasAPanel`.
    auto const panel = row.panel;
    // A panel is drawn for a person, so its absent marker is the human format's unless the
    // operator named one: the same text on every rung (§9.6).
    auto absent = _render.absentOverride.value_or(
        std::string { FormatTable[static_cast<std::size_t>(OutputFormat::Human)].absentText });
    return std::make_unique<PanelView>(
        panel(),
        PanelContext { .absent = std::move(absent),
                       .endpoint = std::string { address },
                       .server = std::string { RemoteKindTable[static_cast<std::size_t>(plan.server)].product },
                       .interval = plan.interval,
                       .cellWidth = _cellWidth,
                       .sixel = _sixel,
                       .rung = rung });
}

SessionEnding RunLiveStatsSession(VerbContext const& context, LiveSessionSeat const& seat)
{
    auto plan = AdmitLiveStats(context);
    if (!plan.has_value())
        return RefusedEnding(std::move(plan).error());

    // Every subject streams through the one door, so there is no second door to be missing and
    // nothing to refuse for want of one: whether this caller may watch is the node's answer.
    assert(seat.subscription != nullptr && "every session reads the stream `main` dialled");
    auto const& subject = LiveSubjectTable[static_cast<std::size_t>(plan->subject)];

    auto parts = LiveSessionParts {
        .plan = *std::move(plan),
        .endpoint = seat.endpoint,
        .dashboardToken = seat.dashboardToken,
        .reactor = seat.reactor,
        .subscription = seat.subscription,
        .reader = subject.reader,
        .clock = seat.clock,
        .streamPool = seat.streamPool,
        .stopWaiter = seat.stopWaiter,
        .terminalPool = seat.terminalPool,
        .sink = seat.sink,
        .remarks = seat.remarks,
        // The format does not change because the streams are a terminal (§1.6): only a human
        // run draws, and every other format streams its records wherever stdout goes.
        .interactive = seat.streamsInteractive && seat.render.format == OutputFormat::Human,
        .terminals = seat.terminals,
        .stops = seat.stops,
        .views = seat.views,
    };

    auto result = SessionResult {};
    auto done = std::binary_semaphore { 0 };
    RunOnReactor(std::move(parts), seat.source, &result, &done);
    done.acquire();

    if (result.run.has_value() && !result.run->has_value())
        return RefusedEnding(std::move(*result.run).error());

    // Closed by the composition on every way out, so what is left is waiting for what was
    // still running when it closed -- here, off the reactor, which must keep turning for it.
    auto abandoned = std::optional<std::string> {};
    if (seat.source->has_value())
        abandoned = DrainSession(**seat.source, seat.drainBound, *seat.drainWait);

    // A loop that threw earned nothing, and an abandonment must not unwind: it ends with the
    // outcome of a run that read nothing.
    auto run = result.run.has_value() ? **std::move(result.run) : LiveSessionRun {};
    auto answer = Concluded(run.exit.outcome);
    answer.advisories = std::move(run.remarks);

    if (abandoned.has_value())
        return SessionEnding { .kind = SessionEndKind::Abandoned,
                               .answer = std::move(answer),
                               .line = *std::move(abandoned),
                               .restore = std::move(result.restore) };
    if (result.failure)
        std::rethrow_exception(result.failure);
    // Ran: the caller destroys the source, whose events put the terminal back themselves.
    return SessionEnding { .kind = SessionEndKind::Ran, .answer = std::move(answer), .line = {}, .restore = nullptr };
}

void EndAbandonedSession(SessionEnding const& ending, IAbandonedExit& exit)
{
    assert(ending.kind == SessionEndKind::Abandoned);
    if (ending.restore != nullptr)
        ending.restore->RestoreNow();
    exit.Flush();
    exit.Say(ending.answer, ending.line);
    exit.Exit(ExitCodeOf(ending.answer.outcome));
}

} // namespace FastCache::Cli
