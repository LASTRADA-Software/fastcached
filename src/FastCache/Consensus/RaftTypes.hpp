// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
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
///
/// **ORDINALS ARE A TRANSMITTED AND PERSISTED CONTRACT. Append only; never insert or
/// reorder.** (#308) The enumerator's numeric value IS the entry's kind byte in both
/// directions: `FileRaftStorage` writes it into every log record on disk and
/// `RaftWire` puts it on the peer wire, and `DecodeWireEnum<EntryKind>` casts it back.
///
/// Inserting an enumerator mid-enum therefore does not fail to load. The bound moves
/// with the enum -- since #197 it is `Last`, derived, so not even a second edit is
/// needed -- the build is green, every test passes, and every record already in
/// `raft.log` comes back one kind along, so a committed `Configuration` reads as the
/// entry below it and the membership change is ordered, committed and never adopted.
/// A leader and a follower on either side of the change disagree about what every
/// frame means with nothing anywhere reporting a fault.
///
/// The `static_assert`s below are the enforcement rather than decoration: without them
/// a mid-enum insertion is one added line with nothing else visible. They pin what
/// TRAVELS and stop where the wire does -- `Last` is deliberately unpinned, because a
/// pinned sentinel is a second edit on every append and that is the whole of #197.
/// So an insertion FAILS THE BUILD and an append costs nothing, which is the way round
/// these two need to be.
enum class EntryKind : std::uint8_t
{
    Command,       ///< Application bytes, delivered through `RaftOutput::applied`.
    NoOp,          ///< Consensus' own; ordered and committed, but never delivered.
    Configuration, ///< The cluster's member set; adopted on append, never delivered.
    Last,          ///< Not a kind, has no ordinal on the wire: the count `DecodeWireEnum` bounds on.
};

static_assert(static_cast<std::uint8_t>(EntryKind::Command) == 0, "EntryKind ordinals are a wire contract");
static_assert(static_cast<std::uint8_t>(EntryKind::NoOp) == 1, "EntryKind ordinals are a wire contract");
static_assert(static_cast<std::uint8_t>(EntryKind::Configuration) == 2, "EntryKind ordinals are a wire contract");

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
    PreCandidate, ///< Asking whether an election could be won, without starting one.
    Candidate,    ///< Standing for election in its current term.
    Leader,       ///< Replicating entries and sending heartbeats.
    Last,         ///< Not a role, and has no row: `RoleTable`'s length.
};

/// Whether a vote was granted, as an `enum class` rather than a `bool`.
///
/// The wire calls this field `voteGranted` and a `bool` is what it holds there;
/// inside the library it is named after the decision, so a call site reading
/// `VoteDecision::Denied` cannot be misread as the success it is not.
///
/// **ORDINALS ARE A WIRE CONTRACT. Append only; never insert or reorder.** (#308) The
/// enumerator's value is the byte `RaftWire` puts on a vote response and
/// `DecodeWireEnum<VoteDecision>` casts back, between builds that upgrade at different
/// times. A swap here is a denied vote read as granted, which is two leaders.
enum class VoteDecision : std::uint8_t
{
    Denied,  ///< The vote was refused; the response's term says why it may have been.
    Granted, ///< The voter has committed its one vote for this term to the candidate.
    Last,    ///< Not a decision, and never travels. See `DecodeWireEnum`.
};

static_assert(static_cast<std::uint8_t>(VoteDecision::Denied) == 0, "VoteDecision ordinals are a wire contract");
static_assert(static_cast<std::uint8_t>(VoteDecision::Granted) == 1, "VoteDecision ordinals are a wire contract");

/// Whether a follower accepted an AppendEntries.
///
/// **ORDINALS ARE A WIRE CONTRACT. Append only; never insert or reorder.** (#308) The
/// enumerator's value is the byte `RaftWire` puts on an append response and
/// `DecodeWireEnum<AppendResult>` casts back. A swap here is a rejected append read as
/// accepted, so a leader advances `matchIndex` for a log that does not match.
enum class AppendResult : std::uint8_t
{
    Rejected, ///< Term too old, or the consistency check at `prevLogIndex` failed.
    Accepted, ///< The follower's log now matches the leader's through the entries sent.
    Last,     ///< Not a result, and never travels. See `DecodeWireEnum`.
};

static_assert(static_cast<std::uint8_t>(AppendResult::Rejected) == 0, "AppendResult ordinals are a wire contract");
static_assert(static_cast<std::uint8_t>(AppendResult::Accepted) == 1, "AppendResult ordinals are a wire contract");

/// Turn a byte that arrived from a peer or from disk into an enumerator.
///
/// Casting an arbitrary byte into an enumeration produces a value no `switch`
/// handles and no invariant covers, and the byte is not this process's to trust --
/// so an out-of-range one is a malformed record to refuse, never a precondition
/// to assert on.
///
/// **The bound is DERIVED from the enum's own `Last`, never named** (#197). What
/// this replaced was a `WireEnumBound<E>::Highest` trait naming the last enumerator
/// BY NAME, one specialization per enum -- the shape
/// `.agent/rules/build-and-toolchain.md` bans for a table length, arriving as a
/// bound. It fails CLOSED rather than open, which is why it was not a correctness
/// bug: append an enumerator, forget to raise the trait, and the new value is
/// REFUSED. But it is refused on a peer that understands it perfectly, with nothing
/// anywhere saying why -- a rolling upgrade where one side quietly rejects frames
/// the other treats as ordinary.
///
/// It also left the tree with TWO mechanisms for one job, and the better one was the
/// local copy: `Cluster::DecodeCommand` could not use the trait at all, because
/// `CommandKind` had gained a trailing `Last` for its `EnumTable` and specializing
/// `WireEnumBound` would have re-introduced the name anchor that change had just
/// removed (#159). So it open-coded `raw >= EnumeratorCount<CommandKind>` -- which is
/// exactly this, written out. That is now the only mechanism.
///
/// **`Last` never travels, and this is what enforces it**: the comparison is
/// `>=`, so `Last`'s own ordinal decodes to `nullopt` like any other byte naming no
/// enumerator. That answers the question #197 left open -- every wire enum here does
/// want `Last`, because the cost is one enumerator that is refused by construction
/// and the alternative is a second edit nothing reminds you to make.
///
/// **Every enum reaching here has ordinals that leave this process**, and the cast
/// below spells the enum as `E` -- so a census of `static_cast<SomeEnum>(byte)` finds
/// none of them, and the enums have to be reached by reading (#308). Each says so at
/// its own declaration; a new one must too.
///
/// @tparam E The enumeration, which must carry a trailing `Last`.
/// @param raw The byte as read.
/// @return The enumerator, or nullopt when the byte names none.
template <EnumWithLast E>
[[nodiscard]] constexpr std::optional<E> DecodeWireEnum(std::uint8_t raw) noexcept
{
    if (static_cast<std::size_t>(raw) >= EnumeratorCount<E>)
        return std::nullopt;
    return static_cast<E>(raw);
}

/// A node asking whether it *could* win an election, without starting one.
///
/// The pre-vote round (Ongaro's thesis §9.6) exists to stop a node that has been
/// partitioned away from disrupting a cluster that is working perfectly well.
/// Such a node times out, increments its term, times out again, and comes back
/// carrying a term far above everyone else's — at which point §5.1 obliges the
/// healthy leader to step down and the cluster runs an election it did not need.
/// The disruption is worst exactly when it is least welcome: the moment a
/// partition heals.
///
/// The fix is to ask first and change nothing. `term` is the term the sender
/// *would* use, one above its own — it has not adopted it and will not unless a
/// quorum says the election is winnable. A voter answering this **records
/// nothing**: no term change, no vote cast, no write. That is what makes the
/// round free, and it is why `PreVoteRequest` is exempt from the §5.1 term rule
/// that every other message obeys.
struct PreVoteRequest
{
    Term term {};             ///< The term the sender would move to; not yet adopted.
    NodeId candidateId;       ///< Who is asking.
    LogIndex lastLogIndex {}; ///< Index of the sender's last log entry.
    Term lastLogTerm {};      ///< Term of the sender's last log entry.
    /// Value equality, so a round trip can be asserted whole rather than
    /// field by field — which is what makes a transposed field visible.
    [[nodiscard]] bool operator==(PreVoteRequest const&) const = default;
};

/// A voter's answer to a pre-vote.
///
/// On a grant, `term` echoes the **request's** term rather than the voter's own.
/// That is not a slip: the pre-candidate is counting answers to a question about
/// a term it has not entered, and a response carrying a higher term would trip
/// the §5.1 rule and demote the very node the grant encourages. On a refusal
/// because the voter is ahead, `term` is the voter's own — which is exactly the
/// case where stepping down is right.
struct PreVoteResponse
{
    Term term {};             ///< The request's term when granted; the voter's when it is ahead.
    VoteDecision decision {}; ///< Whether an election would have this voter's support.
    NodeId voterId;           ///< Who answered, so the sender can count distinct answers.
    /// Value equality, so a round trip can be asserted whole rather than
    /// field by field — which is what makes a transposed field visible.
    [[nodiscard]] bool operator==(PreVoteResponse const&) const = default;
};

/// Candidate asking for a vote (Raft §5.2, §5.4.1).
struct RequestVoteRequest
{
    Term term {};             ///< The candidate's term.
    NodeId candidateId;       ///< Who is asking.
    LogIndex lastLogIndex {}; ///< Index of the candidate's last log entry.
    Term lastLogTerm {};      ///< Term of the candidate's last log entry.
    /// Value equality, so a round trip can be asserted whole rather than
    /// field by field — which is what makes a transposed field visible.
    [[nodiscard]] bool operator==(RequestVoteRequest const&) const = default;
};

/// A voter's answer.
struct RequestVoteResponse
{
    Term term {};             ///< The voter's current term, so a stale candidate steps down.
    VoteDecision decision {}; ///< Whether the vote was granted.
    NodeId voterId;           ///< Who answered, so the candidate can count distinct votes.
    /// Value equality, so a round trip can be asserted whole rather than
    /// field by field — which is what makes a transposed field visible.
    [[nodiscard]] bool operator==(RequestVoteResponse const&) const = default;
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
    /// Value equality, so a round trip can be asserted whole rather than
    /// field by field — which is what makes a transposed field visible.
    [[nodiscard]] bool operator==(AppendEntriesRequest const&) const = default;
};

/// A follower's answer.
struct AppendEntriesResponse
{
    Term term {};           ///< The follower's current term.
    AppendResult result {}; ///< Whether the entries were accepted.
    LogIndex matchIndex {}; ///< On acceptance, how far the follower now matches.
    NodeId followerId;      ///< Who answered.
    /// Value equality, so a round trip can be asserted whole rather than
    /// field by field — which is what makes a transposed field visible.
    [[nodiscard]] bool operator==(AppendEntriesResponse const&) const = default;
};

/// A leader handing a follower state it can no longer replay from the log.
///
/// Needed because a log that is compacted is a log a leader can no longer send
/// from: once the entries a lagging follower asks for have been discarded, the
/// only way to catch it up is to give it the state those entries produced.
///
/// Sent whole rather than chunked. That is a deliberate bound on what this log
/// may hold, not an oversight: it carries cluster configuration and cluster
/// state — never cache entries, which is the decision this whole feature rests
/// on — so a snapshot of it is kilobytes. A design that streamed would need
/// offsets, resumption and a partial-state file on the receiver, all to move
/// something smaller than one compile's output.
struct InstallSnapshotRequest
{
    Term term {};                  ///< The leader's term.
    NodeId leaderId;               ///< So a follower can redirect a client.
    LogIndex lastIncludedIndex {}; ///< Last index the snapshot covers.
    Term lastIncludedTerm {};      ///< Term of that index.
    std::vector<NodeId> members;   ///< The configuration as of that index.
    std::vector<std::byte> state;  ///< The application's own bytes, never interpreted.
    /// Value equality, so a round trip can be asserted whole rather than
    /// field by field — which is what makes a transposed field visible.
    [[nodiscard]] bool operator==(InstallSnapshotRequest const&) const = default;
};

/// A follower's answer to a snapshot.
///
/// Carries `matchIndex` rather than a bare acceptance so the leader can move
/// `nextIndex` forward exactly as an AppendEntries response does, instead of
/// keeping a second rule for how progress advances.
struct InstallSnapshotResponse
{
    Term term {};           ///< The follower's current term.
    AppendResult result {}; ///< Whether the snapshot was taken on.
    LogIndex matchIndex {}; ///< How far the follower now matches.
    NodeId followerId;      ///< Who answered.
    /// Value equality, so a round trip can be asserted whole rather than
    /// field by field — which is what makes a transposed field visible.
    [[nodiscard]] bool operator==(InstallSnapshotResponse const&) const = default;
};

} // namespace FastCache::Consensus
