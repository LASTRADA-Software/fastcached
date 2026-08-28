// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace FastCache::Cluster
{

/// What a directory did with a beacon.
///
/// An enum rather than the `bool` this used to be, because that bool meant four
/// different things and a caller could tell none of them apart -- which is why
/// nothing could ever be said about the one that is worth saying something about.
/// Three of these are ordinary traffic on a shared segment; only `Unnameable` is
/// a fault, and only it is reported.
enum class BeaconOutcome : std::uint8_t
{
    Recorded,     ///< A peer this node can name and reach; it may now be challenged.
    OtherCluster, ///< Announced for a fleet this node is not in.
    Self,         ///< This node's own beacon, come back to it on the broadcast.

    /// Names nothing this node could ever record as a member.
    ///
    /// An empty id or endpoint, or one that is not valid UTF-8. The two are one
    /// answer because they are one rule -- a member is `(id, endpoint)` and both
    /// halves have to survive being written down, replicated, and read back out by
    /// `/fleet.json`, the page, `--cluster-status` and every log line.
    Unnameable,
};

/// A peer this node has heard from, and when.
struct KnownPeer
{
    std::string nodeId;       ///< How membership names it.
    std::string raftEndpoint; ///< Where its Raft peer server answers.

    /// Whether this peer has proved it holds the cluster key.
    ///
    /// Seen and admitted are different facts and are kept apart on purpose. A
    /// beacon is unauthenticated by construction -- anybody on the segment can
    /// send one -- so a directory that recorded "seen" as "trusted" would let a
    /// broadcast alone drive a membership change.
    bool authenticated { false };

    /// When its most recent beacon arrived.
    std::chrono::steady_clock::time_point lastSeen {};
};

/// What this node has heard on the segment, and what it may act on.
///
/// Pure with respect to I/O: time arrives through `IClock` and datagrams arrive
/// as already-decoded values, which is what lets every expiry and admission rule
/// be a `ManualClock` unit test rather than a sleep -- the same split
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
    /// @param clock Time source.
    /// @param clusterId The cluster this node belongs to; beacons naming another
    ///        are ignored.
    /// @param selfNodeId This node's own id, so its own beacons are ignored.
    /// @param expiry How long a peer is remembered after its last beacon.
    PeerDirectory(IClock& clock, std::string clusterId, std::string selfNodeId, std::chrono::seconds expiry = DefaultExpiry);

    /// Record a beacon.
    ///
    /// Ignores a beacon for another cluster and this node's own, ignores one whose
    /// claimed identity could never be written down, and never marks a peer
    /// authenticated -- only `MarkAuthenticated` does that.
    ///
    /// The cluster filter lives here rather than in the caller so that "two
    /// unrelated fleets share a segment" is a unit test rather than a property of
    /// whoever happens to be reading datagrams. So does the "can this be named"
    /// filter, and for a sharper version of the same reason: what this directory
    /// remembers is what a leader eventually proposes as a member, so a peer it
    /// declines to remember is one no proposal can ever be generated for. A caller
    /// that filtered afterwards would leave the bytes in the directory, in its own
    /// log lines, and in whatever reads it next.
    ///
    /// The beacon's `clusterId` is deliberately **not** subject to that filter. It
    /// is compared and never recorded, so checking it would buy nothing and would
    /// cost a fleet whose `--cluster-id` is not UTF-8 every peer it has -- silently,
    /// because both sides would agree to ignore each other.
    /// @param clusterId The cluster the beacon claims.
    /// @param nodeId Who sent it.
    /// @param raftEndpoint Where they say they answer.
    /// @return What was done with it, so a caller can report the one that is a fault.
    BeaconOutcome NoteBeacon(std::string_view clusterId, std::string_view nodeId, std::string_view raftEndpoint);

    /// Record that a peer proved it holds the cluster key.
    ///
    /// Separate from `NoteBeacon` so that the authenticated bit can only ever be
    /// set by a completed handshake. A peer that changes the endpoint it
    /// advertises **loses** it: the endpoint is inside the MAC, so a proof
    /// authenticates one endpoint and not the node in general, and carrying the
    /// bit across a change would admit an address nobody proved.
    /// @param nodeId Who proved it.
    /// @param raftEndpoint The endpoint they proved for.
    /// @return True when a peer was marked, false when none is known by that id.
    bool MarkAuthenticated(std::string_view nodeId, std::string_view raftEndpoint);

    /// Forget peers whose last beacon is older than the expiry.
    /// @return How many were forgotten.
    std::size_t ExpireStale();

    /// Every peer currently known, whether authenticated or not.
    /// @return A snapshot, ordered by node id so callers and tests are stable.
    [[nodiscard]] std::vector<KnownPeer> Peers() const;

    /// Peers that have proved they hold the cluster key.
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

  private:
    IClock& _clock;
    std::string _clusterId;
    std::string _selfNodeId;
    std::chrono::seconds _expiry;
    std::unordered_map<std::string, KnownPeer> _peers;
};

} // namespace FastCache::Cluster
