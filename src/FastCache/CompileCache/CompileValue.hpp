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
/// **Generation 2** retired generation 1 at
/// [#547](https://github.com/LASTRADA-Software/fastcached/issues/547), the first
/// real customer of the machinery above. A bare root — `/`, `C:\` — is its own
/// trailing separator, and neither side of the transformation knew it: the producer
/// matched nothing, so a value stored by a build rooted at the filesystem root kept
/// that machine's absolute paths, and the consumer emitted `//inc/a.hpp` on POSIX
/// and a UNC-shaped path on Windows.
///
/// **What the bump retires, stated positively rather than as a rule that did not
/// apply.** Moving this byte retires stored objects AND direct-mode manifests, on
/// its own: a manifest is stored wrapped in a `CompileValue`, so the launcher
/// decodes it through `DecodeCompileValue` and a build at generation 2 meeting a
/// generation-1 envelope gets nothing back and misses. `manifest-v*` therefore buys
/// no additional retirement here and would spend a second invalidation event to do
/// it — and the standing lock-step that would have demanded one runs `objkey-v*` to
/// `manifest-v*`, which is a different pair and one-way. The producer half re-keys
/// by itself for the reason the `objkey-v4` bullet gives: the key changes for
/// exactly the translation units whose canonicalization changed, so a stale entry
/// becomes unreachable rather than servable under rules it was not written by. The
/// consumer half is what genuinely needs this byte, since there the key is unchanged
/// and only the localization moved.
inline constexpr std::uint8_t CompileValueVersion = 2;

/// The highest leading byte that will ever name a compile-value generation.
///
/// **A contract on the byte space, and it is what makes an unimplemented FRAMING
/// distinguishable from bytes that are not a compile value at all** (#552).
///
/// #483 refuses a value from a generation this build does not implement instead of
/// storing it verbatim -- but only when the frame behind the leading byte still
/// HOLDS TOGETHER, because that is the only positive evidence a build has that it is
/// looking at a stored value. `CompileValueVersion` names the framing as well as the
/// canonicalization spec, and nothing couples it to an `objkey-v*` bump, so "a future
/// generation moved the framing too" is an ordinary future event. Such a value parses
/// as junk, and a node's cache tier stores what it cannot decode VERBATIM -- so the
/// value's producing checkout's absolute paths land under a key every machine
/// computes, which is #229 arriving through the door #483 closed.
///
/// A bounded reserved range breaks the tie without guessing. A leading byte inside it
/// is a compile value of SOME generation whether or not this build can parse the rest;
/// a leading byte outside it is not a compile value, and the tier's verbatim policy --
/// which this layer has no business overturning -- still applies.
///
/// **What it costs, stated rather than discovered.** An opaque value whose first byte
/// falls in `[1, 15]` is now refused where it used to be stored. That is deliberate
/// and it is the safe direction: a wrong refusal costs a miss and a recompile, while a
/// wrong verbatim store is a silent wrong answer served under a correct-looking key --
/// the asymmetry `AGENT.md` states for caching generally. The range is kept small
/// because the opposite mistake has already shipped once: reading "not our leading
/// byte" as evidence of another generation refused EVERY opaque value, since almost
/// none begins with `0x01`.
///
/// Measured, so the cost is a fact and not a hope. No object-file format this cache
/// stores begins in the reserved range: ELF `0x7F`, PE/COFF `0x4D`, COFF x86-64
/// `0x64`, Mach-O `0xCE`/`0xCF`/`0xFE`, `ar` `0x21`. Nor does the opaque value the
/// node tier's own verbatim test uses, which begins `'n'` (`0x6E`).
///
/// 15 rather than a larger bound because generations are allocated sequentially by
/// this project and thirteen unused numbers is more headroom than the format has
/// consumed in its whole life. The `static_assert` below is what stops a future bump
/// leaving the range silently: growing past it is a decision about the byte space,
/// not a version increment.
inline constexpr std::uint8_t MaxCompileValueGeneration = 15;

static_assert(CompileValueVersion >= 1 && CompileValueVersion <= MaxCompileValueGeneration,
              "a compile-value generation must sit inside the reserved leading-byte range, or a build one "
              "generation older cannot tell this value from an opaque blob and will store it verbatim (#552)");

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

/// Whether @p error is `DecodeCompileValue`'s way of saying "this value was written
/// under a generation this build does not implement".
///
/// One predicate rather than an `== ProtocolErrorCode::UnsupportedFeature` at each
/// reader, because there are three of them — `CanonicalStoredValue`, the launcher's
/// `--show-stats` reason, and the tests — and a rule spelled three times is a rule
/// two of them can drift from.
///
/// **Only for an error `DecodeCompileValue` produced**, and that is a real
/// precondition rather than a formality: `ProtocolErrorCode` is shared, and
/// `UnsupportedFeature` already means a WIRE version elsewhere on this protocol, so
/// handed any other producer's error this answers confidently and wrongly. Making
/// that impossible rather than merely stated wants a decoder-local error type, which
/// is a wider change than the one this arrived in.
///
/// @param error What `DecodeCompileValue` refused with — no other producer's.
/// @return True for a foreign generation; false for damaged or mis-framed bytes.
[[nodiscard]] bool IsForeignGeneration(ProtocolError const& error) noexcept;

/// The sentence a server sends an operator about a value it cannot canonicalize.
///
/// One wording, beside the code that decides the fact, because there are three
/// places that state it — the decoder's own `context`, and each of the two servers'
/// refusal replies — and three spellings of one fact is what `IsForeignGeneration`
/// was introduced to stop happening to the classification a level up. An operator
/// meeting this on the daemon and on a node during one rolling upgrade should not
/// have to work out whether the two are the same event.
///
/// @param generation The generation the producer wrote under.
/// @return The sentence, naming that generation and this build's.
[[nodiscard]] std::string ForeignGenerationMessage(std::uint8_t generation);

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
    /// **Positive evidence, not the absence of ours.** A leading byte that is not
    /// this build's is on its own no evidence at all, and reading it that way was a
    /// defect rather than a conservative choice: almost no opaque blob begins with
    /// `0x01`, so every opaque value would land here and be REFUSED, overturning the
    /// node cache tier's documented policy of storing an opaque value verbatim --
    /// which this layer has no business deciding. Two of that tier's tests said so
    /// out loud. So the rest of the layout is decoded as well, and only a frame that
    /// holds together under it is reported as another generation.
    ///
    /// The residual, since it is real: a future generation that moves the FRAMING as
    /// well as the canonicalization reads as junk here and comes back
    /// `NotACompileValue`. Nothing in this build could separate those -- an unknown
    /// layout is unknown -- and a generation that keeps the framing and moves the
    /// canonicalization, which is the shape #547 will have, is caught exactly.
    ///
    /// **What protects a caller is switching on this, not the empty `bytes`.** The
    /// carried bytes being empty stops a server storing *nothing*; it does not stop
    /// one storing the ORIGINAL, and the node's verbatim fallback was never the
    /// canonical bytes but the STORE's own payload — so `outcome == Canonicalized ?
    /// canonical.bytes : payload` compiles, reads naturally, and reinstates the whole
    /// defect. There is no type that can refuse that; the switch with no `default:`
    /// is what makes the third state impossible to leave out.
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
