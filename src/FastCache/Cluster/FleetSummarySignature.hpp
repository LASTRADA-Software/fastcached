// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

/// @file FleetSummarySignature.hpp
/// What a node signs when it answers `FleetSummary`, and the one check of that signature.
///
/// A seed answers strangers -- that is what a seed is for -- so its answer is worth something only
/// as a signature over the ASKER's nonce: a recorded answer replayed to a later asker names a nonce
/// that asker never chose, and verifies nothing.
namespace FastCache::Cluster
{

// The reply carries the key and the signature as the wire's own fixed-width arrays; they must be
// the very types the signature primitive takes, or a width change on one side is a silent
// truncation rather than a build failure.
static_assert(std::is_same_v<decltype(CompileCacheWire::FleetSummaryReply::publicKey), Ed25519PublicKey>,
              "a FleetSummary reply carries exactly one Ed25519 public key");
static_assert(std::is_same_v<decltype(CompileCacheWire::FleetSummaryReply::signature), Ed25519Signature>,
              "a FleetSummary reply carries exactly one Ed25519 signature");

/// The bytes a `FleetSummary` answer signs: its label (`IdentityKeyPurpose::FleetSummary`), the
/// asker's nonce, the whole summary as ONE nested field, and the key, in this project's
/// length-prefixed field grammar.
///
/// **A label of its own**, and the one construction whose first field after it a STRANGER chose:
/// without the label, what separates this signature from every other construction under the same
/// key is that nobody happened to shape a 32-byte field there.
///
/// Every summary field and the KEY are inside the signature, for `DiscoveryWire::ProofMessage`'s
/// reason: the summary is what formation decides on, so a field outside the signature is one a
/// relay can rewrite into a yield, and a key outside it lets a signature be re-attributed. The
/// summary's bytes are `EncodeFleetSummaryFields`', exactly what a beacon carries. One function for
/// both ends, so a signer and a verifier cannot come to spell the list differently.
/// @param nonce The asker's nonce, as the request carried it.
/// @param summary What the answering node says about itself and its fleet.
/// @param key The key the answering node signs with.
/// @return The message, owned.
[[nodiscard]] inline LabelledMessage FleetSummaryMessage(std::span<std::byte const> nonce,
                                                         CompileCacheWire::FleetSummary const& summary,
                                                         Ed25519PublicKey const& key)
{
    auto const fields = CompileCacheWire::EncodeFleetSummaryFields(summary);
    return LabelledMessage::Of(IdentityKeyPurpose::FleetSummary,
                               { nonce, std::span<std::byte const> { fields }, std::span<std::byte const> { key } });
}

/// Whether @p reply is a signature over @p nonce by the key it carries.
///
/// Proof of POSSESSION and nothing more: the holder of that key said this summary in answer to
/// this nonce. Whether the summary is true, or the key anybody's, is not asked here.
/// @param nonce The nonce the asker sent.
/// @param reply What came back.
/// @return True only when the signature verifies; false for an empty cluster id, which no summary
///         may carry and `EncodeFleetSummaryFields` refuses to encode.
[[nodiscard]] inline bool VerifyFleetSummarySignature(std::span<std::byte const> nonce,
                                                      CompileCacheWire::FleetSummaryReply const& reply)
{
    // Before the message is built: its encoder asserts a non-empty cluster id, and a reply is
    // unvalidated wire input whatever decoded it.
    if (reply.summary.clusterId.empty())
        return false;
    return VerifyLabelled(reply.publicKey, FleetSummaryMessage(nonce, reply.summary, reply.publicKey), reply.signature);
}

} // namespace FastCache::Cluster
