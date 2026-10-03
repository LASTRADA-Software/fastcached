// SPDX-License-Identifier: Apache-2.0
#include "ForeignFleetWatch.hpp"
#include "NodeConditions.hpp"

#include <FastCache/Cluster/DiscoveryService.hpp>
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
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::NotEvaluated);

    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-lab", .state = FleetState::Established }));
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);
    CHECK(Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible).contains("c-office"));
    CHECK(Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible).contains("c-lab"));
    CHECK(next.proven.size() == 1); // forwarded whatever it decided

    // Still remembered one step short of the bound, so a lossy segment does not flap the row.
    clock.advance(ForeignFleetWatch::ForgetAfter - 1s);
    watch.Tick();
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);

    // Forgotten -- and since nothing else was heard either, the row cannot say "nobody" now: an
    // empty segment and a firewalled one look alike.
    clock.advance(2s);
    watch.Tick();
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::NotEvaluated);
}

TEST_CASE("Another fleet's cluster id that is not text is rendered escaped", "[node][formation][conditions]")
{
    // A cluster id is compared and never filtered, so another fleet's arrives in whatever bytes it
    // was named in -- and the watch puts it in the row's detail, which every surface renders. It is
    // text there because `NodeConditions` escapes each byte that is not UTF-8.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };

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
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };

    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-office", .state = FleetState::Established }));
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Clear);
    CHECK(next.proven.size() == 1);
}

TEST_CASE("foreign-fleet-visible follows the encounter decision, not the two states", "[node][formation][conditions]")
{
    // Two clusters proven to be one fleet split heal by the tiebreak even when both are
    // established: what is foreign is the encounter table's answer, asked afresh at every
    // decision. Here this node's summary changes under the watch -- it yielded and is solitary --
    // and a fleet it had recorded as foreign stops being one at the next tick, not five minutes on.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };

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
    // Every other fleet's proof went on to formation; this node's own fleet's is heard here and goes
    // no further -- formation has nothing to do with its own.
    CHECK(next.proven.size() == 3);
}

TEST_CASE("foreign-fleet-visible names every fleet it sees, and each is forgotten on its own clock",
          "[node][formation][conditions]")
{
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };

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
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };

    for (auto const index: std::views::iota(std::size_t { 0 }, Cluster::MaxForeignFleets + 1))
        watch.OnFleetProven(Testing::ProvenBeacon(
            FleetSummary { .clusterId = std::format("c-{:02}", index), .state = FleetState::Established }));

    auto const detail = Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible);
    CHECK(detail.contains(std::format("{} other established fleets", Cluster::MaxForeignFleets)));
    CHECK_FALSE(detail.contains(std::format("c-{:02}", Cluster::MaxForeignFleets)));
    CHECK(next.proven.size() == Cluster::MaxForeignFleets + 1);
}

TEST_CASE("A fleet proven to be this one split is healing and never foreign, and says who yields",
          "[node][formation][conditions]")
{
    // The evidence turns what would be foreign into the tiebreak's, so the row the operator reads is
    // the healing one: naming the other fleet, which side yields, why, and on which evidence.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary {
        .clusterId = "c-office", .state = FleetState::Established, .createdAtUnixSeconds = 100 } };
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };
    CHECK(conditions.StateOf(NodeCondition::FleetSplitHealing) == Wire::ConditionState::NotEvaluated);

    evidence.Set("c-lab",
                 Cluster::SplitReading {
                     .evidence = Cluster::SplitEvidence::TheirSpeakerIsOurVoter, .claimedMember = {}, .witness = "n-desk" });
    auto const lab = Testing::ProvenBeacon(
        FleetSummary { .clusterId = "c-lab", .state = FleetState::Established, .createdAtUnixSeconds = 900 });
    watch.OnFleetProven(lab);
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Clear);
    REQUIRE(conditions.StateOf(NodeCondition::FleetSplitHealing) == Wire::ConditionState::Raised);
    auto const detail = Testing::DetailOf(conditions, NodeCondition::FleetSplitHealing);
    CHECK(detail.contains("c-office"));
    CHECK(detail.contains("c-lab"));
    CHECK(detail.contains("it yields to this fleet")); // this one is older
    CHECK(detail.contains("the older fleet stays"));
    CHECK(detail.contains(
        Cluster::SplitEvidenceNames[static_cast<std::size_t>(Cluster::SplitEvidence::TheirSpeakerIsOurVoter)].name));
    CHECK(next.proven.size() == 1);

    // Losing the evidence makes it foreign again at the next tick -- the row follows the reading.
    evidence.Set("c-lab", Cluster::SplitReading {});
    watch.Tick();
    CHECK(conditions.StateOf(NodeCondition::FleetSplitHealing) == Wire::ConditionState::Clear);
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Raised);

    // And a healing split clears a few minutes after the yielding fleet stops being heard.
    evidence.Set("c-lab", Cluster::SplitReading { .evidence = Cluster::SplitEvidence::WeAskedAndTheyListUs });
    watch.OnFleetProven(lab);
    REQUIRE(conditions.StateOf(NodeCondition::FleetSplitHealing) == Wire::ConditionState::Raised);
    // Heard from its own fleet meanwhile, so an empty row is the truth and reads clear.
    clock.advance(ForeignFleetWatch::ForgetAfter + 1s);
    watch.OnFleetProven(Testing::ProvenBeacon(
        FleetSummary { .clusterId = "c-office", .state = FleetState::Established, .createdAtUnixSeconds = 100 }));
    watch.Tick();
    CHECK(conditions.StateOf(NodeCondition::FleetSplitHealing) == Wire::ConditionState::Clear);
}

TEST_CASE("A split only an operator heals is raised as healing and says an operator decides and on whose word",
          "[node][formation][conditions][security]")
{
    // A memo's evidence and a learner's are first-use trust: real splits often enough to tell, never
    // enough to move a fleet. The row is the healing one -- not foreign, since a key this fleet held
    // does tie the two together -- and it names no side as yielding, because none will.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary {
        .clusterId = "c-office", .state = FleetState::Established, .createdAtUnixSeconds = 900 } };
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };

    for (auto const kind: { Cluster::SplitEvidence::WeAskedAndTheyListUs, Cluster::SplitEvidence::TheirSpeakerIsOurLearner })
    {
        INFO(Cluster::SplitEvidenceNames[static_cast<std::size_t>(kind)].name);
        evidence.Set("c-mint", Cluster::SplitReading { .evidence = kind, .claimedMember = {}, .witness = "n-laptop" });
        // Older, so the tiebreak -- were it consulted -- would make this fleet yield.
        watch.OnFleetProven(Testing::ProvenBeacon(
            FleetSummary { .clusterId = "c-mint", .state = FleetState::Established, .createdAtUnixSeconds = 1 }));
        CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::Clear);
        REQUIRE(conditions.StateOf(NodeCondition::FleetSplitHealing) == Wire::ConditionState::Raised);
        auto const detail = Testing::DetailOf(conditions, NodeCondition::FleetSplitHealing);
        CHECK(detail.contains("c-mint: an operator decides"));
        CHECK(detail.contains("n-laptop's word"));
        CHECK(detail.contains(Cluster::SplitEvidenceNames[static_cast<std::size_t>(kind)].name));
        CHECK_FALSE(detail.contains("yields"));
    }
}

TEST_CASE("A foreign fleet's entry says whether it claims a machine of this one, unproven, or shares none",
          "[node][formation][conditions]")
{
    // The two cases an operator reads differently: a stranger, and a fleet naming this one's machines
    // that no key here vouches for -- a look-alike somebody minted, or a member list cut short.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };

    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-lab", .state = FleetState::Established }));
    CHECK(Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible).contains("c-lab: no machine in common"));

    evidence.Set("c-evil", Cluster::SplitReading { .evidence = Cluster::SplitEvidence::None, .claimedMember = "n-office" });
    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-evil", .state = FleetState::Established }));
    auto const both = Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible);
    CHECK(both.contains("c-lab: no machine in common"));
    CHECK(both.contains("c-evil: claims to record n-office, not verified by any key this fleet holds"));
    CHECK(conditions.StateOf(NodeCondition::FleetSplitHealing) == Wire::ConditionState::Clear);
}

TEST_CASE("A foreign fleet whose member list was cut is never said to share no machine, only none it listed",
          "[node][formation][conditions]")
{
    // "No machine in common" is a statement about the whole list. A fleet recording more machines than
    // its datagram carried may hold one of this fleet's among the rest, so the entry names the cut.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };

    auto cut = FleetSummary { .clusterId = "c-big", .state = FleetState::Established };
    for (auto const index: std::views::iota(std::size_t { 0 }, Wire::MaxFleetSummaryMembers))
        cut.members.push_back(std::format("n-big-{}", index));
    cut.memberTotal = 40;
    watch.OnFleetProven(Testing::ProvenBeacon(cut));

    auto const detail = Testing::DetailOf(conditions, NodeCondition::ForeignFleetVisible);
    CHECK(detail.contains(
        std::format("c-big: no machine in common among the {} of 40 it lists, the rest cut", Wire::MaxFleetSummaryMembers)));
    CHECK_FALSE(detail.contains("c-big: no machine in common;"));
}

TEST_CASE("An empty watch reads not-evaluated until it has listened a beacon interval, and clear only once it has heard",
          "[node][formation][conditions]")
{
    // `clear` says no other fleet can see this one, which is only known by having LISTENED: one beacon
    // interval, by which every machine on the segment has beaconed, and a reply heard since.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };
    for (auto const condition: ForeignFleetWatch::WatchedConditions)
    {
        CHECK(conditions.StateOf(condition) == Wire::ConditionState::NotEvaluated);
        CHECK(Testing::DetailOf(conditions, condition).contains("decided once it has"));
    }

    // Still listening one second short of the interval.
    clock.advance(Cluster::DefaultBeaconInterval - 1s);
    watch.Tick();
    CHECK(conditions.StateOf(NodeCondition::ForeignFleetVisible) == Wire::ConditionState::NotEvaluated);

    // A reply from its own fleet: heard, and nobody foreign -- clear, the truth now.
    watch.OnFleetProven(Testing::ProvenBeacon(FleetSummary { .clusterId = "c-office", .state = FleetState::Established }));
    for (auto const condition: ForeignFleetWatch::WatchedConditions)
        CHECK(conditions.StateOf(condition) == Wire::ConditionState::Clear);

    // Replies stop: once nothing has been heard for `ForgetAfter`, it says so at the next tick.
    clock.advance(ForeignFleetWatch::ForgetAfter);
    watch.Tick();
    for (auto const condition: ForeignFleetWatch::WatchedConditions)
    {
        CHECK(conditions.StateOf(condition) == Wire::ConditionState::NotEvaluated);
        CHECK(Testing::DetailOf(conditions, condition).contains("no discovery reply heard for"));
    }
}

TEST_CASE("A watch that never hears a discovery reply says so rather than reading clear", "[node][formation][conditions]")
{
    // A segment whose discovery replies are firewalled: the window passes and nothing is ever proven.
    core::platform::ManualClock clock;
    NodeConditions conditions;
    Testing::ScriptedSummarySource self { FleetSummary { .clusterId = "c-office", .state = FleetState::Established } };
    Testing::ScriptedSplitEvidence evidence;
    Testing::RecordingFleets next;
    ForeignFleetWatch watch { self, evidence, clock, conditions, next, Cluster::DefaultBeaconInterval };
    clock.advance(Cluster::DefaultBeaconInterval);
    watch.Tick();
    for (auto const condition: ForeignFleetWatch::WatchedConditions)
    {
        CHECK(conditions.StateOf(condition) == Wire::ConditionState::NotEvaluated);
        CHECK(Testing::DetailOf(conditions, condition).contains("no discovery reply heard in the"));
    }
}
