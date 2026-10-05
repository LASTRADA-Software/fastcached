// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/BoundedDrain.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>

namespace FastCache
{

/// What a service reports to its supervisor while it stops.
///
/// The SCM reads `dwWaitHint` as "the next checkpoint arrives within this", and a stop that
/// outlives it without a checkpoint reads as a hung service -- to `sc stop`, to `Stop-Service`
/// and to the MSI's ServiceControl wait alike. So the hint is derived from how long the body
/// may legitimately take, and a checkpoint advances while it does.
struct StopPendingPlan
{
    std::chrono::milliseconds waitHint;        ///< What `SERVICE_STOP_PENDING` states.
    std::chrono::milliseconds checkpointEvery; ///< How often the checkpoint advances.
};

/// What a stop costs after any drain: listeners closed, tiers flushed, the last line logged.
inline constexpr std::chrono::milliseconds StopTeardownMargin { 10'000 };

/// How often a stopping service proves it is still stopping.
inline constexpr std::chrono::milliseconds StopCheckpointInterval { 1'000 };

/// The largest wait hint `SERVICE_STOP_PENDING` can carry: `DWORD` milliseconds, ~49.7 days.
/// `--drain-timeout` takes whatever `std::chrono::seconds` holds, with no upper bound of its own,
/// so a hint built past this and narrowed to a `DWORD` wraps to a small, wrong value instead of
/// reporting a long wait -- `StopPendingPlanFor` clamps to it before any caller narrows the count.
inline constexpr std::chrono::milliseconds MaxServiceWaitHint { std::numeric_limits<std::uint32_t>::max() };

/// The plan for a body that drains for @p drainTimeout before it tears down.
/// @param drainTimeout The drain bound; nullopt for a binary with no drain of its own; zero for
///        an unbounded drain, which the checkpoints then carry.
/// @return The plan. `waitHint` is clamped to `MaxServiceWaitHint`.
[[nodiscard]] StopPendingPlan StopPendingPlanFor(std::optional<std::chrono::seconds> drainTimeout) noexcept;

/// Report an advancing checkpoint every `plan.checkpointEvery` until @p stopped answers true.
/// @param plan What to report and how often.
/// @param stopped Whether the body has returned; asked before every report.
/// @param report Receives the checkpoint (1, 2, ...) and the hint.
/// @param wait Where the gap between two reports is spent.
/// @return How many checkpoints were reported.
std::uint32_t ReportStopProgress(
    StopPendingPlan const& plan,
    std::function<bool()> const& stopped,
    std::function<void(std::uint32_t checkPoint, std::chrono::milliseconds waitHint)> const& report,
    IDrainWait& wait);

} // namespace FastCache
