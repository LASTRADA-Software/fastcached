// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Crc32c.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <ranges>
#include <span>
#include <sstream>

namespace FastCache::Distributed
{

namespace
{
    /// One ring's bucket width, in milliseconds.
    /// @param ring Which ring.
    /// @return Its bucket width.
    [[nodiscard]] constexpr std::int64_t RingWidthMillis(FleetRing ring) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(FleetRingTable[static_cast<std::size_t>(ring)].bucket)
            .count();
    }

    /// What this build writes.
    ///
    /// A file that could not say which build wrote it makes an upgrade a silent
    /// corruption, which is the reasoning `ClusterState`'s own version byte records.
    /// Unlike that one, an OLDER version is not an error -- `HistoryFormats` carries
    /// a reader for it and inflates it forward.
    constexpr std::array<char, 4> FileMagic { 'F', 'C', 'F', 'H' };
    constexpr std::uint8_t FileVersion = 2;

    /// The largest ring this build keeps, as a ceiling on a declared bucket count.
    ///
    /// Derived from the table rather than written down: a count a body could not
    /// possibly hold is refused before anything is allocated against it, and the
    /// bound has to move when a ring grows or it becomes the thing that rejects a
    /// legitimate file.
    constexpr std::size_t MaxRingSlots = []() consteval {
        std::size_t most = 0;
        for (auto const& row: FleetRingTable)
            most = std::max(most, row.slots);
        return most;
    }();

    /// Which ring holds a bucket of this width, and how wide its slots are.
    [[nodiscard]] std::int64_t WidthMillisFor(FleetRange range) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(FleetRangeTable[static_cast<std::size_t>(range)].bucket)
            .count();
    }

    /// Fold one reading into a ring, keyed by absolute bucket number.
    ///
    /// The slot carries its own start, so a slot left over from a previous lap is
    /// detectable rather than being mistaken for this lap's data -- which is what
    /// makes a gap after downtime render as a gap rather than as week-old numbers.
    ///
    /// Accumulates rather than overwrites, because a bucket wider than the sample
    /// interval is a FOLD and a fold that kept only its last reading has thrown the
    /// interesting part away: a refusal spike averaged over a day is invisible, and
    /// neither the peak nor the floor is recoverable afterwards.
    ///
    /// @param ring The ring to fold into.
    /// @param widthMillis Width of one of its buckets.
    /// @param nowMillis When this reading was taken.
    /// @param values The reading.
    /// @param previous The reading before this one, or null when there is none or
    ///                 when it is too old to subtract from.
    /// @return True when this opened a bucket that was not already there.
    bool AccumulateInto(std::vector<FleetBucket>& ring,
                        std::int64_t widthMillis,
                        std::int64_t nowMillis,
                        EnumTable<FleetMetric, std::uint64_t> const& values,
                        EnumTable<FleetMetric, std::uint64_t> const* previous)
    {
        auto const number = nowMillis / widthMillis;
        auto const start = number * widthMillis;
        auto& slot = ring[static_cast<std::size_t>(number) % ring.size()];
        auto const opened = !slot.present || slot.startMillis != start;

        if (opened)
            slot = FleetBucket { .startMillis = start, .sampleMillis = nowMillis, .values = values, .present = true };
        else
        {
            // `nowMillis` for the sample instant, NOT the aligned `start`. Stamping
            // the alignment throws the reading's real time away at the moment it is
            // recorded, and no later fold can recover it -- which made the divisor a
            // no-op for a view whose bucket IS its sub-bucket, since those are always
            // exactly one nominal width apart.
            slot.sampleMillis = nowMillis;
            slot.values = values;
        }

        for (auto const& row: FleetMetricTable)
        {
            auto const index = static_cast<std::size_t>(row.metric);
            auto const reading = values[index];
            auto& fold = slot.fold[index];

            if (row.kind == FleetMetricKind::Gauge)
            {
                fold.low = opened ? reading : std::min(fold.low, reading);
                fold.high = opened ? reading : std::max(fold.high, reading);
                fold.total = (opened ? 0 : fold.total) + reading;
                continue;
            }

            // A counter's peak is a RATE, so it needs the reading before this one --
            // which for a bucket's first sample sits in the bucket before it. A
            // counter has no meaningful floor or sum: its mean over a window is the
            // delta across it, and its minimum is not a fact anybody acts on.
            if (opened)
                fold = FleetFold {};
            // Backwards is a restart, whose step is unknowable rather than enormous.
            if (previous != nullptr && reading >= (*previous)[index])
                fold.high = std::max(fold.high, reading - (*previous)[index]);
        }

        slot.coverage = opened ? 1 : slot.coverage + 1;
        return opened;
    }

    /// Fold every present sub-bucket of one window into the bucket a view draws.
    ///
    /// `values` is the window's NEWEST reading rather than a sum, because counters
    /// are stored raw and cumulative: the value at a window's end is what makes the
    /// difference between two windows the work done between them.
    ///
    /// The `fold` is merged across the whole window instead, which is the entire
    /// reason it is carried -- taking the newest sub-bucket's copy would discard
    /// every spike that did not happen to land in the last one.
    ///
    /// @param ring The ring to read.
    /// @param subWidthMillis Width of one of its buckets.
    /// @param windowStart First instant of the window.
    /// @param windowEnd One past its last.
    /// @return The folded bucket, or nullopt when nothing in the window was sampled.
    [[nodiscard]] std::optional<FleetBucket> FoldWithin(std::vector<FleetBucket> const& ring,
                                                        std::int64_t subWidthMillis,
                                                        std::int64_t windowStart,
                                                        std::int64_t windowEnd)
    {
        std::optional<FleetBucket> out;
        for (auto start = windowStart; start < windowEnd; start += subWidthMillis)
        {
            auto const number = start / subWidthMillis;
            auto const& slot = ring[static_cast<std::size_t>(number) % ring.size()];
            if (!slot.present || slot.startMillis != start)
                continue;

            if (!out.has_value())
            {
                out = slot;
                continue;
            }

            if (slot.startMillis > out->startMillis)
            {
                out->startMillis = slot.startMillis;
                out->sampleMillis = slot.sampleMillis;
                out->values = slot.values;
            }

            for (auto const& row: FleetMetricTable)
            {
                auto const index = static_cast<std::size_t>(row.metric);
                auto& into = out->fold[index];
                auto const& from = slot.fold[index];
                // A counter's `low` and `total` are unused and stay zero, so the same
                // three lines serve both kinds rather than branching on one to do
                // nothing.
                into.low = std::min(into.low, from.low);
                into.high = std::max(into.high, from.high);
                into.total += from.total;
            }
            out->coverage += slot.coverage;
        }
        return out;
    }

    /// Big-endian, because that is what every other encoder in this tree writes;
    /// the choice is arbitrary for a private file and consistency is not.
    void AppendU64(std::string& out, std::uint64_t value)
    {
        auto const encoded = HostToBigEndian(value);
        std::array<char, sizeof(encoded)> bytes {};
        std::memcpy(bytes.data(), &encoded, sizeof(encoded));
        out.append(bytes.data(), bytes.size());
    }

    [[nodiscard]] std::uint64_t ReadU64(std::string_view bytes, std::size_t offset) noexcept
    {
        return ReadBigEndian<std::uint64_t>(
            std::span { reinterpret_cast<std::byte const*>(bytes.data()) + offset, sizeof(std::uint64_t) });
    }

    /// A cursor over a file's body, so a reader cannot walk off the end by omission.
    class BodyReader
    {
      public:
        /// @param body The bytes after the header.
        explicit BodyReader(std::string_view body) noexcept:
            _body { body }
        {
        }

        /// @param into Where the next big-endian `uint64` lands.
        /// @return False when the body is exhausted, leaving @p into untouched.
        bool Take(std::uint64_t& into) noexcept
        {
            if (_cursor + sizeof(std::uint64_t) > _body.size())
                return false;
            into = ReadU64(_body, _cursor);
            _cursor += sizeof(std::uint64_t);
            return true;
        }

      private:
        std::string_view _body;
        std::size_t _cursor { 0 };
    };

    /// The rings a reader recovered, before they are installed.
    struct LoadedHistory
    {
        EnumTable<FleetRing, std::vector<FleetBucket>> rings; ///< Sized to THIS build's geometry.
        std::uint64_t generation { 0 };                       ///< Restored, never reset.
    };

    /// Put a bucket where THIS build's geometry says it belongs.
    ///
    /// A ring's slot is `bucketNumber % slots`, so a ring whose size changed between
    /// builds has every one of its buckets at the wrong index -- copying the vector
    /// across would scatter a year of readings into positions that then read as a
    /// previous lap and are silently dropped. Re-placing by the bucket's own start is
    /// what makes a geometry change an inflation rather than a loss, and it costs
    /// nothing on a version whose geometry did not move.
    ///
    /// @param ring The destination, already sized by this build.
    /// @param widthMillis That ring's bucket width.
    /// @param bucket What was read.
    void ReplaceInto(std::vector<FleetBucket>& ring, std::int64_t widthMillis, FleetBucket const& bucket)
    {
        if (!bucket.present || ring.empty())
            return;
        auto const number = bucket.startMillis / widthMillis;
        auto& slot = ring[static_cast<std::size_t>(number) % ring.size()];
        // A ring that SHRANK maps several old buckets onto one slot, so the newest
        // wins -- the same rule a lap already uses.
        if (!slot.present || slot.startMillis < bucket.startMillis)
            slot = bucket;
    }

    /// Read one ring's buckets and re-place each of them.
    ///
    /// @param reader The cursor.
    /// @param ring Which ring these buckets belong to.
    /// @param into The destination.
    /// @param readBucket Reads one bucket in the format being served.
    /// @return False on a truncated or implausible body.
    bool ReadRing(BodyReader& reader, FleetRing ring, std::vector<FleetBucket>& into, auto const& readBucket)
    {
        std::uint64_t count = 0;
        if (!reader.Take(count) || count > MaxRingSlots)
            return false;
        for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, count))
        {
            FleetBucket bucket;
            if (!readBucket(reader, bucket))
                return false;
            ReplaceInto(into, RingWidthMillis(ring), bucket);
        }
        return true;
    }

    /// Read a whole body: the generation, then one ring per entry of @p rings.
    ///
    /// Every version so far has that shape and differs only in which rings it holds
    /// and how one bucket is spelled, so those are the two parameters and the rest is
    /// written once. A version that departs from the shape writes its own reader --
    /// which is what the format table is for.
    ///
    /// @param reader The cursor.
    /// @param out Where the rings land; sized to this build before anything is read.
    /// @param rings Which rings the body carries, in the order it carries them.
    /// @param readBucket Reads one bucket in the format being served.
    /// @return False on a truncated or implausible body.
    bool ReadBody(BodyReader& reader, LoadedHistory& out, std::span<FleetRing const> rings, auto const& readBucket)
    {
        // Sized to THIS build's geometry, not the file's: a ring the file is shorter
        // or longer than is the ordinary case across an upgrade, and `ReplaceInto`
        // puts each bucket where this build keeps it.
        for (auto const& row: FleetRingTable)
            out.rings[static_cast<std::size_t>(row.ring)].assign(row.slots, FleetBucket {});

        if (!reader.Take(out.generation))
            return false;
        for (auto const ring: rings)
            if (!ReadRing(reader, ring, out.rings[static_cast<std::size_t>(ring)], readBucket))
                return false;
        return true;
    }

    /// The rings version 1 carried: minute, then hour.
    constexpr std::array<FleetRing, 2> Version1Rings { FleetRing::Minute, FleetRing::Hour };

    /// The rings version 2 carries: every one this build has, in enumerator order.
    ///
    /// Derived from the table rather than listed, so a ring added there is written and
    /// read without this constant being edited -- and a build whose ring set differs
    /// from the file's is a different version by definition, which is why neither the
    /// writer nor the reader stores a count.
    constexpr auto Version2Rings = []() consteval {
        std::array<FleetRing, EnumeratorCount<FleetRing>> rings {};
        for (auto const& row: FleetRingTable)
            rings[static_cast<std::size_t>(row.ring)] = row.ring;
        return rings;
    }();

    /// Version 1: a generation, then two rings -- minute and hour -- of
    /// `[start][present][9 values]`, with no fold and no coverage.
    ///
    /// Inflated rather than discarded. `coverage` becomes 1, which is *literally*
    /// what a v1 bucket held: the writer overwrote, so however many samples fell in
    /// an hour, exactly one reading was kept. A gauge's fold collapses to that
    /// reading, and a counter's peak rate stays zero -- unknowable, and absent rather
    /// than invented.
    ///
    /// v1's hour ring held 168 slots against this build's 720, which is precisely the
    /// case `ReplaceInto` exists for.
    /// @param reader The cursor.
    /// @param out Where the rings land.
    /// @return False on a truncated or implausible body.
    bool ReadVersion1(BodyReader& reader, LoadedHistory& out)
    {
        return ReadBody(reader, out, Version1Rings, [](BodyReader& from, FleetBucket& bucket) {
            std::uint64_t start = 0;
            std::uint64_t present = 0;
            if (!from.Take(start) || !from.Take(present))
                return false;
            bucket.startMillis = static_cast<std::int64_t>(start);
            // Derived, and only for v1: that format kept one reading per bucket, so
            // the bucket's own start is as close to the sample instant as anything
            // recoverable from it. v2 stores the instant because its buckets fold.
            bucket.sampleMillis = bucket.startMillis;
            bucket.present = present != 0;
            for (auto& value: bucket.values)
                if (!from.Take(value))
                    return false;
            bucket.coverage = bucket.present ? 1 : 0;
            for (auto const& row: FleetMetricTable)
            {
                if (row.kind != FleetMetricKind::Gauge)
                    continue;
                auto const index = static_cast<std::size_t>(row.metric);
                auto const reading = bucket.values[index];
                bucket.fold[index] = FleetFold { .low = reading, .high = reading, .total = reading };
            }
            return true;
        });
    }

    /// Version 2: a generation, then one ring per `FleetRingTable` row of
    /// `[start][sample][present][coverage][9 values][9 x low,high,total]`.
    ///
    /// `sample` is stored rather than derived from `start`, because a ring that folds
    /// sixty readings into a bucket has no way to recover when the last of them
    /// landed -- and a rate divided by a nominal width instead of the span actually
    /// observed understates by however much of the window was never sampled.
    /// @param reader The cursor.
    /// @param out Where the rings land.
    /// @return False on a truncated or implausible body.
    bool ReadVersion2(BodyReader& reader, LoadedHistory& out)
    {
        return ReadBody(reader, out, Version2Rings, [](BodyReader& from, FleetBucket& bucket) {
            std::uint64_t start = 0;
            std::uint64_t sample = 0;
            std::uint64_t present = 0;
            if (!from.Take(start) || !from.Take(sample) || !from.Take(present) || !from.Take(bucket.coverage))
                return false;
            bucket.startMillis = static_cast<std::int64_t>(start);
            bucket.sampleMillis = static_cast<std::int64_t>(sample);
            bucket.present = present != 0;
            for (auto& value: bucket.values)
                if (!from.Take(value))
                    return false;
            for (auto& fold: bucket.fold)
                if (!from.Take(fold.low) || !from.Take(fold.high) || !from.Take(fold.total))
                    return false;
            return true;
        });
    }

    /// One readable on-disk format.
    struct HistoryFormat
    {
        std::uint8_t version;                      ///< What the header says.
        bool (*read)(BodyReader&, LoadedHistory&); ///< Reads a body of that version.
    };

    /// Every format this build can read, oldest first.
    ///
    /// A TABLE and not an equality test, which is the idiom `.agent/rules/storage.md`
    /// already fixes for the store: each row reads its own layout and inflates it
    /// forward, so adding a resolution is a row and the year already on disk survives
    /// the upgrade. Bumping `FileVersion` without adding a row here remains the
    /// decision to discard every history -- and is then a visible one, made by
    /// deleting a row rather than by not noticing a constant.
    constexpr std::array<HistoryFormat, 2> HistoryFormats {
        HistoryFormat { .version = 1, .read = &ReadVersion1 },
        HistoryFormat { .version = 2, .read = &ReadVersion2 },
    };

    /// The reader for a version, if this build has one.
    /// @param version What the header said.
    /// @return The row, or nullptr.
    [[nodiscard]] HistoryFormat const* FormatFor(std::uint8_t version) noexcept
    {
        for (auto const& row: HistoryFormats)
            if (row.version == version)
                return &row;
        return nullptr;
    }
} // namespace

std::optional<FleetRange> FleetRangeFromKey(std::string_view key) noexcept
{
    for (auto const& row: FleetRangeTable)
        if (row.key == key)
            return row.range;
    return std::nullopt;
}

FleetHistory::FleetHistory(IWallClock const& wall):
    _wall { &wall }
{
    // Sized from the table rather than from three named constants, so a ring added
    // there is allocated here without this constructor being edited.
    for (auto const& row: FleetRingTable)
        _rings[static_cast<std::size_t>(row.ring)].assign(row.slots, FleetBucket {});
}

std::int64_t FleetHistory::NowMillis() const noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(_wall->Now().time_since_epoch()).count();
}

void FleetHistory::Record(EnumTable<FleetMetric, std::uint64_t> const& values)
{
    auto const now = NowMillis();
    auto const guard = std::scoped_lock { _mutex };

    // A delta is only a rate when the two readings are ADJACENT. A node samples only
    // while it leads, so a leadership change of three hours ends with one sample
    // carrying three hours of work -- folded in as a per-minute peak that would be a
    // spike that never happened, persisted, and in the day ring for over a year. The
    // same rule the renderer applies to a rate across a gap, enforced here because
    // this fold cannot be undone afterwards. One interval of slack for a timer that
    // fired late.
    auto const adjacent = _hasLast && (now - _lastMillis) <= (2 * FleetSampleInterval.count() * 1000);
    auto const* previous = adjacent ? &_last : nullptr;

    // EVERY ring takes EVERY sample -- online accumulation, rather than a coarse ring
    // fed by buckets ageing out of the one below it. Both give the same numbers, and
    // this one has a single writer per ring and no second code path that first runs
    // twenty-four hours in and is therefore exercised by nothing short of a day-long
    // test.
    auto opened = false;
    for (auto const& row: FleetRingTable)
    {
        auto const index = static_cast<std::size_t>(row.ring);
        auto const openedHere = AccumulateInto(_rings[index], RingWidthMillis(row.ring), now, values, previous);
        // The generation follows the BASE ring, which is the one whose buckets close
        // fastest: an ETag that only moved when the day ring rolled would hold a
        // stale chart for a day.
        if (row.ring == FleetRing::Minute)
            opened = openedHere;
    }

    _last = values;
    _lastMillis = now;
    _hasLast = true;
    if (opened)
        ++_generation;
}

std::vector<FleetBucket> FleetHistory::Buckets(FleetRange range) const
{
    auto const& row = FleetRangeTable[static_cast<std::size_t>(range)];
    auto const width = WidthMillisFor(range);
    auto const subWidth = RingWidthMillis(row.ring);
    auto const now = NowMillis();

    auto const guard = std::scoped_lock { _mutex };

    // Which ring is a COLUMN of the range row, not a conditional. The two-range
    // version asked `range == FleetRange::Week ? _hours : _minutes`, which is a
    // ladder that grows an arm per window -- and the eighth arm is the one somebody
    // forgets, silently drawing a twelve-month view out of a ring holding a day.
    auto const& source = _rings[static_cast<std::size_t>(row.ring)];

    auto const newest = now / width;
    auto const oldest = newest - static_cast<std::int64_t>(row.points) + 1;

    std::vector<FleetBucket> out;
    out.reserve(row.points);
    for (auto const index: std::views::iota(std::size_t { 0 }, row.points))
    {
        auto const start = (oldest + static_cast<std::int64_t>(index)) * width;
        auto found = FoldWithin(source, subWidth, start, start + width);
        if (found.has_value())
        {
            // Only `startMillis` is restamped, to the window's start, so the chart
            // plots evenly. `sampleMillis` is carried through untouched: it is the
            // instant `AccumulateInto` recorded, and deriving it from the sub-bucket's
            // alignment here would throw away exactly what it exists to keep.
            found->startMillis = start;
            out.push_back(*found);
        }
        else
            out.push_back(FleetBucket { .startMillis = start, .sampleMillis = start, .values = {}, .present = false });
    }
    return out;
}

std::uint64_t FleetHistory::Generation() const noexcept
{
    auto const guard = std::scoped_lock { _mutex };
    return _generation;
}

bool FleetHistory::Empty() const noexcept
{
    auto const guard = std::scoped_lock { _mutex };
    return std::ranges::none_of(_rings, [](std::vector<FleetBucket> const& ring) {
        return std::ranges::any_of(ring, [](FleetBucket const& bucket) { return bucket.present; });
    });
}

std::chrono::seconds FleetHistory::UntilBucketCloses(FleetRange range) const noexcept
{
    auto const width = FleetRangeTable[static_cast<std::size_t>(range)].bucket;
    auto const widthMillis = std::chrono::milliseconds { width }.count();
    auto const elapsed = NowMillis() % widthMillis;
    auto const remaining = std::chrono::milliseconds { widthMillis - elapsed };
    // Rounded up and floored at a second: a `max-age=0` would make a browser
    // revalidate every image on every refresh, which is the cost this exists to
    // avoid rather than a freshness guarantee.
    return std::max(std::chrono::seconds { 1 }, std::chrono::ceil<std::chrono::seconds>(remaining));
}

bool FleetHistory::ReadOnly() const noexcept
{
    auto const guard = std::scoped_lock { _mutex };
    return _readOnly;
}

bool FleetHistory::Save(std::filesystem::path const& path) const
{
    std::string body;
    {
        auto const guard = std::scoped_lock { _mutex };

        // Refused BEFORE a byte is composed, and the file is left exactly as it was.
        // A node rolled back to an older build read a history it could not
        // understand; overwriting it would destroy a year of readings that the build
        // it was rolled back FROM could still have read. Everything else in this
        // class is recoverable by waiting.
        if (_readOnly)
            return false;

        AppendU64(body, _generation);
        // One ring per table row, in enumerator order -- the same order `ReadVersion2`
        // reads them back in, and the reason neither writes a ring count.
        for (auto const& row: FleetRingTable)
        {
            auto const& ring = _rings[static_cast<std::size_t>(row.ring)];
            AppendU64(body, ring.size());
            for (auto const& bucket: ring)
            {
                AppendU64(body, static_cast<std::uint64_t>(bucket.startMillis));
                AppendU64(body, static_cast<std::uint64_t>(bucket.sampleMillis));
                AppendU64(body, bucket.present ? 1U : 0U);
                AppendU64(body, bucket.coverage);
                for (auto const value: bucket.values)
                    AppendU64(body, value);
                for (auto const& fold: bucket.fold)
                {
                    AppendU64(body, fold.low);
                    AppendU64(body, fold.high);
                    AppendU64(body, fold.total);
                }
            }
        }
    }

    std::string file;
    file.append(FileMagic.data(), FileMagic.size());
    file.push_back(static_cast<char>(FileVersion));
    AppendU64(file, body.size());
    AppendU64(file, Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }));
    file += body;

    // Temp then rename: a crash between the two leaves the previous file whole
    // rather than half of this one.
    auto const temp = std::filesystem::path { path }.concat(".tmp");
    {
        std::ofstream out { temp, std::ios::binary | std::ios::trunc };
        if (!out)
            return false;
        out.write(file.data(), static_cast<std::streamsize>(file.size()));
        if (!out)
            return false;
    }

    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec)
    {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool FleetHistory::Load(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    if (!in)
        return false;

    // Through the stream buffer rather than an iterator: GCC at -O3 inlines
    // `istreambuf_iterator` far enough to trip `-Werror=null-dereference`, which
    // this codebase has already worked around twice.
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto const raw = buffer.str();

    constexpr std::size_t HeaderSize = FileMagic.size() + 1 + (sizeof(std::uint64_t) * 2);
    if (raw.size() < HeaderSize)
        return false;
    if (!std::equal(FileMagic.begin(), FileMagic.end(), raw.begin()))
        return false;

    auto const version = static_cast<std::uint8_t>(raw[FileMagic.size()]);
    auto const* format = FormatFor(version);
    if (format == nullptr)
    {
        // The magic matched, so this IS one of ours -- and a version this build has no
        // reader for, above the newest it knows, was written by a LATER build. Start
        // empty, but never write: see `_readOnly`. A version below the oldest row is a
        // format deliberately dropped, and overwriting that is the decision the row's
        // removal already made.
        if (version > FileVersion)
        {
            auto const guard = std::scoped_lock { _mutex };
            _readOnly = true;
        }
        return false;
    }

    auto const declared = ReadU64(raw, FileMagic.size() + 1);
    auto const checksum = ReadU64(raw, FileMagic.size() + 1 + sizeof(std::uint64_t));
    std::string_view const body { raw.data() + HeaderSize, raw.size() - HeaderSize };
    if (body.size() != declared)
        return false;
    if (Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }) != checksum)
        return false;

    LoadedHistory loaded;
    BodyReader reader { body };
    if (!format->read(reader, loaded))
        return false;

    auto const guard = std::scoped_lock { _mutex };
    _rings = std::move(loaded.rings);
    // Restored, not reset: a generation that started again from zero would let a
    // client's cached chart match an ETag it had already been served.
    _generation = loaded.generation;
    return true;
}

} // namespace FastCache::Distributed
