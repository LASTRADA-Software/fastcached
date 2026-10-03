// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ProvenFleetSummary.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

/// @file Encounter.hpp
/// What a node does when it meets another fleet, as one pure decision both sides compute alike.
///
/// Two machines that each bootstrapped a solitary cluster and then see each other must end as ONE
/// fleet, so exactly one of them yields -- and each decides on its own, with no message saying which.
/// That works only because the decision is a function of the two summaries alone, and orders them
/// the same way from either side: whatever one node concludes about the pair, the other concludes
/// the mirror of it.
///
/// **The seen side is a `ProvenFleetSummary`, never a bare `FleetSummary`.** These functions trust
/// every field they are given, and a beacon is unauthenticated by construction: deciding on one
/// would let a single spoofed datagram claiming an older fleet make a node yield. So the seen
/// summary is a type only a verified signature produces, and a raw beacon cannot be passed at all.
/// Proven means the signer holds its key, not that what it says is true; see `ProvenFleetSummary`.
namespace FastCache::Cluster
{

/// What meeting another fleet means for this node.
///
/// **Private**: never transmitted or persisted.
enum class Encounter : std::uint8_t
{
    SameFleet,    ///< The seen node claims this node's cluster id, which is not a credential.
    Yield,        ///< This node leaves its fleet and asks to join the seen one.
    Stay,         ///< This node keeps its fleet; the seen one is expected to yield.
    ForeignFleet, ///< Two established fleets see each other: neither yields, and an operator is told.
    Last,
};

/// How one (own state, seen state) pair is decided; the table's only column.
///
/// **Private**: never transmitted or persisted.
enum class EncounterRule : std::uint8_t
{
    Yield,        ///< This node yields, whatever else the summaries say.
    Stay,         ///< This node stays, whatever else the summaries say.
    ForeignFleet, ///< Neither yields.
    TieBreak,     ///< The older fleet wins, then the lower cluster id.
};

/// One (own state, seen state) pair and how it is decided.
struct EncounterRow
{
    CompileCacheWire::FleetState own;  ///< This node's fleet state.
    CompileCacheWire::FleetState seen; ///< The seen node's fleet state.
    EncounterRule rule;                ///< How the pair is decided.
};

/// One row per (own state, seen state) pair.
///
/// Established beats solitary, and an established fleet never yields -- not even to a larger or an
/// older one, since its members, keys and roster are what an operator built and a yield would
/// abandon them. Two solitary fleets are the only pair a comparison decides.
inline constexpr std::array EncounterTable {
    EncounterRow { .own = CompileCacheWire::FleetState::Solitary,
                   .seen = CompileCacheWire::FleetState::Established,
                   .rule = EncounterRule::Yield },
    EncounterRow { .own = CompileCacheWire::FleetState::Established,
                   .seen = CompileCacheWire::FleetState::Solitary,
                   .rule = EncounterRule::Stay },
    EncounterRow { .own = CompileCacheWire::FleetState::Established,
                   .seen = CompileCacheWire::FleetState::Established,
                   .rule = EncounterRule::ForeignFleet },
    EncounterRow { .own = CompileCacheWire::FleetState::Solitary,
                   .seen = CompileCacheWire::FleetState::Solitary,
                   .rule = EncounterRule::TieBreak },
};

/// Whether every (own, seen) pair of fleet states this build knows has exactly one row.
/// @return True when `EncounterTable` decides every pair once.
[[nodiscard]] consteval bool EveryStatePairHasOneRule() noexcept
{
    return std::ranges::all_of(CompileCacheWire::KnownFleetStates, [](CompileCacheWire::FleetState own) {
        return std::ranges::all_of(CompileCacheWire::KnownFleetStates, [own](CompileCacheWire::FleetState seen) {
            return std::ranges::count_if(EncounterTable,
                                         [own, seen](EncounterRow const& row) { return row.own == own && row.seen == seen; })
                   == 1;
        });
    });
}

static_assert(EveryStatePairHasOneRule(), "every (own, seen) fleet-state pair needs exactly one EncounterTable row");

/// Decide what meeting @p proven means for this node.
///
/// A summary claiming this node's own cluster id is never yielded to, whatever the other fields
/// say: the id is a name, not a credential, and no claim to it moves a node. Otherwise the pair's `EncounterTable` row
/// decides. Between two solitary fleets the older creation time wins and a tie in the second goes to the lower cluster id;
/// the ids differ, so the order is strict and exactly one side yields. A creation time of `UnbelievableClockCreatedAt` is
/// compared as the number it is -- the newest possible -- so a node whose clock read before the epoch yields to every sane
/// one.
/// @param own This node's summary.
/// @param proven The other node's summary, as its verified signature states it.
/// @return What this node does.
[[nodiscard]] constexpr Encounter ClassifyEncounter(CompileCacheWire::FleetSummary const& own,
                                                    ProvenFleetSummary const& proven) noexcept
{
    auto const& seen = proven.Summary();
    if (own.clusterId == seen.clusterId)
        return Encounter::SameFleet;
    for (auto const& row: EncounterTable)
    {
        if (row.own != own.state || row.seen != seen.state)
            continue;
        switch (row.rule)
        {
            case EncounterRule::Yield:
                return Encounter::Yield;
            case EncounterRule::Stay:
                return Encounter::Stay;
            case EncounterRule::ForeignFleet:
                return Encounter::ForeignFleet;
            case EncounterRule::TieBreak:
                if (own.createdAtUnixSeconds != seen.createdAtUnixSeconds)
                    return own.createdAtUnixSeconds > seen.createdAtUnixSeconds ? Encounter::Yield : Encounter::Stay;
                return own.clusterId > seen.clusterId ? Encounter::Yield : Encounter::Stay;
        }
    }
    // Unreachable while `EveryStatePairHasOneRule` holds. Staying is the side that fails closed: a
    // node that stays keeps serving its own fleet, while one that yields leaves it.
    return Encounter::Stay;
}

/// Whether this node yields to @p proven.
/// @param own This node's summary.
/// @param proven The other node's summary, as its verified signature states it.
/// @return True exactly when `ClassifyEncounter` answers `Yield`.
[[nodiscard]] constexpr bool YieldTo(CompileCacheWire::FleetSummary const& own, ProvenFleetSummary const& proven) noexcept
{
    return ClassifyEncounter(own, proven) == Encounter::Yield;
}

} // namespace FastCache::Cluster
