// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/ByteCursor.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

namespace FastCache
{

namespace
{

    using namespace StatsReadingWire;

    /// Presence bits of the snapshot's optional blocks, in the order the grammar lists them.
    enum PresenceBit : std::uint8_t
    {
        StoragePresent = 1U << 0U,
        HostPresent = 1U << 1U,
        UpstreamPresent = 1U << 2U,
        ConsensusPresent = 1U << 3U,
    };

    /// Every bit a presence byte may carry; anything else is `Malformed`.
    constexpr std::uint8_t KnownPresenceBits = StoragePresent | HostPresent | UpstreamPresent | ConsensusPresent;

    /// Bytes a bitmap over @p count items occupies.
    [[nodiscard]] constexpr std::size_t BitmapBytes(std::size_t count) noexcept
    {
        return (count + 7) / 8;
    }

    /// The fewest wire bytes one consensus member occupies: its `u32` length. A lower bound for
    /// `ByteCursor::ReadCount`, read off the encoder below.
    constexpr std::size_t MinMemberBytes = sizeof(std::uint32_t);

    class Writer
    {
      public:
        void U8(std::uint8_t value)
        {
            _out.push_back(static_cast<std::byte>(value));
        }

        void U32(std::uint32_t value)
        {
            auto const at = _out.size();
            _out.resize(at + sizeof(value));
            WriteBigEndian<std::uint32_t>(std::span { _out }.subspan(at, sizeof(value)), value);
        }

        void U64(std::uint64_t value)
        {
            auto const at = _out.size();
            _out.resize(at + sizeof(value));
            WriteBigEndian<std::uint64_t>(std::span { _out }.subspan(at, sizeof(value)), value);
        }

        void Text(std::string_view text)
        {
            U32(static_cast<std::uint32_t>(text.size()));
            for (auto const c: text)
                _out.push_back(static_cast<std::byte>(c));
        }

        /// Append a bitmap over @p count items whose bits @p isSet names.
        template <typename Predicate>
        void Bitmap(std::size_t count, Predicate isSet)
        {
            auto const at = _out.size();
            _out.resize(at + BitmapBytes(count), std::byte { 0 });
            for (auto const i: std::views::iota(std::size_t { 0 }, count))
                if (isSet(i))
                    _out[at + (i / 8)] |= static_cast<std::byte>(1U << (i % 8));
        }

        [[nodiscard]] std::vector<std::byte> Take() noexcept
        {
            return std::move(_out);
        }

      private:
        std::vector<std::byte> _out;
    };

    void WriteStorage(Writer& out, StorageStats const& stats)
    {
        for (auto const& field: StorageSizeFields)
            out.U64(static_cast<std::uint64_t>(stats.*field.member));
        for (auto const& field: StorageCounterFields)
            out.U64(stats.*field.member);
    }

    [[nodiscard]] bool ReadStorage(ByteCursor& in, StorageStats& stats)
    {
        for (auto const& field: StorageSizeFields)
        {
            std::uint64_t value = 0;
            if (!in.ReadU64(value))
                return false;
            stats.*field.member = static_cast<std::size_t>(value);
        }
        for (auto const& field: StorageCounterFields)
        {
            if (!in.ReadU64(stats.*field.member))
                return false;
        }
        return true;
    }

    /// Read a bitmap over @p count items.
    [[nodiscard]] std::optional<std::vector<bool>> ReadBitmap(ByteCursor& in, std::size_t count)
    {
        std::vector<bool> bits(count, false);
        auto byte = std::uint8_t { 0 };
        for (auto const i: std::views::iota(std::size_t { 0 }, count))
        {
            if (i % 8 == 0 && !in.ReadU8(byte))
                return std::nullopt;
            bits[i] = ((byte >> (i % 8)) & 1U) != 0;
        }
        // Bits past the last item are padding and must be clear, or two encodings of one reading
        // would differ -- and a set one is a sign of a layout this build does not have.
        if (count % 8 != 0 && (byte >> (count % 8)) != 0)
            return std::nullopt;
        return bits;
    }

} // namespace

std::vector<std::byte> EncodeStatsReading(StatsReading const& reading)
{
    Writer out;
    out.U64(StatsReadingLayout);

    out.Bitmap(reading.counters.size(), [&](std::size_t i) { return reading.counters[i].has_value(); });
    for (auto const& value: reading.counters)
        if (value.has_value())
            out.U64(*value);

    // Structured bindings, so a field added to either struct stops this compiling until it is
    // given a place in the grammar and a name in `SnapshotFieldNames` / `ConsensusFieldNames`.
    auto const& [storage, storageTiers, host, upstreamConfigured, consensus, uptime] = reading.snapshot;

    out.U8(static_cast<std::uint8_t>((storage.has_value() ? StoragePresent : 0U) | (host.has_value() ? HostPresent : 0U)
                                     | (upstreamConfigured.has_value() ? UpstreamPresent : 0U)
                                     | (consensus.has_value() ? ConsensusPresent : 0U)));

    if (storage.has_value())
        WriteStorage(out, *storage);

    out.Bitmap(storageTiers.size(), [&](std::size_t i) { return storageTiers[i].has_value(); });
    for (auto const& tier: storageTiers)
        if (tier.has_value())
            WriteStorage(out, *tier);

    if (host.has_value())
    {
        for (auto const& field: HostSizeFields)
            out.U64(static_cast<std::uint64_t>((*host).*field.member));
        for (auto const& field: HostByteFields)
            out.U64((*host).*field.member);
    }

    if (upstreamConfigured.has_value())
        out.U8(*upstreamConfigured ? 1U : 0U);

    if (consensus.has_value())
    {
        auto const& [members, knownLeader, term, commitIndex, role] = *consensus;
        out.U32(static_cast<std::uint32_t>(members.size()));
        for (auto const& member: members)
            out.Text(member);
        out.U8(knownLeader.has_value() ? 1U : 0U);
        if (knownLeader.has_value())
            out.Text(*knownLeader);
        out.U64(term.value);
        out.U64(commitIndex.value);
        out.U8(static_cast<std::uint8_t>(role));
    }

    out.U64(static_cast<std::uint64_t>(uptime.value.count()));
    return out.Take();
}

std::optional<std::uint64_t> DeclaredStatsReadingLayout(std::span<std::byte const> bytes) noexcept
{
    ByteCursor in { bytes };
    std::uint64_t layout = 0;
    if (!in.ReadU64(layout))
        return std::nullopt;
    return layout;
}

std::expected<StatsReading, StatsReadingFault> DecodeStatsReading(std::span<std::byte const> bytes)
{
    auto const declared = DeclaredStatsReadingLayout(bytes);
    if (!declared.has_value())
        return std::unexpected { StatsReadingFault::Truncated };
    if (*declared != StatsReadingLayout)
        return std::unexpected { StatsReadingFault::ForeignLayout };

    ByteCursor in { bytes, sizeof(std::uint64_t) };
    StatsReading reading;
    auto const truncated = std::unexpected { StatsReadingFault::Truncated };
    auto const malformed = std::unexpected { StatsReadingFault::Malformed };

    auto const counterBits = ReadBitmap(in, reading.counters.size());
    if (!counterBits.has_value())
        return in.Ok() ? malformed : truncated;
    for (auto const i: std::views::iota(std::size_t { 0 }, reading.counters.size()))
    {
        if (!(*counterBits)[i])
            continue;
        std::uint64_t value = 0;
        if (!in.ReadU64(value))
            return truncated;
        reading.counters[i] = value;
    }

    auto& [storage, storageTiers, host, upstreamConfigured, consensus, uptime] = reading.snapshot;

    auto presence = std::uint8_t { 0 };
    if (!in.ReadU8(presence))
        return truncated;
    if ((presence & ~KnownPresenceBits) != 0)
        return malformed;

    if ((presence & StoragePresent) != 0)
    {
        storage.emplace();
        if (!ReadStorage(in, *storage))
            return truncated;
    }

    auto const tierBits = ReadBitmap(in, storageTiers.size());
    if (!tierBits.has_value())
        return in.Ok() ? malformed : truncated;
    for (auto const i: std::views::iota(std::size_t { 0 }, storageTiers.size()))
    {
        if (!(*tierBits)[i])
            continue;
        storageTiers[i].emplace();
        if (!ReadStorage(in, *storageTiers[i]))
            return truncated;
    }

    if ((presence & HostPresent) != 0)
    {
        host.emplace();
        for (auto const& field: HostSizeFields)
        {
            std::uint64_t value = 0;
            if (!in.ReadU64(value))
                return truncated;
            (*host).*field.member = static_cast<std::size_t>(value);
        }
        for (auto const& field: HostByteFields)
        {
            if (!in.ReadU64((*host).*field.member))
                return truncated;
        }
    }

    if ((presence & UpstreamPresent) != 0)
    {
        auto flag = std::uint8_t { 0 };
        if (!in.ReadU8(flag))
            return truncated;
        if (flag > 1)
            return malformed;
        upstreamConfigured = flag == 1;
    }

    if ((presence & ConsensusPresent) != 0)
    {
        auto& status = consensus.emplace();
        auto& [members, knownLeader, term, commitIndex, role] = status;
        std::uint32_t count = 0;
        if (!in.ReadCount(count, MinMemberBytes))
            return truncated;
        // No `reserve(count)`: the count is bounded by the bytes present, not by anything this
        // side owns, and a member is larger in memory than its four-byte minimum on the wire.
        for ([[maybe_unused]] auto const member: std::views::iota(std::uint32_t { 0 }, count))
        {
            if (!in.ReadField(members.emplace_back()))
                return truncated;
        }
        auto leaderPresent = std::uint8_t { 0 };
        if (!in.ReadU8(leaderPresent))
            return truncated;
        if (leaderPresent > 1)
            return malformed;
        if (leaderPresent == 1 && !in.ReadField(knownLeader.emplace()))
            return truncated;
        auto roleByte = std::uint8_t { 0 };
        if (!in.ReadU64(term.value) || !in.ReadU64(commitIndex.value) || !in.ReadU8(roleByte))
            return truncated;
        if (roleByte >= static_cast<std::uint8_t>(Consensus::Role::Last))
            return malformed;
        role = static_cast<Consensus::Role>(roleByte);
    }

    std::uint64_t seconds = 0;
    if (!in.ReadU64(seconds))
        return truncated;
    uptime = Uptime { std::chrono::seconds { static_cast<std::chrono::seconds::rep>(seconds) } };

    if (!in.AtEnd())
        return std::unexpected { StatsReadingFault::TrailingBytes };
    return reading;
}

} // namespace FastCache
