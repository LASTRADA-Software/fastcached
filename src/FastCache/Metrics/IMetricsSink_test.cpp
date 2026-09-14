// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <utility>

using namespace FastCache;

namespace
{
/// How many counters the enum declares.
///
/// Named apart from `FastCache::CounterCount`, which the header derives its own way: two
/// derivations agreeing is something the cases below may assert, one derivation twice is not.
///
/// `EnumeratorCount` rather than a cast written out here, and derived rather than
/// listed: a list of enumerators in this file would be a second place a new
/// counter has to be added, and it would agree perfectly with a sink whose array
/// had stopped covering the enum -- which is the defect this file is about.
constexpr std::size_t DeclaredCounters = EnumeratorCount<IMetricsSink::Counter>;

/// The counter at ordinal @p index.
/// @param index An ordinal below `DeclaredCounters`.
/// @return The enumerator sitting there.
[[nodiscard]] constexpr IMetricsSink::Counter CounterAt(std::size_t index) noexcept
{
    return static_cast<IMetricsSink::Counter>(index);
}
} // namespace

TEST_CASE("A fresh sink reads zero for every counter, which is the honest zero", "[metrics][sink]")
{
    // The one thing worth defending before the walk: a loop over an empty range
    // asserts nothing while passing. Asserting the walk's own length against the
    // enum's count afterwards would only restate how `DeclaredCounters` is defined.
    STATIC_REQUIRE(DeclaredCounters > 0);

    AtomicMetricsSink metrics;

    // The control for the case below. Without it, a sink answering some constant
    // would pass the round-trip walk for the wrong reason, and a zero read back
    // there would be indistinguishable from a counter never touched.
    for (auto const index: std::views::iota(std::size_t { 0 }, DeclaredCounters))
    {
        CAPTURE(index);
        CHECK(metrics.Read(CounterAt(index)) == 0);
    }
}

TEST_CASE("Every counter the enum declares is a counter the sink can actually carry", "[metrics][sink]")
{
    AtomicMetricsSink metrics;

    // A DISTINCT value per counter, and one derived from the ordinal, so the
    // read-back proves WHICH slot answered rather than merely that something did.
    // Incrementing every counter by 1 would pass just as well under a sink that
    // folded two ordinals onto one slot.
    for (auto const index: std::views::iota(std::size_t { 0 }, DeclaredCounters))
        metrics.Increment(CounterAt(index), static_cast<std::uint64_t>(index) + 1);

    // Read AFTER every write, never interleaved: a slot written by a later
    // counter and read before that write would look correct.
    //
    // `CAPTURE` inside the loop and `CHECK` in the same iteration, so every
    // mismatch is reported WITH its ordinal and both values -- `CHECK` does not
    // unwind. Collecting mismatches and asserting a count after the loop is the
    // shape that reports a bare number and names none of them, because a Catch2
    // scoped message is destroyed at the end of the block that made it.
    //
    // What this walk can and cannot see, stated rather than implied. It covers a
    // literal array extent, an off-by-one in that extent, a folded ordinal and a
    // constant answer -- every shape where one translation unit disagrees with
    // ITSELF.
    //
    // It does NOT reproduce #1332. That skew is CROSS-TU: `DeclaredCounters` here and
    // `FastCache::CounterCount` both reduce to `Counter::Last` in this
    // same translation unit, so this case compiles against one enum and passes
    // under the very build that produced the incident. No single-TU assertion can
    // observe it; closing that needs a fixture that compiles one unit against a
    // modified header.
    for (auto const index: std::views::iota(std::size_t { 0 }, DeclaredCounters))
    {
        CAPTURE(index);
        CHECK(metrics.Read(CounterAt(index)) == static_cast<std::uint64_t>(index) + 1);
    }
}

TEST_CASE("Incrementing one counter moves no other", "[metrics][sink]")
{
    STATIC_REQUIRE(DeclaredCounters >= 2);

    AtomicMetricsSink metrics;
    metrics.Increment(CounterAt(0), 5);

    CHECK(metrics.Read(CounterAt(0)) == 5);

    // Not covered by the walk above, which writes EVERY slot and so cannot see a
    // write splashing into one nothing touched. The fresh-sink case cannot see it
    // either, because nothing has been written there at all.
    for (auto const index: std::views::iota(std::size_t { 1 }, DeclaredCounters))
    {
        CAPTURE(index);
        CHECK(metrics.Read(CounterAt(index)) == 0);
    }
}

TEST_CASE("A counter accumulates rather than replacing, and its default step is one", "[metrics][sink]")
{
    AtomicMetricsSink metrics;
    metrics.Increment(CounterAt(0));
    metrics.Increment(CounterAt(0));
    metrics.Increment(CounterAt(0), 3);

    // 5 rather than 3: a tally, so the calls add. A sink that assigned would read
    // 3 here and would pass a test that only ever incremented once.
    CHECK(metrics.Read(CounterAt(0)) == 5);
}

TEST_CASE("Every declared counter has its own index, and the end of the enum has none", "[metrics][sink]")
{
    STATIC_REQUIRE(CounterCount == DeclaredCounters);

    for (auto const index: std::views::iota(std::size_t { 0 }, DeclaredCounters))
    {
        CAPTURE(index);
        CHECK(CounterIndex(CounterAt(index)) == std::optional { index });
    }

    // The value the answer exists for. A converter answering `std::optional { index }` for every input
    // passes the walk above, and it is what a skewed build's stale definition would be.
    CHECK_FALSE(CounterIndex(IMetricsSink::Counter::Last).has_value());
    CHECK_FALSE(CounterIndex(CounterAt(DeclaredCounters + 1)).has_value());
}

TEST_CASE("A counter table hands each counter its own cell, in enumerator order, and none past the enum", "[metrics][sink]")
{
    auto cells = CounterCells<std::uint64_t> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, DeclaredCounters))
    {
        auto* const cell = cells.Find(CounterAt(index));
        REQUIRE(cell != nullptr);
        *cell = index + 1;
    }

    // Read back by POSITION, so a `Find` that folded two counters onto one cell, or handed out cells in
    // an order the positional walk does not share, reads a different value than it wrote.
    auto const positional = std::as_const(cells).Positional();
    REQUIRE(positional.size() == DeclaredCounters);
    for (auto const index: std::views::iota(std::size_t { 0 }, DeclaredCounters))
    {
        CAPTURE(index);
        CHECK(positional[index] == index + 1);
    }

    CHECK(cells.Find(IMetricsSink::Counter::Last) == nullptr);
    CHECK(std::as_const(cells).Find(IMetricsSink::Counter::Last) == nullptr);
}
