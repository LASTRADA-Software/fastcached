// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Consensus/InMemoryRaftStorage.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace FastCache::Consensus;
using namespace std::chrono_literals;

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
