// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>

/// @file EnrollRequestSignature.hpp
/// What a joiner signs when it asks to be admitted, and the one check of that signature.
///
/// An `Enroll` names an id and a key, and both are PUBLIC: a discovery beacon carries the key and
/// the roster names both. Unsigned, any host could poll under a joiner's id and key with an endpoint
/// of its own, and the leader recorded whichever poll came last before the approval -- an endpoint
/// the operator never saw, beside a key that matched. So the request is signed by the key it asks
/// under, over every field it states, and the leader verifies it before it records or refreshes
/// anything: only the key's holder can say where its machine answers.
///
/// **And it is FRESH for the leader**: a refresh signs over the challenge the leader's last `Pending`
/// answer issued for that row (`EnrollRequest::challenge`), which the refresh replaces -- so a
/// request recorded on the way and replayed later answers a challenge the row no longer holds, and
/// moves nothing. A first ask carries none; it creates a row, which moves nothing that was there.
namespace FastCache::Cluster
{

// The request carries the signature as the wire's own fixed-width array; it must be the very type
// the signature primitive takes, for `FleetSummarySignature.hpp`'s reason.
static_assert(std::is_same_v<decltype(CompileCacheWire::EnrollView::signature), Ed25519Signature>,
              "an ENROLL request carries exactly one Ed25519 signature");
static_assert(std::is_same_v<decltype(CompileCacheWire::EnrollView::publicKey), Ed25519PublicKey>,
              "an ENROLL request carries exactly one Ed25519 public key");

/// What a joiner states, and what its signature covers: every field of its request but the signature.
struct EnrollRequestClaim
{
    std::string_view nodeId;              ///< The id it asks under.
    std::string_view nodeEndpoint;        ///< Where its `0xFC` port answers; empty for a role that states none.
    CompileCacheWire::EnrollRole role;    ///< What it asks to be admitted as.
    Ed25519PublicKey publicKey {};        ///< The key it asks under, and signs with.
    std::span<std::byte const> nonce;     ///< What it drew for this one request.
    std::span<std::byte const> challenge; ///< The leader's latest challenge it answers; empty on a first ask.
};

/// The bytes a request signs: its label (`IdentityKeyPurpose::EnrollRequest`), then the id, the
/// endpoint, the role byte, the key, the nonce and the leader's challenge it answers (empty on a
/// first ask), in this project's length-prefixed field grammar.
///
/// **Every field the request states is inside**, so no field can be swapped under a signature made
/// for another -- the endpoint above all, which is what the record keeps. One function for both
/// ends, so a signer and a verifier cannot come to spell the list differently.
/// @param claim What the request states.
/// @return The message, owned.
[[nodiscard]] inline LabelledMessage EnrollRequestMessage(EnrollRequestClaim const& claim)
{
    auto const role = std::array { static_cast<std::byte>(claim.role) };
    return LabelledMessage::Of(IdentityKeyPurpose::EnrollRequest,
                               { WireFields::AsBytes(claim.nodeId),
                                 WireFields::AsBytes(claim.nodeEndpoint),
                                 std::span<std::byte const> { role },
                                 std::span<std::byte const> { claim.publicKey },
                                 claim.nonce,
                                 claim.challenge });
}

/// Sign a request as @p identity.
///
/// The claim's key is @p identity's, by construction: a signature under one key over a request
/// naming another verifies under neither, so the key is not the caller's to state.
/// @param identity The joiner's identity key.
/// @param claim What it states; its `publicKey` is ignored and replaced by @p identity's.
/// @return The signature the request carries.
[[nodiscard]] inline Ed25519Signature SignEnrollRequest(Ed25519KeyPair const& identity, EnrollRequestClaim claim)
{
    claim.publicKey = identity.PublicKey();
    return SignLabelled(identity, EnrollRequestMessage(claim));
}

/// Whether @p signature is @p claim's key's, over exactly what @p claim states.
///
/// Answered before anything else about the request is reported on, as every signed claim here is:
/// a request that does not verify is one nobody holding the key made, whatever it names.
/// @param claim What the request states, the key included.
/// @param signature What it carries.
/// @return True when the key's holder signed exactly this request.
[[nodiscard]] inline bool VerifyEnrollRequest(EnrollRequestClaim const& claim, Ed25519Signature const& signature)
{
    return VerifyLabelled(claim.publicKey, EnrollRequestMessage(claim), signature);
}

} // namespace FastCache::Cluster
