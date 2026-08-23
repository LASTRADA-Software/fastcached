// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Distributed/NodePolicy.hpp>

#include <cstdint>
#include <optional>

namespace FastCache::Distributed::Testing
{

/// Builders for `NodeLoad`, one per fact a case is about.
///
/// Every field of `NodeLoad` is optional by design, and a designated initializer
/// that names only some of them is a **build failure** under
/// `-Wmissing-designated-field-initializers` — which is the right rule, since a
/// field added to the middle of a struct would otherwise silently default at every
/// site. What it costs a test file is four designators at every call, three of them
/// `std::nullopt`, burying the single value the case exists to vary.
///
/// So each builder names the one fact its case is about and fills the rest in.
/// Shared between `NodePolicy_test` and `WorkerRegistry_test` rather than copied
/// into both: two near-identical copies that drift is the defect this codebase
/// keeps a list about, and `Unwrap` is the precedent for one shared helper over
/// eleven private ones.
///
/// `constexpr` throughout, because the policy cases assert against it at compile
/// time.

/// A worker running jobs and reporting nothing else about its machine.
/// @param inFlight Jobs running.
/// @return The load report.
[[nodiscard]] constexpr NodeLoad Busy(std::uint32_t inFlight) noexcept
{
    return NodeLoad { .inFlight = inFlight,
                      .cpuBusyPermille = std::nullopt,
                      .availableMemoryBytes = std::nullopt,
                      .freeScratchBytes = std::nullopt };
}

/// A worker reporting its host CPU.
/// @param inFlight Jobs running.
/// @param permille Host-wide CPU busy, 0..1000.
/// @return The load report.
[[nodiscard]] constexpr NodeLoad WithCpu(std::uint32_t inFlight, std::uint32_t permille) noexcept
{
    return NodeLoad { .inFlight = inFlight,
                      .cpuBusyPermille = permille,
                      .availableMemoryBytes = std::nullopt,
                      .freeScratchBytes = std::nullopt };
}

/// A worker reporting room on its scratch filesystem.
/// @param inFlight Jobs running.
/// @param bytes Free bytes.
/// @return The load report.
[[nodiscard]] constexpr NodeLoad WithScratch(std::uint32_t inFlight, std::uint64_t bytes) noexcept
{
    return NodeLoad { .inFlight = inFlight,
                      .cpuBusyPermille = std::nullopt,
                      .availableMemoryBytes = std::nullopt,
                      .freeScratchBytes = bytes };
}

/// A worker reporting available memory.
/// @param inFlight Jobs running.
/// @param bytes Bytes a new job could get.
/// @return The load report.
[[nodiscard]] constexpr NodeLoad WithMemory(std::uint32_t inFlight, std::uint64_t bytes) noexcept
{
    return NodeLoad { .inFlight = inFlight,
                      .cpuBusyPermille = std::nullopt,
                      .availableMemoryBytes = bytes,
                      .freeScratchBytes = std::nullopt };
}

} // namespace FastCache::Distributed::Testing
