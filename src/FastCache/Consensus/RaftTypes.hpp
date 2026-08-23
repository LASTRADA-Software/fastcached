// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace FastCache::Consensus
{

/// A Raft election term: a logical clock that only ever moves forward.
///
/// A distinct type rather than a `std::uint64_t` alias, and this is the one place
/// in this module where that machinery earns itself. Every Raft RPC carries a term
/// and an index side by side — `prevLogIndex` next to `prevLogTerm`, `lastLogIndex`
/// next to `lastLogTerm` — with the same underlying type and adjacent names, which
/// is the textbook shape of a silently transposable argument. Confusing the two
/// does not fail loudly: it corrupts the up-to-dateness comparison that Raft's
/// Leader Completeness property rests on, and the symptom is a committed entry
/// disappearing under a zero exit code. `StreamId` (`Cache/StreamCodec.hpp`) is the
/// existing precedent for a value struct of this shape.
struct Term
{
    std::uint64_t value { 0 };

    /// Total ordering by value; terms are compared in every RPC.
    [[nodiscard]] constexpr auto operator<=>(Term const&) const noexcept = default;

    /// The term before any election has happened.
    ///
    /// Raft starts every node at term 0, and 0 is also what a log entry's term
    /// can never be, so it doubles as "no term" wherever one is optional.
    /// @return Term zero.
    [[nodiscard]] static constexpr Term None() noexcept
    {
        return Term { .value = 0 };
    }

    /// The next term, which is what a node moves to when it stands for election.
    /// @return This term plus one.
    [[nodiscard]] constexpr Term Next() const noexcept
    {
        return Term { .value = value + 1 };
    }
};

/// A one-based position in the replicated log.
///
/// One-based because Raft's `prevLogIndex` needs a value meaning "before the first
/// entry", and zero is it — see `BeforeFirst()`. A distinct type for the reason
/// `Term` is; see there.
struct LogIndex
{
    std::uint64_t value { 0 };

    /// Total ordering by value.
    [[nodiscard]] constexpr auto operator<=>(LogIndex const&) const noexcept = default;

    /// The position before the first entry, i.e. an empty log's last index.
    ///
    /// Not a sentinel bolted on: an AppendEntries carrying the very first entry
    /// has `prevLogIndex == BeforeFirst()`, so this is an ordinary value the
    /// consistency check is required to accept.
    /// @return Index zero.
    [[nodiscard]] static constexpr LogIndex BeforeFirst() noexcept
    {
        return LogIndex { .value = 0 };
    }

    /// The previous position, saturating at `BeforeFirst()`.
    ///
    /// Saturating rather than wrapping because the caller that walks a follower's
    /// `nextIndex` backwards on rejection would otherwise turn an off-by-one into
    /// an index near 2^64, and the resulting log lookup would miss rather than
    /// assert — a corruption that presents as a follower that never catches up.
    /// @return This index minus one, or zero.
    [[nodiscard]] constexpr LogIndex Prev() const noexcept
    {
        return value == 0 ? BeforeFirst() : LogIndex { .value = value - 1 };
    }

    /// This index advanced by `count` positions.
    /// @param count How many positions to advance.
    /// @return The advanced index.
    [[nodiscard]] constexpr LogIndex Advanced(std::uint64_t count) const noexcept
    {
        return LogIndex { .value = value + count };
    }
};

/// Stable identity of a cluster member.
///
/// A string rather than an integer because a node's identity has to survive a
/// restart and be meaningful in a log line an operator reads; PR 5 derives it
/// from the discovery handshake.
using NodeId = std::string;

/// What a log entry is for.
///
/// The consensus layer needs entries of its own — a new leader appends one to
/// establish its term — and those must not reach the application as commands.
/// Distinguishing them by a tag rather than by an empty payload is deliberate:
/// an empty payload is a legitimate thing for an application to commit, so
/// inferring the difference would make one indistinguishable from the other.
enum class EntryKind : std::uint8_t
{
    Command = 0, ///< Application bytes, delivered through `RaftOutput::applied`.
    NoOp,        ///< Consensus' own; ordered and committed, but never delivered.
};

/// One entry in the replicated log.
///
/// The payload is **opaque to consensus**, which is what makes this a generic
/// library rather than a part of the compile scheduler: Raft orders and commits
/// bytes and never asks what they mean. Cluster configuration and cluster state
/// are what this repository will put here; cache entries deliberately never are.
struct LogEntry
{
    Term term {};                          ///< Term of the leader that created this entry.
    EntryKind kind { EntryKind::Command }; ///< Whether the application ever sees it.
    std::vector<std::byte> payload;        ///< Application bytes; never interpreted here.

    /// Value equality, which the log-matching rules and their tests compare on.
    [[nodiscard]] bool operator==(LogEntry const&) const = default;
};

/// What role a node is currently playing.
///
/// `Follower` is zero because that is what a node starts as and what it returns to
/// whenever it sees a higher term, so a zero-initialized role already means the
/// right thing.
enum class Role : std::uint8_t
{
    Follower = 0, ///< Accepts entries; stands for election when it stops hearing a leader.
    Candidate,    ///< Standing for election in its current term.
    Leader,       ///< Replicating entries and sending heartbeats.
};

/// Whether a vote was granted, as an `enum class` rather than a `bool`.
///
/// The wire calls this field `voteGranted` and a `bool` is what it holds there;
/// inside the library it is named after the decision, so a call site reading
/// `VoteDecision::Denied` cannot be misread as the success it is not.
enum class VoteDecision : std::uint8_t
{
    Denied = 0, ///< The vote was refused; the response's term says why it may have been.
    Granted,    ///< The voter has committed its one vote for this term to the candidate.
};

/// Whether a follower accepted an AppendEntries.
enum class AppendResult : std::uint8_t
{
    Rejected = 0, ///< Term too old, or the consistency check at `prevLogIndex` failed.
    Accepted,     ///< The follower's log now matches the leader's through the entries sent.
};

/// Candidate asking for a vote (Raft §5.2, §5.4.1).
struct RequestVoteRequest
{
    Term term {};             ///< The candidate's term.
    NodeId candidateId;       ///< Who is asking.
    LogIndex lastLogIndex {}; ///< Index of the candidate's last log entry.
    Term lastLogTerm {};      ///< Term of the candidate's last log entry.
};

/// A voter's answer.
struct RequestVoteResponse
{
    Term term {};             ///< The voter's current term, so a stale candidate steps down.
    VoteDecision decision {}; ///< Whether the vote was granted.
    NodeId voterId;           ///< Who answered, so the candidate can count distinct votes.
};

/// Leader replicating entries, and — with `entries` empty — the heartbeat (§5.3).
struct AppendEntriesRequest
{
    Term term {};                  ///< The leader's term.
    NodeId leaderId;               ///< So a follower can redirect a client to the leader.
    LogIndex prevLogIndex {};      ///< Index immediately preceding `entries`.
    Term prevLogTerm {};           ///< Term of the entry at `prevLogIndex`.
    std::vector<LogEntry> entries; ///< Empty for a heartbeat.
    LogIndex leaderCommit {};      ///< The leader's commit index.
};

/// A follower's answer.
struct AppendEntriesResponse
{
    Term term {};           ///< The follower's current term.
    AppendResult result {}; ///< Whether the entries were accepted.
    LogIndex matchIndex {}; ///< On acceptance, how far the follower now matches.
    NodeId followerId;      ///< Who answered.
};

} // namespace FastCache::Consensus
