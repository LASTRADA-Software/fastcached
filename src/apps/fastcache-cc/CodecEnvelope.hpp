// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// Ceiling on what a codec envelope may declare it expands to.
///
/// 256 MiB, the figure every framed surface in this project caps a request at,
/// because that is what it has to be: an envelope's declared length decides an
/// allocation on the receiving side, so the number bounding it is the one the
/// surface already promised to bound. A preprocessed C++ translation unit runs to
/// a few megabytes, so this is orders of magnitude above any honest payload.
///
/// Deliberately NOT `DefaultMaxStoreBytes`, which happens to hold the same number:
/// that one is a *client store policy* mirroring the daemon's `--storage-max-value`
/// and is meant to be retuned, and binding a memory-safety ceiling to it would make
/// lowering a store policy silently lower a denial-of-service guard.
///
/// It is a **default**, not the rule: `Unenvelope` takes the ceiling as an argument
/// so each surface passes its own.
// `ULL`, not `UL`: `unsigned long` is 32 bits on Win64 (LLP64), so a future edit
// raising this past 4 GiB would silently wrap there and nowhere else.
inline constexpr std::size_t DefaultMaxDecompressedBytes = 256ULL * 1024ULL * 1024ULL;

/// Why an enveloped payload could not be opened.
///
/// Distinct reasons rather than one "no", because they are different facts about
/// the peer and call for different answers: a codec this build lacks is a
/// configuration difference between two honest processes, while a declared
/// expansion above the cap is a peer trying to make this process allocate.
enum class EnvelopeError : std::uint8_t
{
    /// The envelope framing is not decodable, or an Identity payload's declared
    /// length disagrees with the bytes beside it.
    Malformed,
    /// The codec id is one this build cannot decode.
    UnsupportedCodec,
    /// The declared decompressed length exceeds the caller's ceiling. Answered
    /// **before** a byte is decompressed, which is the whole point of the field.
    DeclaredTooLarge,
    /// The payload did not expand to exactly the declared length.
    Corrupt,
    Last, ///< Not a reason, and has no row: the descriptor table's length.
};

/// What this refusal is called on the wire.
///
/// Paired with the text below in one table row rather than answered by a second
/// `switch`, for the reason `WorkerProtocol`'s own `RefusalDescriptor` records: a
/// refusal reported under one code while being described as another is worse than
/// not describing it at all. A ternary here once answered `UnsupportedCodec` for a
/// malformed frame while the message said "malformed", which sends an operator
/// hunting a codec mismatch that never happened.
/// @param error The reason.
/// @return The wire code a peer should be told.
[[nodiscard]] CompileCacheWire::ErrorCode WireCodeFor(EnvelopeError error) noexcept;

/// A human-readable reason, for a refusal a person has to act on.
/// @param error The reason.
/// @return Its description.
[[nodiscard]] std::string_view DescribeEnvelopeError(EnvelopeError error) noexcept;

/// The codec ids this build can actually produce and consume, most-preferred first.
///
/// Derived from `Core/Compression`, so a build configured without compression offers
/// only `Identity` and still interoperates -- the negotiation falls back to it rather
/// than refusing, because a build must never lose distribution because two machines
/// were configured differently.
///
/// **Here rather than private to one caller, for the reason `Unenvelope` is.** Both
/// halves of this protocol need the same answer: a client states this in every
/// request, and a worker answers from it. While it was the client's alone the node
/// constructed its worker and its registrar with a hard-coded `{ IdentityCodec }`,
/// and every dispatched object crossed the network uncompressed (#265).
/// @return The ids, most-preferred first, always ending in `IdentityCodec`.
[[nodiscard]] CompileCacheWire::CodecList AvailableCodecs();

/// Wrap a payload in a codec envelope, compressing when it is worth it.
///
/// The encoding half of `Unenvelope`, and one function for the same reason: the two
/// directions of this protocol are one negotiation -- every exchange is
/// client-initiated, so the request states what its sender can decode and the answer
/// picks from that -- and a second implementation of the choice is how the two ends
/// come to disagree. The worker's reply used to be that second implementation, and it
/// chose from its OWN list on both sides, discarded the result and sent `Identity`.
///
/// Falls back to `Identity` whenever compression did not actually shrink the payload,
/// which is the same shrink-check `Core/Compression` applies to stored values: an
/// incompressible object should not pay a decompress on the way out. `rawLength` is
/// always the UNCOMPRESSED size, because that is the field `Unenvelope` bounds its
/// allocation by before it decompresses a byte.
///
/// @param payload The bytes to send.
/// @param peerCodecs What the receiving end said it can decode. An **empty** list is
///        ordinary and yields `Identity` — not a defect to be repaired here.
///        `CompileCacheWire`'s `CodecList` states the contract: "Identity is always
///        implicitly acceptable and need not be listed", because a peer that can speak
///        this protocol at all can read uncompressed bytes. A build with compression
///        configured out sends exactly this.
/// @param ownCodecs What this end can produce -- `AvailableCodecs()` for a client,
///        and for a worker the list it registered with the scheduler, which is what
///        the grant relayed to the client in the first place. A list WIDER than the
///        build can honour is not an error: `Compression::IsAvailable` is asked at
///        the point of use, so such an end answers `Identity` rather than in a codec
///        it cannot produce.
/// @return The framed envelope, ready to be used as one length-prefixed field.
[[nodiscard]] std::vector<std::byte> Envelope(std::span<std::byte const> payload,
                                              CompileCacheWire::CodecList const& peerCodecs,
                                              CompileCacheWire::CodecList const& ownCodecs);

/// Undo a codec envelope, refusing an oversized declared expansion first.
///
/// **The one implementation of this, deliberately.** It was two — the worker
/// opening a request and the launcher opening a worker's reply — and both trusted
/// the declared length, so a guard added to either would have been half a fix and
/// the two would have had to agree forever afterwards.
///
/// `Core/Compression.hpp` states the precondition that makes the guard necessary:
/// `originalLen` is "the trusted expected size taken from the record header", and
/// it bounds the output allocation. Off a socket it is neither trusted nor a record
/// header — and `Decompress` **value-initializes** a buffer of that size, so the
/// pages are touched rather than lazily reserved. A `u32` field therefore turns a
/// twenty-one-byte frame into a 4 GiB allocation the surface's byte budget never
/// charged anyone for.
///
/// `Protocol/CompileCacheWire.hpp` already says this is what the field is for:
/// carrying `rawLen` "is what lets a decoder reject a payload whose declared
/// expansion exceeds its cap before decompressing a byte". This implements the
/// guard that comment promised; no decoder in the tree performed it.
///
/// An **Identity** envelope is checked too, and for a reason worth stating: it
/// takes no decompression path at all, so it never reached `Decompress`'s own
/// length check and could declare any length it liked beside any payload. That is
/// not an allocation, but it is a field describing bytes it does not describe, and
/// a receiver that believes it later is the next defect.
///
/// Two entry points rather than one, because the two callers want different
/// containers and neither may pay a copy for the other's. A single public function
/// returning `std::vector<std::byte>` was tried and cost the *text* caller a copy it
/// never used to pay: an `Identity` source builds its `std::string` straight out of
/// the frame, so routing it through a `vector` first is a second full allocation and
/// memcpy of a preprocessed translation unit, on the path a developer's build is
/// waiting on. (That was once *every* source, because a node negotiated no other
/// codec — see `AvailableCodecs` and #265. It is now the compression-less build's
/// path, which is exactly the one least able to afford a spare copy.) A *generic*
/// public function inverts the same argument the other way: `Decompress` already
/// returns a `vector<std::byte>` sized exactly `rawLength`, which `Unenvelope` moves
/// straight through, while a range-constructing generic result would copy that.
///
/// So the ceiling, the framing check and the `Identity` length check — everything a
/// second implementation could disagree about — live once, in one internal helper,
/// and only the container each caller ends up holding differs.
///
/// @param field The enveloped field, exactly as it arrived.
/// @param maxRawBytes The caller's own ceiling on the decompressed size.
/// @return The original bytes, or why they could not be produced.
[[nodiscard]] std::expected<std::vector<std::byte>, EnvelopeError> Unenvelope(std::span<std::byte const> field,
                                                                              std::size_t maxRawBytes);

/// `Unenvelope`, for a payload the caller will read as text.
///
/// Same guard, same refusals; it differs only in copying the payload once into a
/// `std::string` instead of once into a `std::vector<std::byte>`.
/// @param field The enveloped field, exactly as it arrived.
/// @param maxRawBytes The caller's own ceiling on the decompressed size.
/// @return The original text, or why it could not be produced.
[[nodiscard]] std::expected<std::string, EnvelopeError> UnenvelopeText(std::span<std::byte const> field,
                                                                       std::size_t maxRawBytes);

} // namespace FastCache::Cc
