// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/InMemoryRaftStorage.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Crc32c.hpp>
#include <FastCache/Core/Endian.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;
using FastCache::Testing::ScratchDirectory;
using FastCache::Testing::Unwrap;

namespace
{

/// A log entry with a recognisable payload.
/// @param term Term to stamp it with.
/// @param tag Payload text.
/// @return The entry.
[[nodiscard]] LogEntry Entry(std::uint64_t term, std::string_view tag)
{
    return LogEntry { .term = Term { .value = term },
                      .kind = EntryKind::Command,
                      .payload = FastCache::BytesFromString(tag) };
}

/// Open a file store, failing the test if it cannot be opened.
/// @param directory Where it lives.
/// @return The store.
[[nodiscard]] FileRaftStorage OpenStore(std::filesystem::path const& directory)
{
    return std::move(FileRaftStorage::Open(directory)).value();
}

} // namespace

TEST_CASE("An empty store loads as a node that has never run", "[consensus][raft][storage]")
{
    // Not an error: starting for the first time is the ordinary case, and
    // reporting it as a failure would make a first start indistinguishable from
    // unreadable state.
    InMemoryRaftStorage memory;
    ScratchDirectory scratch { "fc-raft-store" };
    auto file = OpenStore(scratch.Path());

    for (IRaftStorage* store: { static_cast<IRaftStorage*>(&memory), static_cast<IRaftStorage*>(&file) })
    {
        auto const loaded = store->Load();
        REQUIRE(loaded.has_value());
        CHECK(loaded->state.currentTerm == Term::None());
        CHECK_FALSE(loaded->state.votedFor.has_value());
        CHECK(loaded->entries.empty());
    }
}

TEST_CASE("Term and vote survive a round trip", "[consensus][raft][storage]")
{
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(store.SaveState(PersistentState { .currentTerm = Term { .value = 7 }, .votedFor = "n3" }).has_value());
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    CHECK(loaded->state.currentTerm == Term { .value = 7 });
    CHECK(loaded->state.votedFor == std::optional<NodeId> { "n3" });
}

TEST_CASE("A vote for nobody is distinct from a vote for the empty string", "[consensus][raft][storage]")
{
    // Inferring absence from a zero length would make the two identical on disk,
    // and a node that recovered "voted for nobody" as a vote could vote again in
    // the same term.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(
            store.SaveState(PersistentState { .currentTerm = Term { .value = 2 }, .votedFor = std::string {} }).has_value());
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->state.votedFor.has_value());
    CHECK(loaded->state.votedFor.value_or(std::string { "absent" }).empty());
}

TEST_CASE("Log entries survive a round trip", "[consensus][raft][storage]")
{
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(store
                    .SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 },
                                         .entries = { Entry(1, "a"), Entry(1, "b"), Entry(2, "c") } })
                    .has_value());
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 3);
    CHECK(loaded->entries[0].term == Term { .value = 1 });
    CHECK(FastCache::AsStringView(loaded->entries[2].payload) == "c");
    CHECK(loaded->entries[2].term == Term { .value = 2 });
}

TEST_CASE("An entry's kind survives a round trip", "[consensus][raft][storage]")
{
    // A no-op recovered as a command would be delivered to the application, which
    // cannot interpret it.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(store
                    .SaveLog(LogAppend {
                        .fromIndex = LogIndex { .value = 1 },
                        .entries = { LogEntry { .term = Term { .value = 4 }, .kind = EntryKind::NoOp, .payload = {} } } })
                    .has_value());
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 1);
    CHECK(loaded->entries[0].kind == EntryKind::NoOp);
}

TEST_CASE("A truncating append does not leave the overwritten tail behind", "[consensus][raft][storage]")
{
    // A store that only appended would recover entries the cluster had
    // overwritten, which is a divergent log rather than a lost one.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(store
                    .SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 },
                                         .entries = { Entry(1, "a"), Entry(1, "b"), Entry(1, "c") } })
                    .has_value());
        REQUIRE(store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 2 }, .entries = { Entry(3, "B") } }).has_value());
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 2);
    CHECK(FastCache::AsStringView(loaded->entries[0].payload) == "a");
    CHECK(FastCache::AsStringView(loaded->entries[1].payload) == "B");
}

TEST_CASE("The in-memory store truncates the same way", "[consensus][raft][storage]")
{
    InMemoryRaftStorage store;
    REQUIRE(store
                .SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 },
                                     .entries = { Entry(1, "a"), Entry(1, "b"), Entry(1, "c") } })
                .has_value());
    REQUIRE(store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 2 }, .entries = { Entry(3, "B") } }).has_value());

    CHECK(store.StoredEntryCount() == 2);
}

TEST_CASE("A torn record at the tail is discarded, not refused", "[consensus][raft][storage]")
{
    // A record still being written was never acknowledged to a leader, so nobody
    // can have committed on it -- discarding it is correct rather than lenient,
    // and refusing the whole log would strand a node that merely lost power.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(
            store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 }, .entries = { Entry(1, "a"), Entry(1, "b") } })
                .has_value());
    }

    // Simulate a crash mid-write by appending a partial record.
    auto const logPath = scratch.Path() / "raft-log";
    {
        std::ofstream out { logPath, std::ios::binary | std::ios::app };
        REQUIRE(out.is_open());
        char const garbage[] = { 'F', 'C', 'R', 'L', 0x00, 0x00 };
        out.write(garbage, sizeof(garbage));
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    CHECK(loaded->entries.size() == 2);
}

TEST_CASE("A corrupt state record is refused rather than read as a fresh start", "[consensus][raft][storage]")
{
    // Mistaking it for a fresh start would discard a vote this node had already
    // given, which is how one term gets two leaders.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(store.SaveState(PersistentState { .currentTerm = Term { .value = 3 }, .votedFor = "n2" }).has_value());
    }

    auto const statePath = scratch.Path() / "raft-state";
    {
        // Flip a byte in the middle of the record; the trailing CRC no longer
        // matches what precedes it.
        std::fstream inout { statePath, std::ios::binary | std::ios::in | std::ios::out };
        REQUIRE(inout.is_open());
        inout.seekp(8);
        char const flipped = 0x7F;
        inout.write(&flipped, 1);
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().code == ConsensusErrorCode::StorageFailure);
}

TEST_CASE("An injected storage failure is reported, not swallowed", "[consensus][raft][storage]")
{
    InMemoryRaftStorage store { InMemoryRaftStorage::FailurePlan { .failNthSaveState = 2 } };

    CHECK(store.SaveState(PersistentState {}).has_value());

    auto const second = store.SaveState(PersistentState {});
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().code == ConsensusErrorCode::StorageFailure);

    // Only the nominated call fails; the store keeps working afterwards.
    CHECK(store.SaveState(PersistentState {}).has_value());
}

TEST_CASE("A node recovers its term and vote across a restart", "[consensus][raft][storage]")
{
    // The property the whole storage layer exists for: a node that answered a
    // RequestVote and then restarted must not vote again in the same term.
    ScratchDirectory scratch { "fc-raft-store" };
    auto store = OpenStore(scratch.Path());

    auto const config = RaftConfig { .self = "n1",
                                     .voters = { "n1", "n2", "n3" },
                                     .learners = {},
                                     .electionTimeoutMin = 150ms,
                                     .electionTimeoutMax = 300ms,
                                     .heartbeatInterval = 50ms };

    ScriptedRandomSource random { { 0 } };
    {
        auto node = std::move(RaftNode::Create(config, random, TimePoint {})).value();
        auto const output = node.Receive(RequestVoteRequest { .term = Term { .value = 5 },
                                                              .candidateId = "n2",
                                                              .lastLogIndex = LogIndex::BeforeFirst(),
                                                              .lastLogTerm = Term::None() },
                                         TimePoint {});
        REQUIRE(output.persist.has_value());
        REQUIRE(store.SaveState(output.persist.value_or(PersistentState {})).has_value());
    }

    // The process restarts: a new node, the same store.
    auto const recovered = store.Load();
    REQUIRE(recovered.has_value());
    auto restarted = std::move(RaftNode::Create(config, random, TimePoint {}, *recovered)).value();

    CHECK(restarted.CurrentTerm() == Term { .value = 5 });
    CHECK(restarted.VotedFor() == std::optional<NodeId> { "n2" });
    // A recovered node comes back a follower whatever it was: role is not durable
    // state, and resuming as a leader would be a second leader for a term that has
    // since moved on.
    CHECK(restarted.CurrentRole() == Role::Follower);

    // And it refuses a different candidate in that same term.
    auto const output = restarted.Receive(RequestVoteRequest { .term = Term { .value = 5 },
                                                               .candidateId = "n3",
                                                               .lastLogIndex = LogIndex::BeforeFirst(),
                                                               .lastLogTerm = Term::None() },
                                          TimePoint {});

    auto denied = false;
    for (auto const& outbound: output.messages)
        if (auto const* const response = std::get_if<RequestVoteResponse>(&outbound.message))
            denied = response->decision == VoteDecision::Denied;

    CHECK(denied);
}

TEST_CASE("A node recovers its log across a restart", "[consensus][raft][storage]")
{
    ScratchDirectory scratch { "fc-raft-store" };
    auto store = OpenStore(scratch.Path());
    REQUIRE(store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 }, .entries = { Entry(1, "a"), Entry(2, "b") } })
                .has_value());

    auto const recovered = store.Load();
    REQUIRE(recovered.has_value());

    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(RaftConfig { .self = "n1",
                                                        .voters = { "n1", "n2", "n3" },
                                                        .learners = {},
                                                        .electionTimeoutMin = 150ms,
                                                        .electionTimeoutMax = 300ms,
                                                        .heartbeatInterval = 50ms },
                                           random,
                                           TimePoint {},
                                           *recovered))
                    .value();

    CHECK(node.Log().LastIndex() == LogIndex { .value = 2 });
    CHECK(node.Log().LastTerm() == Term { .value = 2 });
}

TEST_CASE("A second append lands after the first, not on top of it", "[consensus][raft][storage]")
{
    // The case that was missing, and the reason a critical defect survived: every
    // other case here either starts at index 1 or truncates inside the existing
    // range, so none of them ever asked where index N+1 begins. Answering that
    // with the last record's start -- which is what an offset table without an
    // end sentinel can do -- overwrites the entry before it.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(
            store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 }, .entries = { Entry(1, "a"), Entry(1, "b") } })
                .has_value());
        REQUIRE(store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 3 }, .entries = { Entry(1, "c") } }).has_value());
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 3);
    CHECK(FastCache::AsStringView(loaded->entries[0].payload) == "a");
    CHECK(FastCache::AsStringView(loaded->entries[1].payload) == "b");
    CHECK(FastCache::AsStringView(loaded->entries[2].payload) == "c");
}

TEST_CASE("Appending one entry at a time builds the whole log", "[consensus][raft][storage]")
{
    // What a leader actually does: RecordLogAppend names the single index just
    // written, so every proposal after the first is an append past the end.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        for (auto const index: std::views::iota(std::uint64_t { 1 }, std::uint64_t { 6 }))
            REQUIRE(store
                        .SaveLog(LogAppend { .fromIndex = LogIndex { .value = index },
                                             .entries = { Entry(1, std::string { "e" } + std::to_string(index)) } })
                        .has_value());
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 5);
    CHECK(FastCache::AsStringView(loaded->entries[0].payload) == "e1");
    CHECK(FastCache::AsStringView(loaded->entries[4].payload) == "e5");
}

TEST_CASE("A same-length replacement leaves no readable old record behind", "[consensus][raft][storage]")
{
    // The case that makes write-then-truncate unsafe. When the replacement is the
    // same encoded length as what it replaces -- routine for a no-op, common for
    // same-size commands -- the bytes after the new tail are an intact old record
    // with valid magic and a valid CRC, so a crash before the truncation would
    // recover an entry the cluster had deleted.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(store
                    .SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 },
                                         .entries = { Entry(1, "aa"), Entry(1, "bb"), Entry(1, "cc") } })
                    .has_value());
        // Exactly the same encoded size as "bb", replacing it and dropping "cc".
        REQUIRE(
            store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 2 }, .entries = { Entry(4, "BB") } }).has_value());
    }

    // The file itself must be short, not merely parsed short: a longer file whose
    // tail happens to parse is what this ordering exists to rule out.
    auto const logSize = std::filesystem::file_size(scratch.Path() / "raft-log");

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 2);
    CHECK(FastCache::AsStringView(loaded->entries[1].payload) == "BB");
    CHECK(loaded->entries[1].term == Term { .value = 4 });

    // Two records of that size and nothing more.
    CHECK(logSize < 3 * (logSize / 2));
}

TEST_CASE("A store opened over an existing log knows where it ends", "[consensus][raft][storage]")
{
    // The offset table is built at Open rather than at Load, so a SaveLog issued
    // before any Load -- or after a Load that failed on the state file and never
    // reached the log -- cannot compute a start of zero and erase everything.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(
            store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 }, .entries = { Entry(1, "a"), Entry(1, "b") } })
                .has_value());
    }

    {
        // No Load at all before writing.
        auto store = OpenStore(scratch.Path());
        REQUIRE(store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 3 }, .entries = { Entry(2, "c") } }).has_value());
    }

    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();

    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 3);
    CHECK(FastCache::AsStringView(loaded->entries[0].payload) == "a");
    CHECK(FastCache::AsStringView(loaded->entries[2].payload) == "c");
}

TEST_CASE("A leader's own writes round-trip through the store", "[consensus][raft][storage]")
{
    // End to end rather than by construction: whatever RaftNode emits as
    // persistLog is fed straight to the store, so an index the node and the store
    // disagree about shows up here rather than in a cluster.
    ScratchDirectory scratch { "fc-raft-store" };
    auto store = OpenStore(scratch.Path());

    auto const config = RaftConfig { .self = "n1",
                                     .voters = { "n1", "n2", "n3" },
                                     .learners = {},
                                     .electionTimeoutMin = 150ms,
                                     .electionTimeoutMax = 300ms,
                                     .heartbeatInterval = 50ms };
    ScriptedRandomSource random { { 0 } };
    auto node = std::move(RaftNode::Create(config, random, TimePoint {})).value();

    auto const feed = [&store](RaftOutput const& output) {
        if (output.persistLog.has_value())
            REQUIRE(store.SaveLog(output.persistLog.value_or(LogAppend {})).has_value());
        if (output.persist.has_value())
            REQUIRE(store.SaveState(output.persist.value_or(PersistentState {})).has_value());
    };

    feed(node.Tick(TimePoint {} + 150ms));
    // The timeout starts a pre-vote round; the real election, and the durable
    // write it produces, follow the grant that carries it.
    feed(node.Receive(PreVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                      TimePoint {} + 150ms));
    feed(
        node.Receive(RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                     TimePoint {} + 150ms));
    REQUIRE(node.CurrentRole() == Role::Leader);

    auto first = node.Propose(FastCache::BytesFromString("one"), TimePoint {} + 200ms);
    REQUIRE(first.has_value());
    feed(first->output);

    auto second = node.Propose(FastCache::BytesFromString("two"), TimePoint {} + 201ms);
    REQUIRE(second.has_value());
    feed(second->output);

    auto const loaded = store.Load();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == node.Log().LastIndex().value);
    CHECK(loaded->entries[0].kind == EntryKind::NoOp);
    CHECK(FastCache::AsStringView(loaded->entries[1].payload) == "one");
    CHECK(FastCache::AsStringView(loaded->entries[2].payload) == "two");
}

TEST_CASE("A log append that would leave a gap is refused", "[consensus][raft][storage]")
{
    // fromIndex states where these entries begin, so writing them anywhere else
    // leaves the store disagreeing with the node about where entries live. No
    // correct driver produces one -- which is exactly why it is an error rather
    // than a coincidence.
    ScratchDirectory scratch { "fc-raft-store" };
    auto file = OpenStore(scratch.Path());
    InMemoryRaftStorage memory;

    for (IRaftStorage* store: { static_cast<IRaftStorage*>(&memory), static_cast<IRaftStorage*>(&file) })
    {
        REQUIRE(
            store->SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 }, .entries = { Entry(1, "a") } }).has_value());

        // Index 2 is the next one; index 3 would skip one.
        auto const gapped = store->SaveLog(LogAppend { .fromIndex = LogIndex { .value = 3 }, .entries = { Entry(1, "c") } });
        REQUIRE_FALSE(gapped.has_value());
        CHECK(gapped.error().code == ConsensusErrorCode::StorageFailure);

        // The contiguous one still works, so the guard rejects only the gap.
        CHECK(store->SaveLog(LogAppend { .fromIndex = LogIndex { .value = 2 }, .entries = { Entry(1, "b") } }).has_value());
    }
}

TEST_CASE("A torn tail is removed by the next append, not written over", "[consensus][raft][storage]")
{
    // Gating the truncate on the offset table rather than the file's real size
    // leaves the torn bytes in place for the next append to overwrite the front
    // of, and whatever suffix survives stays in the file -- discarded on load only
    // because it happens to fail to parse.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(
            store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 }, .entries = { Entry(1, "aaaa") } }).has_value());
    }

    auto const logPath = scratch.Path() / "raft-log";
    auto const goodSize = std::filesystem::file_size(logPath);
    {
        std::ofstream out { logPath, std::ios::binary | std::ios::app };
        REQUIRE(out.is_open());
        std::array<char, 24> const garbage { 'F', 'C', 'R', 'L' };
        out.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }
    REQUIRE(std::filesystem::file_size(logPath) > goodSize);

    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 2 }, .entries = { Entry(1, "b") } }).has_value());
    }

    // Nothing of the torn record is left: the file is exactly the two records.
    auto reopened = OpenStore(scratch.Path());
    auto const loaded = reopened.Load();
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->entries.size() == 2);
    CHECK(FastCache::AsStringView(loaded->entries[1].payload) == "b");

    // Compared against a store given the same two appends and no garbage, rather
    // than against a byte count written out here. A literal states the record
    // layout a second time, so it goes stale the moment a field is added to one --
    // which is exactly what happened when records grew an index.
    ScratchDirectory reference { "fc-raft-store" };
    {
        auto clean = OpenStore(reference.Path());
        REQUIRE(
            clean.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 }, .entries = { Entry(1, "aaaa") } }).has_value());
        REQUIRE(clean.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 2 }, .entries = { Entry(1, "b") } }).has_value());
    }

    CHECK(std::filesystem::file_size(logPath) == std::filesystem::file_size(reference.Path() / "raft-log"));
}

TEST_CASE("A saved snapshot survives a reopen and trims the log", "[consensus][raft][storage]")
{
    // Both stores, one case: the in-memory one is what a cluster simulation gives
    // its nodes, so a rule only the file store obeyed would be untested exactly
    // where restarts are actually exercised.
    ScratchDirectory scratch { "fc-raft-store" };
    auto const snapshot = RaftSnapshot { .lastIncludedIndex = LogIndex { .value = 2 },
                                         .lastIncludedTerm = Term { .value = 1 },
                                         .configuration = { .voters = { "n1", "n2", "n3" }, .learners = {} },
                                         .state = FastCache::BytesFromString("state-bytes") };

    auto seed = [&](IRaftStorage& store) {
        REQUIRE(store
                    .SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 },
                                         .entries = { Entry(1, "a"), Entry(1, "b"), Entry(1, "c") } })
                    .has_value());
        REQUIRE(store.SaveSnapshot(snapshot).has_value());
    };

    InMemoryRaftStorage memory;
    seed(memory);
    {
        auto file = OpenStore(scratch.Path());
        seed(file);
    }

    auto file = OpenStore(scratch.Path());
    for (IRaftStorage* store: { static_cast<IRaftStorage*>(&memory), static_cast<IRaftStorage*>(&file) })
    {
        auto const loaded = store->Load();
        REQUIRE(loaded.has_value());

        REQUIRE(loaded->snapshot.has_value());
        CHECK(Unwrap(loaded->snapshot) == snapshot);

        // The covered prefix is gone and what remains still knows its own index --
        // a positional file could not say where a trimmed log now begins, so entry
        // three would come back as entry one.
        CHECK(loaded->firstIndex == LogIndex { .value = 3 });
        REQUIRE(loaded->entries.size() == 1);
        CHECK(FastCache::AsStringView(loaded->entries[0].payload) == "c");
    }
}

TEST_CASE("A log trimmed to nothing still knows where it resumes", "[consensus][raft][storage]")
{
    // The degenerate case, and the one a per-record index alone does not answer:
    // with every record gone there is none left to state the boundary, so the
    // snapshot has to. Without it the next append is refused as a gap, forever.
    ScratchDirectory scratch { "fc-raft-store" };
    auto const snapshot = RaftSnapshot { .lastIncludedIndex = LogIndex { .value = 3 },
                                         .lastIncludedTerm = Term { .value = 2 },
                                         .configuration = { .voters = { "n1" }, .learners = {} },
                                         .state = FastCache::BytesFromString("all-of-it") };

    InMemoryRaftStorage memory;
    ScratchDirectory fileScratch { "fc-raft-store" };
    auto file = OpenStore(fileScratch.Path());

    for (IRaftStorage* store: { static_cast<IRaftStorage*>(&memory), static_cast<IRaftStorage*>(&file) })
    {
        REQUIRE(store
                    ->SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 },
                                          .entries = { Entry(1, "a"), Entry(2, "b"), Entry(2, "c") } })
                    .has_value());
        REQUIRE(store->SaveSnapshot(snapshot).has_value());

        auto const loaded = store->Load();
        REQUIRE(loaded.has_value());
        CHECK(loaded->entries.empty());
        CHECK(loaded->firstIndex == LogIndex { .value = 4 });

        // And the log carries on from there rather than refusing.
        REQUIRE(
            store->SaveLog(LogAppend { .fromIndex = LogIndex { .value = 4 }, .entries = { Entry(3, "d") } }).has_value());

        auto const after = store->Load();
        REQUIRE(after.has_value());
        REQUIRE(after->entries.size() == 1);
        CHECK(FastCache::AsStringView(after->entries[0].payload) == "d");
        CHECK(after->firstIndex == LogIndex { .value = 4 });
    }
}

TEST_CASE("An append below the snapshot boundary is refused", "[consensus][raft][storage]")
{
    // It names entries the snapshot has deliberately replaced. Silently accepting
    // it would leave the store's indices disagreeing with the node's, which is the
    // fault the gap check exists for -- and a trimmed log is the case where the
    // old one-ended check could not see it.
    ScratchDirectory scratch { "fc-raft-store" };
    auto store = OpenStore(scratch.Path());
    InMemoryRaftStorage memory;

    for (IRaftStorage* target: { static_cast<IRaftStorage*>(&store), static_cast<IRaftStorage*>(&memory) })
    {
        REQUIRE(target
                    ->SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 },
                                          .entries = { Entry(1, "a"), Entry(1, "b"), Entry(1, "c") } })
                    .has_value());
        REQUIRE(target
                    ->SaveSnapshot(RaftSnapshot { .lastIncludedIndex = LogIndex { .value = 2 },
                                                  .lastIncludedTerm = Term { .value = 1 },
                                                  .configuration = { .voters = { "n1" }, .learners = {} },
                                                  .state = {} })
                    .has_value());

        CHECK_FALSE(
            target->SaveLog(LogAppend { .fromIndex = LogIndex { .value = 2 }, .entries = { Entry(2, "x") } }).has_value());

        // The boundary itself is still writable, which is what a leader replacing
        // the first retained entry does.
        CHECK(target->SaveLog(LogAppend { .fromIndex = LogIndex { .value = 3 }, .entries = { Entry(2, "C") } }).has_value());
    }
}

TEST_CASE("A corrupt snapshot is refused rather than read as an empty one", "[consensus][raft][storage]")
{
    // An empty snapshot is a legitimate value -- an application with no state yet
    // produces one -- so a reader that fell back to it on damage would hand a node
    // "you are caught up through index N with nothing in you", and the node would
    // ship that to a follower as though it were state.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(store
                    .SaveSnapshot(RaftSnapshot { .lastIncludedIndex = LogIndex { .value = 4 },
                                                 .lastIncludedTerm = Term { .value = 2 },
                                                 .configuration = { .voters = { "n1" }, .learners = {} },
                                                 .state = FastCache::BytesFromString("real") })
                    .has_value());
    }

    auto const path = scratch.Path() / "raft-snapshot";
    {
        std::fstream out { path, std::ios::binary | std::ios::in | std::ios::out };
        REQUIRE(out.is_open());
        out.seekp(static_cast<std::streamoff>(std::filesystem::file_size(path)) - 1);
        char const flipped = '\x7f';
        out.write(&flipped, 1);
    }

    // Refused at `Open`, not merely at `Load`. A store that opens and then fails on
    // first read is a store whose caller has to remember an ordering, which is
    // what `Open` reading the log and the snapshot together exists to remove.
    CHECK_FALSE(FileRaftStorage::Open(scratch.Path()).has_value());
}

TEST_CASE("A reopened store knows its boundary before anything reads it", "[consensus][raft][storage]")
{
    // `Open` scans the log so the offset table is valid from the moment the store
    // exists -- and a log a snapshot has trimmed to nothing has no record left to
    // state where it resumes, so the scan alone leaves the boundary at 1. A
    // `SaveLog` before `Load` would then be refused as a gap. The same repair has
    // to happen in both places, because either can be the first to touch the store
    // after a restart.
    ScratchDirectory scratch { "fc-raft-store" };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(
            store.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 }, .entries = { Entry(1, "a"), Entry(1, "b") } })
                .has_value());
        REQUIRE(store
                    .SaveSnapshot(RaftSnapshot { .lastIncludedIndex = LogIndex { .value = 2 },
                                                 .lastIncludedTerm = Term { .value = 1 },
                                                 .configuration = { .voters = { "n1" }, .learners = {} },
                                                 .state = FastCache::BytesFromString("s") })
                    .has_value());
    }

    auto reopened = OpenStore(scratch.Path());

    // Deliberately no Load() in between: this is the ordering requirement the
    // comment in `Open` exists to remove.
    REQUIRE(reopened.SaveLog(LogAppend { .fromIndex = LogIndex { .value = 3 }, .entries = { Entry(2, "c") } }).has_value());

    auto const loaded = reopened.Load();
    REQUIRE(loaded.has_value());
    CHECK(loaded->firstIndex == LogIndex { .value = 3 });
    REQUIRE(loaded->entries.size() == 1);
    CHECK(FastCache::AsStringView(loaded->entries[0].payload) == "c");
}

// --------------------------------------------------------------------------
// The format bump (#1449): a store the previous build wrote is refused BY NAME.

namespace
{

/// The previous format, written out by hand: format 1, as it was before learners.
///
/// A PINNED FIXTURE, deliberately not produced by `FileRaftStorage`: the encoder under
/// test now writes format 2, so a fixture derived from it would be a format-2 store
/// with a different number in it, and "the old store is refused" would pass against a
/// reader that had never seen the old layout. Every byte below follows format 1's
/// layout as it shipped -- a u16 version of 1 in the state and snapshot headers, a log
/// record with NO version at all, and a configuration as ONE flat id list -- and only
/// the CRC32C primitive is borrowed, since it is a checksum and not a layout.
namespace FormatOne
{
    constexpr std::uint16_t Version = 1;

    /// Append a big-endian integer.
    /// @param out Where.
    /// @param value What.
    template <typename T>
    void Put(std::vector<std::byte>& out, T value)
    {
        auto scratch = std::array<std::byte, sizeof(T)> {};
        WriteBigEndian<T>(scratch, value);
        out.insert(out.end(), scratch.begin(), scratch.end());
    }

    /// Append one `[u32 length][bytes]` field.
    /// @param out Where.
    /// @param bytes The field.
    void PutField(std::vector<std::byte>& out, std::span<std::byte const> bytes)
    {
        Put<std::uint32_t>(out, static_cast<std::uint32_t>(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    /// Seal a record with its trailing CRC32C.
    /// @param body Everything before the CRC.
    /// @return The whole record.
    [[nodiscard]] std::vector<std::byte> Sealed(std::vector<std::byte> body)
    {
        Put<std::uint32_t>(body, Crc32c::Compute(body));
        return body;
    }

    /// Format 1's configuration: one flat list of ids.
    /// @param members The members.
    /// @return The payload.
    [[nodiscard]] std::vector<std::byte> FlatMembers(std::vector<std::string> const& members)
    {
        auto out = std::vector<std::byte> {};
        for (auto const& member: members)
            PutField(out, FastCache::AsBytes(member));
        return out;
    }

    /// A format-1 `raft-state`: term 3, voted for "n2".
    [[nodiscard]] std::vector<std::byte> State()
    {
        auto body = std::vector<std::byte> {};
        Put<std::uint32_t>(body, 0x46435253U); // "FCRS"
        Put<std::uint16_t>(body, Version);
        Put<std::uint64_t>(body, 3);
        Put<std::uint32_t>(body, 2);
        body.push_back(std::byte { 1 });
        body.push_back(std::byte { 'n' });
        body.push_back(std::byte { '2' });
        return Sealed(std::move(body));
    }

    /// A format-1 `raft-snapshot` at index 4, term 2, of three members.
    [[nodiscard]] std::vector<std::byte> Snapshot()
    {
        auto body = std::vector<std::byte> {};
        Put<std::uint32_t>(body, 0x4643524EU); // "FCRN"
        Put<std::uint16_t>(body, Version);
        Put<std::uint64_t>(body, 4);
        Put<std::uint64_t>(body, 2);
        PutField(body, FlatMembers({ "n1", "n2", "n3" }));
        PutField(body, FastCache::BytesFromString("state"));
        return Sealed(std::move(body));
    }

    /// A format-1 `raft-log` of one Configuration entry at index 1: no version field
    /// anywhere, the index straight after the magic.
    [[nodiscard]] std::vector<std::byte> Log()
    {
        auto const payload = FlatMembers({ "n1", "n2", "n3" });
        auto body = std::vector<std::byte> {};
        Put<std::uint32_t>(body, 0x4643524CU); // "FCRL"
        Put<std::uint64_t>(body, 1);
        Put<std::uint64_t>(body, 1);
        body.push_back(static_cast<std::byte>(EntryKind::Configuration));
        Put<std::uint32_t>(body, static_cast<std::uint32_t>(payload.size()));
        body.insert(body.end(), payload.begin(), payload.end());
        return Sealed(std::move(body));
    }

    /// Write `bytes` to `path`, replacing it.
    /// @param path Where.
    /// @param bytes What.
    void Write(std::filesystem::path const& path, std::vector<std::byte> const& bytes)
    {
        std::ofstream out { path, std::ios::binary | std::ios::trunc };
        REQUIRE(out.is_open());
        auto const text = FastCache::AsStringView(bytes);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(out.good());
    }
} // namespace FormatOne

/// Open `directory` and read it back, and say how that ended.
/// @param directory The store.
/// @return Nothing when it opened and loaded; otherwise the refusal.
[[nodiscard]] std::optional<ConsensusError> OpenAndLoad(std::filesystem::path const& directory)
{
    auto store = FileRaftStorage::Open(directory);
    if (!store.has_value())
        return store.error();
    auto loaded = store->Load();
    if (!loaded.has_value())
        return loaded.error();
    return std::nullopt;
}

} // namespace

TEST_CASE("A store the previous format wrote is refused as UnsupportedFormatVersion, never as damage",
          "[consensus][raft][storage][learner]")
{
    // The storage rule, arriving in the Raft store: the code is what an operator acts
    // on, and "damaged" is what gets a healthy store deleted. Before #1449 an old
    // state or snapshot file was refused as `StorageFailure` -- the damage code -- and
    // an old log could not be refused at all, since its records carried no version.
    //
    // One file per section, alone in a fresh directory -- the other two absent, which
    // a store reads as empty rather than refusing -- so a refusal here is the old
    // file's and nothing else's.
    ScratchDirectory scratch { "fc-raft-store" };

    auto const expectForeign = [&scratch](std::string_view file, std::vector<std::byte> const& bytes) {
        FormatOne::Write(scratch.Path() / file, bytes);
        auto const refused = OpenAndLoad(scratch.Path());
        REQUIRE(refused.has_value());
        auto const& error = Unwrap(refused);
        CAPTURE(error.context);
        CHECK(error.code == ConsensusErrorCode::UnsupportedFormatVersion);
        CHECK(error.code != ConsensusErrorCode::StorageFailure);

        // It names the version it found and the one this build reads, and says what
        // to do -- which is not what the damage message says.
        CHECK(error.context.contains("format 1"));
        CHECK(error.context.contains("format 2"));
        CHECK(error.context.contains("no conversion"));
    };

    SECTION("the state file")
    {
        expectForeign("raft-state", FormatOne::State());
    }

    SECTION("the snapshot file")
    {
        expectForeign("raft-snapshot", FormatOne::Snapshot());
    }

    SECTION("the log, whose records carried no version at all")
    {
        expectForeign("raft-log", FormatOne::Log());
    }
}

TEST_CASE("A store this build wrote opens, and damage to it is still damage", "[consensus][raft][storage][learner]")
{
    // The two controls the case above needs. A reader that refused EVERY store would
    // pass it; so would one that called damage a foreign format. So: a current store,
    // carrying learners in both the snapshot and a log entry, opens and reads back
    // whole -- and the same kinds of file with their bytes damaged are `StorageFailure`.
    ScratchDirectory scratch { "fc-raft-store" };
    auto const configuration = Configuration { .voters = { "n1", "n2" }, .learners = { "laptop" } };
    auto const snapshot = RaftSnapshot { .lastIncludedIndex = LogIndex { .value = 2 },
                                         .lastIncludedTerm = Term { .value = 1 },
                                         .configuration = configuration,
                                         .state = FastCache::BytesFromString("state") };
    {
        auto store = OpenStore(scratch.Path());
        REQUIRE(store.SaveState(PersistentState { .currentTerm = Term { .value = 3 }, .votedFor = "n2" }).has_value());
        REQUIRE(store
                    .SaveLog(LogAppend { .fromIndex = LogIndex { .value = 1 },
                                         .entries = { Entry(1, "a"),
                                                      Entry(1, "b"),
                                                      LogEntry { .term = Term { .value = 1 },
                                                                 .kind = EntryKind::Configuration,
                                                                 .payload = Membership::Encode(configuration) } } })
                    .has_value());
        REQUIRE(store.SaveSnapshot(snapshot).has_value());
    }

    {
        auto store = OpenStore(scratch.Path());
        auto const loaded = store.Load();
        REQUIRE(loaded.has_value());
        REQUIRE(loaded->snapshot.has_value());
        CHECK(Unwrap(loaded->snapshot).configuration == configuration);
        REQUIRE(loaded->entries.size() == 1);
        CHECK(Membership::Decode(loaded->entries[0].payload) == std::optional { configuration });
    }

    // The same log, damaged in its only record's payload: torn, not foreign, so it is
    // discarded as a tail and the store still opens and loads -- which is the
    // torn-tail rule, and the opposite of what a foreign FIRST record gets.
    auto const logPath = scratch.Path() / "raft-log";
    {
        std::fstream inout { logPath, std::ios::binary | std::ios::in | std::ios::out };
        REQUIRE(inout.is_open());
        inout.seekp(-6, std::ios::end);
        char const flipped = 0x7F;
        inout.write(&flipped, 1);
    }
    CHECK_FALSE(OpenAndLoad(scratch.Path()).has_value());

    // The snapshot damaged in its CRC is refused as DAMAGE.
    auto const snapshotPath = scratch.Path() / "raft-snapshot";
    {
        std::fstream inout { snapshotPath, std::ios::binary | std::ios::in | std::ios::out };
        REQUIRE(inout.is_open());
        inout.seekp(-1, std::ios::end);
        char const flipped = 0x7F;
        inout.write(&flipped, 1);
    }
    auto const damaged = OpenAndLoad(scratch.Path());
    REQUIRE(damaged.has_value());
    CHECK(Unwrap(damaged).code == ConsensusErrorCode::StorageFailure);
}
