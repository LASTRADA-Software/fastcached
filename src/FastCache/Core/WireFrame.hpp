// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Endian.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace FastCache::WireFrame
{

/// The fixed frame header every length-declaring binary protocol here starts
/// with: `[magic][version][kind][u32 payloadLength]`, seven bytes, big-endian.
///
/// ## Why this is a header of its own
///
/// The same argument `WireFields` next door makes about the *payload* grammar,
/// applied to the *frame*. `CompileCacheWire` and `RaftWire` each spelled these
/// seven bytes out — the encoder character-identical, the decoder differing only
/// in what it named the third byte, and `IsSupported` identical outright. Sharing
/// the field grammar and then hand-rolling the frame around it leaves the two
/// halves of one format with different authors, and the half left duplicated is
/// the half a reader reaches first.
///
/// What the header does **not** decide is what any of it means. The magic is the
/// caller's, so two protocols on two ports stay distinguishable; the kind byte is
/// returned raw and validated against the caller's own table; and the supported
/// version range is passed in, because the wires version independently and always
/// will. This is the byte layout and nothing above it.
///
/// ## Why `Core/`, and why header-only
///
/// Inherited from `WireFields`, for the same reason stated there: it must be
/// includable from `Protocol/CompileCacheWire.hpp`, which `fastcache-cc` compiles
/// in **without linking `FastCache`** — so anything here that needed a
/// translation unit would break the launcher's *link*, not merely its build.

/// A wire protocol version.
///
/// A plain integer rather than an `enum class`: it is an ordered quantity
/// compared against a supported range, and an enumeration would need a cast at
/// every comparison.
using Version = std::uint8_t;

/// Size of the fixed frame header: magic, version, kind, payload length.
inline constexpr std::size_t HeaderSize = 7;

/// Byte offset of the payload length within the header.
inline constexpr std::size_t LengthOffset = 3;

/// The decoded fixed part of a frame.
///
/// The kind is kept **raw** deliberately: a receiver must be able to step over a
/// frame whose kind this build does not know, and it can only do that if decoding
/// the header succeeded. Validating the kind here would make an unknown one
/// indistinguishable from a lost reader, which is the difference between skipping
/// a frame and closing the connection.
struct Header
{
    Version version {};             ///< Protocol version the sender used.
    std::uint8_t kindRaw {};        ///< Opcode or message type, not yet validated.
    std::uint32_t payloadLength {}; ///< Exact byte count following the header.
};

/// Whether `version` lies within a build's supported range.
///
/// The range is a parameter rather than a constant here because each protocol
/// versions independently — a change to the compile-cache wire must not move the
/// Raft peer wire's floor.
/// @param version What the sender advertised.
/// @param minimum Oldest version this build still decodes.
/// @param current Newest version this build speaks.
/// @return True when within `[minimum, current]`.
[[nodiscard]] constexpr bool IsSupported(Version version, Version minimum, Version current) noexcept
{
    return version >= minimum && version <= current;
}

/// Write the fixed header into the front of `out`.
///
/// @param out The frame buffer; must hold at least `HeaderSize` bytes.
/// @param magic This protocol's first byte.
/// @param version Version to advertise.
/// @param kindRaw The opcode or message type.
/// @param payloadLength Byte count that will follow the header.
inline void PutHeader(
    std::span<std::byte> out, std::byte magic, Version version, std::uint8_t kindRaw, std::uint32_t payloadLength) noexcept
{
    // The precondition, enforced rather than only documented -- which is what
    // `DecodeHeader` below already does for the same one.
    //
    // It is also load-bearing for the BUILD. GCC 14 at -O3, inlining this through
    // `RaftWire::Detail::Frame`, cannot prove the span is non-empty and reports
    // `-Wnull-dereference` on all three stores; clang emits nothing at any level.
    // With `PEDANTIC_COMPILER_WERROR` that is a failed build on one compiler only,
    // which is exactly the shape AGENT.md keeps a local gcc-release gate for.
    // Stating the bound teaches the optimizer what the comment already claimed,
    // so this is a real guard rather than a silencer -- and it never fires: every
    // caller sizes its buffer first.
    if (out.size() < HeaderSize)
        return;

    out[0] = magic;
    out[1] = static_cast<std::byte>(version);
    out[2] = static_cast<std::byte>(kindRaw);
    WireFields::PutBigEndian<std::uint32_t>(out, LengthOffset, payloadLength);
}

/// Decode the fixed header from the front of `bytes`.
///
/// Fails **only** on a short buffer or a wrong magic — the two conditions under
/// which the reader has lost sync and so cannot find where this frame ends. An
/// unsupported version and an unknown kind both decode successfully, because both
/// are recoverable and the caller needs `payloadLength` to step over them and
/// answer.
/// @param bytes At least `HeaderSize` bytes from the front of a frame.
/// @param magic This protocol's expected first byte.
/// @return The header, or nullopt when the buffer is short or the magic is wrong.
[[nodiscard]] inline std::optional<Header> DecodeHeader(std::span<std::byte const> bytes, std::byte magic) noexcept
{
    if (bytes.size() < HeaderSize || bytes[0] != magic)
        return std::nullopt;

    return Header { .version = static_cast<Version>(bytes[1]),
                    .kindRaw = static_cast<std::uint8_t>(bytes[2]),
                    .payloadLength = ReadBigEndian<std::uint32_t>(bytes.subspan(LengthOffset, sizeof(std::uint32_t))) };
}

} // namespace FastCache::WireFrame
