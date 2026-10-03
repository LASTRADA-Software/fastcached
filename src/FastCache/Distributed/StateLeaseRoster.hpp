// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include <core/platform/Clock.hpp>

namespace FastCache::Distributed
{

/// How long a worker trusts the state it applied without hearing from a leader that state counts.
///
/// **What it bounds**: a worker cut off from its fleet with a voter the fleet has since FORGOTTEN
/// still lists that voter, so it would otherwise honour the voter's fresh grants for as long as the
/// isolation lasts. Past this, every grant is refused (`LeaseRefusalReason::Isolated`).
///
/// An hour, the lifetime the retired certified roster gave a worker's trust, plus
/// `LeaseTokenClockSkewSlack` -- chosen on purpose, and wrong in BOTH directions at some value:
/// - **too short** refuses legitimate work during a partition that cuts only CONSENSUS -- the
///   scheduler and the workers still reach each other while the Raft sessions do not -- so every
///   compile there falls back to local;
/// - **too long** widens the window in which an isolated worker honours a forgotten voter.
///
/// **What it does NOT bound**: a forgotten voter that keeps the worker's session alive and goes on
/// speaking AS a leader its stale configuration counts -- a machine running altered code, since an
/// honest one alone loses its quorum and stops leading. Only reaching the fleet and applying the
/// forget ends that.
inline constexpr std::chrono::minutes LeaderSilenceBound { 65 };

/// What `--node-status` says about the roster a node verifies grants against (#178).
struct RosterSummary
{
    std::uint64_t version {};    ///< `ClusterState::rosterVersion`.
    std::uint32_t voters {};     ///< Members that vote.
    std::uint32_t principals {}; ///< Machines admitted by key.
    std::uint32_t revoked {};    ///< Keys the cluster will never admit again.
    /// The identity keys of the voters that have one, in id order: what `NODE-STATUS` offers as the
    /// keys a `--fleet-id` pin names.
    std::vector<Ed25519PublicKey> voterKeys;
};

/// One consensus pass's reading of who leads and when that leader last spoke, AS THE DRIVER KNOWS
/// IT -- before anything decides whether the configuration this node APPLIED counts that leader.
///
/// Raw on purpose: the driver's configuration is its ACTIVE one, the latest in its log and not
/// necessarily committed, so a member the applied state seats as a learner could promote itself
/// in an uncommitted entry and be a voter there. Whether a reading is contact is the roster's to
/// decide, against the voters it applied -- the same state every grant is checked against.
struct LeaderReading
{
    /// This node leads. Its evidence is CheckQuorum, which deposes a leader whose quorum stops
    /// answering, so leading is contact.
    bool leads { false };

    /// Who it follows, when it knows; nothing while nobody is known to lead.
    std::optional<std::string> leader {};

    /// How long ago that leader last spoke -- the last `AppendEntries` or snapshot accepted from it
    /// -- or zero while this node leads. Nothing when nothing was ever heard.
    ///
    /// **An AGE, never an instant**: the driver stamps contact on its own reactor clock, and the
    /// roster measures silence on the clock IT was given. An instant handed between the two would
    /// be compared across two clocks -- equal in production, both `steady_clock`, and apart in any
    /// rig that hands them different ones -- while a duration means the same on either, so silence
    /// is measured on ONE clock, the roster's.
    std::optional<core::platform::SteadyTimePoint::duration> silentFor {};
};

/// The roster a node verifies grants against: the state its own consensus applied (#178).
///
/// **The one roster there is.** Every serving node runs consensus -- a solitary or pending node
/// its own one-machine cluster, a learner or a voter its fleet's -- so every node holds the
/// replicated state, and a grant is checked against the voters it records. No certificate and no
/// expiry: the state is the cluster's own agreement, and a learner cut off from its fleet for days
/// still knows which machines were voters when it last heard, and catches up through Raft.
///
/// Adopted on every applied change, as `Cluster::RosterKeys` is, and read per grant.
///
/// **Bounded by this node's own consensus**, not by a certificate: a state no leader it counts has
/// refreshed for `LeaderSilenceBound` answers `Isolated`, and every grant is refused until a leader
/// speaks again. The consensus tier reports each pass's reading (`NoteLeaderReading`), and a leader
/// counts only when the voters THIS roster applied include it; the start counts as contact, so a
/// node starting cut off has the bound to find its fleet.
class StateLeaseRoster final: public ILeaseRoster
{
  public:
    /// @param clock The ONE steady clock silence is measured on: the start, and every reading's age
    ///        taken back from it. Must outlive this.
    explicit StateLeaseRoster(core::platform::IClock const& clock);

    /// Take one consensus pass's reading, and record it as contact when it is one: this node leads,
    /// or the leader it follows is a VOTER of the state this roster applied. A leader only the
    /// driver's active configuration counts is not.
    ///
    /// Monotone: an older reading than the one held changes nothing, so two reporters cannot move
    /// the record backwards.
    /// @param reading The pass's reading; its age is taken back from now on this roster's clock.
    void NoteLeaderReading(LeaderReading const& reading);

    /// @return True while no leader the applied configuration counts has spoken for longer than
    ///         `LeaderSilenceBound`.
    [[nodiscard]] bool Isolated() const;

    /// Adopt what the cluster now says.
    /// @param state The replicated state.
    void Adopt(Cluster::ClusterState const& state);

    /// @copydoc ILeaseSignerKeys::KeysOf
    ///
    /// Voters only: a GRANT is signed by the voter that issued it, and a learner never leads.
    [[nodiscard]] LeaseSignerKeys KeysOf(std::string_view signer) const override;

    /// @copydoc ILeaseRoster::Read
    ///
    /// `Absent` until the applied state records some voter's key -- before that no grant could
    /// verify, and saying so names THIS node rather than the grant -- then `Isolated` while no
    /// leader it counts has spoken for `LeaderSilenceBound`, and `Current` otherwise.
    [[nodiscard]] RosterReading Read(std::chrono::system_clock::time_point now) const override;

    /// @copydoc ILeaseRoster::MachineKeysOf
    [[nodiscard]] LeaseSignerKeys MachineKeysOf(std::string_view machine) const override;

    /// @return The roster applied, summarised.
    [[nodiscard]] RosterSummary Summary() const;

    /// @return The roster applied, as a copy: the state's own projection (`Cluster::ProjectRoster`).
    [[nodiscard]] Cluster::Roster Held() const;

    /// Whether the state applied names any voter's key yet (#178).
    ///
    /// Not until the first commit that records one -- a scheduler of one records its OWN key there
    /// -- so between a member's start and that commit, a server it cannot place is not evidence
    /// of anything: there is no voter it could have been.
    /// @return True once some voter's key is known.
    [[nodiscard]] bool HoldsVoterKeys() const;

  private:
    core::platform::IClock const& _clock;
    mutable std::shared_mutex _lock;
    core::platform::SteadyTimePoint _lastLeaderContact;             ///< The latest contact, or the construction.
    std::map<std::string, Ed25519PublicKey, std::less<>> _voters;   ///< Voters with a key, by id.
    std::map<std::string, Ed25519PublicKey, std::less<>> _machines; ///< Every member of either seat with a key, by id.
    std::vector<std::string> _voterIds;                             ///< Every applied voter: who may lead.
    std::vector<Ed25519PublicKey> _revoked;                         ///< Every revoked key.
    RosterSummary _summary;                                         ///< For `--node-status`.
    Cluster::Roster _roster;                                        ///< For `explain-admission`.
};

} // namespace FastCache::Distributed
