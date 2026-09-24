// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Cluster/PeerDirectory.hpp>
#include <FastCache/Consensus/IRaftPeerKeys.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include <core/net/IDatagramSocket.hpp>
#include <core/platform/Clock.hpp>

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
    /// `core::net::testing::DatagramBus::BroadcastAddress()` and a deployment at a real broadcast or
    /// multicast address -- which subnet to shout on is a site's decision and
    /// not this layer's.
    ///
    /// Host and port apart rather than `host:port` text, because that is the
    /// shape `core::net::IDatagramSocket` takes -- see `core::net::DatagramAddress`. `--discovery` is
    /// split in `DiscoveryTier::Start`, which is where that value is validated
    /// and where the two error messages it can produce are worth telling apart.
    core::net::DatagramAddress beaconAddress;

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
    ChallengeWithheld, ///< A beacon was recorded, but no nonce could be drawn, so no challenge went out.
    ChallengeAnswered, ///< A challenge arrived and was answered with a proof.
    PeerAuthenticated, ///< A proof verified under the key the roster holds for its id; the peer may be desired.
    PeerUnknownKey,    ///< A proof verified under a key the roster does not hold for its id: reported, not desired.
    PeerRevokedKey,    ///< A proof verified under a key the roster has revoked: reported, not desired.
    ProofRejected,     ///< A proof did not verify, answered nothing this node asked, or claimed another endpoint.
};

/// LAN discovery: find peers, learn which identity key each one holds, and say which of
/// them the cluster already knows (#178).
///
/// The one piece of this feature that speaks to the network, and it is kept as
/// thin as that allows: what a datagram means lives in `DiscoveryWire`, who is
/// remembered lives in `PeerDirectory`, and this drives them over an
/// `core::net::IDatagramSocket`. `PumpOnce` is the whole state machine and is synchronous,
/// so an entire segment forming a cluster is a loop in a unit test rather than
/// several processes and a sleep.
///
/// **It never changes membership itself.** It answers "who has proved a key the roster
/// knows, and where do they answer", and a caller decides what to propose. That separation
/// is deliberate: admitting a node is a Raft decision that only a leader may make, and a
/// discovery layer that proposed directly would have every node in the segment proposing
/// the same change at once.
///
/// **And it admits nobody new** (#178). Under the shared key a proof WAS membership, so any
/// holder was authenticated; now a proof names a KEY, and only a key the roster already holds
/// for that id authenticates. A key the roster does not know, or has revoked, is reported --
/// counted, and named in the log with the id and the address it came from -- and never
/// handed on: LAN auto-admission ends, and admitting a machine is an operator's act
/// (`--enroll-approve`, or `--cluster-admit ...@<key>`).
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
    /// How often a beacon nobody can name is reported, at most.
    ///
    /// Throttled rather than logged per datagram, and the reason is the one that
    /// makes `_pending` one entry per node: a beacon is unauthenticated by
    /// construction, so a single spoofable datagram provokes this line and anything
    /// on the segment can send them at line rate. A log an attacker can grow without
    /// holding the key is a disk-exhaustion hole reached from outside the fleet.
    ///
    /// Generous, because the fault it reports is a *standing* one -- a peer whose
    /// identity is not text stays that way -- and the beacons repeat on their own
    /// interval, so an operator who looks at any minute of the log sees it.
    static constexpr std::chrono::seconds UnnameableReportInterval { 60 };

    /// How often a challenge withheld for want of a nonce is reported, at most.
    ///
    /// Throttled for `UnnameableReportInterval`'s reason: a beacon provokes it, and anything
    /// on the segment can send one. What it reports is this host's own generator failing, so
    /// every beacon from every peer provokes it until that is fixed (#1527).
    static constexpr std::chrono::seconds NoNonceReportInterval { 60 };

    /// How often a proof under a key the roster does not accept is reported, at most.
    ///
    /// Throttled for `UnnameableReportInterval`'s reason, and more pointedly: a proof under a
    /// FRESH key costs its sender nothing to make, so anything on the segment can provoke one
    /// per beacon. Every one is COUNTED; the log carries the latest, by id, key and address,
    /// plus how many it stands for.
    static constexpr std::chrono::seconds UnacceptedKeyReportInterval { 60 };

    /// @param keys This node's own key, which signs its proofs, and the roster that says
    ///        whose key every other id is; must outlive this.
    /// @param metrics Where proofs under keys the roster does not accept are counted.
    DiscoveryService(core::net::IDatagramSocket& socket,
                     core::platform::IClock& clock,
                     ISecureRandom& random,
                     PeerDirectory& directory,
                     DiscoveryConfig config,
                     Consensus::IRaftPeerKeys const& keys,
                     IMetricsSink& metrics,
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
        DiscoveryWire::Challenge challenge;          ///< What was asked.
        std::string endpoint;                        ///< Where the answer must come from.
        core::platform::SteadyTimePoint issuedAt {}; ///< When, so it can expire.
    };

    /// Ask a peer to prove it holds the key.
    /// @param peer Who to challenge.
    /// @param replyTo Where to send it.
    /// @return Whether a challenge went out: false when no nonce could be drawn, which is
    ///         reported here and leaves any earlier challenge to that peer as it was.
    [[nodiscard]] bool IssueChallenge(DiscoveryWire::Beacon const& peer, core::net::DatagramAddress const& replyTo);

    /// Judge a proof that answered a challenge this node issued.
    /// @param proof What arrived.
    /// @param challenge What this node asked.
    /// @param from Where it came from, for a report.
    /// @return What it proved.
    [[nodiscard]] DiscoveryEvent JudgeProof(DiscoveryWire::Proof const& proof,
                                            DiscoveryWire::Challenge const& challenge,
                                            core::net::DatagramAddress const& from);

    /// Report a proof under a key the roster does not accept, throttled.
    /// @param revoked Whether the key is one the roster revoked, rather than one it never held.
    /// @param proof What arrived.
    /// @param from Where it came from.
    void ReportUnacceptedKey(bool revoked, DiscoveryWire::Proof const& proof, core::net::DatagramAddress const& from);

    core::net::IDatagramSocket& _socket;
    core::platform::IClock& _clock;
    ISecureRandom& _random;
    PeerDirectory& _directory;
    DiscoveryConfig _config;
    Consensus::IRaftPeerKeys const& _keys;
    IMetricsSink& _metrics;
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

    /// When a beacon nobody can name may next be reported.
    ///
    /// Value-initialized so the first one always is: the epoch is behind any clock
    /// this runs on, including a `core::platform::ManualClock` that has never been advanced.
    core::platform::SteadyTimePoint _nextUnnameableReport {};

    /// When a challenge withheld for want of a nonce may next be reported; value-initialized
    /// for the reason `_nextUnnameableReport` is.
    core::platform::SteadyTimePoint _nextNoNonceReport {};

    /// When a proof under a key the roster does not accept may next be reported, and how many
    /// have arrived since the last one was; value-initialized for `_nextUnnameableReport`'s
    /// reason.
    core::platform::SteadyTimePoint _nextUnacceptedKeyReport {};
    std::size_t _unacceptedSinceReport { 0 };
};

} // namespace FastCache::Cluster
