// SPDX-License-Identifier: Apache-2.0
#include "ToolchainHashProgress.hpp"

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{

constexpr std::chrono::milliseconds Interval { 1'000 };

/// Every captured line mentioning the hash rate.
/// @param logger Where the reporter wrote.
/// @return Those lines, in order.
[[nodiscard]] std::vector<std::string> RateLines(CapturingLogger const& logger)
{
    std::vector<std::string> out;
    for (auto const& record: logger.Snapshot())
        if (record.message.contains("hashing toolchain files:"))
            out.push_back(record.message);
    return out;
}

} // namespace

TEST_CASE("The hash reports nothing until its interval has elapsed", "[node][toolchain][progress]")
{
    // A warm walk of a whole Windows toolchain takes about two seconds, so the
    // ordinary case must produce no lines at all. An instrument that fills a CI log
    // on every healthy start is one an operator learns to scroll past, which is the
    // opposite of what #354 needs from it.
    ManualClock clock;
    CapturingLogger logger;
    ToolchainHashProgress progress { 100, Interval, clock, logger };

    for (auto i = 0; i < 50; ++i)
        progress.Observe();

    CHECK(progress.Done() == 50);
    CHECK(RateLines(logger).empty());
}

TEST_CASE("The hash reports one line per elapsed interval", "[node][toolchain][progress]")
{
    ManualClock clock;
    CapturingLogger logger;
    ToolchainHashProgress progress { 100, Interval, clock, logger };

    progress.Observe();
    CHECK(RateLines(logger).empty());

    clock.Advance(Interval);
    progress.Observe();
    CHECK(RateLines(logger).size() == 1);

    // Still inside the SECOND window, so nothing more -- the deadline moved when the
    // line was emitted rather than being re-armed per observation.
    progress.Observe();
    CHECK(RateLines(logger).size() == 1);

    clock.Advance(Interval);
    progress.Observe();
    CHECK(RateLines(logger).size() == 2);
}

TEST_CASE("The reported rate is the window's, not the average", "[node][toolchain][progress]")
{
    // The whole reason this is per-interval. A phase that does 900 files in its first
    // second and 10 in its second has an average that says 455 and a truth that says
    // it fell off a cliff -- and "fast then degrading" versus "flat and slow" is
    // exactly the distinction #354 needs, because they point at different causes.
    ManualClock clock;
    CapturingLogger logger;
    ToolchainHashProgress progress { 2'000, Interval, clock, logger };

    for (auto i = 0; i < 900; ++i)
        progress.Observe();
    clock.Advance(Interval);
    progress.Observe();

    auto lines = RateLines(logger);
    REQUIRE(lines.size() == 1);
    // 901 files in one second.
    CHECK(lines.front().contains("901 file/s over the last 1s"));

    for (auto i = 0; i < 9; ++i)
        progress.Observe();
    clock.Advance(Interval);
    progress.Observe();

    lines = RateLines(logger);
    REQUIRE(lines.size() == 2);
    // Ten in the second window -- and the cumulative figure, which is the evidence
    // rather than the verdict, still reads 455.
    CHECK(lines.back().contains("10 file/s over the last 1s"));
    CHECK(lines.back().contains("455 file/s since the start"));
}

TEST_CASE("Concurrent observers produce one line per interval, not one per thread", "[node][toolchain][progress]")
{
    // The walk is sixteen-wide on the machine this exists to diagnose. Sixteen lines
    // per interval would be a log nobody reads, and -- worse -- fifteen of them would
    // report a window of zero length, which reads as a rate of zero and looks exactly
    // like the wedge the instrument is meant to rule out.
    ManualClock clock;
    CapturingLogger logger;
    ToolchainHashProgress progress { 1'000, Interval, clock, logger };

    clock.Advance(Interval);

    std::vector<std::jthread> threads;
    threads.reserve(8);
    for (auto i = 0; i < 8; ++i)
        threads.emplace_back([&progress] {
            for (auto n = 0; n < 25; ++n)
                progress.Observe();
        });
    threads.clear();

    CHECK(progress.Done() == 200);
    CHECK(RateLines(logger).size() == 1);
}

TEST_CASE("A window of no elapsed time reports no rate rather than a huge one", "[node][toolchain][progress]")
{
    // A `ManualClock` that has not moved is an ordinary thing for a caller to present,
    // and dividing by it would print a rate that is arithmetic rather than a
    // measurement. Zero is the honest answer to "how fast over no time".
    ManualClock clock;
    CapturingLogger logger;
    ToolchainHashProgress progress { 10, std::chrono::milliseconds { 0 }, clock, logger };

    progress.Observe();

    auto const lines = RateLines(logger);
    REQUIRE(lines.size() == 1);
    CHECK(lines.front().contains("0 file/s over the last 0s"));
}
