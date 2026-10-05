// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/DiscoveryBounds.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Consensus/IRaftPeerKeys.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <core/platform/Clock.hpp>

namespace FastCache::Cluster
{

/// What a directory did with a beacon.
///
/// An enum rather than the `bool` this used to be, because that bool meant four
/// different things and a caller could tell none of them apart -- which is why
/// nothing could ever be said about the one that is worth saying something about.
/// Most of these are ordinary traffic on a shared segment; `Unnameable` is a fault and
/// is reported, and the three that reached a bound (`DiscoveryBounds`) are counted.
///
/// **Private**: never transmitted or persisted.
enum class BeaconOutcome : std::uint8_t
{
    Recorded, ///< A peer of this node's own fleet it can name; it may now be challenged.
    Foreign,  ///< A node of another fleet, recorded in the foreign table; it may now be challenged.
    Self,     ///< This node's own beacon, come back to it on the broadcast.

    /// Names nothing this node could ever record as a member.
    ///
    /// An empty id, or an id or endpoint that is not valid UTF-8. One answer because
    /// it is one rule -- a member is `(id, endpoint)` and both halves have to survive
    /// being written down, replicated, and read back out by `/fleet.json`, the page,
    /// `--cluster-status` and every log line. An EMPTY endpoint is nameable: it is a
    /// learner, which answers nothing and so announces nowhere to dial.
    Unnameable,

    /// Another fleet, while `MaxForeignFleets` others are already remembered and every one of
    /// them has PROVEN its summary: dropped, and never challenged. Holding a slot against a new
    /// fleet costs a real address and an answered challenge per slot.
    ForeignTableFull,

    /// A peer of this node's own fleet the roster holds no key for, recorded by displacing the
    /// oldest such peer: `MaxUnrosteredPeers` were remembered. It may now be challenged.
    RecordedDisplacing,

    /// Another fleet, recorded by displacing the oldest fleet that has proven nothing:
    /// `MaxForeignFleets` were remembered. It may now be challenged.
    ForeignDisplacing,
};

/// A peer this node has heard from, and when.
struct KnownPeer
{
    std::string nodeId;       ///< How membership names it.
    std::string raftEndpoint; ///< Where its Raft peer server answers.

    /// Whether this peer has proved the key the cluster holds for its id.
    ///
    /// Seen and admitted are different facts and are kept apart on purpose. A
    /// beacon is unauthenticated by construction -- anybody on the segment can
    /// send one -- so a directory that recorded "seen" as "trusted" would let a
    /// broadcast alone drive a membership change.
    bool authenticated { false };

    /// The key it proved, exactly when `authenticated` (#178): what a desire built from this peer
    /// carries, so the cluster records the key discovery saw rather than no opinion about one.
    std::optional<Ed25519PublicKey> provenKey;

    /// When its most recent beacon arrived.
    std::chrono::steady_clock::time_point lastSeen {};

    /// Whether the roster held a live key for its id at its most recent beacon.
    ///
    /// Such a peer is bounded by the roster, which only an operator grows, and is never displaced;
    /// every other peer of this cluster shares `MaxUnrosteredPeers`.
    bool rostered { false };
};

/// What this node has heard on the segment, and what it may act on.
///
/// Pure with respect to I/O: time arrives through `core::platform::IClock` and datagrams arrive
/// as already-decoded values, which is what lets every expiry and admission rule
/// be a `core::platform::ManualClock` unit test rather than a sleep -- the same split
/// `WorkerRegistry` and `LeaseTable` are built on, and for the reason recorded
/// there.
///
/// It answers two questions and keeps them separate: **who is out there**, which
/// any broadcast can influence, and **who has proved they belong**, which only a
/// completed handshake can. Collapsing the two is the whole security failure this
/// layer exists to avoid: a node that is admitted is assigned compile jobs and
/// returns objects cached fleet-wide, so admitting on a beacon alone is object
/// injection into everybody's build.
class PeerDirectory
{
  public:
    /// How long a peer is remembered after its last beacon.
    ///
    /// Generous relative to the beacon interval, because the two errors are not
    /// symmetric: forgetting a live peer costs a rediscovery round-trip and a
    /// membership churn, while remembering a dead one costs one failed connect
    /// that Raft already handles. Loss is expected -- these are broadcasts.
    static constexpr std::chrono::seconds DefaultExpiry { 90 };

    /// Construct over its collaborators.
    ///
    /// **This node's cluster and id are read LIVE from @p self at every beacon**, never copied
    /// here: a node's cluster changes when it dissolves or adopts a fleet, and the discovery
    /// service, its challenge issuer and this directory must name one cluster at every moment --
    /// one source, read by all three, rather than three copies that can disagree.
    /// @param clock Time source.
    /// @param self What this node says about itself: its cluster (a beacon naming another is kept
    ///        apart, in the foreign table) and its id (so its own beacons are ignored). Must
    ///        outlive this.
    /// @param keys The roster: a peer whose id it holds a live key for is bounded by it and never
    ///        displaced. Must outlive this.
    /// @param expiry How long a peer is remembered after its last beacon.
    PeerDirectory(core::platform::IClock& clock,
                  IFleetSummarySource const& self,
                  Consensus::IRaftPeerKeys const& keys,
                  std::chrono::seconds expiry = DefaultExpiry);

    /// Record a beacon.
    ///
    /// Ignores this node's own beacon and one whose claimed identity could never be
    /// written down, keeps another fleet's APART -- in the foreign table, never as a
    /// peer -- and never marks a peer authenticated: only `MarkAuthenticated` does that.
    ///
    /// The cluster split lives here rather than in the caller so that "two
    /// unrelated fleets share a segment" is a unit test rather than a property of
    /// whoever happens to be reading datagrams. So does the "can this be named"
    /// filter, and for a sharper version of the same reason: what this directory
    /// remembers is what a leader eventually proposes as a member, so a peer it
    /// declines to remember is one no proposal can ever be generated for. A caller
    /// that filtered afterwards would leave the bytes in the directory, in its own
    /// log lines, and in whatever reads it next.
    ///
    /// The beacon's `clusterId` is deliberately **not** subject to that filter. It
    /// is compared, and another fleet's is kept only as a key of the foreign table,
    /// which nothing renders -- so checking it would buy nothing and would cost a
    /// fleet whose cluster id is not UTF-8 every peer it has, silently, because
    /// both sides would agree to ignore each other.
    /// @param summary What the beacon says: the cluster it claims, who sent it, and
    ///        where they say they answer.
    /// @return What was done with it, so a caller can report the one that is a fault
    ///         and count the one that is a bound.
    BeaconOutcome NoteBeacon(CompileCacheWire::FleetSummary const& summary);

    /// Record that a peer proved the key the cluster holds for its id.
    ///
    /// Separate from `NoteBeacon` so that the authenticated bit can only ever be
    /// set by a completed handshake. A peer that changes the endpoint it
    /// advertises **loses** it: the endpoint is inside the signature, so a proof
    /// authenticates one endpoint and not the node in general, and carrying the
    /// bit across a change would admit an address nobody proved.
    /// @param nodeId Who proved it.
    /// @param raftEndpoint The endpoint they proved for.
    /// @param key The key they proved it with.
    /// @return True when a peer was marked, false when none is known by that id.
    bool MarkAuthenticated(std::string_view nodeId, std::string_view raftEndpoint, Ed25519PublicKey const& key);

    /// Record that another fleet proved its summary: its slot is kept until its beacons stop,
    /// and no newcomer displaces it.
    ///
    /// Recorded even when its beacon's slot was displaced between the challenge and the proof, by
    /// displacing the oldest fleet that has proven nothing -- the proof is what a slot is for.
    /// @param clusterId The fleet that proved.
    /// @return False only when every slot is held by a fleet that proved, and this one is not
    ///         among them.
    bool MarkForeignProven(std::string_view clusterId);

    /// Forget peers, and other fleets, whose last beacon is older than the expiry.
    /// @return How many were forgotten, peers and other fleets together.
    std::size_t ExpireStale();

    /// Every peer currently known, whether authenticated or not.
    /// @return A snapshot, ordered by node id so callers and tests are stable.
    [[nodiscard]] std::vector<KnownPeer> Peers() const;

    /// Peers that have proved the identity key the cluster holds for them.
    ///
    /// What a membership change may be proposed from, and nothing else.
    /// @return A snapshot, ordered by node id.
    [[nodiscard]] std::vector<KnownPeer> AuthenticatedPeers() const;

    /// How many peers are known, authenticated or not.
    /// @return The count.
    [[nodiscard]] std::size_t Size() const noexcept
    {
        return _peers.size();
    }

    /// How many other fleets are remembered: at most `MaxForeignFleets`.
    /// @return The count.
    [[nodiscard]] std::size_t ForeignFleets() const noexcept
    {
        return _foreign.size();
    }

    /// How many of this cluster's peers the roster holds no key for: at most `MaxUnrosteredPeers`.
    /// @return The count.
    [[nodiscard]] std::size_t UnrosteredPeers() const noexcept;

  private:
    /// Another fleet, as this node heard it.
    struct ForeignFleet
    {
        core::platform::SteadyTimePoint recordedAt {}; ///< When its slot was taken, which ages it.
        core::platform::SteadyTimePoint lastSeen {};   ///< Its most recent beacon, which expires it.
        bool proven { false };                         ///< Whether it answered a challenge.
    };

    /// Record a new fleet, displacing the oldest that has proven nothing when the table is full.
    /// @param clusterId The fleet.
    /// @param proven Whether it has proven its summary already.
    /// @return Whether it was recorded, and whether a slot was displaced for it.
    [[nodiscard]] std::optional<bool> InsertForeign(std::string const& clusterId, bool proven);

    core::platform::IClock& _clock;
    IFleetSummarySource const& _self;
    Consensus::IRaftPeerKeys const& _keys;
    std::chrono::seconds _expiry;

    /// This cluster's peers, by node id: the rostered bounded by the roster, the rest at most
    /// `MaxUnrosteredPeers`, the least recently heard of them displaced first.
    std::unordered_map<std::string, KnownPeer> _peers;

    /// Other fleets heard on the segment, by cluster id.
    ///
    /// Bounded at `MaxForeignFleets`. A fleet that PROVED its summary keeps its slot until its
    /// beacons stop; one that only beaconed is displaced, oldest first, by a newcomer when the table
    /// is full. So a flood of invented ids cannot keep a real fleet from being challenged -- it
    /// displaces other invented ids -- and cannot push out a fleet that has answered. Expired on
    /// the peers' own clock.
    std::unordered_map<std::string, ForeignFleet> _foreign;
};

} // namespace FastCache::Cluster
