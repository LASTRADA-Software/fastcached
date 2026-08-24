// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeMembership.hpp"
#include "SchedulerTier.hpp"

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/ClusterStateMachine.hpp>
#include <FastCache/Cluster/MembershipPolicy.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Net/PlatformListener.hpp>
#include <FastCache/Net/ThreadedAddressResolver.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace FastCache::Node
{

/// One `id=host:port` peer specification, parsed.
///
/// Exposed rather than hidden in the `.cpp` because the rule it applies is worth
/// checking and `main.cpp` -- the only other place it could live -- is in no test
/// target. What it decides is which half of the token is the identity consensus
/// counts and which is the address peers dial, and getting that wrong produces a
/// cluster whose members never match a vote.
/// @param spec The token as an operator wrote it.
/// @return The member, or nullopt when the token is not one.
[[nodiscard]] std::optional<Cluster::ClusterMember> ParsePeerSpec(std::string_view spec);

/// Where clients should be told to reach this node's scheduler.
///
/// The HOST comes from this node's own consensus endpoint and the PORT from what
/// the scheduler surface actually bound, and neither half can supply the other. A
/// scheduler that bound `0.0.0.0:7000` -- which is its default, because peers are
/// on other machines by definition -- names no address a client can dial. The
/// consensus endpoint is dialable by construction, since every peer opens a socket
/// to it, and names the wrong port.
///
/// Exposed rather than hidden in the `.cpp` for the reason `ParsePeerSpec` is: the
/// rule is worth checking, and the alternative home is `main.cpp`, which is in no
/// test target.
/// @param raftEndpoint This node's consensus endpoint, as its peers dial it.
/// @param schedulerBound What the scheduler surface bound, empty when it serves
///        none.
/// @return The endpoint to advertise, empty when there is nothing to advertise.
[[nodiscard]] std::string AdvertisedSchedulerEndpoint(std::string_view raftEndpoint, std::string_view schedulerBound);

/// Consensus, running.
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
/// `SyncRun` cannot drive a reactor: it resumes a coroutine once and throws if it
/// is still suspended, so a driver awaiting `SleepUntil` aborted the process the
/// first time three nodes were started. And a *blocking* listener makes every
/// `co_await` inside `RaftPeerServer` complete synchronously, so its
/// per-connection task runs inline and the accept loop serves one peer and never
/// accepts another -- in a three-node cluster, each node reads from one of its two
/// peers and nobody is ever elected, with nothing crashing and nothing logging a
/// fault.
///
/// So both loops are detached tasks on one `PlatformReactor`, which is what
/// `RaftPeerServer`'s own documentation always said it wanted. The reactor stops
/// when BOTH have finished rather than when somebody outside decides to:
/// `IReactor::Run` returns with its timer heap and its parked work exactly where
/// they were, so a loop still suspended at that moment is a coroutine frame nobody
/// ever resumes and nobody ever frees.
class ConsensusTier final: public Distributed::IClusterAdmin
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
    using RoleObserver = std::function<void(Distributed::SchedulerRole role, std::string_view leaderEndpoint)>;

    /// Told the cluster's member endpoints whenever they change.
    using MembersObserver = std::function<void(std::vector<std::string> const& endpoints)>;

    /// Start consensus, or explain why the node must not start.
    /// @param cfg The parsed configuration.
    /// @param schedulerBound What this node's scheduler surface bound, empty when
    ///        it serves none. Only the PORT is taken from it; see
    ///        `AdvertisedSchedulerEndpoint` for why the host cannot be.
    /// @param onRole Told this node's role; must outlive the tier.
    /// @param onMembers Told the member set; must outlive the tier.
    /// @param logger Where progress and refusals are reported.
    /// @return The running tier, or the fatal reason.
    [[nodiscard]] static std::expected<std::unique_ptr<ConsensusTier>, std::string> Start(NodeConfig const& cfg,
                                                                                          std::string_view schedulerBound,
                                                                                          RoleObserver onRole,
                                                                                          MembersObserver onMembers,
                                                                                          ILogger& logger);

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

    /// Offer a change to the cluster, discarding where it landed.
    ///
    /// The `IClusterAdmin` spelling of `Propose`. The index is dropped because the
    /// caller is an operator surface and an index is not something an operator can
    /// act on -- what they do next is ask for the state again either way.
    /// @param command The change.
    /// @return Nothing, or why it was refused.
    [[nodiscard]] std::expected<void, ConsensusError> ProposeToCluster(Cluster::Command const& command) override;

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

    /// The address this node's peer port bound.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

  private:
    ConsensusTier(Cluster::ClusterMember self,
                  Consensus::FileRaftStorage storage,
                  std::string boundEndpoint,
                  RoleObserver onRole,
                  MembersObserver onMembers,
                  ILogger& logger);

    /// Build the driver and start both loops.
    ///
    /// Split from `Start` because it needs the constructed object -- the driver holds
    /// references to members that do not exist until the constructor has run, which
    /// is the same reason the reference chain is member-ordered rather than local.
    /// @param cfg The parsed configuration.
    /// @param members The bootstrap member set, already parsed and validated.
    /// @param bindAddress Where the peer port binds.
    /// @param bindPort The peer port.
    /// @return Nothing on success, or the fatal reason.
    [[nodiscard]] std::expected<void, std::string> Launch(NodeConfig const& cfg,
                                                          std::vector<Cluster::ClusterMember> const& members,
                                                          std::string_view bindAddress,
                                                          std::uint16_t bindPort);

    /// Translate a consensus role into the scheduler's, and pass it on.
    ///
    /// Called by the driver whenever the role actually moves -- from the timer thread
    /// for an election timeout, from a peer reader for a message that deposed this
    /// node. It does the one thing consensus cannot: turn a leader's *id* into the
    /// *address* a client redirects to, which only the replicated state knows.
    /// @param role What consensus says this node is.
    /// @param knownLeader Who it believes leads, if anybody.
    void PublishRole(Consensus::Role role, std::optional<Consensus::NodeId> const& knownLeader);

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
    SteadyClock _clock;

    /// Constructed here and RUN on `_ioThread`, which is allowed and is what the
    /// daemon's multi-reactor path already does: the descriptor is made in the
    /// constructor and the loop is a separate call.
    PlatformReactor _reactor { _clock };

    Consensus::FileRaftStorage _storage;
    std::unique_ptr<IRandomSource> _random;

    /// Name resolution for the peer dials, off the reactor thread.
    ///
    /// Declared before `_connector`, which holds a reference to it. Threaded
    /// rather than inline because `getaddrinfo` takes no timeout: a peer named by
    /// hostname whose resolver is wedged would otherwise park the reactor -- and
    /// this reactor also carries the election timers and every peer reader, so
    /// that is the whole cluster's liveness. A peer named by literal address,
    /// which is the ordinary case, never reaches a thread at all.
    ThreadedAddressResolver _resolver;

    /// Reactor-driven, so a dial suspends rather than blocking the loop that
    /// carries the election timers. Declared after `_reactor` and `_resolver`
    /// because it references both.
    std::unique_ptr<IConnector> _connector;
    std::unique_ptr<Consensus::RaftPeerTransport> _transport;
    Cluster::ClusterStateMachine _application;
    std::unique_ptr<Consensus::RaftDriver> _driver;
    std::unique_ptr<PlatformListener> _listener;
    std::unique_ptr<Consensus::IRaftMessageSink> _sink;
    std::unique_ptr<Consensus::RaftPeerServer> _peerServer;

    RoleObserver _onRole;
    std::string _boundEndpoint;

    /// This node's own record, which nothing else can supply.
    ///
    /// Its scheduler endpoint is the half nobody else knows: peers learn a node's
    /// consensus address by dialing it, and there is no equivalent for a port they
    /// never connect to. So a node announces itself, and only while it leads --
    /// which is exactly when its scheduler endpoint is the one clients need.
    Cluster::ClusterMember _self;

    /// Told the member set whenever it changes; may be empty.
    MembersObserver _onMembers;

    /// What consensus last said, so a state change can be re-read against it.
    Consensus::Role _lastRole { Consensus::Role::Follower };
    std::optional<Consensus::NodeId> _lastLeader;

    /// What the scheduler was last told, so an unchanged answer is not re-announced.
    ///
    /// Both halves, because either can move on its own: a role change with the same
    /// leader endpoint is a real change, and so is the endpoint arriving for a
    /// leader this node already knew about.
    Distributed::SchedulerRole _publishedRole { Distributed::SchedulerRole::Undecided };
    std::string _publishedEndpoint;

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

    /// Whether this node may propose at all, as the observer last reported it.
    ///
    /// Read by the reconciler and written by the driver's threads, so it is atomic
    /// -- and it is a cache of the driver's answer rather than a second opinion: a
    /// stale read costs one refused proposal, which `Propose` reports and the next
    /// pass repeats.
    std::atomic<bool> _leads { false };

    /// How many of the two reactor loops are still running.
    ///
    /// Whichever finishes last stops the reactor, so `Run()` never returns while a
    /// coroutine is still parked on it -- which would leak that frame outright.
    std::atomic<int> _loopsRunning { 2 };

    /// Wakes the reconciler for a stop rather than leaving it in a bare sleep.
    ///
    /// A stop that had to wait out a full interval would make teardown look hung
    /// for a second per node, which is the shape of defect this repository has
    /// already paid for once as a `systemctl stop` that escalated to SIGKILL.
    std::condition_variable_any _wake;
    std::mutex _wakeMutex;

    // Started last and joined first, which the member order gives for free.
    std::jthread _ioThread;
    std::jthread _reconcileThread;
};

/// Start consensus when the operator configured a cluster, wiring it to the node.
///
/// A function rather than four lines in `WorkerBody`, for the reason
/// `StartCacheTierOrExplain` is one: it is a coherent decision with one answer, it
/// pushed `WorkerBody` past clang-tidy's cognitive-complexity limit inline, and
/// `main.cpp` is in no test target. What it encodes is which of this node's parts
/// consensus drives -- the scheduler's role and the fleet's membership -- and that
/// list is the thing worth reading in one place.
///
/// A null result is success: it means no `--node-id` was given, so this node runs
/// alone and the scheduler tier's standalone leadership stands. That is the ordinary
/// single-machine deployment, not a degraded one.
/// @param cfg The parsed configuration.
/// @param schedulerTier Told this node's role; may be null when it serves none.
/// @param membership Told the replicated member set; must outlive the tier.
/// @param logger Where progress and refusals are reported.
/// @return The tier, a null tier meaning "no cluster configured", or the fatal reason.
[[nodiscard]] std::expected<std::unique_ptr<ConsensusTier>, std::string> StartConsensusOrExplain(
    NodeConfig const& cfg, std::unique_ptr<SchedulerTier> const& schedulerTier, NodeMembership& membership, ILogger& logger);

} // namespace FastCache::Node
