// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/FleetSample.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Distributed
{

/// How often a sample is taken, and therefore what every ring is fed by.
///
/// At namespace scope rather than inside `FleetHistory`, because `FullCoverageOf`
/// divides by it and is declared before that class -- and the alternative was a
/// second literal 60 sitting where nothing would ever compare it to this one.
inline constexpr std::chrono::seconds FleetSampleInterval { 60 };

/// Which stored ring a view is folded from.
///
/// **Rings are storage; ranges are views.** Three rings serve eight windows because
/// a window is a fold of a ring, and nothing stores a "six month series" -- the Day
/// view already folds five minute-buckets and the Week view sixty. Adding a window
/// is therefore a range row, and adding a resolution is a ring.
enum class FleetRing : std::uint8_t
{
    Minute = 0, ///< 1-minute buckets, 24 hours of them.
    Hour,       ///< 1-hour buckets, 30 days.
    Day,        ///< 1-day buckets, 400 days.
    Last
};

/// One ring's geometry.
struct FleetRingRow
{
    FleetRing ring;              ///< The ring this row describes.
    std::string_view key;        ///< Stable name, for diagnostics and the file header.
    std::chrono::seconds bucket; ///< Width of one stored bucket.
    std::size_t slots;           ///< How many buckets the ring holds before it laps.
};

/// Every ring, in enumerator order.
///
/// The spans are what the retention costs: 1440 + 720 + 400 buckets is a **fixed**
/// ceiling rather than a projection, because the rings do not grow. A node reports
/// that number at startup rather than an operator estimating it.
inline constexpr EnumTable<FleetRing, FleetRingRow> FleetRingTable {
    FleetRingRow { .ring = FleetRing::Minute, .key = "minute", .bucket = std::chrono::minutes { 1 }, .slots = 24 * 60 },
    FleetRingRow { .ring = FleetRing::Hour, .key = "hour", .bucket = std::chrono::hours { 1 }, .slots = 30 * 24 },
    FleetRingRow { .ring = FleetRing::Day, .key = "day", .bucket = std::chrono::hours { 24 }, .slots = 400 },
};
static_assert(RowsInEnumeratorOrder(FleetRingTable, &FleetRingRow::ring));

/// How far back a view reaches, and how coarse its buckets are.
///
/// Shortest first, which is the order the control renders them in.
enum class FleetRange : std::uint8_t
{
    OneHour = 0,  ///< 1 hour of 1-minute buckets.
    TwoHours,     ///< 2 hours of 2-minute buckets.
    EightHours,   ///< 8 hours of 8-minute buckets.
    Day,          ///< 24 hours of 5-minute buckets.
    Week,         ///< 7 days of 1-hour buckets.
    Month,        ///< 30 days of 6-hour buckets.
    SixMonths,    ///< 6 months of 1-day buckets.
    TwelveMonths, ///< 12 months of 1-day buckets.
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
    /// Which ring this view folds from.
    ///
    /// A column rather than a conditional, which is what the two-range version had:
    /// `range == FleetRange::Week ? _hours : _minutes` is a ladder that grows an arm
    /// per window, and the eighth arm is the one somebody forgets.
    FleetRing ring;
};

/// Every range, in enumerator order. The geometry matches the mockup exactly.
inline constexpr EnumTable<FleetRange, FleetRangeRow> FleetRangeTable {
    FleetRangeRow { .range = FleetRange::OneHour,
                    .key = "1h",
                    .label = "1 H",
                    .bucketLabel = "1-minute buckets",
                    .bucket = std::chrono::minutes { 1 },
                    .points = 60,
                    .ring = FleetRing::Minute },
    FleetRangeRow { .range = FleetRange::TwoHours,
                    .key = "2h",
                    .label = "2 H",
                    .bucketLabel = "2-minute buckets",
                    .bucket = std::chrono::minutes { 2 },
                    .points = 60,
                    .ring = FleetRing::Minute },
    FleetRangeRow { .range = FleetRange::EightHours,
                    .key = "8h",
                    .label = "8 H",
                    .bucketLabel = "8-minute buckets",
                    .bucket = std::chrono::minutes { 8 },
                    .points = 60,
                    .ring = FleetRing::Minute },
    FleetRangeRow { .range = FleetRange::Day,
                    .key = "24h",
                    .label = "24 H",
                    .bucketLabel = "5-minute buckets",
                    .bucket = std::chrono::minutes { 5 },
                    .points = 288,
                    .ring = FleetRing::Minute },
    FleetRangeRow { .range = FleetRange::Week,
                    .key = "7d",
                    .label = "7 D",
                    .bucketLabel = "1-hour buckets",
                    .bucket = std::chrono::hours { 1 },
                    .points = 168,
                    .ring = FleetRing::Hour },
    FleetRangeRow { .range = FleetRange::Month,
                    .key = "1mo",
                    .label = "1 MO",
                    .bucketLabel = "6-hour buckets",
                    .bucket = std::chrono::hours { 6 },
                    .points = 120,
                    .ring = FleetRing::Hour },
    FleetRangeRow { .range = FleetRange::SixMonths,
                    .key = "6mo",
                    .label = "6 MO",
                    .bucketLabel = "1-day buckets",
                    .bucket = std::chrono::hours { 24 },
                    .points = 180,
                    .ring = FleetRing::Day },
    FleetRangeRow { .range = FleetRange::TwelveMonths,
                    .key = "12mo",
                    .label = "12 MO",
                    .bucketLabel = "1-day buckets",
                    .bucket = std::chrono::hours { 24 },
                    .points = 365,
                    .ring = FleetRing::Day },
};
static_assert(RowsInEnumeratorOrder(FleetRangeTable, &FleetRangeRow::range));

/// Whether every view can be drawn from the ring it names.
///
/// A range whose window reaches further back than its ring holds would render a
/// silent gap for everything past the ring's lap -- and the gap would look like a
/// fleet nobody was watching, which is the one thing this vocabulary must not say by
/// accident. Checked here rather than remembered, so the geometry cannot be edited
/// into a lie.
/// @return True when every range fits inside its ring's span.
[[nodiscard]] consteval bool RangesFitTheirRings() noexcept
{
    return std::ranges::all_of(FleetRangeTable, [](FleetRangeRow const& row) {
        auto const& ring = FleetRingTable[static_cast<std::size_t>(row.ring)];
        // Two things, and a range needs both. Its bucket must be a whole number of
        // the ring's, or a point would be folded from a fraction of one; and its
        // whole window must fit inside what the ring holds, or everything past the
        // lap renders as a gap -- which reads as "nobody was watching".
        auto const folds = row.bucket >= ring.bucket && row.bucket.count() % ring.bucket.count() == 0;
        auto const fits =
            row.bucket * static_cast<std::int64_t>(row.points) <= ring.bucket * static_cast<std::int64_t>(ring.slots);
        return folds && fits;
    });
}
static_assert(RangesFitTheirRings(), "a range must fold from its ring and must not reach past what that ring holds");

/// Look a range up by what a URL said.
/// @param key The `range=` value.
/// @return The row, or nullopt when nothing matches -- never a silent default.
[[nodiscard]] std::optional<FleetRange> FleetRangeFromKey(std::string_view key) noexcept;

/// What `FleetBucket::coverage` reads when a view's bucket was sampled throughout.
///
/// The divisor that gives coverage a meaning, and the reason it is keyed on a RANGE
/// rather than a ring: a ring is storage, and what a reader is looking at is a
/// view's bucket. It comes out the same either way -- a Day point folds five
/// minute-buckets of one sample each, a Month point six hour-buckets of sixty --
/// because every ring is fed by the same base interval, so the window's own width
/// over that interval is the whole of the arithmetic.
///
/// A bucket below this is *partially covered*, and its FOLD is what must not then be
/// compared like for like: a peak or a mean over three sampled hours of a day is not
/// the day's. Its `values` are unaffected, and so is any rate taken across them --
/// those come from `sampleMillis`, which is exact however thin the sampling was.
///
/// @param range Which view.
/// @return Samples in a fully covered bucket of it; never zero.
[[nodiscard]] constexpr std::uint64_t FullCoverageOf(FleetRange range) noexcept
{
    auto const width = FleetRangeTable[static_cast<std::size_t>(range)].bucket.count();
    return static_cast<std::uint64_t>(std::max<std::int64_t>(1, width / FleetSampleInterval.count()));
}

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
    /// One sample per minute is the base resolution; every ring folds from it.
    ///
    /// Kept as a member spelling of `FleetSampleInterval` because callers name it
    /// through this class, and a second literal would be a second source of truth
    /// for the number `FullCoverageOf` divides by.
    static constexpr std::chrono::seconds SampleInterval = FleetSampleInterval;

    /// @param wall Where a bucket's timestamp comes from; injected so a test can place one.
    explicit FleetHistory(IWallClock const& wall);

    /// Record one reading, folding it into whichever bucket its instant belongs to.
    ///
    /// Idempotent within a bucket: sampling twice inside the same minute overwrites
    /// rather than appends, so a jittery timer cannot double-count.
    /// @param values The nine readings, counters cumulative.
    void Record(EnumTable<FleetMetric, std::uint64_t> const& values);

    /// Replay a bucket another machine recorded, at ITS instant rather than now.
    ///
    /// The leader's half of the handover. Identical to `Record` except for where the
    /// timestamp comes from -- which is the whole point: a bucket arriving over a
    /// heartbeat describes a window that has already closed, and stamping it with the
    /// moment it happened to arrive would pile a node's whole catch-up into one
    /// window and leave the rest of the day a gap.
    ///
    /// Folds into the coarse rings exactly as a live sample does, so a node's sixty
    /// adopted minutes become one hour bucket with a coverage of sixty rather than
    /// sixty separate claims.
    /// @param bucket What the other machine reported.
    void Adopt(FleetBucket const& bucket);

    /// The buckets a range renders, oldest first, gaps included.
    /// @param range Which view.
    /// @return Exactly `FleetRangeTable[range].points` buckets.
    [[nodiscard]] std::vector<FleetBucket> Buckets(FleetRange range) const;

    /// Closed base-ring buckets newer than @p afterMillis, oldest first.
    ///
    /// What a node hands to its leader. **Closed only**: the newest bucket is still
    /// open and gains a reading every sample, so shipping it would send a partial
    /// window that the leader could never correct.
    ///
    /// Oldest first and bounded, because a node absent for a day has 1440 of these
    /// and a heartbeat has a payload ceiling -- so a catch-up makes progress from the
    /// far end across rounds rather than resending the newest and never closing the
    /// gap.
    ///
    /// The BASE ring only. The coarser rings are folds of it, and a leader recording
    /// these into its own history folds them again itself -- shipping all three would
    /// be shipping the same readings three times.
    ///
    /// @param afterMillis Exclusive lower bound on `startMillis`; -1 for everything.
    /// @param limit At most this many.
    /// @return The buckets, oldest first.
    [[nodiscard]] std::vector<FleetBucket> ClosedBucketsAfter(std::int64_t afterMillis, std::size_t limit) const;

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
    ///
    /// Refuses, leaving the file untouched, when `Load` found one a later build
    /// wrote -- see `ReadOnly`.
    /// @param path Where to write.
    /// @return True on success.
    bool Save(std::filesystem::path const& path) const;

    /// Whether this history refuses to write, because the file on disk is newer.
    ///
    /// Set by `Load` and never cleared: a node rolled back to an older build must
    /// not overwrite a year of readings the build it was rolled back from could
    /// still read. Sampling continues -- the page is live either way -- and only
    /// persistence stops.
    /// @return True when `Save` will decline.
    [[nodiscard]] bool ReadOnly() const noexcept;

  private:
    /// The store nests one of these per machine inside its own file.
    ///
    /// A friend rather than a public body accessor, because `AppendBody`/`ReadBody`
    /// are a serialization detail of this class and nothing else has business
    /// reaching them -- a public one would be an invitation to write a third encoder
    /// that drifts from these two.
    friend class FleetNodeHistories;

    /// This history's rings, as they sit in a file.
    ///
    /// Exposed so `FleetNodeHistories` can nest one per machine inside its own file
    /// rather than carrying a second copy of the ring encoding -- which would drift
    /// from this one the first time a bucket grew a field, and drift silently,
    /// because both halves would still round-trip against themselves.
    /// @param out Appended to.
    void AppendBody(std::string& out) const;

    /// Read rings back from a nested body.
    /// @param body The bytes `AppendBody` wrote.
    /// @return True when they were understood.
    bool ReadBody(std::string_view body);

    /// One reading at a stated instant; `Record` and `Adopt` differ only in that.
    /// @param now When the reading was taken.
    /// @param values The reading.
    void RecordAt(std::int64_t now, EnumTable<FleetMetric, std::uint64_t> const& values);

    [[nodiscard]] std::int64_t NowMillis() const noexcept;

    IWallClock const* _wall;
    mutable std::mutex _mutex;

    /// One ring per `FleetRingTable` row, in enumerator order.
    ///
    /// Every sample is folded into every ring, rather than a coarse ring being
    /// filled by buckets ageing out of the one below. Both give the same numbers;
    /// this one has a single writer per ring and no second code path that first runs
    /// twenty-four hours in and is therefore never exercised by anything short of a
    /// day-long test.
    EnumTable<FleetRing, std::vector<FleetBucket>> _rings;
    std::uint64_t _generation { 0 };

    /// The previous sample, for the per-sample delta a counter's peak rate needs.
    ///
    /// Held here rather than read back out of a ring, because the reading a bucket's
    /// FIRST sample must be compared against is the last one of the bucket BEFORE it
    /// -- a different slot, and in the day ring a different lap. Without it every
    /// bucket's peak rate would silently omit its opening step, which in a one-sample
    /// spike is the entire spike.
    EnumTable<FleetMetric, std::uint64_t> _last {};

    /// When `_last` was taken, so a delta across a GAP is not read as a peak.
    ///
    /// A node samples only while it leads, so a leadership change of three hours ends
    /// with one sample whose delta is three hours of work. Recorded as a per-minute
    /// peak it would be a spike that never happened -- persisted, and in the 400-day
    /// ring for over a year. The same rule the renderer already applies to a rate
    /// across a gap, enforced here because this fold cannot be undone afterwards.
    std::int64_t _lastMillis { 0 };
    bool _hasLast { false };

    /// Set when `Load` read a file a LATER build wrote.
    ///
    /// The only irreversible thing in this class. A node rolled back to an older
    /// build reads a newer file, cannot understand it, and would then overwrite a
    /// year of readings the newer build could still have read -- so it keeps the
    /// file and refuses to write, rather than starting empty and saving over it.
    /// Everything else here is recoverable by waiting; this is not.
    bool _readOnly { false };
};

/// Every node's own series, as the leader received them.
///
/// One history per NODE ENDPOINT, never per registry entry: a worker with two
/// `--toolchain` flags registers twice against one machine and both entries
/// heartbeat the same figures, so keying per entry would count one machine twice --
/// the rule `WorkerRegistry::NodeCaches()` already exists to enforce.
///
/// This is what makes a fleet's record survive an election. The leader's own fleet
/// series only covers windows it was leading for; these cover the rest, because each
/// machine kept recording itself throughout.
class FleetNodeHistories final: public IFleetHistorySink
{
  public:
    /// @param wall Where a bucket's timestamp comes from; injected so a test can place one.
    explicit FleetNodeHistories(IWallClock const& wall);

    /// Take what a node handed over.
    ///
    /// **Idempotent.** A bucket at or below what this endpoint has already
    /// contributed is ignored, so a heartbeat redelivered after a reply the node
    /// never saw cannot count twice. That high-water mark is what lets the wire carry
    /// no acknowledgement at all: the node advances its own watermark on a successful
    /// heartbeat and the leader is safe against the replay that follows a lost one.
    ///
    /// @param endpoint Which machine reported.
    /// @param buckets Its closed buckets, oldest first.
    /// @return How many were new.
    std::size_t AcceptHistory(std::string_view endpoint, std::span<FleetBucket const> buckets) override;

    /// Fill a rendered fleet view's gaps by summing what the nodes reported.
    ///
    /// **Gaps only.** A window this leader sampled itself is left exactly as it is:
    /// its readings are the registry's own totals and are what the page has always
    /// plotted. Only a window with no reading at all is filled, and it is then marked
    /// `backfilled` so nothing reads a dispatch counter off it.
    ///
    /// @param into The fleet view, as `FleetHistory::Buckets` returned it.
    /// @param range Which view, so the same windows are summed.
    void BackfillInto(std::vector<FleetBucket>& into, FleetRange range) const;

    /// How many machines have reported.
    [[nodiscard]] std::size_t Count() const;

    /// The newest bucket start this endpoint has contributed, or -1.
    /// @param endpoint Which machine.
    /// @return Its high-water mark.
    [[nodiscard]] std::int64_t HighWaterFor(std::string_view endpoint) const;

    /// Read a saved store back.
    ///
    /// Every failure -- absent, short, wrong version, bad checksum -- starts empty
    /// and says so, exactly as `FleetHistory::Load` does. History is a convenience,
    /// and no state of this file may keep a node from starting.
    /// @param path Where `Save` wrote.
    /// @return True when anything was restored.
    bool Load(std::filesystem::path const& path);

    /// Whether `Load` found a file a LATER build wrote, which is never written over.
    /// @return True when this store will refuse to save.
    [[nodiscard]] bool ReadOnly() const;

    /// Write the store out, atomically.
    ///
    /// Not an optimisation. A node advances its own watermark once a heartbeat is
    /// accepted and never resends, so a leader that forgot what it had been handed
    /// would leave those windows a gap for as long as the rings hold them -- which is
    /// the failure this whole phase removes, reintroduced by a restart.
    /// @param path Where to write.
    /// @return True on success.
    [[nodiscard]] bool Save(std::filesystem::path const& path) const;

  private:
    IWallClock const* _wall;
    mutable std::mutex _mutex;

    /// One entry per machine: its series, and how far it has reported.
    struct Entry
    {
        std::unique_ptr<FleetHistory> history; ///< What that machine recorded.
        std::int64_t highWater { -1 };         ///< Newest `startMillis` accepted from it.
    };
    std::map<std::string, Entry, std::less<>> _nodes;

    /// Set when `Load` read a file a LATER build wrote; see `FleetHistory::_readOnly`.
    ///
    /// The same rule, because it is the same loss: this store holds every OTHER
    /// machine's year, so overwriting one a newer build could still have read
    /// discards more than a node's own file does, not less.
    bool _readOnly { false };
};

} // namespace FastCache::Distributed
