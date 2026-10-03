// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/Encounter.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>

/// @file FormationTransitions.hpp
/// How a node moves between modes, as data: one row per (mode, trigger) pair that means anything,
/// and the choice of which proven fleet a solitary node asks to join.
///
/// Every later task and every rule reads a ROW. A new transition -- a shared-cache overlay, a split
/// healing -- is a row, never an `if` threaded through the controller that acts on them.
namespace FastCache::Cluster
{

/// What moved a node between modes.
///
/// **Private**: never transmitted or persisted, so the enumerator values bind nothing.
enum class FormationTrigger : std::uint8_t
{
    YieldDecided,  ///< A proven fleet this node's summary yields to (a FIRST join; never a foreign fleet's).
    Established,   ///< Its own cluster recorded another machine.
    Approved,      ///< The fleet it asked admitted it and handed over the roster.
    Rejected,      ///< A person said no.
    Abandoned,     ///< The fleet stopped answering for `PendingGiveUpAfter`.
    SeatedVoter,   ///< The applied state seats this node as a voter.
    SeatedLearner, ///< The applied state seats this node as a learner.
    SelfForgotten, ///< Forgotten: the applied state (#1539), or a signed `OwnKeyRevoked` verdict.
    DissolvedInto, ///< The applied state carries the fleet's order to dissolve into a survivor.
};

/// What the controller does on a transition, BEFORE the mode it leads to is acted on.
///
/// **Private**, for `FormationTrigger`'s reason. Each value selects different work, so the
/// controller switches on it -- the `EnrollRoleTable` precedent for a closed set whose arms share
/// nothing.
enum class FormationEffect : std::uint8_t
{
    RecordJoinTarget,  ///< Remember the fleet it asks, and that it asked.
    ClearJoinTarget,   ///< Forget the fleet it was asking.
    RememberRejection, ///< Forget the fleet it was asking, and that it refused, for `RejectedRetryAfter`.
    Dissolve,          ///< Take on the fleet's roster and archive its own cluster's store.
    AdoptSeat,         ///< Nothing to write beyond the mode: the seat is the applied state's.
    ArchiveAndMint,    ///< Archive the fleet's store and mint a new solitary cluster.
    LeaveForSurvivor,  ///< Archive the fleet's store, mint a new solitary cluster, and ask the survivor.
    None,              ///< Nothing beyond the mode.
};

/// One transition: from a mode, on a trigger, to a mode, doing an effect.
struct FormationTransition
{
    NodeMode from;            ///< The mode it applies in.
    FormationTrigger trigger; ///< What fires it.
    NodeMode to;              ///< The mode it leads to.
    FormationEffect effect;   ///< What is done first.
    bool reform;              ///< Ends the serving body, so `main` rebuilds it from the new record.
};

/// Every transition that means anything. A (mode, trigger) pair with no row is IGNORED.
///
/// `reform` is true exactly when the node's shape changes -- its Raft listener, how it dials, or
/// whether it schedules -- or the cluster it runs does (`Dissolve`, `ArchiveAndMint`). A founder
/// whose cluster recorded another machine becomes a voter of the SAME cluster, which is now a fleet,
/// so it does not reform: `ConsensusScope` names the change and nothing rebuilds for it.
///
/// **No `YieldDecided` row from `Learner` or `Voter`.** Trust on first use is for the FIRST join
/// only: a machine in a fleet never yields to a foreign one. Healing a split of ONE fleet is a
/// different trigger, on evidence a key verifies, never this one.
inline constexpr std::array FormationTransitions {
    FormationTransition { .from = NodeMode::Solitary,
                          .trigger = FormationTrigger::YieldDecided,
                          .to = NodeMode::Pending,
                          .effect = FormationEffect::RecordJoinTarget,
                          .reform = false },
    FormationTransition { .from = NodeMode::Solitary,
                          .trigger = FormationTrigger::Established,
                          .to = NodeMode::Voter,
                          .effect = FormationEffect::None,
                          .reform = false },
    FormationTransition { .from = NodeMode::Pending,
                          .trigger = FormationTrigger::Approved,
                          .to = NodeMode::Learner,
                          .effect = FormationEffect::Dissolve,
                          .reform = true },
    FormationTransition { .from = NodeMode::Pending,
                          .trigger = FormationTrigger::Rejected,
                          .to = NodeMode::Solitary,
                          .effect = FormationEffect::RememberRejection,
                          .reform = false },
    FormationTransition { .from = NodeMode::Pending,
                          .trigger = FormationTrigger::Abandoned,
                          .to = NodeMode::Solitary,
                          .effect = FormationEffect::ClearJoinTarget,
                          .reform = false },
    FormationTransition { .from = NodeMode::Pending,
                          .trigger = FormationTrigger::Established,
                          .to = NodeMode::Voter,
                          .effect = FormationEffect::ClearJoinTarget,
                          .reform = false },
    FormationTransition { .from = NodeMode::Learner,
                          .trigger = FormationTrigger::SeatedVoter,
                          .to = NodeMode::Voter,
                          .effect = FormationEffect::AdoptSeat,
                          .reform = true },
    FormationTransition { .from = NodeMode::Voter,
                          .trigger = FormationTrigger::SeatedLearner,
                          .to = NodeMode::Learner,
                          .effect = FormationEffect::AdoptSeat,
                          .reform = true },
    FormationTransition { .from = NodeMode::Learner,
                          .trigger = FormationTrigger::SelfForgotten,
                          .to = NodeMode::Solitary,
                          .effect = FormationEffect::ArchiveAndMint,
                          .reform = true },
    FormationTransition { .from = NodeMode::Voter,
                          .trigger = FormationTrigger::SelfForgotten,
                          .to = NodeMode::Solitary,
                          .effect = FormationEffect::ArchiveAndMint,
                          .reform = true },
    // A healing split: the losing fleet's every member leaves for the survivor at once, on the order
    // its leader replicated. Pending in a NEW solitary cluster of its own, never the left one reopened.
    FormationTransition { .from = NodeMode::Voter,
                          .trigger = FormationTrigger::DissolvedInto,
                          .to = NodeMode::Pending,
                          .effect = FormationEffect::LeaveForSurvivor,
                          .reform = true },
    FormationTransition { .from = NodeMode::Learner,
                          .trigger = FormationTrigger::DissolvedInto,
                          .to = NodeMode::Pending,
                          .effect = FormationEffect::LeaveForSurvivor,
                          .reform = true },
};

/// Whether no (mode, trigger) pair has two rows: the controller acts on the first it finds, so a
/// second would be a transition nothing ever takes.
/// @return True when every pair is unique.
[[nodiscard]] consteval bool EveryTransitionIsUnique() noexcept
{
    return std::ranges::all_of(FormationTransitions, [](FormationTransition const& row) {
        return std::ranges::count_if(FormationTransitions,
                                     [&row](FormationTransition const& other) {
                                         return other.from == row.from && other.trigger == row.trigger;
                                     })
               == 1;
    });
}

static_assert(EveryTransitionIsUnique(), "a (mode, trigger) pair has exactly one FormationTransitions row, or none");

/// The row for (@p from, @p trigger).
/// @param from The mode the node is in.
/// @param trigger What happened.
/// @return The row, or nullptr: a trigger a mode has no row for is ignored.
[[nodiscard]] constexpr FormationTransition const* TransitionFor(NodeMode from, FormationTrigger trigger) noexcept
{
    // A loop naming no iterator: `std::array`'s is a pointer on one standard library and a class on
    // another, so a named one cannot satisfy clang-tidy and MSVC at once.
    for (auto const& row: FormationTransitions)
        if (row.from == from && row.trigger == trigger)
            return &row;
    return nullptr;
}

/// How strongly an origin recommends the fleet it proved: lower is preferred.
struct FleetOriginPreferenceRow
{
    FleetOrigin origin;    ///< The origin.
    std::uint8_t rank;     ///< Its preference; lower wins.
    std::string_view name; ///< What a log line calls it.
};

/// The preference between origins, as a COLUMN rather than the enumerators' order.
///
/// A seed an administrator typed at install outranks anything on the LAN, a remembered fleet
/// outranks one DNS names, and a beacon -- anybody who can reach the segment -- comes last. The same
/// order `OrderSeeds` tries the seed sources in after the typed one. One row per origin, so an origin
/// added tomorrow does not build until it states its rank.
inline constexpr EnumTable<FleetOrigin, FleetOriginPreferenceRow> FleetOriginPreference { {
    { .origin = FleetOrigin::FleetSeedFlag, .rank = 0, .name = "--fleet-seed" },
    { .origin = FleetOrigin::Remembered, .rank = 1, .name = "remembered" },
    { .origin = FleetOrigin::DnsSrv, .rank = 2, .name = "dns-srv" },
    { .origin = FleetOrigin::Beacon, .rank = 3, .name = "beacon" },
} };
static_assert(RowsInEnumeratorOrder(FleetOriginPreference, &FleetOriginPreferenceRow::origin),
              "FleetOriginPreference must hold one row per FleetOrigin, in enumerator order");

/// Whether no two origins share a rank, so the preference is a total order.
/// @return True when every rank is its own.
[[nodiscard]] consteval bool EveryOriginRankIsDistinct() noexcept
{
    return std::ranges::all_of(FleetOriginPreference, [](FleetOriginPreferenceRow const& row) {
        return std::ranges::count(FleetOriginPreference, row.rank, &FleetOriginPreferenceRow::rank) == 1;
    });
}

static_assert(EveryOriginRankIsDistinct(), "every FleetOrigin needs a rank of its own");

/// The preference row for @p origin.
/// @param origin An origin; never `Last`.
/// @return Its row.
[[nodiscard]] constexpr FleetOriginPreferenceRow const& PreferenceOf(FleetOrigin origin) noexcept
{
    return FleetOriginPreference[static_cast<std::size_t>(origin)];
}

/// Among the proven fleets this node YIELDS to, the one to ask.
///
/// The best ORIGIN first (a `--fleet-seed` fleet beats a beacon's, whatever its age), then an
/// established fleet before a solitary one, then the older, then the lower cluster id. Whether this
/// node yields at all is `ClassifyEncounter`'s answer, never re-derived here.
/// @param own What this node says about itself.
/// @param seen The proven fleets it knows of.
/// @param pin The cluster `--fleet-id` pins this node to, or none: asked of every candidate through
///        `ClassifyEncounter`, so a fleet the pin refuses is never the one chosen.
/// @return The fleet to ask, or nothing when it yields to none of them.
[[nodiscard]] inline std::optional<ProvenFleet> PreferredTarget(CompileCacheWire::FleetSummary const& own,
                                                                std::span<ProvenFleet const> seen,
                                                                FleetPin const& pin)
{
    auto const key = [](ProvenFleet const& fleet) {
        auto const& summary = fleet.Summary();
        return std::tuple { PreferenceOf(fleet.Origin()).rank,
                            summary.state == CompileCacheWire::FleetState::Established ? 0 : 1,
                            summary.createdAtUnixSeconds,
                            std::string_view { summary.clusterId } };
    };
    ProvenFleet const* best = nullptr;
    for (auto const& fleet: seen)
    {
        // A FIRST join: only a solitary node asks, and the rows a solitary node meets decide alike
        // with split evidence and without (`EvidenceDecidesOnlyForeignPairs`).
        if (ClassifyEncounter(own, fleet.Proven(), SplitEvidence::None, pin) != Encounter::Yield)
            continue;
        if (best == nullptr || key(fleet) < key(*best))
            best = &fleet;
    }
    if (best == nullptr)
        return std::nullopt;
    return *best;
}

} // namespace FastCache::Cluster
