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
} // namespace

Task<std::expected<LiveSessionRun, Answer>> RunComposedSession(LiveSessionParts parts,
                                                               std::optional<LiveEventSource>* source)
{
    auto run = LiveSessionRun {};
    auto sourceParts = LiveSourceParts { .reactor = parts.reactor,
                                         .gatherer = parts.gatherer,
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
    assert(view != nullptr && "every rung has a view");

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

} // namespace FastCache::Cli
