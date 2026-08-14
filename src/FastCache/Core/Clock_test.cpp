// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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

TEST_CASE("CachedClock samples once at construction", "[clock]")
{
    // The value has to be usable before any event loop has run — startup
    // logging and config reload read the clock long before the first refresh.
    FastCache::ManualClock source { FastCache::TimePoint { 5s } };
    FastCache::CachedClock const clock { source };
    REQUIRE(clock.Now() == FastCache::TimePoint { 5s });
}

TEST_CASE("CachedClock holds its value until refreshed", "[clock]")
{
    // This is the whole point: between refreshes every reader gets a stored
    // value rather than paying for an OS clock read.
    FastCache::ManualClock source { FastCache::TimePoint { 1s } };
    FastCache::CachedClock clock { source };

    source.SetNow(FastCache::TimePoint { 60s });
    REQUIRE(clock.Now() == FastCache::TimePoint { 1s });

    clock.Refresh();
    REQUIRE(clock.Now() == FastCache::TimePoint { 60s });
}

TEST_CASE("CachedClock never moves backwards", "[clock]")
{
    // IClock promises monotonicity, and several reactors share one instance —
    // so a refresh that observes an older sample than one already published
    // must not win. A backwards jump would make a live entry look expired.
    FastCache::ManualClock source { FastCache::TimePoint { 100s } };
    FastCache::CachedClock clock { source };
    REQUIRE(clock.Now() == FastCache::TimePoint { 100s });

    source.SetNow(FastCache::TimePoint { 40s });
    clock.Refresh();
    REQUIRE(clock.Now() == FastCache::TimePoint { 100s });
}

TEST_CASE("CachedClock concurrent refreshes converge on the newest sample", "[clock]")
{
    // The production shape: N reactors refreshing one shared clock. Whatever
    // the interleaving, the published value must end up the newest sample and
    // must never be observed going backwards.
    FastCache::SteadyClock source;
    FastCache::CachedClock clock { source };

    std::atomic<bool> go { false };
    std::atomic<bool> regressed { false };
    constexpr int WorkerCount = 8;
    std::vector<std::jthread> workers;
    workers.reserve(WorkerCount);
    for (int worker = 0; worker < WorkerCount; ++worker)
    {
        workers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire))
                std::this_thread::yield();
            auto previous = clock.Now();
            for (int i = 0; i < 20'000; ++i)
            {
                clock.Refresh();
                auto const observed = clock.Now();
                if (observed < previous)
                    regressed.store(true, std::memory_order_relaxed);
                previous = observed;
            }
        });
    }
    go.store(true, std::memory_order_release);
    workers.clear(); // jthread joins on destruction

    CHECK_FALSE(regressed.load());
    auto const published = clock.Now();
    REQUIRE(published <= source.Now());
}

TEST_CASE("Refresh is a no-op for clocks that read their source directly", "[clock]")
{
    // SteadyClock and ManualClock inherit the default. A reactor calls Refresh()
    // unconditionally on whatever clock it was given, so this must be safe and
    // must not disturb a test's manual timeline.
    // Sampled into locals in a defined order: the two sides of a REQUIRE are
    // not sequenced, so comparing two Now() calls inline can read the later one
    // first and fail on a clock that is behaving perfectly.
    FastCache::SteadyClock steady;
    auto const before = steady.Now();
    steady.Refresh();
    auto const after = steady.Now();
    REQUIRE(after >= before);

    FastCache::ManualClock manual { FastCache::TimePoint { 7s } };
    manual.Refresh();
    REQUIRE(manual.Now() == FastCache::TimePoint { 7s });
}
