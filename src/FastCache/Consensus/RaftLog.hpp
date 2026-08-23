// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace FastCache::Consensus
{

/// The replicated log and the matching rules that operate on it.
///
/// **Pure with respect to I/O**: it holds entries in memory, reads no clock and
/// touches no filesystem. Durability is a separate concern behind `IRaftStorage`,
/// for the reason `WorkerRegistry` and `LeaseTable` are pure — it is what lets
/// every rule below be an ordinary unit test instead of a fixture.
///
/// ## What this class is actually for
///
/// Not storage. The container is a `std::vector`; anybody could write that. What
/// lives here is Raft's **Log Matching** property (§5.3):
///
/// > If two logs contain an entry with the same index and term, then the logs are
/// > identical in all entries up through that index.
///
/// That property is not maintained by the vector. It is maintained by `TryAppend`
/// being the only way a follower's log can change, and by that function refusing
/// an append whose `prevLogIndex`/`prevLogTerm` do not already match. Every other
/// mutating entry point would be a hole in it, which is why there are none —
/// see *Why nothing here can truncate on demand* below.
///
/// ## Why nothing here can truncate on demand
///
/// Raft's **Leader Append-Only** property says a leader never overwrites or
/// deletes entries in its own log. A public `Truncate()` would make violating that
/// a one-line mistake, and the violation is invisible: the leader keeps serving,
/// and what goes missing is somebody else's committed entry. Truncation is
/// therefore private and reachable only from `TryAppend`, which performs it only
/// on a genuine term conflict. The public surface makes the property structural
/// rather than something a reviewer has to keep watch over.
class RaftLog
{
  public:
    /// What a follower-side append attempt did.
    struct AppendOutcome
    {
        AppendResult result {}; ///< Whether the consistency check passed.
        LogIndex matchIndex {}; ///< How far the log now provably matches the leader.
    };

    RaftLog() = default;

    /// Reconstruct a log from durable storage.
    ///
    /// Not a back door around the truncation rules above: recovery is the one
    /// moment there is no prior state to protect, because whatever this node knew
    /// is exactly what is being handed back. Every subsequent change still goes
    /// through `Append` or `TryAppend`.
    /// @param entries The stored entries, in index order from `firstIndex`.
    /// @param firstIndex Index of `entries[0]`; 1 for a log that was never
    ///        compacted.
    /// @param precedingTerm Term of the entry at `firstIndex - 1`, which the
    ///        snapshot covers and this log no longer holds.
    explicit RaftLog(std::vector<LogEntry> entries,
                     LogIndex firstIndex = LogIndex { .value = 1 },
                     Term precedingTerm = Term::None()) noexcept:
        _entries { std::move(entries) },
        _firstIndex { firstIndex.value == 0 ? LogIndex { .value = 1 } : firstIndex },
        _precedingTerm { precedingTerm }
    {
    }

    /// Index of the first entry this log physically holds.
    ///
    /// Above `LogIndex::BeforeFirst() + 1` once the log has been compacted:
    /// everything below is covered by a snapshot and is gone. A leader whose
    /// follower needs an index below this cannot replicate to it and must send
    /// the snapshot instead.
    /// @return The first index held.
    [[nodiscard]] LogIndex FirstIndex() const noexcept
    {
        return _firstIndex;
    }

    /// The last index covered by the snapshot, i.e. `FirstIndex() - 1`.
    /// @return The snapshot point.
    [[nodiscard]] LogIndex SnapshotIndex() const noexcept
    {
        return _firstIndex.Prev();
    }

    /// Term of the entry at `SnapshotIndex()`.
    ///
    /// Kept even though the entry itself is gone, because it is what an
    /// AppendEntries covering the boundary names as `prevLogTerm` — without it a
    /// leader could not prove its log matches at the one index the follower
    /// cannot look up.
    /// @return The preceding term.
    [[nodiscard]] Term SnapshotTerm() const noexcept
    {
        return _precedingTerm;
    }

    /// Discard every entry at or below `through`, keeping its term.
    ///
    /// The log carries cluster configuration and cluster state, so it grows
    /// slowly — but not never, and a log nobody ever trims is a restart that
    /// takes longer every time it happens.
    ///
    /// Refuses to discard anything not yet covered: the caller must have made a
    /// snapshot through `through` durable first, or compaction would throw away
    /// the only copy of entries a follower still needs.
    /// @param through Last index to discard; must be at or below `LastIndex()`.
    /// @return True when the log was compacted.
    bool Compact(LogIndex through) noexcept;

    /// Replace the whole log with a snapshot boundary.
    ///
    /// What a follower does when it receives a snapshot covering more than its
    /// log holds: everything it had is either included in the snapshot or from a
    /// term that lost, so there is nothing to keep.
    /// @param lastIncludedIndex Last index the snapshot covers.
    /// @param lastIncludedTerm Term of that index.
    void ResetToSnapshot(LogIndex lastIncludedIndex, Term lastIncludedTerm) noexcept;

    /// Index of the last entry, or `LogIndex::BeforeFirst()` when empty.
    /// @return The last index.
    [[nodiscard]] LogIndex LastIndex() const noexcept;

    /// Term of the last entry, or `Term::None()` when empty.
    /// @return The last term.
    [[nodiscard]] Term LastTerm() const noexcept;

    /// @return True when the log holds no entries.
    [[nodiscard]] bool IsEmpty() const noexcept;

    /// Term of the entry at `index`.
    /// @param index The position to look up.
    /// @return The term, or nullopt when `index` is zero or past the end.
    [[nodiscard]] std::optional<Term> TermAt(LogIndex index) const noexcept;

    /// The entry at `index`.
    ///
    /// Returns a pointer into the log's own storage, invalidated by the next
    /// mutation; callers copy what they need to keep.
    /// @param index The position to look up.
    /// @return The entry, or nullptr when `index` is zero or past the end.
    [[nodiscard]] LogEntry const* EntryAt(LogIndex index) const noexcept;

    /// Every entry from `first` through the end, for building an AppendEntries.
    /// @param first Index of the first entry to take; may be past the end.
    /// @return The entries, empty when `first` is past the end.
    [[nodiscard]] std::vector<LogEntry> EntriesFrom(LogIndex first) const;

    /// Append one entry, as a leader accepting a proposal.
    ///
    /// The leader-side path, and the only one that grows the log without a
    /// consistency check — because a leader's own log *is* the reference every
    /// other log is checked against.
    /// @param entry The entry to append.
    /// @return The index it landed at.
    LogIndex Append(LogEntry entry);

    /// Whether the entry at `index` carries `term`.
    ///
    /// `LogIndex::BeforeFirst()` matches any term: an AppendEntries carrying the
    /// very first entry names the empty prefix, and refusing that would leave a
    /// fresh follower unable to accept anything at all.
    /// @param index The position to test.
    /// @param term The term it must carry.
    /// @return True when they match.
    [[nodiscard]] bool MatchesAt(LogIndex index, Term term) const noexcept;

    /// Whether a candidate's log is at least as up to date as this one (§5.4.1).
    ///
    /// The comparison is lexicographic on `(lastTerm, lastIndex)`: a later last
    /// term wins outright regardless of length, and only equal terms compare by
    /// length. Getting this backwards — comparing index first — is what lets a
    /// node with a long stale log win an election, and Leader Completeness, the
    /// property that says a committed entry is present in every future leader's
    /// log, rests entirely on it. A vote is granted only when this returns true.
    /// @param candidateLastIndex The candidate's last log index.
    /// @param candidateLastTerm The candidate's last log term.
    /// @return True when the candidate may be voted for on log grounds.
    [[nodiscard]] bool CandidateIsAtLeastAsUpToDate(LogIndex candidateLastIndex, Term candidateLastTerm) const noexcept;

    /// Apply a leader's AppendEntries to this log, as a follower (§5.3).
    ///
    /// Rejects unless the entry at `prevIndex` already carries `prevTerm`, which
    /// is the check that maintains Log Matching. On acceptance, entries that are
    /// already present with the same term are **skipped rather than rewritten**,
    /// and only a genuine term conflict truncates.
    ///
    /// Skipping compares **terms only**, never payloads, and that is sound
    /// because a term has at most one leader and a leader writes each index once:
    /// one `(index, term)` pair therefore names one entry, fleet-wide. It is the
    /// same invariant Log Matching itself is stated in terms of, and it is worth
    /// knowing that it is being leaned on here — a test fixture that puts two
    /// different payloads at one `(index, term)` is describing a state Raft cannot
    /// reach, and will "catch" a defect that is not there.
    ///
    /// That distinction is the one to get right. Truncating unconditionally at
    /// `prevIndex + 1` looks equivalent and passes every single-message test, but
    /// a duplicated or reordered AppendEntries — ordinary on a network, not
    /// exotic — then deletes entries the follower had already accepted and
    /// acknowledged. If the leader had counted those acknowledgements toward a
    /// commit, the entry is committed and gone, which is the failure Raft exists
    /// to prevent.
    /// @param prevIndex Index immediately preceding `entries`.
    /// @param prevTerm Term the entry at `prevIndex` must carry.
    /// @param entries The entries to apply; empty for a heartbeat.
    /// @return Whether it was accepted, and how far the log now matches.
    [[nodiscard]] AppendOutcome TryAppend(LogIndex prevIndex, Term prevTerm, std::span<LogEntry const> entries);

  private:
    /// Drop the entry at `from` and every entry after it.
    ///
    /// Private by design; see *Why nothing here can truncate on demand* above.
    /// @param from First index to discard.
    void TruncateFrom(LogIndex from) noexcept;

    /// Whether `index` names an entry this log holds.
    /// @param index The position to test.
    /// @return True when `index` is in `[1, LastIndex()]`.
    [[nodiscard]] bool Holds(LogIndex index) const noexcept;

    /// Position of `index` within `_entries`, valid only when `Holds(index)`.
    /// @param index The log index.
    /// @return The vector offset.
    [[nodiscard]] std::size_t Offset(LogIndex index) const noexcept;

    /// Entries from `_firstIndex` onward; the log is one-based and may have been
    /// compacted, so a vector position is not a log index.
    std::vector<LogEntry> _entries;

    /// Index of `_entries[0]`. One until the log is compacted.
    LogIndex _firstIndex { .value = 1 };

    /// Term of the entry at `_firstIndex - 1`, which the snapshot covers.
    Term _precedingTerm {};
};

} // namespace FastCache::Consensus
