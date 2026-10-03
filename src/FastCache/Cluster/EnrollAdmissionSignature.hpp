// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

/// @file EnrollAdmissionSignature.hpp
/// What a node signs when it answers a joiner -- an admission, a refusal or a "not yet" -- and the
/// one check of that signature.
///
/// An admission's roster is PUBLIC: every member's id and key, which every beacon and summary
/// already carries. So no check of its content can tell the fleet's answer from anybody's who
/// answered at the endpoint polled -- a roster listing the proven key and the joiner is a copy of
/// public facts. What binds an answer to the fleet is a signature the joiner can verify, by a key
/// it has itself PROVEN for that fleet, over a nonce it chose for that one request.
///
/// **Every outcome is signed, not only the admission**: a refusal sends a joiner back to solitary
/// for an hour and a "not yet" keeps it from giving a dead join up, so an unsigned one is a lever
/// anybody at the polled endpoint could pull. The outcome is inside what is signed, so one answer's
/// signature is never another's.
namespace FastCache::Cluster
{

// The reply carries the key and the signature as the wire's own fixed-width arrays; they must be
// the very types the signature primitive takes, for `FleetSummarySignature.hpp`'s reason.
static_assert(std::is_same_v<decltype(CompileCacheWire::EnrollReplySignature::publicKey), Ed25519PublicKey>,
              "an ENROLL reply carries exactly one Ed25519 public key");
static_assert(std::is_same_v<decltype(CompileCacheWire::EnrollReplySignature::signature), Ed25519Signature>,
              "an ENROLL reply carries exactly one Ed25519 signature");

/// What an answer states, and what its signature covers.
struct AdmissionClaim
{
    std::span<std::byte const> nonce;        ///< The joiner's nonce, as its request carried it.
    std::string_view joinerId;               ///< The id the joiner asked under.
    Ed25519PublicKey joinerKey {};           ///< The key the joiner asked under.
    std::string_view clusterId;              ///< The cluster that answers.
    CompileCacheWire::EnrollOutcome outcome; ///< What it answered.
    std::span<std::byte const> roster; ///< The roster's bytes, exactly as the reply carries them; empty but for an approval.
};

/// The bytes an answer signs: its label (`IdentityKeyPurpose::EnrollAdmission`), then the joiner's
/// nonce, its id, its key, the answering cluster's id, the outcome byte and a digest of the roster
/// bytes, in this project's length-prefixed field grammar.
///
/// **The joiner's id and key are inside**, so an admission of one machine is no admission of
/// another asking with the same nonce; **the cluster id is inside**, so a key proven for one fleet
/// cannot admit a joiner into another; and the roster is covered by its DIGEST -- the bytes the
/// joiner keeps and the fingerprint an operator compares are taken over the same bytes. One
/// function for both ends, so a signer and a verifier cannot come to spell the list differently.
/// @param claim What the admission states.
/// @return The message, owned.
[[nodiscard]] inline LabelledMessage EnrollAdmissionMessage(AdmissionClaim const& claim)
{
    auto const digest = DigestOfRoster(claim.roster);
    auto const outcome = std::array { static_cast<std::byte>(claim.outcome) };
    return LabelledMessage::Of(IdentityKeyPurpose::EnrollAdmission,
                               { claim.nonce,
                                 WireFields::AsBytes(claim.joinerId),
                                 std::span<std::byte const> { claim.joinerKey },
                                 WireFields::AsBytes(claim.clusterId),
                                 std::span<std::byte const> { outcome },
                                 std::span<std::byte const> { digest } });
}

/// Sign an answer as @p identity.
/// @param identity The answering node's identity key.
/// @param claim What it answers.
/// @return What the reply carries.
[[nodiscard]] inline CompileCacheWire::EnrollReplySignature SignAdmission(Ed25519KeyPair const& identity,
                                                                          AdmissionClaim const& claim)
{
    return CompileCacheWire::EnrollReplySignature { .publicKey = identity.PublicKey(),
                                                    .signature = SignLabelled(identity, EnrollAdmissionMessage(claim)) };
}

/// What a joiner found an admission's signature to be.
///
/// **PRIVATE: persisted and transmitted nowhere** -- a joiner's own verdict, which it logs by the
/// row's words and counts; the ordinals carry no explicit values.
enum class AdmissionSignature : std::uint8_t
{
    Verified, ///< Signed over this request, by a key this node proved for that cluster.
    Unsigned, ///< The answer carries no signature.
    Forged,   ///< A signature that does not verify over what this node asked.
    Unproven, ///< A genuine signature, by a key this node never proved for that cluster.
    Last,     ///< Not a verdict, and has no row: the length of a table keyed by one.
};

/// One row of `AdmissionSignatureTable`.
struct AdmissionSignatureRow
{
    AdmissionSignature verdict; ///< The verdict this row describes.
    std::string_view words;     ///< What a joiner logs about it, after the endpoint that answered.
};

/// What each verdict is called where a joiner reports it.
inline constexpr EnumTable<AdmissionSignature, AdmissionSignatureRow> AdmissionSignatureTable { {
    { .verdict = AdmissionSignature::Verified, .words = "signed by the key this node proved for that fleet" },
    { .verdict = AdmissionSignature::Unsigned,
      .words = "an answer nobody signed; nothing binds it to the fleet this node asked, so it counts as no answer" },
    { .verdict = AdmissionSignature::Forged,
      .words = "an answer whose signature does not verify over this request -- one recorded from another ask, "
               "or rewritten on the way" },
    { .verdict = AdmissionSignature::Unproven, .words = "an answer signed by a key this node never proved for that fleet" },
} };

static_assert(RowsInEnumeratorOrder(AdmissionSignatureTable, &AdmissionSignatureRow::verdict),
              "AdmissionSignatureTable must hold one row per AdmissionSignature, in enumerator order");

/// What @p verdict is called.
/// @param verdict The verdict.
/// @return Its words.
[[nodiscard]] constexpr std::string_view WordsFor(AdmissionSignature verdict) noexcept
{
    return AdmissionSignatureTable[static_cast<std::size_t>(verdict)].words;
}

/// Whether @p signature binds @p claim to the fleet the joiner asked, by @p proven.
///
/// **The signature is checked before the key is asked about**, as every signed claim here is: a
/// signature that does not verify over this request is `Forged` whoever it names, and only a
/// genuine one is then held to the key the joiner proved.
/// @param claim What the joiner asked, and the roster and cluster the admission states.
/// @param signature What the reply carries.
/// @param proven The key this node proved, through a FLEET-SUMMARY answer or a beacon's proof, for
///        `claim.clusterId` at the endpoint it asked.
/// @return The verdict.
[[nodiscard]] inline AdmissionSignature VerifyAdmission(
    AdmissionClaim const& claim,
    std::optional<CompileCacheWire::EnrollReplySignature> const& signature,
    Ed25519PublicKey const& proven)
{
    if (!signature.has_value())
        return AdmissionSignature::Unsigned;
    if (!VerifyLabelled(signature->publicKey, EnrollAdmissionMessage(claim), signature->signature))
        return AdmissionSignature::Forged;
    if (signature->publicKey != proven)
        return AdmissionSignature::Unproven;
    return AdmissionSignature::Verified;
}

} // namespace FastCache::Cluster
