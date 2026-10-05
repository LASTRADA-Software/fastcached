// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/BeaconDestinations.hpp>
#include <FastCache/Cluster/ChallengeCookies.hpp>
#include <FastCache/Cluster/DiscoveryBounds.hpp>
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Cluster/PeerDirectory.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/ThrottledReport.hpp>
#include <FastCache/Cluster/WorkBudget.hpp>
#include <FastCache/Consensus/IRaftPeerKeys.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <core/net/IDatagramSocket.hpp>
#include <core/net/NetError.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Cluster
{

/// How often a node announces itself unless told otherwise.
inline constexpr std::chrono::seconds DefaultBeaconInterval { 15 };

// Every work budget comes back in full inside one beacon round, shared and one source host's
// alike, so a round's honest burst of challenges or proofs never leaves the next round short.
static_assert(std::ranges::all_of(WorkBudgets,
                                  [](WorkBudgetRow const& row) {
                                      return row.refillEvery * static_cast<std::int64_t>(row.burst) <= DefaultBeaconInterval
                                             && row.sourceRefillEvery * static_cast<std::int64_t>(row.sourceBurst)
                                                    <= DefaultBeaconInterval;
                                  }),
              "every work budget must refill within one default beacon interval");

/// Everything a node needs to take part in discovery, beside what it says about itself -- which
/// comes from its `IFleetSummarySource`, since it changes while the node runs.
struct DiscoveryConfig
{
    /// Where beacons are sent, asked at every beacon. REQUIRED: a service without one is a
    /// precondition violation.
    ///
    /// A seam rather than an address, because the default is a SET that changes while the node
    /// runs -- every up interface's directed broadcast (`DirectedBroadcastDestinations`) -- while an
    /// operator's `--discovery`, and a test's segment, are one address (`FixedBeaconDestination`).
    /// Which it is is decided in `DiscoveryTier`, by whether the operator NAMED the flag.
    std::shared_ptr<IBeaconDestinations const> beaconDestinations;

    /// How often this node announces itself.
    std::chrono::seconds beaconInterval { DefaultBeaconInterval };

    /// The longest a challenge this node issued stays answerable: at least half of this, never
    /// more (`ChallengeCookies`).
    ///
    /// Short, because its only job is to bound how long a nonce is worth
    /// capturing. A joiner that misses the window sees the next beacon and is
    /// challenged again, which costs one interval.
    std::chrono::seconds challengeLifetime { 30 };
};

/// What a single pump did, so a caller and a test can see it rather than infer it.
enum class DiscoveryEvent : std::uint8_t
{
    Nothing,            ///< Timed out with no datagram.
    Closed,             ///< The socket was shut down.
    Ignored,            ///< Not ours, malformed, this node's own, or past a bound.
    PeerSeen,           ///< A beacon was recorded; a challenge went out.
    ChallengeWithheld,  ///< A beacon was recorded, but no cookie key could be drawn, so no challenge went out.
    ChallengeAnswered,  ///< A challenge arrived and was answered with a proof.
    PeerAuthenticated,  ///< A proof verified under the key the roster holds for its id; the peer may be desired.
    PeerUnknownKey,     ///< A proof verified under a key the roster does not hold for its id: reported, not desired.
    PeerRevokedKey,     ///< A proof verified under a key the roster has revoked: reported, not desired.
    ProofRejected,      ///< A proof answered no challenge this node issued -- for another cluster, node or endpoint
                        ///< included -- or its signature did not verify.
    ForeignFleetProven, ///< Another cluster's node proved its summary: handed to the fleet observer, never desired.
    ReplyWithheld,      ///< A challenge or a proof was not sent: larger than the datagram it answers (`AnswerFits`),
                        ///< or past the answer budget; counted.
    ProofExpired,       ///< A proof answered a challenge whose window has closed: neither counted nor logged.
    ProofReplayed,      ///< A proof answered a challenge a verified proof already answered: neither counted nor
                        ///< logged, since a duplicated datagram looks exactly like one.
    ProofUnchecked,     ///< A proof answered a live challenge, but the check budget was spent, so its signature
                        ///< was not checked and nothing was spent; counted and said.
    ProofExhausted,     ///< A proof answered a challenge whose cookie has already failed `MaxForgeriesPerChallenge`
                        ///< signature checks, so it was not checked; counted and said.
};

/// What one beacon attempt came to, so a caller can tell "nothing to do" from "it failed".
///
/// **Private: never transmitted, never persisted.** Five answers rather than a `bool`, because
/// two of them are not failures of the send: a `false` for "there was nowhere to send it" read
/// as "the stack refused it" and was reported as a refusal at every beat, on a machine where no
/// refusal ever happened. `NoDestination` and `Withheld` are said by whoever knows them, once per
/// change; the two refusals are the caller's to report, since only it sees them beat after beat.
enum class BeaconSendOutcome : std::uint8_t
{
    Sent,          ///< The local stack accepted the beacon at every destination.
    PartlyRefused, ///< Accepted at some destinations and refused at others: peers on a refused link
                   ///< do not hear this node, although it is announcing -- the LAN refused while a
                   ///< VPN adapter takes it.
    NoDestination, ///< There was nowhere to send it; said by the destinations when that changes
                   ///< (`DirectedBroadcastDestinations`).
    Withheld,      ///< This node's summary names an endpoint only the dialling machine reaches;
                   ///< said, throttled, where it is withheld (`AnnouncesOnlyThisMachine`).
    AllRefused,    ///< The local stack refused the beacon at every destination it was sent to.
};

/// One destination the local stack refused a beacon for, and what it answered.
struct BeaconRefusal
{
    core::net::DatagramAddress destination; ///< Where the beacon was to go.
    core::net::NetError error;              ///< What the local stack said, which is the only cause there is.
};

/// What one beacon attempt came to, and, for every destination that refused it, why.
///
/// The reasons travel with the outcome because the caller that reports a refusal is not the code
/// that saw it: a line saying "refused" with no cause leaves an operator unable to tell a firewall's
/// `EACCES` from a route that is not there yet.
struct BeaconSendReport
{
    BeaconSendOutcome outcome;                        ///< What it came to.
    std::vector<core::net::DatagramAddress> accepted; ///< Each destination that took it, in order.
    std::vector<BeaconRefusal> refused;               ///< Each refused destination, in the order they were tried.
};

/// @param refused Refused destinations.
/// @return Each as `host:port: error`, joined by `; `, for a log line.
[[nodiscard]] std::string DescribeRefusals(std::span<BeaconRefusal const> refused);

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
///
/// **Another cluster is challenged and answered too**, because a solitary machine has to see a
/// fleet to yield to it and an established one has to see a foreign fleet to say so. What an
/// answer signs is this node's public summary -- the bytes its beacon already shouts -- so
/// answering announces nothing a listener lacked. Another cluster's proof is judged by its
/// signature alone and handed to the `IFleetObserver`; it is never asked of the roster, never
/// recorded as a peer and never desired.
class DiscoveryService
{
  public:
    /// Construct over its collaborators; all must outlive the service.
    /// @param socket Where datagrams come from and go.
    /// @param clock Time source.
    /// @param random Where the keys challenges are MACed under come from.
    /// @param directory Who is known and who has proved themselves.
    /// @param config What this node announces and accepts.
    /// @param logger Where joins and rejections are reported.
    /// How often a beacon nobody can name is reported, at most.
    ///
    /// Throttled rather than logged per datagram: a beacon is unauthenticated by
    /// construction, so a single spoofable datagram provokes this line and anything
    /// on the segment can send them at line rate. A log an attacker can grow without
    /// holding the key is a disk-exhaustion hole reached from outside the fleet.
    ///
    /// Generous, because the fault it reports is a *standing* one -- a peer whose
    /// identity is not text stays that way -- and the beacons repeat on their own
    /// interval, so an operator who looks at any minute of the log sees it.
    static constexpr std::chrono::seconds UnnameableReportInterval { 60 };

    /// How often a challenge withheld for want of a cookie key is reported, at most.
    ///
    /// Throttled for `UnnameableReportInterval`'s reason: a beacon provokes it, and anything
    /// on the segment can send one. What it reports is this host's own generator failing, so
    /// every beacon from every peer provokes it until that is fixed (#1527).
    static constexpr std::chrono::seconds NoNonceReportInterval { 60 };

    /// How often each kind of rejected proof is reported, at most: a forgery, an answer to no
    /// challenge this node issued, and a proof under a key the roster does not accept.
    ///
    /// Throttled for `UnnameableReportInterval`'s reason, and more pointedly: a proof costs its
    /// sender nothing to make, so anything on the segment can provoke one per datagram. Each kind
    /// has its own throttle -- one kind never hides another -- and every line says how many it
    /// stands for.
    static constexpr std::chrono::seconds RejectedProofReportInterval { 60 };

    /// How often a beacon of a new fleet dropped because every remembered fleet has proven itself
    /// is reported, at most.
    ///
    /// Said at all, unlike the other bounds, because reaching this one is not a flood of datagrams:
    /// it takes `MaxForeignFleets` fleets proven under keys of their own, each an answering host on
    /// the segment, and what it costs is that a real fleet arriving now is not seen. Throttled for
    /// `UnnameableReportInterval`'s reason: once the table is held, every beacon of every further
    /// fleet provokes it.
    static constexpr std::chrono::seconds ForeignTableFullReportInterval { 60 };

    /// How often an announcement withheld because it names only this machine is reported, at
    /// most: it is a standing configuration, and the beacon interval would otherwise repeat it.
    static constexpr std::chrono::seconds UnannounceableReportInterval { 60 };

    /// @param self What this node says about itself, read afresh for every beacon, every
    ///        challenge and every answer, and the ONE source of this node's cluster: the
    ///        directory, the challenges and the own-or-foreign judgement of a proof all read it
    ///        live, so a cluster changed by a dissolve or an adoption is followed by all three at
    ///        once. Must outlive this.
    /// @param fleets Where another cluster's proven summary goes; must outlive this.
    /// @param keys This node's own key, which signs its proofs, and the roster that says
    ///        whose key every other id is; must outlive this.
    /// @param metrics Where proofs under keys the roster does not accept, and beacons past a
    ///        bound, are counted.
    DiscoveryService(core::net::IDatagramSocket& socket,
                     core::platform::IClock& clock,
                     ISecureRandom& random,
                     PeerDirectory& directory,
                     DiscoveryConfig config,
                     IFleetSummarySource const& self,
                     IFleetObserver& fleets,
                     Consensus::IRaftPeerKeys const& keys,
                     IMetricsSink& metrics,
                     ILogger& logger);

    /// Announce this node at every destination `DiscoveryConfig::beaconDestinations` names now.
    /// @return `Sent` when the local stack accepted the datagram at every destination,
    ///         `PartlyRefused` when at some, `AllRefused` when at none, and `NoDestination` or
    ///         `Withheld` when no datagram was sent at all -- neither of which is a failure of the
    ///         send; with where it was accepted, and every refusal with what the stack said.
    BeaconSendReport SendBeacon();

    /// Handle at most one datagram.
    /// @param timeout How long to wait for one.
    /// @return What happened.
    DiscoveryEvent PumpOnce(std::chrono::milliseconds timeout);

    /// Forget expired peers.
    void Maintain();

    /// How many verified challenges are remembered as spent.
    /// @return The count; a key epoch holds a state for every challenge it issued (`MaxEpochChallenges`).
    [[nodiscard]] std::size_t SpentChallenges() const noexcept
    {
        return _cookies.SpentHeld();
    }

  private:
    /// What this node may put on the wire about itself: the summary its beacon carries and its
    /// proof signs.
    ///
    /// ONE door for both, so a beacon and a proof from this node never describe it differently
    /// -- the endpoint a peer challenged must be the endpoint the answer signs -- and so the one
    /// rule about what may never be announced is folded into the only way to announce.
    /// @return This node's summary, or nullopt when it names an endpoint only the dialling
    ///         machine reaches, which is reported (throttled) and withheld.
    [[nodiscard]] std::optional<CompileCacheWire::FleetSummary> AnnounceableSummary();

    /// Ask a peer to prove it holds the key.
    /// @param peer Who to challenge.
    /// @param replyTo Where to send it.
    /// @param beaconBytes The size of the beacon datagram, which the challenge is padded to and
    ///        must not exceed (`DiscoveryWire::EncodeChallenge`).
    /// @return `PeerSeen` when a challenge went out; `ChallengeWithheld` when no cookie key could be
    ///         drawn, reported; `ReplyWithheld` when the challenge cannot fit the beacon, counted.
    [[nodiscard]] DiscoveryEvent IssueChallenge(DiscoveryWire::Beacon const& peer,
                                                core::net::DatagramAddress const& replyTo,
                                                std::size_t beaconBytes);

    /// Answer a challenge with a proof, when it may be answered.
    /// @param challenge What was asked.
    /// @param challengeBytes The size of the challenge datagram, which the proof must not exceed.
    /// @param replyTo Where it came from, and where the proof goes.
    /// @return `ChallengeAnswered` when a proof went out; `ReplyWithheld` when the proof would be
    ///         larger than the challenge or the answer budget is spent, counted either way and
    ///         decided BEFORE anything is signed; `Ignored` when this node may announce nothing.
    [[nodiscard]] DiscoveryEvent AnswerChallenge(DiscoveryWire::Challenge const& challenge,
                                                 std::size_t challengeBytes,
                                                 core::net::DatagramAddress const& replyTo);

    /// Judge a proof: the challenge it answers, its signature, and then whose key it is.
    /// @param proof What arrived.
    /// @param from Where it came from, for a report.
    /// @return What it proved.
    [[nodiscard]] DiscoveryEvent JudgeProof(DiscoveryWire::Proof const& proof, core::net::DatagramAddress const& from);

    /// Say what a proof that verified nothing was, throttled per kind, and count a forgery.
    /// @param refusal Why it verified nothing.
    /// @param from Where it came from: the only part of an unverified proof that is printed.
    /// @return The event it is.
    [[nodiscard]] DiscoveryEvent Refused(CookieRefusal refusal, core::net::DatagramAddress const& from);

    /// Report a proof under a key the roster does not accept, throttled.
    /// @param revoked Whether the key is one the roster revoked, rather than one it never held.
    /// @param proof What arrived.
    /// @param from Where it came from.
    void ReportUnacceptedKey(bool revoked, DiscoveryWire::Proof const& proof, core::net::DatagramAddress const& from);

    core::net::IDatagramSocket& _socket;
    core::platform::IClock& _clock;
    PeerDirectory& _directory;
    DiscoveryConfig _config;
    IFleetSummarySource const& _self;
    IFleetObserver& _fleets;
    /// Every challenge this node sends, naming `_self`'s cluster as it is at the time, and the one
    /// judge of every proof that answers one. Holds no challenge: only the proofs that verified.
    ChallengeCookies _cookies;
    Consensus::IRaftPeerKeys const& _keys;
    IMetricsSink& _metrics;
    ILogger& _logger;

    /// When a beacon nobody can name may next be reported.
    ///
    /// Value-initialized so the first one always is: the epoch is behind any clock
    /// this runs on, including a `core::platform::ManualClock` that has never been advanced.
    core::platform::SteadyTimePoint _nextUnnameableReport {};

    /// When a challenge withheld for want of a cookie key may next be reported; value-initialized
    /// for the reason `_nextUnnameableReport` is.
    core::platform::SteadyTimePoint _nextNoNonceReport {};

    /// When an announcement withheld because it names only this machine may next be reported;
    /// value-initialized for the reason `_nextUnnameableReport` is.
    core::platform::SteadyTimePoint _nextUnannounceableReport {};

    /// The rejected-proof lines, one throttle per kind: see `RejectedProofReportInterval`.
    ThrottledReport _unacceptedKeyReport { RejectedProofReportInterval };
    ThrottledReport _forgedProofReport { RejectedProofReportInterval };
    ThrottledReport _unissuedProofReport { RejectedProofReportInterval };
    ThrottledReport _uncheckedProofReport { RejectedProofReportInterval };
    ThrottledReport _exhaustedProofReport { RejectedProofReportInterval };
    ThrottledReport _foreignTableFullReport { ForeignTableFullReportInterval };

    /// What answering challenges may still cost, per source host and all together, on the injected
    /// clock (`WorkBudget`).
    WorkBudget _answerBudget;

    /// What checking proofs may still cost, the same way: asked only for a proof whose cookie holds,
    /// since a forgery does not spend the cookie it names.
    WorkBudget _checkBudget;
};

} // namespace FastCache::Cluster
