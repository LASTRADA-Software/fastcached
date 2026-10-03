// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/FormationTransitions.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/SeedSources.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <tests/FormationFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;

namespace
{
/// A summary of fleet @p id in @p state, created at @p created, whose leader answers at @p endpoint.
/// @param id The cluster id.
/// @param state The fleet state.
/// @param created The creation time, in seconds since the Unix epoch.
/// @param endpoint Where its leader answers the `0xFC` port.
/// @return The summary; every other field empty.
[[nodiscard]] FleetSummary Fleet(std::string_view id,
                                 FleetState state,
                                 std::uint64_t created,
                                 std::string_view endpoint = {})
{
    return FleetSummary { .clusterId = std::string { id },
                          .state = state,
                          .createdAtUnixSeconds = created,
                          .leaderNodeEndpoint = std::string { endpoint } };
}

/// The cluster id of the fleet `PreferredTarget` chose, or empty when it chose none.
/// @param chosen What it returned.
/// @return The chosen fleet's id.
[[nodiscard]] std::string ChosenId(std::optional<ProvenFleet> const& chosen)
{
    return chosen.has_value() ? Testing::Unwrap(chosen).Summary().clusterId : std::string {};
}
} // namespace

TEST_CASE("Every transition row is unique and every reform leaves the node in another shape",
          "[cluster][formation][transitions]")
{
    // The node's SHAPE is what a reform rebuilds: its Raft listener, how it dials, whether it
    // schedules, or the cluster it runs (the two effects that leave one). Which cluster a mode's
    // consensus runs is NOT shape: a founder whose cluster recorded another machine is a voter of
    // the SAME cluster, which is now a fleet, and nothing about it is rebuilt.
    for (auto const& row: FormationTransitions)
    {
        INFO(NodeModeRowFor(row.from).name << " -> " << NodeModeRowFor(row.to).name);
        CHECK(std::ranges::count_if(FormationTransitions,
                                    [&row](FormationTransition const& other) {
                                        return other.from == row.from && other.trigger == row.trigger;
                                    })
              == 1);
        auto const& before = NodeModeRowFor(row.from);
        auto const& after = NodeModeRowFor(row.to);
        auto const shapeChanges = before.raftListener != after.raftListener || before.dials != after.dials
                                  || before.scheduler != after.scheduler || row.effect == FormationEffect::Dissolve
                                  || row.effect == FormationEffect::ArchiveAndMint
                                  || row.effect == FormationEffect::LeaveForSurvivor;
        CHECK(row.reform == shapeChanges);
    }
}

TEST_CASE("A machine that joined a fleet never yields to a foreign fleet", "[cluster][formation][transitions]")
{
    // Trust on first use is for the FIRST join only: a learner or a voter has no row for the trigger
    // a yield to a foreign fleet fires. Healing a split of ONE fleet is a different trigger, decided
    // on evidence a key verifies, and never this one.
    CHECK(TransitionFor(NodeMode::Voter, FormationTrigger::YieldDecided) == nullptr);
    CHECK(TransitionFor(NodeMode::Learner, FormationTrigger::YieldDecided) == nullptr);
    CHECK(TransitionFor(NodeMode::Pending, FormationTrigger::YieldDecided) == nullptr); // one join at a time

    // The control: the solitary node, which has joined nothing, does yield.
    auto const* const first = TransitionFor(NodeMode::Solitary, FormationTrigger::YieldDecided);
    REQUIRE(first != nullptr);
    CHECK(first->to == NodeMode::Pending);
    CHECK(first->effect == FormationEffect::RecordJoinTarget);
}

TEST_CASE("Only a member leaves on a dissolve, and always for a new cluster of its own", "[cluster][formation][transitions]")
{
    // The order is the FLEET's: its voters and learners follow it. A solitary or pending node has no
    // fleet an order could be about.
    for (auto const mode: { NodeMode::Voter, NodeMode::Learner })
    {
        auto const* const row = TransitionFor(mode, FormationTrigger::DissolvedInto);
        REQUIRE(row != nullptr);
        CHECK(row->to == NodeMode::Pending);
        CHECK(row->effect == FormationEffect::LeaveForSurvivor);
        CHECK(row->reform);
    }
    CHECK(TransitionFor(NodeMode::Solitary, FormationTrigger::DissolvedInto) == nullptr);
    CHECK(TransitionFor(NodeMode::Pending, FormationTrigger::DissolvedInto) == nullptr);
}

TEST_CASE("A trigger a mode has no row for is ignored", "[cluster][formation][transitions]")
{
    CHECK(TransitionFor(NodeMode::Solitary, FormationTrigger::Approved) == nullptr);
    CHECK(TransitionFor(NodeMode::Solitary, FormationTrigger::SelfForgotten) == nullptr);
    CHECK(TransitionFor(NodeMode::Learner, FormationTrigger::Established) == nullptr);
    CHECK(TransitionFor(NodeMode::Voter, FormationTrigger::SeatedVoter) == nullptr);
}

TEST_CASE("A fleet named by --fleet-seed is preferred over an older fleet a beacon found",
          "[cluster][formation][transitions]")
{
    // The seed an administrator typed at install outranks anything on the LAN, whatever its age.
    auto const own = Fleet("m", FleetState::Solitary, 500);
    auto const beaconOld = Testing::ProvenBeacon(Fleet("rogue", FleetState::Established, 1));
    auto const seeded = Testing::ProvenSeed(Fleet("office", FleetState::Established, 400), SeedSource::FleetSeedFlag);
    REQUIRE(seeded.Origin() == FleetOrigin::FleetSeedFlag);
    REQUIRE(beaconOld.Origin() == FleetOrigin::Beacon);

    auto const seen = std::array { beaconOld, seeded };
    CHECK(ChosenId(PreferredTarget(own, seen)) == "office");
    auto const reversed = std::array { seeded, beaconOld };
    CHECK(ChosenId(PreferredTarget(own, reversed)) == "office"); // by rank, never by arrival order

    // The control: with the seed gone, the beacon's older fleet is the one asked.
    auto const beaconOnly = std::array { beaconOld };
    CHECK(ChosenId(PreferredTarget(own, beaconOnly)) == "rogue");
}

TEST_CASE("The preferred target is established first then older then the lower id", "[cluster][formation][transitions]")
{
    auto const own = Fleet("m", FleetState::Solitary, 500);
    auto const oldSolitary = Testing::ProvenBeacon(Fleet("a", FleetState::Solitary, 100));
    auto const youngFleet = Testing::ProvenBeacon(Fleet("z", FleetState::Established, 400));
    auto const sameAgeLower = Testing::ProvenBeacon(Fleet("b", FleetState::Established, 400));
    auto const younger = Testing::ProvenBeacon(Fleet("c", FleetState::Solitary, 900));

    auto const seen = std::array { oldSolitary, youngFleet, sameAgeLower, younger };
    CHECK(ChosenId(PreferredTarget(own, seen)) == "b");
    auto const noFleet = std::array { oldSolitary, younger };
    CHECK(ChosenId(PreferredTarget(own, noFleet)) == "a");
    auto const nothingToYieldTo = std::array { younger };
    CHECK_FALSE(PreferredTarget(own, nothingToYieldTo).has_value());
}

TEST_CASE("An established node never picks a target, however old the fleet it sees", "[cluster][formation][transitions]")
{
    // Whether to yield at all is `ClassifyEncounter`'s answer, never re-derived: two established
    // fleets with no member in common are foreign, and nobody yields.
    auto const own = Fleet("mine", FleetState::Established, 500);
    auto const older =
        std::array { Testing::ProvenSeed(Fleet("office", FleetState::Established, 1), SeedSource::FleetSeedFlag) };
    CHECK_FALSE(PreferredTarget(own, older).has_value());
}

TEST_CASE("Every fleet origin has a rank of its own, typed seed first and beacon last", "[cluster][formation][transitions]")
{
    // A column rather than the enumerators' order: the order `OrderSeeds` tries seed sources in
    // after the typed one, and a beacon -- anybody who can reach the segment -- last.
    CHECK(PreferenceOf(FleetOrigin::FleetSeedFlag).rank < PreferenceOf(FleetOrigin::Remembered).rank);
    CHECK(PreferenceOf(FleetOrigin::Remembered).rank < PreferenceOf(FleetOrigin::DnsSrv).rank);
    CHECK(PreferenceOf(FleetOrigin::DnsSrv).rank < PreferenceOf(FleetOrigin::Beacon).rank);

    // And the rank is what decides, for every pair of seed sources: the better origin wins over an
    // older fleet from the worse one.
    auto const own = Fleet("m", FleetState::Solitary, 500);
    auto const remembered = Testing::ProvenSeed(Fleet("home", FleetState::Established, 400), SeedSource::Remembered);
    auto const dns = Testing::ProvenSeed(Fleet("dns", FleetState::Established, 1), SeedSource::DnsSrv);
    auto const seen = std::array { dns, remembered };
    CHECK(ChosenId(PreferredTarget(own, seen)) == "home");
}
