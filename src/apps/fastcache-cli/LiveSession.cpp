// SPDX-License-Identifier: Apache-2.0
#include "LiveSession.hpp"
#include "TerminalCapabilities.hpp"

#include <cassert>
#include <chrono>
#include <format>
#include <utility>

namespace FastCache::Cli
{

namespace
{
    /// The ladder a composed session decides through: the rung from what the terminal reported,
    /// or `Piped` when there is no terminal.
    class RungLadder final: public IViewLadder
    {
      public:
        /// @param views What each rung draws through.
        /// @param capabilities What the terminal reported, or nullopt for a run with none.
        RungLadder(IRungViews* views, std::optional<TerminalCapabilities> capabilities) noexcept:
            _views { views },
            _capabilities { capabilities }
        {
        }

        [[nodiscard]] std::unique_ptr<IDashboardView> Decide() override
        {
            auto const rung = _capabilities.has_value() ? ChooseRenderRung(*_capabilities) : RenderRung::Piped;
            return _views->For(rung);
        }

      private:
        IRungViews* _views;
        std::optional<TerminalCapabilities> _capabilities;
    };

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

    /// Wait for @p hold's source to drain, and say so where another thread can read it.
    /// @param hold The session's hold; outlives this.
    DetachedTask WatchDrain(LiveSessionHold* hold)
    {
        co_await hold->source->Drained();
        hold->drained.store(true, std::memory_order_release);
    }
} // namespace

Task<DashboardExit> RunLiveSession(IDashboardEventSource* events,
                                   IViewLadder* ladder,
                                   IFrameSink* sink,
                                   DashboardLimits limits)
{
    // Before the loop and outside it: the loop draws per frame, and this must not.
    auto const view = ladder->Decide();
    assert(view != nullptr && "a view ladder always has a bottom rung");
    co_return co_await RunDashboard(events, view.get(), sink, limits);
}

SessionEnding DecideSessionEnding(DrainResult drain,
                                  Outcome earned,
                                  std::string_view endpoint,
                                  std::optional<Duration> sampleAge)
{
    auto ending = SessionEnding { .exitCode = ExitCodeOf(earned), .abandonment = std::nullopt };
    if (drain == DrainResult::Drained)
        return ending;

    // A sample that is out has an age, and the age is the part an operator can act on: a
    // scrape out for five seconds against a one-second timeout says the timeout is not being
    // honoured. With no sample out, something else did not finish -- and inventing an age for
    // it would send them to the wrong place.
    ending.abandonment =
        sampleAge.has_value()
            ? std::format("gave up waiting for a sample from {} that had been out for {} ms; the exit status is "
                          "what the session had already earned",
                          endpoint,
                          std::chrono::duration_cast<std::chrono::milliseconds>(*sampleAge).count())
            : std::format("gave up waiting for the live-stats session to finish: no sample from {} was out, so a "
                          "terminal read or the stop watch did not end",
                          endpoint);
    return ending;
}

Task<std::expected<LiveSessionRun, Answer>> RunComposedSession(LiveSessionParts parts, LiveSessionHold* hold)
{
    auto run = LiveSessionRun {};
    auto source = LiveSourceParts { .reactor = parts.reactor,
                                    .gatherer = parts.gatherer,
                                    .pool = parts.samplePool,
                                    .interval = parts.plan.interval,
                                    .terminal = nullptr,
                                    .stop = nullptr,
                                    .stopWaiter = nullptr };
    auto capabilities = std::optional<TerminalCapabilities> {};

    if (parts.interactive)
    {
        auto started = co_await parts.terminals->Acquire(parts.terminalPool, parts.reactor);
        if (!started.has_value())
            co_return std::unexpected(
                Concluded(Outcome::Refused,
                          std::format("cannot draw live-stats on this terminal: {}; run it with its output "
                                      "redirected for one line per sample instead",
                                      started.error())));
        capabilities = started->capabilities;
        source.terminal = std::move(started->events);
    }
    else if (auto installed = parts.stops->Install(); installed.has_value())
    {
        hold->stop = *std::move(installed);
        source.stop = hold->stop.get();
        source.stopWaiter = parts.stopWaiter;
    }
    else
    {
        // Not a refusal: without it Ctrl-C still ends the process, with the platform's own
        // status rather than the one the samples earned, and that is worth a sentence rather
        // than a session nobody can start.
        run.remarks.push_back(std::format("Ctrl-C will end this run without its exit status: {}", installed.error()));
    }

    hold->source.emplace(std::move(source));
    WatchDrain(hold);

    auto ladder = RungLadder { parts.views, capabilities };
    auto const closeOnExit = CloseOnExit { &*hold->source };
    run.exit =
        co_await RunLiveSession(&*hold->source, &ladder, parts.sink, DashboardLimits { .samples = parts.plan.samples });
    co_return run;
}

SessionEnding DrainSession(std::atomic<bool> const& drained,
                           LiveEventSource const& source,
                           Outcome earned,
                           std::string_view endpoint,
                           DrainBound bound,
                           IDrainWait& wait)
{
    auto const drain = DrainWithin([&drained] { return !drained.load(std::memory_order_acquire); }, bound, wait);
    auto const since = source.SampleOutstandingSince();
    auto const age = since.has_value() ? std::optional<Duration> { wait.Now() - *since } : std::nullopt;
    return DecideSessionEnding(drain, earned, endpoint, age);
}

} // namespace FastCache::Cli
