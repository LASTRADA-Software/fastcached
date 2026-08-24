// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/InMemoryRaftStorage.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
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
                                     .members = { "n1", "n2", "n3" },
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
                                                        .members = { "n1", "n2", "n3" },
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
        for (auto index = std::uint64_t { 1 }; index <= 5; ++index)
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
                                     .members = { "n1", "n2", "n3" },
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
                                         .members = { "n1", "n2", "n3" },
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
                                         .members = { "n1" },
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
                                                  .members = { "n1" },
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
                                                 .members = { "n1" },
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
                                                 .members = { "n1" },
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
