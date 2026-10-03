// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/MachineStanding.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstdint>
#include <optional>

namespace FastCache::Distributed
{

/// How the admission vocabulary travels, in ONE place (#1471).
///
/// `CompileCacheWire.hpp` must stay dependency-free, so it MIRRORS `Membership` and
/// `MembershipParticipant` rather than carrying them; the two halves are joined by the tables
/// below. They live in the library rather than in the node app because a mirror needs a reader at
/// each end -- the node ENCODES an answer and `fastcache-cli` RENDERS one -- and two copies of a
/// mapping are two things that can disagree about which bit means which route, in a verb whose
/// whole subject is attributing a decision correctly.

/// One row of `MembershipWireVerdicts`.
struct MembershipVerdictRow
{
    Membership verdict;                   ///< What the fold calls it.
    CompileCacheWire::WireMembership tag; ///< What the wire calls it.
};

/// Which wire tag each admission verdict travels as.
///
/// A table rather than a cast: `Membership`'s order is load-bearing to `PrecedenceOf`, so
/// transmitting the ordinal directly would make a precedence change a wire change.
constexpr EnumTable<Membership, MembershipVerdictRow> MembershipWireVerdicts { {
    { .verdict = Membership::Outsider, .tag = CompileCacheWire::WireMembership::Outsider },
    { .verdict = Membership::Member, .tag = CompileCacheWire::WireMembership::Member },
    { .verdict = Membership::Forgotten, .tag = CompileCacheWire::WireMembership::Forgotten },
} };

static_assert(RowsInEnumeratorOrder(MembershipWireVerdicts, &MembershipVerdictRow::verdict),
              "MembershipWireVerdicts must hold one row per Membership, in enumerator order");

/// One row of `MembershipWireRoutes`.
struct MembershipRouteRow
{
    MembershipParticipant route; ///< What the fold calls it.
    std::uint32_t bit;           ///< The bit it travels as, a `CompileCacheWire::WireMembershipRoute` value; 0 for none.
};

/// Which bit each admission route travels as.
///
/// A SET on the wire, so these are powers of two rather than ordinals: an operator asking why a
/// caller is served needs to know when BOTH a proof and a ticket admit it, and a single value could
/// not say so. `Reserved` travels as NO bit, and two things keep it off the wire: its row here
/// carries zero, so OR-ing it in changes nothing, and `MembershipParticipantSet::Add` never records
/// it, so no decision names it in the first place.
constexpr EnumTable<MembershipParticipant, MembershipRouteRow> MembershipWireRoutes { {
    { .route = MembershipParticipant::Reserved, .bit = 0 },
    { .route = MembershipParticipant::Loopback, .bit = CompileCacheWire::WireMembershipRoute::Loopback },
    { .route = MembershipParticipant::OpenPolicy, .bit = CompileCacheWire::WireMembershipRoute::OpenPolicy },
    { .route = MembershipParticipant::ProvenIdentity, .bit = CompileCacheWire::WireMembershipRoute::ProvenIdentity },
    { .route = MembershipParticipant::MachineTicket, .bit = CompileCacheWire::WireMembershipRoute::MachineTicket },
    { .route = MembershipParticipant::KeyTombstone, .bit = CompileCacheWire::WireMembershipRoute::KeyTombstone },
} };

static_assert(RowsInEnumeratorOrder(MembershipWireRoutes, &MembershipRouteRow::route),
              "MembershipWireRoutes must hold one row per MembershipParticipant, in enumerator order: a route with "
              "no bit would travel as silence, and this verb exists to stop a silence being read as an answer");

/// @return Whether no row of the table that TRANSMITS carries a retired route bit.
///
/// Asked here rather than only of the wire header's named constants, because this table is what
/// puts bits on the wire: a row spelled with a literal, or a new participant given a retired bit,
/// passes a check over the names and is caught only here.
[[nodiscard]] constexpr bool NoTransmittedRouteIsRetired() noexcept
{
    return std::ranges::none_of(
        MembershipWireRoutes, [](MembershipRouteRow const& row) { return CompileCacheWire::CarriesRetiredRoute(row.bit); });
}

static_assert(NoTransmittedRouteIsRetired(),
              "a retired route bit is reserved: a deployed client reads it by its old name, so no route may travel as one");

/// One row of `MachineStandingWire`.
struct MachineStandingRow
{
    MachineStanding standing;                  ///< What the roster says.
    CompileCacheWire::WireMachineStanding tag; ///< What the wire calls it.
};

/// Which wire tag each standing travels as: a table rather than a cast, so the private enum's order
/// is not a wire contract.
constexpr EnumTable<MachineStanding, MachineStandingRow> MachineStandingWire { {
    { .standing = MachineStanding::Voter, .tag = CompileCacheWire::WireMachineStanding::Voter },
    { .standing = MachineStanding::Learner, .tag = CompileCacheWire::WireMachineStanding::Learner },
    { .standing = MachineStanding::Pending, .tag = CompileCacheWire::WireMachineStanding::Pending },
    { .standing = MachineStanding::Revoked, .tag = CompileCacheWire::WireMachineStanding::Revoked },
    { .standing = MachineStanding::Unknown, .tag = CompileCacheWire::WireMachineStanding::Unknown },
} };

static_assert(RowsInEnumeratorOrder(MachineStandingWire, &MachineStandingRow::standing),
              "MachineStandingWire must hold one row per MachineStanding, in enumerator order");

/// @param standing Where a machine stands.
/// @return Its wire tag.
[[nodiscard]] constexpr CompileCacheWire::WireMachineStanding OnTheWire(MachineStanding standing) noexcept
{
    return MachineStandingWire[static_cast<std::size_t>(standing)].tag;
}

/// @param tag A standing as the wire carries it.
/// @return The standing, or nullopt for a tag this table does not hold.
[[nodiscard]] constexpr std::optional<MachineStanding> StandingOnTheWire(CompileCacheWire::WireMachineStanding tag) noexcept
{
    for (auto const& row: MachineStandingWire)
        if (row.tag == tag)
            return row.standing;
    return std::nullopt;
}

/// @p decision as the wire carries it.
/// @param decision What the oracle concluded.
/// @return The same answer, in the wire's vocabulary.
[[nodiscard]] constexpr CompileCacheWire::AdmissionExplanationFields OnTheWire(MembershipDecision decision) noexcept
{
    auto bits = std::uint32_t { 0 };
    for (auto const& row: MembershipWireRoutes)
        if (decision.decidedBy.Has(row.route))
            bits |= row.bit;

    return { .verdict = MembershipWireVerdicts[static_cast<std::size_t>(decision.verdict)].tag, .decidedBy = bits };
}

} // namespace FastCache::Distributed
