// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace std::chrono_literals;

TEST_CASE("SteadyClock advances monotonically", "[clock]")
{
    FastCache::SteadyClock clock;
    auto const first = clock.Now();
    std::this_thread::sleep_for(1ms);
    auto const second = clock.Now();
    REQUIRE(second >= first);
}

TEST_CASE("ManualClock starts at the given time", "[clock]")
{
    auto const start = FastCache::TimePoint { 1234ms };
    FastCache::ManualClock clock { start };
    REQUIRE(clock.Now() == start);
}

TEST_CASE("ManualClock Advance moves the clock forward by the given duration", "[clock]")
{
    FastCache::ManualClock clock;
    auto const before = clock.Now();
    clock.Advance(500ms);
    REQUIRE(clock.Now() - before == 500ms);
}

TEST_CASE("ManualClock SetNow hard-sets the clock", "[clock]")
{
    FastCache::ManualClock clock;
    auto const target = FastCache::TimePoint { 99s };
    clock.SetNow(target);
    REQUIRE(clock.Now() == target);
}

TEST_CASE("ManualClock multiple Advances accumulate", "[clock]")
{
    FastCache::ManualClock clock;
    auto const before = clock.Now();
    clock.Advance(100ms);
    clock.Advance(200ms);
    clock.Advance(50ms);
    REQUIRE(clock.Now() - before == 350ms);
}
