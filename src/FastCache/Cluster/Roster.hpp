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

/// A member as the roster carries it: who, where it is dialled, which seat, which key (#178).
///
/// None of the scheduler endpoint's bookkeeping, which changes every time a member leads: a
/// roster is what a machine that runs no consensus needs to know about who may vouch for whom,
/// and a field that moves on every election would move its digest with it.
struct RosterMember
{
    Consensus::NodeId id;                      ///< Its identity.
    std::string raftEndpoint;                  ///< Where its consensus port answers.
    MemberSeat seat { MemberSeat::Voter };     ///< Whether it votes.
    std::optional<Ed25519PublicKey> publicKey; ///< The key it proves itself with, when one is recorded.

    [[nodiscard]] friend bool operator==(RosterMember const&, RosterMember const&) = default;
};

/// Who the cluster says its machines are, projected out of `ClusterState` (#178).
///
/// The one shape a roster has, whichever surface carries it: enrollment hands a joiner its
/// first one, and NODE-ANNOUNCE's certified roster carries every later one. Sorted exactly as
/// `ClusterState` sorts -- members and principals by id, revoked keys by id and then key -- so
/// every voter projecting the same state produces the same bytes, which is what lets their
/// endorsements of it be compared at all.
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
/// A roster is signed through its digest, so an encoding a build did not write must be refused
/// by name rather than read -- and a digest over bytes two builds lay out differently is two
/// digests of one roster, which is a certification that can never reach a majority.
inline constexpr std::uint8_t RosterFormatVersion = 1;

/// Encode @p roster canonically: one encoding per roster, whoever encodes it.
/// @param roster The roster.
/// @return The bytes a digest and a signature are taken over.
[[nodiscard]] std::vector<std::byte> EncodeRoster(Roster const& roster);

/// Decode a roster.
///
/// Refuses another layout version by NAME (`UnsupportedVersion`), and anything else that is not
/// a roster as `MalformedFrame`: a wrong width, a seat or role this build does not know, a
/// principal or revoked entry with no key.
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
