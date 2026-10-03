// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>
#include <FastCache/Core/Sha256.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Cluster
{

/// A member as the roster carries it: who, where its consensus and `0xFC` ports answer, which seat,
/// which key (#178).
///
/// The `0xFC` endpoint as RECORDED, and none of its bookkeeping (`schedulerEndpointHistory`): what
/// a machine an approval admits needs is where to reach its fleet's voters. So the digest moves when
/// a member announces a move -- which a roster's digest could not afford while voters certified it
/// per digest, and can since the certified roster retired.
struct RosterMember
{
    Consensus::NodeId id;                  ///< Its identity.
    std::string raftEndpoint;              ///< Where its consensus port answers.
    MemberSeat seat { MemberSeat::Voter }; ///< Whether it votes.
    Ed25519PublicKey publicKey {};         ///< The key it proves itself with -- required, as the state's is.

    /// Where its `0xFC` port answers, as the cluster RECORDS it (`ClusterMember::schedulerEndpoint`);
    /// empty when the record states none. What a learner it admits remembers the fleet's voters at,
    /// rather than a guess from their consensus host -- so it moves the roster's digest when a member
    /// announces a move, as anything the record holds does.
    std::string schedulerEndpoint;

    [[nodiscard]] friend bool operator==(RosterMember const&, RosterMember const&) = default;
};

/// Who the cluster says its machines are, projected out of `ClusterState` (#178).
///
/// The one shape a roster has: what an approval hands a joiner, which then applies its fleet's
/// replicated state. Sorted exactly as `ClusterState` sorts -- members and principals by id, revoked
/// keys by id and then key -- so every node projecting the same state produces the same bytes, and
/// the fingerprint a joiner prints is the one the leader's list shows.
struct Roster
{
    std::vector<RosterMember> members;        ///< Every member, voters and learners.
    std::vector<ClusterPrincipal> principals; ///< Machines admitted by key rather than as members.
    std::vector<RevokedKey> revoked;          ///< Keys the cluster will never admit again.

    [[nodiscard]] friend bool operator==(Roster const&, Roster const&) = default;
};

/// The roster @p state holds.
/// @param state The replicated state.
/// @return Its roster.
[[nodiscard]] Roster ProjectRoster(ClusterState const& state);

/// The layout `EncodeRoster` writes, first in every encoding.
///
/// An admission is signed over the roster's digest, and a person compares its fingerprint, so an
/// encoding a build did not write must be refused by name rather than read -- a digest over bytes
/// two builds lay out differently is two fingerprints of one roster.
///
/// **2** since each member carries its recorded `0xFC` endpoint. Bumped on the lane rather than left
/// for the wire's flag day because a roster is PERSISTED: a learner's formation record keeps its
/// approval's (`FormationRecord::fleet`), and a start decoding a version-1 record as this layout would
/// report damage that is not there.
inline constexpr std::uint8_t RosterFormatVersion = 2;

/// Encode @p roster canonically: one encoding per roster, whoever encodes it.
/// @param roster The roster.
/// @return The bytes a digest and a signature are taken over.
[[nodiscard]] std::vector<std::byte> EncodeRoster(Roster const& roster);

/// Decode a roster.
///
/// Refuses another layout version by NAME (`UnsupportedVersion`), and anything else that is not
/// a roster as `MalformedFrame`: a wrong width, a seat or role this build does not know, a
/// revoked entry with no key -- and a member or principal with no key, or the all-zero one,
/// each refused by name.
/// @param bytes The encoding.
/// @return The roster, or why the bytes are not one.
[[nodiscard]] std::expected<Roster, ConsensusError> DecodeRoster(std::span<std::byte const> bytes);

/// A roster's digest: SHA-256 over its encoding.
using RosterDigest = Sha256::Digest;

/// The digest of @p encoded, AS RECEIVED.
///
/// The entry point a verifier uses: it checks the digest of the bytes it was handed before it
/// decodes them, so a signature can only ever vouch for bytes that were actually sent.
/// @param encoded A roster's encoding.
/// @return Its digest.
[[nodiscard]] RosterDigest DigestOfRoster(std::span<std::byte const> encoded);

/// The digest of @p roster's canonical encoding.
/// @param roster The roster.
/// @return Its digest.
[[nodiscard]] RosterDigest DigestOfRoster(Roster const& roster);

/// A digest as an operator compares it: `SHA256:` and 43 characters of unpadded base64url.
///
/// One spelling, shown whole, for the reason a public key is: a prefix of a fingerprint is a
/// comparison that passes for two different rosters.
/// @param digest The digest.
/// @return Its text.
[[nodiscard]] std::string RenderRosterFingerprint(RosterDigest const& digest);

} // namespace FastCache::Cluster
