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
constexpr std::size_t HeaderSize = 6; // magic(1) + type(1) + count(4).

/// The fewest blob bytes one encoded member can occupy: its length prefix, with an
/// empty member.
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
        total += 4 + m.size();
    std::vector<std::byte> out;
    out.reserve(total);
    out.push_back(Magic);
    out.push_back(TypeSet);
    auto const appendU32 = [&out](std::uint32_t v) {
        std::array<std::byte, 4> bytes {};
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
    std::size_t offset = HeaderSize;

    // The count is a CLAIM about bytes this blob must already carry, checked before
    // anything is sized from it -- `WireFields::DeclaredCountFits`, the guard issue
    // #267 generalised. Unguarded, this reserved straight from the `u32`.
    //
    // These bytes are a CLIENT's, which is the part that is not obvious from here. A
    // set is an ordinary value blob distinguished only by the `flags` word, and the
    // memcached text verbs let a client choose that word: `set k 1584562177 0 6`
    // carrying `FC 01 FF FF FF FF` is a six-byte "set" declaring 0xFFFFFFFF members,
    // and any redis set verb on the same engine then decodes it. That reserved
    // 0xFFFFFFFF `std::string` -- about 137 GB -- for a plain client with no
    // privilege, which is why this is the widest-reaching member of the family.
    if (!WireFields::DeclaredCountFits(count, MinMemberBytes, blob.size() - HeaderSize))
        return false;

    // Reserved from what the BYTES IN HAND could hold rather than from the count
    // alone: a validated count is still an amplifier, because a member is 32 bytes in
    // memory against 4 on the wire. A real set of short members reserves exactly what
    // it needs, and a minimum-size hostile blob is clamped to its own size.
    out.reserve(std::min<std::size_t>(count, (blob.size() - HeaderSize) / sizeof(std::string)));
    for (auto i = std::uint32_t { 0 }; i < count; ++i)
    {
        if (offset + 4 > blob.size())
            return false;
        auto const len = ReadBigEndian<std::uint32_t>(blob.subspan(offset));
        offset += 4;
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
