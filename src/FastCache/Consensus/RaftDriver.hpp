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
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace FastCache::Consensus
{

/// When a `RaftDriver` trades its log for a snapshot of what the log produced.
///
/// The policy is the caller's because the cost it balances is: a snapshot is the
/// *whole* application state written out, so trimming often makes a small log
/// expensive, and trimming never makes a long-lived node re-read its entire
/// history at every restart and keep every entry in memory in between. Only
/// whoever knows how large that state is can say where the crossover sits.
///
/// **At namespace scope rather than nested in `RaftDriver`, and that is forced.**
/// A defaulted constructor parameter of a type nested in the same class is
/// rejected by both compilers this project builds with, for reasons that read as
/// opposites: GCC will not convert `{}` to an incomplete nested aggregate, and
/// clang refuses `= CompactionPolicy {}` because the nested type's own default
/// member initializer is not available inside the enclosing class definition.
/// Neither is a bug; the type is simply not complete there. Hoisting it makes
/// both spellings legal, and MSVC accepted every one of them, so a local build
/// would have reported this tree green.
struct CompactionPolicy
{
    /// Applied entries above the snapshot that provoke one; zero never does.
    ///
    /// Measured from the snapshot boundary rather than from the log's length,
    /// because those differ on a follower whose leader has already trimmed further
    /// than it has applied -- and it is the *unsnapshotted* part that a restart has
    /// to replay.
    std::uint64_t appliedEntriesBeforeCompaction { 0 };
};

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
/// ## Why it holds a lock
///
/// The node is advanced from more than one thread by construction, and not by
/// accident: the timer loop ticks it, a peer reader hands it messages, and
/// whatever wants to change the cluster's own configuration proposes on its own
/// schedule. `RaftNode` is a plain state machine with no synchronization of its
/// own -- that is what makes it testable -- so the serialization has to live
/// here, at the one place all three routes meet.
///
/// Held across the durability writes and the sends, which is the point rather
/// than an oversight: those are exactly the steps whose ORDER the algorithm
/// depends on, and a second thread interleaving a vote between a node's persist
/// and its reply is the crash-restart double vote spelled with threads instead
/// of a power cut. Never held across a suspension: `Run` awaits outside it.
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
    /// Everything an observer is told when this node's role moves.
    ///
    /// One report rather than a parameter per fact, so the next thing worth
    /// reporting is a field here instead of another positional argument at every
    /// call site and in every observer.
    struct RoleChange
    {
        Role role {}; ///< What this node is playing now.

        /// The term it is playing it in.
        ///
        /// Carried because a role alone cannot be read after the fact: it says
        /// leadership moved and nothing about how often, and an intermittent
        /// election is diagnosable from its logs or not at all.
        Term term {};

        /// Who it believes leads, if anybody.
        ///
        /// It travels with the role because a follower's whole use for one is to
        /// redirect, and a refusal that cannot say who to ask instead cannot be
        /// acted on.
        std::optional<NodeId> knownLeader;

        /// Why, when a peer's higher term caused this; absent otherwise.
        ///
        /// A change this node decided for itself -- winning an election, timing out
        /// -- has no cause to name, and the two are worth telling apart: one is this
        /// node acting and the other is something being done to it.
        std::optional<TermAdoption> cause;
    };

    /// Told this node's role whenever it changes, and never otherwise.
    ///
    /// **Pushed rather than polled**, because the alternatives are both worse. A
    /// caller polling `Node().CurrentRole()` needs a thread and an interval, and the
    /// interval is a window during which this node has stopped leading and is still
    /// handing out other machines' capacity. `RaftNode` deliberately reports
    /// transitions as *state* rather than as events -- a node that emitted one per
    /// transition would make its own callers order-dependent -- so the observation
    /// belongs here, at the one place that knows when a step has been taken.
    ///
    /// Called from whichever thread advanced the node: the timer loop for an
    /// election timeout, the transport's reader for a message that deposed it. An
    /// implementation must therefore be safe to call from either, and must not block
    /// -- it is on the path that also has to send the next heartbeat.
    using RoleObserver = std::function<void(RoleChange const& change)>;

    /// @param node The state machine to drive; taken by value and owned.
    /// @param storage Where durable state goes; must outlive this driver.
    /// @param transport How peers are reached; must outlive this driver.
    /// @param application What committed entries are handed to; must outlive this.
    /// @param compaction When to trim the log; defaults to never.
    ///
    /// The default is "never" so that a caller which has not thought about
    /// compaction gets the behaviour that cannot lose anything -- a log that grows
    /// is wasteful, and a snapshot taken by a machine whose `TakeSnapshot` is not
    /// yet meaningful is wrong.
    RaftDriver(RaftNode node,
               IRaftStorage& storage,
               IRaftTransport& transport,
               IRaftStateMachine& application,
               CompactionPolicy compaction = {}) noexcept;

    /// Install the role observer.
    ///
    /// A setter and not a constructor parameter, which is one of the documented
    /// carve-outs: the natural owner of the observer is the object that owns the
    /// driver, and it cannot pass `this` to a member it is still constructing.
    /// Called once, before `Run`.
    /// @param observer Told about role changes; may be empty to stop observing.
    void ObserveRole(RoleObserver observer);

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

    /// Offer the cluster a new member set, one member added or removed.
    ///
    /// `RaftNode::ProposeMembership` reached through the same lock and the same
    /// output ordering as `Propose`, and it needs its own entry point rather than
    /// a caller doing it: a configuration change is a durability write followed by
    /// a broadcast, so its output has to be delivered in order by whoever owns
    /// that order.
    /// @param members The proposed member set.
    /// @param now The current instant.
    /// @return Where the entry landed, or why it was refused.
    [[nodiscard]] std::expected<LogIndex, ConsensusError> ProposeMembership(std::vector<NodeId> members, TimePoint now);

    /// What consensus counts, and how far it has agreed.
    ///
    /// Both under one lock, because the caller compares them and two acquisitions
    /// would let a configuration change land between them — reading a member set
    /// from before it and a commit index from after, which is a pair that never
    /// existed. By value for the reason `Failure` is: `Node()` hands back a
    /// reference to a member the timer loop and a peer reader are both free to
    /// move.
    struct Progress
    {
        std::vector<NodeId> members; ///< The member set this node operates under.
        LogIndex commitIndex;        ///< How far its log is committed.

        /// The term it is operating in.
        ///
        /// Taken under the same lock as the other two, which is the point: a caller
        /// comparing a log index it remembers against a commit index has to know
        /// whether the two belong to the same term, and reading the term separately
        /// would let leadership move between the reads and produce a triple that
        /// never existed.
        Term term;
    };

    /// @return The member set and commit index, read together.
    [[nodiscard]] Progress CurrentProgress() const;

    /// The node being driven, for inspection.
    ///
    /// **Not synchronized**, and it cannot be: a reference outlives any lock this
    /// could take. For tests and for a caller that knows no other thread is
    /// advancing this driver.
    /// @return The node.
    [[nodiscard]] RaftNode const& Node() const noexcept;

    /// The failure that stopped this driver, if one did.
    ///
    /// Latched: once storage has failed, every later call refuses with the same
    /// error rather than pretending the node is still participating.
    /// By value, because reading it takes the same lock everything else here does
    /// -- a reference would hand the caller a member another thread may be writing.
    /// @return The failure, or nullopt.
    [[nodiscard]] std::optional<ConsensusError> Failure() const;

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
    /// by one heartbeat interval. That is a deliberate limit rather than an
    /// oversight: waking the wait early needs a reactor-side cancellation this
    /// library does not have yet, and a supervisor that cannot wait stops the
    /// reactor as well. The flag is atomic because the natural caller is a signal
    /// handler or a shutdown thread, not the loop itself.
    void Stop() noexcept;

  private:
    /// Perform one output in the required order; `_mutex` must be held.
    [[nodiscard]] std::expected<void, ConsensusError> Deliver(RaftOutput output);

    /// Carry out what a proposal asked for and report where it landed.
    ///
    /// The tail both proposal entry points share, so the ordering they claim to
    /// have in common is a fact rather than an assertion: the index is read before
    /// the output is moved from, and a delivery failure replaces the index rather
    /// than being reported alongside one. `_mutex` must be held.
    /// @param proposed What the node answered.
    /// @return Where the entry landed, or why there is none.
    [[nodiscard]] std::expected<LogIndex, ConsensusError> Land(std::expected<RaftNode::Proposal, ConsensusError> proposed);

    /// How long `Run` may sleep, given the node's own next deadline.
    ///
    /// Bounded rather than taken from the node alone; the reason is at the
    /// definition and it is the difference between a cluster that settles and one
    /// that re-elects every second.
    /// @param now The instant the sleep starts from.
    /// @return The instant to wake at.
    [[nodiscard]] TimePoint SleepDeadline(TimePoint now) const;

    /// Report the role if it has moved since the last report.
    /// @param cause What a step-down reported, if this step had one.
    void PublishRoleIfChanged(std::optional<TermAdoption> const& cause);

    /// Trade the applied prefix of the log for a snapshot, if enough has piled up.
    ///
    /// Runs after the outputs rather than as one of them, because it is
    /// maintenance rather than something the algorithm asked for -- but it is
    /// ordered here, in the driver, for the reason every other durability write is:
    /// the entries are gone from memory the moment `CompactThroughApplied` returns,
    /// so a node that discarded them without first recording what they produced
    /// comes back from a restart missing committed state.
    /// @return Nothing, or the storage failure that stopped this node.
    [[nodiscard]] std::expected<void, ConsensusError> CompactIfDue();

    /// Serializes every route into `_node`. See the class comment.
    mutable std::mutex _mutex;

    RaftNode _node;
    IRaftStorage& _storage;
    IRaftTransport& _transport;
    IRaftStateMachine& _application;
    CompactionPolicy _compaction;
    RoleObserver _onRole;

    /// What was last reported, so an unchanged role is not re-announced.
    ///
    /// Seeded to the node's role at construction rather than to a sentinel, so a
    /// node that recovered as a follower and stays one reports nothing -- an
    /// observer that fired once at startup for no transition would make "the role
    /// changed" mean two different things.
    ///
    /// The **term** is part of what counts as changed, and that is not decoration.
    /// A node campaigning round after round without winning stays a candidate with
    /// no known leader the whole time, so a report keyed on (role, leader) alone
    /// says nothing at all during exactly the storm somebody is trying to read
    /// afterwards. A cluster that is working changes term never, so watching it
    /// costs nothing and speaks only when something is wrong.
    Role _reportedRole;
    Term _reportedTerm;
    std::optional<NodeId> _reportedLeader;

    /// Written only by the loop thread, so it needs no synchronization of its
    /// own — unlike `_stopped`, which any thread may set.
    std::optional<ConsensusError> _failure;

    std::atomic<bool> _stopped { false };
};

} // namespace FastCache::Consensus
