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
#include <utility>

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

    /// What a store's file says about itself.
    ///
    /// A file that could not say which build wrote it makes an upgrade a silent
    /// corruption, which is the reasoning `ClusterState`'s own version byte records.
    /// Unlike that one, an OLDER version is not necessarily an error: `HistoryFormats`
    /// carries a reader for one and inflates it forward.
    ///
    /// A descriptor rather than a base class, because the two stores share the
    /// framing and nothing else -- one holds rings, the other a body per machine. A
    /// third store is then a third constant, and it gets the durability, the checksum
    /// and the never-overwrite-a-newer-file rule by construction rather than by
    /// somebody remembering to copy them.
    struct FileEnvelope
    {
        std::array<char, 4> magic; ///< What this file is.
        std::uint8_t newest;       ///< The newest version this build writes.
    };

    /// A node's own series: three rings in one file.
    constexpr FileEnvelope RingFile { .magic = { 'F', 'C', 'F', 'H' }, .newest = 2 };

    /// The received-histories store: one nested `FleetHistory` body per machine.
    ///
    /// Its own magic, so a path typed into the wrong flag is refused by name rather
    /// than read as a truncated history.
    constexpr FileEnvelope NodeStoreFile { .magic = { 'F', 'C', 'N', 'H' }, .newest = 1 };

    /// Magic, version byte, body length, body checksum.
    constexpr std::size_t EnvelopeSize = 4 + 1 + (sizeof(std::uint64_t) * 2);

    /// What one bucket costs in a body: four scalars, the readings, and the fold.
    ///
    /// Derived from the metric table rather than written down, so a tenth metric
    /// moves it. Only a reservation hint -- being wrong costs a reallocation, not a
    /// wrong file -- but a hint nothing keeps current is a hint that stops helping.
    constexpr std::size_t BucketBytes =
        (4 + EnumeratorCount<FleetMetric> + (EnumeratorCount<FleetMetric> * 3)) * sizeof(std::uint64_t);

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

    /// Stamp a big-endian word over one already appended.
    /// @param out The buffer.
    /// @param offset Where the word sits.
    /// @param value What it should say.
    void WriteU64(std::string& out, std::size_t offset, std::uint64_t value)
    {
        auto const encoded = HostToBigEndian(value);
        std::array<char, sizeof(encoded)> bytes {};
        std::memcpy(bytes.data(), &encoded, sizeof(encoded));
        std::ranges::copy(bytes, out.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    [[nodiscard]] std::uint64_t ReadU64(std::string_view bytes, std::size_t offset) noexcept
    {
        return ReadBigEndian<std::uint64_t>(
            std::span { reinterpret_cast<std::byte const*>(bytes.data()) + offset, sizeof(std::uint64_t) });
    }

    /// A buffer with the header's bytes reserved in front, ready for a body.
    ///
    /// The body is appended straight into the buffer that becomes the file, because
    /// a received-histories store is most of a megabyte per machine: composing the
    /// body separately would copy all of it twice, once to join the header and once
    /// again to reach the stream.
    /// @return The buffer, `EnvelopeSize` placeholder bytes long.
    [[nodiscard]] std::string FramedBuffer()
    {
        return std::string(EnvelopeSize, char { 0 });
    }

    /// Stamp the header onto such a buffer and put it where a crash cannot leave
    /// half of it.
    /// @param path Where the file belongs.
    /// @param envelope What to stamp.
    /// @param file The whole file: header space, then the body.
    /// @return True when the rename succeeded.
    [[nodiscard]] bool WriteFramed(std::filesystem::path const& path, FileEnvelope const& envelope, std::string& file)
    {
        std::string_view const body { file.data() + EnvelopeSize, file.size() - EnvelopeSize };

        std::string header;
        header.reserve(EnvelopeSize);
        header.append(envelope.magic.data(), envelope.magic.size());
        header.push_back(static_cast<char>(envelope.newest));
        AppendU64(header, body.size());
        AppendU64(header, Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }));
        std::ranges::copy(header, file.begin());

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

    /// What reading a framed file produced.
    enum class FramedOutcome : std::uint8_t
    {
        Ok,      ///< The body is there and its checksum matched.
        Missing, ///< Absent, short, not ours, or damaged: start empty, overwrite freely.
        Newer,   ///< Ours, and written by a LATER build: start empty and NEVER write.
    };

    /// A framed file's contents.
    ///
    /// `body` OWNS its bytes rather than viewing a buffer this returned by value: a
    /// struct a decoder hands back must not borrow from what it decoded, or the
    /// obvious spelling is a use-after-free.
    struct FramedFile
    {
        FramedOutcome outcome { FramedOutcome::Missing }; ///< Whether there is anything to read.
        std::uint8_t version { 0 };                       ///< Which build's shape the body is in.
        std::string body;                                 ///< The bytes after the header.
    };

    /// Read a framed file and check everything about it except the shape of its body.
    /// @param path Where to look.
    /// @param envelope What the file must claim to be.
    /// @return The body, or why there is none.
    [[nodiscard]] FramedFile ReadFramed(std::filesystem::path const& path, FileEnvelope const& envelope)
    {
        std::ifstream in { path, std::ios::binary };
        if (!in)
            return {};

        // Sized and read in one go rather than through `istreambuf_iterator`: GCC at
        // -O3 inlines that far enough to trip `-Werror=null-dereference`, which this
        // codebase has already worked around twice.
        in.seekg(0, std::ios::end);
        auto const end = in.tellg();
        if (end < 0)
            return {};
        in.seekg(0, std::ios::beg);

        std::string raw(static_cast<std::size_t>(end), char { 0 });
        in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
        if (in.gcount() != static_cast<std::streamsize>(raw.size()))
            return {};

        if (raw.size() < EnvelopeSize)
            return {};
        if (!std::equal(envelope.magic.begin(), envelope.magic.end(), raw.begin()))
            return {};

        auto const version = static_cast<std::uint8_t>(raw[envelope.magic.size()]);
        // The magic matched, so this IS one of ours -- and a version above the newest
        // this build writes was written by a LATER one. Everything else here is
        // recoverable by waiting; overwriting a year of readings that the newer build
        // could still have read is not.
        if (version > envelope.newest)
            return FramedFile { .outcome = FramedOutcome::Newer, .version = version, .body = {} };

        auto const declared = ReadU64(raw, envelope.magic.size() + 1);
        auto const checksum = ReadU64(raw, envelope.magic.size() + 1 + sizeof(std::uint64_t));
        // The header is dropped rather than copied past: what is left IS the body.
        raw.erase(0, EnvelopeSize);
        if (raw.size() != declared)
            return {};
        if (Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(raw.data()), raw.size() }) != checksum)
            return {};

        return FramedFile { .outcome = FramedOutcome::Ok, .version = version, .body = std::move(raw) };
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

        /// @param length How many bytes to take.
        /// @param into A view of them, valid as long as the body is.
        /// @return False when the body is exhausted, leaving @p into untouched.
        ///
        /// Written as a remaining-length comparison rather than `_cursor + length`,
        /// because a length read off a damaged file can overflow that sum and let a
        /// bounds check pass on a span that runs off the end.
        bool Take(std::uint64_t length, std::string_view& into) noexcept
        {
            if (length > _body.size() - _cursor)
                return false;
            into = _body.substr(_cursor, static_cast<std::size_t>(length));
            _cursor += static_cast<std::size_t>(length);
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
    RecordAt(NowMillis(), values);
}

void FleetHistory::Adopt(FleetBucket const& bucket)
{
    if (!bucket.present)
        return;
    // Its own sample instant, not this leader's clock. See the header: stamping a
    // catch-up with the moment it arrived would pile a day into one window.
    RecordAt(bucket.sampleMillis, bucket.values);
}

void FleetHistory::RecordAt(std::int64_t now, EnumTable<FleetMetric, std::uint64_t> const& values)
{
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

std::vector<FleetBucket> FleetHistory::ClosedBucketsAfter(std::int64_t afterMillis, std::size_t limit) const
{
    if (limit == 0)
        return {};

    auto const width = RingWidthMillis(FleetRing::Minute);
    auto const now = NowMillis();
    // The bucket `now` falls in is still OPEN -- it gains a reading every sample --
    // so the newest one that may travel is the one before it. Sending an open bucket
    // would hand the leader a partial window it could never be told to correct.
    auto const newestClosedStart = ((now / width) * width) - width;

    auto const guard = std::scoped_lock { _mutex };
    auto const& ring = _rings[static_cast<std::size_t>(FleetRing::Minute)];

    // Selected by INDEX first, and the buckets copied only once the batch is known.
    // A node catching up after a restart matches most of a day's ring, and a bucket
    // is three hundred bytes: sorting the buckets themselves moved half a megabyte
    // per heartbeat round to keep a hundred and twenty-eight of them, under this
    // lock, while the sampler waited to record the next one.
    std::vector<std::pair<std::int64_t, std::size_t>> chosen;
    for (auto const index: std::views::iota(std::size_t { 0 }, ring.size()))
    {
        auto const& slot = ring[index];
        if (!slot.present || slot.startMillis <= afterMillis || slot.startMillis > newestClosedStart)
            continue;
        chosen.emplace_back(slot.startMillis, index);
    }

    // A ring is stored by `number % slots`, so its iteration order is wherever the
    // lap happens to sit rather than time order. Ordered here because "oldest first"
    // is the whole of how a catch-up converges: taking an arbitrary 128 would leave
    // the gap open forever. Only the prefix that fits is fully ordered.
    auto const taken = std::min(limit, chosen.size());
    std::ranges::partial_sort(chosen, chosen.begin() + static_cast<std::ptrdiff_t>(taken));

    std::vector<FleetBucket> out;
    out.reserve(taken);
    for (auto const& [start, index]: std::span { chosen }.first(taken))
        out.push_back(ring[index]);
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

void FleetHistory::AppendBody(std::string& out) const
{
    // Its OWN lock, not the caller's. `FleetNodeHistories::Save` walks a map of
    // these and holds only the map's mutex, so a body composed under that alone
    // would be read while the machine it belongs to was still recording into it.
    auto const guard = std::scoped_lock { _mutex };

    // The whole of what this appends, so a store of a hundred machines does not
    // grow its buffer twenty times per machine.
    auto reserved = sizeof(std::uint64_t);
    for (auto const& row: FleetRingTable)
        reserved += sizeof(std::uint64_t) + (_rings[static_cast<std::size_t>(row.ring)].size() * BucketBytes);
    out.reserve(out.size() + reserved);

    AppendU64(out, _generation);
    // One ring per table row, in enumerator order -- the same order `ReadVersion2`
    // reads them back in, and the reason neither writes a ring count.
    for (auto const& row: FleetRingTable)
    {
        auto const& ring = _rings[static_cast<std::size_t>(row.ring)];
        AppendU64(out, ring.size());
        for (auto const& bucket: ring)
        {
            AppendU64(out, static_cast<std::uint64_t>(bucket.startMillis));
            AppendU64(out, static_cast<std::uint64_t>(bucket.sampleMillis));
            AppendU64(out, bucket.present ? 1U : 0U);
            AppendU64(out, bucket.coverage);
            for (auto const value: bucket.values)
                AppendU64(out, value);
            for (auto const& fold: bucket.fold)
            {
                AppendU64(out, fold.low);
                AppendU64(out, fold.high);
                AppendU64(out, fold.total);
            }
        }
    }
}

bool FleetHistory::ReadBody(std::string_view body)
{
    LoadedHistory loaded;
    BodyReader reader { body };
    if (!ReadVersion2(reader, loaded))
        return false;

    auto const guard = std::scoped_lock { _mutex };
    _rings = std::move(loaded.rings);
    _generation = loaded.generation;
    return true;
}

bool FleetHistory::Save(std::filesystem::path const& path) const
{
    // Refused BEFORE a byte is composed, and the file is left exactly as it was. A
    // node rolled back to an older build read a history it could not understand;
    // overwriting it would destroy a year of readings that the build it was rolled
    // back FROM could still have read. Everything else in this class is recoverable
    // by waiting.
    if (ReadOnly())
        return false;

    auto file = FramedBuffer();
    AppendBody(file);
    return WriteFramed(path, RingFile, file);
}

bool FleetHistory::Load(std::filesystem::path const& path)
{
    auto const opened = ReadFramed(path, RingFile);
    if (opened.outcome == FramedOutcome::Newer)
    {
        auto const guard = std::scoped_lock { _mutex };
        _readOnly = true;
        return false;
    }
    if (opened.outcome != FramedOutcome::Ok)
        return false;

    // A version at or below the newest this build writes, and still no reader, is a
    // format deliberately dropped from the table. Overwriting that is the decision
    // the row's removal already made.
    auto const* const format = FormatFor(opened.version);
    if (format == nullptr)
        return false;

    LoadedHistory loaded;
    BodyReader reader { opened.body };
    if (!format->read(reader, loaded))
        return false;

    auto const guard = std::scoped_lock { _mutex };
    _rings = std::move(loaded.rings);
    // Restored, not reset: a generation that started again from zero would let a
    // client's cached chart match an ETag it had already been served.
    _generation = loaded.generation;
    return true;
}

FleetNodeHistories::FleetNodeHistories(IWallClock const& wall):
    _wall { &wall }
{
}

std::size_t FleetNodeHistories::AcceptHistory(std::string_view endpoint, std::span<FleetBucket const> buckets)
{
    // The map's lock covers finding the entry and moving its mark, and nothing else.
    // Adopting a batch walks three rings up to a hundred and twenty-eight times, and
    // doing that here would put every other machine's heartbeat behind this one's.
    // The entry itself outlives the lock: nothing ever erases one, and the history is
    // held by pointer so a rehash cannot move it.
    Entry* entry = nullptr;
    {
        auto const guard = std::scoped_lock { _mutex };
        auto found = _nodes.find(endpoint);
        if (found == _nodes.end())
            found = _nodes
                        .emplace(std::string { endpoint },
                                 Entry { .history = std::make_unique<FleetHistory>(*_wall), .highWater = -1 })
                        .first;
        entry = &found->second;
    }

    std::size_t taken = 0;
    for (auto const& bucket: buckets)
    {
        // At or below the mark is a replay, not news. Ignored rather than refused:
        // the node is behaving correctly when it resends after a reply it never saw,
        // and the only wrong answer here is to add the reading twice.
        if (!bucket.present || bucket.startMillis <= entry->highWater)
            continue;
        entry->history->Adopt(bucket);
        // Plain assignment: the guard above already established that this bucket is
        // strictly newer, so a `max` could only ever pick it.
        entry->highWater = bucket.startMillis;
        ++taken;
    }
    return taken;
}

void FleetNodeHistories::BackfillInto(std::vector<FleetBucket>& into, FleetRange range) const
{
    // The steady-state case on a leader that has been up as long as the view is
    // wide: every window is its own, and there is nothing to fill. Asked before a
    // single machine's series is rendered, because rendering them all is the
    // expensive half of this function.
    if (std::ranges::none_of(into, [](FleetBucket const& each) { return !each.present; }))
        return;

    std::vector<FleetHistory const*> histories;
    {
        auto const guard = std::scoped_lock { _mutex };
        histories.reserve(_nodes.size());
        for (auto const& [endpoint, entry]: _nodes)
            histories.push_back(entry.history.get());
    }
    if (histories.empty())
        return;

    // Each machine's own view of the same windows, so the sums line up with the
    // fleet view being filled rather than with whatever resolution happened to be
    // stored. Rendered with the map's lock RELEASED: this is a page being drawn, and
    // a heartbeat must not wait behind it.
    std::vector<std::vector<FleetBucket>> perNode;
    perNode.reserve(histories.size());
    for (auto const* history: histories)
        perNode.push_back(history->Buckets(range));

    for (auto const index: std::views::iota(std::size_t { 0 }, into.size()))
    {
        // A window this leader sampled is left alone. Its readings are the registry's
        // own totals -- what the page has always plotted -- and replacing them with a
        // sum assembled from a different set of machines would make the chart step
        // wherever the two disagreed.
        if (into[index].present)
            continue;

        FleetBucket filled = into[index];
        auto contributors = std::size_t { 0 };
        for (auto const& node: perNode)
        {
            if (index >= node.size() || !node[index].present)
                continue;
            ++contributors;
            filled.sampleMillis = std::max(filled.sampleMillis, node[index].sampleMillis);
            filled.coverage = std::max(filled.coverage, node[index].coverage);
            for (auto const& row: FleetMetricTable)
            {
                // NODE-scoped only. A machine cannot answer for a dispatch outcome,
                // so summing one would be summing zeroes into a number a reader would
                // take for a measurement.
                if (row.scope != FleetMetricScope::Node)
                    continue;
                auto const slot = static_cast<std::size_t>(row.metric);
                filled.values[slot] += node[index].values[slot];
                filled.fold[slot].low += node[index].fold[slot].low;
                filled.fold[slot].high += node[index].fold[slot].high;
                filled.fold[slot].total += node[index].fold[slot].total;
            }
        }

        if (contributors == 0)
            continue;
        filled.present = true;
        filled.backfilled = true;
        into[index] = filled;
    }
}

bool FleetNodeHistories::Save(std::filesystem::path const& path) const
{
    // The same refusal `FleetHistory::Save` makes, and for the same reason: a build
    // that cannot read this file must not replace it with one it can.
    std::vector<std::pair<std::string, Entry const*>> listed;
    {
        auto const guard = std::scoped_lock { _mutex };
        if (_readOnly)
            return false;
        listed.reserve(_nodes.size());
        for (auto const& [endpoint, entry]: _nodes)
            listed.emplace_back(endpoint, &entry);
    }

    // Composed with the map's lock RELEASED. One machine's series is most of a
    // megabyte, so serializing a fleet of them under the lock every heartbeat has to
    // take would put each inbound `Accept` behind all of it. Each `FleetHistory`
    // guards itself, and nothing erases an entry once made.
    auto file = FramedBuffer();
    AppendU64(file, listed.size());
    for (auto const& [endpoint, entry]: listed)
    {
        AppendU64(file, endpoint.size());
        file += endpoint;
        AppendU64(file, static_cast<std::uint64_t>(entry->highWater));
        // The nested body, written by the very code that reads it back. A second copy
        // of the ring encoding here would drift from that one the first time a bucket
        // grew a field -- and would drift silently, because both halves would still
        // round-trip against themselves.
        //
        // Its length is stamped afterwards rather than composed apart and joined,
        // because joining copies the whole of it a second time.
        auto const lengthAt = file.size();
        AppendU64(file, 0);
        auto const bodyAt = file.size();
        entry->history->AppendBody(file);
        WriteU64(file, lengthAt, file.size() - bodyAt);
    }
    return WriteFramed(path, NodeStoreFile, file);
}

bool FleetNodeHistories::Load(std::filesystem::path const& path)
{
    auto const opened = ReadFramed(path, NodeStoreFile);
    if (opened.outcome == FramedOutcome::Newer)
    {
        auto const guard = std::scoped_lock { _mutex };
        _readOnly = true;
        return false;
    }
    if (opened.outcome != FramedOutcome::Ok)
        return false;

    BodyReader reader { opened.body };
    std::uint64_t count = 0;
    if (!reader.Take(count))
        return false;
    // A count no body could hold is refused before anything is allocated against it,
    // the same guard `ReadRing` applies to a bucket count.
    if (count > opened.body.size())
        return false;

    std::map<std::string, Entry, std::less<>> restored;
    for ([[maybe_unused]] auto const index: std::views::iota(std::uint64_t { 0 }, count))
    {
        std::uint64_t length = 0;
        std::string_view endpoint;
        std::uint64_t highWater = 0;
        std::uint64_t nestedSize = 0;
        std::string_view nested;
        if (!reader.Take(length) || !reader.Take(length, endpoint) || !reader.Take(highWater) || !reader.Take(nestedSize)
            || !reader.Take(nestedSize, nested))
            return false;

        auto history = std::make_unique<FleetHistory>(*_wall);
        if (!history->ReadBody(nested))
            return false;
        restored.emplace(std::string { endpoint },
                         Entry { .history = std::move(history), .highWater = static_cast<std::int64_t>(highWater) });
    }

    auto const guard = std::scoped_lock { _mutex };
    _nodes = std::move(restored);
    return true;
}

bool FleetNodeHistories::ReadOnly() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _readOnly;
}

std::size_t FleetNodeHistories::Count() const
{
    auto const guard = std::scoped_lock { _mutex };
    return _nodes.size();
}

std::int64_t FleetNodeHistories::HighWaterFor(std::string_view endpoint) const
{
    auto const guard = std::scoped_lock { _mutex };
    auto const found = _nodes.find(endpoint);
    return found == _nodes.end() ? -1 : found->second.highWater;
}

} // namespace FastCache::Distributed
