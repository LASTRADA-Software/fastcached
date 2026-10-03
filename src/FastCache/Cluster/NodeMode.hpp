// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

/// @file NodeMode.hpp
/// What a node IS, as one table: the mode its cluster dir records, and every consequence of that
/// mode as a column.
///
/// A mode is read from the state a node keeps, never inferred from the flags it was started with,
/// and whatever depends on it -- which consensus runs, whether the Raft surface is bound, what the
/// beacon says, whether the scheduler is served, which way a Raft session is dialled -- reads its
/// column here rather than switching on the mode. A mode added tomorrow is one row, and a column
/// added tomorrow fails the build at every row that does not state it.
namespace FastCache::Cluster
{

/// What a node is, read from its cluster dir.
///
/// **PERSISTED**: the value is the formation record's mode byte, so the values are explicit, the
/// list is append-only and a retired value is never reused. There is no principal mode: every
/// machine that joins a fleet joins it as a learner member.
enum class NodeMode : std::uint8_t
{
    Solitary = 0x01, ///< Its own one-voter cluster, minted into its cluster dir.
    Pending = 0x02,  ///< Still its solitary cluster, with a request to join a fleet recorded.
    Learner = 0x03,  ///< A learner of the fleet it joined; it dials the leader.
    Voter = 0x04,    ///< A voter of its fleet: promoted by an operator, or the fleet's founder.
};

/// Every mode this build implements, as ONE list, for `KnownEnrollRoles`' reason: a persisted
/// enum carries no `Last`, so the record's decoder refuses a byte outside this list and the table
/// below is asserted complete against it.
inline constexpr std::array KnownNodeModes { NodeMode::Solitary, NodeMode::Pending, NodeMode::Learner, NodeMode::Voter };

/// Which cluster a mode's consensus runs. Every mode runs one.
///
/// **Private**: never transmitted or persisted.
enum class ConsensusScope : std::uint8_t
{
    OwnCluster, ///< The solitary cluster this node minted.
    Fleet,      ///< The fleet this node joined, or founded.
};

/// Whether a mode binds the Raft surface.
///
/// **Private**: never transmitted or persisted.
enum class RaftListenerState : std::uint8_t
{
    Open,   ///< Bound: voters dial it.
    Closed, ///< Not bound: this node dials the leader instead.
};

/// Whether a mode serves the scheduler, which answers `Lease` only while this node leads.
///
/// **Private**: never transmitted or persisted.
enum class SchedulerDuty : std::uint8_t
{
    Serves, ///< The scheduler tier exists here.
    None,   ///< No scheduler tier.
};

/// Whether a mode's members are, or are about to be, machines other than this one.
///
/// What decides whether a worker there must be able to check a lease: a node other machines are
/// members of admits them, so its compile surface faces them. It errs towards `Beyond` -- the
/// fail-closed direction, a refusal of a worker that holds no roster -- for every mode but one
/// that is this machine alone by definition.
///
/// **Private**: never transmitted or persisted.
enum class FleetReach : std::uint8_t
{
    ThisMachine, ///< This machine alone: its own cluster, with nobody asked to join it.
    Beyond,      ///< Other machines are members, or about to be.
};

/// What one mode means.
struct NodeModeRow
{
    NodeMode mode;                               ///< The mode.
    std::string_view name;                       ///< One spelling: --print-surfaces, --node-status, logs.
    ConsensusScope consensus;                    ///< Which cluster its consensus runs.
    RaftListenerState raftListener;              ///< Whether the Raft surface is bound.
    CompileCacheWire::FleetState announces;      ///< What its beacon and FleetSummary say.
    SchedulerDuty scheduler;                     ///< Whether it serves the scheduler.
    Consensus::RaftWire::SessionDirection dials; ///< How it dials voters: TwoWay for a learner.
    FleetReach members;                          ///< Whether other machines are, or are about to be, members.
};

/// One row per mode this build implements.
///
/// A plain array rather than an `EnumTable`, for `EnrollRoleTable`'s reason: a persisted enum
/// carries no `Last`. Completeness is asserted against `KnownNodeModes` instead.
///
/// A pending node is still its solitary cluster and keeps serving exactly as one, so its row
/// differs from `Solitary`'s in its name, in `members` and in what it announces: it has asked a
/// fleet to take it, so other machines are about to be its members -- the strongest signal there is
/// that remote ones are coming -- and its summary points at that fleet (`FleetState::Pending`), so a
/// node meeting it joins the fleet it asked rather than a cluster about to be left.
///
/// A learner's Raft surface is closed because the leader never dials a learner: the learner dials
/// in, and the session carries both ways. A voter serves the scheduler, which answers `Lease` only
/// while this node leads; a fleet's founder is its first voter and its first leader.
inline constexpr std::array NodeModeTable {
    NodeModeRow { .mode = NodeMode::Solitary,
                  .name = "solitary",
                  .consensus = ConsensusScope::OwnCluster,
                  .raftListener = RaftListenerState::Open,
                  .announces = CompileCacheWire::FleetState::Solitary,
                  .scheduler = SchedulerDuty::Serves,
                  .dials = Consensus::RaftWire::SessionDirection::OneWay,
                  .members = FleetReach::ThisMachine },
    NodeModeRow { .mode = NodeMode::Pending,
                  .name = "pending",
                  .consensus = ConsensusScope::OwnCluster,
                  .raftListener = RaftListenerState::Open,
                  .announces = CompileCacheWire::FleetState::Pending,
                  .scheduler = SchedulerDuty::Serves,
                  .dials = Consensus::RaftWire::SessionDirection::OneWay,
                  .members = FleetReach::Beyond },
    NodeModeRow { .mode = NodeMode::Learner,
                  .name = "learner",
                  .consensus = ConsensusScope::Fleet,
                  .raftListener = RaftListenerState::Closed,
                  .announces = CompileCacheWire::FleetState::Established,
                  .scheduler = SchedulerDuty::None,
                  .dials = Consensus::RaftWire::SessionDirection::TwoWay,
                  .members = FleetReach::Beyond },
    NodeModeRow { .mode = NodeMode::Voter,
                  .name = "voter",
                  .consensus = ConsensusScope::Fleet,
                  .raftListener = RaftListenerState::Open,
                  .announces = CompileCacheWire::FleetState::Established,
                  .scheduler = SchedulerDuty::Serves,
                  .dials = Consensus::RaftWire::SessionDirection::OneWay,
                  .members = FleetReach::Beyond },
};

/// Whether every mode this build knows has exactly one row, and no row names a mode it does not.
/// @return True when `NodeModeTable` is complete and has no duplicate.
[[nodiscard]] consteval bool EveryNodeModeHasARow() noexcept
{
    return NodeModeTable.size() == KnownNodeModes.size() && std::ranges::all_of(KnownNodeModes, [](NodeMode mode) {
               return std::ranges::count(NodeModeTable, mode, &NodeModeRow::mode) == 1;
           });
}

static_assert(EveryNodeModeHasARow(), "every NodeMode needs exactly one NodeModeTable row");

/// Whether every row's name is its own. The name is the one spelling a mode has in a status line
/// and a log, so two modes sharing it would read as one.
/// @return True when no two rows share a name.
[[nodiscard]] consteval bool EveryNodeModeNameIsUnique() noexcept
{
    return std::ranges::all_of(NodeModeTable, [](NodeModeRow const& row) {
        return std::ranges::count(NodeModeTable, row.name, &NodeModeRow::name) == 1;
    });
}

static_assert(EveryNodeModeNameIsUnique(), "every NodeModeTable row needs a name of its own");

/// The row for @p mode.
/// @param mode A mode the record's decoder accepted.
/// @return Its row; the first row for a value no row names, which the assertion above makes unreachable.
[[nodiscard]] constexpr NodeModeRow const& NodeModeRowFor(NodeMode mode) noexcept
{
    for (auto const& row: NodeModeTable)
        if (row.mode == mode)
            return row;
    return NodeModeTable.front();
}

} // namespace FastCache::Cluster
