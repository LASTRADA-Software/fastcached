// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/ByteCursor.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::StreamCodec
{

/// Encoding for the redis stream value-type (the `X*` command family).
///
/// Like a set, a stream lives in an ordinary value blob (so every storage
/// backend, the eviction accounting, and the CoW snapshot path treat it like
/// any other value) and is distinguished from a string/set only by the
/// `FcTypeStream` tag carried in the entry's `flags`. This header is the single
/// source of truth for that on-blob layout; nothing else parses it.
///
/// A stream is an append-only log of entries — each with a monotonically
/// increasing `<ms>-<seq>` ID and an ordered list of field/value pairs — plus
/// the consumer-group bookkeeping redis maintains (per-group last-delivered ID
/// and a pending-entries list of unacknowledged deliveries).
///
/// Layout (big-endian fixed-width fields, matching Core/Endian.hpp):
///   [u8  magic = 0xFC]
///   [u8  type  = 0x02 (Stream)]
///   [u64 lastId.ms][u64 lastId.seq]
///   [u64 maxDeletedId.ms][u64 maxDeletedId.seq]
///   [u64 entriesAdded]
///   [u32 entryCount]
///   entryCount × Entry
///   [u32 groupCount]
///   groupCount × Group
/// where:
///   Entry = [u64 id.ms][u64 id.seq][u32 fieldCount]
///           fieldCount × { [u32 len][len bytes] name, [u32 len][len bytes] value }
///   Group = [u32 len][len bytes] name
///           [u64 lastDelivered.ms][u64 lastDelivered.seq]
///           [u64 entriesRead]
///           [u32 consumerCount] consumerCount × { [u32 len][len bytes] name }
///           [u32 pelCount]      pelCount × Pending
///   Pending = [u64 id.ms][u64 id.seq][u64 deliveryTimeMs][u64 deliveryCount]
///             [u32 len][len bytes] consumer
/// Entries are kept sorted by ID (the log is append-only); the PEL is kept
/// sorted by ID so range scans over pending entries are linear.

/// Entry-flags tag marking a value blob as a stream rather than a string/set.
/// Redis string writes always store flags == 0 and sets store `FcTypeSet`
/// (0x5E700001), so this value is otherwise unused and unambiguously
/// identifies a stream entry.
constexpr std::uint32_t FcTypeStream = 0x5E700002U; // "STREAM" + version nibble.

constexpr std::byte Magic { 0xFC };
constexpr std::byte TypeStream { 0x02 };

/// @param flags The stored entry's flags word.
/// @return True if the flags tag marks the entry as a stream.
[[nodiscard]] constexpr bool IsStream(std::uint32_t flags) noexcept
{
    return flags == FcTypeStream;
}

/// A stream entry identifier: a millisecond timestamp and an intra-millisecond
/// sequence number. Renders on the wire as the decimal string `"<ms>-<seq>"`.
struct StreamId
{
    std::uint64_t ms { 0 };  ///< Millisecond component (typically wall-clock ms).
    std::uint64_t seq { 0 }; ///< Sequence within the millisecond.

    /// Total ordering by (ms, seq), as redis compares stream IDs.
    [[nodiscard]] constexpr auto operator<=>(StreamId const&) const noexcept = default;

    /// Render as the canonical `"<ms>-<seq>"` decimal string.
    /// @return The formatted ID.
    [[nodiscard]] std::string Format() const
    {
        return std::format("{}-{}", ms, seq);
    }

    /// The smallest possible ID (`0-0`); also the `-` range sentinel target.
    [[nodiscard]] static constexpr StreamId Min() noexcept
    {
        return StreamId { .ms = 0, .seq = 0 };
    }

    /// The largest possible ID; also the `+` range sentinel target.
    [[nodiscard]] static constexpr StreamId Max() noexcept
    {
        return StreamId { .ms = ~std::uint64_t { 0 }, .seq = ~std::uint64_t { 0 } };
    }

    /// The next ID strictly greater than this one (for exclusive range starts /
    /// XREAD "entries after id"). Saturates at `Max()`.
    /// @return The successor ID.
    [[nodiscard]] constexpr StreamId Next() const noexcept
    {
        if (seq != ~std::uint64_t { 0 })
            return StreamId { .ms = ms, .seq = seq + 1 };
        if (ms != ~std::uint64_t { 0 })
            return StreamId { .ms = ms + 1, .seq = 0 };
        return Max();
    }
};

/// One logged entry: an ID and its ordered field/value pairs.
struct StreamEntry
{
    StreamId id {};                                             ///< The entry's assigned ID.
    std::vector<std::pair<std::string, std::string>> fields {}; ///< Ordered field/value pairs.
};

/// One pending (delivered-but-unacknowledged) entry in a consumer group's PEL.
struct PendingEntry
{
    StreamId id {};                     ///< The pending entry's ID.
    std::string consumer {};            ///< Consumer that currently owns the entry.
    std::uint64_t deliveryTimeMs { 0 }; ///< Wall-clock ms of the last delivery.
    std::uint64_t deliveryCount { 0 };  ///< How many times the entry has been delivered.
};

/// One consumer group over a stream: its read cursor, members, and PEL.
struct ConsumerGroup
{
    std::string name {};                   ///< Group name.
    StreamId lastDelivered {};             ///< Highest ID handed out via `>` reads.
    std::uint64_t entriesRead { 0 };       ///< Logical count of entries read by the group.
    std::vector<std::string> consumers {}; ///< Known consumer names (sorted, unique).
    std::vector<PendingEntry> pel {};      ///< Pending-entries list, sorted by ID.
};

/// The full decoded stream: its log, ID watermarks, and consumer groups.
struct Stream
{
    std::vector<StreamEntry> entries {};  ///< The append-only log, sorted by ID.
    StreamId lastId {};                   ///< Highest ID ever assigned (XADD watermark).
    StreamId maxDeletedId {};             ///< Highest ID ever deleted (XSETID/XDEL tracking).
    std::uint64_t entriesAdded { 0 };     ///< Total entries ever added (never decremented).
    std::vector<ConsumerGroup> groups {}; ///< Consumer groups, by creation/name.
};

namespace detail
{
    /// Append a host-order u32 as big-endian to `out`.
    inline void AppendU32(std::vector<std::byte>& out, std::uint32_t v)
    {
        std::array<std::byte, 4> bytes {};
        WriteBigEndian(std::span<std::byte> { bytes }, v);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    /// Append a host-order u64 as big-endian to `out`.
    inline void AppendU64(std::vector<std::byte>& out, std::uint64_t v)
    {
        std::array<std::byte, 8> bytes {};
        WriteBigEndian(std::span<std::byte> { bytes }, v);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    /// Append a length-prefixed (u32) byte string to `out`.
    inline void AppendString(std::vector<std::byte>& out, std::string_view s)
    {
        AppendU32(out, static_cast<std::uint32_t>(s.size()));
        auto const* const p = reinterpret_cast<std::byte const*>(s.data());
        out.insert(out.end(), p, p + s.size());
    }

    /// Append a stream ID (two big-endian u64s) to `out`.
    inline void AppendId(std::vector<std::byte>& out, StreamId id)
    {
        AppendU64(out, id.ms);
        AppendU64(out, id.seq);
    }

    /// Read a stream id: two big-endian `u64`s, as `AppendId` writes them.
    /// @param cursor The cursor to read from.
    /// @param out Receives the id.
    /// @return True on success; false leaves the cursor failed.
    [[nodiscard]] inline bool ReadId(ByteCursor& cursor, StreamId& out)
    {
        return cursor.ReadU64(out.ms) && cursor.ReadU64(out.seq);
    }

    /// The fewest blob bytes one element of each counted run can occupy, read off
    /// `Encode` below -- which is what fixes them -- and pinned to it by tests that
    /// encode one empty element and measure the difference.
    ///
    /// **Security bounds, not sizing hints.** Each must be a true LOWER bound on its
    /// element's wire cost, because `ByteCursor::ReadCount` refuses a count the
    /// remaining bytes cannot supply and an over-estimate would refuse honest data.
    /// They therefore under-estimate for typical data -- that is what makes them
    /// correct. Nothing here sizes an allocation from them.
    ///
    /// `BoundedReserve` used to assume ONE byte per element for all five, which made
    /// its bound around twenty times too loose for an entry and nine times for a
    /// group, on top of clamping where it should have refused (issue #269).

    /// The bytes a stream id occupies: `AppendId` writes two `u64`s.
    constexpr std::size_t IdBytes = 2 * sizeof(std::uint64_t);

    /// The bytes a count field occupies. Distinct from `WireFields::FieldPrefixSize`
    /// even though both are four: a count is not a length prefix, and spelling them
    /// apart is what keeps each minimum readable against `Encode`.
    constexpr std::size_t CountBytes = sizeof(std::uint32_t);

    /// A stream entry: its id, then its field count.
    constexpr std::size_t MinEntryBytes = IdBytes + CountBytes;

    /// One field of an entry: two length-prefixed strings, both empty.
    constexpr std::size_t MinFieldBytes = 2 * WireFields::FieldPrefixSize;

    /// A consumer group: an empty name, its last-delivered id, its entries-read
    /// counter, then the consumer and pending-entry counts.
    constexpr std::size_t MinGroupBytes = WireFields::FieldPrefixSize + IdBytes + sizeof(std::uint64_t) + (2 * CountBytes);

    /// A consumer: one length-prefixed name, empty.
    constexpr std::size_t MinConsumerBytes = WireFields::FieldPrefixSize;

    /// A pending entry: its id, delivery time and delivery count, then an empty
    /// consumer name.
    constexpr std::size_t MinPendingBytes = IdBytes + (2 * sizeof(std::uint64_t)) + WireFields::FieldPrefixSize;

} // namespace detail

/// Encode a decoded stream into its value blob.
/// @param stream The stream to serialise (entries assumed sorted by ID).
/// @return The encoded blob.
[[nodiscard]] inline std::vector<std::byte> Encode(Stream const& stream)
{
    std::vector<std::byte> out;
    out.push_back(Magic);
    out.push_back(TypeStream);
    detail::AppendId(out, stream.lastId);
    detail::AppendId(out, stream.maxDeletedId);
    detail::AppendU64(out, stream.entriesAdded);
    detail::AppendU32(out, static_cast<std::uint32_t>(stream.entries.size()));
    for (auto const& entry: stream.entries)
    {
        detail::AppendId(out, entry.id);
        detail::AppendU32(out, static_cast<std::uint32_t>(entry.fields.size()));
        for (auto const& [name, value]: entry.fields)
        {
            detail::AppendString(out, name);
            detail::AppendString(out, value);
        }
    }
    detail::AppendU32(out, static_cast<std::uint32_t>(stream.groups.size()));
    for (auto const& group: stream.groups)
    {
        detail::AppendString(out, group.name);
        detail::AppendId(out, group.lastDelivered);
        detail::AppendU64(out, group.entriesRead);
        detail::AppendU32(out, static_cast<std::uint32_t>(group.consumers.size()));
        for (auto const& consumer: group.consumers)
            detail::AppendString(out, consumer);
        detail::AppendU32(out, static_cast<std::uint32_t>(group.pel.size()));
        for (auto const& pending: group.pel)
        {
            detail::AppendId(out, pending.id);
            detail::AppendU64(out, pending.deliveryTimeMs);
            detail::AppendU64(out, pending.deliveryCount);
            detail::AppendString(out, pending.consumer);
        }
    }
    return out;
}

/// Decode a stream value blob.
/// @param blob The stored value bytes.
/// @param out  Receives the decoded stream (cleared first).
/// @return True on a well-formed blob; false if it is truncated/corrupt.
[[nodiscard]] inline bool Decode(std::span<std::byte const> blob, Stream& out)
{
    out = Stream {};
    if (blob.size() < 2 || blob[0] != Magic || blob[1] != TypeStream)
        return false;
    ByteCursor r { blob, /*offset=*/2 };
    if (!detail::ReadId(r, out.lastId) || !detail::ReadId(r, out.maxDeletedId) || !r.ReadU64(out.entriesAdded))
        return false;
    std::uint32_t entryCount = 0;
    if (!r.ReadCount(entryCount, detail::MinEntryBytes))
        return false;
    for (auto i = std::uint32_t { 0 }; i < entryCount; ++i)
    {
        StreamEntry entry;
        if (!detail::ReadId(r, entry.id))
            return false;
        std::uint32_t fieldCount = 0;
        if (!r.ReadCount(fieldCount, detail::MinFieldBytes))
            return false;
        for (auto f = std::uint32_t { 0 }; f < fieldCount; ++f)
        {
            std::string name;
            std::string value;
            if (!r.ReadField(name) || !r.ReadField(value))
                return false;
            entry.fields.emplace_back(std::move(name), std::move(value));
        }
        out.entries.push_back(std::move(entry));
    }
    std::uint32_t groupCount = 0;
    if (!r.ReadCount(groupCount, detail::MinGroupBytes))
        return false;
    for (auto g = std::uint32_t { 0 }; g < groupCount; ++g)
    {
        ConsumerGroup group;
        if (!r.ReadField(group.name) || !detail::ReadId(r, group.lastDelivered) || !r.ReadU64(group.entriesRead))
            return false;
        std::uint32_t consumerCount = 0;
        if (!r.ReadCount(consumerCount, detail::MinConsumerBytes))
            return false;
        for (auto c = std::uint32_t { 0 }; c < consumerCount; ++c)
        {
            std::string consumer;
            if (!r.ReadField(consumer))
                return false;
            group.consumers.push_back(std::move(consumer));
        }
        std::uint32_t pelCount = 0;
        if (!r.ReadCount(pelCount, detail::MinPendingBytes))
            return false;
        for (auto p = std::uint32_t { 0 }; p < pelCount; ++p)
        {
            PendingEntry pending;
            if (!detail::ReadId(r, pending.id) || !r.ReadU64(pending.deliveryTimeMs) || !r.ReadU64(pending.deliveryCount)
                || !r.ReadField(pending.consumer))
                return false;
            group.pel.push_back(std::move(pending));
        }
        out.groups.push_back(std::move(group));
    }
    return r.Ok();
}

/// Parse an unsigned 64-bit decimal from `text`.
/// @param text The decimal digits.
/// @param out  Receives the parsed value on success.
/// @return True if `text` is wholly a valid unsigned integer.
[[nodiscard]] inline bool ParseU64(std::string_view text, std::uint64_t& out) noexcept
{
    if (text.empty())
        return false;
    auto const* const begin = text.data();
    auto const* const end = text.data() + text.size();
    auto const [ptr, ec] = std::from_chars(begin, end, out);
    return ec == std::errc {} && ptr == end;
}

/// Parse a stream ID in `"<ms>"` or `"<ms>-<seq>"` form.
///
/// A bare `"<ms>"` leaves the sequence unset; callers decide the default
/// (0 for range starts / explicit XADD IDs, max for range ends). The `*`,
/// `-`, `+`, `$`, `>` sentinels are NOT handled here — the protocol layer
/// resolves those before calling, since their meaning is command-specific.
/// @param text       The ID text.
/// @param seqDefault Sequence to use when `text` omits the `-<seq>` part.
/// @return The parsed ID, or nullopt on malformed input.
[[nodiscard]] inline std::optional<StreamId> ParseId(std::string_view text, std::uint64_t seqDefault = 0) noexcept
{
    auto const dash = text.find('-');
    StreamId id {};
    if (dash == std::string_view::npos)
    {
        if (!ParseU64(text, id.ms))
            return std::nullopt;
        id.seq = seqDefault;
        return id;
    }
    if (!ParseU64(text.substr(0, dash), id.ms) || !ParseU64(text.substr(dash + 1), id.seq))
        return std::nullopt;
    return id;
}

} // namespace FastCache::StreamCodec
