// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Protocol/ProvenIdentity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace FastCache::Distributed
{

/// @file MachineStanding.hpp
/// Where a machine stands in the roster a node holds: what `explain-admission <machine>` reports
/// beside the routes that admit it.

/// Where a machine stands in a roster.
///
/// **PRIVATE: persisted and transmitted nowhere.** It travels as `WireMachineStanding`, through
/// `MachineStandingWire`, so these ordinals carry no contract.
///
/// No `Principal`: principal mode is retired -- every joined machine is a learner member, and no
/// state records a principal.
enum class MachineStanding : std::uint8_t
{
    Voter,   ///< A member counted by every quorum.
    Learner, ///< A member replicated to and counted by no quorum.
    Pending, ///< Not recorded; this node's enrollment window holds a request under that id.
    Revoked, ///< Its key, or the only key its id was recorded under, is revoked.
    Unknown, ///< Nothing this node holds names it.
    Last,    ///< Enumerator count.
};

/// One row of `SeatStandings`.
struct SeatStandingRow
{
    Cluster::MemberSeat seat; ///< A member's seat.
    MachineStanding standing; ///< The standing that seat is.
};

/// The standing each seat is: a row per seat, so a seat added to `MemberSeat` without one fails the
/// build here rather than being reported as some other seat.
inline constexpr EnumTable<Cluster::MemberSeat, SeatStandingRow> SeatStandings { {
    { .seat = Cluster::MemberSeat::Voter, .standing = MachineStanding::Voter },
    { .seat = Cluster::MemberSeat::Learner, .standing = MachineStanding::Learner },
} };

static_assert(RowsInEnumeratorOrder(SeatStandings, &SeatStandingRow::seat),
              "SeatStandings must hold one row per MemberSeat, in enumerator order");

namespace Detail
{
    /// @param seat A member's seat.
    /// @return The standing it is.
    [[nodiscard]] constexpr MachineStanding StandingOfSeat(Cluster::MemberSeat seat) noexcept
    {
        return SeatStandings[static_cast<std::size_t>(seat)].standing;
    }

    /// The member @p subject names: by key when it parses as one, else by id.
    /// @param roster The roster.
    /// @param subject An id, or an identity key's text form.
    /// @param key The subject as a key, when it is one.
    /// @return The member, or null.
    [[nodiscard]] inline Cluster::RosterMember const* MemberNamed(Cluster::Roster const& roster,
                                                                  std::string_view subject,
                                                                  std::optional<Ed25519PublicKey> const& key)
    {
        auto const found = std::ranges::find_if(roster.members, [&](Cluster::RosterMember const& member) {
            return key.has_value() ? member.publicKey == *key : member.id == subject;
        });
        return found == roster.members.end() ? nullptr : &*found;
    }

    /// The revocation @p subject names: by key when it parses as one, else by the id it was
    /// recorded under.
    /// @param roster The roster.
    /// @param subject An id, or an identity key's text form.
    /// @param key The subject as a key, when it is one.
    /// @return The revocation, or null.
    [[nodiscard]] inline Cluster::RevokedKey const* RevocationNamed(Cluster::Roster const& roster,
                                                                    std::string_view subject,
                                                                    std::optional<Ed25519PublicKey> const& key)
    {
        auto const found = std::ranges::find_if(roster.revoked, [&](Cluster::RevokedKey const& revoked) {
            return key.has_value() ? revoked.publicKey == *key : revoked.id == subject;
        });
        return found == roster.revoked.end() ? nullptr : &*found;
    }

    /// @param subject An id, or an identity key's text form.
    /// @return The key, when the subject is one. `ParseEd25519PublicKey` decides: an id is never a
    ///         43-character base64url string that decodes to a key.
    [[nodiscard]] inline std::optional<Ed25519PublicKey> KeyNamed(std::string_view subject)
    {
        auto parsed = ParseEd25519PublicKey(subject);
        return parsed.has_value() ? std::optional { *parsed } : std::nullopt;
    }
} // namespace Detail

/// Where @p subject stands in @p roster.
///
/// A recorded id or key is its seat, whatever else is true -- an id admitted again under a new key
/// after a revocation is its seat, not revoked. A key or id found only among the revoked is
/// `Revoked`; an id only in the enrollment window is `Pending`; anything else is `Unknown`.
/// @param roster The roster this node holds.
/// @param subject An id, or an identity key's text form (`ParseEd25519PublicKey` decides which).
/// @param pending Whether this node's enrollment window holds a pending request under that id.
/// @return The standing.
[[nodiscard]] inline MachineStanding StandingOfMachine(Cluster::Roster const& roster, std::string_view subject, bool pending)
{
    auto const key = Detail::KeyNamed(subject);
    if (auto const* member = Detail::MemberNamed(roster, subject, key); member != nullptr)
        return Detail::StandingOfSeat(member->seat);
    if (Detail::RevocationNamed(roster, subject, key) != nullptr)
        return MachineStanding::Revoked;
    if (pending && !key.has_value())
        return MachineStanding::Pending;
    return MachineStanding::Unknown;
}

/// The key a machine question folds, so its routes are the ones the surfaces would enforce for a
/// connection presenting that key: the live key of a recorded id or key, the revoked key of a
/// revoked one, and nothing for anything else.
/// @param roster The roster this node holds.
/// @param subject An id, or an identity key's text form.
/// @return The id and key, or nullopt when the roster names neither.
[[nodiscard]] inline std::optional<ProvenIdentity> KeyOfMachine(Cluster::Roster const& roster, std::string_view subject)
{
    auto const key = Detail::KeyNamed(subject);
    if (auto const* member = Detail::MemberNamed(roster, subject, key); member != nullptr)
        return ProvenIdentity { .id = member->id, .key = member->publicKey };
    if (auto const* revoked = Detail::RevocationNamed(roster, subject, key); revoked != nullptr)
        return ProvenIdentity { .id = revoked->id, .key = revoked->publicKey };
    return std::nullopt;
}

} // namespace FastCache::Distributed
