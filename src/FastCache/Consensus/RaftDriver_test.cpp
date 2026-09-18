// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/ClusterStateMachine.hpp>
#include <FastCache/Consensus/FileRaftStorage.hpp>
#include <FastCache/Consensus/InMemoryRaftStorage.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Logger.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
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

    /// Journalled as "read", so a case can see WHEN recovery asked -- before the
    /// snapshot, or after it. Refuses only the one command a case named.
    [[nodiscard]] std::expected<void, ConsensusError> CanRead(std::span<std::byte const> command) const override
    {
        _journal.events.emplace_back("read");
        if (_unreadableCommand.has_value() && std::ranges::equal(command, *_unreadableCommand))
            return std::unexpected { FastCache::UnsupportedFormatVersion("a command this fake was told it cannot read") };
        return {};
    }

    /// Journalled as "can-restore", so a case can see that the question came before
    /// anything was persisted. Refuses while a case has said so.
    [[nodiscard]] std::expected<void, ConsensusError> CanRestore(std::span<std::byte const> state) const override
    {
        std::ignore = state;
        _journal.events.emplace_back("can-restore");
        if (_unreadableSnapshots)
            return std::unexpected { FastCache::UnsupportedFormatVersion("a snapshot this fake was told it cannot read") };
        return {};
    }

    [[nodiscard]] std::vector<std::byte> TakeSnapshot() override
    {
        _journal.events.emplace_back("snapshot");
        return _state;
    }

    [[nodiscard]] std::expected<void, ConsensusError> RestoreSnapshot(std::span<std::byte const> state,
                                                                      SnapshotOrigin origin) override
    {
        _origins.push_back(origin);
        if (_refuseRestore)
        {
            // Refused, and NOTHING changes: the contract, held by the fake too.
            _journal.events.emplace_back("restore-refused");
            return std::unexpected { FastCache::UnsupportedFormatVersion("a snapshot this fake was told it cannot read") };
        }

        _journal.events.emplace_back("restore");
        _state.assign(state.begin(), state.end());

        // A restore REPLACES: anything applied before it is described by the
        // snapshot, so keeping it would double-count.
        _applied.clear();
        return {};
    }

    /// Refuse `command` from now on, as a build that cannot read it would.
    /// @param command The payload to refuse.
    void RefuseCommand(std::vector<std::byte> command)
    {
        _unreadableCommand = std::move(command);
    }

    /// Refuse every snapshot from now on, when handed one to restore.
    void RefuseRestore() noexcept
    {
        _refuseRestore = true;
    }

    /// Answer `CanRestore` with a refusal from now on, or accept again.
    /// @param unreadable Whether snapshots are beyond this fake.
    void SetSnapshotsUnreadable(bool unreadable) noexcept
    {
        _unreadableSnapshots = unreadable;
    }

    /// @return Which caller each restore came from, in order.
    [[nodiscard]] std::vector<SnapshotOrigin> const& Origins() const noexcept
    {
        return _origins;
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
    std::vector<SnapshotOrigin> _origins;
    std::optional<std::vector<std::byte>> _unreadableCommand;
    bool _refuseRestore = false;
    bool _unreadableSnapshots = false;
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
                        .voters = { "solo" },
                        .learners = {},
                        .electionTimeoutMin = 150ms,
                        .electionTimeoutMax = 300ms,
                        .heartbeatInterval = 50ms };
}

[[nodiscard]] RaftConfig TrioConfig()
{
    return RaftConfig { .self = "n1",
                        .voters = { "n1", "n2", "n3" },
                        .learners = {},
                        .electionTimeoutMin = 150ms,
                        .electionTimeoutMax = 300ms,
                        .heartbeatInterval = 50ms };
}

/// Build a driver the one way there is, requiring that recovery accepted the node.
///
/// Every case below that is not ABOUT a refusal builds through this, so the one that
/// is about one is the only place `Create`'s error is read.
/// @param node The node, as recovered or fresh.
/// @param storage Its durable state.
/// @param transport Its peers.
/// @param machine Its application.
/// @param compaction When it trims its log.
/// @return The driver.
[[nodiscard]] std::unique_ptr<RaftDriver> MakeDriver(RaftNode node,
                                                     IRaftStorage& storage,
                                                     IRaftTransport& transport,
                                                     IRaftStateMachine& machine,
                                                     CompactionPolicy compaction = {})
{
    auto driver = RaftDriver::Create(std::move(node), storage, transport, machine, compaction);
    REQUIRE(driver.has_value());
    return *std::move(driver);
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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

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
    for (auto const& event: std::ranges::subrange { journal.events.begin(), firstSend })
        CHECK(event != "send");

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(SoloConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, clock.Now())).value(), storage, transport, machine);
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, clock.Now())).value(), storage, transport, machine);
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, clock.Now())).value(), storage, transport, machine);
    auto& driver = *owned;

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

    auto const owned = MakeDriver(std::move(RaftNode::Create(SoloConfig(), random, TimePoint {})).value(),
                                  storage,
                                  transport,
                                  machine,
                                  CompactionPolicy { .appliedEntriesBeforeCompaction = Threshold });
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(SoloConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

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
    auto const owned = MakeDriver(std::move(RaftNode::Create(SoloConfig(), random, TimePoint {})).value(),
                                  storage,
                                  transport,
                                  machine,
                                  CompactionPolicy { .appliedEntriesBeforeCompaction = 2 });
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

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

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

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

TEST_CASE("CurrentProgress reports the role and the leader, under the same lock", "[consensus][raft][driver]")
{
    // `CurrentProgress` is the ONE read anything outside this driver has of what a
    // node believes about its own cluster
    // ([#435](https://github.com/LASTRADA-Software/fastcached/issues/435)), and until
    // it carried these two it could not answer the question at all: a member set and
    // a commit index say nothing about whether this node is campaigning or who it
    // follows.
    //
    // The alternative is `Node().CurrentRole()` beside `Node().ActiveConfiguration()`, and
    // `Node()` says in as many words that it is not synchronized. That is four reads
    // of a state machine the timer loop and every peer reader are free to move, and
    // the moment they disagree is an election -- which is exactly when somebody is
    // reading this.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    RecordingMachine machine { journal };
    ScriptedRandomSource random { { 0 } };

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

    // A quiet follower knows its members and no leader. Asserted BEFORE anything
    // moves, because "names no leader" is the state an election is in and a case
    // that only ever saw a settled cluster could not tell the two apart.
    auto const quiet = driver.CurrentProgress();
    CHECK(quiet.role == Role::Follower);
    CHECK_FALSE(quiet.knownLeader.has_value());
    CHECK(quiet.term == Term { .value = 0 });
    CHECK(quiet.configuration == Configuration { .voters = { "n1", "n2", "n3" }, .learners = {} });

    // Campaigning: a role that moved without a leader appearing, which is the
    // reading that separates a node standing for election from one quietly
    // following. Both hold the same members and the same commit index.
    REQUIRE(driver.Tick(TimePoint {} + 150ms).has_value());
    REQUIRE(CarryPreVote(driver, TimePoint {} + 150ms));

    auto const campaigning = driver.CurrentProgress();
    CHECK(campaigning.role == Role::Candidate);
    CHECK_FALSE(campaigning.knownLeader.has_value());
    CHECK(campaigning.term == Term { .value = 1 });

    // And elected: the role and the leader move together, and the leader is this
    // node -- which `role == Leader` also says, so the pairing is what proves the
    // field is filled from the node rather than defaulted.
    REQUIRE(
        driver
            .Receive(RequestVoteResponse { .term = Term { .value = 1 }, .decision = VoteDecision::Granted, .voterId = "n2" },
                     TimePoint {} + 150ms)
            .has_value());

    auto const elected = driver.CurrentProgress();
    CHECK(elected.role == Role::Leader);
    CHECK(elected.knownLeader == std::optional<NodeId> { "n1" });
    CHECK(elected.term == Term { .value = 1 });

    // Deposed by a higher term from a peer: a leader this node is NOT, which no
    // other field here reports.
    REQUIRE(driver
                .Receive(AppendEntriesRequest { .term = Term { .value = 2 },
                                                .leaderId = "n3",
                                                .prevLogIndex = LogIndex::BeforeFirst(),
                                                .prevLogTerm = Term::None(),
                                                .entries = {},
                                                .leaderCommit = LogIndex::BeforeFirst() },
                         TimePoint {} + 200ms)
                .has_value());

    auto const deposed = driver.CurrentProgress();
    CHECK(deposed.role == Role::Follower);
    CHECK(deposed.knownLeader == std::optional<NodeId> { "n3" });
    CHECK(deposed.term == Term { .value = 2 });
}

// --------------------------------------------------------------------------
// Recovery hands the snapshot to the application (#1542).

TEST_CASE("Constructing a driver over a recovered snapshot restores it before anything else happens",
          "[consensus][raft][driver][snapshot]")
{
    // The contract at its narrowest: the node recovered a snapshot, and before any
    // step -- before anything could apply an entry above it -- the application has
    // been handed the snapshot's state. And the control: a node that recovered no
    // snapshot restores nothing, because "no snapshot" is not "an empty state".
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    ScriptedRandomSource random { { 0 } };

    SECTION("a recovered snapshot reaches the application at construction")
    {
        auto const state = FastCache::BytesFromString("the state as of index 5");
        auto recovered =
            RecoveredState { .state = PersistentState { .currentTerm = Term { .value = 2 }, .votedFor = std::nullopt },
                             .entries = {},
                             .firstIndex = LogIndex { .value = 6 },
                             .snapshot = RaftSnapshot { .lastIncludedIndex = LogIndex { .value = 5 },
                                                        .lastIncludedTerm = Term { .value = 2 },
                                                        .configuration = SoloConfig().Bootstrap(),
                                                        .state = state } };

        RecordingMachine machine { journal };
        auto const owned =
            MakeDriver(std::move(RaftNode::Create(SoloConfig(), random, TimePoint {}, std::move(recovered))).value(),
                       storage,
                       transport,
                       machine);
        auto const& driver = *owned;

        REQUIRE(journal.events == std::vector<std::string> { "restore" });
        CHECK(machine.Origins() == std::vector { SnapshotOrigin::Recovered });
        CHECK(machine.State() == state);
        CHECK(machine.Applied().empty());
        CHECK(driver.Node().LastApplied() == LogIndex { .value = 5 });
    }

    SECTION("a node with no snapshot restores nothing")
    {
        RecordingMachine machine { journal };
        auto const owned =
            MakeDriver(std::move(RaftNode::Create(SoloConfig(), random, TimePoint {})).value(), storage, transport, machine);
        auto const& driver = *owned;

        CHECK(journal.events.empty());
        CHECK(driver.Node().SnapshotIndex() == LogIndex::BeforeFirst());
    }
}

namespace
{
/// A transport for a node that has nobody to talk to.
class NoPeers final: public IRaftTransport
{
  public:
    void Send(NodeId const& to, RaftMessage message) override
    {
        std::ignore = to;
        std::ignore = message;
    }
};

/// Propose one cluster command on a leading driver, requiring that it landed.
/// @param driver A driver whose node leads.
/// @param command The change.
/// @param at When.
/// @return The index it landed at.
[[nodiscard]] LogIndex ProposeCommand(RaftDriver& driver, Cluster::Command const& command, TimePoint at)
{
    auto const landed = driver.Propose(Cluster::Encode(command), at);
    REQUIRE(landed.has_value());
    return *landed;
}

/// Build a solo driver over `store` and `machine`, recovered from `store`, the way
/// `ConsensusTier` builds one: `Load`, `RaftNode::Create`, construct.
/// @param store The node's durable state.
/// @param machine Its application.
/// @param transport Its peers, of which it has none.
/// @param random Its randomness.
/// @param start When it starts.
/// @return The driver, a follower until `Elect`.
[[nodiscard]] std::unique_ptr<RaftDriver> SoloOver(
    FileRaftStorage& store, IRaftStateMachine& machine, NoPeers& transport, IRandomSource& random, TimePoint start)
{
    auto recovered = store.Load();
    REQUIRE(recovered.has_value());
    auto node = RaftNode::Create(SoloConfig(), random, start, *std::move(recovered));
    REQUIRE(node.has_value());
    return MakeDriver(*std::move(node), store, transport, machine, CompactionPolicy { .appliedEntriesBeforeCompaction = 4 });
}

/// Carry a solo driver to leadership.
/// @param driver The driver.
/// @param at When its election timeout falls due.
void Elect(RaftDriver& driver, TimePoint at)
{
    REQUIRE(driver.Tick(at).has_value());
    REQUIRE(CarryPreVote(driver, at));
    REQUIRE(driver.Node().CurrentRole() == Role::Leader);
}
} // namespace

TEST_CASE("A node restarted after compacting comes back holding every cluster fact its snapshot covered",
          "[consensus][raft][driver][snapshot][cluster]")
{
    // #1542 at the production seam: the store `ConsensusTier` opens, the state machine
    // it applies to, and a driver built the way it builds one. A node compacted past a
    // member, a setting and a forget tombstone, then restarted. Before the fix the
    // recovered node's applied index sat at the snapshot's boundary and nothing handed
    // the snapshot to the application, so all three were gone -- and a forgotten client
    // was admitted again, with nothing reporting it: removal failing OPEN.
    ScratchDirectory scratch { "fc-raft-restore" };
    NullLogger logger;
    NoPeers transport;
    ScriptedRandomSource random { { 0 } };

    auto forgottenAt = LogIndex {};
    auto aboveAt = LogIndex {};
    auto before = Cluster::ClusterState {};

    {
        auto store = FileRaftStorage::Open(scratch.Path());
        REQUIRE(store.has_value());
        Cluster::ClusterStateMachine machine { logger, {} };
        auto const driver = SoloOver(*store, machine, transport, random, TimePoint {});
        Elect(*driver, TimePoint {} + 150ms);

        auto const at = TimePoint {} + 200ms;
        (void) ProposeCommand(*driver,
                              Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                                                 .key = "n2",
                                                 .value = "10.0.0.2:6675",
                                                 .schedulerEndpoint = {},
                                                 .publicKey = std::nullopt,
                                                 .role = std::nullopt },
                              at);
        forgottenAt = ProposeCommand(*driver,
                                     Cluster::Command { .kind = Cluster::CommandKind::ForgetClient,
                                                        .key = "10.0.0.7",
                                                        .value = {},
                                                        .schedulerEndpoint = {},
                                                        .publicKey = std::nullopt,
                                                        .role = std::nullopt },
                                     at);
        (void) ProposeCommand(*driver,
                              Cluster::Command { .kind = Cluster::CommandKind::SetSetting,
                                                 .key = std::string { Cluster::LeaseLifetimeSetting },
                                                 .value = "40min",
                                                 .schedulerEndpoint = {},
                                                 .publicKey = std::nullopt,
                                                 .role = std::nullopt },
                              at);

        // Padded until the snapshot covers the tombstone -- counted by the boundary
        // rather than by a hand-computed number of entries, for the compaction case's
        // reason: a new leader's own no-op is an entry too. Bounded, so a driver that
        // never compacts fails here rather than looping.
        for (auto const step: std::views::iota(0, 16))
        {
            if (driver->Node().SnapshotIndex() >= forgottenAt)
                break;
            (void) ProposeCommand(*driver,
                                  Cluster::Command { .kind = Cluster::CommandKind::SetSetting,
                                                     .key = std::string { Cluster::FleetOpenSetting },
                                                     .value = step % 2 == 0 ? "1" : "0",
                                                     .schedulerEndpoint = {},
                                                     .publicKey = std::nullopt,
                                                     .role = std::nullopt },
                                  at);
        }
        REQUIRE(driver->Node().SnapshotIndex() >= forgottenAt);

        // And one fact ABOVE the snapshot, which a restart re-applies rather than
        // restores -- so the case sees both halves of recovery, in their order.
        aboveAt = ProposeCommand(*driver,
                                 Cluster::Command { .kind = Cluster::CommandKind::AdmitClient,
                                                    .key = "10.0.0.9",
                                                    .value = {},
                                                    .schedulerEndpoint = {},
                                                    .publicKey = std::nullopt,
                                                    .role = std::nullopt },
                                 at);
        REQUIRE(driver->Node().SnapshotIndex() < aboveAt);

        before = machine.State();
        REQUIRE(before.HasForgotten("10.0.0.7"));
        REQUIRE(before.AdmitsClient("10.0.0.9"));
    }

    // The restart: the same directory, a fresh state machine -- a process remembers
    // nothing it applied -- and a driver built exactly as before.
    auto store = FileRaftStorage::Open(scratch.Path());
    REQUIRE(store.has_value());
    Cluster::ClusterStateMachine machine { logger, {} };
    auto const driver = SoloOver(*store, machine, transport, random, TimePoint {} + 1s);
    REQUIRE(driver->Node().SnapshotIndex() >= forgottenAt);

    // Before a single step: everything the snapshot covered is back.
    //
    // The tombstone is a REQUIRE, and not for tidiness. A missed restore fails every
    // check below together, and exactly four of them used to: a Catch2 binary's exit
    // status is its failed-assertion count, and four collides with `SKIP_RETURN_CODE 4`,
    // so the neutered fix was scored SKIPPED rather than failed (#1152) -- measured, on
    // this case, before this line was a REQUIRE. Stopping at the headline property
    // makes that defect one failure, whatever is appended below it.
    auto const restored = machine.State();
    REQUIRE(restored.HasForgotten("10.0.0.7"));
    CHECK(restored.RaftEndpointOf("n2") == std::optional<std::string> { "10.0.0.2:6675" });
    CHECK(restored.SettingOf(Cluster::LeaseLifetimeSetting) == std::optional<std::string> { "40min" });
    // And nothing above it yet: that is re-applied once it is committed again, never
    // restored -- a snapshot is state as of its index and no further.
    CHECK_FALSE(restored.AdmitsClient("10.0.0.9"));

    // Leading again, it commits what it holds, and the entry above the snapshot lands ON
    // TOP of the restored state rather than in place of it: the whole state is what it
    // was before the restart.
    Elect(*driver, TimePoint {} + 1s + 150ms);
    CHECK(driver->Node().LastApplied() >= aboveAt);
    CHECK(machine.State() == before);
}

TEST_CASE("A driver over recovered state its application cannot read is refused, and the application is handed nothing",
          "[consensus][raft][driver][snapshot]")
{
    // The follow-up to #1542: recovery restores a node's snapshot, and a node whose own
    // snapshot -- or a command its own log holds above it -- is something the application
    // cannot read does not get a driver at all. Asked in ONE order, which is what makes a
    // refusal hand the application nothing: every command first, read-only, then the
    // snapshot, which replaces or changes nothing. The journal is the evidence -- a
    // refused command must leave no "restore" in it -- so it is each section's REQUIRE:
    // a wrong order fails every check after it together, and four failures in one run are
    // scored SKIPPED rather than failed (#1152).
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    ScriptedRandomSource random { { 0 } };
    RecordingMachine machine { journal };

    auto const state = FastCache::BytesFromString("the state as of index 5");
    auto const readable = FastCache::BytesFromString("a command at index 7");
    auto const unreadable = FastCache::BytesFromString("a command at index 8");

    // A no-op at 6, which is Raft's own and never the application's: it is not asked
    // about, so a case counting reads sees two, not three.
    auto const recovered = [&] {
        return RecoveredState {
            .state = PersistentState { .currentTerm = Term { .value = 2 }, .votedFor = std::nullopt },
            .entries = { LogEntry { .term = Term { .value = 2 }, .kind = EntryKind::NoOp, .payload = {} },
                         LogEntry { .term = Term { .value = 2 }, .kind = EntryKind::Command, .payload = readable },
                         LogEntry { .term = Term { .value = 2 }, .kind = EntryKind::Command, .payload = unreadable } },
            .firstIndex = LogIndex { .value = 6 },
            .snapshot = RaftSnapshot { .lastIncludedIndex = LogIndex { .value = 5 },
                                       .lastIncludedTerm = Term { .value = 2 },
                                       .configuration = SoloConfig().Bootstrap(),
                                       .state = state }
        };
    };
    auto const create = [&] {
        return RaftDriver::Create(std::move(RaftNode::Create(SoloConfig(), random, TimePoint {}, recovered())).value(),
                                  storage,
                                  transport,
                                  machine);
    };

    SECTION("control: every command is asked about, and only then is the snapshot restored")
    {
        auto const driver = create();
        REQUIRE(driver.has_value());
        REQUIRE(journal.events == std::vector<std::string> { "read", "read", "restore" });
        CHECK(machine.State() == state);
        CHECK(machine.Origins() == std::vector { SnapshotOrigin::Recovered });
    }

    SECTION("a command the application cannot read refuses the driver before the snapshot is touched")
    {
        machine.RefuseCommand(unreadable);
        auto const driver = create();
        REQUIRE_FALSE(driver.has_value());
        CHECK(driver.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
        CHECK(driver.error().context.starts_with("log entry 8: "));

        // Nothing handed over: no restore was even attempted, so the snapshot's state is
        // not sitting in an application whose node will never run.
        REQUIRE(journal.events == std::vector<std::string> { "read", "read" });
        CHECK(machine.State().empty());
        CHECK(machine.Origins().empty());
    }

    SECTION("a snapshot the application cannot read refuses the driver and changes nothing")
    {
        machine.RefuseRestore();
        auto const driver = create();
        REQUIRE_FALSE(driver.has_value());
        CHECK(driver.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
        CHECK(driver.error().context.starts_with("the snapshot as of log entry 5: "));
        REQUIRE(journal.events == std::vector<std::string> { "read", "read", "restore-refused" });
        CHECK(machine.State().empty());
        CHECK(machine.Origins() == std::vector { SnapshotOrigin::Recovered });
    }
}

TEST_CASE("A follower's driver refuses a leader's snapshot its application cannot read, and applies nothing after it",
          "[consensus][raft][driver][snapshot]")
{
    // #1552 at the driver: the application is asked BEFORE the node sees the offer, so a
    // refusal reaches the node as a verdict and nothing is persisted, acknowledged or
    // restored. The leader is answered `Rejected`; the refusal is held and reported ONCE,
    // however often the leader offers again; the entry after the snapshot is not applied
    // on the stale base -- this node does not even hold the entry it would follow; and the
    // refusal ends by itself once the node can read what it is offered.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    ScriptedRandomSource random { { 0 } };
    RecordingMachine machine { journal };
    machine.SetSnapshotsUnreadable(true);

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

    auto reported = std::vector<std::optional<RaftDriver::InstallRefusal>> {};
    driver.ObserveInstallRefusal(
        [&reported](std::optional<RaftDriver::InstallRefusal> const& refusal) { reported.push_back(refusal); });

    auto const offer = InstallSnapshotRequest { .term = Term { .value = 1 },
                                                .leaderId = "n2",
                                                .lastIncludedIndex = LogIndex { .value = 5 },
                                                .lastIncludedTerm = Term { .value = 1 },
                                                .configuration = TrioConfig().Bootstrap(),
                                                .state = FastCache::BytesFromString("the state as of index 5") };

    REQUIRE(driver.Receive(offer, TimePoint {} + 10ms).has_value());

    // Asked first, and nothing taken on: no snapshot persisted, none restored.
    REQUIRE(std::ranges::find(journal.events, std::string { "can-restore" }) != journal.events.end());
    CHECK(std::ranges::find(journal.events, std::string { "persist-snapshot" }) == journal.events.end());
    CHECK(std::ranges::find(journal.events, std::string { "restore" }) == journal.events.end());
    CHECK(driver.Node().LastApplied() == LogIndex::BeforeFirst());

    // The leader was told no.
    REQUIRE_FALSE(transport.Sent().empty());
    auto const* const answer = std::get_if<InstallSnapshotResponse>(&transport.Sent().back().second);
    REQUIRE(answer != nullptr);
    CHECK(answer->result == AppendResult::Rejected);

    // Held, and reported once however often the leader offers again.
    auto const held = driver.CurrentProgress().installRefusal;
    REQUIRE(held.has_value());
    auto const refusal = Unwrap(held);
    CHECK(refusal.index == LogIndex { .value = 5 });
    CHECK(refusal.leader == "n2");
    CHECK(refusal.reason.code == ConsensusErrorCode::UnsupportedFormatVersion);
    REQUIRE(driver.Receive(offer, TimePoint {} + 60ms).has_value());
    REQUIRE(driver.Receive(offer, TimePoint {} + 110ms).has_value());
    CHECK(reported.size() == 1);

    // The entry after the snapshot is not applied on the stale base: the node does not hold
    // the entry it would follow, so the append is refused and nothing reaches the machine.
    REQUIRE(driver
                .Receive(AppendEntriesRequest { .term = Term { .value = 1 },
                                                .leaderId = "n2",
                                                .prevLogIndex = LogIndex { .value = 5 },
                                                .prevLogTerm = Term { .value = 1 },
                                                .entries = { LogEntry { .term = Term { .value = 1 },
                                                                        .kind = EntryKind::Command,
                                                                        .payload = FastCache::BytesFromString("six") } },
                                                .leaderCommit = LogIndex { .value = 6 } },
                         TimePoint {} + 120ms)
                .has_value());
    CHECK(machine.Applied().empty());
    CHECK(driver.Node().LastApplied() == LogIndex::BeforeFirst());

    SECTION("and once it can read the snapshot, it takes it on and the refusal ends")
    {
        machine.SetSnapshotsUnreadable(false);
        REQUIRE(driver.Receive(offer, TimePoint {} + 160ms).has_value());
        CHECK(driver.Node().LastApplied() == LogIndex { .value = 5 });
        CHECK_FALSE(driver.CurrentProgress().installRefusal.has_value());
        REQUIRE(reported.size() == 2);
        CHECK_FALSE(reported.back().has_value());
    }
}

TEST_CASE("A machine that accepts a snapshot and then refuses to restore it stops the driver", "[consensus][raft][driver]")
{
    // The one ordering the question-first design cannot rule out by itself: `CanRestore`
    // said yes, the node took the snapshot on -- persisted it, acknowledged it -- and then
    // `RestoreSnapshot` said no. Continuing would apply every later entry on the state the
    // snapshot was meant to replace, which is #1552 by another road, so the driver stops as
    // it does for a failed durability write, and says why.
    Journal journal;
    RecordingStorage storage { journal };
    RecordingTransport transport { journal };
    ScriptedRandomSource random { { 0 } };
    RecordingMachine machine { journal };
    machine.RefuseRestore();

    auto const owned =
        MakeDriver(std::move(RaftNode::Create(TrioConfig(), random, TimePoint {})).value(), storage, transport, machine);
    auto& driver = *owned;

    auto const received = driver.Receive(InstallSnapshotRequest { .term = Term { .value = 1 },
                                                                  .leaderId = "n2",
                                                                  .lastIncludedIndex = LogIndex { .value = 5 },
                                                                  .lastIncludedTerm = Term { .value = 1 },
                                                                  .configuration = TrioConfig().Bootstrap(),
                                                                  .state = FastCache::BytesFromString("state") },
                                         TimePoint {} + 10ms);
    REQUIRE_FALSE(received.has_value());
    CHECK(received.error().code == ConsensusErrorCode::UnsupportedFormatVersion);
    CHECK(driver.Failure().has_value());
    CHECK_FALSE(driver.Tick(TimePoint {} + 500ms).has_value());
}
