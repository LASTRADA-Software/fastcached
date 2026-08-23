// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftStorage.hpp>
#include <FastCache/Consensus/RaftConfig.hpp>
#include <FastCache/Consensus/RaftLog.hpp>
#include <FastCache/Consensus/RaftOutput.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/IRandomSource.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace FastCache::Consensus
{

/// Which timer governs a node in a given role.
enum class TimerKind : std::uint8_t
{
    Election = 0, ///< Time until this node stands for election.
    Heartbeat,    ///< Time until this node next sends heartbeats.
};

/// The per-role facts the state machine reads rather than branches on.
struct RoleTraits
{
    Role role {};          ///< The role this row describes.
    std::string_view name; ///< For log lines and test failure messages.
    TimerKind timer {};    ///< Which deadline `Tick` compares against.
};

/// Behaviour that varies by role, as data.
///
/// Three rows today. The reason this is a table and not a `switch` is the fourth
/// row that is already foreseeable: a non-voting **Learner**, which is how a node
/// is brought up to date without being counted in a quorum — exactly what
/// discovery needs when a machine joins a running cluster. Adding it should be a
/// row plus whatever columns it forces, not an edit to every function that asks
/// what role this node is playing.
inline constexpr std::array<RoleTraits, 4> RoleTable { {
    { .role = Role::Follower, .name = "follower", .timer = TimerKind::Election },
    { .role = Role::PreCandidate, .name = "pre-candidate", .timer = TimerKind::Election },
    { .role = Role::Candidate, .name = "candidate", .timer = TimerKind::Election },
    { .role = Role::Leader, .name = "leader", .timer = TimerKind::Heartbeat },
} };

/// The row describing `role`.
/// @param role The role to look up.
/// @return Its traits.
[[nodiscard]] RoleTraits const& TraitsOf(Role role) noexcept;

/// One Raft node, as a deterministic state machine.
///
/// **Performs no I/O and reads no clock of its own.** Time arrives as a
/// parameter, randomness through an injected `IRandomSource`, and everything the
/// node wants done leaves as a `RaftOutput` for a driver to carry out. That is
/// not stylistic: consensus is the one thing in this system where a rare
/// interleaving is a correctness bug rather than a slow request, so the whole
/// algorithm has to be reachable from a test that can produce those
/// interleavings on demand. A node that read a clock or opened a socket could
/// only be tested against a real network, which is to say against whichever
/// interleavings happened to occur.
///
/// The driver owes it two things, and both are contracts rather than
/// suggestions: make `RaftOutput::persist` durable **before** sending
/// `RaftOutput::messages` (see `RaftOutput`), and call `Tick` no later than
/// `NextDeadline()`.
///
/// Thread-compatible, not thread-safe: one node is driven by one loop. Sharing
/// it would need a lock, and a lock around a state machine that also does I/O
/// through its return value would be held across the I/O.
class RaftNode
{
  public:
    /// Create a node that has never run before, or say why the configuration
    /// cannot run.
    ///
    /// A factory rather than a throwing constructor, because setup here is
    /// genuinely fallible and the caller is a daemon that should refuse to start
    /// with a message naming the offending field. It is also the only way to make
    /// `RaftConfig::Validate` unbypassable: a constructor that merely *documents*
    /// the precondition is one an omission can skip, and the resulting failures
    /// are silent rather than loud — a `self` outside `members` makes `Peers()`
    /// return every member while `Quorum()` still assumes this node is one of
    /// them, so the node counts a self-vote it is not entitled to; an empty
    /// `members` gives a quorum of one and the node elects itself instantly.
    ///
    /// Everything the node needs is supplied here and fixed afterwards, so there
    /// is no window in which a node exists but does not yet know who it is.
    /// @param config The cluster configuration.
    /// @param random Source for election-timeout jitter; must outlive this node.
    /// @param now The current instant, used to arm the first election timeout.
    /// @param recovered What durable storage held; defaults to a node that has
    ///        never run. One entry point rather than two, because a
    ///        default-constructed `RecoveredState` *is* a fresh node — a separate
    ///        restore path would be a second construction sequence able to drift
    ///        from this one.
    /// @return The node, or why the configuration was refused.
    [[nodiscard]] static std::expected<RaftNode, ConsensusError> Create(RaftConfig config,
                                                                        IRandomSource& random,
                                                                        TimePoint now,
                                                                        RecoveredState recovered = {});

    /// @return The role this node is currently playing.
    [[nodiscard]] Role CurrentRole() const noexcept;

    /// @return The latest term this node has seen.
    [[nodiscard]] Term CurrentTerm() const noexcept;

    /// @return Who this node voted for in the current term, if anyone.
    [[nodiscard]] std::optional<NodeId> const& VotedFor() const noexcept;

    /// The leader this node believes is in charge, if it knows of one.
    ///
    /// What a follower redirects a client to. Absent between a leader dying and
    /// the next one winning, which is a real state a caller must handle rather
    /// than wait out — the whole point of the fallback rule around this feature
    /// is that "there is no leader right now" is answerable immediately.
    /// @return The known leader, or nullopt.
    [[nodiscard]] std::optional<NodeId> const& KnownLeader() const noexcept;

    /// @return The node's log.
    [[nodiscard]] RaftLog const& Log() const noexcept;

    /// The highest index known to be committed.
    ///
    /// Committed means present in every future leader's log, so an entry at or
    /// below this can be acted on and will never be taken back.
    /// @return The commit index.
    [[nodiscard]] LogIndex CommitIndex() const noexcept;

    /// What a successful proposal yields.
    struct Proposal
    {
        LogIndex index {}; ///< Where the entry landed; watch `CommitIndex()` for it.
        RaftOutput output; ///< What the driver should do about it.
    };

    /// Offer an entry to the cluster.
    ///
    /// Only a leader may accept one, so this refuses on every other node — with
    /// the leader's identity when it knows one, because "ask that node instead"
    /// and "nobody leads right now" are different answers and only the second
    /// means give up and act locally.
    ///
    /// Acceptance is not commitment. The entry is appended to this leader's log
    /// and replicated; it is committed when a quorum has it, which is reported
    /// through `RaftOutput::applied` on some later event. A caller that needs to
    /// know when to act watches for the index, and one that cannot wait treats a
    /// proposal like any other refusal.
    /// @param payload Application bytes, never interpreted here.
    /// @param now The current instant.
    /// @return Where it landed and what to do, or why it was refused.
    [[nodiscard]] std::expected<Proposal, ConsensusError> Propose(std::vector<std::byte> payload, TimePoint now);

    /// Propose a new member set, one member added or removed (§4.3).
    ///
    /// Restricted to a single-member delta, and that restriction is the whole
    /// safety argument: any majority of the old configuration and any majority
    /// of the new one then share at least one member, so the two cannot elect
    /// different leaders in the same term. Going from three members to five in
    /// one step makes `{n1,n2}` a majority of the old and `{n3,n4,n5}` a
    /// majority of the new, with nobody in common — which is what joint
    /// consensus exists to handle and why this refuses instead.
    ///
    /// Only one change may be in flight. A second proposed before the first
    /// commits would be built on a configuration that can still be rolled back
    /// by a truncation, so the safety argument above would be comparing against
    /// a set that never existed.
    ///
    /// The new configuration takes effect on **this node** immediately, before
    /// it is committed, because a configuration that waited for commitment could
    /// not be used to reach it.
    /// @param members The proposed member set.
    /// @param now The current instant.
    /// @return Where the entry landed and what to do, or why it was refused.
    [[nodiscard]] std::expected<Proposal, ConsensusError> ProposeMembership(std::vector<NodeId> members, TimePoint now);

    /// Compact the log, keeping `state` as the snapshot that replaces it.
    ///
    /// The log carries cluster configuration and cluster state only, so it grows
    /// slowly — but a log nobody ever trims is a restart that takes longer every
    /// time, and a leader that has to keep every entry forever in case some
    /// follower is behind.
    ///
    /// Only applied state may be discarded: entries above `LastApplied()` have
    /// not reached the application, so a snapshot does not describe them. The
    /// configuration is captured alongside, because a follower catching up from
    /// this snapshot has no entries left to learn it from.
    /// @param state The application's serialized state as of `LastApplied()`.
    /// @param output Receives the snapshot to make durable.
    /// @return True when the log was compacted.
    ///
    /// The snapshot leaves through `output` rather than being written here, for
    /// the reason every other durability write does: the entries it replaces are
    /// gone from memory the moment this returns, so a node that discarded them
    /// without recording what they produced comes back from a restart missing
    /// committed state.
    bool CompactThroughApplied(std::vector<std::byte> state, RaftOutput& output);

    /// The snapshot this node currently holds, as a durable record.
    ///
    /// Composed from the log's own boundary rather than stored beside it: the log
    /// already owns where the snapshot ends, and a second copy of that pair is a
    /// second thing that can come to disagree with it.
    /// @return The record.
    [[nodiscard]] RaftSnapshot CurrentSnapshot() const;

    /// The last index this node's snapshot covers, or `BeforeFirst()`.
    /// @return The snapshot point.
    [[nodiscard]] LogIndex SnapshotIndex() const noexcept
    {
        return _log.SnapshotIndex();
    }

    /// How far the application has been advanced.
    /// @return The last applied index.
    [[nodiscard]] LogIndex LastApplied() const noexcept
    {
        return _lastApplied;
    }

    /// The member set this node is currently operating under.
    ///
    /// The latest configuration in its log, which is not necessarily a committed
    /// one; see `ProposeMembership`.
    /// @return The active members.
    [[nodiscard]] std::vector<NodeId> const& ActiveMembers() const noexcept
    {
        return _members;
    }

    /// When the driver must next call `Tick`.
    ///
    /// Which deadline this is depends on the role and comes from `RoleTable`.
    /// @return The next instant at which something is due.
    [[nodiscard]] TimePoint NextDeadline() const noexcept;

    /// Advance time.
    ///
    /// Idempotent before the deadline: calling it early returns an empty output
    /// rather than misbehaving, so a driver that wakes spuriously — which every
    /// real reactor does — costs nothing.
    /// @param now The current instant.
    /// @return What the driver should do.
    [[nodiscard]] RaftOutput Tick(TimePoint now);

    /// Handle one received message.
    /// @param message The message, from any member.
    /// @param now The current instant.
    /// @return What the driver should do.
    [[nodiscard]] RaftOutput Receive(RaftMessage const& message, TimePoint now);

  private:
    /// Private, so `Create` is the only way in and its validation cannot be
    /// bypassed by omission.
    /// @param config A configuration that has already passed `Validate()`.
    /// @param random Source for election-timeout jitter.
    /// @param now The current instant.
    /// @param recovered What durable storage held.
    RaftNode(RaftConfig config, IRandomSource& random, TimePoint now, RecoveredState recovered);

    /// Whether `id` is a configured member of this cluster.
    ///
    /// Every identity in an incoming message is self-declared, and two of them
    /// decide something: a granted vote counts toward leadership, and an accepted
    /// AppendEntries publishes its sender as the leader clients are redirected to.
    /// @param id The claimed identity.
    /// @return True when the configuration contains it.
    [[nodiscard]] bool IsMember(NodeId const& id) const;
    [[nodiscard]] std::size_t Quorum() const noexcept;
    void AdoptMembers(std::vector<NodeId> members);
    void RefreshConfiguration();
    [[nodiscard]] bool HasUncommittedConfiguration() const;
    [[nodiscard]] LogIndex LatestConfigurationIndex() const;

    /// Begin an election for the next term (§5.2).
    void StartPreVote(TimePoint now, RaftOutput& output);
    void StartElection(TimePoint now, RaftOutput& output);

    /// Adopt a higher term and return to being a follower (§5.1).
    ///
    /// The single most-invoked rule in Raft, and the one whose omission is
    /// hardest to see: it applies to **every** message, response as well as
    /// request. A leader that ignores a higher term arriving on an
    /// AppendEntriesResponse keeps believing it leads a term the cluster has
    /// moved past, and goes on answering clients as though it did.
    void StepDown(Term term, TimePoint now, RaftOutput& output);

    /// Win the election and start heartbeating.
    void BecomeLeader(TimePoint now, RaftOutput& output);

    /// Arm the election timer with a freshly drawn randomized timeout.
    ///
    /// Re-drawn every time rather than drawn once per node, because a fixed
    /// per-node timeout makes the node with the shortest one win every election
    /// forever — and if that node is partitioned but alive, its repeated
    /// candidacies disrupt the cluster on a fixed schedule.
    void ArmElectionTimer(TimePoint now);

    /// Queue a message to every peer.
    void BroadcastToPeers(RaftOutput& output, RaftMessage const& message) const;

    /// The AppendEntries this leader owes `peer`, given how far it has caught up.
    ///
    /// Carries whatever sits at and after that peer's `nextIndex`, which is empty
    /// for a peer that is up to date — so a heartbeat is not a separate kind of
    /// message, it is this one with nothing left to send. Keeping them one thing
    /// is what makes a heartbeat also the mechanism that discovers a divergent
    /// follower, rather than a second code path that has to remember to.
    /// @param peer The follower.
    /// @return The request to send it.
    [[nodiscard]] AppendEntriesRequest MakeAppendEntriesFor(NodeId const& peer) const;

    /// Send each peer the AppendEntries it is owed.
    void ReplicateToPeers(RaftOutput& output) const;

    /// Record how far a follower has confirmed it matches, and where to send next.
    ///
    /// One function rather than one per response type: an AppendEntries and an
    /// InstallSnapshot both establish a match index by the same rule, and two
    /// copies of a rule this delicate are two places for it to drift.
    /// @param follower Which peer answered.
    /// @param reported The match index it claims, before clamping.
    void AdvanceFollowerProgress(NodeId const& follower, LogIndex reported);

    /// Advance the commit index if a quorum has caught up (§5.4.2).
    void AdvanceCommitIndex();

    /// Emit every entry that has become committed since the last call.
    void ApplyCommitted(RaftOutput& output);

    /// Ask the driver to make the log durable from `fromIndex` onward.
    ///
    /// Reads the entries out of the log rather than taking them from the caller,
    /// so the record is of what was actually written — including a truncation
    /// that a follower's repair performed, which the incoming request does not
    /// describe on its own.
    /// @param output Where to record it.
    /// @param fromIndex First index that changed.
    void RecordLogAppend(RaftOutput& output, LogIndex fromIndex);

    /// Record the durable state in `output`, for the driver to write first.
    void MarkPersist(RaftOutput& output) const;

    /// Per-message handlers; each may append to `output`.
    /// Whether the §5.1 term rule must NOT be applied to this message.
    /// @param message The message being received.
    /// @return True for a pre-vote request, and for a granted pre-vote response.
    [[nodiscard]] static bool IsPreVoteExempt(RaftMessage const& message) noexcept;
    void OnPreVote(PreVoteRequest const& request, TimePoint now, RaftOutput& output);
    void OnPreVoteResponse(PreVoteResponse const& response, TimePoint now, RaftOutput& output);
    void OnRequestVote(RequestVoteRequest const& request, TimePoint now, RaftOutput& output);
    void OnRequestVoteResponse(RequestVoteResponse const& response, TimePoint now, RaftOutput& output);
    void OnAppendEntries(AppendEntriesRequest const& request, TimePoint now, RaftOutput& output);
    void OnAppendEntriesResponse(AppendEntriesResponse const& response, TimePoint now, RaftOutput& output);
    void OnInstallSnapshot(InstallSnapshotRequest const& request, TimePoint now, RaftOutput& output);
    void OnInstallSnapshotResponse(InstallSnapshotResponse const& response, TimePoint now, RaftOutput& output);
    [[nodiscard]] bool NeedsSnapshot(NodeId const& peer) const;
    [[nodiscard]] InstallSnapshotRequest MakeInstallSnapshotFor() const;

    /// The term carried by any message, so the §5.1 rule can be applied once.
    [[nodiscard]] static Term TermOf(RaftMessage const& message) noexcept;

    RaftConfig _config;

    /// `_config.Peers()`, computed once.
    ///
    /// Derived rather than re-derived: the member set is fixed at construction,
    /// while broadcasting happens on every heartbeat — that is, on the interval
    /// that decides how fast a dead leader is noticed, so it is the one path here
    /// that runs at a rate worth caring about. Rebuilding the vector each time
    /// allocated once per heartbeat per node for a value that cannot change.
    std::vector<NodeId> _peers;

    IRandomSource& _random;

    Role _role { Role::Follower };
    Term _currentTerm {};
    std::optional<NodeId> _votedFor;
    std::optional<NodeId> _knownLeader;
    RaftLog _log;

    TimePoint _electionDeadline {};
    TimePoint _heartbeatDeadline {};

    LogIndex _commitIndex {}; ///< Highest index known committed.
    LogIndex _lastApplied {}; ///< Highest index already emitted as applied.

    /// Per-peer: the next index to send. A guess, revised downward on rejection.
    std::unordered_map<NodeId, LogIndex> _nextIndex;

    /// Per-peer: the highest index known to be replicated there.
    ///
    /// Distinct from `_nextIndex` and not derivable from it, which is the usual
    /// place to go wrong: `nextIndex` is optimistic and moves both ways, while
    /// `matchIndex` is a fact that only ever increases. Committing on the
    /// optimistic one would commit an entry nobody has acknowledged.
    std::unordered_map<NodeId, LogIndex> _matchIndex;

    /// Voters that granted this node their vote in the current term, itself
    /// included. A set rather than a counter because a retransmitted response
    /// would otherwise be counted twice, and two counted votes from one node is a
    /// quorum that does not exist.
    std::unordered_set<NodeId> _votesGranted;

    /// The configuration as of the snapshot.
    ///
    /// The fall-back for `RefreshConfiguration` when the retained log holds no
    /// configuration entry — which is precisely what compaction produces. Without
    /// it the fall-back is the *bootstrap* set, so a node that took part in a
    /// membership change and then compacted would forget it, silently and only
    /// after a restart.
    std::vector<NodeId> _snapshotMembers;

    /// The application state the log's discarded prefix produced.
    ///
    /// Held by the node rather than fetched from the application when needed,
    /// because it must be shippable to a follower at the moment that follower
    /// turns out to be behind — and asking the application for a snapshot *then*
    /// would produce one as of a different index than the log was compacted to.
    std::vector<std::byte> _snapshotState;

    /// The member set this node is operating under: the latest configuration in
    /// its log, or `_config.members` when the log holds none.
    ///
    /// Separate from `_config.members`, which stays the set this node was
    /// *bootstrapped* with. Keeping both is what lets a restart re-derive the
    /// active set from the log rather than silently reverting a change the
    /// cluster already made.
    std::vector<NodeId> _members;

    /// Peers that said an election would be winnable, itself included.
    ///
    /// Separate from `_votesGranted` rather than reusing it, because the two
    /// count answers to different questions in different terms: a pre-vote is
    /// about the term this node has NOT entered, and folding them would let a
    /// pre-vote be counted toward the real election that follows -- which is a
    /// vote nobody cast.
    std::unordered_set<NodeId> _preVotesGranted;
};

} // namespace FastCache::Consensus
