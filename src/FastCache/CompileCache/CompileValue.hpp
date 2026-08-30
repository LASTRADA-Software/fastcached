// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Errors/ProtocolError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/// The bytes a server must store for one STORE, canonicalized against the roots
/// the producer sent.
///
/// **What makes a stored value portable, and the one place the whole recipe lives.**
/// A text region is captured compiler output full of the producing machine's
/// absolute paths; a consumer localizes the tokens back to its own roots. A value
/// stored without this step is not one that merely fails to help another checkout —
/// it actively harms it, because `/showIncludes` notes replayed into a build system
/// become that object's recorded dependencies, and dependencies naming another
/// checkout can never be invalidated by an edit in this one.
///
/// **The whole sequence, not the middle of it.** Decode, build the layout from the
/// two root fields, rewrite the regions, re-encode. That recipe used to be spelled
/// out in `Protocol/CompileCacheHandler` and nowhere else, so when a compile node
/// became a second server for this wire
/// ([#229](https://github.com/LASTRADA-Software/fastcached/issues/229)) there was
/// one copy and nothing said the other end lacked it — `grep` answered "one caller",
/// which reads exactly like normal
/// ([#319](https://github.com/LASTRADA-Software/fastcached/issues/319)). Sharing
/// only the rewrite would leave "two callers", which reads the same way: a third
/// server would still have to know the layout comes from those two fields, that the
/// rewrite precedes the encode, and that the encode is what gets stored. A policy
/// every server must apply belongs with the thing it applies to, entire.
///
/// The keys are portable by construction, so at fleet scale one uncanonicalized
/// store is inherited by every machine that computes the same key. That is a
/// property of the design rather than a description of any deployment.
///
/// **Refusing is the caller's business, not this function's.** A value that does not
/// decode comes back as `std::nullopt` and each server answers in its own terms —
/// the daemon with `MalformedValue`, a node by storing the bytes verbatim, because a
/// cache tier deciding what a value may contain is a different policy from this one.
///
/// **Cannot otherwise fail, and says so by returning the bytes.** A path under
/// neither root is echoed verbatim rather than refused, which is what lets a
/// toolchain path survive the round trip — `/showIncludes` names hundreds of SDK
/// headers that are correctly not tokens, so a consumer must never read "no
/// sentinel" as "corrupt". This used to return `bool` and answer a false with wire
/// status `CanonicalizationFailed`, which no server could ever send (issues #59,
/// #69).
///
/// Idempotent: a region already carrying tokens matches neither root and is left
/// alone, so canonicalizing twice is not a second rewrite. A node with an upstream
/// relies on that — it stores canonically and forwards, and the daemon behind it
/// runs the same recipe again.
///
/// @param value     The encoded value exactly as the STORE carried it.
/// @param sourceRoot The producer's source root, from the STORE's own field.
/// @param buildTree  The producer's build tree, from the STORE's own field.
/// @return The bytes to store, or `std::nullopt` when @p value did not decode.
[[nodiscard]] std::optional<std::vector<std::byte>> CanonicalStoredValue(std::span<std::byte const> value,
                                                                         std::string_view sourceRoot,
                                                                         std::string_view buildTree);

} // namespace FastCache
