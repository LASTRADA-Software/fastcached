// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftConfig.hpp>
#include <FastCache/Consensus/RaftLog.hpp>
#include <FastCache/Consensus/RaftOutput.hpp>
#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/IRandomSource.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
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
inline constexpr std::array<RoleTraits, 3> RoleTable { {
    { .role = Role::Follower, .name = "follower", .timer = TimerKind::Election },
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
    /// Restoring one from durable state is a separate entry point, added with the
    /// storage seam that produces such state.
    /// @param config The cluster configuration.
    /// @param random Source for election-timeout jitter; must outlive this node.
    /// @param now The current instant, used to arm the first election timeout.
    /// @return The node, or why the configuration was refused.
    [[nodiscard]] static std::expected<RaftNode, ConsensusError> Create(RaftConfig config,
                                                                        IRandomSource& random,
                                                                        TimePoint now);

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
    RaftNode(RaftConfig config, IRandomSource& random, TimePoint now);

    /// Whether `id` is a configured member of this cluster.
    ///
    /// Every identity in an incoming message is self-declared, and two of them
    /// decide something: a granted vote counts toward leadership, and an accepted
    /// AppendEntries publishes its sender as the leader clients are redirected to.
    /// @param id The claimed identity.
    /// @return True when the configuration contains it.
    [[nodiscard]] bool IsMember(NodeId const& id) const;

    /// Begin an election for the next term (§5.2).
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

    /// A heartbeat, i.e. an AppendEntries carrying no entries.
    [[nodiscard]] AppendEntriesRequest MakeHeartbeat() const;

    /// Record the durable state in `output`, for the driver to write first.
    void MarkPersist(RaftOutput& output) const;

    /// Per-message handlers; each may append to `output`.
    void OnRequestVote(RequestVoteRequest const& request, TimePoint now, RaftOutput& output);
    void OnRequestVoteResponse(RequestVoteResponse const& response, TimePoint now, RaftOutput& output);
    void OnAppendEntries(AppendEntriesRequest const& request, TimePoint now, RaftOutput& output);
    void OnAppendEntriesResponse(AppendEntriesResponse const& response, TimePoint now, RaftOutput& output);

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

    /// Voters that granted this node their vote in the current term, itself
    /// included. A set rather than a counter because a retransmitted response
    /// would otherwise be counted twice, and two counted votes from one node is a
    /// quorum that does not exist.
    std::unordered_set<NodeId> _votesGranted;
};

} // namespace FastCache::Consensus
