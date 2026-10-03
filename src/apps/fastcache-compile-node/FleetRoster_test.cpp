// SPDX-License-Identifier: Apache-2.0
//
// A worker admitted as a LEARNER, verifying its fleet's grants against the state it applied, across
// a voter being forgotten (#178). Driven through `FleetHarness`, because the property is a SEQUENCE
// across machines: a grant minted by a voter, the cluster forgetting that voter, and the learner
// applying the change.
//
// The case names the rule whose removal turns it red, and has a GREEN control beside it under the
// same break -- the harness rule: a case that cannot fail for the reason it exists is not evidence.
#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <string_view>

#include <tests/FleetHarness.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace std::chrono_literals;
using FastCache::Testing::FleetHarness;
using FastCache::Testing::Unwrap;

namespace
{

/// The two voters the case runs, by the endpoint that is also each one's member id.
inline std::string const SchedA = "sched-a:6676";
inline std::string const SchedB = "sched-b:6676";

/// The learner under test: where it answers compiles, which every grant for it names.
inline std::string const Laptop = "laptop:6675";

/// The toolchain every grant here names.
inline constexpr std::string_view Toolchain = "gcc-14";

} // namespace

TEST_CASE("A worker admitted as a learner accepts a grant and refuses one from a forgotten voter", "[fleet][roster]")
{
    // The learner applies its fleet's state (here: A's), and that state is its whole roster. B is a
    // voter, then the cluster FORGETS it -- the record goes and its key is revoked in one entry --
    // and B, still holding its key, still mints. The learner names B's grant the removed machine's
    // once it has applied the forget: the state it applied is current, and nothing about it lapses.
    //
    // RED when the learner does not re-apply its state (the harness's per-step adopt skipped): B's
    // second grant is accepted. GREEN under that same break: A's grant, the control.
    FleetHarness fleet;
    fleet.AddScheduler(SchedA);
    fleet.AddScheduler(SchedB);
    auto const formed = FleetHarness::StateOf({ SchedA, SchedB }, {}, 1);
    fleet.SetClusterStateAt(SchedA, formed);
    fleet.SetClusterStateAt(SchedB, formed);
    fleet.ElectLeader(SchedB);
    fleet.RegisterWorker(SchedB, Laptop, Toolchain, 4);

    FleetHarness::LearnerWorker laptop { fleet, Laptop, SchedA };
    REQUIRE(laptop.Roster().voters == 2);

    // The premise: while B is a voter, its grant is honoured.
    REQUIRE_FALSE(laptop.Check(fleet.GrantAt(SchedB, Toolchain, "obj-before"), Toolchain).has_value());
    auto const later = fleet.GrantAt(SchedB, Toolchain, "obj-after");

    fleet.SetClusterStateAt(SchedA, FleetHarness::StateOf({ SchedA, SchedB }, { SchedB }, 2));
    fleet.Step(1s);

    // Named as the removed machine, and counted on its own row by the worker's endpoint.
    auto const refused = laptop.Check(later, Toolchain);
    REQUIRE(refused.has_value());
    CHECK(Unwrap(refused).reason == Distributed::LeaseRefusalReason::SignerRevoked);
    CHECK(Distributed::DescribeLeaseRefusal(Unwrap(refused).reason).workerCounter
          == IMetricsSink::Counter::WorkerJobsRefusedLeaseSignerRevoked);

    // The control: a voter in good standing signs exactly as before.
    fleet.ElectLeader(SchedA);
    fleet.RegisterWorker(SchedA, Laptop, Toolchain, 4);
    CHECK_FALSE(laptop.Check(fleet.GrantAt(SchedA, Toolchain, "obj-control"), Toolchain).has_value());
}
