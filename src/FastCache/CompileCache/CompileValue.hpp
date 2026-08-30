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

/// Rewrite every path a value's text regions carry into canonical tokens.
///
/// **What makes a stored value portable, and the one place it happens.** A region
/// is captured compiler output full of the producing machine's absolute paths; a
/// consumer localizes those tokens back to its own roots. A value stored without
/// this step is not a value that merely fails to help another checkout — it is one
/// that actively harms it, because `/showIncludes` notes replayed into a build
/// system become that object's recorded dependencies, and dependencies naming
/// another checkout can never be invalidated by an edit in this one.
///
/// **Lives here, beside the value, rather than in a server.** It used to live in
/// `Protocol/CompileCacheHandler` as a file-local helper, on the reasoning that
/// canonicalization is "the shared cache's job". That was true while the only
/// thing serving this wire was `fastcached`. Since a compile node became a cache
/// tier serving the same verbs ([#229](https://github.com/LASTRADA-Software/fastcached/issues/229))
/// there are two servers and there was one copy, so a value stored through a node
/// kept its producer's absolute paths and every consumer replayed them
/// ([#319](https://github.com/LASTRADA-Software/fastcached/issues/319)). A policy
/// every server must apply belongs with the thing it applies to.
///
/// The keys are portable by construction, so at fleet scale one uncanonicalized
/// store is inherited by every machine that computes the same key. That is a
/// property of the design rather than a description of any deployment.
///
/// Idempotent: a region already carrying tokens matches neither root and is left
/// alone, so canonicalizing twice is not a second rewrite.
///
/// **Cannot fail, and says so by returning nothing.** A path under neither root is
/// echoed verbatim rather than refused, which is what lets a toolchain path survive
/// the round trip -- `/showIncludes` names hundreds of SDK headers that are
/// correctly not tokens, so a consumer must never read "no sentinel" as "corrupt".
/// This used to return `bool` and answer a false with wire status
/// `CanonicalizationFailed`, which no server could ever send (issues #59, #69).
///
/// @param value    The decoded value to rewrite in place.
/// @param producer The producing machine's roots, as the STORE reported them.
void CanonicalizeStoredRegions(CompileValue& value, PathCanon::Layout const& producer);

} // namespace FastCache
