// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/Encounter.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/ProvenFleetSummary.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    ClassifyEncounter(own, seen, SplitEvidence::None, FleetPin {});
    YieldTo(own, seen, SplitEvidence::None, FleetPin {});
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
        case Encounter::Follow:
        case Encounter::PinnedElsewhere:
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
        // A pending node is a pointer. A solitary node follows it whatever the ages say -- the rows
        // that would have yielded to it by the tiebreak are exactly the ones that must not.
        Row { .name = "solitary meets an older pending node",
              .own = Fleet("c1", Solitary, 9),
              .seen = Fleet("c2", Pending, 1),
              .expected = Encounter::Follow },
        Row { .name = "solitary meets a younger pending node",
              .own = Fleet("c1", Solitary, 1),
              .seen = Fleet("c2", Pending, 9),
              .expected = Encounter::Follow },
        Row { .name = "established meets a pending node",
              .own = Fleet("c1", Established, 9),
              .seen = Fleet("c2", Pending, 1),
              .expected = Encounter::Stay },
        Row { .name = "pending meets an older solitary",
              .own = Fleet("c1", Pending, 9),
              .seen = Fleet("c2", Solitary, 1),
              .expected = Encounter::Stay },
        Row { .name = "pending meets an established fleet",
              .own = Fleet("c1", Pending, 9),
              .seen = Fleet("c2", Established, 1),
              .expected = Encounter::Stay },
        Row { .name = "pending meets an older pending node",
              .own = Fleet("c1", Pending, 9),
              .seen = Fleet("c2", Pending, 1),
              .expected = Encounter::Stay },
    };
    for (auto const& row: rows)
    {
        INFO(row.name);
        CHECK(ClassifyEncounter(row.own, Proven(row.seen), SplitEvidence::None, FleetPin {}) == row.expected);
        CHECK(YieldTo(row.own, Proven(row.seen), SplitEvidence::None, FleetPin {}) == (row.expected == Encounter::Yield));
    }
}

TEST_CASE("Exactly one of two solitary fleets yields to the other whichever meets first", "[cluster][formation][encounter]")
{
    // The split-brain property itself: for every pair, YieldTo(a, b, FleetPin {}) XOR YieldTo(b, a, FleetPin {}).
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
            CHECK(YieldTo(a, Proven(b), SplitEvidence::None, FleetPin {})
                  != YieldTo(b, Proven(a), SplitEvidence::None, FleetPin {}));
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
            CHECK(static_cast<int>(YieldTo(a, Proven(b), SplitEvidence::None, FleetPin {}))
                      + static_cast<int>(YieldTo(b, Proven(a), SplitEvidence::None, FleetPin {}))
                  == (bothEstablished ? 0 : 1));
            CHECK(ClassifyEncounter(b, Proven(a), SplitEvidence::None, FleetPin {})
                  == MirrorOf(ClassifyEncounter(a, Proven(b), SplitEvidence::None, FleetPin {})));
            if (bothEstablished)
                CHECK(ClassifyEncounter(a, Proven(b), SplitEvidence::None, FleetPin {}) == Encounter::ForeignFleet);
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
            // A pending summary's leader slots are its pointer (`FleetStateTable`), so the other
            // leader it names goes there.
            (CompileCacheWire::LeaderSlotsNameAskedFleet(seenState) ? seen.pointsAt.leaderId : seen.leaderId) =
                "someone-else";
            seen.nodeId = "someone-else";
            INFO(static_cast<int>(ownState) << " against " << static_cast<int>(seenState));
            CHECK(ClassifyEncounter(own, Proven(seen), SplitEvidence::None, FleetPin {}) == Encounter::SameFleet);
            CHECK_FALSE(YieldTo(own, Proven(seen), SplitEvidence::None, FleetPin {}));
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
        CHECK(ClassifyEncounter(broken, Proven(sane), SplitEvidence::None, FleetPin {}) == Encounter::Yield);
        CHECK(ClassifyEncounter(sane, Proven(broken), SplitEvidence::None, FleetPin {}) == Encounter::Stay);
    }
    // Established still beats solitary, whichever clock is broken.
    CHECK(ClassifyEncounter(Fleet("aa", FleetState::Established, UnbelievableClockCreatedAt),
                            Proven(Fleet("zz", FleetState::Solitary, 0)),
                            SplitEvidence::None,
                            FleetPin {})
          == Encounter::Stay);
}

TEST_CASE("Two established fleets proven to be one fleet decide by the tiebreak, and exactly one moves",
          "[cluster][formation][encounter][split]")
{
    // The owner's rule: a split of ONE fleet heals without an operator -- on a voter's key. With
    // evidence on both sides the pair is the tiebreak's, so exactly one yields -- the same XOR two
    // solitary fleets obey.
    auto const fleets = std::array { Fleet("aa", FleetState::Established, 7),
                                     Fleet("bb", FleetState::Established, 7),
                                     Fleet("cc", FleetState::Established, 3),
                                     Fleet("dd", FleetState::Established, UnbelievableClockCreatedAt) };
    auto const verified = SplitEvidence::TheirSpeakerIsOurVoter;
    auto pairs = std::size_t { 0 };
    for (auto const& a: fleets)
        for (auto const& b: fleets)
        {
            if (a.clusterId == b.clusterId)
                continue;
            ++pairs;
            INFO(a.clusterId << " against " << b.clusterId);
            CHECK(YieldTo(a, Proven(b), verified, FleetPin {}) != YieldTo(b, Proven(a), verified, FleetPin {}));
            CHECK(ClassifyEncounter(b, Proven(a), verified, FleetPin {})
                  == MirrorOf(ClassifyEncounter(a, Proven(b), verified, FleetPin {})));

            // Evidence on ONE side only: that side decides by the tiebreak and the other stays
            // foreign, so the two can never both yield -- the one that must yield waits for its own
            // evidence rather than the other moving for it.
            auto const oneSided = static_cast<int>(YieldTo(a, Proven(b), verified, FleetPin {}))
                                  + static_cast<int>(YieldTo(b, Proven(a), SplitEvidence::None, FleetPin {}));
            CHECK(oneSided <= 1);
            CHECK(ClassifyEncounter(b, Proven(a), SplitEvidence::None, FleetPin {}) == Encounter::ForeignFleet);
        }
    CHECK(pairs == fleets.size() * (fleets.size() - 1));

    // The winner is the tiebreak's: the older fleet stays, the younger yields.
    CHECK(
        ClassifyEncounter(
            Fleet("zz", FleetState::Established, 9), Proven(Fleet("yy", FleetState::Established, 1)), verified, FleetPin {})
        == Encounter::Yield);
    CHECK(
        ClassifyEncounter(
            Fleet("yy", FleetState::Established, 1), Proven(Fleet("zz", FleetState::Established, 9)), verified, FleetPin {})
        == Encounter::Stay);
}

TEST_CASE("Split evidence changes nothing but what would otherwise be foreign", "[cluster][formation][encounter][split]")
{
    // Every pair a solitary node is in, and the same-id rule, decide alike whatever the evidence says:
    // evidence heals a split and must never move a first join.
    // And a pending node stays a pointer under any evidence: nothing turns a follow into a yield.
    auto const fleets = std::array { Fleet("aa", FleetState::Solitary, 7), Fleet("bb", FleetState::Established, 3),
                                     Fleet("cc", FleetState::Solitary, 1), Fleet("dd", FleetState::Established, 9),
                                     Fleet("ee", FleetState::Pending, 2),  Fleet("ff", FleetState::Pending, 11) };
    auto decided = std::size_t { 0 };
    for (auto const& a: fleets)
        for (auto const& b: fleets)
            for (auto const kind: Enumerators<SplitEvidence>())
            {
                INFO(a.clusterId << " against " << b.clusterId << " with "
                                 << SplitEvidenceNames[static_cast<std::size_t>(kind)].name);
                auto const without = ClassifyEncounter(a, Proven(b), SplitEvidence::None, FleetPin {});
                auto const with = ClassifyEncounter(a, Proven(b), kind, FleetPin {});
                ++decided;
                if (without == Encounter::ForeignFleet && HealingOf(kind) == SplitHealing::Automatically)
                    CHECK((with == Encounter::Yield || with == Encounter::Stay));
                else
                    CHECK(with == without);
            }
    CHECK(decided == fleets.size() * fleets.size() * static_cast<std::size_t>(SplitEvidence::Last));
}

TEST_CASE("Only a voter's key heals a split by itself; a memo's and a learner's are an operator's decision",
          "[cluster][formation][encounter][split][security]")
{
    // A whole fleet follows a dissolve, so the evidence that moves one must rest on a key an operator
    // chose to trust. A memo's key was trusted on first use -- whoever beaconed oldest -- and a
    // learner's was admitted by auto-approval: either one, believed, lets an outsider move an
    // established fleet. So each is a split an operator is told about, decided here as foreign.
    CHECK(HealingOf(SplitEvidence::None) == SplitHealing::NotASplit);
    CHECK(HealingOf(SplitEvidence::TheirSpeakerIsOurVoter) == SplitHealing::Automatically);
    CHECK(HealingOf(SplitEvidence::TheirSpeakerIsOurLearner) == SplitHealing::ByOperator);
    CHECK(HealingOf(SplitEvidence::WeAskedAndTheyListUs) == SplitHealing::ByOperator);

    // An older established fleet: what the younger one decides on each kind.
    auto const own = Fleet("zz", FleetState::Established, 9);
    auto const older = Proven(Fleet("aa", FleetState::Established, 1));
    CHECK(ClassifyEncounter(own, older, SplitEvidence::TheirSpeakerIsOurVoter, FleetPin {}) == Encounter::Yield);
    CHECK(ClassifyEncounter(own, older, SplitEvidence::TheirSpeakerIsOurLearner, FleetPin {}) == Encounter::ForeignFleet);
    CHECK(ClassifyEncounter(own, older, SplitEvidence::WeAskedAndTheyListUs, FleetPin {}) == Encounter::ForeignFleet);
    CHECK(IsSplit(SplitEvidence::WeAskedAndTheyListUs)); // told, all the same
    CHECK_FALSE(IsSplit(SplitEvidence::None));
}

namespace
{
/// A pin to @p id, naming @p voters' keys -- by default the key `Proven` signs @p id's summary with.
/// @param id The pinned cluster id.
/// @param voters Whose `TestKeyPair` the pin names; empty for @p id's own.
/// @return The pin.
[[nodiscard]] FleetPin PinTo(std::string_view id, std::initializer_list<std::string_view> voters = {})
{
    auto named = std::vector<std::string_view> { voters };
    if (named.empty())
        named.push_back(id);
    auto fleet = PinnedFleet { .clusterId = std::string { id }, .voterKeys = {} };
    for (auto const voter: named)
        fleet.voterKeys.push_back(Testing::TestKeyPair(std::string { voter }).PublicKey());
    return FleetPin { .fleet = std::move(fleet) };
}
} // namespace

TEST_CASE("A pinned node does not yield to an older established fleet of another cluster",
          "[cluster][formation][encounter][pin][security]")
{
    // The attack the pin exists for: anybody can prove "established, created at 0" under a fresh key.
    auto const own = Fleet("own", FleetState::Solitary, 900);
    auto const rogue = Proven(Fleet("rogue", FleetState::Established, 0));

    // The control first: trust on first use yields to it, which is why it is an attack.
    CHECK(ClassifyEncounter(own, rogue, SplitEvidence::None, FleetPin {}) == Encounter::Yield);
    CHECK(ClassifyEncounter(own, rogue, SplitEvidence::None, PinTo("office")) == Encounter::PinnedElsewhere);
    CHECK_FALSE(YieldTo(own, rogue, SplitEvidence::None, PinTo("office")));
}

TEST_CASE("A fleet claiming the pinned cluster id under a key the pin does not name is never yielded to",
          "[cluster][formation][encounter][pin][security]")
{
    // The id is a name every beacon carries, so the impostor copies it exactly and signs with its own key.
    auto const own = Fleet("own", FleetState::Solitary, 900);
    auto const impostor = Testing::ProvenBy(Fleet("office", FleetState::Established, 0), "n-rogue");
    CHECK(ClassifyEncounter(own, impostor, SplitEvidence::None, FleetPin {}) == Encounter::Yield); // the control
    CHECK(ClassifyEncounter(own, impostor, SplitEvidence::None, PinTo("office")) == Encounter::PinnedElsewhere);

    // Nor does it win the tiebreak the pinned fleet would: an older node decides as if unpinned, and stays.
    auto const older = Fleet("older", FleetState::Solitary, 1);
    auto const solitaryImpostor = Testing::ProvenBy(Fleet("office", FleetState::Solitary, 900), "n-rogue");
    CHECK(ClassifyEncounter(older, solitaryImpostor, SplitEvidence::None, PinTo("office")) == Encounter::Stay);
}

TEST_CASE("A pinned node yields to the fleet it is pinned to, under any of the pinned voters' keys",
          "[cluster][formation][encounter][pin]")
{
    auto const own = Fleet("own", FleetState::Solitary, 900);
    auto const office = Fleet("office", FleetState::Established, 950);
    CHECK(ClassifyEncounter(own, Proven(office), SplitEvidence::None, PinTo("office")) == Encounter::Yield);

    // A second voter of the office speaks for it: accepted when the pin names that voter too, and
    // refused when it does not -- the key, not the id, is what decides.
    auto const bySecond = Testing::ProvenBy(office, "n-desk");
    CHECK(ClassifyEncounter(own, bySecond, SplitEvidence::None, PinTo("office", { "office", "n-desk" }))
          == Encounter::Yield);
    CHECK(ClassifyEncounter(own, bySecond, SplitEvidence::None, PinTo("office")) == Encounter::PinnedElsewhere);

    // And it wins the tiebreak against an OLDER solitary node pinned to it, which would otherwise wait
    // forever to be joined by a fleet it can never join back.
    auto const older = Fleet("older", FleetState::Solitary, 1);
    auto const solitaryOffice = Proven(Fleet("office", FleetState::Solitary, 900));
    CHECK(ClassifyEncounter(older, solitaryOffice, SplitEvidence::None, FleetPin {}) == Encounter::Stay);
    CHECK(ClassifyEncounter(older, solitaryOffice, SplitEvidence::None, PinTo("office")) == Encounter::Yield);
}

TEST_CASE("The pin compares the whole cluster id, never a prefix and never folded",
          "[cluster][formation][encounter][pin][security]")
{
    // Every pin names the RIGHT key, so the id alone is what each row varies.
    auto const own = Fleet("own", FleetState::Solitary, 900);
    auto const office = Proven(Fleet("0123abcd", FleetState::Established, 1));
    CHECK(ClassifyEncounter(own, office, SplitEvidence::None, PinTo("0123abcd")) == Encounter::Yield);
    for (auto const* const almost: { "0123abc", "0123abcd0", "0123ABCD", "" })
    {
        INFO("pinned to '" << almost << "'");
        CHECK(ClassifyEncounter(own, office, SplitEvidence::None, PinTo(almost, { "0123abcd" }))
              == Encounter::PinnedElsewhere);
    }
}

TEST_CASE("The pin only withholds a yield and never adds one outside a tiebreak", "[cluster][formation][encounter][pin]")
{
    // An established node does not yield to a solitary one because the pin names it, and a pending
    // node pinned to is followed, not joined: the pin restricts what moves a node.
    auto const established = Fleet("own", FleetState::Established, 1);
    CHECK(ClassifyEncounter(
              established, Proven(Fleet("office", FleetState::Solitary, 900)), SplitEvidence::None, PinTo("office"))
          == Encounter::Stay);
    auto const solitary = Fleet("own", FleetState::Solitary, 900);
    CHECK(ClassifyEncounter(solitary, Proven(Fleet("office", FleetState::Pending, 1)), SplitEvidence::None, PinTo("office"))
          == Encounter::Follow);

    // A node pinned to its OWN cluster -- the fleet's founder -- yields to nobody.
    auto const founder = Fleet("office", FleetState::Solitary, 900);
    CHECK(ClassifyEncounter(founder, Proven(Fleet("older", FleetState::Solitary, 1)), SplitEvidence::None, PinTo("office"))
          == Encounter::PinnedElsewhere);
    CHECK(
        ClassifyEncounter(founder, Proven(Fleet("younger", FleetState::Solitary, 950)), SplitEvidence::None, PinTo("office"))
        == Encounter::Stay);

    // And a split a voter's key proves does not move a fleet pinned to itself into the survivor.
    auto const fleet = Fleet("office", FleetState::Established, 9);
    auto const survivor = Proven(Fleet("aa", FleetState::Established, 1));
    CHECK(ClassifyEncounter(fleet, survivor, SplitEvidence::TheirSpeakerIsOurVoter, FleetPin {}) == Encounter::Yield);
    CHECK(ClassifyEncounter(fleet, survivor, SplitEvidence::TheirSpeakerIsOurVoter, PinTo("office"))
          == Encounter::PinnedElsewhere);
}
