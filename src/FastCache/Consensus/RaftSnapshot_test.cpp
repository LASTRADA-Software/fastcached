// SPDX-License-Identifier: Apache-2.0
//
// Snapshots and log compaction. The log here carries cluster configuration and
// cluster state -- never cache entries -- so it grows slowly, but a log nobody
// ever trims is a restart that takes longer every time it happens and a leader
// that must keep every entry forever in case some follower is behind.
#include <FastCache/Consensus/IRaftStorage.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/IRandomSource.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

namespace
{

constexpr auto ElectionMin = 150ms;

/// A time point `millis` after the epoch.
/// @param millis Offset in milliseconds.
/// @return The instant.
[[nodiscard]] TimePoint At(std::int64_t millis)
{
    return TimePoint {} + std::chrono::milliseconds { millis };
}

/// A three-node configuration for `self`.
/// @param self Which member this node is.
/// @return The configuration.
[[nodiscard]] RaftConfig ThreeNodes(NodeId self = "n1")
{
    return RaftConfig { .self = std::move(self),
                        .members = { "n1", "n2", "n3" },
                        .electionTimeoutMin = ElectionMin,
                        .electionTimeoutMax = 300ms,
                        .heartbeatInterval = 50ms };
}

/// Messages of one alternative from an output.
/// @tparam T The message type.
/// @param output The output to scan.
/// @return Those messages, in order.
template <typename T>
[[nodiscard]] std::vector<T> MessagesOfType(RaftOutput const& output)
{
    std::vector<T> found;
    for (auto const& outbound: output.messages)
        if (auto const* const concrete = std::get_if<T>(&outbound.message); concrete != nullptr)
            found.push_back(*concrete);
    return found;
}

/// A leader of term 1 holding `count` committed command entries.
struct LeaderWithLog
{
    ScriptedRandomSource random { { 0 } };
    RaftNode node = std::move(RaftNode::Create(ThreeNodes(), random, TimePoint {})).value();

    explicit LeaderWithLog(std::size_t count)
    {
        (void) node.Tick(At(ElectionMin.count()));
        for (auto const* const voter: { "n2", "n3" })
            (void) node.Receive(
                PreVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = voter },
                At(ElectionMin.count()));
        (void) node.Receive(
            RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
            At(ElectionMin.count()));

        for (auto index = std::size_t { 0 }; index < count; ++index)
            (void) node.Propose(BytesFromString("v" + std::to_string(index)), At(200));

        Acknowledge(node.Log().LastIndex());
    }

    /// Have both peers acknowledge through `through`, committing it.
    /// @param through How far they match.
    void Acknowledge(LogIndex through)
    {
        for (auto const* const peer: { "n2", "n3" })
            (void) node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                        .result = AppendResult::Accepted,
                                                        .matchIndex = through,
                                                        .followerId = peer },
                                At(201));
    }
};

} // namespace

TEST_CASE("A compacted log still answers about its boundary", "[consensus][raft][snapshot]")
{
    // The one index whose entry is gone but whose TERM must survive: it is what
    // an AppendEntries spanning the boundary names as prevLogTerm, so without it
    // a leader could not prove its log matches at the only index the follower
    // cannot look up -- and every append across the boundary would be refused
    // forever.
    RaftLog log;
    for (auto term = std::uint64_t { 1 }; term <= 5; ++term)
        (void) log.Append(LogEntry { .term = Term { .value = term }, .kind = EntryKind::Command, .payload = {} });

    REQUIRE(log.Compact(LogIndex { .value = 3 }));

    CHECK(log.FirstIndex() == LogIndex { .value = 4 });
    CHECK(log.SnapshotIndex() == LogIndex { .value = 3 });
    CHECK(log.SnapshotTerm() == Term { .value = 3 });
    CHECK(log.LastIndex() == LogIndex { .value = 5 });

    // The boundary answers; everything below it is gone.
    CHECK(log.TermAt(LogIndex { .value = 3 }) == Term { .value = 3 });
    CHECK_FALSE(log.TermAt(LogIndex { .value = 2 }).has_value());
    CHECK(log.EntryAt(LogIndex { .value = 3 }) == nullptr);
    CHECK(log.EntryAt(LogIndex { .value = 4 }) != nullptr);

    // And an append across the boundary is accepted, which is the whole point.
    auto const entry = LogEntry { .term = Term { .value = 6 }, .kind = EntryKind::Command, .payload = {} };
    auto const outcome = log.TryAppend(LogIndex { .value = 5 }, Term { .value = 5 }, std::span { &entry, 1 });
    CHECK(outcome.result == AppendResult::Accepted);
}

TEST_CASE("A fully compacted log is not a fresh one", "[consensus][raft][snapshot]")
{
    // Reporting Term::None() with every entry discarded would make a fully
    // compacted node look brand new to §5.4.1, so any candidate at all would
    // out-rank it -- and a node holding the most state would be the one least
    // able to be elected.
    RaftLog log;
    (void) log.Append(LogEntry { .term = Term { .value = 7 }, .kind = EntryKind::Command, .payload = {} });
    REQUIRE(log.Compact(LogIndex { .value = 1 }));

    CHECK(log.LastIndex() == LogIndex { .value = 1 });
    CHECK(log.LastTerm() == Term { .value = 7 });

    // A candidate one term behind cannot out-rank it.
    CHECK_FALSE(log.CandidateIsAtLeastAsUpToDate(LogIndex { .value = 9 }, Term { .value = 6 }));
    CHECK(log.CandidateIsAtLeastAsUpToDate(LogIndex { .value = 1 }, Term { .value = 7 }));
}

TEST_CASE("Compaction refuses what it has no snapshot for", "[consensus][raft][snapshot]")
{
    RaftLog log;
    (void) log.Append(LogEntry { .term = Term { .value = 1 }, .kind = EntryKind::Command, .payload = {} });

    // Past the end: a caller asking to discard entries it holds no state for.
    CHECK_FALSE(log.Compact(LogIndex { .value = 5 }));

    REQUIRE(log.Compact(LogIndex { .value = 1 }));

    // Below the boundary: already gone, and re-compacting would move the term
    // backwards.
    CHECK_FALSE(log.Compact(LogIndex { .value = 1 }));
}

TEST_CASE("A truncation cannot reach into a snapshot", "[consensus][raft][snapshot]")
{
    // Those entries are committed by definition -- a snapshot is only ever taken
    // of applied state -- so a conflicting suffix must never be able to discard
    // them.
    RaftLog log;
    for (auto term = std::uint64_t { 1 }; term <= 3; ++term)
        (void) log.Append(LogEntry { .term = Term { .value = term }, .kind = EntryKind::Command, .payload = {} });
    REQUIRE(log.Compact(LogIndex { .value = 2 }));

    // A leader whose log disagrees below the boundary is refused rather than
    // allowed to truncate into it.
    auto const entry = LogEntry { .term = Term { .value = 9 }, .kind = EntryKind::Command, .payload = {} };
    auto const outcome = log.TryAppend(LogIndex { .value = 1 }, Term { .value = 1 }, std::span { &entry, 1 });
    CHECK(outcome.result == AppendResult::Rejected);
    CHECK(log.SnapshotIndex() == LogIndex { .value = 2 });
}

TEST_CASE("A node compacts only what the application has applied", "[consensus][raft][snapshot]")
{
    // An entry above lastApplied has not reached the application, so the snapshot
    // handed in does not describe it -- compacting past that point would replace
    // entries with a state that never included them.
    LeaderWithLog fix { 3 };
    REQUIRE(fix.node.LastApplied() > LogIndex::BeforeFirst());

    auto const applied = fix.node.LastApplied();
    REQUIRE(fix.node.CompactThroughApplied(BytesFromString("state")));
    CHECK(fix.node.SnapshotIndex() == applied);

    // Nothing left to discard, so a second attempt declines rather than moving
    // the boundary backwards.
    CHECK_FALSE(fix.node.CompactThroughApplied(BytesFromString("state")));
}

TEST_CASE("A leader sends a snapshot to a follower it can no longer replay to", "[consensus][raft][snapshot]")
{
    LeaderWithLog fix { 4 };
    REQUIRE(fix.node.CompactThroughApplied(BytesFromString("the-state")));
    REQUIRE(fix.node.SnapshotIndex() > LogIndex::BeforeFirst());

    // A follower rejects far enough back that what it needs is gone.
    for (auto attempt = 0; attempt < 10; ++attempt)
        (void) fix.node.Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                        .result = AppendResult::Rejected,
                                                        .matchIndex = LogIndex::BeforeFirst(),
                                                        .followerId = "n2" },
                                At(300));

    auto const beat = fix.node.Tick(At(100'000));
    auto const snapshots = MessagesOfType<InstallSnapshotRequest>(beat);
    REQUIRE_FALSE(snapshots.empty());
    CHECK(snapshots[0].lastIncludedIndex == fix.node.SnapshotIndex());
    CHECK(snapshots[0].state == BytesFromString("the-state"));

    // The configuration travels with it, because a follower catching up from a
    // snapshot has no entries left to learn the member set from.
    CHECK(snapshots[0].members == fix.node.ActiveMembers());
}

TEST_CASE("A follower adopts a snapshot it cannot replay to", "[consensus][raft][snapshot]")
{
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes("n2"), random, TimePoint {})).value();

    auto const output = node.Receive(InstallSnapshotRequest { .term = Term { .value = 4 },
                                                              .leaderId = "n1",
                                                              .lastIncludedIndex = LogIndex { .value = 9 },
                                                              .lastIncludedTerm = Term { .value = 3 },
                                                              .members = { "n1", "n2", "n3", "n4" },
                                                              .state = BytesFromString("caught-up") },
                                     At(10));

    CHECK(node.Log().SnapshotIndex() == LogIndex { .value = 9 });
    CHECK(node.Log().LastIndex() == LogIndex { .value = 9 });
    CHECK(node.CommitIndex() == LogIndex { .value = 9 });

    // The configuration came with it.
    CHECK(node.ActiveMembers().size() == 4);

    // The application is told to REPLACE, not to advance.
    REQUIRE(output.restoreSnapshot.has_value());
    CHECK(output.restoreSnapshot.value_or(SnapshotRestore {}).state == BytesFromString("caught-up"));
    CHECK(output.applied.empty());

    auto const replies = MessagesOfType<InstallSnapshotResponse>(output);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].result == AppendResult::Accepted);
    CHECK(replies[0].matchIndex == LogIndex { .value = 9 });
}

TEST_CASE("A stale snapshot does not roll a follower backwards", "[consensus][raft][snapshot]")
{
    // A duplicate or reordered snapshot must not discard a log holding MORE than
    // it covers: those entries have been acknowledged, and a leader may have
    // committed on that acknowledgement.
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes("n2"), random, TimePoint {})).value();

    (void) node.Receive(InstallSnapshotRequest { .term = Term { .value = 4 },
                                                 .leaderId = "n1",
                                                 .lastIncludedIndex = LogIndex { .value = 9 },
                                                 .lastIncludedTerm = Term { .value = 3 },
                                                 .members = { "n1", "n2", "n3" },
                                                 .state = BytesFromString("newer") },
                        At(10));
    REQUIRE(node.CommitIndex() == LogIndex { .value = 9 });

    auto const output = node.Receive(InstallSnapshotRequest { .term = Term { .value = 4 },
                                                              .leaderId = "n1",
                                                              .lastIncludedIndex = LogIndex { .value = 4 },
                                                              .lastIncludedTerm = Term { .value = 2 },
                                                              .members = { "n1", "n2", "n3" },
                                                              .state = BytesFromString("older") },
                                     At(11));

    CHECK(node.CommitIndex() == LogIndex { .value = 9 });
    CHECK(node.Log().SnapshotIndex() == LogIndex { .value = 9 });

    // Accepted rather than refused -- there is nothing wrong with the message,
    // this node simply already has more -- but nothing was restored.
    CHECK_FALSE(output.restoreSnapshot.has_value());
    auto const replies = MessagesOfType<InstallSnapshotResponse>(output);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].result == AppendResult::Accepted);
}

TEST_CASE("A snapshot from an older term is refused", "[consensus][raft][snapshot]")
{
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(ThreeNodes("n2"), random, TimePoint {})).value();

    // Reach term 5 first.
    (void) node.Receive(AppendEntriesRequest { .term = Term { .value = 5 },
                                               .leaderId = "n1",
                                               .prevLogIndex = LogIndex::BeforeFirst(),
                                               .prevLogTerm = Term::None(),
                                               .entries = {},
                                               .leaderCommit = LogIndex::BeforeFirst() },
                        At(10));

    auto const output = node.Receive(InstallSnapshotRequest { .term = Term { .value = 2 },
                                                              .leaderId = "n3",
                                                              .lastIncludedIndex = LogIndex { .value = 9 },
                                                              .lastIncludedTerm = Term { .value = 1 },
                                                              .members = { "n1", "n2", "n3" },
                                                              .state = BytesFromString("stale") },
                                     At(11));

    CHECK(node.Log().SnapshotIndex() == LogIndex::BeforeFirst());
    CHECK_FALSE(output.restoreSnapshot.has_value());

    auto const replies = MessagesOfType<InstallSnapshotResponse>(output);
    REQUIRE(replies.size() == 1);
    CHECK(replies[0].result == AppendResult::Rejected);
}

TEST_CASE("A snapshot acknowledgement moves the follower's progress", "[consensus][raft][snapshot]")
{
    // By exactly the rule an AppendEntries response uses rather than a second one
    // of its own, so a match index still only ever increases.
    LeaderWithLog fix { 3 };
    REQUIRE(fix.node.CompactThroughApplied(BytesFromString("state")));

    auto const covered = fix.node.SnapshotIndex();
    (void) fix.node.Receive(
        InstallSnapshotResponse {
            .term = Term { .value = 1 }, .result = AppendResult::Accepted, .matchIndex = covered, .followerId = "n2" },
        At(400));

    // The next AppendEntries for that peer starts after what the snapshot
    // covered, rather than at an index the log no longer holds.
    auto const beat = fix.node.Tick(At(100'000));
    auto const appends = MessagesOfType<AppendEntriesRequest>(beat);
    auto sawPeer = false;
    for (auto const& request: appends)
        if (request.prevLogIndex >= covered)
            sawPeer = true;
    CHECK(sawPeer);
}
