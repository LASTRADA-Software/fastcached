// SPDX-License-Identifier: Apache-2.0
//
// What a machine with no worker is to the fleet that can see it
// ([#1440](https://github.com/LASTRADA-Software/fastcached/issues/1440)).
//
// These are the ticket's own acceptance cases, and they need two machines to say anything, so
// they live here over `FleetHarness` rather than beside `WorkerRegistry`: the property is that
// a scheduler-only machine appears on a page that a DIFFERENT machine's worker also appears on,
// and that its history survives leadership moving to somewhere else.
//
// **Every case here is impossible to write against worker registration**, which is the point
// rather than an aside. A node with `--slots=0` registers nothing, so before `NodeAnnounce`
// existed there was no verb through which any of this could have been arranged -- the fixture
// itself is the evidence that the old shape could not express the fact.
#include <FastCache/Distributed/FleetHistory.hpp>
#include <FastCache/Distributed/FleetView.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <tests/FleetHarness.hpp>
#include <tests/FleetHistoryFakes.hpp>

using namespace FastCache;
using namespace FastCache::Testing;

namespace
{

constexpr std::string_view SchedulerA = "sched-a:6676";
constexpr std::string_view SchedulerB = "sched-b:6676";

/// The 0xFC endpoints of those same two boxes, which is what a machine announces itself under.
///
/// Deliberately a different port from the scheduler endpoint on the same host: a row filed
/// under the wrong one of the two would still look plausible, so the case can tell them apart.
constexpr std::string_view MachineA = "sched-a:6674";
constexpr std::string_view MachineB = "sched-b:6674";

constexpr std::string_view WorkerMachine = "builder-1:6674";
constexpr std::string_view Toolchain = "gcc-14-x86_64";

/// The report for @p endpoint, or nothing.
/// @param reports What a scheduler would draw.
/// @param endpoint Which machine.
/// @return A pointer into @p reports, or null.
[[nodiscard]] Distributed::NodeReport const* RowFor(std::vector<Distributed::NodeReport> const& reports,
                                                    std::string_view endpoint)
{
    auto const found = std::ranges::find_if(reports, [endpoint](auto const& r) { return r.endpoint == endpoint; });
    return found == reports.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("A scheduler-only machine and a worker machine are both Machines rows", "[node][fleet][presence]")
{
    FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.ElectLeader(SchedulerA);

    // The leader announces ITSELF -- it runs a scheduler and no worker, which is the machine
    // this ticket is about -- and a second machine both announces and registers a worker,
    // which is what an ordinary fleet member does.
    fleet.AnnounceMachine(SchedulerA, MachineA);
    fleet.AnnounceMachine(SchedulerA, WorkerMachine);
    fleet.RegisterWorker(SchedulerA, WorkerMachine, Toolchain, 4);

    auto const reports = fleet.MachinesAt(SchedulerA);

    // BOTH, and the count matters: a fold that let a presence row shadow a worker row, or a
    // worker row swallow a presence row, would leave exactly one here and look like a fleet
    // with one machine in it.
    REQUIRE(reports.size() == 2);

    auto const* const leaderRow = RowFor(reports, MachineA);
    auto const* const workerRow = RowFor(reports, WorkerMachine);
    REQUIRE(leaderRow != nullptr);
    REQUIRE(workerRow != nullptr);

    // **Absent, never zero.** A registered worker always offers at least one slot, because
    // `--slots=0` means there is no worker at all -- so a zero in this field could only ever
    // mean *a machine with no worker* said in the vocabulary of *a worker offering nothing*,
    // and the page would draw it as a real ceiling of zero.
    CHECK_FALSE(leaderRow->registeredSlots.has_value());
    CHECK(leaderRow->fingerprints.empty());

    // The worker machine is untouched by any of this: a presence row is added beside worker
    // rows and never edits one, so a fleet that runs workers renders exactly as it did.
    REQUIRE(workerRow->registeredSlots.has_value());
    CHECK(*workerRow->registeredSlots == 4);
    CHECK(workerRow->fingerprints.size() == 1);
}

TEST_CASE("Fleet totals skip a machine that can take no work", "[node][fleet][presence]")
{
    // Not the same assertion as the absent cell above, and the difference is the one that
    // reaches an operator: a zero would be arithmetically invisible in `registered` and would
    // still put a machine that can take no work into the denominator of *how busy is this
    // fleet*. So the totals are asserted against the worker machine ALONE while the page shows
    // two machines -- a state no version of this that counted zeros could produce.
    FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.ElectLeader(SchedulerA);

    fleet.AnnounceMachine(SchedulerA, MachineA);
    fleet.AnnounceMachine(SchedulerA, WorkerMachine);
    fleet.RegisterWorker(SchedulerA, WorkerMachine, Toolchain, 4);

    Distributed::FleetSnapshot snapshot;
    snapshot.role = Distributed::SchedulerRole::Leader;
    snapshot.nodes = fleet.MachinesAt(SchedulerA);

    auto const totals = Distributed::TotalsFor(snapshot);

    REQUIRE(snapshot.nodes.size() == 2);
    CHECK(totals.registered == 4);
}

TEST_CASE("A machine hands its history to whoever leads now", "[node][fleet][presence][fleethistory]")
{
    // The ticket's second acceptance case. A scheduler-only machine is exactly the one whose
    // series an election is about to orphan: it has no worker heartbeat to carry the buckets,
    // so before `NodeAnnounce` its history reached a new leader through nothing at all.
    FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.AddScheduler(std::string { SchedulerB });

    PlacedWallClock wall;
    Distributed::FleetNodeHistories atA { wall };
    Distributed::FleetNodeHistories atB { wall };
    fleet.SetHistorySinkAt(SchedulerA, &atA);
    fleet.SetHistorySinkAt(SchedulerB, &atB);

    // One closed bucket, built here rather than sampled: the subject is the RECEIVING side, so
    // the bucket is this case's input rather than a stand-in for a mechanism. `FleetSampler`
    // producing one is `NodePresenceTier_test`'s business.
    //
    // **`present` is spelled out because it defaults to FALSE, and the first version of this
    // case did not spell it** -- so `AcceptHistory` skipped every bucket as absent, and the
    // case still reached its `Count() == 2` assertion, because an entry is created for a
    // machine that reports whether or not any of its readings land. A count of machines is not
    // a count of accepted buckets, and only `HighWaterFor` below tells the two apart. Had this
    // case asserted the count alone it would have been green over a handover that never
    // happened -- which is the exact defect it was written to catch.
    std::vector<Distributed::FleetBucket> const handover { Distributed::FleetBucket {
        .startMillis = 60'000, .sampleMillis = 120'000, .present = true } };

    fleet.ElectLeader(SchedulerA);
    fleet.AnnounceMachine(SchedulerA, MachineA, handover);
    fleet.AnnounceMachine(SchedulerA, MachineB, handover);

    // The premise, asserted rather than assumed: while A leads, A is where the buckets are.
    REQUIRE(atA.Count() == 2);
    REQUIRE(atA.HighWaterFor(MachineA) == 60'000);
    REQUIRE(atB.Count() == 0);

    // Leadership moves. Nothing replicates a history store, so the only route by which the new
    // leader can learn what the old one was handed is the machines announcing themselves again.
    fleet.ElectLeader(SchedulerB);
    fleet.AnnounceMachine(SchedulerB, MachineA, handover);
    fleet.AnnounceMachine(SchedulerB, MachineB, handover);

    // **Filed under the MACHINE, never under a worker id**, which is what makes this work for a
    // machine that has no worker to have an id.
    CHECK(atB.Count() == 2);
    CHECK(atB.HighWaterFor(MachineA) == 60'000);
    CHECK(atB.HighWaterFor(MachineB) == 60'000);
}

TEST_CASE("A worker-only fleet is unchanged by any of this", "[node][fleet][presence]")
{
    // **The control, and it is what makes the three cases above mean anything.** The neuter
    // this file exists to survive is routing the announcement through worker registration --
    // and under that neuter a fleet whose machines all run workers goes on passing, because
    // every one of them registers. So a suite without this case cannot distinguish "presence
    // works" from "registration happens to cover every machine the other cases used".
    FleetHarness fleet;
    fleet.AddScheduler(std::string { SchedulerA });
    fleet.ElectLeader(SchedulerA);

    fleet.RegisterWorker(SchedulerA, WorkerMachine, Toolchain, 4);

    auto const reports = fleet.MachinesAt(SchedulerA);
    REQUIRE(reports.size() == 1);

    auto const* const row = RowFor(reports, WorkerMachine);
    REQUIRE(row != nullptr);
    REQUIRE(row->registeredSlots.has_value());
    CHECK(*row->registeredSlots == 4);
    CHECK(row->fingerprints.size() == 1);
}
