// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftTypes.hpp>
#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

// What a node's STANDING in a configuration allows it: `StandingTable`, and the two private enums
// its columns take. A header of its own rather than part of `RaftNode.hpp`, because the columns
// are read outside the state machine -- `Cluster::LinkOfSeat` asks how a seat is reached -- and a
// reader asking one table should not have to compile the state machine, its log and its storage
// seam to do it.

namespace FastCache::Consensus
{

/// Which timer governs a node.
///
/// **Private: never transmitted or persisted**, so the enumerators carry no values.
enum class TimerKind : std::uint8_t
{
    Election,  ///< Time until this node stands for election.
    Heartbeat, ///< Time until this node next sends heartbeats.
    None,      ///< Nothing falls due: this node never stands (#1449).
};

/// How a member in a standing is reached by the others.
///
/// **Private: never transmitted or persisted**, so the enumerators carry no values. It is
/// `StandingTable`'s `link` column, and the one place that decides it: whether a member
/// must be recorded with an endpoint (`Cluster::SeatNeedsEndpoint`), whether the leader
/// may add it to the configuration without one, and whether a node reaches it by dialling
/// or waits for the session it opens.
enum class PeerLink : std::uint8_t
{
    Dialled,    ///< Every member dials it, so it must answer at an address every member can reach.
    DialsIn,    ///< It dials every voter two-way and is written to on that session; nobody dials it.
    NotReached, ///< Nobody reaches it: it is counted nowhere.
};

/// The per-standing facts the state machine reads rather than branches on.
struct StandingTraits
{
    Standing standing {};  ///< The standing this row describes.
    std::string_view name; ///< For log lines, test failure messages and a status report.

    /// What a follower or candidate in this standing waits on.
    ///
    /// `Election` for a node that stands, `None` for one that never does. It governs
    /// only the roles whose own timer is `Election`: a LEADER that has been demoted or
    /// removed is still owed its heartbeats until the change that did it commits, and
    /// only it can commit that change.
    TimerKind timer {};

    /// Whether it grants a vote or a pre-vote when asked.
    ///
    /// A refusal decided HERE is `VoteRefusal::CastsNoVote`, which is what "a learner
    /// refuses by row" means: the node answers, and the row is the reason.
    bool votes {};

    /// How the others reach a member in this standing.
    ///
    /// A learner is the laptop -- behind NAT, a VPN or a lid -- so an address for it would
    /// answer nothing: it `DialsIn`, and that is why it may be recorded with no endpoint at
    /// all. A voter is `Dialled` by every member. A node counted nowhere is not reached.
    PeerLink link {};
};

/// Behaviour that varies by standing, as data.
///
/// The learner row is the point (#1449), and `NoCluster` is its oldest instance: a node
/// waiting to be admitted was already a node that neither stands nor votes, spelled as
/// a special case in `NextDeadline` and as an accident of `IsMember` finding nobody. It
/// is the same two columns, so it is the same kind of row.
///
/// `Outsider` keeps what such a node always did -- it stands and it votes -- because
/// nothing here asked for that to change. It cannot win for itself (a node counts its
/// own vote only while it is a VOTER), and a voter it asks refuses it by the candidate's
/// own row, so its campaigning costs a message per timeout and decides nothing.
inline constexpr EnumTable<Standing, StandingTraits> StandingTable { {
    { .standing = Standing::NoCluster,
      .name = "no cluster",
      .timer = TimerKind::None,
      .votes = false,
      .link = PeerLink::NotReached },
    { .standing = Standing::Voter, .name = "voter", .timer = TimerKind::Election, .votes = true, .link = PeerLink::Dialled },
    { .standing = Standing::Learner,
      .name = "learner",
      .timer = TimerKind::None,
      .votes = false,
      .link = PeerLink::DialsIn },
    { .standing = Standing::Outsider,
      .name = "outsider",
      .timer = TimerKind::Election,
      .votes = true,
      .link = PeerLink::NotReached },
} };

static_assert(RowsInEnumeratorOrder(StandingTable, &StandingTraits::standing),
              "StandingTable must hold one row per Standing, in enumerator order");

static_assert(std::ranges::none_of(StandingTable,
                                   [](StandingTraits const& row) { return row.timer == TimerKind::Heartbeat; }),
              "a standing decides whether a node STANDS; heartbeats are a role's, and a standing that asked for "
              "them would have a follower broadcasting as though it led");

static_assert(std::ranges::all_of(StandingTable,
                                  [](StandingTraits const& row) { return row.link != PeerLink::DialsIn || !row.votes; }),
              "a standing reached only over the session it opens must not vote: a quorum counting it would wait on "
              "a machine nobody can dial, for as long as its lid is shut");

/// The row describing `standing`.
/// @param standing The standing to look up.
/// @return Its traits.
[[nodiscard]] constexpr StandingTraits const& TraitsOf(Standing standing) noexcept
{
    return StandingTable[static_cast<std::size_t>(standing)];
}

} // namespace FastCache::Consensus
