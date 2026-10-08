// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ConsensusStanding.hpp"
#include "LocalCache.hpp"
#include "NodeConditions.hpp"
#include "NodeConfig.hpp"
#include "NodeFormation.hpp"
#include "NodeMembership.hpp"
#include "NodeRefusal.hpp"
#include "NodeRoster.hpp"
#include "NodeSurfaces.hpp"
#include "SchedulerTier.hpp"
#include "SharedCacheDirectory.hpp"
#include "SharedCacheHost.hpp"

#include <FastCache/Cluster/AnnouncedEndpoints.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/ClusterStateMachine.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Cluster/RosterKeys.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/ForwardingSink.hpp>
#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/platform/Clock.hpp>
// For `ConsensusStatus`, which is the shape a scrape reports this node's own quorum
// in. Defined beside `MetricsSnapshot` rather than here because the renderer is what
// has to know its shape, and `/metrics` is the one surface every node already serves.
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <WorkerProtocol.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/ThreadedAddressResolver.hpp>

namespace FastCache::Node
{

class SchedulingLeaderPublisher;

/// Why a consensus tier cannot start without this node's id.
///
/// Every Raft message is addressed by member id, so a node with none could never be voted for.
/// **No configuration reaches it**: the start mints the id into the state directory before this
/// tier exists, so it names the caller that skipped that, rather than a flag to change. It ends
/// without a full stop for `ConsensusNamesNoDialAddressRefusal`'s reason.
inline constexpr std::string_view ConsensusNeedsNodeIdRefusal =
    "consensus needs this node's id and was started without one: every Raft message is addressed by member id, so a "
    "node without one could never be voted for. The start mints it into the state directory before consensus "
    "exists, so this is a caller that skipped that";

/// Why a consensus tier cannot start without this node's identity key (#178).
///
/// Every Raft peer connection proves each end's OWN key, so a node without one could neither
/// be heard nor hear anybody -- and running consensus unauthenticated instead is the
/// per-connection fallback #1308 refused, one key later. **No configuration reaches it**: a
/// node always has a state directory (`NodeStateDirectory`), and the start resolves
/// the key there -- or refuses, naming the file -- before this tier exists. So it names the
/// caller that skipped that, rather than a flag to change. It ends without a full stop for
/// `ConsensusNamesNoDialAddressRefusal`'s reason.
inline constexpr std::string_view ConsensusNeedsIdentityKeyRefusal =
    "consensus needs this node's identity key and was started without one: every Raft peer connection proves each "
    "end's own key, so a node without one could neither be heard nor hear anybody. The start resolves it out of the "
    "state directory before consensus exists, so this is a caller that skipped that";

/// This node's own member record, which consensus runs as and announces.
///
/// Its entry among the members the formation starts with, or -- where they name it with no
/// endpoint yet -- one built from the address consensus runs under (`ConsensusDialAddressOf`).
/// A mode that dials in (a learner, whose row closes the Raft port) is its entry as recorded,
/// endpoint or none, and is built with NO endpoint where it is absent: nobody dials it, so it is
/// never refused for lacking an address. Any other mode that names no address is refused: a
/// member nobody can reach could never win a vote and could never be voted for.
///
/// Exposed rather than hidden in `Start` because the rule is worth checking and `main.cpp` is in no
/// test target: which mode may start without an address is a rule, and `Start` needs a whole tier
/// to reach it.
/// @param cfg The resolved configuration, with its identity and formation applied.
/// @param members The members the formation starts consensus with (`BootstrapMembersOf`).
/// @param publicKey The identity key this node proves itself with, which a record always holds.
/// @return The record, or `ConsensusNeedsNodeIdRefusal` / `ConsensusNamesNoDialAddressRefusal`.
[[nodiscard]] std::expected<Cluster::ClusterMember, std::string> ConsensusSelfMemberOf(
    NodeConfig const& cfg, std::span<Cluster::MemberSpec const> members, Ed25519PublicKey const& publicKey);

/// Where a member's consensus port answers, as a log line says it.
///
/// A learner dials in, so the cluster may record it with no consensus endpoint, and a line
/// interpolating the empty string reads `recorded laptop at , scheduler ...` -- a blank where
/// the renderers say absent. Exposed so a case can read the wording without a running tier.
/// @param raftEndpoint The recorded endpoint, possibly empty.
/// @return `at <endpoint>`, or `with no consensus endpoint` when it is empty.
[[nodiscard]] std::string DescribeConsensusEndpoint(std::string_view raftEndpoint);

/// Every peer that reaches this node by dialling in, which the transport places as such.
///
/// Read through the one column two ways, and both are needed. The RECORD's seats
/// (`Cluster::LinkOfSeat`) name every learner the cluster agreed on; the CONFIGURATION's
/// standings (`Consensus::Membership::StandingOf`, then `TraitsOf(...).link`) name the ones
/// consensus counts right now -- which after a restart includes a learner whose admission sits in
/// the log tail, applied only once this node has led and committed again, while the leader sends
/// to it from the moment it leads. Placing from the record alone counted every such drop as a
/// peer nothing can reach.
/// @param state The applied state.
/// @param configuration The configuration consensus holds.
/// @return The ids, sorted, each once.
[[nodiscard]] std::vector<Consensus::NodeId> DialInPeers(Cluster::ClusterState const& state,
                                                         Consensus::Configuration const& configuration);

/// What one read of the driver says about this node's own cluster.
///
/// Pure, and separated from `ConsensusTier::Status()` for the reason
/// `QuorumProposalPending` is separated from the reconciler: the acquisition needs a
/// live driver — a reactor, a listener, a peer transport and a state directory — and
/// the mapping needs none of that, so folding the two together would put the answer
/// #435 is about behind a running cluster and out of reach of every unit test.
///
/// **It carries the member set VERBATIM**, in whatever order consensus holds it. A
/// renderer that wants it sorted sorts it; sorting here would make a caller unable
/// to see the order a configuration was adopted in, and this is the read an operator
/// uses to compare two nodes.
/// @param progress One read of the driver, taken under its lock.
/// @return What to report about this node.
[[nodiscard]] ConsensusStatus ConsensusStatusFrom(Consensus::RaftDriver::Progress const& progress);

/// The line announcing what this node has become.
///
/// The **term** is what makes a dump readable after the fact. Without it the log
/// says leadership moved and nothing about how often, so an election that
/// reproduces once in N CI runs is diagnosable from its logs or not at all --
/// which is what issue #117 was, and why nobody could tell whether the algorithm
/// or the fixture was wrong.
///
/// A free function so a case can read it: the wording is the diagnostic, and a
/// rendering reachable only from a running tier is one no case can read.
/// @param role What this node is, in the scheduler's vocabulary.
/// @param term The consensus term it is playing it in.
/// @param leaderEndpoint Where its leader answers; empty when there is none or
///        when this node is it.
/// @return The line, without a level or a newline.
[[nodiscard]] std::string DescribeRole(Distributed::SchedulerRole role,
                                       Consensus::Term term,
                                       std::string_view leaderEndpoint);

/// The line explaining a demotion that a peer's higher term caused.
///
/// Named with the CONSENSUS role rather than the scheduler's, because
/// `pre-candidate` and `candidate` both read as `undecided` there and which of
/// them was demoted is most of the answer.
/// @param adopted The term this node has just adopted.
/// @param cause What it was, and who carried the term.
/// @return The line, without a level or a newline.
[[nodiscard]] std::string DescribeTermAdoption(Consensus::Term adopted, Consensus::TermAdoption const& cause);

/// Why this node will not start consensus on the state its directory holds (#1542).
///
/// `RaftDriver::Create` refused because the application cannot read what the node
/// recovered -- its own snapshot, or a command its own log holds -- and running on the
/// rest would mean running without the members, the settings and the revoked keys
/// that state carried. What an operator needs is WHERE (the directory), WHAT (which
/// part, the version found and the version this build reads, all of it in the
/// refusal's context) and what to DO, which is the store's own remedy: the three files
/// go aside together whichever of them could not be read.
///
/// A free function for the reason `DescribeRole` is: the wording is the diagnostic, and
/// a rendering reachable only from a tier that failed to start is one no case can read.
/// @param directory The node's state directory, as `NodeStateDirectory` names it.
/// @param refusal What `RaftDriver::Create` refused with.
/// @return The refusal, as `ConsensusTier::Start` reports it.
[[nodiscard]] std::string UnreadableConsensusStateRefusal(std::filesystem::path const& directory,
                                                          ConsensusError const& refusal);

/// What this node says while it refuses its leader's snapshot (#1552).
///
/// The detail of the `unreadable-leader-snapshot` condition and the body of its log line:
/// who offered what, why this build cannot read it, and what that leaves this node doing.
/// A free function for the reason `DescribeRole` is.
/// @param refusal What the driver refused.
/// @return The sentence, without a level or a prefix.
[[nodiscard]] std::string DescribeInstallRefusal(Consensus::RaftDriver::InstallRefusal const& refusal);

/// Say, on every surface, that this node refuses its leader's snapshot -- or no longer does.
///
/// What the tier does whenever its driver's refusal starts, changes or ends (#1552). A node
/// refusing is a follower that is BEHIND and stays so until it can read what its leader
/// sends; from the outside that looks exactly like a slow node, so it is an Alert condition
/// and an Error line. Its end is said as well, because watching it clear is watching the
/// upgrade land. A free function so a case can drive both directions without a leader
/// running another build, which is the only thing that produces one.
/// @param refusal The refusal now in force, or nullopt once it has ended.
/// @param logger Where the line goes.
/// @param conditions Where `unreadable-leader-snapshot` is answered; null when nobody reads it.
void ReportInstallRefusal(std::optional<Consensus::RaftDriver::InstallRefusal> const& refusal,
                          ILogger& logger,
                          NodeConditions* conditions);

/// Whether a configuration change this node proposed is still in flight.
///
/// `RaftNode` refuses a second change while one is in flight, so a reconciler that
/// re-proposed every interval would log a refusal per interval for as long as
/// replication took. Waiting is therefore right — but only for as long as the
/// proposal can still be the one that lands.
///
/// **A term this node no longer holds is what ends the wait**, and that clause is the
/// whole reason this is a function rather than one comparison. A proposal made in a
/// term that has since moved is not in flight: it was never committed, an uncommitted
/// entry from a dead term is truncated by whoever leads next, and the index it landed
/// at may hold something else entirely or nothing at all. Comparing the remembered
/// index against the commit index ALONE, a node that proposed at index N, was deposed,
/// and was later elected again reads `N > commitIndex` forever and never proposes
/// again — so the joiner that change was going to admit is never counted, is excused
/// from every deadline for having no cluster, and the cluster silently loses the
/// ability to re-elect once one more member goes away
/// ([#388](https://github.com/LASTRADA-Software/fastcached/issues/388)).
///
/// The single symptom was one `Warn` naming an index that no longer exists.
///
/// Pure, and separated from the reconciler for that reason: the decision is four
/// values and the acquisition is a live cluster, and only one of those can be put
/// into a test.
/// @param proposedAt Where this node's last proposal landed, or a default index if none.
/// @param proposedIn The term it was made in.
/// @param commitIndex How far the log is committed now.
/// @param currentTerm The term this node is operating in now.
/// @return True while the proposal may still commit, so no new one should be made.
[[nodiscard]] constexpr bool QuorumProposalPending(Consensus::LogIndex proposedAt,
                                                   Consensus::Term proposedIn,
                                                   Consensus::LogIndex commitIndex,
                                                   Consensus::Term currentTerm) noexcept
{
    if (proposedIn != currentTerm)
        return false;

    return proposedAt > commitIndex;
}

/// The shape a node's consensus tier takes: its mode row's columns, read in one place.
struct ConsensusTierShape
{
    bool listens { true }; ///< Whether the Raft port is bound and a peer server accepts on it (`raftListener`).
    /// Which way the sessions this node dials flow (`dials`): `TwoWay` for a learner.
    Consensus::RaftWire::SessionDirection direction { Consensus::RaftWire::SessionDirection::OneWay };
};

/// The shape @p cfg's mode gives its consensus tier.
///
/// A configuration no record shaped listens one-way, as every node did before a mode decided: its
/// Raft surface resolves nothing, so the start refuses it by the surface's own sentence.
/// @param cfg The configuration, its formation applied.
/// @return The shape.
[[nodiscard]] ConsensusTierShape ConsensusTierShapeOf(NodeConfig const& cfg) noexcept;

/// What a node's formation is told by its consensus. Either may be empty, which a case that is not
/// about formation leaves it.
///
/// Both are PUSHED, for `ConsensusTier::RoleObserver`'s reason: the applied state and a signed
/// revocation arrive on consensus's threads, and the formation acts on the applied state alone
/// (`FormationController::OnClusterState`) -- never on a proposal or a leader's word.
struct FormationHooks
{
    /// Told every applied state and every move of who leads: the state, the cluster whose consensus
    /// applied it (the tier knows which; a state names none), who leads, and where the leader answers
    /// the `0xFC` port -- empty when nobody is known to lead or the state records no endpoint for it.
    /// Called on consensus's threads: it records and returns, and never calls back into the tier.
    std::function<void(Cluster::ClusterState const& state,
                       std::string_view clusterId,
                       std::optional<Consensus::NodeId> const& leader,
                       std::string_view leaderNodeEndpoint)>
        onState;

    /// Told that an acceptor that proved its id answered, SIGNED, that this node's own key is
    /// revoked -- how a learner offline through its own forget learns of it (#1555).
    Consensus::OwnKeyRevokedObserver onOwnKeyRevoked;
};

/// Who leads and when it last spoke to this node, as a reconcile pass reads the driver
/// (`ConsensusTier::LeaderContactObserver`).
///
/// RAW: it does not decide whether the leader counts. The driver's configuration is its ACTIVE one,
/// which an uncommitted entry can move, while the rule is a leader the APPLIED configuration counts
/// -- so that is asked by the roster, of the voters it applied. Pure, so the reading is pinned by a
/// test: while this node leads, an age of zero; otherwise the leader it names and how long ago the
/// driver last accepted contact from it -- an AGE, so the roster measures silence on its own clock.
/// @param progress The driver's progress, read together.
/// @param now The pass's instant, on the driver's steady clock.
/// @return The reading.
[[nodiscard]] Distributed::LeaderReading LeaderReadingOf(Consensus::RaftDriver::Progress const& progress,
                                                         core::platform::SteadyTimePoint now);

/// Consensus, running.
///
/// **A mode's ROW decides the tier's shape, and nothing here switches on the mode.** Its
/// `raftListener` column decides whether the Raft port is bound and a peer server accepts on it --
/// a learner listens for nobody -- and its `dials` column decides which way every session this node
/// dials flows: `TwoWay` for a learner, which nobody dials, so the voter it dials answers on the
/// same connection. Who is dialled is the seat's link column (`Cluster::LinkOfSeat`): a member
/// whose seat dials IN -- a learner -- is never dialled, by a voter or by another learner.
///
/// **What this replaces is the reason it exists.** Until now every node called
/// `SchedulerService::SetRole(Leader, {})` at startup — a placeholder that was said
/// out loud where it stood, and one whose consequence in a real fleet is that
/// *every* node believes it schedules. Two nodes handing out the same machine's
/// slots is not a degraded fleet; it is the one thing the architecture says only one
/// node may do at a time.
///
/// So this owns a `RaftDriver` and pushes what it decides into the two places that
/// need it: leadership into the scheduler, and the replicated member set into the
/// membership oracle. Both go through callbacks rather than being polled, because
/// the window between "the cluster agreed" and "this node acts on it" is a window in
/// which this node refuses a peer it has already admitted — which from the peer's
/// side is indistinguishable from being refused outright.
///
/// ## The reference chain, again
///
/// Storage, transport, state machine, driver, peer server, and two threads, each
/// holding the one before it. Owned as members for the reason `CacheTier` and
/// `SchedulerTier` are: in a function body their declaration order is load-bearing
/// and silently so, and getting it wrong is a dangling reference rather than a
/// compile error.
///
/// ## One reactor, and the reason it is one
///
/// The election timers and the peer port share a reactor and a thread. The first
/// version gave them one each, on the reasoning that `RaftDriver::Run` needs a
/// timer wheel while `RaftPeerServer::Run` blocks in `accept` -- and both halves
/// of that were wrong in a way nothing reported.
///
/// `core::async::syncRun` cannot drive a reactor: it resumes a coroutine once and throws if it
/// is still suspended, so a driver awaiting `SleepUntil` aborted the process the
/// first time three nodes were started. And a *blocking* listener makes every
/// `co_await` inside `RaftPeerServer` complete synchronously, so its
/// per-connection task runs inline and the accept loop serves one peer and never
/// accepts another -- in a three-node cluster, each node reads from one of its two
/// peers and nobody is ever elected, with nothing crashing and nothing logging a
/// fault.
///
/// So both loops are detached tasks on one `core::net::PlatformLoop`, which is what
/// `RaftPeerServer`'s own documentation always said it wanted. The reactor stops
/// when BOTH have finished rather than when somebody outside decides to:
/// `core::net::EventLoop::Run` returns with its timer heap and its parked work exactly where
/// they were, so a loop still suspended at that moment is a coroutine frame nobody
/// ever resumes and nobody ever frees.
class ConsensusTier final: public Distributed::IClusterAdmin, public IConsensusStandingSource
{
  public:
    /// Applied entries above the snapshot before the log is traded for one.
    ///
    /// A constant rather than a flag, deliberately. What this log carries is cluster
    /// membership and cluster settings — changes an operator makes by hand, so a
    /// fleet reaches this figure over months rather than minutes — and the state a
    /// snapshot replaces them with is a member list and a handful of strings. There
    /// is no deployment whose arithmetic comes out differently enough to tune, and a
    /// knob nobody turns is a knob that rots. Non-zero, though: a log nobody ever
    /// trims is a restart that re-reads its whole history every time, which is what
    /// the driver's own default of "never" would leave here.
    static constexpr std::uint64_t CompactAfterEntries { 512 };

    /// How often this node checks whether the cluster's state says what it knows.
    ///
    /// A poll rather than an edge, because the two things it reconciles arrive
    /// without one: leadership can move while a proposal is in flight, and a peer
    /// proves the key on a beacon interval of its own. A second is short against
    /// both -- an election already costs longer than that -- and the loop does
    /// nothing at all in the ordinary case where the state already agrees.
    static constexpr std::chrono::milliseconds ReconcileInterval { 1000 };

    /// Reconcile passes a proposed quorum change may wait before it is reported.
    ///
    /// Not a timeout: nothing is retried or abandoned, because a configuration entry
    /// cannot be withdrawn once appended. What it bounds is *silence*. The change
    /// that never commits is the one naming a member which will not accept this node
    /// as its leader — two nodes each bootstrapped as a cluster of one, discovering
    /// each other — and from both ends that looks exactly like a cluster which is
    /// simply busy. Thirty seconds is long against a replication round and short
    /// against an operator's patience.
    static constexpr std::uint32_t QuorumProposalPatience { 30 };

    /// Told this node's role whenever consensus changes it.
    ///
    /// The leader's endpoint travels with it, because `NotLeader` carries a redirect
    /// and a refusal that cannot say who to ask instead is one a client cannot act
    /// on. Empty means "nobody leads right now", which is a *different* fact from
    /// "somebody else does" and is what an election in progress looks like.
    ///
    /// A `string_view` rather than a `std::string`: every consumer forwards it
    /// straight to `SchedulerService::SetRole`, which takes a view, so a by-value
    /// parameter here would be a copy made once per role change purely to be read.
    ///
    /// The TERM travels with it since #322, because it goes inside every lease grant
    /// the scheduler mints: without it a token captured before an election stays
    /// replayable after one. Only the driver knows the term, which is why it is
    /// pushed rather than pulled -- and why a role that stays `Leader` across a term
    /// change is now a real announcement rather than a repeat of one.
    using RoleObserver =
        std::function<void(Distributed::SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t term)>;

    /// Told the cluster's member endpoints whenever they change.
    /// Told the cluster state at every commit.
    ///
    /// The STATE rather than the endpoint list since #1112: admission reads the
    /// `fleet-open` row as well as the members, and an observer handed only the
    /// endpoints is one the openness half has to reach by a second route somebody can
    /// forget. One commit, one call, both facts.
    using MembersObserver = std::function<void(Cluster::ClusterState const& state)>;

    /// Told, at every reconcile pass, who leads and how long ago it last spoke (`LeaderReadingOf`)
    /// -- raw, for the roster to judge against the configuration it APPLIED, and an age, for the
    /// roster to measure on its own clock. Called on the reconciler thread: it records and returns.
    using LeaderContactObserver = std::function<void(Distributed::LeaderReading const& reading)>;

    /// Start consensus, or explain why the node must not start.
    /// @param cfg The parsed configuration.
    /// @param advertised Where this node's `0xFC` port answers, read at every reconcile pass: the
    ///        endpoint its own record asserts (`ClusterMember::schedulerEndpoint`), so a reload of
    ///        `--advertise` reaches the record at the next pass this node leads. The one object the
    ///        worker registers under and NODE-ANNOUNCE names (`AnnouncedEndpoint`). Must outlive the
    ///        tier.
    /// @param identityKey This node's identity key, as the start resolved it out of the state
    ///        directory (#178). Taken rather than read again, so the key every peer connection
    ///        proves and the one the node announced are one reading of one file. Disengaged is
    ///        refused (`ConsensusNeedsIdentityKeyRefusal`).
    /// @param onRole Told this node's role; must outlive the tier.
    /// @param onMembers Told the member set; must outlive the tier.
    /// @param onLeaderContact Told each pass's reading of when a counted leader last spoke -- what
    ///        bounds how long a worker trusts the state it applied; must outlive the tier.
    /// @param metrics Where a refused peer connection is counted; must outlive the tier.
    /// @param logger Where progress and refusals are reported.
    /// @param conditions Where this tier answers its node conditions; null when nobody
    ///        reads them. Must outlive the tier.
    /// @param hooks What this node's formation is told; empty for a node whose formation does not
    ///        listen. Stated by every caller, never defaulted: a defaulted collaborator is how a new
    ///        production caller would drop the formation without a word.
    /// @param boundListener A socket already bound to the peer port the configuration names and
    ///        listening, served as it is instead of binding that port again; null binds it, which
    ///        is what a node does. For a caller that chose the port by binding it: releasing the
    ///        socket so the tier can bind the same number leaves an ephemeral port free for
    ///        anything else on the host to take in between. Owned from here on, on every path --
    ///        a start refused before the listener is served closes it.
    /// @return The running tier, or the fatal reason.
    [[nodiscard]] static std::expected<std::unique_ptr<ConsensusTier>, NodeRefusal> Start(
        NodeConfig const& cfg,
        Cc::IAdvertisedEndpointSource const& advertised,
        std::optional<Ed25519KeyPair> const& identityKey,
        RoleObserver onRole,
        MembersObserver onMembers,
        LeaderContactObserver onLeaderContact,
        IMetricsSink& metrics,
        ILogger& logger,
        NodeConditions* conditions,
        FormationHooks hooks,
        std::unique_ptr<BlockingListener> boundListener = nullptr);

    ConsensusTier(ConsensusTier const&) = delete;
    ConsensusTier& operator=(ConsensusTier const&) = delete;
    ConsensusTier(ConsensusTier&&) = delete;
    ConsensusTier& operator=(ConsensusTier&&) = delete;

    /// Stops every loop and joins every thread.
    ///
    /// A destructor rather than a `Shutdown()` somebody has to remember at every
    /// return path — the lesson `AdminEndpoint` records. The peer server's listener
    /// is closed before its thread is joined, because POSIX does not unblock a
    /// parked `accept()`.
    ~ConsensusTier() override;

    /// Propose a change to the cluster's state.
    ///
    /// Refused unless this node leads, and refused for a command `Validate` rejects
    /// — which is the only place a change CAN be refused, since applying happens
    /// after commitment when there is nobody left to report to.
    /// @param command The change.
    /// @return The index it was appended at, or why it was not.
    [[nodiscard]] std::expected<Consensus::LogIndex, ConsensusError> Propose(Cluster::Command const& command);

    /// The cluster's state as this node last applied it.
    ///
    /// By value, because the applying thread is not this one: see
    /// `ClusterStateMachine::State`.
    [[nodiscard]] Cluster::ClusterState ClusterState() const override;

    /// What this node believes about its OWN consensus configuration.
    ///
    /// Deliberately a different question from `ClusterState()`, and confusing the
    /// two is how the gap #435 records came about: that reports the FLEET's member
    /// record from `ClusterStateMachine`, which a leader answers and which is a
    /// different set from the quorum. This one is the quorum, answered by whoever is
    /// asked, leader or not — because the node whose view matters when a cluster
    /// will not re-elect is precisely the one that redirects you.
    ///
    /// A snapshot taken under the driver's lock, so the role, the term, the member
    /// set and the commit index describe one moment rather than four.
    /// @return This node's consensus state.
    [[nodiscard]] ConsensusStatus Status() const;

    /// Where this node sits in the configuration consensus holds (#1449).
    ///
    /// Asked of `Consensus::Membership::StandingOf` with this node's own id, so the
    /// answer `--node-status` reports and the one `RaftNode` acts on have one author.
    /// @return The standing; always engaged, since a tier that exists runs consensus.
    [[nodiscard]] std::optional<Consensus::Standing> CurrentStanding() const override;

    /// Whether this node has applied the log it recovered when the tier started (`AppliedStateOf`),
    /// from ONE read of the driver, so the applied and last indices describe one moment.
    /// @return `Behind` or `CaughtUp`.
    [[nodiscard]] AppliedStateReading CurrentAppliedState() const override;

    /// Offer a change to the cluster, discarding where it landed.
    ///
    /// The `IClusterAdmin` spelling of `Propose`. The index is dropped because the
    /// caller is an operator surface and an index is not something an operator can
    /// act on -- what they do next is ask for the state again either way.
    /// @param command The change.
    /// @return Nothing, or why it was refused.
    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override;

    /// @copydoc Distributed::IClusterAdmin::NoteAnnouncedEndpoint
    ///
    /// Kept, latest per member, until the next pass this node leads, which re-proposes the member's
    /// record when it differs (`Cluster::AnnouncedEndpointDesires`). Any thread.
    void NoteAnnouncedEndpoint(Consensus::NodeId const& member, std::string endpoint) override;

    /// Add to what this node believes the membership should include.
    ///
    /// Additive and idempotent: a record already desired with the same content is
    /// dropped, and one whose content differs replaces it, so a caller may hand
    /// over the same peers on every pass of its own loop without growing anything.
    /// Nothing is proposed here -- the reconciler does that when this node leads,
    /// which is the separation discovery's own documentation insists on: it answers
    /// who proved the key and where they answer, and a caller proposes.
    /// @param records Members this node knows should be present.
    void Desire(std::span<Cluster::DesiredMember const> records);

    /// This node's own record, as it announces it.
    ///
    /// Exposed because discovery announces the same consensus endpoint on the
    /// segment that consensus dials, and deriving it twice is how the beacon and
    /// the transport come to disagree about where this node answers.
    [[nodiscard]] Cluster::ClusterMember const& Self() const noexcept
    {
        return _self;
    }

    /// This node's key material and the roster's keys, as the Raft peer wire proves them.
    ///
    /// Exposed because discovery proves the SAME key on the segment (#178): a beacon signed with
    /// one reading of the key file and a handshake signed with another could disagree about who
    /// this node is, and a proof classified against a second copy of the roster could disagree
    /// about who everybody else is. One object answers both.
    /// @return The keys; valid for as long as this tier.
    [[nodiscard]] Consensus::IRaftPeerKeys const& Keys() const noexcept
    {
        return _roster;
    }

    /// The address this node's peer port bound; EMPTY for a mode whose row binds none -- a learner,
    /// which dials every voter and is answered on that session.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

    /// Whether the peer port's accept loop is degraded, or has stopped while this tier was not
    /// shutting down.
    ///
    /// The tier's own, because it runs its own reactor; `main` forwards it into the node's one
    /// registry (`core::net::AcceptLoopHealth::forward`), which the liveness probe and the condition read.
    /// @return The registry the peer server reports to.
    [[nodiscard]] core::net::AcceptLoopHealth& AcceptLoops() noexcept
    {
        return _acceptLoops;
    }

    /// Which way the sessions this node dials flow: its mode row's `dials` column.
    /// @return `TwoWay` for a learner, `OneWay` otherwise.
    [[nodiscard]] Consensus::RaftWire::SessionDirection Direction() const noexcept;

    /// Every member this node dials now, self excluded, sorted. Read off the transport, so it is
    /// the dial list itself rather than a second account of it.
    /// @return The ids.
    [[nodiscard]] std::vector<Consensus::NodeId> DialTargets() const;

  private:
    ConsensusTier(Cluster::ClusterMember self,
                  Cc::IAdvertisedEndpointSource const& advertised,
                  Consensus::FileRaftStorage storage,
                  Ed25519KeyPair identityKey,
                  std::span<Cluster::MemberSpec const> knownMembers,
                  std::string boundEndpoint,
                  RoleObserver onRole,
                  MembersObserver onMembers,
                  LeaderContactObserver onLeaderContact,
                  std::string clusterId,
                  IMetricsSink& metrics,
                  ILogger& logger,
                  NodeConditions* conditions,
                  FormationHooks hooks);

    /// Bind the Raft port on this node's reactor, or serve the one a caller handed over already
    /// bound, or explain why neither can be.
    /// @param bind Where to bind, and where a handed listener must be bound.
    /// @param boundListener The peer port already bound, or null to bind it; see `Start`.
    /// @return Nothing on success, or the fatal reason.
    [[nodiscard]] std::expected<void, NodeRefusal> Listen(SurfaceEndpoint const& bind,
                                                          std::unique_ptr<BlockingListener> boundListener);

    /// Build the driver and start both loops.
    ///
    /// Split from `Start` because it needs the constructed object -- the driver holds
    /// references to members that do not exist until the constructor has run, which
    /// is the same reason the reference chain is member-ordered rather than local.
    /// @param cfg The parsed configuration.
    /// @param dialable Every peer this node knows an address for. What the
    ///        transport is built from, and **not** the same list as @p bootstrap:
    ///        a node waiting to be admitted has to be able to answer the leader
    ///        that admits it, and cannot learn where that leader is until it has
    ///        answered one. Everything it needs is on its own command line, and
    ///        nothing else can supply it.
    /// @param bootstrap The member set consensus starts under; empty for a node
    ///        waiting to be admitted.
    /// @param bind Where the peer port binds; DISENGAGED for a mode whose row binds none, which
    ///        then runs no peer server either.
    /// @param direction Which way the sessions this node dials flow.
    /// @param boundListener The peer port already bound, or null to bind it; see `Start`.
    /// @return Nothing on success, or the fatal reason.
    [[nodiscard]] std::expected<void, NodeRefusal> Launch(NodeConfig const& cfg,
                                                          std::vector<Cluster::MemberSpec> const& dialable,
                                                          std::vector<Cluster::MemberSpec> const& bootstrap,
                                                          std::optional<SurfaceEndpoint> const& bind,
                                                          Consensus::RaftWire::SessionDirection direction,
                                                          std::unique_ptr<BlockingListener> boundListener);

    /// Translate a consensus role into the scheduler's, and pass it on.
    ///
    /// Called by the driver whenever the role actually moves -- from the timer thread
    /// for an election timeout, from a peer reader for a message that deposed this
    /// node. It does the one thing consensus cannot: turn a leader's *id* into the
    /// *address* a client redirects to, which only the replicated state knows.
    /// It is also where a deposition is reported, because the driver's report is
    /// the only thing that carries *why* -- and the answer is gone by the time
    /// anyone could ask the node.
    /// @param change What consensus says this node is now, and what moved it.
    void PublishRole(Consensus::RaftDriver::RoleChange const& change);

    /// Tell the scheduler what this node is, if that has changed since last time.
    ///
    /// Called both when consensus moves the role and when the replicated state
    /// moves, because the two answers arrive separately and the second one is the
    /// address. A node announces its own record once it is elected, so the entry
    /// carrying the leader's scheduler endpoint commits strictly AFTER the role
    /// change that provoked it -- and a follower that only ever heard the first of
    /// those answers `NotLeader` with nothing for the rest of the term, which a
    /// client cannot tell from an election in progress and answers by compiling
    /// locally. Every time.
    void Republish();

    /// Tell the formation the applied state, who leads, and where the leader answers.
    /// @param state The state this node's consensus applied last.
    void TellFormation(Cluster::ClusterState const& state);

    /// Act on a change to the replicated state.
    /// @param state What the cluster now holds.
    void OnStateChanged(Cluster::ClusterState const& state);

    /// Propose whatever the replicated state does not yet say, if this node may.
    ///
    /// A loop of its own rather than a call from the role observer, and that is
    /// forced rather than stylistic. The observer runs inside `RaftDriver::Deliver`,
    /// so proposing from it re-enters the driver from within itself -- and
    /// `RaftDriver::RoleObserver` says in as many words that an observer must not
    /// block, being on the path that still has to send the next heartbeat.
    void Reconcile();

    /// Teach the transport where every member of the cluster answers.
    ///
    /// Runs on **every** node rather than only the leader, and that is the whole
    /// difference between this and everything else the reconciler does. A member is
    /// admitted by one node and has to be dialled by all of them: the leader
    /// replicates to it, and every other member sends it votes. A follower that
    /// waited to become leader before learning an address would be a follower whose
    /// votes go nowhere.
    ///
    /// It also tells the transport which members dial in rather than being dialled, read from
    /// each member's seat through the link column (`Cluster::LinkOfSeat`), so a message for a
    /// learner with no session is counted as that and never as a peer nothing can reach.
    /// @param state The cluster's state as this node last applied it.
    /// @param desired What this node believes should be present, snapshotted once
    ///        by the caller: taking it twice in one pass would let discovery land
    ///        between, so a leader would dial one set and propose from another.
    void LearnMembers(Cluster::ClusterState const& state, std::span<Cluster::DesiredMember const> desired);

    /// Say, once per member, that a desire was refused because the cluster forgot its id
    /// and revoked its key.
    ///
    /// The refusal itself is `Cluster::MembershipProposals`'s (#1528, #1555); this is only
    /// what makes it visible, since a refused desire and one the state already matches
    /// both propose nothing. The revoked key is named, because it is what the machine is
    /// forgotten by, wherever it now dials from. Reconciler thread only.
    /// @param state The state the plan was made against.
    /// @param refused The desires this pass's plan refused.
    void ReportForgottenDesires(Cluster::ClusterState const& state, std::span<Cluster::DesiredMember const> refused);

    /// Move the quorum one step towards the cluster's member set.
    ///
    /// Leader only, and one member at a time -- see `Cluster::NextQuorumChange` for
    /// which step and why. Called after `LearnMembers`, because a member counted
    /// towards a quorum before anything can dial it is a cluster that stops forming
    /// one.
    /// @param state The cluster's state as this node last applied it.
    /// Say what this node counts as its consensus configuration, when it changes.
    ///
    /// Every node, not only a leader: there is no other way to learn what a given
    /// node believes its own quorum to be. `--cluster-status` reports the fleet's
    /// member record and is answered by the leader alone, and no counter carries it
    /// (#435).
    ///
    /// Reconciler thread only.
    void ReportQuorum();

    void ReconcileQuorum(Cluster::ClusterState const& state);

    /// Say which recorded voters consensus is holding as learners until they catch up.
    ///
    /// The wait is `Cluster::NextQuorumChange`'s (#1537); this names it where an
    /// operator reads, once when it starts and at Warn once it has lasted
    /// `QuorumProposalPatience` passes. Reconciler thread only.
    /// @param waiting The members this pass's plan named.
    /// @param progress The read the plan was made from, for the indices it quotes.
    void ReportCatchingUp(std::span<Consensus::NodeId const> waiting, Consensus::RaftDriver::Progress const& progress);

    /// Record that one of the two reactor loops has ended.
    ///
    /// The second one to call this stops the reactor. Public to the class only --
    /// it is called from the detached tasks `Launch` submits, which are lambdas
    /// rather than members and so reach it through a pointer.
    void NoteLoopFinished() noexcept;

    ILogger& _logger;

    // Declaration order IS construction order, and each is referenced by the one
    // below it. This is the ordering the class exists to make the language check.

    /// The clock the reactor stamps with, and the one `DriverSink` reads.
    ///
    /// They have to be the same kind: `DriverSink` reads `steady_clock` directly,
    /// so a reactor on a wall clock would put a received message and a fired timer
    /// on two timelines, and an NTP step would look like an election timeout.
    core::platform::SteadyClock _clock;

    /// Constructed here and RUN on `_ioThread`, which is allowed and is what the
    /// daemon's multi-reactor path already does: the descriptor is made in the
    /// constructor and the loop is a separate call.
    core::net::PlatformLoop _reactor { _clock };

    Consensus::FileRaftStorage _storage;

    /// Election timeouts.
    std::unique_ptr<IRandomSource> _random;

    /// Every peer connection's handshake nonce: the operating system's generator, and NOT
    /// `_random`, whose seeded engine repeats wherever its seed does (#1527).
    SystemSecureRandom _nonces;

    /// Where a refused peer connection is counted.
    IMetricsSink& _metrics;

    /// This node's identity key, and what the cluster says every other member proves itself
    /// with (#178): the members this node's command line names, until the replicated state
    /// says otherwise, which it does from `OnStateChanged` on every applied change. Declared
    /// before `_identity` and before the transport and the server, which all hold a reference
    /// into it, and before `_application`, whose apply callback writes it.
    Cluster::RosterKeys _roster;

    /// Who this node is on the Raft peer wire: its id, proved with `_roster`'s own key, and
    /// every peer's proof and verdict checked against `_roster`'s keys.
    Consensus::RaftPeerIdentity _identity;

    /// Name resolution for the peer dials, off the reactor thread.
    ///
    /// Declared before `_connector`, which holds a reference to it. Threaded
    /// rather than inline because `getaddrinfo` takes no timeout: a peer named by
    /// hostname whose resolver is wedged would otherwise park the reactor -- and
    /// this reactor also carries the election timers and every peer reader, so
    /// that is the whole cluster's liveness. A peer named by literal address,
    /// which is the ordinary case, never reaches a thread at all.
    core::net::ThreadedAddressResolver _resolver;

    /// Reactor-driven, so a dial suspends rather than blocking the loop that
    /// carries the election timers. Declared after `_reactor` and `_resolver`
    /// because it references both.
    std::unique_ptr<core::net::IConnector> _connector;

    /// Where the transport delivers what an acceptor writes back on a two-way session. The
    /// transport is built before the driver it delivers into, so this is bound to `_sink` once
    /// that exists, before the transport starts. Declared before `_transport`, which holds it.
    Consensus::ForwardingSink _inbound;

    std::unique_ptr<Consensus::RaftPeerTransport> _transport;
    Cluster::ClusterStateMachine _application;
    std::unique_ptr<Consensus::RaftDriver> _driver;
    /// The last index the log held when the driver was built: what this node recovered, and what its
    /// applied state must reach before an absent key means an absent member (`CurrentAppliedState`).
    Consensus::LogIndex _recoveredLastIndex {};
    std::unique_ptr<core::net::IListener> _listener;
    std::unique_ptr<Consensus::IRaftMessageSink> _sink;
    core::net::AcceptLoopHealth _acceptLoops; ///< Declared before the server that reports to it.
    std::unique_ptr<Consensus::RaftPeerServer> _peerServer;

    RoleObserver _onRole;
    std::string _boundEndpoint;

    /// This node's own record, which nothing else can supply.
    ///
    /// Its `0xFC` endpoint is the half nobody else knows: peers learn a node's consensus
    /// address by dialing it, and there is no equivalent for a port they never connect to.
    /// So a node asserts its own -- read from `_advertised` at every pass, never kept here --
    /// and every other member's arrives by a PROVEN announcement (`NoteAnnouncedEndpoint`).
    Cluster::ClusterMember _self;

    /// Where this node's `0xFC` port answers now: what its own record asserts.
    Cc::IAdvertisedEndpointSource const& _advertised;

    /// Told the member set whenever it changes; may be empty.
    MembersObserver _onMembers;

    /// Told each reconcile pass's leader-contact reading; may be empty.
    LeaderContactObserver _onLeaderContact;

    /// Where this tier answers `unreadable-leader-snapshot`; null when nobody reads it.
    NodeConditions* _conditions;

    /// The fleet this node runs in: the formation record's cluster id, as the formation is told it.
    std::string _clusterId;

    /// What this node's formation is told; either half may be empty.
    FormationHooks _hooks;

    /// What consensus last said, so a state change can be re-read against it.
    ///
    /// The term is carried for the log line rather than for any decision: a role
    /// without one says leadership moved and nothing about how often, which is the
    /// difference between a dump that can be read after an intermittent election
    /// and one that cannot.
    Consensus::Role _lastRole { Consensus::Role::Follower };
    Consensus::Term _lastTerm {};
    std::optional<Consensus::NodeId> _lastLeader;

    /// What the scheduler was last told, so an unchanged answer is not re-announced.
    ///
    /// Both halves, because either can move on its own: a role change with the same
    /// leader endpoint is a real change, and so is the endpoint arriving for a
    /// leader this node already knew about.
    Distributed::SchedulerRole _publishedRole { Distributed::SchedulerRole::Undecided };
    std::string _publishedEndpoint;

    /// What the LOG was last told, which is deliberately a different question.
    ///
    /// The scheduler needs to hear a role and an address and has no use for a term;
    /// a reader of the log needs the term most of all, because three roles collapse
    /// to two here and a node campaigning round after round is `Undecided` with no
    /// endpoint every single time. Sharing one suppression test between the two
    /// makes the storm silent to keep the scheduler quiet.
    Consensus::Term _publishedTerm {};

    /// Whether the scheduler has been told anything at all yet.
    ///
    /// The first publish must happen even when it announces `Undecided`, and that
    /// is not a formality: `SchedulerService` starts as a STANDALONE leader --
    /// correct for a node with no `--node-id`, and wrong the instant there is a
    /// cluster. Suppressing the first announcement because it equalled this
    /// object's own initial value left the service leading, so all three nodes of
    /// a fresh cluster answered as leader at once. Which is the exact failure the
    /// consensus tier exists to prevent.
    bool _published { false };

    /// Everything this node believes should be a member, `_self` included.
    ///
    /// Written by whoever discovers a peer and read by the reconciler thread, which
    /// are different threads by construction -- hence the mutex, held only across
    /// the vector operations and never across a proposal.
    mutable std::mutex _desiredMutex;
    std::vector<Cluster::DesiredMember> _desired;

    /// The latest `0xFC` endpoint each member PROVED and announced to this node's scheduler;
    /// guarded by `_desiredMutex`. Dropped at every change of leadership (`PublishRole`): a follower
    /// is told nothing, and a stale map must not outlive the leadership it was announced to.
    Cluster::AnnouncedEndpointMap _announced;

    /// Where a member's endpoint re-proposal landed, and in which term.
    struct EndpointProposal
    {
        Consensus::LogIndex at; ///< The proposal's index.
        Consensus::Term in;     ///< The term it was made in.
    };

    /// Each member whose endpoint re-proposal has not committed, so at most one is in flight per
    /// member (`QuorumProposalPending` decides). Reconciler thread only; dropped at every change of
    /// leadership, seen through `_leadershipChanges`.
    std::map<Consensus::NodeId, EndpointProposal, std::less<>> _endpointsInFlight;

    /// How many times this node gained or lost leadership: bumped at the transition (`PublishRole`),
    /// read by the reconciler, which drops `_endpointsInFlight` when it moved since its last pass.
    std::atomic<std::uint64_t> _leadershipChanges { 0 };

    /// The `_leadershipChanges` the in-flight map was last kept under. Reconciler thread only.
    std::uint64_t _inFlightLeadership { 0 };

    /// The member ids this node was started with, which it never proposes removing.
    ///
    /// What tells "the operator forgot this member" apart from "nobody ever wrote it
    /// down" -- see `Cluster::NextQuorumChange`, which shrinks a healthy cluster to
    /// one node without it. Taken from the command line rather than from what this
    /// process has observed, because the difference is a restart: an observation is
    /// rebuilt from live members only, so a fleet restarted after a removal would
    /// count a forgotten member forever.
    ///
    /// Empty for a node that joined a fleet, which asserts nothing about who
    /// is a member -- so every member of the cluster it joins is one it may later be
    /// told to forget.
    std::vector<Consensus::NodeId> _bootstrapIds;

    /// The desires already reported as refused for a forgotten host (#1528).
    ///
    /// So each is said once while it stays refused, rather than once per pass for as
    /// long as the forgotten machine goes on proving the key. Reconciler thread only.
    std::vector<Consensus::NodeId> _reportedForgotten;

    /// Recorded voters held as learners until they catch up, and for how many passes
    /// (#1537). What `ReportCatchingUp` says once and escalates once. Reconciler thread
    /// only.
    std::unordered_map<Consensus::NodeId, std::uint32_t> _catchingUp;

    /// Where the last configuration change this node proposed landed.
    ///
    /// Read against the commit index to answer one question: has the previous change
    /// been agreed? `RaftNode` refuses a second while one is in flight, so without
    /// this the reconciler would propose one per interval and log a refusal per
    /// interval for as long as replication took. What it also catches is the case
    /// that never resolves -- a configuration naming a member that will never
    /// acknowledge this leader -- which is otherwise completely silent, so the wait
    /// is reported once it becomes unreasonable rather than left to be inferred.
    ///
    /// Reconciler thread only; nothing else reads it.
    Consensus::LogIndex _quorumProposedAt {};

    /// What this node last said it counts, and whether it has said anything yet.
    ///
    /// The flag is not redundant with an empty configuration: a node waiting to be admitted
    /// legitimately counts nobody, so "empty" is a real reading rather than the
    /// absence of one, and reporting only on a CHANGE would leave that state silent
    /// and indistinguishable from a loop that never ran. Reconciler thread only.
    Consensus::Configuration _reportedConfiguration;

    /// Whether `ReportQuorum` has said anything yet.
    bool _quorumReported { false };

    /// The term that proposal was made in.
    ///
    /// Kept beside the index because the index alone cannot say whether the wait is
    /// still meaningful — see `QuorumProposalPending`, which is where the reasoning
    /// lives. Reconciler thread only.
    Consensus::Term _quorumProposedIn {};

    /// How many passes the current proposal has been waiting, for that one report.
    std::uint32_t _quorumWaited { 0 };

    /// Whether this node may propose at all, as the observer last reported it.
    ///
    /// Read by the reconciler and written by the driver's threads, so it is atomic
    /// -- and it is a cache of the driver's answer rather than a second opinion: a
    /// stale read costs one refused proposal, which `Propose` reports and the next
    /// pass repeats.
    std::atomic<bool> _leads { false };

    /// How many of the reactor loops `Start` submitted are still running: set from the list it
    /// submits, never restated -- a mode that binds no Raft port runs one, every other two.
    ///
    /// Whichever finishes last stops the reactor, so `Run()` never returns while a
    /// coroutine is still parked on it -- which would leak that frame outright.
    std::atomic<int> _loopsRunning { 0 };

    // Started last and joined first, which the member order gives for free.
    std::jthread _ioThread;
    std::jthread _reconcileThread;
};

/// The consensus reader a `/metrics` scrape uses, disengaged when this node runs none.
///
/// A function rather than a ternary in `WorkerBody`, for the reason
/// `StartConsensusOrExplain` below is one: spelled inline it took `WorkerBody` to a
/// cognitive complexity of 61 against clang-tidy's threshold of 60 -- a build
/// failure, since `WarningsAsErrors` is `*` -- and `main.cpp` is in no test target.
///
/// What it encodes is that *this node runs no consensus* is a fact about the NODE,
/// while an empty `std::function` is merely how a scrape spells it. Deciding it here
/// keeps the absence in one place instead of in a call site that is already
/// assembling five other fields, and it is the branch that makes the scrape say NO
/// cluster rather than a cluster of nobody.
/// @param tier This node's consensus tier, or null when it runs no consensus.
/// @return A reader of that tier's status, or an empty function when there is none.
[[nodiscard]] std::function<ConsensusStatus()> ConsensusScrapeSource(ConsensusTier const* tier);

/// What the consensus tier tells about the fleet's shared cache, at every applied state, in this
/// order: where the shared cache is, whether this machine serves it, and then the private tier's
/// upstream, which re-judges what it reports from the directory it was just told.
///
/// One record, so the three cannot be transposed at the call site; the order they are told in lives
/// in `ApplySharedCacheState`, which a harness with no host calls too.
struct SharedCacheListeners
{
    SharedCacheDirectory& directory; ///< Where the shared cache is.
    SharedCacheHost& host;           ///< Whether this machine serves it.
    ICacheUpstream* upstream;        ///< The private tier's upstream; null on a node that keeps no objects.

    /// Tell each of them @p state, in order.
    /// @param state The state the cluster just applied.
    void Applied(Cluster::ClusterState const& state) const
    {
        ApplySharedCacheState(state, directory, &host, upstream);
    }
};

/// Start consensus when the operator configured a cluster, wiring it to the node.
///
/// A function rather than four lines in `WorkerBody`, for the reason
/// `StartCacheTierOrExplain` is one: it is a coherent decision with one answer, it
/// pushed `WorkerBody` past clang-tidy's cognitive-complexity limit inline, and
/// `main.cpp` is in no test target. What it encodes is which of this node's parts
/// consensus drives -- the scheduler's role, the fleet's membership,
/// and the roster every grant is verified against -- and that list is the thing worth
/// reading in one place.
///
/// A null result is success: it means no `--listen-raft`, so this node runs no consensus --
/// and therefore no scheduler, since #178 makes every scheduler a consensus member. That is the
/// ordinary shape of a pure worker, not a degraded one.
/// @param cfg The parsed configuration.
/// @param schedulerTier Told this node's role; may be null when it serves none.
/// @param advertised Where this node's `0xFC` port answers now; see `ConsensusTier::Start`.
///        Must outlive the tier.
/// @param identityKey This node's identity key, as the start resolved it; see
///        `ConsensusTier::Start`.
/// @param membership Told the replicated member set; must outlive the tier.
/// @param roster Told every applied state (#178); must outlive the tier.
/// @param schedulers Told every applied state: where this node's worker and presence loop register
///        next, so a voter that moved its `0xFC` endpoint is reached without a reform. Must outlive
///        the tier.
/// @param schedulingLeader Told who leads at every role change and how long it has been silent at
///        every pass, on every node that runs consensus whether or not it schedules: what a node
///        running no scheduler redirects a launcher's scheduling verbs to (#1639). Must outlive
///        the tier.
/// @param sharedCache Told every applied state: the directory and the host by reference, since every
///        node builds both and a consensus node that told them nothing would neither find nor serve
///        the tier the cluster named; the upstream where there is one. Each must outlive the tier.
/// @param metrics Where a refused peer connection is counted; must outlive the tier.
/// @param logger Where progress and refusals are reported.
/// @param conditions Where the tier answers its node conditions; null when nobody reads them.
/// @param hooks What this node's formation is told; see `ConsensusTier::Start`.
/// @return The tier, a null tier meaning "no cluster configured", or the fatal reason.
[[nodiscard]] std::expected<std::unique_ptr<ConsensusTier>, NodeRefusal> StartConsensusOrExplain(
    NodeConfig const& cfg,
    std::unique_ptr<SchedulerTier> const& schedulerTier,
    Cc::IAdvertisedEndpointSource const& advertised,
    std::optional<Ed25519KeyPair> const& identityKey,
    NodeMembership& membership,
    NodeRoster& roster,
    AppliedSchedulers& schedulers,
    SchedulingLeaderPublisher& schedulingLeader,
    SharedCacheListeners sharedCache,
    IMetricsSink& metrics,
    ILogger& logger,
    NodeConditions* conditions,
    FormationHooks hooks);

} // namespace FastCache::Node
