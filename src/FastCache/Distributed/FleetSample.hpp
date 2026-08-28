// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace FastCache::Distributed
{

/// One number a sample records.
///
/// Nine slots rather than nine members, so a tenth is a row and every walk over
/// them -- sampling, encoding, decoding, rendering -- picks it up without being
/// edited. The five dispatch counters are `LeaseOutcomeTable`'s, read through the
/// same `IMetricsSink::Counter` values so the page and the history cannot drift
/// into two vocabularies for one fact.
enum class FleetMetric : std::uint8_t
{
    DispatchGranted = 0, ///< Cumulative: compiles handed to a worker.
    DispatchNoWorker,    ///< Cumulative: nothing registered for that toolchain.
    DispatchNoCapacity,  ///< Cumulative: every matching worker full of our own work.
    DispatchWithdrawn,   ///< Cumulative: a ceiling withdrew the slots.
    DispatchDuplicate,   ///< Cumulative: already being built somewhere.
    CacheHits,           ///< Cumulative, summed over machines.
    CacheMisses,         ///< Cumulative, summed over machines.
    OfferableSlots,      ///< Gauge: slots a compile could start on.
    JobsInFlight,        ///< Gauge: this fleet's compiles running.
    Last
};

/// Whether a slot accumulates or is read afresh each time.
///
/// The distinction is load-bearing at render: a counter's *rate* is the
/// difference between two buckets, while a gauge's value is the bucket itself.
/// Getting it wrong draws a monotonic ramp for everything.
enum class FleetMetricKind : std::uint8_t
{
    Counter = 0, ///< Monotonic; the interesting quantity is the delta.
    Gauge,       ///< A level; the interesting quantity is the value.
};

/// Whether a MACHINE can answer for a slot about itself.
///
/// Not "which series carries it" -- the fleet series carries every slot, because a
/// leader can answer for all nine and its readings are fleet-wide sums. This is the
/// narrower question the node series asks: is there a version of this number that is
/// true of one machine, whoever happens to be leading?
///
/// For a cache figure or a slot count there is, and it is what that machine did. For
/// a dispatch outcome there is not: only a scheduler produces one, a follower has
/// none, and a follower reporting zero refusals is indistinguishable from a leader
/// that genuinely refused none.
enum class FleetMetricScope : std::uint8_t
{
    /// A machine can answer this about itself: its cache, its slots, its compiles.
    Node = 0,
    /// Only a scheduler can, and only a leader has one that means anything.
    Fleet,
    Last
};

/// What one slot is, for the walks that need to know.
struct FleetMetricRow
{
    FleetMetric metric;     ///< The slot this row describes.
    FleetMetricKind kind;   ///< Delta or level.
    FleetMetricScope scope; ///< Who can answer for it.
    std::string_view key;   ///< Stable name, for JSON and for the on-disk header.
};

/// Every slot, in enumerator order.
inline constexpr EnumTable<FleetMetric, FleetMetricRow> FleetMetricTable {
    FleetMetricRow { .metric = FleetMetric::DispatchGranted,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "granted" },
    FleetMetricRow { .metric = FleetMetric::DispatchNoWorker,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "no-worker" },
    FleetMetricRow { .metric = FleetMetric::DispatchNoCapacity,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "no-capacity" },
    FleetMetricRow { .metric = FleetMetric::DispatchWithdrawn,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "withdrawn" },
    FleetMetricRow { .metric = FleetMetric::DispatchDuplicate,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Fleet,
                     .key = "duplicate" },
    FleetMetricRow { .metric = FleetMetric::CacheHits,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Node,
                     .key = "cache-hits" },
    FleetMetricRow { .metric = FleetMetric::CacheMisses,
                     .kind = FleetMetricKind::Counter,
                     .scope = FleetMetricScope::Node,
                     .key = "cache-misses" },
    FleetMetricRow { .metric = FleetMetric::OfferableSlots,
                     .kind = FleetMetricKind::Gauge,
                     .scope = FleetMetricScope::Node,
                     .key = "offerable" },
    FleetMetricRow { .metric = FleetMetric::JobsInFlight,
                     .kind = FleetMetricKind::Gauge,
                     .scope = FleetMetricScope::Node,
                     .key = "in-flight" },
};
static_assert(RowsInEnumeratorOrder(FleetMetricTable, &FleetMetricRow::metric));

/// Both scopes are populated, or the split this table exists to express does nothing.
///
/// A table where every row said `Fleet` would make a node record nothing about itself
/// and read exactly like the leadership guard this replaced; one where every row said
/// `Node` would have a follower reporting dispatch counters it never produced. Either
/// is silent, and both survive every other check here.
static_assert(std::ranges::any_of(FleetMetricTable,
                                  [](FleetMetricRow const& row) { return row.scope == FleetMetricScope::Node; })
                  && std::ranges::any_of(FleetMetricTable,
                                         [](FleetMetricRow const& row) { return row.scope == FleetMetricScope::Fleet; }),
              "a scope no row carries is a split that quietly does nothing");

/// What folding several samples into one bucket kept, per slot.
///
/// A bucket wider than the sample interval is a fold, and a fold that kept only its
/// last reading has thrown away the part an operator reads: **a refusal spike
/// averaged over a day is invisible**, and a gauge's floor -- the moment the fleet
/// had nothing left -- is the end that matters. Neither is recoverable afterwards,
/// so both are computed while folding.
///
/// What each field means depends on the slot's `FleetMetricKind`, and that is the
/// same split `values` already lives with: a counter's interesting quantity is its
/// delta and a gauge's is its level.
struct FleetFold
{
    /// Gauge: the smallest reading in the bucket. Unused by a counter, whose floor
    /// is a rate rather than a level and is not a fact anybody acts on.
    std::uint64_t low { 0 };

    /// Gauge: the largest reading. Counter: the largest per-SAMPLE delta folded in
    /// -- the peak rate inside the window, which is exactly what an average over a
    /// day destroys.
    std::uint64_t high { 0 };

    /// Gauge: the readings summed, so a mean survives alongside `coverage`. Unused
    /// by a counter, whose mean over a window is the delta across it and needs no
    /// second number.
    std::uint64_t total { 0 };
};

/// One bucket of the history.
///
/// `present` rather than a sentinel value, because zero is a legitimate reading
/// for every slot here. A bucket nobody sampled is a **gap** -- a fleet that did
/// nothing and a fleet nobody was watching are different facts, and the page draws
/// them differently.
struct FleetBucket
{
    /// Wall-clock start of the bucket, in milliseconds since the epoch.
    ///
    /// Wall clock and not `TimePoint`: this outlives the process, and a steady
    /// clock's origin is meaningless across a restart.
    std::int64_t startMillis { 0 };

    /// Wall-clock instant the reading in `values` was actually taken.
    ///
    /// Separate from `startMillis` because the two answer different questions and
    /// only one of them can be the bucket's x-position. `Buckets()` folds a window
    /// down to its newest sample and then stamps the WINDOW's start on it, so the
    /// chart's points are evenly spaced -- which is right for plotting and wrong
    /// for dividing by. The newest bucket is always still open, so a rate that
    /// divided the delta by the nominal width reported a fraction of the truth on
    /// exactly the point an operator reads first.
    ///
    /// Persisted, and it has to be. That was once derived from the stored bucket's
    /// own `startMillis`, on the argument that a ring's bucket IS its sample instant
    /// at sub-bucket granularity -- true when the coarsest ring was the hour a Week
    /// view read one-for-one, and false the moment a ring folds sixty samples into a
    /// bucket. Restoring a partly-sampled day bucket with its window's start would
    /// divide its delta by a nominal twenty-four hours, which is precisely the error
    /// this field exists to prevent.
    std::int64_t sampleMillis { 0 };
    EnumTable<FleetMetric, std::uint64_t> values {}; ///< Raw readings, counters cumulative.

    /// What was lost between the first reading in this bucket and `values`.
    ///
    /// Meaningful only where `coverage` exceeds one; in the base ring a bucket is
    /// one sample and every field here equals the reading itself.
    EnumTable<FleetMetric, FleetFold> fold {};

    /// How many samples folded into this bucket.
    ///
    /// The third state. `present` distinguishes *nobody sampled* from *somebody
    /// did*; this distinguishes a day a node contributed three hours to from one it
    /// contributed twenty-four to, which are both `present` and must not be
    /// compared like for like. A node absent for most of a year is the ORDINARY
    /// case for a workstation on a VPN, not an edge one.
    std::uint64_t coverage { 0 };

    bool present { false }; ///< False means nobody sampled this bucket.

    /// Summed from what the nodes reported rather than sampled by this leader.
    ///
    /// Computed at RENDER and never stored -- which is why it costs the file format
    /// nothing. It marks a window this leader was not scheduling for, so the
    /// FLEET-scoped slots in it are unknowable rather than zero: a backfilled bucket
    /// claiming `granted = 0` beside a real one at 5000 would draw a rate spike off a
    /// number nobody measured, which is the same defect a gap folded in as a peak
    /// already was.
    bool backfilled { false };
};

/// Where a node's handed-over history goes.
///
/// An interface so `SchedulerService` can route what arrives without holding a
/// history itself: it stays pure with respect to I/O and knows nothing about files,
/// rings or ranges, exactly as it knows nothing about sockets.
///
/// Declared HERE rather than beside `FleetHistory`, and that is the whole point of
/// this header: the scheduler decides a fleet's capacity, and a header of its own
/// that dragged in a ring buffer, a `std::map` and `<filesystem>` would make those
/// decisions compile against a file format.
class IFleetHistorySink
{
  public:
    IFleetHistorySink() = default;
    virtual ~IFleetHistorySink() = default;
    IFleetHistorySink(IFleetHistorySink const&) = delete;
    IFleetHistorySink& operator=(IFleetHistorySink const&) = delete;
    IFleetHistorySink(IFleetHistorySink&&) = delete;
    IFleetHistorySink& operator=(IFleetHistorySink&&) = delete;

    /// Take what a node handed over.
    /// @param endpoint Which machine reported; the machine, never the worker id.
    /// @param buckets Its closed buckets, oldest first.
    /// @return How many were new -- a redelivered batch answers zero.
    virtual std::size_t AcceptHistory(std::string_view endpoint, std::span<FleetBucket const> buckets) = 0;
};

} // namespace FastCache::Distributed
