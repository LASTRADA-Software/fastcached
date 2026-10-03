// SPDX-License-Identifier: Apache-2.0
#include "ForeignFleetWatch.hpp"
#include "NodeConditions.hpp"

#include <FastCache/Cluster/PeerDirectory.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>

#include <core/platform/Clock.hpp>
#include <tests/FormationFakes.hpp>
#include <tests/NodeConditionFakes.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;
using namespace std::chrono_literals;

namespace Wire = CompileCacheWire;

TEST_CASE("Two established fleets that see each other raise foreign-fleet-visible naming both",
          "[node][formation][conditions]")
{
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, clock, conditions, next };
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Clear);

    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-lab", .state = FleetState::Established }));
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);
    CHECK(Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible).contains("c-office"));
    CHECK(Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible).contains("c-lab"));
    CHECK(next.proven.size() == 1); // forwarded whatever it decided

    // Still remembered one step short of the bound, so a lossy segment does not flap the row.
    clock.advance(ForeignFleetWatch::ForgetAfter - 1s);
    watch.Tick();
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);

    clock.advance(2s);
    watch.Tick();
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Clear);
}

TEST_CASE("Another fleet's cluster id that is not text is rendered escaped", "[node][formation][conditions]")
{
    // A cluster id is compared and never filtered, so another fleet's arrives in whatever bytes it
    // was named in -- and the watch puts it in the row's detail, which every surface renders. It is
    // text there because `NodeConditions` escapes each byte that is not UTF-8.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, clock, conditions, next };

    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-\xFF"
                                                                          "lab",
                                                             .state = FleetState::Established,
                                                             .nodeId = "n-lab" }));
    REQUIRE(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);
    auto const detail = Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible);
    CHECK(detail.contains("c-\\xFFlab"));
    CHECK_FALSE(detail.contains("\xFF"));
}

TEST_CASE("A solitary node seeing an established fleet raises nothing", "[node][formation][conditions]")
{
    // It yields instead, which is formation's business: the fleet still goes on to `next`.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-laptop", .state = FleetState::Solitary } };
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, clock, conditions, next };

    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-office", .state = FleetState::Established }));
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Clear);
    CHECK(next.proven.size() == 1);
}

TEST_CASE("foreign-fleet-visible follows the encounter decision, not the two states", "[node][formation][conditions]")
{
    // Two clusters that share a member are one fleet split, and heal by the tiebreak even when
    // both are established: what is foreign is the encounter table's answer, asked afresh at every
    // decision. Here this node's summary changes under the watch -- it yielded and is solitary --
    // and a fleet it had recorded as foreign stops being one at the next tick, not five minutes on.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, clock, conditions, next };

    auto const lab = Testing::ProvenBeacon(FleetSummary { .clusterId = "c-lab", .state = FleetState::Established });
    watch.OnFleetProven(lab);
    REQUIRE(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);

    self.Set(FleetSummary { .clusterId = "c-office", .state = FleetState::Solitary });
    watch.Tick();
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Clear);

    // And a proof that is no longer foreign clears at once, on arrival.
    self.Set(FleetSummary { .clusterId = "c-office", .state = FleetState::Established });
    watch.OnFleetProven(lab);
    REQUIRE(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);
    self.Set(FleetSummary { .clusterId = "c-office", .state = FleetState::Solitary });
    watch.OnFleetProven(lab);
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Clear);

    // Both established and still not foreign: a proof claiming this node's OWN cluster id is the
    // same fleet by the table's first rule, whatever the states say. A watch testing the two
    // states would raise here.
    self.Set(FleetSummary { .clusterId = "c-office", .state = FleetState::Established });
    watch.OnFleetProven(
        Testing::ProvenBeacon(FleetSummary { .clusterId = "c-office", .state = FleetState::Established, .nodeId = "n2" }));
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Clear);
    CHECK(next.proven.size() == 4);
}

TEST_CASE("foreign-fleet-visible names every fleet it sees, and each is forgotten on its own clock",
          "[node][formation][conditions]")
{
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, clock, conditions, next };

    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-lab", .state = FleetState::Established }));
    clock.advance(1min);
    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-qa", .state = FleetState::Established }));
    auto const both = Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible);
    CHECK(both.contains("c-office"));
    CHECK(both.contains("c-lab"));
    CHECK(both.contains("c-qa"));

    // c-lab lapses first; c-qa is still proven, so the row stays and names only it.
    clock.advance(ForeignFleetWatch::ForgetAfter - 30s);
    watch.Tick();
    REQUIRE(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);
    auto const one = Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible);
    CHECK_FALSE(one.contains("c-lab"));
    CHECK(one.contains("c-qa"));
}

TEST_CASE("foreign-fleet-visible remembers at most MaxForeignFleets fleets", "[node][formation][conditions]")
{
    // Every proven fleet is somebody's key over this node's nonce, and a key costs nothing: the
    // watch is bounded like the discovery table in front of it, and never evicts.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, clock, conditions, next };

    for (auto const index: std::views::iota(std::size_t { 0 }, Cluster::MaxForeignFleets + 1))
        watch.OnFleetProven(Testing::ProvenBeacon(
            FleetSummary { .clusterId = std::format("c-{:02}", index), .state = FleetState::Established }));

    auto const detail = Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible);
    CHECK(detail.contains(std::format("{} other established fleets", Cluster::MaxForeignFleets)));
    CHECK_FALSE(detail.contains(std::format("c-{:02}", Cluster::MaxForeignFleets)));
    CHECK(next.proven.size() == Cluster::MaxForeignFleets + 1);
}
