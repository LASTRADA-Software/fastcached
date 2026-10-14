// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/BoundedDrain.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>

#include <tests/OverrunningDrainWait.hpp>

using FastCache::DefaultDrainWait;
using FastCache::DrainBound;
using FastCache::DrainResult;
using FastCache::DrainWithin;
using FastCache::Testing::OverrunningDrainWait;

using namespace std::chrono_literals;

namespace
{

/// A predicate that reports busy for the first `n` calls and idle afterwards.
class BusyForCalls
{
  public:
    /// @param calls How many calls report outstanding work.
    explicit BusyForCalls(std::size_t calls) noexcept:
        _remaining { calls }
    {
    }

    /// @return True while calls remain.
    bool operator()() noexcept
    {
        if (_remaining == 0)
            return false;
        --_remaining;
        return true;
    }

  private:
    std::size_t _remaining;
};

} // namespace

TEST_CASE("DrainWithin: nothing outstanding drains without waiting", "[core][drain]")
{
    auto wait = OverrunningDrainWait { 1 };
    CHECK(DrainWithin([] { return false; }, DrainBound {}, wait) == DrainResult::Drained);
    CHECK(wait.Sleeps() == 0);
    CHECK(wait.Elapsed() == 0ms);
}

TEST_CASE("DrainWithin: work that finishes inside the ceiling reports Drained", "[core][drain]")
{
    auto wait = OverrunningDrainWait { 1 };
    auto busy = BusyForCalls { 3 };
    CHECK(DrainWithin([&busy] { return busy(); }, DrainBound { .ceiling = 1000ms, .poll = 10ms }, wait)
          == DrainResult::Drained);
    CHECK(wait.Sleeps() == 3);
}

TEST_CASE("DrainWithin: a ceiling is measured, not counted", "[core][drain]")
{
    // The regression test for #452. A loop that does `waited += poll` reaches
    // its ceiling after exactly `ceiling / poll` sleeps whatever they cost, so
    // with a 3x overrun it waits three times the bound it states. Both
    // assertions below are what separate the two implementations: the counting
    // one sleeps 500 times and hands out 15000ms.
    constexpr auto Overrun = 3;
    constexpr auto Ceiling = 5000ms;
    constexpr auto Poll = 10ms;

    auto wait = OverrunningDrainWait { Overrun };
    CHECK(DrainWithin([] { return true; }, DrainBound { .ceiling = Ceiling, .poll = Poll }, wait) == DrainResult::Ceiling);

    // Bounded on both sides: it must not give up early either, or a drain that
    // stops instantly would pass the upper bound trivially. Elapsed is exactly
    // `Sleeps() * Poll * Overrun`, so these two pin the sleep count as well:
    // 167 sleeps and 5010ms here, against 500 and 15000ms under the defect.
    CHECK(wait.Elapsed() >= Ceiling);
    CHECK(wait.Elapsed() < Ceiling + (Poll * Overrun));
}

TEST_CASE("DrainWithin: a zero ceiling looks once and never sleeps", "[core][drain]")
{
    auto wait = OverrunningDrainWait { 1 };
    CHECK(DrainWithin([] { return true; }, DrainBound { .ceiling = 0ms, .poll = 10ms }, wait) == DrainResult::Ceiling);
    CHECK(wait.Sleeps() == 0);

    auto idle = OverrunningDrainWait { 1 };
    CHECK(DrainWithin([] { return false; }, DrainBound { .ceiling = 0ms, .poll = 10ms }, idle) == DrainResult::Drained);
}

TEST_CASE("DrainWithin: the default bound is what every shutdown path wants", "[core][drain]")
{
    // Pinned because the four call sites now state no bound at all: a change
    // here silently retimes every shutdown in the tree.
    constexpr auto Defaults = DrainBound {};
    STATIC_REQUIRE(Defaults.ceiling == 5000ms);
    STATIC_REQUIRE(Defaults.poll == 10ms);
}

TEST_CASE("DrainWithin: the production wait blocks and returns", "[core][drain]")
{
    // The shared overrunning fake (tests/OverrunningDrainWait.hpp) proves the
    // arithmetic; this proves the seam it stands in for is wired to a real
    // clock and a real sleep. A small ceiling, so the case costs what it says:
    // the assertion is that it comes back at all and reports the ceiling, not
    // how precisely it lands on it.
    auto const start = std::chrono::steady_clock::now();
    CHECK(DrainWithin([] { return true; }, DrainBound { .ceiling = 40ms, .poll = 5ms }, DefaultDrainWait())
          == DrainResult::Ceiling);
    CHECK(std::chrono::steady_clock::now() - start >= 40ms);
}
