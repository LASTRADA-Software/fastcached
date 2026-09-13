// SPDX-License-Identifier: Apache-2.0
#include "LiveSession.hpp"

#include <cassert>
#include <chrono>
#include <format>

namespace FastCache::Cli
{

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
