// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Consensus/IRaftStateMachine.hpp>
#include <FastCache/Consensus/IRaftStorage.hpp>
#include <FastCache/Consensus/IRaftTransport.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Core/Clock.hpp>

#include <atomic>
#include <cstddef>
#include <expected>
#include <optional>
#include <vector>

namespace FastCache::Consensus
{

/// Carries out what a `RaftNode` asks for, in the order the algorithm requires.
///
/// The node decides and the driver acts; this is the only place the two meet, so
/// it is the only place the ordering contract can be enforced rather than
/// remembered. Every path funnels through one private `Deliver`, which is why
/// there is no route that sends before it persists.
///
/// ## The order, and why each step is where it is
///
/// **Durable state first — both parts of it.** `currentTerm`/`votedFor`, then the
/// log. A node that answers a RequestVote and crashes before the vote is durable
/// comes back believing it has not voted, votes again in the same term, and one
/// term gets two leaders. The log is second rather than first because the crash
/// window in between should leave the *safe* half-state: a node at a newer term
/// missing an entry is indistinguishable from one that never appended it, while
/// a node holding an entry from a term it does not remember entering is a state
/// nothing else in the algorithm reasons about.
///
/// **Then send.** Once, and without waiting: see `IRaftTransport`.
///
/// **Then apply.** Applying is local and the peers cannot make progress until the
/// messages are out, so doing it first would add the application's latency to the
/// replication path and buy nothing.
///
/// ## What a storage failure does
///
/// It stops the driver. Everything else in this system falls back to a local
/// compile and carries on; this cannot, because the only way to continue past a
/// failed durability write is to act on state that is not durable — which is
/// precisely the thing that turns a stalled node into a corrupt cluster. The
/// failure is latched and reported rather than retried, because a retry loop over
/// a disk that is full or gone is a node that looks alive and does nothing.
class RaftDriver
{
  public:
    /// @param node The state machine to drive; taken by value and owned.
    /// @param storage Where durable state goes; must outlive this driver.
    /// @param transport How peers are reached; must outlive this driver.
    /// @param application What committed entries are handed to; must outlive this.
    RaftDriver(RaftNode node, IRaftStorage& storage, IRaftTransport& transport, IRaftStateMachine& application) noexcept;

    /// Advance time, doing whatever falls due.
    /// @param now The current instant.
    /// @return Nothing, or the storage failure that stopped this node.
    [[nodiscard]] std::expected<void, ConsensusError> Tick(TimePoint now);

    /// Feed in a message received from a peer.
    /// @param message What arrived.
    /// @param now The current instant.
    /// @return Nothing, or the storage failure that stopped this node.
    [[nodiscard]] std::expected<void, ConsensusError> Receive(RaftMessage const& message, TimePoint now);

    /// Offer an entry to the cluster.
    /// @param payload Application bytes.
    /// @param now The current instant.
    /// @return Where it landed, or why it was refused.
    [[nodiscard]] std::expected<LogIndex, ConsensusError> Propose(std::vector<std::byte> payload, TimePoint now);

    /// @return The node being driven, for inspection.
    [[nodiscard]] RaftNode const& Node() const noexcept;

    /// @return When `Tick` must next be called.
    [[nodiscard]] TimePoint NextDeadline() const noexcept;

    /// The failure that stopped this driver, if one did.
    ///
    /// Latched: once storage has failed, every later call refuses with the same
    /// error rather than pretending the node is still participating.
    /// @return The failure, or nullopt.
    [[nodiscard]] std::optional<ConsensusError> const& Failure() const noexcept;

    /// Drive the node's timers on a reactor until stopped or broken.
    ///
    /// Only the timer half: messages arrive through `Receive`, called by whatever
    /// owns the transport. Splitting it that way is what lets this loop be tested
    /// against `TestReactor` and a `ManualClock` with no sockets in sight, and it
    /// is also the shape a real transport wants — it already has a read loop and
    /// needs somewhere to hand what it read.
    /// A pointer rather than a reference because this is a coroutine: a reference
    /// parameter is bound before the first suspension and then outlives every
    /// frame that could have kept it alive, which is why clang-tidy refuses them
    /// here. `SleepUntil` holds its reactor the same way, for the same reason.
    /// @param reactor Supplies both the timer wheel and the clock; must outlive
    ///        the returned task and must not be null.
    /// @return A task that completes when `Stop` is called or storage fails.
    [[nodiscard]] Task<void> Run(IReactor* reactor);

    /// Ask `Run` to finish. Safe to call before it starts, and from any thread.
    ///
    /// The loop observes this when its current wait expires, so teardown can lag
    /// by up to `electionTimeoutMax`. That is a deliberate limit rather than an
    /// oversight: waking the wait early needs a reactor-side cancellation this
    /// library does not have yet, and a supervisor that cannot wait stops the
    /// reactor as well. The flag is atomic because the natural caller is a signal
    /// handler or a shutdown thread, not the loop itself.
    void Stop() noexcept;

  private:
    /// Perform one output in the required order.
    [[nodiscard]] std::expected<void, ConsensusError> Deliver(RaftOutput output);

    RaftNode _node;
    IRaftStorage& _storage;
    IRaftTransport& _transport;
    IRaftStateMachine& _application;

    /// Written only by the loop thread, so it needs no synchronization of its
    /// own — unlike `_stopped`, which any thread may set.
    std::optional<ConsensusError> _failure;

    std::atomic<bool> _stopped { false };
};

} // namespace FastCache::Consensus
