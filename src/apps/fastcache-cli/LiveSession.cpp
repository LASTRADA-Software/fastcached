// SPDX-License-Identifier: Apache-2.0
#include "LiveSession.hpp"
#include "TerminalCapabilities.hpp"

#include <FastCache/Async/ResumeOn.hpp>

#include <cassert>
#include <chrono>
#include <exception>
#include <format>
#include <semaphore>
#include <utility>

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
    };

    /// Run a composed session on its reactor, and release @p done when it is over.
    ///
    /// A `DetachedTask` because nothing on the reactor awaits it: the waiter is a thread. The
    /// catch is what keeps that thread from waiting forever -- a detached coroutine that let an
    /// exception out would terminate without releasing anything -- and the exception is carried
    /// to the waiter rather than swallowed.
    /// @param parts What to compose from; its reactor is where this runs.
    /// @param source Where the running source is kept.
    /// @param out Where the result goes.
    /// @param done Released once `out` is written.
    /// @return The detached task.
    DetachedTask RunOnReactor(LiveSessionParts parts,
                              std::optional<LiveEventSource>* source,
                              SessionResult* out,
                              std::binary_semaphore* done)
    {
        co_await ResumeOn { *parts.reactor };
        try
        {
            out->run = co_await RunComposedSession(std::move(parts), source);
        }
        catch (...)
        {
            out->failure = std::current_exception();
        }
        done->release();
    }
} // namespace

Task<std::expected<LiveSessionRun, Answer>> RunComposedSession(LiveSessionParts parts,
                                                               std::optional<LiveEventSource>* source)
{
    auto run = LiveSessionRun {};
    auto sourceParts = LiveSourceParts { .reactor = parts.reactor,
                                         .gatherer = parts.gatherer,
                                         .dialer = parts.dialer,
                                         .pool = parts.samplePool,
                                         .clock = parts.clock,
                                         .interval = parts.plan.interval,
                                         .terminal = nullptr,
                                         .stop = nullptr,
                                         .stopWaiter = nullptr };
    auto rung = RenderRung::Piped;

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
        sourceParts.terminal = std::move(started->events);
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
    auto const view = parts.views->For(rung);
    if (view == nullptr)
    {
        // Released before refusing, closed first as its contract asks: nothing reads it, and an
        // acquired terminal left in raw mode is the most visible way this command can fail.
        if (sourceParts.terminal != nullptr)
        {
            sourceParts.terminal->Close();
            sourceParts.terminal.reset();
        }
        co_return std::unexpected(
            Concluded(Outcome::Local,
                      "cannot draw live-stats on this terminal: the interactive views are not built yet; run "
                      "it with its output redirected for one line per sample instead"));
    }

    source->emplace(std::move(sourceParts));
    auto const closeOnExit = CloseOnExit { &**source };
    run.exit = co_await RunDashboard(
        &**source, parts.reader, view.get(), parts.sink, DashboardLimits { .samples = parts.plan.samples });
    co_return run;
}

std::string DescribeAbandonment(std::string_view endpoint, std::optional<Duration> sampleAge)
{
    if (!sampleAge.has_value())
        return std::format("gave up waiting for the live-stats session to finish: no sample from {} was out, so a "
                           "terminal read or the stop watch did not end",
                           endpoint);
    return std::format("gave up waiting for a sample from {} that had been out for {} ms; the exit status is "
                       "what the session had already earned",
                       endpoint,
                       std::chrono::duration_cast<std::chrono::milliseconds>(*sampleAge).count());
}

std::optional<std::string> DrainSession(LiveEventSource const& source,
                                        std::string_view endpoint,
                                        DrainBound bound,
                                        IDrainWait& wait)
{
    if (DrainWithin([&source] { return !source.IsDrained(); }, bound, wait) == DrainResult::Drained)
        return std::nullopt;
    auto const since = source.SampleOutstandingSince();
    return DescribeAbandonment(endpoint, since.has_value() ? std::optional<Duration> { wait.Now() - *since } : std::nullopt);
}

StandardRungViews::StandardRungViews(RenderOptions render, FigureProjection project):
    _render { std::move(render) },
    _project { project }
{
}

std::unique_ptr<IDashboardView> StandardRungViews::For(RenderRung rung)
{
    if (rung != RenderRung::Piped)
        return nullptr;
    return std::make_unique<PipedRecordView>(_render.format, _render.absentOverride, _project);
}

SessionEnding RunLiveStatsSession(VerbContext const& context, LiveSessionSeat const& seat)
{
    auto plan = AdmitLiveStats(context);
    if (!plan.has_value())
        return SessionEnding { .kind = SessionEndKind::Refused, .answer = std::move(plan).error(), .line = {} };

    // What `RunVerb` asks of every row before its handler, asked here because a session is
    // not reached through `RunVerb`: a sample would otherwise gather from nothing.
    if (context.stats == nullptr)
        return SessionEnding {
            .kind = SessionEndKind::Refused,
            .answer = Concluded(Outcome::Unreachable,
                                std::string { WireTable[static_cast<std::size_t>(Wire::Stats)].unavailable }),
            .line = {},
        };

    auto const& subject = LiveSubjectTable[static_cast<std::size_t>(plan->subject)];
    if (subject.reader == nullptr)
        return SessionEnding {
            .kind = SessionEndKind::Refused,
            .answer = Concluded(Outcome::Local,
                                std::format("live-stats {} cannot stream from this build: its sessions do not "
                                            "sample the leader's admin document yet",
                                            subject.key)),
            .line = {},
        };

    auto const endpoint = plan->endpoint;
    auto parts = LiveSessionParts {
        .plan = *std::move(plan),
        .reactor = seat.reactor,
        .gatherer = context.stats,
        .dialer = seat.dialer,
        .reader = subject.reader,
        .clock = seat.clock,
        .samplePool = seat.samplePool,
        .stopWaiter = seat.stopWaiter,
        .terminalPool = seat.terminalPool,
        .sink = seat.sink,
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
        return SessionEnding { .kind = SessionEndKind::Refused, .answer = std::move(*result.run).error(), .line = {} };

    // Closed by the composition on every way out, so what is left is waiting for what was
    // still running when it closed -- here, off the reactor, which must keep turning for it.
    auto abandoned = std::optional<std::string> {};
    if (seat.source->has_value())
        abandoned = DrainSession(**seat.source, endpoint, seat.drainBound, *seat.drainWait);

    // A loop that threw earned nothing, and an abandonment must not unwind: it ends with the
    // outcome of a run that read nothing.
    auto run = result.run.has_value() ? **std::move(result.run) : LiveSessionRun {};
    auto answer = Concluded(run.exit.outcome);
    answer.advisories = std::move(run.remarks);

    if (abandoned.has_value())
        return SessionEnding { .kind = SessionEndKind::Abandoned,
                               .answer = std::move(answer),
                               .line = *std::move(abandoned) };
    if (result.failure)
        std::rethrow_exception(result.failure);
    return SessionEnding { .kind = SessionEndKind::Ran, .answer = std::move(answer), .line = {} };
}

} // namespace FastCache::Cli
