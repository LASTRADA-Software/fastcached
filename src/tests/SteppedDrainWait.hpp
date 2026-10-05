// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/BoundedDrain.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <utility>

#include <core/platform/Clock.hpp>

namespace FastCache::Testing
{

/// A drain wait that advances its own clock by what was asked, never blocks, and runs @p onSleep
/// at each poll -- so a bounded wait ends at its bound at once, a case reads how long it WOULD have
/// taken, and a case can play the heartbeat thread acting while a suspend is being waited out.
///
/// A shared fake, for `ScriptedSocket.hpp`'s reason: a copy per file drifts in silence. Without a
/// hook it is the plain instant wait an enrollment poll's cases want; with one, the host-event cases
/// play the heartbeat thread inside a suspend's wait.
///
/// Safe to share between threads, as a service host shares its wait between the start's reporter
/// and the stop's: the clock and the count are atomics, and the hook runs on whichever thread slept.
class SteppedDrainWait final: public IDrainWait
{
  public:
    /// @param onSleep Run after each poll's time has passed; empty for none.
    explicit SteppedDrainWait(std::function<void()> onSleep = {}):
        _onSleep { std::move(onSleep) }
    {
    }

    /// @return The accumulated instant: every requested pause, and nothing else.
    [[nodiscard]] core::platform::SteadyTimePoint Now() const noexcept override
    {
        return core::platform::SteadyTimePoint {} + Duration { _ticks.load(std::memory_order_acquire) };
    }

    /// Add @p requested to the clock rather than sleeping, count the poll, then run the hook.
    /// @param requested The pause the caller asked for.
    void Sleep(std::chrono::milliseconds requested) noexcept override
    {
        _ticks.fetch_add(std::chrono::duration_cast<Duration>(requested).count(), std::memory_order_acq_rel);
        _sleeps.fetch_add(1, std::memory_order_acq_rel);
        if (_onSleep)
            _onSleep();
    }

    /// @return How much time the waits spent, by this clock.
    [[nodiscard]] std::chrono::milliseconds Elapsed() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Now() - core::platform::SteadyTimePoint {});
    }

    /// @return How many polls ran.
    [[nodiscard]] int Sleeps() const noexcept
    {
        return _sleeps.load(std::memory_order_acquire);
    }

  private:
    using Duration = core::platform::SteadyTimePoint::duration; ///< The clock's own unit.

    std::function<void()> _onSleep;          ///< Run after each poll; empty for none.
    std::atomic<Duration::rep> _ticks { 0 }; ///< The clock `Now` reads, in `Duration` ticks.
    std::atomic<int> _sleeps { 0 };          ///< Polls so far.
};

} // namespace FastCache::Testing
