// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

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

/// What one slot is, for the walks that need to know.
struct FleetMetricRow
{
    FleetMetric metric;   ///< The slot this row describes.
    FleetMetricKind kind; ///< Delta or level.
    std::string_view key; ///< Stable name, for JSON and for the on-disk header.
};

/// Every slot, in enumerator order.
inline constexpr EnumTable<FleetMetric, FleetMetricRow> FleetMetricTable {
    FleetMetricRow { .metric = FleetMetric::DispatchGranted, .kind = FleetMetricKind::Counter, .key = "granted" },
    FleetMetricRow { .metric = FleetMetric::DispatchNoWorker, .kind = FleetMetricKind::Counter, .key = "no-worker" },
    FleetMetricRow { .metric = FleetMetric::DispatchNoCapacity, .kind = FleetMetricKind::Counter, .key = "no-capacity" },
    FleetMetricRow { .metric = FleetMetric::DispatchWithdrawn, .kind = FleetMetricKind::Counter, .key = "withdrawn" },
    FleetMetricRow { .metric = FleetMetric::DispatchDuplicate, .kind = FleetMetricKind::Counter, .key = "duplicate" },
    FleetMetricRow { .metric = FleetMetric::CacheHits, .kind = FleetMetricKind::Counter, .key = "cache-hits" },
    FleetMetricRow { .metric = FleetMetric::CacheMisses, .kind = FleetMetricKind::Counter, .key = "cache-misses" },
    FleetMetricRow { .metric = FleetMetric::OfferableSlots, .kind = FleetMetricKind::Gauge, .key = "offerable" },
    FleetMetricRow { .metric = FleetMetric::JobsInFlight, .kind = FleetMetricKind::Gauge, .key = "in-flight" },
};
static_assert(RowsInEnumeratorOrder(FleetMetricTable, &FleetMetricRow::metric));

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
    EnumTable<FleetMetric, std::uint64_t> values {}; ///< Raw readings, counters cumulative.
    bool present { false };                          ///< False means nobody sampled this bucket.
};

/// How far back a view reaches, and how coarse its buckets are.
enum class FleetRange : std::uint8_t
{
    Day = 0, ///< 24 hours of 5-minute buckets.
    Week,    ///< 7 days of 1-hour buckets.
    Last
};

/// What one range is called and how it is shaped.
struct FleetRangeRow
{
    FleetRange range;             ///< The range this row describes.
    std::string_view key;         ///< What a URL says: `24h`, `7d`.
    std::string_view label;       ///< What the control reads: `24 H`, `7 D`.
    std::string_view bucketLabel; ///< What the caption says the buckets are.
    std::chrono::seconds bucket;  ///< Width of one rendered bucket.
    std::size_t points;           ///< How many buckets a render draws.
};

/// Both ranges, in enumerator order. The geometry matches the mockup exactly.
inline constexpr EnumTable<FleetRange, FleetRangeRow> FleetRangeTable {
    FleetRangeRow { .range = FleetRange::Day,
                    .key = "24h",
                    .label = "24 H",
                    .bucketLabel = "5-minute buckets",
                    .bucket = std::chrono::minutes { 5 },
                    .points = 288 },
    FleetRangeRow { .range = FleetRange::Week,
                    .key = "7d",
                    .label = "7 D",
                    .bucketLabel = "1-hour buckets",
                    .bucket = std::chrono::hours { 1 },
                    .points = 168 },
};
static_assert(RowsInEnumeratorOrder(FleetRangeTable, &FleetRangeRow::range));

/// Look a range up by what a URL said.
/// @param key The `range=` value.
/// @return The row, or nullopt when nothing matches -- never a silent default.
[[nodiscard]] std::optional<FleetRange> FleetRangeFromKey(std::string_view key) noexcept;

/// The leader's rolling record of what the fleet has been doing.
///
/// Sampled on a timer while this node LEADS, because a follower's registry holds
/// whatever registered against it rather than the fleet -- sampling there would
/// record a fraction as though it were the whole.
///
/// Thread-safe: the sampler thread writes and the admin thread reads.
class FleetHistory
{
  public:
    /// One sample per minute is the base resolution; both ranges fold from it.
    static constexpr std::chrono::seconds SampleInterval { 60 };

    /// @param wall Where a bucket's timestamp comes from; injected so a test can place one.
    explicit FleetHistory(IWallClock const& wall);

    /// Record one reading, folding it into whichever bucket its instant belongs to.
    ///
    /// Idempotent within a bucket: sampling twice inside the same minute overwrites
    /// rather than appends, so a jittery timer cannot double-count.
    /// @param values The nine readings, counters cumulative.
    void Record(EnumTable<FleetMetric, std::uint64_t> const& values);

    /// The buckets a range renders, oldest first, gaps included.
    /// @param range Which view.
    /// @return Exactly `FleetRangeTable[range].points` buckets.
    [[nodiscard]] std::vector<FleetBucket> Buckets(FleetRange range) const;

    /// How many buckets have closed since this object was made.
    ///
    /// The whole of an `ETag`: it changes exactly when a rendered chart would, and
    /// costs nothing to compute. Survives a reload, because it is restored from the
    /// file -- otherwise a restart would re-serve a stale cached chart under a
    /// generation a client had already seen.
    [[nodiscard]] std::uint64_t Generation() const noexcept;

    /// Whether anything has been recorded at all.
    [[nodiscard]] bool Empty() const noexcept;

    /// How much longer the newest bucket of a range stays the newest.
    ///
    /// What a `Cache-Control: max-age` must be, and the reason it is not a fixed
    /// sixty seconds: a viewer told to hold a chart for a minute one second before
    /// its bucket closes then sits a whole bucket behind for the rest of that
    /// minute, with nothing on the page saying so. Never zero -- a `max-age=0`
    /// would revalidate on every image on every refresh.
    /// @param range Which view.
    /// @return Seconds until this range's newest bucket is superseded.
    [[nodiscard]] std::chrono::seconds UntilBucketCloses(FleetRange range) const noexcept;

    /// Read a saved history back.
    ///
    /// Every failure -- absent, short, wrong version, bad checksum -- starts empty
    /// and says so. History is a convenience, and no state of this file may keep a
    /// node from starting.
    /// @param path Where `Save` wrote.
    /// @return True when buckets were restored; false when starting empty.
    bool Load(std::filesystem::path const& path);

    /// Write the history out, atomically.
    ///
    /// Whole-file to a temporary then `rename`, so a crash mid-write leaves the
    /// previous file rather than half of this one.
    /// @param path Where to write.
    /// @return True on success.
    bool Save(std::filesystem::path const& path) const;

  private:
    [[nodiscard]] std::int64_t NowMillis() const noexcept;

    IWallClock const* _wall;
    mutable std::mutex _mutex;

    /// The 1-minute ring, 24 hours of it. The Day view folds 5 of these per point
    /// and the Week view folds 60, so there is one writer and one resolution.
    std::vector<FleetBucket> _minutes;
    /// The 1-hour ring, 7 days. Folded from `_minutes` as they age out.
    std::vector<FleetBucket> _hours;
    std::uint64_t _generation { 0 };
};

} // namespace FastCache::Distributed
