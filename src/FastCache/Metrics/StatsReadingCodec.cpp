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

    // Presence bits of the snapshot's optional blocks, in the order the grammar lists them. Named
    // constants rather than an unscoped enum: they are bits OR-ed into one wire byte, not a choice.
    constexpr std::uint8_t StoragePresent = 1U << 0U;   ///< `MetricsSnapshot::storage`.
    constexpr std::uint8_t HostPresent = 1U << 1U;      ///< `MetricsSnapshot::host`.
    constexpr std::uint8_t UpstreamPresent = 1U << 2U;  ///< `MetricsSnapshot::upstreamConfigured`.
    constexpr std::uint8_t ConsensusPresent = 1U << 3U; ///< `MetricsSnapshot::consensus`.
    constexpr std::uint8_t HostLoadPresent = 1U << 4U;  ///< `MetricsSnapshot::hostLoad`.

    /// Every bit a presence byte may carry; anything else is `Malformed`.
    constexpr std::uint8_t KnownPresenceBits =
        StoragePresent | HostPresent | UpstreamPresent | ConsensusPresent | HostLoadPresent;

    // Presence bits inside a host-load block, in the order its figures travel.
    constexpr std::uint8_t CpuPresent = 1U << 0U;             ///< `HostLoadReading::cpu`.
    constexpr std::uint8_t AvailableMemoryPresent = 1U << 1U; ///< `HostLoadReading::availableMemoryBytes`.

    /// Every bit a host-load presence byte may carry; anything else is `Malformed`.
    constexpr std::uint8_t KnownHostLoadBits = CpuPresent | AvailableMemoryPresent;

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

    /// Read a host block.
    /// @param in The bytes, positioned at the block.
    /// @param host Where the fields go.
    /// @return False when the bytes end first.
    [[nodiscard]] bool ReadHost(ByteCursor& in, HostCapacity& host)
    {
        for (auto const& field: HostSizeFields)
        {
            std::uint64_t value = 0;
            if (!in.ReadU64(value))
                return false;
            host.*field.member = static_cast<std::size_t>(value);
        }
        for (auto const& field: HostByteFields)
        {
            if (!in.ReadU64(host.*field.member))
                return false;
        }
        return true;
    }

    /// Read a consensus block.
    /// @param in The bytes, positioned at the block.
    /// @param status Where the fields go.
    /// @return Nothing when the block read whole; otherwise why it did not.
    [[nodiscard]] std::optional<StatsReadingFault> ReadConsensus(ByteCursor& in, ConsensusStatus& status)
    {
        auto& [members, knownLeader, term, commitIndex, role] = status;
        std::uint32_t count = 0;
        if (!in.ReadCount(count, MinMemberBytes))
            return StatsReadingFault::Truncated;
        // No `reserve(count)`: the count is bounded by the bytes present, not by anything this
        // side owns, and a member is larger in memory than its four-byte minimum on the wire.
        for ([[maybe_unused]] auto const member: std::views::iota(std::uint32_t { 0 }, count))
        {
            if (!in.ReadField(members.emplace_back()))
                return StatsReadingFault::Truncated;
        }
        auto leaderPresent = std::uint8_t { 0 };
        if (!in.ReadU8(leaderPresent))
            return StatsReadingFault::Truncated;
        if (leaderPresent > 1)
            return StatsReadingFault::Malformed;
        if (leaderPresent == 1 && !in.ReadField(knownLeader.emplace()))
            return StatsReadingFault::Truncated;
        auto roleByte = std::uint8_t { 0 };
        if (!in.ReadU64(term.value) || !in.ReadU64(commitIndex.value) || !in.ReadU8(roleByte))
            return StatsReadingFault::Truncated;
        if (roleByte >= static_cast<std::uint8_t>(Consensus::Role::Last))
            return StatsReadingFault::Malformed;
        role = static_cast<Consensus::Role>(roleByte);
        return std::nullopt;
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

    // Structured bindings here too, so a field added to `StatsReading` stops this compiling until it has a
    // place in the grammar and a name in `ReadingFieldNames`.
    auto const& [counters, snapshot, version] = reading;

    // By position, which is this grammar's whole design: the digest above is what makes a position mean
    // the same row at both ends.
    auto const cells = counters.Positional();
    out.Bitmap(cells.size(), [&](std::size_t i) { return cells[i].has_value(); });
    for (auto const& value: cells)
        if (value.has_value())
            out.U64(*value);

    // Structured bindings, so a field added to either struct stops this compiling until it is
    // given a place in the grammar and a name in `SnapshotFieldNames` / `ConsensusFieldNames`.
    auto const& [storage, storageTiers, host, hostLoad, upstreamConfigured, consensus, uptime] = snapshot;

    out.U8(static_cast<std::uint8_t>((storage.has_value() ? StoragePresent : 0U) | (host.has_value() ? HostPresent : 0U)
                                     | (upstreamConfigured.has_value() ? UpstreamPresent : 0U)
                                     | (consensus.has_value() ? ConsensusPresent : 0U)
                                     | (hostLoad.has_value() ? HostLoadPresent : 0U)));

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

    if (hostLoad.has_value())
    {
        auto const& [cpu, availableMemoryBytes] = *hostLoad;
        out.U8(static_cast<std::uint8_t>((cpu.has_value() ? CpuPresent : 0U)
                                         | (availableMemoryBytes.has_value() ? AvailableMemoryPresent : 0U)));
        if (cpu.has_value())
        {
            auto const& [busy, total] = *cpu;
            out.U64(busy);
            out.U64(total);
        }
        if (availableMemoryBytes.has_value())
            out.U64(*availableMemoryBytes);
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
    out.Text(version);
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

    auto const cells = reading.counters.Positional();
    auto const counterBits = ReadBitmap(in, cells.size());
    if (!counterBits.has_value())
        return in.Ok() ? malformed : truncated;
    for (auto const i: std::views::iota(std::size_t { 0 }, cells.size()))
    {
        if (!(*counterBits)[i])
            continue;
        std::uint64_t value = 0;
        if (!in.ReadU64(value))
            return truncated;
        cells[i] = value;
    }

    auto& [storage, storageTiers, host, hostLoad, upstreamConfigured, consensus, uptime] = reading.snapshot;

    auto presence = std::uint8_t { 0 };
    if (!in.ReadU8(presence))
        return truncated;
    if ((presence & ~KnownPresenceBits) != 0)
        return malformed;

    if ((presence & StoragePresent) != 0 && !ReadStorage(in, storage.emplace()))
        return truncated;

    auto const tierBits = ReadBitmap(in, storageTiers.size());
    if (!tierBits.has_value())
        return in.Ok() ? malformed : truncated;
    for (auto const i: std::views::iota(std::size_t { 0 }, storageTiers.size()))
    {
        if (!(*tierBits)[i])
            continue;
        if (!ReadStorage(in, storageTiers[i].emplace()))
            return truncated;
    }

    if ((presence & HostPresent) != 0 && !ReadHost(in, host.emplace()))
        return truncated;

    if ((presence & HostLoadPresent) != 0)
    {
        auto& [cpu, availableMemoryBytes] = hostLoad.emplace();
        auto loadPresence = std::uint8_t { 0 };
        if (!in.ReadU8(loadPresence))
            return truncated;
        if ((loadPresence & ~KnownHostLoadBits) != 0)
            return malformed;
        if ((loadPresence & CpuPresent) != 0)
        {
            auto& [busy, total] = cpu.emplace();
            if (!in.ReadU64(busy) || !in.ReadU64(total))
                return truncated;
        }
        if ((loadPresence & AvailableMemoryPresent) != 0 && !in.ReadU64(availableMemoryBytes.emplace()))
            return truncated;
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
        if (auto const fault = ReadConsensus(in, consensus.emplace()); fault.has_value())
            return std::unexpected { *fault };
    }

    std::uint64_t seconds = 0;
    if (!in.ReadU64(seconds))
        return truncated;
    uptime = Uptime { std::chrono::seconds { static_cast<std::chrono::seconds::rep>(seconds) } };

    if (!in.ReadField(reading.version))
        return truncated;

    if (!in.AtEnd())
        return std::unexpected { StatsReadingFault::TrailingBytes };
    return reading;
}

} // namespace FastCache
