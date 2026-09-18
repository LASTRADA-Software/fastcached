// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace FastCache::Consensus
{

/// The state Raft requires on stable storage, other than the log itself.
///
/// Small, and that is the point: it is written on every term change and every
/// vote, so it sits on the latency path of an election.
struct PersistentState
{
    Term currentTerm {};            ///< Latest term this node has seen.
    std::optional<NodeId> votedFor; ///< Who it voted for in `currentTerm`, if anyone.

    /// Value equality, for tests and for skipping a redundant write.
    [[nodiscard]] bool operator==(PersistentState const&) const = default;
};

/// Any message one node sends another.
///
/// A variant rather than a base class with virtual dispatch: these are inert
/// data, the set is closed by the protocol rather than open for extension, and
/// `std::visit` over it gives exhaustiveness checking that a `switch` on a kind
/// tag would not.
using RaftMessage = std::variant<PreVoteRequest,
                                 PreVoteResponse,
                                 RequestVoteRequest,
                                 RequestVoteResponse,
                                 AppendEntriesRequest,
                                 AppendEntriesResponse,
                                 InstallSnapshotRequest,
                                 InstallSnapshotResponse>;

/// Who sent a message.
///
/// Every message carries exactly one member id and it is always the sender -- but
/// spelled four different ways, so this cannot be the one-line `std::visit` a term
/// read is. Detected rather than enumerated: eight near-identical arms differing only
/// in a field name is the copy-paste this codebase treats as a defect, and a ninth
/// message type naming its sender something else fails to compile here rather than
/// going quietly unattributed.
///
/// A free function rather than `RaftNode`'s, where it lived until #1308, because it now
/// has two readers asking one question: the node, to say what disturbed it, and the
/// peer server, to refuse a message that names a sender other than the one its
/// connection proved. Two spellings of which field is the sender would be two answers
/// that can drift, and the second one guards a trust boundary.
/// @param message The message.
/// @return The sender's id, borrowed from @p message.
[[nodiscard]] inline NodeId const& SenderOf(RaftMessage const& message) noexcept
{
    return std::visit(
        [](auto const& concrete) -> NodeId const& {
            if constexpr (requires { concrete.candidateId; })
                return concrete.candidateId;
            else if constexpr (requires { concrete.voterId; })
                return concrete.voterId;
            else if constexpr (requires { concrete.leaderId; })
                return concrete.leaderId;
            else
                return concrete.followerId;
        },
        message);
}

/// A change to the durable log: write `entries` at `fromIndex`, discarding
/// anything already at or after it.
///
/// Carries the entries rather than a bare "the log changed, go look": the driver
/// is asked to make something durable and must be told exactly what, and a
/// signal that referred back to the node's in-memory log would be racing the
/// next event to read it.
///
/// `fromIndex` expresses truncation as well as appending, which is why it is an
/// index and not a count. A follower repairing a divergent suffix writes a
/// `fromIndex` at or below what it already has, and the storage must discard the
/// old tail — otherwise a restart would recover entries the cluster overwrote.
struct LogAppend
{
    LogIndex fromIndex {};         ///< First index these entries occupy.
    std::vector<LogEntry> entries; ///< Entries to write, in index order.
};

/// A committed entry, ready for the application to act on.
///
/// Carries the payload by value rather than an index into the log: an entry is
/// applied once and the driver may hand it straight to the application, while a
/// reference into a log that a later append can truncate is a lifetime problem
/// dressed as an optimization.
struct AppliedEntry
{
    LogIndex index {};              ///< Where it sits in the log.
    std::vector<std::byte> payload; ///< The application bytes, verbatim.
};

/// One message, addressed.
struct OutboundMessage
{
    NodeId to;           ///< The member to send it to.
    RaftMessage message; ///< What to send.
};

/// A peer's higher term arrived and demoted this node (§5.1).
///
/// The one event in this algorithm that ends a leadership without this node
/// deciding anything, and until it was reported a log dump could show only
/// *that* leadership moved. Which peer carried the term is the half that names a
/// disruptor; the previous term and role are what tell an election storm apart
/// from one node returning from a partition.
///
/// Carried out through the output channel rather than logged where it happens,
/// for the reason `persist` is: `RaftNode` holds no logger, reads no clock and
/// is a pure function of (state, event) — a diagnostic that reached out from
/// inside it would be the one side effect a simulation could not observe.
struct TermAdoption
{
    Term previousTerm {}; ///< What this node held before adopting.
    Role previousRole {}; ///< What it was playing, so a deposed leader stands out.
    NodeId from;          ///< The peer whose message carried the higher term.
};

/// A snapshot the application must adopt wholesale.
///
/// Emitted when a follower receives state covering more than its log holds. The
/// application cannot replay its way there — the entries that would have taken
/// it are gone from every node that compacted them — so it replaces its state
/// rather than advancing it.
struct RaftSnapshot
{
    LogIndex lastIncludedIndex {}; ///< The index the state is as of.
    Term lastIncludedTerm {};      ///< Term of that index.
    Configuration configuration;   ///< The configuration as of that index, voters and learners.
    std::vector<std::byte> state;  ///< The application's own bytes, never interpreted here.

    /// Value equality, for tests and for skipping a redundant write.
    [[nodiscard]] bool operator==(RaftSnapshot const&) const = default;
};

/// Why this node DENIED a vote or a pre-vote it was asked for.
///
/// **Private: never transmitted and never persisted**, so the enumerators carry no
/// explicit values. The wire says `VoteDecision::Denied` and nothing else, and a
/// candidate needs nothing else -- a refusal is a refusal to it. What needs the reason
/// is whoever reads THIS node: a test asserting which rule refused, and a driver that
/// reports it.
///
/// It exists because a learner asked for a vote must refuse BY ROW rather than by
/// silence (#1449). Silence is a node that does not answer, which a candidate cannot
/// tell from a lost message; a bare `Denied` is an answer whose reason no test can
/// assert, and every one of the refusals below produces the same `Denied`. So each
/// refusal is a row, and a case asserts the row it expects rather than a decision
/// several rules share.
enum class VoteRefusal : std::uint8_t
{
    CastsNoVote,        ///< This node's standing grants no vote: a learner, or a node with no cluster.
    StaleTerm,          ///< The request's term is behind this node's.
    CandidateNotAVoter, ///< The candidate is not a voter in this node's configuration.
    LeaderLive,         ///< A leader is demonstrably live, so there is no election to support.
    AlreadyVoted,       ///< This node's one vote for the term is promised to somebody else.
    LogBehind,          ///< The candidate's log is behind this node's (§5.4.1).
    Last,               ///< Not a refusal, and has no row: `VoteRefusalTable`'s length.
};

/// How one `VoteRefusal` is spelled.
struct VoteRefusalRow
{
    VoteRefusal refusal {}; ///< The refusal this row describes.
    std::string_view name;  ///< For a log line and a test failure message.
};

/// One row per `VoteRefusal`, in enumerator order.
inline constexpr EnumTable<VoteRefusal, VoteRefusalRow> VoteRefusalTable { {
    { .refusal = VoteRefusal::CastsNoVote, .name = "casts no vote" },
    { .refusal = VoteRefusal::StaleTerm, .name = "stale term" },
    { .refusal = VoteRefusal::CandidateNotAVoter, .name = "candidate is not a voter" },
    { .refusal = VoteRefusal::LeaderLive, .name = "a leader is live" },
    { .refusal = VoteRefusal::AlreadyVoted, .name = "already voted" },
    { .refusal = VoteRefusal::LogBehind, .name = "candidate's log is behind" },
} };

static_assert(RowsInEnumeratorOrder(VoteRefusalTable, &VoteRefusalRow::refusal),
              "VoteRefusalTable must hold one row per VoteRefusal, in enumerator order");

/// Everything a node wants done as a result of one event.
///
/// `RaftNode` performs no I/O, so this is how it asks for I/O to be performed.
/// Returning the actions rather than invoking them through a sink interface is
/// what makes the state machine a pure function of (state, event) — which is
/// what lets a whole cluster be simulated deterministically — and it is also the
/// only shape that can express the ordering rule below, because a sink's
/// `Persist()` cannot be awaited from inside a synchronous transition.
///
/// ## The ordering is not advisory
///
/// A driver **must** make `persist` durable before putting any of `messages` on
/// the wire. Raft's one-vote-per-term rule is only as good as that write: a node
/// that answers a RequestVote and then crashes before the vote reaches stable
/// storage comes back up believing it has not voted, votes again in the same
/// term for a different candidate, and two leaders are elected for one term.
/// Every guarantee in the algorithm rests on there being at most one.
///
/// The same applies to a term change: replying with a term that was never
/// persisted lets the node come back at an older term and accept an entry it has
/// already told a leader it would not.
struct RaftOutput
{
    /// State to make durable before anything is sent; absent when unchanged.
    ///
    /// Optional so the common case — a heartbeat that changes no durable state —
    /// costs no write. An `fsync` per heartbeat would put a disk flush on the
    /// interval that decides how fast the cluster notices a dead leader.
    std::optional<PersistentState> persist;

    /// Log entries to make durable, before `messages` go out and under the same
    /// rule.
    ///
    /// Separate from `persist` because the two change at different rates and for
    /// different reasons: term and vote move once per election, while the log
    /// moves on every proposal. Folding the log into `PersistentState` would mean
    /// rewriting the whole of it whenever a vote changed, and carrying nothing
    /// here at all — which an earlier draft of this phase did — leaves a leader
    /// replicating an entry the driver was never told to write, so a crash
    /// between sending and the next durable write loses an entry other nodes
    /// already have.
    std::optional<LogAppend> persistLog;

    /// Messages to send once `persist` is durable.
    std::vector<OutboundMessage> messages;

    /// Entries that have become committed, in index order, for the driver to hand
    /// to the application state machine.
    ///
    /// Committed means "will be present in every future leader's log", which is
    /// the only point at which it is safe to act on an entry. Applied **after**
    /// `messages` go out: sending is what lets other nodes make progress, while
    /// applying is local, so doing it first would add the application's latency to
    /// the replication path for no gain.
    ///
    /// Each **command** entry is emitted exactly once across all outputs from one
    /// node, in ascending index order — the driver may apply them blindly rather
    /// than tracking what it has already seen. Consensus' own entries
    /// (`EntryKind::NoOp`) are committed like any other but never delivered, so
    /// the indices seen here can skip one.
    std::vector<AppliedEntry> applied;

    /// A snapshot to make durable, before anything is sent and under the same
    /// rule; absent normally.
    ///
    /// Carried through the output channel rather than written by whoever asked
    /// for the compaction, for the reason `persist` and `persistLog` are: it is a
    /// durability write that has to be ordered against the messages, and a node
    /// that acknowledged a snapshot it had not written would retract that
    /// acknowledgement on restart -- after a leader may have committed on it.
    std::optional<RaftSnapshot> saveSnapshot;

    /// State the application must adopt in place of its own; absent normally.
    ///
    /// Delivered instead of `applied` rather than alongside it: the two are
    /// alternative ways to reach the same point, and handing over both would
    /// have the application replay entries the snapshot already includes.
    std::optional<RaftSnapshot> restoreSnapshot;

    /// Why this node's role moved, when a peer's higher term moved it; absent
    /// otherwise.
    ///
    /// Nothing to *do*, unlike every field above it — which is the point of
    /// putting it here rather than inventing a second channel. A driver that
    /// ignores it is still correct; one that reports it turns "leadership moved"
    /// into "leadership moved because n3 arrived holding term 2", which is the
    /// difference between a log that can be read after the fact and one that
    /// cannot.
    std::optional<TermAdoption> adoptedTerm;

    /// Which rule denied the vote or pre-vote this event answered; absent when it
    /// granted one or answered none.
    ///
    /// Nothing to DO, like `adoptedTerm`: the `Denied` itself is already in
    /// `messages`. What this adds is the reason, which the wire does not carry and a
    /// candidate does not need -- see `VoteRefusal`. A driver that ignores it is still
    /// correct; a case that asserts it can say which rule refused rather than only
    /// that one did.
    std::optional<VoteRefusal> voteRefusal;

    /// The index of a leader's snapshot this node would not take on, because its
    /// application cannot read the state (#1552); absent otherwise.
    ///
    /// Nothing to DO -- the `Rejected` answer is already in `messages`, and nothing was
    /// installed, persisted or applied. What it adds is that the refusal happened, which
    /// the driver turns into a report an operator can see: a node refusing every snapshot
    /// its leader sends is a node that will never catch up until something changes, and
    /// from the outside it looks exactly like a slow one.
    std::optional<LogIndex> refusedSnapshot;
};

} // namespace FastCache::Consensus
