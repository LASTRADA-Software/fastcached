// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/ByteCursor.hpp>
#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/Errors/StorageError.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace FastCache::SetCodec
{

/// Encoding for the redis set value-type.
///
/// A set is stored in an ordinary value blob (so every storage backend, the
/// eviction accounting, and the CoW snapshot path treat it like any other
/// value) and distinguished from a string only by the `FcTypeSet` tag carried
/// in the entry's `flags`. This is the single source of truth for that on-blob
/// layout; nothing else parses it.
///
/// Layout (big-endian fixed-width fields, matching the project's wire
/// convention in Core/Endian.hpp):
///   [u8  magic = 0xFC]
///   [u8  type  = 0x01 (Set)]
///   [u32 count]
///   count × { [u32 len][len bytes] }
/// Members are kept sorted and unique so membership is a binary search and
/// SMEMBERS is deterministic.

/// Entry-flags tag marking a value blob as a set rather than a string.
/// Redis string writes always store flags == 0, so this value is otherwise
/// unused and unambiguously identifies a set entry.
constexpr std::uint32_t FcTypeSet = 0x5E700001U; // "SET" + version nibble.

constexpr std::byte Magic { 0xFC };
constexpr std::byte TypeSet { 0x01 };
/// Magic and type bytes, before the count field.
constexpr std::size_t TagBytes = 2;
constexpr std::size_t HeaderSize = TagBytes + WireFields::FieldPrefixSize; // magic, type, count.

/// The fewest blob bytes one encoded member can occupy: its length prefix, with an
/// empty member.
///
/// **A security bound, not a sizing hint.** It must be a true LOWER bound on what a
/// member costs, because `Decode` refuses any count the remaining bytes cannot supply
/// and an over-estimate would refuse honest data. It will therefore always
/// under-estimate the count for typical members, whose real cost is `4 + len` --
/// that is what makes it correct, not what makes it improvable. Anyone "tightening"
/// it toward a typical member size silently weakens the guard, which is why nothing
/// here sizes an allocation from it.
///
/// Read off `Encode`'s loop below, which is what fixes it, and pinned against that
/// encoder by a test that encodes one empty member and measures the difference -- so a
/// field added there fails a test rather than quietly leaving `Decode`'s guard weaker
/// than the format it guards. Spelled with `FieldPrefixSize` rather than a literal 4,
/// which would restate the framing contract beside the one place it is defined.
constexpr std::size_t MinMemberBytes = WireFields::FieldPrefixSize;

/// @param flags The stored entry's flags word.
/// @return True if the flags tag marks the entry as a set.
[[nodiscard]] constexpr bool IsSet(std::uint32_t flags) noexcept
{
    return flags == FcTypeSet;
}

/// Encode `members` (assumed already sorted & unique) into a set value blob.
/// @param members Sorted, unique member list.
/// @return The encoded blob.
[[nodiscard]] inline std::vector<std::byte> Encode(std::span<std::string const> members)
{
    std::size_t total = HeaderSize;
    for (auto const& m: members)
        total += WireFields::FieldPrefixSize + m.size();
    std::vector<std::byte> out;
    out.reserve(total);
    out.push_back(Magic);
    out.push_back(TypeSet);
    auto const appendU32 = [&out](std::uint32_t v) {
        std::array<std::byte, WireFields::FieldPrefixSize> bytes {};
        WriteBigEndian(std::span<std::byte> { bytes }, v);
        out.insert(out.end(), bytes.begin(), bytes.end());
    };
    appendU32(static_cast<std::uint32_t>(members.size()));
    for (auto const& m: members)
    {
        appendU32(static_cast<std::uint32_t>(m.size()));
        auto const* const p = reinterpret_cast<std::byte const*>(m.data());
        out.insert(out.end(), p, p + m.size());
    }
    return out;
}

/// Decode a set value blob into its sorted member list.
///
/// **Returns the error rather than a `bool`, and that is the whole of #296.** A
/// `bool` cannot carry an outcome with more than two meanings, so every caller chose
/// one -- and three of them chose `StorageErrorCode::Corrupt`, which this project
/// documents as "on-disk record failed CRC32C verification". These bytes are a
/// CLIENT's and the store is healthy, so that told an operator their disk was
/// failing, on demand, from an unprivileged connection. Handing back a code the
/// caller cannot pick is what stops the next caller picking again.
///
/// It is never `Corrupt` from here, and it could not honestly be: integrity is
/// verified BELOW this, by `CowTreeStorage`'s CRC32C and by the compression codec,
/// both of which report `Corrupt` themselves before a byte reaches this function. A
/// failure here is always "these bytes are not a well-formed set".
///
/// @param blob The stored value bytes.
/// @param out  Receives the decoded members (cleared first).
/// @return Nothing on a well-formed blob; `MalformedValue` naming what it refused.
[[nodiscard]] inline std::expected<void, StorageError> Decode(std::span<std::byte const> blob, std::vector<std::string>& out)
{
    /// The refusal, spelled once so every path out carries the same code and a
    /// reason an operator can act on. `Corrupt` used to erase all four reasons.
    /// @param why What the bytes did not honour.
    auto const malformed = [](std::string_view why) {
        return std::unexpected(MakeMalformedValueError(std::string { "set value blob: " } + std::string { why }));
    };

    out.clear();
    if (blob.size() < HeaderSize || blob[0] != Magic || blob[1] != TypeSet)
        return malformed("not a set blob (short, or wrong magic/type tag)");

    // The count is a CLAIM about bytes this blob must already carry, checked before
    // anything is sized from it. `ByteCursor::ReadCount` is where that check lives now
    // and it takes the bound as an argument with no default, so the guard cannot be
    // skipped by a decoder that forgets it exists (#272). Unguarded, this reserved
    // straight from the `u32`.
    //
    // These bytes are a CLIENT's, which is the part that is not obvious from here: a
    // set is an ordinary value distinguished only by its `flags` word, and the
    // memcached text verbs let a client choose that word. The worked attack -- plant
    // six bytes over memcached, read them back with SMEMBERS -- is spelled out and
    // executed by `SetCodec_test.cpp`, which is where it stays correct.
    ByteCursor cursor { blob, TagBytes };
    std::uint32_t count = 0;
    if (!cursor.ReadCount(count, MinMemberBytes))
        return malformed("member count claims more members than the remaining bytes could hold");

    // NOT reserved, and that is deliberate -- but the reason is narrower than "it
    // would be an amplifier", so state it exactly. A blob whose count survives the
    // guard above can still be TRUNCATED: `count` may be `remaining / 4` while the
    // first member's length swallows the rest, so the walk below fails on member one.
    // `reserve(count)` would have committed `count * sizeof(std::string)` -- 8x the
    // blob, since a member is 32 bytes in memory against 4 on the wire -- before
    // discovering that. Growing through `push_back` commits only what the blob
    // actually supplied, which is what a truncated claim deserves.
    //
    // It does NOT bound a WELL-FORMED blob of many empty members: that really does
    // decode to `count` strings and reach the same 8x, with or without a reserve. The
    // ceiling there is the value size the storage tier accepted, not this loop, and
    // capping the member count is a separate decision from validating the claim.
    for (auto i = std::uint32_t { 0 }; i < count; ++i)
    {
        std::string member;
        if (!cursor.ReadField(member))
            return malformed("truncated: a member's length prefix overruns the blob");
        out.push_back(std::move(member));
    }
    return {};
}

/// Re-establish the sorted-unique invariant after mutation.
/// @param members Member list to normalise in place.
inline void Normalise(std::vector<std::string>& members)
{
    std::ranges::sort(members);
    auto const dup = std::ranges::unique(members);
    members.erase(dup.begin(), dup.end());
}

} // namespace FastCache::SetCodec
