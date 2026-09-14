// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Distributed/NodePolicy.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <cstdint>
#include <optional>

namespace FastCache::Cli
{

/// @file NodeSlots.hpp
/// What a node's compile slots are doing, worked out from two readings the way the scheduler
/// works it out from a heartbeat -- so the `node` panel names the limit and the remedy the fleet
/// is acting on, rather than a second opinion about them.

/// A node's slots at one reading.
struct NodeSlots
{
    std::uint32_t inFlight { 0 };   ///< Compiles running at the newer reading.
    std::uint32_t registered { 0 }; ///< What the node offers the scheduler (`OfferableSlots`).

    /// Every ceiling and the one that binds, or absent while the panel cannot yet know them.
    ///
    /// Absent is not `Registered`, and that distinction is the reason this is optional: a node
    /// whose CPU the platform reports and which has not been read TWICE yet has a CPU ceiling
    /// nobody can compute, and stating `registered` for it would tell an operator nothing holds
    /// the machine back at exactly the moment that is unknown. Likewise for a reading carrying
    /// no load at all.
    std::optional<Distributed::SlotCeilings> ceilings {};

    /// Host-wide CPU busy between the two readings, or absent when it cannot be taken.
    std::optional<std::uint32_t> cpuBusyPermille {};
    /// Memory a new process could obtain at the newer reading, or absent when not reported.
    std::optional<std::uint64_t> availableMemoryBytes {};
};

/// The node's slots at @p now, differencing its CPU against @p previous.
///
/// CPU is a difference between two ADJACENT readings: @p previous is the reading immediately
/// before @p now, or null when there was none -- a session's first reading, or a gap. A rate is
/// never taken across a gap, so a caller does not skip back to an older reading.
///
/// A figure the platform does not report stays absent and the ceilings are still computed,
/// because that is what the scheduler does with the same absence (`SlotCeilingsFor` schedules
/// on the other properties). A figure the platform DOES report but that cannot be taken yet --
/// CPU with no adjacent reading, or a pair `CpuBusyPermille` declines -- leaves the ceilings
/// absent instead, since the scheduler has a value there and the panel does not.
/// @param previous The adjacent earlier reading, or null.
/// @param now The newer reading.
/// @return The slots, or nullopt when @p now carries no host at all (the daemon).
[[nodiscard]] std::optional<NodeSlots> NodeSlotsOf(StatsReading const* previous, StatsReading const& now) noexcept;

} // namespace FastCache::Cli
