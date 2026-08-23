// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Cluster/PeerDirectory.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IDatagramSocket.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace FastCache::Cluster
{

/// Everything a node needs to take part in discovery.
struct DiscoveryConfig
{
    std::string clusterId;    ///< Which fleet this node belongs to.
    std::string nodeId;       ///< This node's Raft id.
    std::string raftEndpoint; ///< Where this node answers Raft peer traffic.

    /// Where beacons are sent.
    ///
    /// A parameter rather than a constant so a test can point a whole segment at
    /// `DatagramBus::BroadcastAddress` and a deployment at a real broadcast or
    /// multicast address -- which subnet to shout on is a site's decision and
    /// not this layer's.
    std::string beaconAddress;

    /// The cluster's pre-shared key.
    ///
    /// Never sent, and nothing derived from it is either. It only ever appears
    /// inside an HMAC over a nonce this node chose or was given.
    std::vector<std::byte> presharedKey;

    /// How often this node announces itself.
    std::chrono::seconds beaconInterval { 15 };

    /// How long a challenge this node issued stays answerable.
    ///
    /// Short, because its only job is to bound how long a nonce is worth
    /// capturing. A joiner that misses the window sees the next beacon and is
    /// challenged again, which costs one interval.
    std::chrono::seconds challengeLifetime { 30 };
};

/// What a single pump did, so a caller and a test can see it rather than infer it.
enum class DiscoveryEvent : std::uint8_t
{
    Nothing,           ///< Timed out with no datagram.
    Closed,            ///< The socket was shut down.
    Ignored,           ///< Not ours, malformed, or for another cluster.
    PeerSeen,          ///< A beacon was recorded; a challenge went out.
    ChallengeAnswered, ///< A challenge arrived and was answered with a proof.
    PeerAuthenticated, ///< A proof checked out; the peer may now be proposed.
    ProofRejected,     ///< A proof did not check out, and was discarded.
};

/// LAN discovery: find peers, prove who holds the cluster key, and say who may
/// be admitted.
///
/// The one piece of this feature that speaks to the network, and it is kept as
/// thin as that allows: what a datagram means lives in `DiscoveryWire`, who is
/// remembered lives in `PeerDirectory`, and this drives them over an
/// `IDatagramSocket`. `PumpOnce` is the whole state machine and is synchronous,
/// so an entire segment forming a cluster is a loop in a unit test rather than
/// several processes and a sleep.
///
/// **It never changes membership itself.** It answers "who has proved they hold
/// the key, and where do they answer", and a caller decides what to propose. That
/// separation is deliberate: admitting a node is a Raft decision that only a
/// leader may make, and a discovery layer that proposed directly would have every
/// node in the segment proposing the same change at once.
class DiscoveryService
{
  public:
    /// Construct over its collaborators; all must outlive the service.
    /// @param socket Where datagrams come from and go.
    /// @param clock Time source.
    /// @param random Where nonces come from.
    /// @param directory Who is known and who has proved themselves.
    /// @param config What this node announces and accepts.
    /// @param logger Where joins and rejections are reported.
    DiscoveryService(IDatagramSocket& socket,
                     IClock& clock,
                     IRandomSource& random,
                     PeerDirectory& directory,
                     DiscoveryConfig config,
                     ILogger& logger);

    /// Announce this node on the segment.
    /// @return Whether the datagram was accepted by the local stack.
    bool SendBeacon();

    /// Handle at most one datagram.
    /// @param timeout How long to wait for one.
    /// @return What happened.
    DiscoveryEvent PumpOnce(std::chrono::milliseconds timeout);

    /// Forget expired peers and stale challenges.
    void Maintain();

    /// How many challenges are outstanding.
    /// @return The count.
    [[nodiscard]] std::size_t PendingChallenges() const noexcept
    {
        return _pending.size();
    }

  private:
    /// A challenge this node issued and is waiting on.
    struct Pending
    {
        DiscoveryWire::Challenge challenge; ///< What was asked.
        std::string endpoint;               ///< Where the answer must come from.
        TimePoint issuedAt {};              ///< When, so it can expire.
    };

    /// Ask a peer to prove it holds the key.
    /// @param peer Who to challenge.
    /// @param replyTo Where to send it.
    void IssueChallenge(DiscoveryWire::Beacon const& peer, std::string_view replyTo);

    IDatagramSocket& _socket;
    IClock& _clock;
    IRandomSource& _random;
    PeerDirectory& _directory;
    DiscoveryConfig _config;
    ILogger& _logger;

    /// Outstanding challenges, keyed by the node they were sent to.
    ///
    /// One per node rather than a list: a peer that is challenged again before
    /// answering replaces its own entry, so a flood of beacons from one source
    /// cannot grow this without bound. That matters because a beacon is
    /// unauthenticated -- anything on the segment can send one, and an attacker
    /// who could make this table grow per datagram would have a memory-exhaustion
    /// hole reached without holding the key.
    std::unordered_map<std::string, Pending> _pending;
};

} // namespace FastCache::Cluster
