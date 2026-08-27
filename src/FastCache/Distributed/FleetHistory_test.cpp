// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/FleetHistory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <ranges>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;

namespace
{

/// A wall clock a case can place, so a bucket boundary is a decision rather than
/// a race with the machine this runs on.
class PlacedWallClock final: public IWallClock
{
  public:
    [[nodiscard]] std::chrono::system_clock::time_point Now() const noexcept override
    {
        return _now;
    }

    void Set(std::chrono::system_clock::time_point at) noexcept
    {
        _now = at;
    }
    void Advance(std::chrono::seconds by) noexcept
    {
        _now += by;
    }

  private:
    /// A round epoch offset, so a bucket start is easy to reason about in a failure
    /// message: 2026-01-01T00:00:00Z is a whole number of hours and of minutes.
    std::chrono::system_clock::time_point _now { std::chrono::seconds { 1'767'225'600 } };
};

/// One reading with every slot set, so a case varies only what it is about.
[[nodiscard]] EnumTable<FleetMetric, std::uint64_t> Reading(std::uint64_t granted, std::uint64_t inFlight = 0)
{
    EnumTable<FleetMetric, std::uint64_t> values {};
    values[static_cast<std::size_t>(FleetMetric::DispatchGranted)] = granted;
    values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] = inFlight;
    return values;
}

[[nodiscard]] std::uint64_t GrantedOf(FleetBucket const& bucket)
{
    return bucket.values[static_cast<std::size_t>(FleetMetric::DispatchGranted)];
}

} // namespace

TEST_CASE("A fresh history is empty and every bucket is a gap", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    CHECK(history.Empty());
    auto const day = history.Buckets(FleetRange::Day);
    REQUIRE(day.size() == 288);
    // Absent, not zero. A fleet that did nothing and a fleet nobody was watching
    // are different facts, and only one of them is true here.
    CHECK(std::ranges::none_of(day, [](auto const& b) { return b.present; }));
}

TEST_CASE("A sample lands in the bucket its instant belongs to", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(10));
    CHECK_FALSE(history.Empty());

    auto const day = history.Buckets(FleetRange::Day);
    REQUIRE(!day.empty());
    // The newest point is the one just written; everything before it is a gap.
    CHECK(day.back().present);
    CHECK(GrantedOf(day.back()) == 10);
    CHECK_FALSE(day.front().present);
}

TEST_CASE("Sampling twice inside one bucket overwrites rather than accumulating", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(10));
    auto const first = history.Generation();
    clock.Advance(std::chrono::seconds { 5 });
    history.Record(Reading(14));

    auto const day = history.Buckets(FleetRange::Day);
    // Counters are stored raw and cumulative, so the newest reading in a bucket is
    // the right one -- a jittery timer must not be able to double-count.
    CHECK(GrantedOf(day.back()) == 14);
    CHECK(history.Generation() == first);
}

TEST_CASE("A closed bucket advances the generation", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(1));
    auto const before = history.Generation();
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(Reading(2));

    // The whole of an ETag: it moves exactly when a rendered chart would.
    CHECK(history.Generation() > before);
}

TEST_CASE("Downtime leaves a gap rather than a flat line", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(5));
    clock.Advance(std::chrono::hours { 3 });
    history.Record(Reading(9));

    auto const day = history.Buckets(FleetRange::Day);
    auto const present = std::ranges::count_if(day, [](auto const& b) { return b.present; });
    // Two readings three hours apart are two points, not a line drawn across the
    // hours nobody observed.
    CHECK(present == 2);
    CHECK(day.back().present);
}

TEST_CASE("The week view folds to hourly buckets", "[distributed][fleethistory]")
{
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(1));
    clock.Advance(std::chrono::minutes { 30 });
    history.Record(Reading(7));

    auto const week = history.Buckets(FleetRange::Week);
    REQUIRE(week.size() == 168);
    auto const present = std::ranges::count_if(week, [](auto const& b) { return b.present; });
    // Both samples fall inside one hour, so the week view shows one point carrying
    // the later reading.
    CHECK(present == 1);
    CHECK(GrantedOf(week.back()) == 7);
}

TEST_CASE("A range is looked up by what the URL said, and an unknown one is refused", "[distributed][fleethistory]")
{
    CHECK(FleetRangeFromKey("24h") == FleetRange::Day);
    CHECK(FleetRangeFromKey("7d") == FleetRange::Week);
    // Never a silent default: a typo that quietly served a different range would
    // put a reader on the wrong axis without saying so.
    CHECK_FALSE(FleetRangeFromKey("30d").has_value());
    CHECK_FALSE(FleetRangeFromKey("").has_value());
}

TEST_CASE("History survives a save and reload", "[distributed][fleethistory]")
{
    Testing::ScratchDirectory const scratch { "fleet-history-roundtrip" };
    auto const file = scratch.Path() / "history.bin";

    PlacedWallClock clock;
    std::uint64_t generation = 0;
    {
        FleetHistory history { clock };
        history.Record(Reading(11));
        clock.Advance(std::chrono::minutes { 1 });
        history.Record(Reading(23));
        generation = history.Generation();
        REQUIRE(history.Save(file));
    }

    FleetHistory restored { clock };
    REQUIRE(restored.Load(file));
    CHECK_FALSE(restored.Empty());
    CHECK(GrantedOf(restored.Buckets(FleetRange::Day).back()) == 23);
    // Restored rather than reset: a generation starting again from zero would let a
    // client's cached chart match an ETag it had already been served.
    CHECK(restored.Generation() == generation);
}

TEST_CASE("A history file that cannot be trusted starts empty rather than throwing", "[distributed][fleethistory]")
{
    Testing::ScratchDirectory const scratch { "fleet-history-damaged" };
    PlacedWallClock clock;

    SECTION("absent")
    {
        FleetHistory history { clock };
        CHECK_FALSE(history.Load(scratch.Path() / "nothing-here.bin"));
        CHECK(history.Empty());
    }

    SECTION("truncated")
    {
        auto const file = scratch.Path() / "short.bin";
        {
            std::ofstream out { file, std::ios::binary };
            out << "FCFH";
        }
        FleetHistory history { clock };
        CHECK_FALSE(history.Load(file));
        CHECK(history.Empty());
    }

    SECTION("corrupt body")
    {
        auto const file = scratch.Path() / "corrupt.bin";
        {
            FleetHistory good { clock };
            good.Record(Reading(3));
            REQUIRE(good.Save(file));
        }
        // Flip a byte well past the header, so the checksum is what catches it.
        {
            std::fstream patch { file, std::ios::binary | std::ios::in | std::ios::out };
            patch.seekp(64);
            patch.put('\xFF');
        }
        FleetHistory history { clock };
        CHECK_FALSE(history.Load(file));
        // The node still starts. History is a convenience, and no state of this file
        // may keep one from coming up.
        CHECK(history.Empty());
    }
}

TEST_CASE("A bucket remembers when it was sampled, not when its window opened", "[distributed][fleethistory]")
{
    // The two are the same thing for a reading that lands on a boundary and
    // different for every other one, and only the second is what a rate may be
    // divided by. Stamping the alignment at RECORD time throws the instant away
    // where no later fold can recover it -- which made the Week view, whose bucket
    // is its own sub-bucket, always report exactly one nominal width between two
    // adjacent points however far apart they were really taken.
    PlacedWallClock clock;
    FleetHistory history { clock };

    // Two readings in consecutive hours, but only ten minutes apart: the first at
    // the end of its hour, the second at the start of the next.
    clock.Set(std::chrono::sys_days { std::chrono::January / 1 / 2030 } + std::chrono::hours { 5 }
              + std::chrono::minutes { 55 });
    history.Record(Reading(100));
    clock.Set(std::chrono::sys_days { std::chrono::January / 1 / 2030 } + std::chrono::hours { 6 }
              + std::chrono::minutes { 5 });
    history.Record(Reading(130));

    auto const week = history.Buckets(FleetRange::Week);
    REQUIRE(week.size() >= 2);
    auto const& last = week.back();
    auto const& prior = week[week.size() - 2];
    REQUIRE(last.present);
    REQUIRE(prior.present);

    // Drawn one hour apart, as the chart needs.
    CHECK(last.startMillis - prior.startMillis == 3'600'000);
    // Taken ten minutes apart, as a rate needs.
    CHECK(last.sampleMillis - prior.sampleMillis == 600'000);
}
