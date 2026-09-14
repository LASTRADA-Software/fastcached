// SPDX-License-Identifier: Apache-2.0
#include "NodeSlots.hpp"

#include <FastCache/Platform/HostLoad.hpp>

#include <algorithm>
#include <limits>

namespace FastCache::Cli
{

namespace
{
    /// @p value narrowed to the `u32` the scheduler's arithmetic takes, saturating.
    [[nodiscard]] std::uint32_t Saturated(std::uint64_t value) noexcept
    {
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
    }

    /// The CPU busy share between @p previous and @p now, and whether one was owed.
    struct CpuShare
    {
        std::optional<std::uint32_t> permille; ///< The share, when it could be taken.
        bool owed { false };                   ///< The platform reports CPU, so a share was expected.
    };

    [[nodiscard]] CpuShare CpuShareOf(StatsReading const* previous, HostLoadReading const& now) noexcept
    {
        if (!now.cpu.has_value())
            return CpuShare { .permille = std::nullopt, .owed = false };
        auto const* earlier =
            previous != nullptr && previous->snapshot.hostLoad.has_value() && previous->snapshot.hostLoad->cpu.has_value()
                ? &*previous->snapshot.hostLoad->cpu
                : nullptr;
        if (earlier == nullptr)
            return CpuShare { .permille = std::nullopt, .owed = true };
        return CpuShare { .permille = CpuBusyPermille(*earlier, *now.cpu), .owed = true };
    }
} // namespace

std::optional<NodeSlots> NodeSlotsOf(StatsReading const* previous, StatsReading const& now) noexcept
{
    if (!now.snapshot.host.has_value())
        return std::nullopt;
    auto const& host = *now.snapshot.host;

    auto slots = NodeSlots { .inFlight = Saturated(host.busySlots),
                             .registered = Saturated(host.configuredSlots),
                             .ceilings = std::nullopt,
                             .cpuBusyPermille = std::nullopt,
                             .availableMemoryBytes = std::nullopt };

    // A reading carrying no load says nothing the ceilings are made of, which is not the same as
    // a platform that will not say: the scheduler has figures here and the panel has none.
    if (!now.snapshot.hostLoad.has_value())
        return slots;
    auto const& load = *now.snapshot.hostLoad;

    auto const cpu = CpuShareOf(previous, load);
    slots.cpuBusyPermille = cpu.permille;
    slots.availableMemoryBytes = load.availableMemoryBytes;
    if (cpu.owed && !cpu.permille.has_value())
        return slots;

    // Exactly the scheduler's inputs: the cores the CPU ceiling scales by, the slots the node
    // registered with, and the load a heartbeat would carry. Free scratch is the host's figure,
    // which the node reads on its scratch filesystem.
    slots.ceilings = Distributed::SlotCeilingsFor(Distributed::NodeCapacity { .logicalCores = Saturated(host.logicalCores) },
                                                  slots.registered,
                                                  Distributed::NodeLoad { .inFlight = slots.inFlight,
                                                                          .cpuBusyPermille = cpu.permille,
                                                                          .availableMemoryBytes = load.availableMemoryBytes,
                                                                          .freeScratchBytes = host.diskFreeBytes,
                                                                          .cache = {} });
    return slots;
}

} // namespace FastCache::Cli
