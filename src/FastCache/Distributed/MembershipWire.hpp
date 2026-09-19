// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstdint>

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
    std::uint32_t bit;           ///< The bit it travels as, a `CompileCacheWire::WireMembershipRoute` value.
};

/// Which bit each admission route travels as.
///
/// A SET on the wire, so these are powers of two rather than ordinals: an operator asking why a
/// host is still served needs to know that BOTH `--fleet-member` and the cluster admit it, and a
/// single value could not say so.
constexpr EnumTable<MembershipParticipant, MembershipRouteRow> MembershipWireRoutes { {
    { .route = MembershipParticipant::FleetMemberList, .bit = CompileCacheWire::WireMembershipRoute::FleetMemberList },
    { .route = MembershipParticipant::ClusterMembers, .bit = CompileCacheWire::WireMembershipRoute::ClusterMembers },
    { .route = MembershipParticipant::ClientTombstone, .bit = CompileCacheWire::WireMembershipRoute::ClientTombstone },
    { .route = MembershipParticipant::OpenPolicy, .bit = CompileCacheWire::WireMembershipRoute::OpenPolicy },
    { .route = MembershipParticipant::ProvenIdentity, .bit = CompileCacheWire::WireMembershipRoute::ProvenIdentity },
    { .route = MembershipParticipant::KeyTombstone, .bit = CompileCacheWire::WireMembershipRoute::KeyTombstone },
} };

static_assert(RowsInEnumeratorOrder(MembershipWireRoutes, &MembershipRouteRow::route),
              "MembershipWireRoutes must hold one row per MembershipParticipant, in enumerator order: a route with "
              "no bit would travel as silence, and this verb exists to stop a silence being read as an answer");

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
