// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/Encounter.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/ProvenFleetSummary.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <tests/FormationFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>

using namespace FastCache;
using namespace FastCache::Cluster;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;

namespace
{
/// A summary of fleet @p id in @p state, created at @p created.
/// @param id The cluster id.
/// @param state The fleet state.
/// @param created The creation time, in seconds since the Unix epoch.
/// @return The summary; every other field empty.
[[nodiscard]] FleetSummary Fleet(std::string_view id, FleetState state, std::uint64_t created)
{
    return FleetSummary { .clusterId = std::string { id }, .state = state, .createdAtUnixSeconds = created };
}

/// @p summary as the node that says it would prove it: signed by its own key over a challenge.
/// @param summary What the node says.
/// @return The summary, verified.
[[nodiscard]] ProvenFleetSummary Proven(FleetSummary const& summary)
{
    return Testing::ProvenBy(summary, summary.clusterId);
}

/// Whether the encounter decision can be asked of a seen side of type @p Seen.
template <typename Seen>
concept DecidesAgainst = requires(FleetSummary const& own, Seen const& seen) {
    ClassifyEncounter(own, seen);
    YieldTo(own, seen);
};

// A raw summary -- what a beacon carries -- cannot even be passed as the seen side; only a
// verified one can. Both directions, so the refusal is not an artefact of a concept nothing meets.
static_assert(!DecidesAgainst<FleetSummary>, "a bare FleetSummary must not reach the yield decision");
static_assert(DecidesAgainst<ProvenFleetSummary>, "a ProvenFleetSummary must reach the yield decision");

/// One encounter and what the node meeting it must conclude.
struct Row
{
    std::string_view name; ///< What the row pins.
    FleetSummary own;      ///< This node.
    FleetSummary seen;     ///< The node it meets.
    Encounter expected;    ///< What this node must conclude.
};

/// What the other side of an encounter must conclude when this side concludes @p mine.
/// @param mine This side's conclusion.
/// @return The mirror of it.
[[nodiscard]] Encounter MirrorOf(Encounter mine)
{
    switch (mine)
    {
        case Encounter::Yield:
            return Encounter::Stay;
        case Encounter::Stay:
            return Encounter::Yield;
        case Encounter::SameFleet:
        case Encounter::ForeignFleet:
        case Encounter::Last:
            break;
    }
    return mine;
}
} // namespace

TEST_CASE("ClassifyEncounter decides every pair of fleet states and every tie", "[cluster][formation][encounter]")
{
    using enum FleetState;
    auto const rows = std::array {
        Row { .name = "same cluster id is the same fleet",
              .own = Fleet("c1", Solitary, 5),
              .seen = Fleet("c1", Established, 9),
              .expected = Encounter::SameFleet },
        Row { .name = "solitary meets established",
              .own = Fleet("c1", Solitary, 1),
              .seen = Fleet("c2", Established, 9),
              .expected = Encounter::Yield },
        Row { .name = "established meets solitary",
              .own = Fleet("c1", Established, 9),
              .seen = Fleet("c2", Solitary, 1),
              .expected = Encounter::Stay },
        Row { .name = "established never yields even to an older one",
              .own = Fleet("c1", Established, 9),
              .seen = Fleet("c2", Established, 1),
              .expected = Encounter::ForeignFleet },
        Row { .name = "established meets a younger established",
              .own = Fleet("c1", Established, 1),
              .seen = Fleet("c2", Established, 9),
              .expected = Encounter::ForeignFleet },
        Row { .name = "solitary meets an older solitary",
              .own = Fleet("c1", Solitary, 9),
              .seen = Fleet("c2", Solitary, 1),
              .expected = Encounter::Yield },
        Row { .name = "solitary meets a younger solitary",
              .own = Fleet("c1", Solitary, 1),
              .seen = Fleet("c2", Solitary, 9),
              .expected = Encounter::Stay },
        // Two PCs minted in the same second: the lower cluster id wins, and each side agrees. These
        // two rows are the only pin on WHICH side yields: a comparison flipped to `<` is still a
        // strict order, so it is still antisymmetric and exactly one side still yields -- the
        // symmetry cases below stay green under it, and only these rows notice the wrong winner.
        Row { .name = "same second and own id is higher",
              .own = Fleet("bb", Solitary, 7),
              .seen = Fleet("aa", Solitary, 7),
              .expected = Encounter::Yield },
        Row { .name = "same second and own id is lower",
              .own = Fleet("aa", Solitary, 7),
              .seen = Fleet("bb", Solitary, 7),
              .expected = Encounter::Stay },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(ClassifyEncounter(row.own, Proven(row.seen)) == row.expected);
        CHECK(YieldTo(row.own, Proven(row.seen)) == (row.expected == Encounter::Yield));
    }
}

TEST_CASE("Exactly one of two solitary fleets yields to the other whichever meets first", "[cluster][formation][encounter]")
{
    // The split-brain property itself: for every pair, YieldTo(a, b) XOR YieldTo(b, a).
    auto const fleets = std::array { Fleet("aa", FleetState::Solitary, 7),
                                     Fleet("bb", FleetState::Solitary, 7),
                                     Fleet("cc", FleetState::Solitary, 3),
                                     Fleet("dd", FleetState::Solitary, 11) };
    for (auto const& a: fleets)
        for (auto const& b: fleets)
        {
            if (a.clusterId == b.clusterId)
                continue;
            INFO(a.clusterId << " against " << b.clusterId);
            CHECK(YieldTo(a, Proven(b)) != YieldTo(b, Proven(a)));
        }
}

TEST_CASE("Two sides of every encounter conclude mirror images of each other", "[cluster][formation][encounter]")
{
    // Every state, older and younger, a same-second tie, and a clock that read before the epoch.
    auto const fleets = std::array {
        Fleet("aa", FleetState::Solitary, 7),
        Fleet("bb", FleetState::Solitary, 7),
        Fleet("cc", FleetState::Solitary, 3),
        Fleet("dd", FleetState::Solitary, UnbelievableClockCreatedAt),
        Fleet("ee", FleetState::Solitary, UnbelievableClockCreatedAt),
        Fleet("ff", FleetState::Established, 7),
        Fleet("gg", FleetState::Established, 1),
        Fleet("hh", FleetState::Established, UnbelievableClockCreatedAt),
    };
    auto pairs = std::size_t { 0 };
    for (auto const& a: fleets)
        for (auto const& b: fleets)
        {
            if (a.clusterId == b.clusterId)
                continue;
            ++pairs;
            INFO(a.clusterId << " against " << b.clusterId);
            auto const bothEstablished = a.state == FleetState::Established && b.state == FleetState::Established;
            // Exactly one yields, or neither when both are established -- never both.
            CHECK(static_cast<int>(YieldTo(a, Proven(b))) + static_cast<int>(YieldTo(b, Proven(a)))
                  == (bothEstablished ? 0 : 1));
            CHECK(ClassifyEncounter(b, Proven(a)) == MirrorOf(ClassifyEncounter(a, Proven(b))));
            if (bothEstablished)
                CHECK(ClassifyEncounter(a, Proven(b)) == Encounter::ForeignFleet);
        }
    CHECK(pairs == fleets.size() * (fleets.size() - 1));
}

TEST_CASE("A node never yields to its own cluster id whatever the other fields say", "[cluster][formation][encounter]")
{
    for (auto const ownState: CompileCacheWire::KnownFleetStates)
        for (auto const seenState: CompileCacheWire::KnownFleetStates)
        {
            // The seen summary claims an older fleet and a different leader: still the same fleet.
            auto const own = Fleet("c1", ownState, 9);
            auto seen = Fleet("c1", seenState, 1);
            seen.leaderId = "someone-else";
            seen.nodeId = "someone-else";
            INFO(static_cast<int>(ownState) << " against " << static_cast<int>(seenState));
            CHECK(ClassifyEncounter(own, Proven(seen)) == Encounter::SameFleet);
            CHECK_FALSE(YieldTo(own, Proven(seen)));
        }
}

TEST_CASE("A clock that read before the epoch yields to every sane one", "[cluster][formation][encounter]")
{
    auto const broken = Fleet("aa", FleetState::Solitary, UnbelievableClockCreatedAt);
    // The broken one holds the LOWER id, so an id tie-break alone would have kept it.
    for (auto const created: std::array<std::uint64_t, 4> { 0, 1, 1'790'000'000, UnbelievableClockCreatedAt - 1 })
    {
        auto const sane = Fleet("zz", FleetState::Solitary, created);
        INFO(created);
        CHECK(ClassifyEncounter(broken, Proven(sane)) == Encounter::Yield);
        CHECK(ClassifyEncounter(sane, Proven(broken)) == Encounter::Stay);
    }
    // Established still beats solitary, whichever clock is broken.
    CHECK(ClassifyEncounter(Fleet("aa", FleetState::Established, UnbelievableClockCreatedAt),
                            Proven(Fleet("zz", FleetState::Solitary, 0)))
          == Encounter::Stay);
}
