// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
constexpr std::size_t HeaderSize = 1 + 1 + WireFields::FieldPrefixSize; // magic, type, count.

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
/// @param blob The stored value bytes.
/// @param out  Receives the decoded members (cleared first).
/// @return True on a well-formed blob; false if it is truncated/corrupt.
[[nodiscard]] inline bool Decode(std::span<std::byte const> blob, std::vector<std::string>& out)
{
    out.clear();
    if (blob.size() < HeaderSize || blob[0] != Magic || blob[1] != TypeSet)
        return false;
    auto const count = ReadBigEndian<std::uint32_t>(blob.subspan(2));
    auto const remaining = blob.size() - HeaderSize;

    // The count is a CLAIM about bytes this blob must already carry, checked before
    // anything is sized from it -- `WireFields::DeclaredCountFits`, the guard issue
    // #267 generalised. Unguarded, this reserved straight from the `u32`.
    //
    // These bytes are a CLIENT's, which is the part that is not obvious from here: a
    // set is an ordinary value distinguished only by its `flags` word, and the
    // memcached text verbs let a client choose that word. The worked attack -- plant
    // six bytes over memcached, read them back with SMEMBERS -- is spelled out and
    // executed by `SetCodec_test.cpp`, which is where it stays correct.
    if (!WireFields::DeclaredCountFits(count, MinMemberBytes, remaining))
        return false;

    // NOT reserved, and that is deliberate. Validating the count and pre-sizing the
    // container are different decisions, and only the first is load-bearing: growth
    // through `emplace_back` is amortised O(1) and moves 32-byte strings, while every
    // way of pre-sizing from this count is either an amplifier or wrong.
    //
    // Reserving the validated count is still 8x -- a member is 32 bytes in memory
    // against 4 on the wire -- which on a 256 MiB value is 2 GiB, the same shape this
    // guard exists to close. Clamping to `remaining / sizeof(std::string)` bounds the
    // memory but under-reserves real sets by 2.7x for eight-byte members, because a
    // member's true wire cost is `4 + len` and only the `4` is knowable up front.
    // A reallocation is not what a critical unbounded-reserve ticket is about.
    std::size_t offset = HeaderSize;
    for (auto i = std::uint32_t { 0 }; i < count; ++i)
    {
        if (offset + WireFields::FieldPrefixSize > blob.size())
            return false;
        auto const len = ReadBigEndian<std::uint32_t>(blob.subspan(offset));
        offset += WireFields::FieldPrefixSize;
        if (offset + len > blob.size())
            return false;
        out.emplace_back(reinterpret_cast<char const*>(blob.data() + offset), len);
        offset += len;
    }
    return true;
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
