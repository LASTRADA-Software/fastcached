// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>

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

    /// Messages to send once `persist` is durable.
    std::vector<OutboundMessage> messages;
};

} // namespace FastCache::Consensus
