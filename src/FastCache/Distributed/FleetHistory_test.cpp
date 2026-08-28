// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Crc32c.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#include <tests/FleetHistoryFakes.hpp>
#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;
using FastCache::Testing::PlacedWallClock;

namespace
{

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

[[nodiscard]] FleetFold const& FoldOf(FleetBucket const& bucket, FleetMetric metric)
{
    return bucket.fold[static_cast<std::size_t>(metric)];
}

/// Big-endian, matching the file format, so a hand-built fixture is byte-exact.
void AppendU64(std::string& out, std::uint64_t value)
{
    for (auto const shift: { 56, 48, 40, 32, 24, 16, 8, 0 })
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
}

/// A history file in the ORIGINAL v1 layout, written by hand.
///
/// By hand and not by an older binary, because that is the only way this stays a
/// test of the v1 reader: a fixture produced by the current build would be v2 and
/// would exercise nothing. v1 was a generation, then two rings -- minute of 1440 and
/// hour of 168 -- each a count followed by `[start][present][9 values]` per bucket,
/// with no coverage and no fold.
///
/// @param generation What the file claims.
/// @param minuteStart Bucket start of the one present minute bucket.
/// @param minuteGranted Its `DispatchGranted` reading.
/// @param hourStart Bucket start of the one present hour bucket.
/// @param hourGranted Its `DispatchGranted` reading.
/// @return The complete file, header included.
[[nodiscard]] std::string VersionOneFile(std::uint64_t generation,
                                         std::int64_t minuteStart,
                                         std::uint64_t minuteGranted,
                                         std::int64_t hourStart,
                                         std::uint64_t hourGranted)
{
    constexpr std::size_t Slots = EnumeratorCount<FleetMetric>;
    constexpr std::size_t V1MinuteSlots = 24 * 60;
    constexpr std::size_t V1HourSlots = 7 * 24;
    constexpr std::int64_t MinuteMillis = 60 * 1000;
    constexpr std::int64_t HourMillis = 60 * MinuteMillis;

    std::string body;
    AppendU64(body, generation);

    auto const ring = [&](std::size_t slots, std::int64_t widthMillis, std::int64_t presentStart, std::uint64_t granted) {
        AppendU64(body, slots);
        auto const presentIndex = static_cast<std::size_t>(presentStart / widthMillis) % slots;
        for (auto const index: std::views::iota(std::size_t { 0 }, slots))
        {
            auto const here = index == presentIndex;
            AppendU64(body, here ? static_cast<std::uint64_t>(presentStart) : 0U);
            AppendU64(body, here ? 1U : 0U);
            for (auto const slot: std::views::iota(std::size_t { 0 }, Slots))
                AppendU64(body, here && slot == static_cast<std::size_t>(FleetMetric::DispatchGranted) ? granted : 0U);
        }
    };
    ring(V1MinuteSlots, MinuteMillis, minuteStart, minuteGranted);
    ring(V1HourSlots, HourMillis, hourStart, hourGranted);

    std::string file;
    file += "FCFH";
    file.push_back(static_cast<char>(1));
    AppendU64(file, body.size());
    AppendU64(file, Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }));
    file += body;
    return file;
}

/// Write @p bytes to @p path.
void WriteFile(std::filesystem::path const& path, std::string_view bytes)
{
    std::ofstream out { path, std::ios::binary | std::ios::trunc };
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Read @p path back whole.
[[nodiscard]] std::string ReadFile(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
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

TEST_CASE("Every range folds from a ring that actually holds it", "[distributed][fleethistory]")
{
    // Eight views over three rings, and the geometry is checked at compile time by
    // `RangesFitTheirRings`. Asserted here as well because that check is a
    // `consteval` a reader has to go looking for, and because this is the thing the
    // whole storage split exists to guarantee: a view reaching past its ring's lap
    // would render a gap that reads as "nobody was watching".
    for (auto const& row: FleetRangeTable)
    {
        INFO("range " << row.key);
        auto const& ring = FleetRingTable[static_cast<std::size_t>(row.ring)];
        CHECK(row.bucket >= ring.bucket);
        CHECK(row.bucket.count() % ring.bucket.count() == 0);
        CHECK(row.bucket * static_cast<std::int64_t>(row.points) <= ring.bucket * static_cast<std::int64_t>(ring.slots));
    }

    PlacedWallClock clock;
    FleetHistory history { clock };
    history.Record(Reading(1));
    for (auto const& row: FleetRangeTable)
    {
        INFO("range " << row.key);
        CHECK(history.Buckets(row.range).size() == row.points);
    }
}

TEST_CASE("A spike inside a coarse bucket survives the fold", "[distributed][fleethistory]")
{
    // The reason a bucket carries a fold at all. Five minutes of nothing, then one
    // minute in which sixty compiles were granted, then five more of nothing: the
    // Day view folds all of that into one 5-minute point, and a bucket that kept
    // only its last reading would show the average and hide the burst entirely.
    PlacedWallClock clock;
    FleetHistory history { clock };

    std::uint64_t granted = 0;
    for (auto const minute: std::views::iota(0, 5))
    {
        granted += (minute == 3) ? 60 : 1;
        history.Record(Reading(granted, minute == 3 ? 40 : 2));
        clock.Advance(std::chrono::minutes { 1 });
    }

    auto const day = history.Buckets(FleetRange::Day);
    auto const busy = std::ranges::find_if(day, [](FleetBucket const& b) { return b.present && b.coverage > 1; });
    REQUIRE(busy != day.end());

    // A counter's peak is the largest per-SAMPLE delta folded in, which is the one
    // number an average over the window destroys.
    CHECK(FoldOf(*busy, FleetMetric::DispatchGranted).high == 60);
    // A gauge keeps both ends: the floor is when the fleet had nothing left, and
    // that is at least as interesting as the ceiling.
    CHECK(FoldOf(*busy, FleetMetric::JobsInFlight).high == 40);
    CHECK(FoldOf(*busy, FleetMetric::JobsInFlight).low == 2);
}

TEST_CASE("Coverage counts the samples a bucket actually folded", "[distributed][fleethistory]")
{
    // The third state. `present` separates "nobody sampled" from "somebody did";
    // this separates a window a node contributed one minute to from one it
    // contributed five to, which are both present and must not be read alike.
    PlacedWallClock clock;
    FleetHistory history { clock };

    for ([[maybe_unused]] auto const minute: std::views::iota(0, 3))
    {
        history.Record(Reading(1));
        clock.Advance(std::chrono::minutes { 1 });
    }

    auto const day = history.Buckets(FleetRange::Day);
    auto const folded = std::ranges::find_if(day, [](FleetBucket const& b) { return b.present; });
    REQUIRE(folded != day.end());
    CHECK(folded->coverage == 3);

    // In the base ring a bucket IS one sample, so coverage there is always one --
    // which is what makes it a meaningful divisor rather than a tautology.
    auto const hour = history.Buckets(FleetRange::OneHour);
    for (auto const& bucket: hour)
        if (bucket.present)
            CHECK(bucket.coverage == 1);
}

TEST_CASE("A window sampled throughout reports exactly its full coverage", "[distributed][fleethistory]")
{
    // `FullCoverageOf` is what gives `coverage` a meaning, so it has to agree with
    // what recording actually produces rather than being arithmetic nobody checks.
    // One sample a minute for a whole 5-minute window: the Day point folding it must
    // read exactly the number that function names.
    PlacedWallClock clock;
    FleetHistory history { clock };

    REQUIRE(FullCoverageOf(FleetRange::Day) == 5);
    for ([[maybe_unused]] auto const minute: std::views::iota(std::uint64_t { 0 }, FullCoverageOf(FleetRange::Day)))
    {
        history.Record(Reading(1));
        clock.Advance(std::chrono::minutes { 1 });
    }

    auto const day = history.Buckets(FleetRange::Day);
    auto const full = std::ranges::find_if(day, [](FleetBucket const& b) { return b.present; });
    REQUIRE(full != day.end());
    CHECK(full->coverage == FullCoverageOf(FleetRange::Day));

    // And it is the window's own width over the base interval for every view, which
    // is why it is keyed on a range rather than on the ring underneath it.
    for (auto const& row: FleetRangeTable)
    {
        INFO("range " << row.key);
        CHECK(FullCoverageOf(row.range) == static_cast<std::uint64_t>(row.bucket.count() / FleetSampleInterval.count()));
    }
}

TEST_CASE("A version 1 history is inflated forward rather than discarded", "[distributed][fleethistory]")
{
    // The load-bearing one. A format is convertible exactly as long as its reader is
    // in the table; bumping the version without adding a row is the decision to
    // throw away every history on every machine, and this is what makes that a
    // decision rather than an oversight.
    Testing::ScratchDirectory const scratch { "fleet-history-v1" };
    auto const file = scratch.Path() / "v1.bin";

    PlacedWallClock clock;
    auto const nowMillis = std::chrono::duration_cast<std::chrono::milliseconds>(clock.Now().time_since_epoch()).count();
    auto const minuteStart = (nowMillis / 60'000) * 60'000;
    auto const hourStart = (nowMillis / 3'600'000) * 3'600'000;

    WriteFile(file, VersionOneFile(77, minuteStart, 11, hourStart, 23));

    FleetHistory history { clock };
    REQUIRE(history.Load(file));
    CHECK_FALSE(history.Empty());
    CHECK(history.Generation() == 77);

    // The readings are there, in both rings -- and the hour ring grew from 168 slots
    // to 720 between the versions, so a bucket copied across by INDEX rather than
    // re-placed by its own start would have landed in the wrong slot and then read
    // as a previous lap.
    auto const day = history.Buckets(FleetRange::Day);
    REQUIRE_FALSE(day.empty());
    CHECK(GrantedOf(day.back()) == 11);

    auto const week = history.Buckets(FleetRange::Week);
    REQUIRE_FALSE(week.empty());
    CHECK(week.back().present);
    CHECK(GrantedOf(week.back()) == 23);

    // v1 stored one reading per bucket and no fold, so coverage is one -- what a v1
    // bucket literally held. A counter's peak is unknowable from it and stays absent
    // rather than being invented.
    CHECK(week.back().coverage == 1);
    CHECK(FoldOf(week.back(), FleetMetric::DispatchGranted).high == 0);

    // And it saves forward, in the current format.
    auto const upgraded = scratch.Path() / "v2.bin";
    REQUIRE(history.Save(upgraded));
    FleetHistory again { clock };
    REQUIRE(again.Load(upgraded));
    CHECK(GrantedOf(again.Buckets(FleetRange::Day).back()) == 11);
}

TEST_CASE("A history newer than this build is kept, and never written over", "[distributed][fleethistory]")
{
    // The one irreversible thing here, and the reason this phase exists. A node
    // rolled back to an older build reads a file it cannot understand; if it then
    // started empty and saved, it would destroy a year of readings that the build it
    // was rolled back FROM could still have read. Everything else about this file is
    // recoverable by waiting.
    Testing::ScratchDirectory const scratch { "fleet-history-newer" };
    auto const file = scratch.Path() / "newer.bin";

    PlacedWallClock clock;

    // A real, current file with its version byte raised to something no reader
    // claims. The rest stays valid, so nothing but the version can be what refuses.
    {
        FleetHistory writer { clock };
        writer.Record(Reading(5));
        REQUIRE(writer.Save(file));
    }
    auto bytes = ReadFile(file);
    REQUIRE(bytes.size() > 4);
    bytes[4] = static_cast<char>(200);
    WriteFile(file, bytes);
    auto const before = ReadFile(file);

    FleetHistory history { clock };
    CHECK_FALSE(history.Load(file));
    CHECK(history.Empty());
    CHECK(history.ReadOnly());

    // Sampling continues -- the page is live either way, and only persistence stops.
    history.Record(Reading(9));
    CHECK_FALSE(history.Empty());

    CHECK_FALSE(history.Save(file));
    CHECK(ReadFile(file) == before);
}

TEST_CASE("A history older than every reader is not treated as newer", "[distributed][fleethistory]")
{
    // Removing a reader row is a deliberate decision to stop carrying that format,
    // and a build that made it must be free to write. Only a version ABOVE the
    // newest this build knows can have been written by a later one.
    Testing::ScratchDirectory const scratch { "fleet-history-ancient" };
    auto const file = scratch.Path() / "ancient.bin";

    PlacedWallClock clock;
    {
        FleetHistory writer { clock };
        writer.Record(Reading(5));
        REQUIRE(writer.Save(file));
    }
    auto bytes = ReadFile(file);
    bytes[4] = static_cast<char>(0);
    WriteFile(file, bytes);

    FleetHistory history { clock };
    CHECK_FALSE(history.Load(file));
    CHECK_FALSE(history.ReadOnly());
    CHECK(history.Save(file));
}

TEST_CASE("A gap in sampling is not folded in as a peak", "[distributed][fleethistory]")
{
    // A node samples only while it leads, so a leadership change of three hours ends
    // with one sample whose delta is three hours of work. Folded in as a per-minute
    // peak that is a spike which never happened -- and it would be PERSISTED, in the
    // day ring, for over a year.
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(100));
    clock.Advance(std::chrono::hours { 3 });
    history.Record(Reading(10'000));
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(Reading(10'007));

    auto const week = history.Buckets(FleetRange::Week);
    auto const worst = std::ranges::max(week | std::views::transform([](FleetBucket const& b) {
                                            return b.fold[static_cast<std::size_t>(FleetMetric::DispatchGranted)].high;
                                        }));
    // Seven, from the one adjacent pair. Emphatically not 9,900.
    CHECK(worst == 7);
}

TEST_CASE("Adjacent samples still produce a peak after the gap rule", "[distributed][fleethistory]")
{
    // The other half: a guard that refused every delta would be just as wrong, and a
    // rule that only ever suppresses is indistinguishable from one that never fires.
    PlacedWallClock clock;
    FleetHistory history { clock };

    history.Record(Reading(0));
    clock.Advance(std::chrono::minutes { 1 });
    history.Record(Reading(42));

    auto const day = history.Buckets(FleetRange::Day);
    auto const worst = std::ranges::max(day | std::views::transform([](FleetBucket const& b) {
                                            return b.fold[static_cast<std::size_t>(FleetMetric::DispatchGranted)].high;
                                        }));
    CHECK(worst == 42);
}

TEST_CASE("A restart keeps when a bucket was sampled, not when its window opened", "[distributed][fleethistory]")
{
    // `sampleMillis` used to be derived from the stored bucket's own start, which was
    // true while the coarsest ring was the hour a Week view read one-for-one. A ring
    // that folds sixty readings has no way to recover when the last of them landed,
    // so a partly-sampled bucket restored that way has its delta divided by a nominal
    // width -- the exact error this field exists to prevent.
    Testing::ScratchDirectory const scratch { "fleet-history-sample-instant" };
    auto const file = scratch.Path() / "history.bin";

    PlacedWallClock clock;
    // Sampled near the END of an hour, so the instant and the window's start are far
    // apart and a derived value cannot accidentally match.
    clock.Advance(std::chrono::minutes { 58 });

    std::int64_t sampled = 0;
    {
        FleetHistory history { clock };
        history.Record(Reading(7));
        sampled = history.Buckets(FleetRange::Week).back().sampleMillis;
        REQUIRE(history.Save(file));
    }

    FleetHistory restored { clock };
    REQUIRE(restored.Load(file));
    auto const week = restored.Buckets(FleetRange::Week);
    REQUIRE_FALSE(week.empty());
    CHECK(week.back().sampleMillis == sampled);
    // And it is genuinely not the window's start, or this case would pass on a bug.
    CHECK(week.back().sampleMillis != week.back().startMillis);
}

TEST_CASE("A node hands over closed buckets, oldest first, and never the open one", "[distributed][fleethistory]")
{
    // What rides the heartbeat. The newest bucket is still open -- it gains a reading
    // every sample -- so shipping it would hand the leader a partial window it could
    // never be told to correct.
    PlacedWallClock clock;
    FleetHistory history { clock };

    for (auto const minute: std::views::iota(0, 5))
    {
        history.Record(Reading(static_cast<std::uint64_t>(minute) + 1));
        clock.Advance(std::chrono::minutes { 1 });
    }
    // Five recorded, and the clock now sits in a sixth, empty window. Four are
    // closed; the fifth is the one the clock left behind and is closed too.
    auto const all = history.ClosedBucketsAfter(-1, 100);
    REQUIRE(all.size() == 5);
    CHECK(std::ranges::is_sorted(all, {}, &FleetBucket::startMillis));
    CHECK(GrantedOf(all.front()) == 1);
    CHECK(GrantedOf(all.back()) == 5);

    // Record into the current window: it is now open, and must not travel.
    history.Record(Reading(6));
    auto const closed = history.ClosedBucketsAfter(-1, 100);
    CHECK(closed.size() == 5);
    CHECK(GrantedOf(closed.back()) == 5);

    // The watermark excludes what has already been handed over.
    auto const after = history.ClosedBucketsAfter(all[2].startMillis, 100);
    REQUIRE(after.size() == 2);
    CHECK(GrantedOf(after.front()) == 4);

    // And the ceiling takes the OLDEST, so a catch-up converges from the far end
    // rather than resending the newest and never closing the gap.
    auto const bounded = history.ClosedBucketsAfter(-1, 2);
    REQUIRE(bounded.size() == 2);
    CHECK(GrantedOf(bounded.front()) == 1);
    CHECK(GrantedOf(bounded.back()) == 2);

    CHECK(history.ClosedBucketsAfter(-1, 0).empty());
}

TEST_CASE("A leader fills the windows it missed from what the nodes kept", "[distributed][fleethistory]")
{
    // The whole point of the issue. This leader was elected part-way through, so its
    // own series has nothing for the earlier windows -- but every machine kept
    // recording itself throughout, and handed that over.
    PlacedWallClock clock;

    // Two machines, each recording four minutes.
    FleetHistory nodeA { clock };
    FleetHistory nodeB { clock };
    for (auto const minute: std::views::iota(0, 4))
    {
        auto const value = static_cast<std::uint64_t>(minute) + 1;
        nodeA.Record(Reading(0, value));      // in flight: 1,2,3,4
        nodeB.Record(Reading(0, value * 10)); // in flight: 10,20,30,40
        clock.Advance(std::chrono::minutes { 1 });
    }

    FleetNodeHistories received { clock };
    CHECK(received.AcceptHistory("a:1", nodeA.ClosedBucketsAfter(-1, 100)) == 4);
    CHECK(received.AcceptHistory("b:1", nodeB.ClosedBucketsAfter(-1, 100)) == 4);
    CHECK(received.Count() == 2);

    // A leader whose own series is empty: it was not leading for any of that.
    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    CHECK(std::ranges::none_of(view, [](FleetBucket const& b) { return b.present; }));

    received.BackfillInto(view, FleetRange::OneHour);

    auto filledCount = 0;
    for (auto const& bucket: view)
    {
        if (!bucket.present)
            continue;
        ++filledCount;
        // Summed across machines, and marked so nothing reads a dispatch counter off
        // a window this leader was not scheduling for.
        CHECK(bucket.backfilled);
        auto const inFlight = bucket.values[static_cast<std::size_t>(FleetMetric::JobsInFlight)];
        CHECK(inFlight % 11 == 0); // n + 10n
    }
    CHECK(filledCount == 4);
}

TEST_CASE("A window the leader sampled itself is never replaced by a sum", "[distributed][fleethistory]")
{
    // Gaps only. A window this leader sampled holds the registry's own totals -- what
    // the page has always plotted -- and replacing them with a sum assembled from a
    // different set of machines would step the chart wherever the two disagreed.
    PlacedWallClock clock;

    FleetHistory node { clock };
    node.Record(Reading(0, 7));
    clock.Advance(std::chrono::minutes { 1 });

    FleetHistory leader { clock };
    leader.Record(Reading(0, 999));

    FleetNodeHistories received { clock };
    received.AcceptHistory("a:1", node.ClosedBucketsAfter(-1, 100));

    auto view = leader.Buckets(FleetRange::OneHour);
    received.BackfillInto(view, FleetRange::OneHour);

    auto const own = std::ranges::find_if(view, [](FleetBucket const& b) {
        return b.present && b.values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] == 999;
    });
    REQUIRE(own != view.end());
    CHECK_FALSE(own->backfilled);
}

TEST_CASE("A redelivered heartbeat does not count twice", "[distributed][fleethistory]")
{
    // The wire carries no acknowledgement, so a node that never saw a reply resends.
    // The high-water mark is what makes that safe -- and it is why no reply field was
    // needed in the first place.
    PlacedWallClock clock;

    FleetHistory node { clock };
    for ([[maybe_unused]] auto const minute: std::views::iota(0, 3))
    {
        node.Record(Reading(0, 5));
        clock.Advance(std::chrono::minutes { 1 });
    }
    auto const batch = node.ClosedBucketsAfter(-1, 100);
    REQUIRE(batch.size() == 3);

    FleetNodeHistories received { clock };
    CHECK(received.AcceptHistory("a:1", batch) == 3);
    // The very same batch again, as a lost reply would produce.
    CHECK(received.AcceptHistory("a:1", batch) == 0);
    CHECK(received.HighWaterFor("a:1") == batch.back().startMillis);

    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    received.BackfillInto(view, FleetRange::OneHour);

    for (auto const& bucket: view)
        if (bucket.present)
            // Five, not ten. A machine reporting the same window twice must not
            // double the fleet's reading of it.
            CHECK(bucket.values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] == 5);

    CHECK(received.HighWaterFor("nobody:1") == -1);
}

TEST_CASE("A handover carries no fold, and the leader rebuilds one", "[distributed][fleethistory]")
{
    // What the wire record leaves out and why it can. A counter's peak is a RATE and
    // cannot be recovered from a single reading -- but it can be recovered from the
    // SEQUENCE of readings, which is exactly what travels. Carrying the fold as well
    // would be a second answer to a question already answered, and the one a decoder
    // trusted would be the one nothing kept correct.
    PlacedWallClock clock;

    // A NODE-scoped counter, because that is what a machine can answer for and
    // therefore all the backfill sums: a dispatch outcome belongs to the scheduler,
    // and summing one across machines would be summing zeroes into a number a
    // reader would take for a measurement.
    constexpr auto Hits = static_cast<std::size_t>(FleetMetric::CacheHits);
    FleetHistory node { clock };
    // A counter that steps hard once: 0, then 100, then 10 more.
    for (auto const hits: std::array<std::uint64_t, 3> { 0, 100, 110 })
    {
        EnumTable<FleetMetric, std::uint64_t> values {};
        values[Hits] = hits;
        node.Record(values);
        clock.Advance(std::chrono::minutes { 1 });
    }

    auto const own = node.Buckets(FleetRange::OneHour);
    auto const spike =
        std::ranges::find_if(own, [](FleetBucket const& each) { return each.present && each.fold[Hits].high == 100; });
    REQUIRE(spike != own.end());

    // Out through the wire and back, which is the only shape the leader ever sees.
    // Three closed buckets, not two: the clock was advanced past the last reading,
    // so the window it landed in has closed as well.
    auto const handed = HistoryFromWire(HistoryToWire(node.ClosedBucketsAfter(-1, 100)));
    REQUIRE(handed.size() == 3);
    for (auto const& bucket: handed)
        CHECK(bucket.fold[Hits].high == 0);

    FleetNodeHistories received { clock };
    REQUIRE(received.AcceptHistory("a:1", handed) == 3);

    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    received.BackfillInto(view, FleetRange::OneHour);

    auto const rebuilt = std::ranges::find_if(
        view, [&spike](FleetBucket const& each) { return each.present && each.startMillis == spike->startMillis; });
    REQUIRE(rebuilt != view.end());
    // The same peak the node itself recorded, from readings alone.
    CHECK(rebuilt->fold[Hits].high == 100);
}

TEST_CASE("A leader's record of the other machines survives a restart", "[distributed][fleethistory]")
{
    // Not an optimisation. A node advances its own watermark once a heartbeat is
    // accepted and never resends it, so a leader that forgot what it had been handed
    // would leave those windows a gap for as long as the rings hold them -- the very
    // failure the handover exists to remove, reintroduced by a restart.
    Testing::ScratchDirectory const scratch { "fleet-received-roundtrip" };
    auto const file = scratch.Path() / "received-history.bin";

    PlacedWallClock clock;
    FleetHistory nodeA { clock };
    FleetHistory nodeB { clock };
    for (auto const minute: std::views::iota(0, 4))
    {
        auto const value = static_cast<std::uint64_t>(minute) + 1;
        nodeA.Record(Reading(0, value));
        nodeB.Record(Reading(0, value * 10));
        clock.Advance(std::chrono::minutes { 1 });
    }

    std::int64_t highA = 0;
    auto const batchA = nodeA.ClosedBucketsAfter(-1, 100);
    {
        FleetNodeHistories received { clock };
        REQUIRE(received.AcceptHistory("a:1", batchA) == 4);
        REQUIRE(received.AcceptHistory("b:1", nodeB.ClosedBucketsAfter(-1, 100)) == 4);
        highA = received.HighWaterFor("a:1");
        REQUIRE(received.Save(file));
    }

    FleetNodeHistories restored { clock };
    REQUIRE(restored.Load(file));
    CHECK(restored.Count() == 2);
    // The mark as well as the readings: without it a restarted leader would take the
    // batch a node resends after a reply it never saw and count every window twice.
    CHECK(restored.HighWaterFor("a:1") == highA);
    CHECK(restored.AcceptHistory("a:1", batchA) == 0);

    FleetHistory leader { clock };
    auto view = leader.Buckets(FleetRange::OneHour);
    restored.BackfillInto(view, FleetRange::OneHour);

    auto filledCount = 0;
    for (auto const& bucket: view)
    {
        if (!bucket.present)
            continue;
        ++filledCount;
        CHECK(bucket.backfilled);
        CHECK(bucket.values[static_cast<std::size_t>(FleetMetric::JobsInFlight)] % 11 == 0);
    }
    CHECK(filledCount == 4);
}

TEST_CASE("A received-history file that cannot be trusted starts empty", "[distributed][fleethistory]")
{
    // The same rule as a node's own history, and for the same reason: this is a
    // convenience, and no state of the file may keep a leader from coming up.
    Testing::ScratchDirectory const scratch { "fleet-received-damaged" };
    PlacedWallClock clock;

    SECTION("absent")
    {
        FleetNodeHistories received { clock };
        CHECK_FALSE(received.Load(scratch.Path() / "nothing-here.bin"));
        CHECK(received.Count() == 0);
    }

    SECTION("another file's magic")
    {
        // A node series and a received store sit in the same directory, so pointing
        // one reader at the other's file is a configuration slip rather than a
        // fantasy -- and it must be refused rather than half-read.
        auto const file = scratch.Path() / "wrong-magic.bin";
        {
            FleetHistory own { clock };
            own.Record(Reading(3));
            REQUIRE(own.Save(file));
        }
        FleetNodeHistories received { clock };
        CHECK_FALSE(received.Load(file));
        CHECK(received.Count() == 0);
    }

    SECTION("corrupt body")
    {
        auto const file = scratch.Path() / "corrupt.bin";
        {
            FleetHistory node { clock };
            node.Record(Reading(0, 7));
            clock.Advance(std::chrono::minutes { 1 });
            FleetNodeHistories received { clock };
            REQUIRE(received.AcceptHistory("a:1", node.ClosedBucketsAfter(-1, 100)) == 1);
            REQUIRE(received.Save(file));
        }
        {
            std::fstream patch { file, std::ios::binary | std::ios::in | std::ios::out };
            patch.seekp(64);
            patch.put(static_cast<char>(0xFF));
        }
        FleetNodeHistories received { clock };
        CHECK_FALSE(received.Load(file));
        CHECK(received.Count() == 0);
    }
}
