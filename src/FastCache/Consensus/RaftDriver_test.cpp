// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Consensus/InMemoryRaftStorage.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{

/// Records the order in which the driver did things.
///
/// The ordering contract is the whole reason this class exists, and it cannot be
/// asserted from the outputs alone -- persist-then-send and send-then-persist
/// produce identical state. Only the sequence of side effects tells them apart.
struct Journal
{
    std::vector<std::string> events;
};

class RecordingStorage final: public IRaftStorage
{
  public:
    explicit RecordingStorage(Journal& journal) noexcept:
        _journal { journal }
    {
    }

    std::expected<void, ConsensusError> SaveState(PersistentState const& state) override
    {
        _journal.events.emplace_back("persist-state");
        return _inner.SaveState(state);
    }

    std::expected<void, ConsensusError> SaveLog(LogAppend const& append) override
    {
        _journal.events.emplace_back("persist-log");
        return _inner.SaveLog(append);
    }

    std::expected<void, ConsensusError> SaveSnapshot(RaftSnapshot const& snapshot) override
    {
        _journal.events.emplace_back("persist-snapshot");
        return _inner.SaveSnapshot(snapshot);
    }

    std::expected<RecoveredState, ConsensusError> Load() override
    {
        return _inner.Load();
    }

  private:
    Journal& _journal;
    std::vector<std::byte> _state;
    InMemoryRaftStorage _inner;
};

class RecordingTransport final: public IRaftTransport
{
  public:
    explicit RecordingTransport(Journal& journal) noexcept:
        _journal { journal }
    {
    }

    void Send(NodeId const& to, RaftMessage message) override
    {
        _journal.events.emplace_back("send");
        _sent.emplace_back(to, std::move(message));
    }

    /// @return Everything sent so far, in order.
    [[nodiscard]] std::vector<std::pair<NodeId, RaftMessage>> const& Sent() const noexcept
    {
        return _sent;
    }

  private:
    Journal& _journal;
    std::vector<std::pair<NodeId, RaftMessage>> _sent;
};

class RecordingMachine final: public IRaftStateMachine
{
  public:
    explicit RecordingMachine(Journal& journal) noexcept:
        _journal { journal }
    {
    }

    void Apply(AppliedEntry const& entry) override
    {
        _journal.events.emplace_back("apply");
        _applied.push_back(entry);
    }

    [[nodiscard]] std::vector<std::byte> TakeSnapshot() override
    {
        _journal.events.emplace_back("snapshot");
        return _state;
    }

    void RestoreSnapshot(std::span<std::byte const> state) override
    {
        _journal.events.emplace_back("restore");
        _state.assign(state.begin(), state.end());

        // A restore REPLACES: anything applied before it is described by the
        // snapshot, so keeping it would double-count.
        _applied.clear();
    }

    /// @return The state a restore installed, for assertions.
    [[nodiscard]] std::vector<std::byte> const& State() const noexcept
    {
        return _state;
    }

    /// @return Everything applied so far, in order.
    [[nodiscard]] std::vector<AppliedEntry> const& Applied() const noexcept
    {
        return _applied;
    }

  private:
    Journal& _journal;
    std::vector<AppliedEntry> _applied;
    std::vector<std::byte> _state;
};

/// Storage whose every write fails, for the stop-on-failure case.
class FailingStorage final: public IRaftStorage
{
  public:
    std::expected<void, ConsensusError> SaveState(PersistentState const& /*state*/) override
    {
        return std::unexpected { FastCache::StorageFailure("disk is gone") };
    }

    std::expected<void, ConsensusError> SaveLog(LogAppend const& /*append*/) override
    {
        return std::unexpected { FastCache::StorageFailure("disk is gone") };
    }

    std::expected<void, ConsensusError> SaveSnapshot(RaftSnapshot const& /*snapshot*/) override
    {
        return std::unexpected { FastCache::StorageFailure("disk is gone") };
    }

    std::expected<RecoveredState, ConsensusError> Load() override
    {
        return RecoveredState {};
    }
};

[[nodiscard]] RaftConfig SoloConfig()
{
    return RaftConfig { .self = "solo",
                        .members = { "solo" },
                        .electionTimeoutMin = 150ms,
                        .electionTimeoutMax = 300ms,
                        .heartbeatInterval = 50ms };
}

[[nodiscard]] RaftConfig TrioConfig()
{
    return RaftConfig { .self = "n1",
                        .members = { "n1", "n2", "n3" },
                        .electionTimeoutMin = 150ms,
                        .electionTimeoutMax = 300ms,
                        .heartbeatInterval = 50ms };
}

/// Carry a driver's pre-vote round so its node becomes a candidate.
///
/// An election timeout starts a pre-vote round rather than an election, so a
/// case that wants a candidate -- or the durable write a candidacy produces --
/// needs the grant that turns one into the other.
/// @param driver The driver to advance.
/// @param at When the timeout falls due.
/// @return Whether the grant was accepted.
[[nodiscard]] bool CarryPreVote(RaftDriver& driver, TimePoint at)
{
    return driver
        .Receive(PreVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" }, at)
        .has_value();
}

} // namespace

TEST_CASE("Durable state is written before anything is sent", "[consensus][raft][driver]")
{
    // Not observable from the resulting state -- both orders leave the same node
    // and the same store -- so the sequence of side effects is the only evidence.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine
    };

    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());

    // The pre-vote round sends while persisting nothing, and that is correct
    // rather than an exception to the rule: it changes no durable state, so
    // there is nothing that must reach the disk before it. The ordering this
    // case exists for is the ELECTION's, so the journal is read from there.
    journal.events.clear();
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));

    REQUIRE_FALSE(journal.events.empty());
    auto const firstSend = std::ranges::find(journal.events, "send");
    REQUIRE(firstSend != journal.events.end());

    // Every persist precedes the first send.
    for (auto event = journal.events.begin(); event != firstSend; ++event)
        CHECK(*event != "send");

    CHECK(std::ranges::find(journal.events.begin(), firstSend, "persist-state") != firstSend);
}

TEST_CASE("Term and vote are written before the log", "[consensus][raft][driver]")
{
    // The crash window between them should leave the safe half-state: a node at a
    // newer term missing an entry is indistinguishable from one that never
    // appended it, while an entry from a term the node does not remember entering
    // is a state nothing else reasons about.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine
    };

    // Becoming leader writes both: the term/vote from standing for election, and
    // the no-op entry.
    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));
    journal.events.clear();
    REQUIRE(
        driver
            .Receive(RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                     TimePoint {} + 150ms)
            .has_value());

    auto const state = std::ranges::find(journal.events, "persist-state");
    auto const log = std::ranges::find(journal.events, "persist-log");
    REQUIRE(state != journal.events.end());
    REQUIRE(log != journal.events.end());
    CHECK(state < log);
}

TEST_CASE("Committed entries are applied after the messages go out", "[consensus][raft][driver]")
{
    // Applying is local; peers cannot make progress until the messages are out,
    // so applying first would add the application's latency to replication.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine
    };

    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));
    REQUIRE(
        driver
            .Receive(RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                     TimePoint {} + 150ms)
            .has_value());

    journal.events.clear();
    REQUIRE(driver
                .Receive(AppendEntriesResponse { .term = Term { .value = 1 },
                                                 .result = AppendResult::Accepted,
                                                 .matchIndex = LogIndex { .value = 1 },
                                                 .followerId = "n2" },
                         TimePoint {} + 200ms)
                .has_value());

    // A no-op commits here and is never delivered, so nothing is applied -- which
    // is itself the assertion that the driver does not invent deliveries.
    CHECK(std::ranges::find(journal.events, "apply") == journal.events.end());
}

TEST_CASE("A proposal reaches storage, the wire and the application", "[consensus][raft][driver]")
{
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    RaftDriver driver {
        std::move(RaftNode::Create(SoloConfig(), random, TimePoint {})).value(), storage, transport, machine
    };

    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));
    REQUIRE(driver.Node().CurrentRole() == Role::Leader);

    auto const index = driver.Propose(FastCache::BytesFromString("only"), TimePoint {} + 200ms);
    REQUIRE(index.has_value());

    // A single-node cluster is its own quorum, so it commits at once.
    REQUIRE(machine.Applied().size() == 1);
    CHECK(FastCache::AsStringView(machine.Applied().front().payload) == "only");
    CHECK(std::ranges::find(journal.events, "persist-log") != journal.events.end());
}

TEST_CASE("A storage failure stops the driver and latches", "[consensus][raft][driver]")
{
    // Everything else in this system falls back and carries on; this cannot,
    // because continuing past a failed durability write means acting on state that
    // is not durable. A retry loop over a disk that is gone is a node that looks
    // alive and does nothing, so the failure is latched instead.
    Journal journal;
    FailingStorage storage;
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine
    };

    // The pre-vote round writes nothing, so it cannot fail; the election it
    // leads to is the first thing that touches the store.
    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());

    // The pre-vote round has already gone out, and legitimately: it writes
    // nothing, so nothing had to be durable before it. What this case is about
    // is the ELECTION's sends, so the count is taken from here.
    auto const sentBeforeElection = transport.Sent().size();

    auto const first =
        driver.Receive(PreVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                       TimePoint {} + 150ms);
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().code == ConsensusErrorCode::StorageFailure);

    // Nothing left the node: the failure happened before any send.
    CHECK(transport.Sent().size() == sentBeforeElection);

    REQUIRE(driver.Failure().has_value());

    // And it stays failed rather than quietly resuming.
    CHECK_FALSE(driver.Tick(TimePoint {} + 400ms).has_value());
    CHECK_FALSE(driver
                    .Receive(AppendEntriesRequest { .term = Term { .value = 9 },
                                                    .leaderId = "n2",
                                                    .prevLogIndex = LogIndex::BeforeFirst(),
                                                    .prevLogTerm = Term::None(),
                                                    .entries = {},
                                                    .leaderCommit = LogIndex::BeforeFirst() },
                             TimePoint {} + 401ms)
                    .has_value());
}

TEST_CASE("Run ticks the node on the reactor's timer", "[consensus][raft][driver]")
{
    // Driven by TestReactor and a ManualClock, so the loop is exercised with no
    // sockets and no wall-clock waiting anywhere.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    ManualClock clock;
    TestReactor reactor { clock };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, clock.Now())).value(), storage, transport, machine
    };

    // Started the way SleepUntil_test starts a root task: hand the handle to the
    // reactor rather than awaiting it, since there is no coroutine here to await
    // from.
    auto loop = driver.Run(&reactor);
    reactor.Submit(loop.Native());
    (void) reactor.Drain();

    // Parked on the election deadline, having done nothing yet.
    CHECK(reactor.PendingTimers() == 1);
    CHECK(driver.Node().CurrentRole() == Role::Follower);

    clock.Advance(150ms);
    (void) reactor.Drain();

    // A pre-candidate, not a candidate: the loop drove the timeout, and nothing
    // has answered the pre-vote it asked.
    CHECK(driver.Node().CurrentRole() == Role::PreCandidate);
    CHECK_FALSE(transport.Sent().empty());

    driver.Stop();
    clock.Advance(400ms);
    (void) reactor.Drain();
}

TEST_CASE("A node elected between ticks still heartbeats on time", "[consensus][raft][driver]")
{
    // Invisible to every other case in this file and to `RaftClusterHarness`,
    // because both advance a node by calling `Tick` directly. `Run` parks on a
    // deadline read BEFORE it suspends, and `SleepUntil` cannot be cancelled --
    // while `Receive`, which in production arrives from a peer-reader coroutine on
    // the same reactor, can move that deadline EARLIER: a candidate that wins goes
    // from an election
    // deadline up to `electionTimeoutMax` away to a heartbeat deadline one
    // interval away. Sleeping to the stale value delays the new leader's second
    // heartbeat past the shortest election timeout a follower can draw, that
    // follower elects itself, and the cluster does it again one term later.
    // Measured on three real nodes before the bound went in: nine role changes in
    // twelve seconds with nothing else wrong.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    ManualClock clock;
    TestReactor reactor { clock };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, clock.Now())).value(), storage, transport, machine
    };

    auto loop = driver.Run(&reactor);
    reactor.Submit(loop.Native());
    (void) reactor.Drain();

    clock.Advance(150ms);
    (void) reactor.Drain();
    REQUIRE(CarryPreVote(driver, clock.Now()));
    REQUIRE(driver.Node().CurrentRole() == Role::Candidate);

    // The vote that carries the election, delivered while the loop is parked --
    // which is exactly how it arrives in production.
    REQUIRE(
        driver
            .Receive(RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                     clock.Now())
            .has_value());
    REQUIRE(driver.Node().CurrentRole() == Role::Leader);

    // Everything the election itself produced, including the first heartbeat a
    // new leader sends immediately. What is asserted is the SECOND one.
    auto const afterElection = transport.Sent().size();

    clock.Advance(50ms);
    (void) reactor.Drain();
    CHECK(transport.Sent().size() > afterElection);

    driver.Stop();
    clock.Advance(400ms);
    (void) reactor.Drain();
}

TEST_CASE("Stop ends the run loop", "[consensus][raft][driver]")
{
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    ManualClock clock;
    TestReactor reactor { clock };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, clock.Now())).value(), storage, transport, machine
    };

    auto loop = driver.Run(&reactor);
    reactor.Submit(loop.Native());
    (void) reactor.Drain();
    driver.Stop();

    clock.Advance(400ms);
    (void) reactor.Drain();

    // Nothing is left parked, so the loop actually finished rather than
    // rescheduling itself forever.
    CHECK(reactor.PendingTimers() == 0);
}

TEST_CASE("An applied log is traded for a snapshot once enough has piled up", "[consensus][raft][driver]")
{
    // The residual this closes: `CompactThroughApplied` existed and nothing called
    // it, so a long-lived cluster's log grew without bound and every restart
    // replayed the whole of it. Slowly -- cluster configuration changes are rare by
    // construction -- but "slowly" is not "never".
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    constexpr auto Threshold = std::uint64_t { 4 };

    RaftDriver driver { std::move(RaftNode::Create(SoloConfig(), random, TimePoint {})).value(),
                        storage,
                        transport,
                        machine,
                        CompactionPolicy { .appliedEntriesBeforeCompaction = Threshold } };

    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));
    REQUIRE(driver.Node().CurrentRole() == Role::Leader);

    // Driven to one short of the threshold rather than by a hand-counted number of
    // proposals: a new leader appends a no-op of its own term, which is applied
    // like any other entry and is exactly the sort of implementation detail a
    // counted assertion would silently encode. A single-node cluster is its own
    // quorum, so each proposal commits and applies at once.
    auto step = 0;
    auto const unsnapshotted = [&driver] {
        return driver.Node().LastApplied().value - driver.Node().Log().SnapshotIndex().value;
    };

    while (unsnapshotted() + 1 < Threshold)
    {
        REQUIRE(driver.Propose(FastCache::BytesFromString(std::format("e{}", step)), TimePoint {} + 200ms).has_value());
        ++step;
    }

    CHECK(std::ranges::find(journal.events, "persist-snapshot") == journal.events.end());
    CHECK(driver.Node().Log().SnapshotIndex() == LogIndex::BeforeFirst());

    // One more crosses it.
    REQUIRE(driver.Propose(FastCache::BytesFromString("crossing"), TimePoint {} + 210ms).has_value());

    // The snapshot is asked of the application and made durable, and the log below
    // it is gone -- which is the point: what a restart replays is now bounded.
    CHECK(std::ranges::find(journal.events, "snapshot") != journal.events.end());
    CHECK(std::ranges::find(journal.events, "persist-snapshot") != journal.events.end());
    CHECK(driver.Node().Log().SnapshotIndex() == driver.Node().LastApplied());
    CHECK(driver.Node().Log().FirstIndex() == driver.Node().LastApplied().Advanced(1));

    // And the node carries on from there rather than refusing its own next append
    // as a gap -- the failure a trimmed log invites, and the reason the boundary is
    // recovered rather than assumed.
    auto const after = driver.Propose(FastCache::BytesFromString("after"), TimePoint {} + 220ms);
    REQUIRE(after.has_value());
    CHECK(driver.Node().Log().LastIndex() == *after);
}

TEST_CASE("A driver told nothing about compaction never discards anything", "[consensus][raft][driver]")
{
    // The default, and it is the safe one deliberately: a log that grows is
    // wasteful, while a snapshot taken from a machine whose `TakeSnapshot` means
    // nothing yet is wrong.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    RaftDriver driver {
        std::move(RaftNode::Create(SoloConfig(), random, TimePoint {})).value(), storage, transport, machine
    };

    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));

    for (auto const step: std::views::iota(0, 8))
        REQUIRE(driver.Propose(FastCache::BytesFromString(std::format("e{}", step)), TimePoint {} + 200ms).has_value());

    CHECK(std::ranges::find(journal.events, "snapshot") == journal.events.end());
    CHECK(driver.Node().Log().SnapshotIndex() == LogIndex::BeforeFirst());
}

TEST_CASE("A snapshot that cannot be written stops the driver", "[consensus][raft][driver]")
{
    // Compaction discards the entries from memory the moment it succeeds, so a
    // node that carried on after failing to record what they produced would come
    // back from a restart missing committed state. It is the same latch every other
    // durability failure gets, and it is reported rather than swallowed because a
    // caller that treated maintenance as best-effort would never learn.
    Journal journal;
    InMemoryRaftStorage storage { InMemoryRaftStorage::FailurePlan { .failNthSaveSnapshot = 1 } };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    // Two, not one: the no-op a new leader appends is applied during the election
    // itself, so a threshold of one would fall due before there is a proposal to
    // attribute the refusal to.
    RaftDriver driver { std::move(RaftNode::Create(SoloConfig(), random, TimePoint {})).value(),
                        storage,
                        transport,
                        machine,
                        CompactionPolicy { .appliedEntriesBeforeCompaction = 2 } };

    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));

    auto const proposed = driver.Propose(FastCache::BytesFromString("one"), TimePoint {} + 200ms);
    REQUIRE(!proposed.has_value());
    REQUIRE(driver.Failure().has_value());

    // Latched: the next call refuses with the same error rather than pretending
    // this node is still taking part.
    CHECK(!driver.Tick(TimePoint {} + 400ms).has_value());
}

TEST_CASE("A role change is reported with the term it happened in", "[consensus][raft][driver]")
{
    // Nothing covered `ObserveRole` at all, which is how the daemon's role line
    // came to carry no term: the observer is the only route the fact travels, and
    // an untested route is one that can quietly stop carrying anything.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine
    };

    auto reported = std::vector<RaftDriver::RoleChange> {};
    driver.ObserveRole([&reported](RaftDriver::RoleChange const& change) { reported.push_back(change); });

    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));
    REQUIRE(
        driver
            .Receive(RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                     TimePoint {} + 150ms)
            .has_value());

    REQUIRE_FALSE(reported.empty());
    CHECK(reported.back().role == Role::Leader);
    CHECK(reported.back().term == Term { .value = 1 });
    CHECK(reported.back().knownLeader == std::optional<NodeId> { "n1" });
    CHECK_FALSE(reported.back().cause.has_value());
}

TEST_CASE("A deposition is reported with the peer that caused it", "[consensus][raft][driver]")
{
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine
    };

    auto reported = std::vector<RaftDriver::RoleChange> {};
    driver.ObserveRole([&reported](RaftDriver::RoleChange const& change) { reported.push_back(change); });

    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));
    REQUIRE(
        driver
            .Receive(RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                     TimePoint {} + 150ms)
            .has_value());
    reported.clear();

    REQUIRE(driver
                .Receive(AppendEntriesRequest { .term = Term { .value = 2 },
                                                .leaderId = "n3",
                                                .prevLogIndex = LogIndex::BeforeFirst(),
                                                .prevLogTerm = Term::None(),
                                                .entries = {},
                                                .leaderCommit = LogIndex::BeforeFirst() },
                         TimePoint {} + 200ms)
                .has_value());

    REQUIRE(reported.size() == 1);
    CHECK(reported[0].role == Role::Follower);
    CHECK(reported[0].term == Term { .value = 2 });
    REQUIRE(reported[0].cause.has_value());
    auto const& cause = Unwrap(reported[0].cause);
    CHECK(cause.from == "n3");
    CHECK(cause.previousRole == Role::Leader);
    CHECK(cause.previousTerm == Term { .value = 1 });
}

TEST_CASE("A term that moves without the role moving is still reported", "[consensus][raft][driver]")
{
    // The case a report keyed on (role, leader) alone cannot see, and the one
    // somebody reads a dump to find. A node being disturbed by a peer that keeps
    // campaigning is a follower knowing no leader before and after each round, so
    // keying on those two says nothing at all while the term climbs -- which is
    // exactly the storm issue #117's dump could not be read for.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    RaftDriver driver {
        std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine
    };

    auto reported = std::vector<RaftDriver::RoleChange> {};
    driver.ObserveRole([&reported](RaftDriver::RoleChange const& change) { reported.push_back(change); });

    // Two campaigns by a peer, well inside this node's own election timeout so it
    // never stands for anything itself. It is a follower with no known leader
    // throughout -- the state it started in -- and only the term moves.
    for (auto const term: { std::uint64_t { 2 }, std::uint64_t { 3 } })
        REQUIRE(driver
                    .Receive(RequestVoteRequest { .term = Term { .value = term },
                                                  .candidateId = "n2",
                                                  .lastLogIndex = LogIndex::BeforeFirst(),
                                                  .lastLogTerm = Term::None() },
                             TimePoint {} + 10ms)
                    .has_value());

    // Collected with a loop rather than `std::ranges::to`, which this repository
    // uses nowhere else and which does not compile under clang against
    // libstdc++ 14 -- one of the standard libraries CI builds against.
    auto terms = std::vector<std::uint64_t> {};
    for (auto const& change: reported)
        terms.push_back(change.term.value);

    CHECK(terms == std::vector<std::uint64_t> { 2, 3 });

    for (auto const& change: reported)
    {
        CHECK(change.role == Role::Follower);
        CHECK_FALSE(change.knownLeader.has_value());
        REQUIRE(change.cause.has_value());
        CHECK(Unwrap(change.cause).from == "n2");
    }
}
