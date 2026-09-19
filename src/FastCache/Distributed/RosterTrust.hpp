// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/RosterStore.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Distributed
{

/// What `--node-status` says about the roster a node verifies grants against (#178).
struct RosterSummary
{
    std::uint64_t version {};    ///< `ClusterState::rosterVersion`.
    std::uint32_t voters {};     ///< Members that vote.
    std::uint32_t principals {}; ///< Machines admitted by key.
    std::uint32_t revoked {};    ///< Keys the cluster will never admit again.

    /// When the certificate lapses; ABSENT on a consensus member, whose roster is the state it
    /// applied and needs none.
    std::optional<std::chrono::system_clock::time_point> certifiedUntil;
};

/// What one offered roster did to a worker's trust.
///
/// **PRIVATE: persisted and transmitted nowhere.**
enum class RosterOfferOutcome : std::uint8_t
{
    Adopted,   ///< A newer roster, now held.
    Refreshed, ///< The roster already held, re-endorsed: its certification now lasts longer.
    Kept,      ///< Older than the one held, or no newer certification: nothing changed.
    Refused,   ///< Not certified by the voters this worker trusts; counted.
};

/// The roster a worker with no consensus verifies its grants against (#178).
///
/// **The trust root is the ANCHORS until a roster is adopted, and the roster after.** A worker
/// starts holding either a roster it persisted on an earlier run or nothing; with nothing, the
/// first roster it adopts is one a strict majority of its anchors -- the keys typed on
/// `--voter-key` -- endorse. From then on the roster it holds certifies its successor
/// (`Cluster::CertifyRoster`), and the anchors are not consulted again, as the replicated state
/// wins over `--raft-peer`'s typed keys: a revoked ex-leader named on an old command line must
/// not get a say back by being typed.
///
/// Thread-safe: the presence loop offers rosters while every compile reads.
class RosterTrust final: public ILeaseRoster
{
  public:
    /// @param assertedClusterId The fleet `--cluster-id` NAMED, when it named one: a first
    ///        roster naming another is refused. Without one, the fleet is whichever the first
    ///        roster names -- the anchors signed that name, so it is theirs to choose -- and every
    ///        later roster must name the fleet the held one does.
    /// @param anchors The keys typed on `--voter-key`, which certify the first roster.
    /// @param persisted What an earlier run adopted, when this worker keeps one.
    /// @param store Where an adopted roster is kept, or null for a worker with no state
    ///        directory. Borrowed.
    /// @param metrics Where refused rosters are counted. Borrowed.
    /// @param logger Where an adoption and a refusal are said. Borrowed.
    /// @param slack How far this clock may trail the endorsers'.
    RosterTrust(std::optional<std::string> assertedClusterId,
                std::vector<Ed25519PublicKey> anchors,
                std::optional<Cluster::PersistedRoster> persisted,
                IRosterStore* store,
                IMetricsSink& metrics,
                ILogger& logger,
                std::chrono::seconds slack = LeaseTokenClockSkewSlack);

    /// Consider a roster a scheduler handed over.
    /// @param offered What arrived.
    /// @param now This machine's wall clock.
    /// @return What it did.
    RosterOfferOutcome Offer(Cluster::CertifiedRoster const& offered, std::chrono::system_clock::time_point now);

    [[nodiscard]] LeaseSignerKeys KeysOf(std::string_view signer) const override;

    [[nodiscard]] RosterReading Read(std::chrono::system_clock::time_point now) const override;

    /// @return The roster held, summarised, or nothing before the first adoption.
    [[nodiscard]] std::optional<RosterSummary> Summary() const;

  private:
    /// Who certifies the next roster: the held roster's voters, or the anchors before one.
    [[nodiscard]] Cluster::CertifyingVoters CertifyingVotersLocked() const;

    /// Which fleet this worker holds a roster of: the held roster's, else what was asserted,
    /// else nothing yet.
    [[nodiscard]] std::optional<std::string_view> ExpectedClusterLocked() const;

    std::optional<std::string> const _assertedClusterId;
    std::vector<Ed25519PublicKey> const _anchors;
    IRosterStore* const _store;
    IMetricsSink& _metrics;
    ILogger& _logger;
    std::chrono::seconds const _slack;

    mutable std::shared_mutex _lock;
    std::optional<Cluster::AdoptedRoster> _held; ///< Guarded by `_lock`.

    /// The last refusal said out loud, so a scheduler serving the same refused roster every
    /// round is said once and counted every time. Guarded by `_lock`.
    std::optional<Cluster::RosterRefusal> _lastSaid;
};

/// The roster a CONSENSUS member verifies grants against: the state it applied (#178).
///
/// No certificate and no expiry: the state is the cluster's own agreement, and a member cut off
/// from its peers still knows which of them were voters when it last heard. Adopted on every
/// applied change, as `Cluster::RosterKeys` is, and read per grant.
class StateLeaseRoster final: public ILeaseRoster
{
  public:
    /// Adopt what the cluster now says.
    /// @param state The replicated state.
    void Adopt(Cluster::ClusterState const& state);

    [[nodiscard]] LeaseSignerKeys KeysOf(std::string_view signer) const override;

    [[nodiscard]] RosterReading Read(std::chrono::system_clock::time_point now) const override;

    /// @return The roster applied, summarised, with no certificate.
    [[nodiscard]] RosterSummary Summary() const;

    /// Whether the state applied names any voter's key yet (#178).
    ///
    /// Not until the first commit that records one -- a scheduler of one records its OWN key there
    /// -- so between a member's start and that commit, a server it cannot place is not evidence
    /// of anything: there is no voter it could have been.
    /// @return True once some voter's key is known.
    [[nodiscard]] bool HoldsVoterKeys() const;

  private:
    mutable std::shared_mutex _lock;
    std::map<std::string, Ed25519PublicKey, std::less<>> _voters; ///< Voters with a key, by id.
    std::vector<Ed25519PublicKey> _revoked;                       ///< Every revoked key.
    RosterSummary _summary;                                       ///< For `--node-status`.
};

} // namespace FastCache::Distributed
