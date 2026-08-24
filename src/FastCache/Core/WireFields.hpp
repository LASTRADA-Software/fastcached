// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Endian.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace FastCache::WireFields
{

/// The length-prefixed field grammar every binary protocol here packs its
/// payloads with: a sequence of `[u32 length][length bytes]`.
///
/// ## Why this is a header of its own
///
/// It was written once, inside `Protocol/CompileCacheWire.hpp`, whose own
/// documentation states the reason it should not be written twice: *"a second
/// hand-rolled packer is how a producer and its parser drift"*. That is exactly
/// what a second protocol needing the same grammar would have produced, so the
/// grammar moved here and `CompileCacheWire` now consumes it. The consequence
/// worth knowing is that a change to the packing rules here changes **every**
/// protocol that shares it — which is the point, and is why the encoder and the
/// splitter live side by side rather than in the two files that use them.
///
/// ## Why `Core/` and not `Protocol/Framing/`
///
/// `Protocol/Framing/` is where the *stream* readers live, and they are compiled
/// into the library. This is a pure byte-level transform with no socket, no clock
/// and no state, and it has to be includable from `Protocol/CompileCacheWire.hpp`
/// — which `fastcache-cc` compiles in **without linking `FastCache`**, so an
/// include of anything that needs a translation unit there breaks the launcher's
/// link rather than merely its build. Sitting in `Core/` beside `Endian.hpp` and
/// `Bytes.hpp`, header-only and dependency-free, is what keeps that constraint
/// satisfiable rather than merely stated. Anything added here must keep both
/// properties.
///
/// ## What it deliberately does not do
///
/// It knows nothing about frame headers, magic bytes, opcodes or versions. Those
/// are per-protocol and stay with their protocol; only the payload grammar is
/// shared. A caller frames the result however its own wire says to.
///
/// The module performs no I/O and holds no state, so there is nothing to inject —
/// the same deliberate exception to the project's dependency-injection rule that
/// `CompileCacheWire` documents, for the same reason.

/// Largest payload a single field can carry, since each length prefix is a u32.
///
/// Enforced by the encoder rather than assumed. Casting an over-large size down
/// to `std::uint32_t` would quietly emit a field whose declared length disagrees
/// with its contents, which is precisely the desynchronisation a declared length
/// exists to prevent.
inline constexpr std::uint64_t MaxPayload = 0xFFFFFFFFULL;

/// Bytes a field costs before its contents: its length prefix.
inline constexpr std::size_t FieldPrefixSize = sizeof(std::uint32_t);

// The byte/text reinterpretation is `Core/Bytes.hpp`'s, re-exported so a caller
// working in this grammar has one name for it rather than two spellings of the
// same `reinterpret_cast` in scope at once.
using FastCache::AsBytes;
using FastCache::AsStringView;

/// Render a wire integer as big-endian bytes, sized exactly.
///
/// Returns an owning `std::array` rather than writing into a caller's buffer so
/// the result can be handed straight to `Encode` as a field: the array is a
/// caller-side temporary whose lifetime spans the call, which is the shape a
/// `std::span` field list needs and which costs no allocation.
/// @tparam T The integer type, which fixes the field width.
/// @param value Host-order value.
/// @return The big-endian bytes.
template <WireInteger T>
[[nodiscard]] std::array<std::byte, sizeof(T)> ToBigEndian(T value) noexcept
{
    std::array<std::byte, sizeof(T)> out {};
    WriteBigEndian<T>(out, value);
    return out;
}

/// Decode a field that must hold exactly one big-endian wire integer.
///
/// The width check is the whole point and is why this returns an optional: a
/// field of the wrong size is a malformed frame from a peer, which is a
/// recoverable condition to be answered, never a precondition to assert on.
/// @tparam T The expected integer type.
/// @param field The field's bytes.
/// @return The value, or nullopt when the field is not exactly `sizeof(T)` bytes.
template <WireInteger T>
[[nodiscard]] std::optional<T> FromBigEndian(std::span<std::byte const> field) noexcept
{
    if (field.size() != sizeof(T))
        return std::nullopt;
    return ReadBigEndian<T>(field);
}

/// Write a big-endian wire integer at `offset` within `out`.
/// @tparam T The integer type, which fixes the width written.
/// @param out Destination buffer, already sized to hold it.
/// @param offset Byte offset to write at.
/// @param value Host-order value.
template <WireInteger T>
void PutBigEndian(std::span<std::byte> out, std::size_t offset, T value) noexcept
{
    WriteBigEndian<T>(out.subspan(offset, sizeof(T)), value);
}

/// A field list, which is what every function below takes.
using FieldList = std::span<std::span<std::byte const> const>;

/// View a call-site field list as a `FieldList`.
///
/// `std::initializer_list` has contiguous storage, so this is a view and not a
/// copy — but the storage is a temporary bound to the full expression, so the
/// result must be consumed within it. Exported once rather than left for each
/// caller to hand-roll, because every entry point here takes the span form and
/// the conversion would otherwise be spelled at each of them.
/// @param fields The fields, in wire order.
/// @return A view over them.
[[nodiscard]] inline FieldList AsFields(std::initializer_list<std::span<std::byte const>> fields) noexcept
{
    return FieldList { fields.begin(), fields.size() };
}

/// Total encoded size of `fields`, including every length prefix.
/// @param fields The fields, in wire order.
/// @return The byte count, computed in 64 bits so an overflow is visible.
[[nodiscard]] inline std::uint64_t EncodedSize(FieldList fields) noexcept
{
    auto total = std::uint64_t { 0 };
    for (auto const& field: fields)
        total += FieldPrefixSize + std::uint64_t { field.size() };
    return total;
}

/// Check that `fields` can be described by this grammar, and report the size.
///
/// Every encoder calls this before sizing a buffer, so the ceiling is enforced
/// rather than assumed. Both checks are needed and neither implies the other: a
/// single field larger than the ceiling cannot be described by its own prefix,
/// and on a 32-bit `std::size_t` the running total cannot reach the ceiling to
/// catch it.
/// @param fields The fields, in wire order.
/// @return The exact encoded size.
/// @throws std::length_error When a field, or the total, exceeds the u32 ceiling.
[[nodiscard]] inline std::size_t RequireEncodable(FieldList fields)
{
    auto total = std::uint64_t { 0 };
    for (auto const& field: fields)
    {
        if (std::uint64_t { field.size() } > MaxPayload)
            throw std::length_error("wire field exceeds the u32 field length");
        total += FieldPrefixSize + std::uint64_t { field.size() };
    }

    if (total > MaxPayload)
        throw std::length_error("wire payload exceeds the u32 field length");
    return static_cast<std::size_t>(total);
}

/// Pack `fields` into `out` starting at `offset`, in `[u32 length][bytes]` form.
///
/// Writing into a caller's buffer rather than returning one is what lets a
/// protocol put its own frame header in front **without copying the payload
/// again**. That is not a micro-optimization: a compile-cache STORE frame
/// carries a whole object file, and encoding the payload separately before
/// prepending a header would raise peak footprint from about twice the object to
/// three times it, on the hot path of a parallel build.
///
/// @param out Destination, which must already hold `RequireEncodable(fields)`
///            bytes from `offset` onwards; the caller sizes it.
/// @param offset Where the first length prefix goes.
/// @param fields The fields, in wire order.
inline void EncodeInto(std::span<std::byte> out, std::size_t offset, FieldList fields)
{
    for (auto const& field: fields)
    {
        PutBigEndian<std::uint32_t>(out, offset, static_cast<std::uint32_t>(field.size()));
        offset += FieldPrefixSize;
        std::ranges::copy(field, out.subspan(offset).begin());
        offset += field.size();
    }
}

/// Pack fields into `[u32 length][bytes]` form, with no frame header in front.
///
/// The buffer is sized exactly once and then filled in place. Growing it with
/// `reserve` + `push_back` + `insert` would be the obvious spelling, but the
/// total is known up front, so one allocation and no reallocation is both simpler
/// and faster — and it keeps GCC's `-Wfree-nonheap-object` analysis out of a
/// false positive it reaches when a `reserve`d vector is fed from a stack buffer
/// at -O3.
/// @param fields The fields, in wire order.
/// @return The packed fields.
/// @throws std::length_error When a field, or the total, exceeds the u32 ceiling.
[[nodiscard]] inline std::vector<std::byte> Encode(FieldList fields)
{
    auto const size = RequireEncodable(fields);

    // Returned early rather than falling through to a zero-length `EncodeInto`, and
    // the reason is a diagnostic rather than a saving. A `std::vector` of size zero
    // may hold a **null** `data()`, and GCC at -O3 inlines this whole chain down to
    // the `memcpy` in `WriteBigEndian` and reports `-Wnull-dereference` on it -- a
    // build failure under `-Werror`, and one clang does not produce, so it reaches CI
    // rather than a local gate. It is not a false positive either: `memcpy(nullptr,
    // …, 0)` is undefined behaviour however harmless it looks. Returning here proves
    // to the compiler what the loop below already relied on, which is that there is
    // something to write into.
    if (size == 0)
        return {};

    std::vector<std::byte> out(size);
    EncodeInto(out, 0, fields);
    return out;
}

/// Pack a fixed field list written out at the call site.
/// @param fields The fields, in wire order.
/// @return The packed fields.
/// @throws std::length_error When a field, or the total, exceeds the u32 ceiling.
[[nodiscard]] inline std::vector<std::byte> Encode(std::initializer_list<std::span<std::byte const>> fields)
{
    return Encode(AsFields(fields));
}

namespace Detail
{

    /// Walk up to `limit` fields off the front of `payload`.
    ///
    /// The single field walker both public splitters are built from, so the
    /// bounds arithmetic that decides whether a peer's frame is well-formed has
    /// one author. It stops at `limit` fields **or** at the end of the payload,
    /// whichever comes first, and reports nothing about which — that is the
    /// caller's question and the two ask it differently.
    /// @param payload The bytes to walk.
    /// @param limit Most fields to take.
    /// @return The fields as spans into `payload`, or nullopt when a length
    ///         prefix is short or a length runs past the end.
    [[nodiscard]] inline std::optional<std::vector<std::span<std::byte const>>> SplitUpTo(std::span<std::byte const> payload,
                                                                                          std::size_t limit)
    {
        std::vector<std::span<std::byte const>> fields;
        if (limit != std::numeric_limits<std::size_t>::max())
            // Bounded only when the caller named a count. The unbounded walk
            // deliberately does not reserve from `payload.size()`: a payload of
            // many empty fields would make the reservation itself the cost, so
            // the growth is left to amortize.
            fields.reserve(limit);

        std::size_t offset = 0;
        while (offset != payload.size() && fields.size() < limit)
        {
            if (payload.size() - offset < FieldPrefixSize)
                return std::nullopt;
            auto const length = ReadBigEndian<std::uint32_t>(payload.subspan(offset, FieldPrefixSize));
            offset += FieldPrefixSize;
            if (payload.size() - offset < length)
                return std::nullopt;
            fields.push_back(payload.subspan(offset, length));
            offset += length;
        }
        return fields;
    }

} // namespace Detail

/// Split a payload into exactly `expectedCount` fields.
///
/// A short field, a length running past the end, and any trailing byte after the
/// last field are all rejected. The frame's declared payload length and these
/// per-field lengths are redundant by design, and disagreement between them is a
/// recoverable decode failure for the caller to answer rather than the silent
/// desynchronisation it would be without a declared total.
///
/// Takes the count as a runtime argument rather than a template parameter so a
/// protocol can keep its arity in its own descriptor table instead of spelling it
/// again at every call site.
/// @param payload The bytes following a frame header.
/// @param expectedCount How many fields the payload must hold.
/// @return The fields as spans into `payload`, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::vector<std::span<std::byte const>>> SplitExactly(std::span<std::byte const> payload,
                                                                                         std::size_t expectedCount)
{
    // Bounded at `expectedCount` rather than walked to the end and counted
    // afterwards. The two agree on every input, but an unbounded walk of a
    // hostile payload made entirely of empty fields would materialize a span per
    // four bytes before rejecting it — turning a length check into the
    // allocation it exists to prevent.
    auto fields = Detail::SplitUpTo(payload, expectedCount);
    if (!fields.has_value() || fields->size() != expectedCount)
        return std::nullopt;
    if (EncodedSize(*fields) != payload.size())
        return std::nullopt; // trailing bytes
    return fields;
}

/// Split a payload into however many fields it holds.
///
/// The variable-arity counterpart to `SplitExactly`, for a repeated group whose
/// length is the payload itself — a Raft AppendEntries' entry list is the case it
/// exists for. It is deliberately **not** a relaxation of `SplitExactly`: that
/// one's exact-count and no-trailing-bytes checks are what make a fixed message
/// shape self-describing, and a protocol that wants both nests one inside a field
/// of the other rather than giving up either.
/// @param payload The bytes to split.
/// @return The fields as spans into `payload`, or nullopt when malformed.
[[nodiscard]] inline std::optional<std::vector<std::span<std::byte const>>> SplitAll(std::span<std::byte const> payload)
{
    return Detail::SplitUpTo(payload, std::numeric_limits<std::size_t>::max());
}

} // namespace FastCache::WireFields
