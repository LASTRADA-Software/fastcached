// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace FastCache::Distributed
{

/// How hard a machine may be driven.
///
/// Zero is `Workstation`, and that is the safety property rather than an ordering
/// convenience: a node whose class nobody set is somebody's desktop until proven
/// otherwise. Getting this backwards means a developer's machine is saturated by
/// other people's builds because an operator forgot a flag — a failure they
/// experience as "my editor stutters" and never connect to a build fleet.
enum class NodeClass : std::uint8_t
{
    /// Somebody works at this machine. Capacity is left free for them.
    Workstation = 0,
    /// Nobody works at this machine. It may be driven to its slot limit.
    Dedicated,
};

/// One row per class: what it is called, and how much of the machine is not ours.
///
/// A table rather than an `if`, because a third class is foreseeable — a laptop on
/// battery, a CI runner that may be driven hard but not swapped — and each would
/// otherwise be another branch threaded through the scheduler.
struct NodeClassTraits
{
    NodeClass nodeClass;         ///< The class this row describes.
    std::string_view name;       ///< Spelling accepted on the command line.
    std::uint32_t reservedCores; ///< Cores never offered to the fleet.
};

/// Every class, in enumerator order.
///
/// The workstation reserve is **2** rather than 1. One core free is enough for a
/// scheduler to keep running and not enough for a person: a modern editor, its
/// language server and a browser will each want one, and the whole point of the
/// reserve is that the machine stays usable while it contributes. It is a default
/// rather than a rule -- `--reserve-cores` overrides it -- but a default nobody
/// changes is the one most people run.
inline constexpr std::array<NodeClassTraits, 2> NodeClassTable {
    NodeClassTraits { .nodeClass = NodeClass::Workstation, .name = "workstation", .reservedCores = 2 },
    NodeClassTraits { .nodeClass = NodeClass::Dedicated, .name = "dedicated", .reservedCores = 0 },
};

/// Whether `NodeClassTable` has exactly one row per enumerator, in order.
///
/// The completeness check the table exists to make possible: a class added to the
/// enum without a row is a build failure rather than a machine that is silently
/// driven by whichever row happened to sort first.
/// @return True when the table covers the enum in order.
[[nodiscard]] consteval bool NodeClassTableIsComplete() noexcept
{
    for (auto index = std::size_t { 0 }; index < NodeClassTable.size(); ++index)
        if (static_cast<std::size_t>(NodeClassTable[index].nodeClass) != index)
            return false;
    return true;
}

static_assert(NodeClassTableIsComplete(), "NodeClassTable must hold one row per NodeClass, in enumerator order");

/// Look a class up by the name an operator spelled.
/// @param name The spelling from the command line.
/// @return The class, or nullopt when the name is not one of them.
[[nodiscard]] constexpr std::optional<NodeClass> NodeClassByName(std::string_view name) noexcept
{
    for (auto const& row: NodeClassTable)
        if (row.name == name)
            return row.nodeClass;
    return std::nullopt;
}

/// The traits for a class.
/// @param nodeClass The class.
/// @return Its row.
[[nodiscard]] constexpr NodeClassTraits const& TraitsFor(NodeClass nodeClass) noexcept
{
    return NodeClassTable[static_cast<std::size_t>(nodeClass)];
}

/// What a machine is, as far as scheduling onto it is concerned.
///
/// Deliberately not `Platform::HostFacts`: that describes what a machine *is* — its
/// OS, its architecture, the facts an operator reads — while this is what a
/// scheduler *weighs*. They are collected together and travel together, but a
/// scheduler that took the whole of `HostFacts` would be coupled to every field
/// somebody adds to it for a diagnostic.
struct NodeCapacity
{
    /// Hardware threads the machine has. Zero means "did not say", which is
    /// treated as one core rather than as none — see `OfferableSlots`.
    std::uint32_t logicalCores { 0 };
    /// Physical memory, in bytes. Zero means "did not say".
    std::uint64_t totalMemoryBytes { 0 };
    /// Free space on the scratch filesystem, in bytes. Zero means "did not say".
    std::uint64_t freeDiskBytes { 0 };
    /// How hard this machine may be driven.
    NodeClass nodeClass { NodeClass::Workstation };
    /// Cores held back from the fleet. Defaults to the class's reserve; an
    /// operator may raise or lower it.
    std::uint32_t reservedCores { 0 };
    /// Whether `reservedCores` was set explicitly, or should follow the class.
    bool reserveIsExplicit { false };
};

/// How many concurrent jobs this machine should be offered.
///
/// The workstation reserve applied, and clamped so a machine always offers at least
/// one: a node that offered zero would register, heartbeat, never be picked, and be
/// indistinguishable at the client from a fleet that is permanently busy — the
/// failure `WorkerRegistration`'s own zero-slot refusal exists to prevent, arriving
/// here by arithmetic instead of by configuration.
///
/// A machine that reported no core count is treated as having one. Refusing to
/// schedule onto it would punish a worker for a fact it merely failed to collect,
/// and one slot is the answer that is never wrong by much.
/// @param capacity What the machine says about itself.
/// @param advertisedSlots What the worker asked to be given, or 0 to derive it.
/// @return Concurrent jobs to offer; never zero.
[[nodiscard]] constexpr std::uint32_t OfferableSlots(NodeCapacity const& capacity, std::uint32_t advertisedSlots) noexcept
{
    auto const cores = capacity.logicalCores == 0 ? 1U : capacity.logicalCores;
    auto const ceiling = advertisedSlots == 0 ? cores : std::min(advertisedSlots, cores);
    auto const reserve = capacity.reserveIsExplicit ? capacity.reservedCores : TraitsFor(capacity.nodeClass).reservedCores;

    // Saturating rather than wrapping: a two-core workstation reserving two cores
    // must offer one, not 4294967295.
    return reserve >= ceiling ? 1U : ceiling - reserve;
}

} // namespace FastCache::Distributed
