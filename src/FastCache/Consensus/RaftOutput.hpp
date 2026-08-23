// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>

#include <cstddef>
#include <optional>
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
using RaftMessage = std::variant<RequestVoteRequest, RequestVoteResponse, AppendEntriesRequest, AppendEntriesResponse>;

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
};

} // namespace FastCache::Consensus
