// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/StopPending.hpp>

#include <algorithm>

namespace FastCache
{

namespace
{

    /// @p drain narrowed to milliseconds and clamped to `MaxServiceWaitHint`, without ever
    /// multiplying a @p drain large enough to overflow.
    ///
    /// `--drain-timeout` takes whatever `std::chrono::seconds` holds, `seconds::max()` included,
    /// and `std::chrono::milliseconds{drain}` computes `drain.count() * 1000` -- which overflows
    /// the representation long before the result could be compared against anything. So the
    /// clamp has to run in SECONDS, before that multiplication, rather than after it on a value
    /// that may already be wrong.
    [[nodiscard]] constexpr std::chrono::milliseconds ClampedDrainMillis(std::chrono::seconds drain) noexcept
    {
        constexpr auto MaxDrainSeconds = std::chrono::duration_cast<std::chrono::seconds>(MaxServiceWaitHint);
        if (drain >= MaxDrainSeconds)
            return MaxServiceWaitHint;
        return std::chrono::duration_cast<std::chrono::milliseconds>(drain);
    }

} // namespace

StopPendingPlan StopPendingPlanFor(std::optional<std::chrono::seconds> drainTimeout) noexcept
{
    auto const drain = drainTimeout.value_or(std::chrono::seconds { 0 });
    auto const hint = std::min(ClampedDrainMillis(drain) + StopTeardownMargin, MaxServiceWaitHint);
    return StopPendingPlan { .waitHint = hint, .checkpointEvery = StopCheckpointInterval };
}

std::uint32_t ReportStopProgress(
    StopPendingPlan const& plan,
    std::function<bool()> const& stopped,
    std::function<void(std::uint32_t checkPoint, std::chrono::milliseconds waitHint)> const& report,
    IDrainWait& wait)
{
    std::uint32_t checkPoint = 0;
    while (!stopped())
    {
        ++checkPoint;
        report(checkPoint, plan.waitHint);
        wait.Sleep(plan.checkpointEvery);
    }
    return checkPoint;
}

} // namespace FastCache
