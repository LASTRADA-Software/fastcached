// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Endian.hpp>

#include <algorithm>
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
[[nodiscard]] inline bool IsStream(std::uint32_t flags) noexcept
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

    /// Cursor over a byte span with bounds-checked big-endian reads. Each read
    /// returns false and leaves `ok` false once the buffer is exhausted, so the
    /// decoder can bail on a single truncation check at the end.
    struct Reader
    {
        std::span<std::byte const> blob;
        std::size_t offset { 0 };
        bool ok { true };

        /// Bytes still unread. Used to clamp count-driven `reserve()` calls so a
        /// corrupt blob claiming a huge element count cannot trigger a giant
        /// allocation before the per-element reads fail on the missing bytes.
        /// @return Number of remaining bytes (0 once exhausted).
        [[nodiscard]] std::size_t Remaining() const noexcept
        {
            return offset < blob.size() ? blob.size() - offset : 0;
        }

        [[nodiscard]] bool ReadU32(std::uint32_t& out)
        {
            if (!ok || offset + 4 > blob.size())
                return ok = false;
            out = ReadBigEndian<std::uint32_t>(blob.subspan(offset));
            offset += 4;
            return true;
        }

        [[nodiscard]] bool ReadU64(std::uint64_t& out)
        {
            if (!ok || offset + 8 > blob.size())
                return ok = false;
            out = ReadBigEndian<std::uint64_t>(blob.subspan(offset));
            offset += 8;
            return true;
        }

        [[nodiscard]] bool ReadId(StreamId& out)
        {
            return ReadU64(out.ms) && ReadU64(out.seq);
        }

        [[nodiscard]] bool ReadString(std::string& out)
        {
            std::uint32_t len = 0;
            if (!ReadU32(len))
                return false;
            if (offset + len > blob.size())
                return ok = false;
            out.assign(reinterpret_cast<char const*>(blob.data() + offset), len);
            offset += len;
            return true;
        }
    };

    /// Reserve capacity for `count` elements in `vec`, but never more than `cap`
    /// (the bytes left in the blob — every element costs at least one byte, so a
    /// count exceeding the remaining bytes is necessarily corrupt). This keeps a
    /// truncated/corrupt blob with a huge count field from triggering a multi-GB
    /// allocation; the per-element reads that follow then fail cleanly on the
    /// missing bytes and `Decode` returns false.
    /// @param vec   The vector to reserve into.
    /// @param count The (untrusted) element count read from the blob.
    /// @param cap   Upper bound on the reservation (remaining blob bytes).
    template <typename T>
    inline void BoundedReserve(std::vector<T>& vec, std::uint32_t count, std::size_t cap)
    {
        vec.reserve(std::min<std::size_t>(count, cap));
    }

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
    detail::Reader r { .blob = blob, .offset = 2 };
    if (!r.ReadId(out.lastId) || !r.ReadId(out.maxDeletedId) || !r.ReadU64(out.entriesAdded))
        return false;
    std::uint32_t entryCount = 0;
    if (!r.ReadU32(entryCount))
        return false;
    detail::BoundedReserve(out.entries, entryCount, r.Remaining());
    for (auto i = std::uint32_t { 0 }; i < entryCount; ++i)
    {
        StreamEntry entry;
        if (!r.ReadId(entry.id))
            return false;
        std::uint32_t fieldCount = 0;
        if (!r.ReadU32(fieldCount))
            return false;
        detail::BoundedReserve(entry.fields, fieldCount, r.Remaining());
        for (auto f = std::uint32_t { 0 }; f < fieldCount; ++f)
        {
            std::string name;
            std::string value;
            if (!r.ReadString(name) || !r.ReadString(value))
                return false;
            entry.fields.emplace_back(std::move(name), std::move(value));
        }
        out.entries.push_back(std::move(entry));
    }
    std::uint32_t groupCount = 0;
    if (!r.ReadU32(groupCount))
        return false;
    detail::BoundedReserve(out.groups, groupCount, r.Remaining());
    for (auto g = std::uint32_t { 0 }; g < groupCount; ++g)
    {
        ConsumerGroup group;
        if (!r.ReadString(group.name) || !r.ReadId(group.lastDelivered) || !r.ReadU64(group.entriesRead))
            return false;
        std::uint32_t consumerCount = 0;
        if (!r.ReadU32(consumerCount))
            return false;
        detail::BoundedReserve(group.consumers, consumerCount, r.Remaining());
        for (auto c = std::uint32_t { 0 }; c < consumerCount; ++c)
        {
            std::string consumer;
            if (!r.ReadString(consumer))
                return false;
            group.consumers.push_back(std::move(consumer));
        }
        std::uint32_t pelCount = 0;
        if (!r.ReadU32(pelCount))
            return false;
        detail::BoundedReserve(group.pel, pelCount, r.Remaining());
        for (auto p = std::uint32_t { 0 }; p < pelCount; ++p)
        {
            PendingEntry pending;
            if (!r.ReadId(pending.id) || !r.ReadU64(pending.deliveryTimeMs) || !r.ReadU64(pending.deliveryCount)
                || !r.ReadString(pending.consumer))
                return false;
            group.pel.push_back(std::move(pending));
        }
        out.groups.push_back(std::move(group));
    }
    return r.ok;
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
