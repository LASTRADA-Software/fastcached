// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Errors/ConsensusError.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file RosterCertificate.hpp
/// How a machine that runs no consensus comes to trust a roster (#178).
///
/// A consensus member reads the roster out of the state it applied. A worker has no state:
/// it is handed a roster by whichever scheduler answers it, and that scheduler may be a
/// machine the cluster has since removed -- one that still holds its key and would happily
/// sign a roster keeping itself on it. So a roster a worker adopts is one a strict MAJORITY
/// of the voters it already trusts have endorsed, each with its own identity key, recently.
///
/// ## The rule, in one sentence
///
/// A worker holding roster v adopts v' >= v only if a strict majority of v's voters endorse
/// `[clusterId, v', SHA-256(v'), notAfter]`, unexpired. A roster signed by the leader alone
/// would fail requirement (b): a revoked ex-leader could sign one that keeps itself.
///
/// ## Threat model
///
/// This defends against machines ALREADY revoked in the roster a worker holds, not against a
/// compromised current voter: Raft here tolerates crashes, not Byzantine members. What an
/// endorsement's expiry adds is a bound on how long a worker cut off from the cluster -- or
/// kept talking to an ex-leader that withholds every newer roster -- goes on trusting the
/// voters it last heard of.
namespace FastCache::Cluster
{

/// The label a roster endorsement is signed under.
///
/// Its own label beside the construction it signs, as the Raft handshake's and the lease's
/// are: this is signed by a member's OWN key, never the pre-shared one, so it has no
/// `SigningDomain` row.
inline constexpr std::string_view RosterEndorsementLabel = "fastcache-roster-endorsement-v1";

/// How long one endorsement vouches for a roster: one hour (owner decision 4, #178).
///
/// The longer it is, the longer a worker cut off from its leader keeps compiling -- and the
/// longer it keeps trusting a voter the cluster revoked while it was not listening. The
/// shorter it is, the sooner a leader that is merely slow to re-endorse locks a fleet out.
/// `RosterExpired` and `fastcache_roster_expires_in_seconds` are what make either direction
/// visible.
inline constexpr std::chrono::seconds RosterEndorsementLifetime { std::chrono::hours { 1 } };

/// How often a voter re-endorses a roster that has not changed: every fifteen minutes (owner
/// decision 4). Four endorsements per lifetime, so a leader that misses two in a row still
/// hands out a certified roster.
inline constexpr std::chrono::seconds RosterEndorsementRefresh { std::chrono::minutes { 15 } };

/// One voter's signed statement that a roster is the cluster's (#178).
struct RosterEndorsement
{
    std::string clusterId;                             ///< The fleet, as `--cluster-id` names it.
    std::uint64_t version {};                          ///< `ClusterState::rosterVersion`.
    RosterDigest rosterDigest {};                      ///< SHA-256 of the roster's encoding.
    std::chrono::system_clock::time_point notAfter {}; ///< When this endorsement stops vouching.
    Consensus::NodeId endorser;                        ///< The voter that signed it.
    Ed25519Signature signature {};                     ///< Over `EndorsementMessage`.

    [[nodiscard]] friend bool operator==(RosterEndorsement const&, RosterEndorsement const&) = default;
};

/// What an endorsement's signature is over: its label and every claim but the signature,
/// length-prefixed. One function for the signer and every verifier.
/// @param endorsement The endorsement; its signature is not read.
/// @return The signed message.
[[nodiscard]] std::vector<std::byte> EndorsementMessage(RosterEndorsement const& endorsement);

/// Sign an endorsement.
/// @param claims Everything but the signature, which is ignored.
/// @param sign Signs with the endorser's identity key.
/// @return The endorsement, signed.
[[nodiscard]] RosterEndorsement SignEndorsement(RosterEndorsement claims,
                                                std::function<Ed25519Signature(std::span<std::byte const>)> const& sign);

/// Whether @p endorsement's signature verifies under @p key.
/// @param endorsement The endorsement.
/// @param key The key its endorser is trusted to hold.
/// @return True when it does.
[[nodiscard]] bool VerifyEndorsement(RosterEndorsement const& endorsement, Ed25519PublicKey const& key);

/// Encode one endorsement, as NODE-ANNOUNCE carries it.
/// @param endorsement The endorsement.
/// @return Its bytes.
[[nodiscard]] std::vector<std::byte> EncodeEndorsement(RosterEndorsement const& endorsement);

/// Decode one endorsement.
/// @param bytes Its bytes.
/// @return The endorsement, or why the bytes are not one. Nothing about it is VERIFIED here.
[[nodiscard]] std::expected<RosterEndorsement, ConsensusError> DecodeEndorsement(std::span<std::byte const> bytes);

/// A roster and the endorsements a leader collected for it, as NODE-ANNOUNCE's reply carries it
/// and as a worker keeps it (#178).
///
/// The roster travels as its ENCODING, and that is load-bearing: its digest is taken over the
/// bytes a worker received, before they are decoded, so an endorsement can only ever vouch for
/// bytes that were actually sent.
struct CertifiedRoster
{
    std::string clusterId;                       ///< The fleet.
    std::uint64_t version {};                    ///< Which roster this is.
    std::vector<std::byte> roster;               ///< `EncodeRoster`'s bytes.
    std::vector<RosterEndorsement> endorsements; ///< What each voter said about it.

    [[nodiscard]] friend bool operator==(CertifiedRoster const&, CertifiedRoster const&) = default;
};

/// The layout `EncodeCertifiedRoster` writes, first in every encoding -- including a worker's
/// state directory, where a file a later build wrote is refused by name rather than misread.
inline constexpr std::uint8_t CertifiedRosterFormatVersion = 1;

/// Encode a certified roster.
/// @param certified The roster and its endorsements.
/// @return Its bytes.
[[nodiscard]] std::vector<std::byte> EncodeCertifiedRoster(CertifiedRoster const& certified);

/// Decode a certified roster. Verifies NOTHING: that is `CertifyRoster`'s.
/// @param bytes Its bytes.
/// @return It, or why the bytes are not one.
[[nodiscard]] std::expected<CertifiedRoster, ConsensusError> DecodeCertifiedRoster(std::span<std::byte const> bytes);

/// A roster a machine adopted, with the instant its certification lapses, as it keeps the roster
/// between runs (#178).
///
/// The instant is KEPT rather than recomputed: it was decided by the voters that certified this
/// roster, which are its PREDECESSOR's, and a restart has only this one to hand. Recomputing it
/// from this roster's own voters would answer a different question.
struct PersistedRoster
{
    CertifiedRoster certificate;                             ///< As adopted.
    std::chrono::system_clock::time_point certifiedUntil {}; ///< As computed when it was.

    [[nodiscard]] friend bool operator==(PersistedRoster const&, PersistedRoster const&) = default;
};

/// The layout `EncodePersistedRoster` writes, first. A file a later build wrote is refused by
/// name, never read as this layout.
inline constexpr std::uint8_t PersistedRosterFormatVersion = 1;

/// Encode a kept roster, as a state directory holds it.
/// @param persisted The roster and its lapse.
/// @return Its bytes.
[[nodiscard]] std::vector<std::byte> EncodePersistedRoster(PersistedRoster const& persisted);

/// Decode a kept roster. Verifies nothing: it was verified when it was adopted, by this machine.
/// @param bytes Its bytes.
/// @return It, or why the bytes are not one -- `UnsupportedWireVersion` for another layout.
[[nodiscard]] std::expected<PersistedRoster, ConsensusError> DecodePersistedRoster(std::span<std::byte const> bytes);

/// A machine whose endorsement a worker counts.
struct TrustedEndorser
{
    /// The member id it endorses as, when the worker knows it; nullopt for a trust ANCHOR,
    /// which is a key typed on a command line and names nobody.
    std::optional<Consensus::NodeId> id;
    Ed25519PublicKey key {}; ///< The key its endorsement must verify under.
};

/// Whose endorsements certify the NEXT roster a worker adopts, and how many make a majority.
struct CertifyingVoters
{
    /// The endorsers counted: every voter the trusted roster records a key for and has not
    /// revoked -- or every anchor.
    std::vector<TrustedEndorser> endorsers;

    /// How many votes there are: every VOTER, keyless ones included -- a voter that stated no
    /// key still counts toward the majority it can never help reach, as it counts toward a
    /// Raft quorum. For anchors, the anchors.
    std::size_t voters {};
};

/// The voters of a roster the worker already trusts: its voters with a key that is not
/// revoked, out of all its voters.
/// @param roster The trusted roster.
/// @return Who certifies its successor.
[[nodiscard]] CertifyingVoters VotersOf(Roster const& roster);

/// Trust anchors as certifying voters: each key an endorser of its own, and a majority of them
/// needed.
/// @param anchors The keys typed on `--voter-key`.
/// @return Who certifies a worker's first roster.
[[nodiscard]] CertifyingVoters AnchorVoters(std::span<Ed25519PublicKey const> anchors);

/// Why an offered roster was not adopted.
///
/// **PRIVATE: persisted and transmitted nowhere**; a refusal travels as a counter and a log
/// line, so the enumerator values bind nothing.
enum class RosterRefusal : std::uint8_t
{
    Malformed = 0, ///< Its roster bytes are not a roster this build reads.
    WrongCluster,  ///< It names a fleet other than this worker's.
    Stale,         ///< Older than the roster already held: ordinary, never counted.
    Conflicting,   ///< The version already held, with other contents.
    Uncertified,   ///< A strict majority of the certifying voters did not endorse it.
    Expired,       ///< They did, and too many of those endorsements have lapsed.
    Last,          ///< Not a refusal.
};

/// How one refusal is said.
struct RosterRefusalRow
{
    RosterRefusal refusal; ///< The refusal this row describes.
    std::string_view why;  ///< What an operator reads, completing "refused ...: ".
};

/// One row per `RosterRefusal`, in enumerator order.
inline constexpr EnumTable<RosterRefusal, RosterRefusalRow> RosterRefusalTable { {
    { .refusal = RosterRefusal::Malformed, .why = "its roster is not one this build reads" },
    { .refusal = RosterRefusal::WrongCluster, .why = "it is another fleet's" },
    { .refusal = RosterRefusal::Stale, .why = "it is older than the roster already held" },
    { .refusal = RosterRefusal::Conflicting, .why = "it has the version already held and other contents" },
    { .refusal = RosterRefusal::Uncertified,
      .why = "a strict majority of the voters this worker trusts did not endorse it" },
    { .refusal = RosterRefusal::Expired, .why = "the endorsements that would certify it have lapsed" },
} };
static_assert(RowsInEnumeratorOrder(RosterRefusalTable, &RosterRefusalRow::refusal),
              "RosterRefusalTable must hold one row per RosterRefusal, in enumerator order");

/// @param refusal A refusal.
/// @return What an operator reads about it.
[[nodiscard]] constexpr std::string_view DescribeRosterRefusal(RosterRefusal refusal) noexcept
{
    return RosterRefusalTable[static_cast<std::size_t>(refusal)].why;
}

/// A roster a worker holds and verifies grants against.
struct AdoptedRoster
{
    CertifiedRoster certificate; ///< As received, and as persisted.
    Roster roster;               ///< Decoded.
    RosterDigest digest {};      ///< Of `certificate.roster`.

    /// The instant a majority of its endorsements have lapsed: the latest `notAfter` still
    /// held by a majority of the certifying voters.
    std::chrono::system_clock::time_point certifiedUntil {};
};

/// What the rule needs to decide one offer.
struct CertificationInput
{
    std::string_view clusterId;       ///< This worker's fleet.
    CertifyingVoters const& voters;   ///< Whose endorsements count.
    std::uint64_t minimumVersion {};  ///< The version already held; an offer below it is stale.
    std::optional<RosterDigest> held; ///< The digest already held at `minimumVersion`, when one is.
    std::chrono::system_clock::time_point now;
    std::chrono::seconds slack; ///< How far this clock may trail the endorsers'.
};

/// Decide whether @p offered is a roster this worker adopts.
///
/// **The digest is taken over the bytes received** before they are decoded, and an
/// endorsement counts only when it names this cluster, this version and that digest, verifies
/// under a certifying voter's key, has not lapsed, and claims no `notAfter` further ahead than
/// one lifetime plus the slack, which no honest voter writes. Each voter counts once, however
/// many endorsements it signed. A majority that verified and has LAPSED is `Expired`; anything
/// short of a majority otherwise is `Uncertified`.
/// @param offered What a scheduler handed over.
/// @param input Who may certify it, and what is already held.
/// @return The roster to adopt, or why not.
[[nodiscard]] std::expected<AdoptedRoster, RosterRefusal> CertifyRoster(CertifiedRoster const& offered,
                                                                        CertificationInput const& input);

} // namespace FastCache::Cluster
