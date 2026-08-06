// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Errors/ProtocolError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace FastCache
{

/// One captured compiler text stream (e.g. stdout carrying `/showIncludes`, or
/// stderr carrying diagnostics), tagged with the grammar that locates path
/// spans within it. The server canonicalizes each region's `bytes`; the object
/// blob is never a region and is never rewritten.
struct TextRegion
{
    PathCanon::Grammar grammar; ///< Grammar identifying path spans in `bytes`.
    std::string bytes;          ///< The captured text (canonical form once stored).
};

/// The structured value of a cached compile result: the untouched object blob
/// plus zero or more tagged text regions. This is the shape the STORE frame
/// carries and the canonical form the FETCH reply returns.
struct CompileValue
{
    std::vector<std::byte> objectBlob;   ///< Binary object file; stored verbatim, never rewritten.
    std::vector<TextRegion> textRegions; ///< Captured text streams, path-canonicalized on store.
};

/// Serialize a CompileValue to its on-the-wire / stored byte form.
///
/// Layout (all integers big-endian, matching the project's wire helpers):
/// `[u8 version=1][u32 objectLen][object bytes][u32 regionCount]`
/// then per region `[u8 grammar][u32 textLen][text bytes]`.
/// @param value The value to encode.
/// @return The encoded bytes.
[[nodiscard]] std::vector<std::byte> EncodeCompileValue(CompileValue const& value);

/// Deserialize a CompileValue previously produced by EncodeCompileValue.
/// Validates every length against the remaining input so a truncated or
/// malformed frame is rejected rather than over-read.
/// @param bytes The encoded input.
/// @return The decoded value, or ProtocolError(MalformedFrame) on any overrun,
///         unknown version, or unknown grammar tag.
[[nodiscard]] std::expected<CompileValue, ProtocolError> DecodeCompileValue(std::span<std::byte const> bytes);

} // namespace FastCache
