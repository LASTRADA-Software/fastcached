// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Errors/ProtocolError.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// The generation of the stored-value contract this build writes and reads: the
/// first byte of every encoded value, and the one thing a reader learns about a
/// value before it decodes anything else.
///
/// **It is not only a framing version.** Canonical text travels nowhere but inside
/// a CompileValue, so `PathCanon` deliberately carries no version of its own and
/// says so at length — which makes this byte the name of the *canonicalization
/// spec* the value's text regions were rewritten under, as well as of the layout
/// they are framed in. Two servers stamping one number onto text they rewrote by
/// different rules is a wrong value under a correct key, at fleet scale, on a
/// correctly-maintained fleet: a fleet is permanently mid-upgrade
/// ([#173](https://github.com/LASTRADA-Software/fastcached/issues/173)), so
/// "every server on this wire canonicalizes identically" is a property that has to
/// hold ACROSS versions or it does not hold at all
/// ([#483](https://github.com/LASTRADA-Software/fastcached/issues/483)).
///
/// So the coupling is **asserted, not remembered**. `CompileValue_test.cpp` runs a
/// fixed conformance corpus through `CanonicalStoredValue` and
/// `PathCanon::LocalizeRegion`, digests the result, and pins that digest against
/// this number in a generation table. Move the canonicalization behaviour without
/// moving this byte and the test goes red naming the bump; move the byte without a
/// row and it goes red too. Before that table the invariant was stated in
/// `PathCanon.hpp` and enforced by nothing, which is the difference between an
/// invariant and a hope.
inline constexpr std::uint8_t CompileValueVersion = 1;

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
/// `[u8 CompileValueVersion][u32 objectLen][object bytes][u32 regionCount]`
/// then per region `[u8 grammar][u32 textLen][text bytes]`.
/// @param value The value to encode.
/// @return The encoded bytes.
[[nodiscard]] std::vector<std::byte> EncodeCompileValue(CompileValue const& value);

/// Deserialize a CompileValue previously produced by EncodeCompileValue.
/// Validates every length against the remaining input so a truncated or
/// malformed frame is rejected rather than over-read.
/// @param bytes The encoded input.
/// @return The decoded value; `ProtocolError(UnsupportedFeature)` when the leading
///         byte names a generation other than `CompileValueVersion`; or
///         `ProtocolError(MalformedFrame)` on any overrun, unknown grammar tag or
///         trailing byte.
///
/// The two error codes are not interchangeable and the split is the whole point of
/// `CanonicalizationOutcome` below: damaged bytes and a value written by another
/// build are different facts, and a caller that cannot tell them apart applies one
/// policy to both. It is the same distinction the storage rulebook draws between
/// `UnsupportedFormatVersion` and `Corrupt`, for the same reason — one of those two
/// answers is what makes somebody delete a healthy cache.
[[nodiscard]] std::expected<CompileValue, ProtocolError> DecodeCompileValue(std::span<std::byte const> bytes);

/// What `CanonicalStoredValue` found in the bytes a STORE carried, and therefore
/// what the server holding them is allowed to do with them.
///
/// **Three states, because the absent one was two facts and the servers on this
/// wire apply OPPOSITE policies to them.** This used to be a `std::optional`, whose
/// `nullopt` meant "did not decode". The daemon read that as *reject*
/// (`MalformedValue`) and a node's cache tier read it as *store the bytes verbatim*
/// — each defensible for genuinely opaque bytes, and one of them catastrophic for a
/// stored value written by another generation. A launcher at generation N storing
/// into a node at N+1 put the producing checkout's absolute paths into the shared
/// cache under a key every machine computes, which is
/// [#229](https://github.com/LASTRADA-Software/fastcached/issues/229) /
/// [#319](https://github.com/LASTRADA-Software/fastcached/issues/319) reached by
/// nothing worse than a rolling upgrade
/// ([#483](https://github.com/LASTRADA-Software/fastcached/issues/483)).
enum class CanonicalizationOutcome : std::uint8_t
{
    /// Decoded and rewritten. `bytes` is what the server must store.
    Canonicalized,

    /// Not this wire's stored-value format at all — empty, truncated, or framed
    /// wrongly. What to do about it stays the caller's own policy, exactly as it
    /// was: a cache tier deciding what an opaque value may contain is a different
    /// question from this one.
    NotACompileValue,

    /// A stored value whose leading byte names a generation this build does not
    /// implement. **No server may store it, forward it, or serve it**, because it
    /// cannot canonicalize it and an uncanonicalized store is the defect above.
    /// Refusing costs the hits of one upgrade window; accepting costs every
    /// consumer that then replays the producer's paths into its dependency graph,
    /// which no edit in its own checkout can ever invalidate.
    ///
    /// Read off the leading byte, so bytes that are not a stored value at all but
    /// begin with an unrecognised tag land here rather than in `NotACompileValue`.
    /// That misclassification is deliberate and one-directional: the cost is a
    /// refused store of something no client on this wire sends, and the alternative
    /// is guessing a foreign generation's framing well enough to rule it out.
    ForeignGeneration,
};

/// The result of canonicalizing one STORE's bytes: what was found, and the bytes
/// to store when there are any.
struct StoredValueCanonicalization
{
    /// The canonical bytes to store. Non-empty only for `Canonicalized`; the other
    /// two outcomes carry nothing, because there is nothing this build may write.
    std::vector<std::byte> bytes;

    /// What was found. Every caller switches on it with no `default:`, so a fourth
    /// state is a compile error at each rather than a silent fall-through.
    CanonicalizationOutcome outcome { CanonicalizationOutcome::NotACompileValue };

    /// For `ForeignGeneration`, the leading byte that was read — the generation the
    /// producer wrote under. Zero for the other outcomes. It is here so a refusal
    /// can NAME the generation it declined: a mixed-version fleet otherwise presents
    /// as an endlessly cold cache with no diagnostic, which this wire has already
    /// recorded paying for once.
    std::uint8_t generation { 0 };
};

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
/// **The version this holds across is the clause the rule used to lack.** "Every
/// server on this wire" is not a statement about a moment: a fleet is permanently
/// mid-upgrade, so the servers on the wire are at several generations at once and
/// the property has to hold across them or it never holds. Two things carry it, and
/// neither works alone — `CompileValueVersion` names the canonicalization spec and
/// is pinned to the behaviour by a conformance digest, so two builds that rewrite
/// text differently cannot both call themselves generation N; and
/// `ForeignGeneration` below is what a server does when it meets a generation it is
/// not, which is refuse rather than guess
/// ([#483](https://github.com/LASTRADA-Software/fastcached/issues/483)).
///
/// **Refusing is the caller's business for ONE of the three outcomes.** Bytes that
/// are not a stored value at all (`NotACompileValue`) are still each server's own
/// policy — the daemon answers `MalformedValue`, a node stores them verbatim,
/// because a cache tier deciding what an opaque value may contain is a different
/// question from this one. A `ForeignGeneration` value is **not** in that category
/// and no server has a choice about it: storing it uncanonicalized is the defect
/// this whole function exists to prevent, so it carries no bytes to store.
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
/// @return What was found, plus the bytes to store when the outcome is
///         `Canonicalized`.
[[nodiscard]] StoredValueCanonicalization CanonicalStoredValue(std::span<std::byte const> value,
                                                               std::string_view sourceRoot,
                                                               std::string_view buildTree);

} // namespace FastCache
