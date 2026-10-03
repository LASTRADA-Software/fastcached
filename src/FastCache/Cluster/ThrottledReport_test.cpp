// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ThrottledReport.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>

#include <core/platform/Clock.hpp>

using namespace FastCache::Cluster;
using namespace std::chrono_literals;

TEST_CASE("A throttled report is said at once, then at most once an interval, counting what it stands for",
          "[cluster][throttle]")
{
    core::platform::ManualClock clock;
    auto report = ThrottledReport { 60s };

    CHECK(report.Note(clock.now()) == std::optional<std::size_t> { 1 });
    clock.advance(15s);
    CHECK_FALSE(report.Note(clock.now()).has_value());
    clock.advance(15s);
    CHECK_FALSE(report.Note(clock.now()).has_value());
    clock.advance(30s);
    CHECK(report.Note(clock.now()) == std::optional<std::size_t> { 3 });
}

TEST_CASE("A throttled report counts without saying, and says what it counted at the next look", "[cluster][throttle]")
{
    // The caller that counts only a CHANGE: one counted inside the interval is said the first time
    // it looks after the interval, although nothing new happened then.
    core::platform::ManualClock clock;
    auto report = ThrottledReport { 60s };

    CHECK_FALSE(report.Due(clock.now()).has_value()); // nothing counted: nothing to say
    report.Count();
    CHECK(report.Due(clock.now()) == std::optional<std::size_t> { 1 });
    clock.advance(15s);
    report.Count();
    CHECK_FALSE(report.Due(clock.now()).has_value());
    clock.advance(45s);
    CHECK(report.Due(clock.now()) == std::optional<std::size_t> { 1 });
    CHECK_FALSE(report.Due(clock.now()).has_value());
}

TEST_CASE("A discarded count is not said, and the interval still runs", "[cluster][throttle]")
{
    core::platform::ManualClock clock;
    auto report = ThrottledReport { 60s };

    CHECK(report.Note(clock.now()) == std::optional<std::size_t> { 1 });
    clock.advance(15s);
    report.Count();
    report.Discard();
    clock.advance(60s);
    CHECK_FALSE(report.Due(clock.now()).has_value());
    CHECK(report.Note(clock.now()) == std::optional<std::size_t> { 1 });
}
