// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Crc32c.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Distributed/FleetHistory.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <ranges>
#include <sstream>

namespace FastCache::Distributed
{

namespace
{
    /// Slots in the base ring: 24 hours at one sample a minute.
    constexpr std::size_t MinuteSlots = 24 * 60;
    /// Slots in the coarse ring: 7 days at one an hour.
    constexpr std::size_t HourSlots = 7 * 24;

    constexpr std::int64_t MinuteMillis = 60 * 1000;
    constexpr std::int64_t HourMillis = 60 * MinuteMillis;

    /// What this build writes, and the only thing it will read back.
    ///
    /// A file that could not say which build wrote it makes an upgrade a silent
    /// corruption, which is the reasoning `ClusterState`'s own version byte records.
    /// Unlike that one, a mismatch here is not an error: it starts empty.
    constexpr std::array<char, 4> FileMagic { 'F', 'C', 'F', 'H' };
    constexpr std::uint8_t FileVersion = 1;

    /// Which ring holds a bucket of this width, and how wide its slots are.
    [[nodiscard]] std::int64_t WidthMillisFor(FleetRange range) noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(FleetRangeTable[static_cast<std::size_t>(range)].bucket)
            .count();
    }

    /// Place one reading into a ring keyed by absolute bucket number.
    ///
    /// The slot carries its own start, so a slot left over from a previous lap is
    /// detectable rather than being mistaken for this lap's data -- which is what
    /// makes a gap after downtime render as a gap rather than as week-old numbers.
    /// @return True when this opened a bucket that was not already there.
    bool PlaceInto(std::vector<FleetBucket>& ring,
                   std::int64_t widthMillis,
                   std::int64_t nowMillis,
                   EnumTable<FleetMetric, std::uint64_t> const& values)
    {
        auto const number = nowMillis / widthMillis;
        auto const start = number * widthMillis;
        auto& slot = ring[static_cast<std::size_t>(number) % ring.size()];
        auto const opened = !slot.present || slot.startMillis != start;
        slot = FleetBucket { .startMillis = start, .values = values, .present = true };
        return opened;
    }

    /// The newest present sub-bucket inside one window, if any.
    ///
    /// "Newest" and not "sum", because counters here are stored raw and cumulative:
    /// the value at the end of a window is what makes the difference between two
    /// windows equal the work done between them.
    [[nodiscard]] std::optional<FleetBucket> NewestWithin(std::vector<FleetBucket> const& ring,
                                                          std::int64_t subWidthMillis,
                                                          std::int64_t windowStart,
                                                          std::int64_t windowEnd)
    {
        std::optional<FleetBucket> best;
        for (auto start = windowStart; start < windowEnd; start += subWidthMillis)
        {
            auto const number = start / subWidthMillis;
            auto const& slot = ring[static_cast<std::size_t>(number) % ring.size()];
            if (!slot.present || slot.startMillis != start)
                continue;
            if (!best.has_value() || slot.startMillis > best->startMillis)
                best = slot;
        }
        return best;
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
} // namespace

std::optional<FleetRange> FleetRangeFromKey(std::string_view key) noexcept
{
    for (auto const& row: FleetRangeTable)
        if (row.key == key)
            return row.range;
    return std::nullopt;
}

FleetHistory::FleetHistory(IWallClock const& wall):
    _wall { &wall },
    _minutes(MinuteSlots, FleetBucket {}),
    _hours(HourSlots, FleetBucket {})
{
}

std::int64_t FleetHistory::NowMillis() const noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(_wall->Now().time_since_epoch()).count();
}

void FleetHistory::Record(EnumTable<FleetMetric, std::uint64_t> const& values)
{
    auto const now = NowMillis();
    auto const guard = std::lock_guard { _mutex };

    // Both rings take every sample. The hour ring keeps the last reading of each
    // hour, which for a cumulative counter is exactly what makes the difference
    // between two hours the work done in between.
    auto const opened = PlaceInto(_minutes, MinuteMillis, now, values);
    PlaceInto(_hours, HourMillis, now, values);
    if (opened)
        ++_generation;
}

std::vector<FleetBucket> FleetHistory::Buckets(FleetRange range) const
{
    auto const& row = FleetRangeTable[static_cast<std::size_t>(range)];
    auto const width = WidthMillisFor(range);
    auto const now = NowMillis();

    auto const guard = std::lock_guard { _mutex };

    // The Day view folds five 1-minute slots per point; the Week view reads its own
    // ring one for one. One writer, one base resolution, and the fold lives here
    // rather than in the sampler -- so changing the geometry cannot lose data that
    // was already recorded.
    auto const& source = range == FleetRange::Week ? _hours : _minutes;
    auto const subWidth = range == FleetRange::Week ? HourMillis : MinuteMillis;

    auto const newest = now / width;
    auto const oldest = newest - static_cast<std::int64_t>(row.points) + 1;

    std::vector<FleetBucket> out;
    out.reserve(row.points);
    for (auto const index: std::views::iota(std::size_t { 0 }, row.points))
    {
        auto const start = (oldest + static_cast<std::int64_t>(index)) * width;
        auto found = NewestWithin(source, subWidth, start, start + width);
        if (found.has_value())
        {
            found->startMillis = start;
            out.push_back(*found);
        }
        else
            out.push_back(FleetBucket { .startMillis = start, .values = {}, .present = false });
    }
    return out;
}

std::uint64_t FleetHistory::Generation() const noexcept
{
    auto const guard = std::lock_guard { _mutex };
    return _generation;
}

bool FleetHistory::Empty() const noexcept
{
    auto const guard = std::lock_guard { _mutex };
    return std::ranges::none_of(_minutes, [](auto const& b) { return b.present; })
           && std::ranges::none_of(_hours, [](auto const& b) { return b.present; });
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

bool FleetHistory::Save(std::filesystem::path const& path) const
{
    std::string body;
    {
        auto const guard = std::lock_guard { _mutex };
        AppendU64(body, _generation);
        for (auto const* ring: { &_minutes, &_hours })
        {
            AppendU64(body, ring->size());
            for (auto const& bucket: *ring)
            {
                AppendU64(body, static_cast<std::uint64_t>(bucket.startMillis));
                AppendU64(body, bucket.present ? 1U : 0U);
                for (auto const value: bucket.values)
                    AppendU64(body, value);
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

    constexpr std::size_t HeaderSize = FileMagic.size() + 1 + sizeof(std::uint64_t) * 2;
    if (raw.size() < HeaderSize)
        return false;
    if (!std::equal(FileMagic.begin(), FileMagic.end(), raw.begin()))
        return false;
    if (static_cast<std::uint8_t>(raw[FileMagic.size()]) != FileVersion)
        return false;

    auto const declared = ReadU64(raw, FileMagic.size() + 1);
    auto const checksum = ReadU64(raw, FileMagic.size() + 1 + sizeof(std::uint64_t));
    std::string_view const body { raw.data() + HeaderSize, raw.size() - HeaderSize };
    if (body.size() != declared)
        return false;
    if (Crc32c::Compute(std::span { reinterpret_cast<std::byte const*>(body.data()), body.size() }) != checksum)
        return false;

    constexpr std::size_t Slots = EnumeratorCount<FleetMetric>;
    constexpr std::size_t PerBucket = (2 + Slots) * sizeof(std::uint64_t);

    std::size_t cursor = 0;
    auto const take = [&](std::uint64_t& into) {
        if (cursor + sizeof(std::uint64_t) > body.size())
            return false;
        into = ReadU64(body, cursor);
        cursor += sizeof(std::uint64_t);
        return true;
    };

    std::uint64_t generation = 0;
    if (!take(generation))
        return false;

    std::array<std::vector<FleetBucket>, 2> rings;
    for (auto& ring: rings)
    {
        std::uint64_t count = 0;
        if (!take(count) || count > MinuteSlots || body.size() - cursor < count * PerBucket)
            return false;
        ring.reserve(count);
        for ([[maybe_unused]] auto const _: std::views::iota(std::uint64_t { 0 }, count))
        {
            FleetBucket bucket;
            std::uint64_t start = 0;
            std::uint64_t present = 0;
            if (!take(start) || !take(present))
                return false;
            bucket.startMillis = static_cast<std::int64_t>(start);
            bucket.present = present != 0;
            for (auto& value: bucket.values)
                if (!take(value))
                    return false;
            ring.push_back(bucket);
        }
    }
    if (rings[0].size() != MinuteSlots || rings[1].size() != HourSlots)
        return false;

    auto const guard = std::lock_guard { _mutex };
    _minutes = std::move(rings[0]);
    _hours = std::move(rings[1]);
    // Restored, not reset: a generation that started again from zero would let a
    // client's cached chart match an ETag it had already been served.
    _generation = generation;
    return true;
}

} // namespace FastCache::Distributed
