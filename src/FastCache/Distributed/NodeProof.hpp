// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterSigning.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Core/Sha256.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <span>
#include <string>
#include <string_view>

namespace FastCache::Distributed
{

/// What a caller on the `0xFC` surface proves when it holds the cluster key.
///
/// ## What this is for
///
/// Node-to-node admission on that surface is decided by the caller's SOURCE ADDRESS --
/// `ClusterMembership::Classify` against the committed endpoints, `--fleet-member` against
/// a local list. An address is a stand-in for *this is one of our nodes*, and it stops
/// being one the moment an address is not stable: a worker that joins over a VPN gets a
/// different one each session, and no literal host match can follow it
/// ([#178](https://github.com/LASTRADA-Software/fastcached/issues/178) item 1,
/// [#1428](https://github.com/LASTRADA-Software/fastcached/issues/1428)).
///
/// Every caller that matters here already holds the cluster key -- #282 refuses a
/// network-facing keyless worker at startup and #1308 refuses keyless consensus -- so it
/// can PROVE membership instead of being inferred from where it dialled from.
///
/// ## What a PSK proof establishes, and what it does NOT
///
/// It establishes *this caller holds the cluster key*. It does **not** establish WHICH
/// holder: under a shared key every holder can mint any id's proof, so the node id below
/// is a LABEL that travels inside the MAC rather than an authenticated identity. Binding
/// it still buys something real -- a captured proof cannot have a different id swapped
/// onto it, and the id is what this server records and logs -- but a reader who takes
/// `provenNodeId` for an identity will build something that is not there.
///
/// The consequence is stated because it decides what this can be used for: removing one
/// key-holding machine still means rotating the key on all the others. Making node
/// REMOVAL meaningful needs per-node identity, which is #178's "credential in the frame"
/// and its own threat model. `Consensus::IRaftPeerCredential` says the same of the Raft
/// wire, in the same words, and for the same reason.
///
/// ## The construction
///
/// One tag, over `[challenge][nodeId]`, in `SigningDomain::NodeProof`. Three properties,
/// none of them incidental:
///
/// - **The challenge is the SERVER's**, freshly drawn per connection, so a proof is not
///   replayable onto a second connection. It is spent whatever the outcome, which is
///   discovery's rule (`DiscoveryService`) and is what stops a caller from retrying
///   against one nonce until something verifies.
/// - **The id is inside the MAC**, so the one this server records is the one the holder
///   claimed rather than whatever a man in the middle substituted.
/// - **The peer's ADDRESS is deliberately not covered.** That is the whole point: an
///   address that changes per session is what this replaces. Covering it would reinvent
///   the failure and also refuse the documented NAT and VPN setups, which #242 already
///   settled for the registration endpoint.
///
/// There is no signed verdict, which is where this departs from the Raft handshake. There
/// both ends must agree about the connection and an unsigned refusal of a key holder is a
/// confident wrong signal. Here the proof is an OPTIONAL upgrade: a caller presenting none
/// is admitted or refused by address exactly as before, so the only audience for a refusal
/// is this server's own counters. `SigningDomain::NodeProof`'s comment carries that
/// argument beside the label it is not paired with.
///
/// ## No seam, and that is not an omission
///
/// `Consensus/` needed `IRaftPeerCredential` because `Cluster/` includes `Consensus/`, so
/// a consensus header reaching back would have made the two directories include each
/// other. `Distributed/` already includes `Cluster/` and nothing goes the other way, so
/// this calls `Cluster::SignFields` directly -- which is the one door the pre-shared key
/// signs through, and `ctest -R psk-signing-seam` is what keeps it the only one.
///
/// This module performs no I/O, holds no state and reads no clock, so there is nothing to
/// inject: the same stated exception `ClusterSigning` and `WireFields` document.

// The wire's two widths against the types that fill them. `CompileCacheWire.hpp` spells its own
// numbers because the launcher compiles it in and it may include nothing from `Core/` beyond
// three leaf headers; this is the one file where both those numbers and these types are visible,
// so it is the only place the two can be held equal.
//
// A BUILD failure rather than a test, because the failure mode is silent on both sides: a nonce
// that grew past the wire field would be truncated to its first 32 bytes, both ends would sign
// different inputs, and the refusal would arrive as `NodeProofRejected` -- a wrong-key diagnosis
// for a version mismatch, which is the confident wrong signal this tree keeps paying for.
static_assert(NonceBytes == CompileCacheWire::NodeChallengeBytes,
              "the challenge field on the wire must be exactly as wide as a Nonce");
static_assert(Sha256::DigestSize == CompileCacheWire::NodeProofTagBytes,
              "the tag field on the wire must be exactly as wide as a SHA-256 digest");

/// The tag a key holder must present for @p nodeId against @p challenge.
///
/// @param key The cluster's pre-shared key. An empty key signs perfectly well and
///        authenticates nothing; refusing one is the caller's, exactly as
///        `Cluster::SignFields` documents.
/// @param challenge The server's nonce for this connection, as it was sent.
/// @param nodeId The id the caller claims. Covered by the tag; not authenticated AS an
///        identity, per this header's second section.
/// @return The expected tag.
[[nodiscard]] inline Sha256::Digest MintNodeProof(std::span<std::byte const> key,
                                                  std::span<std::byte const> challenge,
                                                  std::string_view nodeId)
{
    return Cluster::SignFields(key, Cluster::SigningDomain::NodeProof, { challenge, WireFields::AsBytes(nodeId) });
}

/// Whether @p presented is the proof @p nodeId owes for @p challenge.
///
/// Through `Cluster::VerifyFields`, never by comparing a minted tag: `==` on two digests
/// stops at the first difference, and a caller who can retry reads a tag out of the
/// timing one byte at a time. That reasoning is `VerifyFields`' own and is not restated
/// here beyond naming why this does not mint-and-compare.
///
/// @param key The cluster's pre-shared key.
/// @param challenge The nonce this server sent on this connection.
/// @param nodeId The id the caller claimed, as received.
/// @param presented The tag the caller sent.
/// @return True when it authenticates.
[[nodiscard]] inline bool VerifyNodeProof(std::span<std::byte const> key,
                                          std::span<std::byte const> challenge,
                                          std::string_view nodeId,
                                          Sha256::Digest const& presented)
{
    // The braced overload, not `AsFields`: that one views an initializer_list whose storage
    // is bound to the full expression, and spelling it here would put a lifetime rule at a
    // call site for no gain. `VerifyFields` already has the form that takes the list.
    return Cluster::VerifyFields(
        key, Cluster::SigningDomain::NodeProof, { challenge, WireFields::AsBytes(nodeId) }, presented);
}

} // namespace FastCache::Distributed
