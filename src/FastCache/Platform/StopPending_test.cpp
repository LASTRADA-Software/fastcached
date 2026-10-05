// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Platform/StopPending.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>

using namespace std::chrono_literals;

namespace
{

/// A wait that advances its own clock by what was asked and never blocks, recording the asks.
class CountingWait final: public FastCache::IDrainWait
{
  public:
    [[nodiscard]] core::platform::SteadyTimePoint Now() const noexcept override
    {
        return now;
    }

    void Sleep(std::chrono::milliseconds requested) noexcept override
    {
        now += requested;
        requests.push_back(requested);
    }

    core::platform::SteadyTimePoint now {};
    std::vector<std::chrono::milliseconds> requests; ///< Every gap asked for, in order.
};

} // namespace

TEST_CASE("A stop's wait hint covers the drain the binary performs, plus its teardown", "[platform][stop-pending]")
{
    // The node drains compiles for --drain-timeout before it tears down; a hint shorter than
    // that tells the SCM, and an MSI waiting on the stop, that the service hung.
    auto const node = FastCache::StopPendingPlanFor(std::chrono::seconds { 30 });
    CHECK(node.waitHint == 30s + FastCache::StopTeardownMargin);
    CHECK(node.checkpointEvery == FastCache::StopCheckpointInterval);

    // A binary with no drain of its own states nothing to wait for but its teardown.
    CHECK(FastCache::StopPendingPlanFor(std::nullopt).waitHint == FastCache::StopTeardownMargin);

    // `0s` is the node's "wait forever": no finite hint is honest, so the hint is the
    // teardown's and the CHECKPOINTS carry the rest.
    CHECK(FastCache::StopPendingPlanFor(std::chrono::seconds { 0 }).waitHint == FastCache::StopTeardownMargin);
}

TEST_CASE("A wait hint that would overflow a DWORD is saturated instead", "[platform][stop-pending]")
{
    // ~49.7 days is where a millisecond count stops fitting a DWORD; a 60-day drain is past it.
    CHECK(FastCache::StopPendingPlanFor(std::chrono::hours { 24 * 60 }).waitHint == FastCache::MaxServiceWaitHint);

    // The largest value --drain-timeout can be handed at all must not overflow computing the
    // hint, and must land on the same ceiling.
    CHECK(FastCache::StopPendingPlanFor(std::chrono::seconds::max()).waitHint == FastCache::MaxServiceWaitHint);
}

TEST_CASE("A stop reports one advancing checkpoint per interval until the body has returned", "[platform][stop-pending]")
{
    auto const plan = FastCache::StopPendingPlanFor(std::chrono::seconds { 30 });
    CountingWait wait;
    std::vector<std::pair<std::uint32_t, std::chrono::milliseconds>> reports;
    auto polls = 0;

    auto const reported = FastCache::ReportStopProgress(
        plan,
        [&polls] { return ++polls > 3; },
        [&reports](std::uint32_t checkPoint, std::chrono::milliseconds hint) { reports.emplace_back(checkPoint, hint); },
        wait);

    CHECK(reported == 3);
    REQUIRE(reports.size() == 3);
    CHECK(reports[0].first == 1);
    CHECK(reports[1].first == 2);
    CHECK(reports[2].first == 3);
    CHECK(reports[2].second == plan.waitHint);
    CHECK(wait.requests == std::vector<std::chrono::milliseconds>(3, plan.checkpointEvery));
}

TEST_CASE("A body that has already returned is reported nothing", "[platform][stop-pending]")
{
    CountingWait wait;
    auto calls = 0;
    auto const reported = FastCache::ReportStopProgress(
        FastCache::StopPendingPlanFor(std::nullopt),
        [] { return true; },
        [&calls](std::uint32_t, std::chrono::milliseconds) { ++calls; },
        wait);
    CHECK(reported == 0);
    CHECK(calls == 0);
    CHECK(wait.requests.empty());
}
