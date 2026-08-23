// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/RaftLog.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache::Consensus;

namespace
{

/// Build a log entry with a distinguishable payload, so a test can tell *which*
/// entry survived a truncation rather than merely how many did.
/// @param term Term to stamp the entry with.
/// @param tag Payload text, which is what identifies the entry.
/// @return The entry.
[[nodiscard]] LogEntry Entry(std::uint64_t term, std::string_view tag)
{
    return LogEntry { .term = Term { .value = term }, .payload = FastCache::BytesFromString(tag) };
}

/// The payload of the entry at `index`, as text, for identity assertions.
/// @param log The log to read.
/// @param index One-based position.
/// @return The payload as a string, or "<none>" when there is no such entry.
[[nodiscard]] std::string PayloadAt(RaftLog const& log, std::uint64_t index)
{
    auto const* const entry = log.EntryAt(LogIndex { .value = index });
    if (entry == nullptr)
        return "<none>";

    return std::string { FastCache::AsStringView(entry->payload) };
}

/// A log holding three entries: terms 1, 1, 2 tagged a, b, c.
/// @return The populated log.
[[nodiscard]] RaftLog ThreeEntryLog()
{
    RaftLog log;
    (void) log.Append(Entry(1, "a"));
    (void) log.Append(Entry(1, "b"));
    (void) log.Append(Entry(2, "c"));
    return log;
}

} // namespace

TEST_CASE("An empty log reports the before-first position", "[consensus][raft][log]")
{
    RaftLog const log;

    CHECK(log.IsEmpty());
    CHECK(log.LastIndex() == LogIndex::BeforeFirst());
    CHECK(log.LastTerm() == Term::None());
    CHECK_FALSE(log.TermAt(LogIndex { .value = 1 }).has_value());
    CHECK(log.EntryAt(LogIndex::BeforeFirst()) == nullptr);
}

TEST_CASE("Appending returns successive one-based indices", "[consensus][raft][log]")
{
    RaftLog log;

    CHECK(log.Append(Entry(1, "a")) == LogIndex { .value = 1 });
    CHECK(log.Append(Entry(1, "b")) == LogIndex { .value = 2 });
    CHECK(log.LastIndex() == LogIndex { .value = 2 });
    CHECK(log.LastTerm() == Term { .value = 1 });
    CHECK(PayloadAt(log, 1) == "a");
    CHECK(PayloadAt(log, 2) == "b");
}

TEST_CASE("EntriesFrom yields the suffix a leader would replicate", "[consensus][raft][log]")
{
    auto const log = ThreeEntryLog();

    CHECK(log.EntriesFrom(LogIndex { .value = 2 }).size() == 2);
    CHECK(log.EntriesFrom(LogIndex { .value = 4 }).empty());
    // Index zero names no entry, so there is no suffix starting at it.
    CHECK(log.EntriesFrom(LogIndex::BeforeFirst()).empty());
}

// --------------------------------------------------------------------------
// The consistency check (Raft §5.3).

TEST_CASE("The empty prefix matches any term", "[consensus][raft][log]")
{
    RaftLog const empty;

    // A leader sending the very first entry names prevLogIndex 0. Refusing that
    // would leave a fresh follower unable to accept anything at all.
    CHECK(empty.MatchesAt(LogIndex::BeforeFirst(), Term::None()));
    CHECK(empty.MatchesAt(LogIndex::BeforeFirst(), Term { .value = 7 }));
}

TEST_CASE("MatchesAt requires the term actually stored at the index", "[consensus][raft][log]")
{
    auto const log = ThreeEntryLog();

    CHECK(log.MatchesAt(LogIndex { .value = 2 }, Term { .value = 1 }));
    CHECK(log.MatchesAt(LogIndex { .value = 3 }, Term { .value = 2 }));
    CHECK_FALSE(log.MatchesAt(LogIndex { .value = 3 }, Term { .value = 1 }));
    // Past the end matches nothing: the follower simply does not have it yet.
    CHECK_FALSE(log.MatchesAt(LogIndex { .value = 4 }, Term { .value = 2 }));
}

TEST_CASE("TryAppend rejects when the previous entry is missing or differs", "[consensus][raft][log]")
{
    auto log = ThreeEntryLog();
    auto const incoming = std::vector { Entry(2, "d") };

    SECTION("previous index is past the end of this log")
    {
        auto const outcome = log.TryAppend(LogIndex { .value = 9 }, Term { .value = 2 }, incoming);
        CHECK(outcome.result == AppendResult::Rejected);
        CHECK(log.LastIndex() == LogIndex { .value = 3 });
    }

    SECTION("previous index is held but with a different term")
    {
        auto const outcome = log.TryAppend(LogIndex { .value = 3 }, Term { .value = 1 }, incoming);
        CHECK(outcome.result == AppendResult::Rejected);
        // A rejection must not mutate the log; the leader will retry further back.
        CHECK(log.LastIndex() == LogIndex { .value = 3 });
        CHECK(PayloadAt(log, 3) == "c");
    }
}

TEST_CASE("TryAppend accepts the first entry into an empty log", "[consensus][raft][log]")
{
    RaftLog log;
    auto const incoming = std::vector { Entry(1, "a"), Entry(1, "b") };

    auto const outcome = log.TryAppend(LogIndex::BeforeFirst(), Term::None(), incoming);

    CHECK(outcome.result == AppendResult::Accepted);
    CHECK(outcome.matchIndex == LogIndex { .value = 2 });
    CHECK(PayloadAt(log, 1) == "a");
    CHECK(PayloadAt(log, 2) == "b");
}

TEST_CASE("A heartbeat is accepted and changes nothing", "[consensus][raft][log]")
{
    auto log = ThreeEntryLog();

    auto const outcome = log.TryAppend(LogIndex { .value = 3 }, Term { .value = 2 }, {});

    CHECK(outcome.result == AppendResult::Accepted);
    CHECK(outcome.matchIndex == LogIndex { .value = 3 });
    CHECK(log.LastIndex() == LogIndex { .value = 3 });
}

// --------------------------------------------------------------------------
// Truncation: the two rules that decide whether Log Matching survives a network.

TEST_CASE("A conflicting term truncates from the conflict and appends the leader's suffix", "[consensus][raft][log]")
{
    auto log = ThreeEntryLog(); // terms 1,1,2 tagged a,b,c
    auto const incoming = std::vector { Entry(3, "C"), Entry(3, "D") };

    // The leader says index 3 should be term 3, not the term 2 we hold. Entry
    // "c" was from a term that lost, so it goes, and the leader's suffix
    // replaces it.
    auto const outcome = log.TryAppend(LogIndex { .value = 2 }, Term { .value = 1 }, incoming);

    CHECK(outcome.result == AppendResult::Accepted);
    CHECK(outcome.matchIndex == LogIndex { .value = 4 });
    CHECK(log.LastIndex() == LogIndex { .value = 4 });
    CHECK(PayloadAt(log, 1) == "a");
    CHECK(PayloadAt(log, 2) == "b");
    CHECK(PayloadAt(log, 3) == "C");
    CHECK(PayloadAt(log, 4) == "D");
}

TEST_CASE("Entries already present at the same term are skipped, not rewritten", "[consensus][raft][log]")
{
    auto log = ThreeEntryLog();

    // The leader re-sends entries 2 and 3, which we already hold at those terms.
    auto const incoming = std::vector { Entry(1, "b"), Entry(2, "c") };
    auto const outcome = log.TryAppend(LogIndex { .value = 1 }, Term { .value = 1 }, incoming);

    CHECK(outcome.result == AppendResult::Accepted);
    CHECK(log.LastIndex() == LogIndex { .value = 3 });
    CHECK(PayloadAt(log, 3) == "c");
}

TEST_CASE("A duplicated older AppendEntries does not delete newer entries", "[consensus][raft][log]")
{
    // This is the rule an unconditional truncate-at-prevIndex+1 gets wrong. It
    // passes every single-message test and then loses committed entries the
    // moment the network duplicates or reorders one, which is ordinary rather
    // than exotic.
    auto log = ThreeEntryLog(); // a, b, c at indices 1..3

    // A stale retransmission of "entry 2 only", arriving after 3 was accepted.
    auto const stale = std::vector { Entry(1, "b") };
    auto const outcome = log.TryAppend(LogIndex { .value = 1 }, Term { .value = 1 }, stale);

    CHECK(outcome.result == AppendResult::Accepted);
    // Entry 3 must survive: nothing about this message contradicted it.
    CHECK(log.LastIndex() == LogIndex { .value = 3 });
    CHECK(PayloadAt(log, 3) == "c");
}

TEST_CASE("An accepted append reports what the request proved, not the log's length", "[consensus][raft][log]")
{
    // The leader counts match indices to decide what is committed, so a match
    // index beyond what this request established would let it commit an entry
    // the follower's acknowledgement never covered.
    auto log = ThreeEntryLog();

    auto const stale = std::vector { Entry(1, "b") };
    auto const outcome = log.TryAppend(LogIndex { .value = 1 }, Term { .value = 1 }, stale);

    CHECK(outcome.matchIndex == LogIndex { .value = 2 });
    CHECK(log.LastIndex() == LogIndex { .value = 3 });
}

TEST_CASE("Log Matching holds after an accepted append", "[consensus][raft][log]")
{
    // "If two logs contain an entry with the same index and term, then the logs
    // are identical in all entries up through that index." Assert it directly:
    // build a leader log, replicate a suffix onto a divergent follower, and
    // require every prefix entry to agree.
    //
    // The divergence has to be at a *different term*, and that is not an
    // arbitrary choice of fixture. Two different entries sharing one (index,
    // term) is unreachable in Raft -- a term has at most one leader and a leader
    // writes each index once -- and `TryAppend`'s skip rule relies on exactly
    // that. A fixture violating it is asserting against a state the algorithm
    // never produces, and the first draft of this test did.
    RaftLog leader;
    for (auto const& entry: std::vector { Entry(1, "a"), Entry(2, "b"), Entry(3, "c"), Entry(3, "d") })
        (void) leader.Append(entry);

    // A follower that took "y" from the term-2 leader at index 3; the term-3
    // leader won without it, so index 3 onward diverges at a lower term.
    RaftLog follower;
    (void) follower.Append(Entry(1, "a"));
    (void) follower.Append(Entry(2, "b"));
    (void) follower.Append(Entry(2, "y"));

    // Backtrack to the last agreeing point, as a leader does on rejection.
    auto const rejected = follower.TryAppend(LogIndex { .value = 3 }, Term { .value = 3 }, {});
    REQUIRE(rejected.result == AppendResult::Rejected);

    auto const accepted =
        follower.TryAppend(LogIndex { .value = 2 }, Term { .value = 2 }, leader.EntriesFrom(LogIndex { .value = 3 }));
    REQUIRE(accepted.result == AppendResult::Accepted);

    REQUIRE(follower.LastIndex() == leader.LastIndex());
    for (auto const index: std::vector<std::uint64_t> { 1, 2, 3, 4 })
    {
        CHECK(follower.TermAt(LogIndex { .value = index }) == leader.TermAt(LogIndex { .value = index }));
        CHECK(PayloadAt(follower, index) == PayloadAt(leader, index));
    }
}

// --------------------------------------------------------------------------
// Up-to-dateness (Raft §5.4.1) — what Leader Completeness rests on.

TEST_CASE("A candidate with a later last term wins regardless of length", "[consensus][raft][log]")
{
    // The case that catches the classic inversion. The voter's log is longer,
    // but every entry past the candidate's came from a term that lost. Comparing
    // index before term grants this the wrong way round and elects a leader
    // missing a committed entry.
    RaftLog voter;
    (void) voter.Append(Entry(1, "a"));
    (void) voter.Append(Entry(1, "b"));
    (void) voter.Append(Entry(1, "c"));

    CHECK(voter.CandidateIsAtLeastAsUpToDate(LogIndex { .value = 1 }, Term { .value = 2 }));
}

TEST_CASE("A candidate with an earlier last term loses regardless of length", "[consensus][raft][log]")
{
    RaftLog voter;
    (void) voter.Append(Entry(2, "a"));

    CHECK_FALSE(voter.CandidateIsAtLeastAsUpToDate(LogIndex { .value = 9 }, Term { .value = 1 }));
}

TEST_CASE("At equal last terms the longer log wins", "[consensus][raft][log]")
{
    auto const voter = ThreeEntryLog(); // last is (index 3, term 2)

    CHECK(voter.CandidateIsAtLeastAsUpToDate(LogIndex { .value = 4 }, Term { .value = 2 }));
    CHECK(voter.CandidateIsAtLeastAsUpToDate(LogIndex { .value = 3 }, Term { .value = 2 }));
    CHECK_FALSE(voter.CandidateIsAtLeastAsUpToDate(LogIndex { .value = 2 }, Term { .value = 2 }));
}

TEST_CASE("Any candidate is up to date against an empty log", "[consensus][raft][log]")
{
    RaftLog const voter;

    CHECK(voter.CandidateIsAtLeastAsUpToDate(LogIndex::BeforeFirst(), Term::None()));
    CHECK(voter.CandidateIsAtLeastAsUpToDate(LogIndex { .value = 5 }, Term { .value = 3 }));
}
