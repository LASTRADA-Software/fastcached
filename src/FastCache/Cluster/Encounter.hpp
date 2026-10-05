// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/FleetPin.hpp>
#include <FastCache/Cluster/ProvenFleetSummary.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

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
///
/// **Two established fleets decide only on SPLIT EVIDENCE**, the third input, and only on the kind
/// that HEALS AUTOMATICALLY. Two fleets proven to be one fleet split in two heal by the same tiebreak;
/// two that share nothing are foreign and neither moves. What tells them apart is never the seen
/// fleet's member list alone -- anybody can mint a fleet listing any id -- but a fact a key this fleet
/// already held verifies (`SplitEvidenceFor`). And only a VOTER's key moves a fleet on its own: the
/// other kinds rest on a key this fleet came to hold on somebody's first-use trust, so they are told
/// to an operator and decide nothing (`SplitEvidenceRow::healing`).
///
/// **A pending node is a POINTER, never a fleet to join.** It has asked another fleet to take it, so
/// its own cluster ends when that fleet admits it; a node yielding to it would be left in a cluster
/// nobody runs. Its summary names the fleet it asked (`FleetState::Pending`), and a solitary node
/// meeting one is told to `Follow`: ask that fleet for itself and decide on what IT proves, exactly
/// as if its own beacon had arrived. Nothing is yielded to on the pointer's word alone.
namespace FastCache::Cluster
{

/// Why a proven fleet of another cluster is taken for this fleet split in two, if it is.
///
/// Decided by `SplitEvidenceFor` on a fact this side ties to a key it held BEFORE the encounter,
/// never on the seen fleet's claim alone. **Private**: never transmitted or persisted.
enum class SplitEvidence : std::uint8_t
{
    None,                     ///< Nothing this side can verify: the fleet is foreign, whatever it lists.
    TheirSpeakerIsOurVoter,   ///< Its speaker is a VOTER of this fleet's state, under that id WITH that key.
    TheirSpeakerIsOurLearner, ///< Its speaker is a LEARNER of this fleet's state, under that id WITH that key.
    WeAskedAndTheyListUs, ///< A machine of this fleet asked it under the key that proves it now, and it lists that machine.
    Last,                 ///< Not evidence: the length of a table keyed by one.
};

/// What a kind of split evidence moves. **Private**: never transmitted or persisted.
enum class SplitHealing : std::uint8_t
{
    NotASplit,     ///< No evidence: the pair is decided as if the two fleets shared nothing.
    ByOperator,    ///< A split an OPERATOR heals: told as `fleet-split-healing`, and no fleet moves on it.
    Automatically, ///< A split the tiebreak heals: the losing fleet's leader proposes its dissolve.
};

/// How an evidence kind is named where an operator reads it, and what it moves.
struct SplitEvidenceRow
{
    SplitEvidence evidence; ///< The kind this row names.
    std::string_view name;  ///< Its name.
    SplitHealing healing;   ///< What it moves.
};

/// One row per evidence kind.
///
/// **Only a VOTER's key heals a split by itself.** A whole established fleet -- its voters, its keys,
/// its leases -- follows a dissolve, so the evidence for one must rest on a key an OPERATOR chose to
/// trust, and a voter is exactly that: a discovered machine is a learner until an operator promotes it.
///
/// - A learner's key is one auto-approval admitted on first use: a learner running a second process
///   under its own key could mint an older fleet and move this one with one beacon.
/// - (C) rests on a key a machine of this fleet once trusted on FIRST USE, by asking whichever fleet
///   beaconed oldest -- and a fleet that refused it, never answered it, or admitted it and then
///   dissolved itself away all leave the same memo, so the asker cannot tell a real split from a
///   fleet minted to be asked.
///
/// Both are real splits often enough to be worth telling, so they raise `fleet-split-healing` as an
/// operator's decision and fail CLOSED, as a foreign fleet does.
inline constexpr EnumTable<SplitEvidence, SplitEvidenceRow> SplitEvidenceNames { {
    { .evidence = SplitEvidence::None, .name = "none", .healing = SplitHealing::NotASplit },
    { .evidence = SplitEvidence::TheirSpeakerIsOurVoter,
      .name = "their speaker is a voter here, under its key",
      .healing = SplitHealing::Automatically },
    { .evidence = SplitEvidence::TheirSpeakerIsOurLearner,
      .name = "their speaker is a learner here, under its key",
      .healing = SplitHealing::ByOperator },
    { .evidence = SplitEvidence::WeAskedAndTheyListUs,
      .name = "a machine here asked that fleet and it lists that machine",
      .healing = SplitHealing::ByOperator },
} };
static_assert(RowsInEnumeratorOrder(SplitEvidenceNames, &SplitEvidenceRow::evidence),
              "SplitEvidenceNames must hold one row per SplitEvidence, in enumerator order");

/// Whether `None` is the one kind that is no split, and every other kind is one.
/// @return True when exactly `SplitEvidence::None` reads `SplitHealing::NotASplit`.
[[nodiscard]] consteval bool OnlyNoneIsNotASplit() noexcept
{
    return std::ranges::all_of(SplitEvidenceNames, [](SplitEvidenceRow const& row) {
        return (row.evidence == SplitEvidence::None) == (row.healing == SplitHealing::NotASplit);
    });
}

static_assert(OnlyNoneIsNotASplit(), "SplitEvidence::None, and nothing else, is no split");

/// What @p evidence moves.
/// @param evidence A kind of split evidence.
/// @return Its row's `healing`.
[[nodiscard]] constexpr SplitHealing HealingOf(SplitEvidence evidence) noexcept
{
    return SplitEvidenceNames[static_cast<std::size_t>(evidence)].healing;
}

/// Whether @p evidence is a split at all: one an operator heals, or one that heals by itself.
/// @param evidence A kind of split evidence.
/// @return False for `SplitEvidence::None` alone.
[[nodiscard]] constexpr bool IsSplit(SplitEvidence evidence) noexcept
{
    return HealingOf(evidence) != SplitHealing::NotASplit;
}

/// What meeting another fleet means for this node.
///
/// **Private**: never transmitted or persisted.
enum class Encounter : std::uint8_t
{
    SameFleet,    ///< The seen node claims this node's cluster id, which is not a credential.
    Yield,        ///< This node leaves its fleet and asks to join the seen one.
    Stay,         ///< This node keeps its fleet; the seen one is expected to yield.
    ForeignFleet, ///< Two established fleets with no split evidence: neither yields, and an operator is told.
    Follow,       ///< The seen node is pending: the fleet it names is asked for itself, and nothing is yielded to yet.
    /// The table would have this node yield, and `--fleet-id` pins it to another cluster: it stays,
    /// and an operator is told (`foreign-fleet-visible`).
    PinnedElsewhere,
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
    Follow,       ///< The seen node points at the fleet it asked; that fleet is asked, and decided on.
};

/// One (own state, seen state) pair and how it is decided, without split evidence and with it.
struct EncounterRow
{
    CompileCacheWire::FleetState own;  ///< This node's fleet state.
    CompileCacheWire::FleetState seen; ///< The seen node's fleet state.
    EncounterRule rule;                ///< How the pair is decided without evidence that heals automatically.
    EncounterRule split; ///< How it is decided when evidence that heals automatically proves the seen fleet a split.
};

/// One row per (own state, seen state) pair, each deciding both with split evidence and without.
///
/// Established beats solitary, and an established fleet never yields to a FOREIGN one -- not even to a
/// larger or an older one, since its members, keys and roster are what an operator built and a yield
/// would abandon them. But two established fleets proven to be ONE fleet split in two, by evidence
/// that heals automatically, are decided by the same tiebreak two solitary ones are, so they heal
/// without an operator, and exactly one moves.
///
/// A pending node is followed by a solitary one -- the only node that would have yielded to it --
/// and stays for everybody else: an established fleet yields to nobody on a pointer, and a pending
/// node has asked already, so its poll decides what it does next and no encounter does.
inline constexpr std::array EncounterTable {
    EncounterRow { .own = CompileCacheWire::FleetState::Solitary,
                   .seen = CompileCacheWire::FleetState::Established,
                   .rule = EncounterRule::Yield,
                   .split = EncounterRule::Yield },
    EncounterRow { .own = CompileCacheWire::FleetState::Established,
                   .seen = CompileCacheWire::FleetState::Solitary,
                   .rule = EncounterRule::Stay,
                   .split = EncounterRule::Stay },
    EncounterRow { .own = CompileCacheWire::FleetState::Established,
                   .seen = CompileCacheWire::FleetState::Established,
                   .rule = EncounterRule::ForeignFleet,
                   .split = EncounterRule::TieBreak },
    EncounterRow { .own = CompileCacheWire::FleetState::Solitary,
                   .seen = CompileCacheWire::FleetState::Solitary,
                   .rule = EncounterRule::TieBreak,
                   .split = EncounterRule::TieBreak },
    EncounterRow { .own = CompileCacheWire::FleetState::Solitary,
                   .seen = CompileCacheWire::FleetState::Pending,
                   .rule = EncounterRule::Follow,
                   .split = EncounterRule::Follow },
    EncounterRow { .own = CompileCacheWire::FleetState::Established,
                   .seen = CompileCacheWire::FleetState::Pending,
                   .rule = EncounterRule::Stay,
                   .split = EncounterRule::Stay },
    EncounterRow { .own = CompileCacheWire::FleetState::Pending,
                   .seen = CompileCacheWire::FleetState::Solitary,
                   .rule = EncounterRule::Stay,
                   .split = EncounterRule::Stay },
    EncounterRow { .own = CompileCacheWire::FleetState::Pending,
                   .seen = CompileCacheWire::FleetState::Established,
                   .rule = EncounterRule::Stay,
                   .split = EncounterRule::Stay },
    EncounterRow { .own = CompileCacheWire::FleetState::Pending,
                   .seen = CompileCacheWire::FleetState::Pending,
                   .rule = EncounterRule::Stay,
                   .split = EncounterRule::Stay },
};

/// Whether split evidence decides ONLY what would otherwise be foreign, and decides it by the
/// tiebreak: evidence heals a split, and must never change how a first join or a solitary node is
/// decided.
/// @return True when every row's `split` equals its `rule`, except a foreign row's, which is `TieBreak`.
[[nodiscard]] consteval bool EvidenceDecidesOnlyForeignPairs() noexcept
{
    return std::ranges::all_of(EncounterTable, [](EncounterRow const& row) {
        return row.rule == EncounterRule::ForeignFleet ? row.split == EncounterRule::TieBreak : row.split == row.rule;
    });
}

static_assert(EvidenceDecidesOnlyForeignPairs(),
              "split evidence may decide only a pair that would otherwise be foreign, and only by the tiebreak");

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

namespace Detail
{
    /// What the encounter table says about a pair of different clusters, before the pin restricts it.
    /// @param own This node's summary.
    /// @param seen The other node's summary, as proven.
    /// @param evidence Whether the other fleet is proven a split of this one.
    /// @param speaker The key that signed @p seen.
    /// @param pin This node's pin, which decides a tiebreak it names the winner of.
    /// @return The table's answer.
    [[nodiscard]] constexpr Encounter ByTable(CompileCacheWire::FleetSummary const& own,
                                              CompileCacheWire::FleetSummary const& seen,
                                              Ed25519PublicKey const& speaker,
                                              SplitEvidence evidence,
                                              FleetPin const& pin) noexcept
    {
        for (auto const& row: EncounterTable)
        {
            if (row.own != own.state || row.seen != seen.state)
                continue;
            switch (HealingOf(evidence) == SplitHealing::Automatically ? row.split : row.rule)
            {
                case EncounterRule::Yield:
                    return Encounter::Yield;
                case EncounterRule::Stay:
                    return Encounter::Stay;
                case EncounterRule::ForeignFleet:
                    return Encounter::ForeignFleet;
                case EncounterRule::Follow:
                    return Encounter::Follow;
                case EncounterRule::TieBreak:
                    // The fleet this node is pinned to wins every tiebreak this node is in: a pinned
                    // node whose own cluster happens to be the older would otherwise stay, waiting to
                    // be joined by a fleet it could never join back, and never reach the one it was
                    // told to. The pinned fleet AS ITS VOTERS SIGN IT: a fleet claiming the id under any
                    // other key wins nothing here.
                    if (pin.fleet.has_value() && AdmitsFleet(pin, seen.clusterId, speaker))
                        return Encounter::Yield;
                    if (own.createdAtUnixSeconds != seen.createdAtUnixSeconds)
                        return own.createdAtUnixSeconds > seen.createdAtUnixSeconds ? Encounter::Yield : Encounter::Stay;
                    return own.clusterId > seen.clusterId ? Encounter::Yield : Encounter::Stay;
            }
        }
        // Unreachable while `EveryStatePairHasOneRule` holds. Staying is the side that fails closed: a
        // node that stays keeps serving its own fleet, while one that yields leaves it.
        return Encounter::Stay;
    }
} // namespace Detail

/// Decide what meeting @p proven means for this node.
///
/// A summary claiming this node's own cluster id is never yielded to, whatever the other fields
/// say: the id is a name, not a credential, and no claim to it moves a node. Otherwise the pair's `EncounterTable` row
/// decides, its `split` column on evidence that heals automatically (`SplitHealing::Automatically`) and its `rule`
/// column on any other -- a split only an operator heals is decided as a foreign fleet is; a seen node that is pending is
/// never yielded to, only followed to the fleet it names. Between two solitary fleets the older
/// creation time wins and a tie in the second goes to the lower cluster id; the ids differ, so the order is strict and
/// exactly one side yields. A creation time of `UnbelievableClockCreatedAt` is compared as the number it is -- the newest
/// possible -- so a node whose clock read before the epoch yields to every sane one.
///
/// **The pin is asked HERE, so no caller decides a yield without it** (`FleetPin`), of the cluster id
/// AND the key that signed the summary -- a fleet claiming the pinned id under a key the pin does not
/// name is an impostor, whatever else it says. It restricts what moves a node and never widens it: a
/// yield the pin does not admit is `PinnedElsewhere`, and the one answer it turns the other way is a
/// TIEBREAK whose winner is the pinned fleet itself, signed by one of its pinned voters. A
/// pinned pair is therefore no longer a mirror of itself -- the asymmetry the operator asked for.
/// @param own This node's summary.
/// @param proven The other node's summary, as its verified signature states it.
/// @param evidence Whether the other fleet is proven a split of this one (`SplitEvidenceFor`); a first
///        join, which only solitary rows decide, passes `SplitEvidence::None`.
/// @param pin The cluster and voter keys `--fleet-id` pins this node to, or none.
/// @return What this node does.
[[nodiscard]] constexpr Encounter ClassifyEncounter(CompileCacheWire::FleetSummary const& own,
                                                    ProvenFleetSummary const& proven,
                                                    SplitEvidence evidence,
                                                    FleetPin const& pin) noexcept
{
    auto const& seen = proven.Summary();
    if (own.clusterId == seen.clusterId)
        return Encounter::SameFleet;
    auto const decided = Detail::ByTable(own, seen, proven.Key(), evidence, pin);
    if (decided == Encounter::Yield && !AdmitsFleet(pin, seen.clusterId, proven.Key()))
        return Encounter::PinnedElsewhere;
    return decided;
}

/// What decided the tiebreak between @p own and @p seen, as an operator reads it.
/// @param own This node's summary.
/// @param seen The other fleet's summary.
/// @return Which field decided.
[[nodiscard]] constexpr std::string_view TieBreakReason(CompileCacheWire::FleetSummary const& own,
                                                        CompileCacheWire::FleetSummary const& seen) noexcept
{
    return own.createdAtUnixSeconds != seen.createdAtUnixSeconds
               ? std::string_view { "the older fleet stays" }
               : std::string_view { "both were created in the same second, so the lower cluster id stays" };
}

/// Whether this node yields to @p proven.
/// @param own This node's summary.
/// @param proven The other node's summary, as its verified signature states it.
/// @param evidence Whether the other fleet is proven a split of this one.
/// @param pin The cluster `--fleet-id` pins this node to, or none.
/// @return True exactly when `ClassifyEncounter` answers `Yield`.
[[nodiscard]] constexpr bool YieldTo(CompileCacheWire::FleetSummary const& own,
                                     ProvenFleetSummary const& proven,
                                     SplitEvidence evidence,
                                     FleetPin const& pin) noexcept
{
    return ClassifyEncounter(own, proven, evidence, pin) == Encounter::Yield;
}

} // namespace FastCache::Cluster
