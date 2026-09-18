// SPDX-License-Identifier: Apache-2.0
//
// A worker that runs no consensus, trusting the fleet's rosters across leadership moving and a
// scheduler the cluster revoked (#178). Driven through `FleetHarness`, because every property
// here is a SEQUENCE across machines: a roster certified by one set of voters, a grant minted
// by a scheduler that has since been removed, a round that reaches a deposed leader first.
//
// Each case names the rule whose removal turns it red, and each has a GREEN control beside it
// under the same break -- the harness rule: a case that cannot fail for the reason it exists
// is not evidence.
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <tests/FleetHarness.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace std::chrono_literals;
using FastCache::Testing::FleetHarness;
using FastCache::Testing::Unwrap;

namespace
{

/// The three voters every case here runs, by the endpoint that is also each one's member id.
inline std::string const SchedA = "sched-a:6676";
inline std::string const SchedB = "sched-b:6676";
inline std::string const SchedC = "sched-c:6676";

/// The worker under test.
inline std::string const Worker = "worker-1:6675";

/// The toolchain every grant here names.
inline constexpr std::string_view Toolchain = "gcc-14";

/// A fleet of three voters, all of them holding @p state, led by C, with the worker registered
/// there so C can grant -- a registration is a leader's verb, so it follows the election.
/// @param fleet The harness.
/// @param state What every voter applied.
void FormFleet(FleetHarness& fleet, Cluster::ClusterState const& state)
{
    for (auto const& scheduler: { SchedA, SchedB, SchedC })
    {
        fleet.AddScheduler(scheduler);
        fleet.SetClusterStateAt(scheduler, state);
    }
    fleet.ElectLeader(SchedC);
    fleet.RegisterWorker(SchedC, Worker, Toolchain, 4);
}

/// What the worker holds, as a roster version -- or nothing.
/// @param worker The worker.
/// @return The version it holds.
[[nodiscard]] std::optional<std::uint64_t> HeldVersion(FleetHarness::RosterWorker const& worker)
{
    auto const summary = worker.Roster();
    return summary.has_value() ? std::optional { summary->version } : std::nullopt;
}

} // namespace

TEST_CASE("A roster endorsed only by the voter the cluster is revoking is refused", "[fleet][roster]")
{
    // The attack #178's design exists for. Scheduler C led, and the cluster revoked it -- but
    // the worker has not heard, and C still holds its key and still answers. It withholds the
    // roster that revokes it and serves one of its own instead, keeping itself as the only
    // voter, endorsed by itself. A roster certified by the leader alone would be adopted, and
    // from then on C's grants would verify at this worker for good. A strict MAJORITY of the
    // voters the worker already trusts is what refuses it: C is one of three.
    //
    // RED when the majority rule is removed (`needed` becomes 1 in `CertifyRoster`): the worker
    // adopts C's roster. GREEN under that same break: the control, the cluster's next roster
    // endorsed by A and B, relayed by the same scheduler, is adopted either way.
    FleetHarness fleet;
    auto const formed = FleetHarness::StateOf({ SchedA, SchedB, SchedC }, {}, 1);
    FormFleet(fleet, formed);
    fleet.EndorseAt(SchedC, SchedA, formed);
    fleet.EndorseAt(SchedC, SchedB, formed);

    FleetHarness::RosterWorker worker { fleet, Worker, { SchedA, SchedB, SchedC }, { SchedC } };
    REQUIRE(worker.Announce());
    REQUIRE(HeldVersion(worker) == std::optional<std::uint64_t> { 1 });

    SECTION("the revoked voter's own roster, endorsed by it alone")
    {
        auto const own = FleetHarness::StateOf({ SchedC }, {}, 7);
        fleet.SetClusterStateAt(SchedC, own);
        fleet.EndorseAt(SchedC, SchedC, own);
        auto const refusedBefore = fleet.Metrics().Read(IMetricsSink::Counter::WorkerRostersRefusedUncertified);

        REQUIRE(worker.Announce());
        CHECK(HeldVersion(worker) == std::optional<std::uint64_t> { 1 });
        CHECK(fleet.Metrics().Read(IMetricsSink::Counter::WorkerRostersRefusedUncertified) == refusedBefore + 1);
    }

    SECTION("control: the cluster's next roster, endorsed by a majority")
    {
        auto const next = FleetHarness::StateOf({ SchedA, SchedB, SchedC }, { SchedC }, 2);
        fleet.SetClusterStateAt(SchedC, next);
        fleet.EndorseAt(SchedC, SchedA, next);
        fleet.EndorseAt(SchedC, SchedB, next);

        REQUIRE(worker.Announce());
        CHECK(HeldVersion(worker) == std::optional<std::uint64_t> { 2 });
    }
}

TEST_CASE("A worker cut off with a withholding ex-leader refuses its grants once the roster lapses", "[fleet][roster]")
{
    // The bound the endorsement lifetime buys. C led, was revoked while the worker could reach
    // nobody else, and withholds every newer roster -- so the worker's roster is never
    // re-certified. C still holds its key, and in the roster the worker holds C is a voter, so
    // its grants verify. For one lifetime and the skew slack that is accepted; past it, every
    // grant is refused `roster-expired`, and C can mint nothing anybody honours.
    //
    // RED when the lapse is not checked (`SignedLeaseValidator` ignoring `RosterStanding`): the
    // grant past the window is honoured. GREEN under that break: the same grant inside the
    // window, which is the control.
    FleetHarness fleet;
    auto const formed = FleetHarness::StateOf({ SchedA, SchedB, SchedC }, {}, 1);
    FormFleet(fleet, formed);
    fleet.EndorseAt(SchedC, SchedA, formed);
    fleet.EndorseAt(SchedC, SchedB, formed);

    FleetHarness::RosterWorker worker { fleet, Worker, { SchedA, SchedB, SchedC }, { SchedC } };
    REQUIRE(worker.Announce());
    REQUIRE(HeldVersion(worker) == std::optional<std::uint64_t> { 1 });

    // Cut off: A and B are unreachable from the worker, and C -- deposed by a change the worker
    // never hears -- keeps answering as the leader it thinks it is. Nobody endorses C's roster
    // any more, so its announcement replies carry no certified roster.
    fleet.SetUnreachable(SchedA, true);
    fleet.SetUnreachable(SchedB, true);

    // The worker keeps reaching C the whole time, so it stays on C's registry: a registration
    // lapses long before a roster does, and re-registering is what its heartbeat round does.
    auto const stepKeepingRegistered = [&fleet](std::chrono::milliseconds by) {
        fleet.Step(by);
        fleet.RegisterWorker(SchedC, Worker, Toolchain, 4);
    };

    SECTION("control: inside the window, C's grant is honoured")
    {
        stepKeepingRegistered(Cluster::RosterEndorsementLifetime - 1min);
        (void) worker.Announce();
        CHECK_FALSE(worker.Check(fleet.GrantAt(SchedC, Toolchain, "obj-early"), Toolchain).has_value());
    }

    SECTION("past the lifetime and the slack, every grant is refused roster-expired")
    {
        stepKeepingRegistered(Cluster::RosterEndorsementLifetime + Distributed::LeaseTokenClockSkewSlack + 1s);
        (void) worker.Announce();
        auto const refusal = worker.Check(fleet.GrantAt(SchedC, Toolchain, "obj-late"), Toolchain);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).reason == Distributed::LeaseRefusalReason::RosterExpired);
        CHECK(Distributed::DescribeLeaseRefusal(Unwrap(refusal).reason).code == CompileCacheWire::ErrorCode::RosterExpired);
    }
}

TEST_CASE("A worker whose remembered leader was deposed adopts the new leader's roster in the same round", "[fleet][roster]")
{
    // The recovery half: the worker's round reaches the deposed leader first, which answers
    // `NotLeader` naming A; the round follows it to A in the SAME round -- `DialAndAnnounce`'s
    // rule, reached through production's presence announcement -- and A's reply carries the
    // roster A and B certified since, the one revoking C. One round, adopted, and C's grants
    // stop verifying at once.
    //
    // RED when the presence round stops following a redirect (the announcement dropping the
    // leader it was told): the round ends at C and the new roster is not adopted. GREEN under
    // that break: the control, a worker whose first scheduler IS the leader.
    FleetHarness fleet;
    auto const formed = FleetHarness::StateOf({ SchedA, SchedB, SchedC }, {}, 1);
    FormFleet(fleet, formed);
    fleet.EndorseAt(SchedC, SchedA, formed);
    fleet.EndorseAt(SchedC, SchedB, formed);

    // Minted while C led, and honoured then: C was a voter in good standing.
    auto const fromC = fleet.GrantAt(SchedC, Toolchain, "obj-c");

    // The cluster moves on: A leads, C is revoked, and A and B endorse the roster saying so.
    auto const next = FleetHarness::StateOf({ SchedA, SchedB, SchedC }, { SchedC }, 2);
    auto const moveOn = [&fleet, &next] {
        fleet.ElectLeader(SchedA);
        fleet.SetClusterStateAt(SchedA, next);
        fleet.SetClusterStateAt(SchedB, next);
        fleet.EndorseAt(SchedA, SchedA, next);
        fleet.EndorseAt(SchedA, SchedB, next);
    };

    SECTION("reaching the deposed leader first")
    {
        FleetHarness::RosterWorker worker { fleet, Worker, { SchedA, SchedB, SchedC }, { SchedC } };
        REQUIRE(worker.Announce());
        REQUIRE(HeldVersion(worker) == std::optional<std::uint64_t> { 1 });

        moveOn();
        auto const before = fleet.Calls().size();
        REQUIRE(worker.Announce());

        // One round, two schedulers asked, in order: C redirected, A answered.
        REQUIRE(fleet.Calls().size() == before + 2);
        CHECK(fleet.Calls()[before].endpoint == SchedC);
        CHECK(fleet.Calls()[before + 1].endpoint == SchedA);
        CHECK(HeldVersion(worker) == std::optional<std::uint64_t> { 2 });

        // And C's grant is refused by name: the roster the worker now holds revoked it.
        auto const refusal = worker.Check(fromC, Toolchain);
        REQUIRE(refusal.has_value());
        CHECK(Unwrap(refusal).reason == Distributed::LeaseRefusalReason::SignerRevoked);
    }

    SECTION("control: a worker whose first scheduler is the leader")
    {
        moveOn();
        FleetHarness::RosterWorker worker { fleet, Worker, { SchedA, SchedB, SchedC }, { SchedA } };
        REQUIRE(worker.Announce());
        CHECK(HeldVersion(worker) == std::optional<std::uint64_t> { 2 });
    }
}
