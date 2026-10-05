// SPDX-License-Identifier: Apache-2.0
#include "NodeConditions.hpp"
#include "NodeIdentity.hpp"
#include "NodeMembership.hpp"
#include "NodeRoster.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/StateLeaseRoster.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <core/Ranges.hpp>
#include <core/platform/Clock.hpp>
#include <tests/NodeConditionFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScratchPath.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using namespace std::chrono_literals;

namespace Wire = CompileCacheWire;
using FastCache::Testing::TestKeyPair;

namespace
{
/// The steady clock a roster under test measures leader silence on. One object for the whole
/// binary, so it outlives every roster a case builds; no case here waits out the bound.
/// @return The clock.
[[nodiscard]] ::core::platform::IClock const& RosterClock()
{
    static ::core::platform::SteadyClock const clock;
    return clock;
}

/// The instant every case is decided at.
constexpr auto Noon = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };

/// A worker with no consensus: its fleet, and nothing reaching it but this machine.
[[nodiscard]] NodeConfig Worker()
{
    auto cfg = Testing::FirstStart(NodeConfig {});
    cfg.schedulers = { "127.0.0.1:6674" };
    cfg.clusterId = "fleet";
    // A worker that runs no consensus, which holds no roster at all.
    cfg.raftListen.clear();
    return cfg;
}

/// The same worker, reachable from other machines: a wildcard bind that admits everybody.
[[nodiscard]] NodeConfig NetworkFacingWorker()
{
    auto cfg = Worker();
    cfg.nodeListen = "0.0.0.0:6674";
    cfg.fleetOpen = true;
    return cfg;
}

/// A roster of three keyed voters.
[[nodiscard]] Cluster::Roster ThreeVoters()
{
    Cluster::Roster roster;
    for (auto const* const id: { "n1", "n2", "n3" })
        roster.members.push_back(Cluster::RosterMember { .id = id,
                                                         .raftEndpoint = std::string { id } + ":6680",
                                                         .seat = Cluster::MemberSeat::Voter,
                                                         .publicKey = TestKeyPair(id).PublicKey(),
                                                         .schedulerEndpoint = {} });
    return roster;
}

} // namespace

TEST_CASE("A node's lease roster is the state it applied, which the wall clock never lapses", "[node][roster]")
{
    auto cfg = Worker();
    cfg.raftListen = "127.0.0.1:6680";
    auto const roster = NodeRoster::Build(cfg, RosterClock(), nullptr);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);
    // The one roster there is: the state this node's own consensus applied.
    REQUIRE(dynamic_cast<Distributed::StateLeaseRoster const*>(node.Lease()) != nullptr);

    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = TestKeyPair("n1").PublicKey() } };
    state.rosterVersion = 4;
    node.Applied(state);

    CHECK(node.Lease()->KeysOf("n1").live == TestKeyPair("n1").PublicKey());
    auto const summary = node.Summary();
    REQUIRE(summary.has_value());
    CHECK(Testing::Unwrap(summary).version == 4);
    CHECK(Testing::Unwrap(summary).voters == 1);
    // The wall clock lapses nothing: a learner whose machine was shut for days reads the state it
    // last applied. Only silence on the STEADY clock while it runs does (`LeaderSilenceBound`).
    CHECK(node.Lease()->Read(Noon + std::chrono::days { 30 }).standing == Distributed::RosterStanding::Current);

    // And the roster `explain-admission <machine>` answers from is the state's own projection.
    CHECK(node.HeldRoster() == std::optional { Cluster::ProjectRoster(state) });
}

TEST_CASE("A consensus pass past the silence bound raises consensus-leader-silent, and one contact clears it",
          "[node][roster][conditions][isolation]")
{
    // The row and the refusals it explains read ONE fact -- the roster's own standing -- so they
    // cannot disagree: raised exactly while every grant is refused `Isolated`, cleared at the first
    // contact from a leader the applied configuration counts.
    auto cfg = Worker();
    cfg.raftListen = "127.0.0.1:6680";
    core::platform::ManualClock clock;
    NodeConditions conditions;
    auto const roster = NodeRoster::Build(cfg, clock, &conditions);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);
    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = TestKeyPair("n1").PublicKey() } };
    node.Applied(state);

    auto const silent = Distributed::LeaderReading {};
    clock.advance(Distributed::LeaderSilenceBound - std::chrono::seconds { 1 });
    node.ConsensusPass(silent);
    CHECK(conditions.StateOf(NodeCondition::ConsensusLeaderSilent) == Wire::ConditionState::Clear);
    CHECK(node.Lease()->Read(Noon).standing == Distributed::RosterStanding::Current);

    clock.advance(std::chrono::seconds { 2 });
    node.ConsensusPass(silent);
    CHECK(conditions.StateOf(NodeCondition::ConsensusLeaderSilent) == Wire::ConditionState::Raised);
    CHECK(Testing::DetailOf(conditions, NodeCondition::ConsensusLeaderSilent).contains("every lease grant is refused"));
    CHECK(node.Lease()->Read(Noon).standing == Distributed::RosterStanding::Isolated);

    node.ConsensusPass(
        Distributed::LeaderReading { .leads = false, .leader = "n1", .silentFor = std::chrono::seconds { 0 } });
    CHECK(conditions.StateOf(NodeCondition::ConsensusLeaderSilent) == Wire::ConditionState::Clear);
    CHECK(node.Lease()->Read(Noon).standing == Distributed::RosterStanding::Current);
}

TEST_CASE("A learner that slept past the silence bound refuses grants on waking, and its leader's first word restores them",
          "[node][roster][isolation][sleep]")
{
    // Review focus 5, on the path every formation node runs (W-9): the certified roster and its
    // expiry -- what the old sleep cases pinned -- are gone (c7f3b5610), and a node's roster is the
    // state its own consensus applied, a `StateLeaseRoster`. A sleeping process runs no pass at all;
    // what it wakes to is a steady clock that moved two hours at once. Its first pass hears nobody
    // yet (the session is still re-forming), so every grant is refused `Isolated` -- never served
    // against a roster nobody refreshed -- and the first reading from a leader the applied state
    // counts restores it, with no host event involved. (That the reading ARRIVES after a sleep is
    // the learner session's liveness, W-3, not this roster's.)
    auto cfg = Worker();
    cfg.raftListen = "127.0.0.1:6680";
    core::platform::ManualClock clock;
    NodeConditions conditions;
    auto const roster = NodeRoster::Build(cfg, clock, &conditions);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);
    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = TestKeyPair("n1").PublicKey() } };
    node.Applied(state);
    auto const leaderSpoke =
        Distributed::LeaderReading { .leads = false, .leader = "n1", .silentFor = std::chrono::seconds { 0 } };
    node.ConsensusPass(leaderSpoke);
    REQUIRE(node.Lease()->Read(Noon).standing == Distributed::RosterStanding::Current);

    // Two hours asleep: no pass ran, and the steady clock moved at once.
    clock.advance(std::chrono::hours { 2 });
    node.ConsensusPass(Distributed::LeaderReading {});
    CHECK(node.Lease()->Read(Noon + std::chrono::hours { 2 }).standing == Distributed::RosterStanding::Isolated);
    CHECK(conditions.StateOf(NodeCondition::ConsensusLeaderSilent) == Wire::ConditionState::Raised);

    // The leader's first word after the wake.
    node.ConsensusPass(leaderSpoke);
    CHECK(node.Lease()->Read(Noon + std::chrono::hours { 2 }).standing == Distributed::RosterStanding::Current);
    CHECK(conditions.StateOf(NodeCondition::ConsensusLeaderSilent) == Wire::ConditionState::Clear);
}

TEST_CASE("A consensus member places a server only once its applied state names a voter's key", "[node][roster][proof]")
{
    // #178 PR 6: a node proves itself only to a server its roster does not call a stranger. A
    // consensus member's roster is the state it applied, and until the first commit records a
    // voter's key -- its OWN, when it schedules for itself -- there is no voter a server could have
    // been. Calling it `NotVoter` then made a scheduler that also runs a worker refuse to prove
    // itself to itself until a heartbeat round after that commit, with a warning at every start.
    auto cfg = Worker();
    cfg.raftListen = "127.0.0.1:6680";
    auto const roster = NodeRoster::Build(cfg, RosterClock(), nullptr);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);

    CHECK(node.StandingOf("n1", TestKeyPair("n1").PublicKey()) == ServerStanding::Unchecked);

    Cluster::ClusterState state;
    state.members = { Cluster::ClusterMember { .id = "n1",
                                               .raftEndpoint = "n1:6680",
                                               .schedulerEndpoint = {},
                                               .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                               .seat = Cluster::MemberSeat::Voter,
                                               .publicKey = TestKeyPair("n1").PublicKey() } };
    state.revokedKeys = { Cluster::RevokedKey { .id = Consensus::NodeId { "n0" },
                                                .publicKey = TestKeyPair("n0").PublicKey() } };
    node.Applied(state);

    // Once a voter's key is known, the three answers are the roster's.
    CHECK(node.StandingOf("n1", TestKeyPair("n1").PublicKey()) == ServerStanding::Voter);
    CHECK(node.StandingOf("n1", TestKeyPair("x").PublicKey()) == ServerStanding::NotVoter);
    CHECK(node.StandingOf("n0", TestKeyPair("n0").PublicKey()) == ServerStanding::Revoked);
}

TEST_CASE("A consensus member names a revoked server as revoked before it knows any voter's key", "[node][roster][proof]")
{
    // The control on the rule above: what a member has NOT learned yet excuses a stranger, never a
    // machine the state it applied says was forgotten.
    auto cfg = Worker();
    cfg.raftListen = "127.0.0.1:6680";
    auto const roster = NodeRoster::Build(cfg, RosterClock(), nullptr);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);

    Cluster::ClusterState state;
    state.revokedKeys = { Cluster::RevokedKey { .id = Consensus::NodeId { "n0" },
                                                .publicKey = TestKeyPair("n0").PublicKey() } };
    node.Applied(state);

    CHECK(node.StandingOf("n0", TestKeyPair("n0").PublicKey()) == ServerStanding::Revoked);
}

TEST_CASE("A worker no other machine can reach holds no roster and checks no grant", "[node][roster]")
{
    auto const roster = NodeRoster::Build(Worker(), RosterClock(), nullptr);
    REQUIRE(roster.has_value());
    auto& node = *Testing::Unwrap(roster);
    CHECK(node.Lease() == nullptr);
    CHECK_FALSE(node.Summary().has_value());
    CHECK_FALSE(node.HeldRoster().has_value());
}

TEST_CASE("A worker other machines can reach refuses to start when it runs no consensus", "[node][roster]")
{
    // Where `RosterlessWorkerRefusal` is answered: the only roster is the state a node's own
    // consensus applies, so a worker that runs none, reachable from other machines, could verify
    // no grant -- whatever its state directory holds.
    auto const scratch = Testing::ScratchDirectory { "node-roster-none" };
    auto cfg = NetworkFacingWorker();
    cfg.clusterDir = scratch.Path();

    auto const refused = NodeRoster::Build(cfg, RosterClock(), nullptr);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason == RosterlessWorkerRefusal);
    // The configuration decides, and the startup table refuses this shape first: a belt.
    CHECK(refused.error().cause == NodeRefusalCause::EarlierRule);

    // The control: the same worker running consensus starts, verifying against its state.
    cfg.raftListen = "127.0.0.1:6680";
    auto const started = NodeRoster::Build(cfg, RosterClock(), nullptr);
    REQUIRE(started.has_value());
    CHECK(Testing::Unwrap(started)->Lease() != nullptr);
}

TEST_CASE("Whether a worker must hold a roster is its mode's row, under every mode", "[node][roster][formation][mode]")
{
    // Walked over the mode TABLE, each mode reaching the roster decision itself. A worker whose
    // mode says other machines are, or are about to be, members admits them -- so with no
    // consensus, a wildcard node port and nothing kept, it has nothing to check their leases
    // against and is refused by name. A mode that dials in runs consensus whatever its flags say,
    // so its roster is the state it applies. A solitary node is this machine alone: it starts
    // checking nothing, and admits nobody else for it to have to.
    //
    // The expectation is STATED per mode rather than read off the column it tests: a case that
    // derived it from `members` would agree with any value the column held.
    struct Expected
    {
        Cluster::NodeMode mode;
        bool joined;
    };
    constexpr auto Joined = std::to_array<Expected>({
        { .mode = Cluster::NodeMode::Solitary, .joined = false },
        { .mode = Cluster::NodeMode::Pending, .joined = true },
        { .mode = Cluster::NodeMode::Learner, .joined = true },
        { .mode = Cluster::NodeMode::Voter, .joined = true },
    });
    for (auto const& row: Cluster::NodeModeTable)
    {
        INFO(row.name);
        auto const* const expected = core::findIfOrNull(Joined, [&row](Expected const& e) { return e.mode == row.mode; });
        REQUIRE(expected != nullptr);
        auto const scratch = Testing::ScratchDirectory { "node-roster-mode" };
        auto record = Cluster::FormationRecord { .mode = row.mode,
                                                 .own = { .clusterId = "own-c", .createdAtUnixSeconds = 100 },
                                                 .joining = std::nullopt,
                                                 .fleet = std::nullopt,
                                                 .archivePending = std::nullopt,
                                                 .rejectedBy = std::nullopt,
                                                 .askedJoins = {} };
        if (row.consensus == Cluster::ConsensusScope::Fleet && row.raftListener == Cluster::RaftListenerState::Closed)
            record.fleet = Cluster::FleetMembership { .clusterId = "fleet-c",
                                                      .roster = Cluster::EncodeRoster(ThreeVoters()),
                                                      .createdAtUnixSeconds = 0,
                                                      .admittedBy = {} };
        auto cfg = NodeConfig {};
        cfg.schedulers = { "127.0.0.1:6674" };
        cfg.raftListen.clear();
        cfg.nodeListen = "0.0.0.0:6674";
        cfg.clusterDir = scratch.Path();
        REQUIRE(ApplyFormation(cfg, record, {}).has_value());

        auto const joined = expected->joined;
        CHECK((row.members == Cluster::FleetReach::Beyond) == joined);
        CHECK(AdmitsRemotePeers(cfg, RosterPresence::Absent) == joined);
        auto const roster = NodeRoster::Build(cfg, RosterClock(), nullptr);
        if (RunsConsensus(cfg))
        {
            CHECK(row.raftListener == Cluster::RaftListenerState::Closed);
            REQUIRE(roster.has_value());
            CHECK(Testing::Unwrap(roster)->Lease() != nullptr);
        }
        else if (joined)
        {
            REQUIRE_FALSE(roster.has_value());
            CHECK(roster.error().reason == RosterlessWorkerRefusal);
        }
        else
        {
            REQUIRE(roster.has_value());
            CHECK(Testing::Unwrap(roster)->Lease() == nullptr);
        }
    }
}
