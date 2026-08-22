// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftLog.hpp>

#include <cstddef>
#include <ranges>
#include <utility>

namespace FastCache::Consensus
{

LogIndex RaftLog::LastIndex() const noexcept
{
    return LogIndex { .value = _entries.size() };
}

Term RaftLog::LastTerm() const noexcept
{
    return _entries.empty() ? Term::None() : _entries.back().term;
}

bool RaftLog::IsEmpty() const noexcept
{
    return _entries.empty();
}

bool RaftLog::Holds(LogIndex index) const noexcept
{
    return index != LogIndex::BeforeFirst() && index <= LastIndex();
}

std::optional<Term> RaftLog::TermAt(LogIndex index) const noexcept
{
    if (!Holds(index))
        return std::nullopt;
    return _entries[index.value - 1].term;
}

LogEntry const* RaftLog::EntryAt(LogIndex index) const noexcept
{
    if (!Holds(index))
        return nullptr;
    return &_entries[index.value - 1];
}

std::vector<LogEntry> RaftLog::EntriesFrom(LogIndex first) const
{
    if (!Holds(first))
        return {};
    return { _entries.begin() + static_cast<std::ptrdiff_t>(first.value - 1), _entries.end() };
}

LogIndex RaftLog::Append(LogEntry entry)
{
    _entries.push_back(std::move(entry));
    return LastIndex();
}

bool RaftLog::MatchesAt(LogIndex index, Term term) const noexcept
{
    // The empty prefix matches any term: a leader sending the very first entry
    // names it, and refusing that would leave a fresh follower unable to accept
    // anything at all.
    if (index == LogIndex::BeforeFirst())
        return true;

    auto const actual = TermAt(index);
    return actual.has_value() && *actual == term;
}

bool RaftLog::CandidateIsAtLeastAsUpToDate(LogIndex candidateLastIndex, Term candidateLastTerm) const noexcept
{
    // Lexicographic on (term, index), term first. Comparing index first is the
    // classic inversion, and it elects a node holding a long run of uncommitted
    // entries from a term that lost -- taking Leader Completeness with it.
    auto const ownLastTerm = LastTerm();
    if (candidateLastTerm != ownLastTerm)
        return candidateLastTerm > ownLastTerm;

    return candidateLastIndex >= LastIndex();
}

void RaftLog::TruncateFrom(LogIndex from) noexcept
{
    if (!Holds(from))
        return;
    _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(from.value - 1), _entries.end());
}

RaftLog::AppendOutcome RaftLog::TryAppend(LogIndex prevIndex, Term prevTerm, std::span<LogEntry const> entries)
{
    if (!MatchesAt(prevIndex, prevTerm))
        return AppendOutcome { .result = AppendResult::Rejected, .matchIndex = LogIndex::BeforeFirst() };

    // Find where the incoming run stops agreeing with what we already hold. Both
    // ways out of the loop leave `appendFrom` at the first entry to copy, so the
    // copy itself happens once, below -- the two arms differ only in whether a
    // conflicting suffix has to be dropped first.
    auto appendFrom = entries.size(); // every entry already present: copy nothing.
    for (auto const offset: std::views::iota(std::size_t { 0 }, entries.size()))
    {
        auto const target = prevIndex.Advanced(offset + 1);
        auto const existing = TermAt(target);

        // Past the end of what we hold: everything from here on is new.
        if (!existing.has_value())
        {
            appendFrom = offset;
            break;
        }

        // Same index and same term means the same entry, by Log Matching. Skip
        // it -- rewriting it would be a no-op, but truncating first would not,
        // and that is the mistake this loop exists to avoid.
        if (*existing == entries[offset].term)
            continue;

        // A genuine conflict: this entry and everything after it are from a term
        // that lost, so they are discarded and the leader's suffix replaces them.
        TruncateFrom(target);
        appendFrom = offset;
        break;
    }

    _entries.insert(_entries.end(), entries.begin() + static_cast<std::ptrdiff_t>(appendFrom), entries.end());

    // Deliberately what this request proved, not LastIndex(). A duplicate of an
    // older AppendEntries skips every entry it carries and leaves a longer log in
    // place; reporting LastIndex() would tell the leader the follower matches
    // further than this request established, and the leader counts match indices
    // to decide what is committed.
    return AppendOutcome { .result = AppendResult::Accepted, .matchIndex = prevIndex.Advanced(entries.size()) };
}

} // namespace FastCache::Consensus
