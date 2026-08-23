// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeMembership.hpp"
#include "SchedulerTier.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/ClusterStateMachine.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>
#include <FastCache/Consensus/RaftPeerServer.hpp>
#include <FastCache/Consensus/RaftPeerTransport.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
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
/// ## Two threads, not one
///
/// `RaftDriver::Run` drives the election and heartbeat timers on a reactor;
/// `RaftPeerServer::Run` blocks in `accept`. Neither can host the other — the
/// driver's loop must not be held up by a peer that stopped reading, and the accept
/// loop cannot service a timer wheel. They meet only through the message sink, which
/// is the one place their threads touch.
class ConsensusTier
{
  public:
    /// How often a parked `accept()` returns so a stop can be observed.
    ///
    /// The only portable way to wake one: POSIX does not unblock it when another
    /// thread closes the socket. Short enough that a `systemctl stop` does not look
    /// hung, long enough that an idle cluster is not spinning.
    static constexpr std::chrono::milliseconds AcceptPoll { 250 };

    /// How long a peer connection may stall before it is abandoned.
    ///
    /// Consensus messages are small and a peer that cannot take one in this long is
    /// a peer whose absence the election timer should be deciding about, not a read
    /// this node waits on.
    static constexpr std::chrono::milliseconds PeerIoTimeout { 2'000 };

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
    /// @param onRole Told this node's role; must outlive the tier.
    /// @param onMembers Told the member set; must outlive the tier.
    /// @param logger Where progress and refusals are reported.
    /// @return The running tier, or the fatal reason.
    [[nodiscard]] static std::expected<std::unique_ptr<ConsensusTier>, std::string> Start(NodeConfig const& cfg,
                                                                                          RoleObserver onRole,
                                                                                          MembersObserver onMembers,
                                                                                          ILogger& logger);

    ConsensusTier(ConsensusTier const&) = delete;
    ConsensusTier& operator=(ConsensusTier const&) = delete;
    ConsensusTier(ConsensusTier&&) = delete;
    ConsensusTier& operator=(ConsensusTier&&) = delete;

    /// Stops both loops and joins both threads.
    ///
    /// A destructor rather than a `Shutdown()` somebody has to remember at every
    /// return path — the lesson `AdminEndpoint` records. The peer server's listener
    /// is closed before its thread is joined, because POSIX does not unblock a
    /// parked `accept()`.
    ~ConsensusTier();

    /// Propose a change to the cluster's state.
    ///
    /// Refused unless this node leads, and refused for a command `Validate` rejects
    /// — which is the only place a change CAN be refused, since applying happens
    /// after commitment when there is nobody left to report to.
    /// @param command The change.
    /// @return The index it was appended at, or why it was not.
    [[nodiscard]] std::expected<Consensus::LogIndex, ConsensusError> Propose(Cluster::Command const& command);

    /// The cluster's state as this node last applied it.
    [[nodiscard]] Cluster::ClusterState const& State() const noexcept;

    /// The address this node's peer port bound.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

  private:
    ConsensusTier(NodeConfig const& cfg,
                  Consensus::FileRaftStorage storage,
                  std::unique_ptr<IListener> listener,
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
    /// @return Nothing on success, or the fatal reason.
    [[nodiscard]] std::expected<void, std::string> Launch(NodeConfig const& cfg,
                                                          std::vector<Cluster::ClusterMember> const& members);

    /// Translate a consensus role into the scheduler's, and pass it on.
    ///
    /// Called by the driver whenever the role actually moves -- from the timer thread
    /// for an election timeout, from a peer reader for a message that deposed this
    /// node. It does the one thing consensus cannot: turn a leader's *id* into the
    /// *address* a client redirects to, which only the replicated state knows.
    /// @param role What consensus says this node is.
    /// @param knownLeader Who it believes leads, if anybody.
    void PublishRole(Consensus::Role role, std::optional<Consensus::NodeId> const& knownLeader);

    ILogger& _logger;

    // Declaration order IS construction order, and each is referenced by the one
    // below it. This is the ordering the class exists to make the language check.
    Consensus::FileRaftStorage _storage;
    std::unique_ptr<IRandomSource> _random;
    std::unique_ptr<IConnector> _connector;
    std::unique_ptr<Consensus::RaftPeerTransport> _transport;
    Cluster::ClusterStateMachine _application;
    std::unique_ptr<Consensus::RaftDriver> _driver;
    std::unique_ptr<IListener> _listener;
    std::unique_ptr<Consensus::IRaftMessageSink> _sink;
    std::unique_ptr<Consensus::RaftPeerServer> _peerServer;

    RoleObserver _onRole;
    std::string _boundEndpoint;

    // Started last and joined first, which the member order gives for free.
    std::jthread _timerThread;
    std::jthread _acceptThread;
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
