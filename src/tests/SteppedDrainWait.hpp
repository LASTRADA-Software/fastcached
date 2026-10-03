// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/BoundedDrain.hpp>

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
        return _now;
    }

    /// Add @p requested to the clock rather than sleeping, count the poll, then run the hook.
    /// @param requested The pause the caller asked for.
    void Sleep(std::chrono::milliseconds requested) noexcept override
    {
        _now += requested;
        ++_sleeps;
        if (_onSleep)
            _onSleep();
    }

    /// @return How much time the waits spent, by this clock.
    [[nodiscard]] std::chrono::milliseconds Elapsed() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(_now - core::platform::SteadyTimePoint {});
    }

    /// @return How many polls ran.
    [[nodiscard]] int Sleeps() const noexcept
    {
        return _sleeps;
    }

  private:
    std::function<void()> _onSleep;          ///< Run after each poll; empty for none.
    core::platform::SteadyTimePoint _now {}; ///< The clock `Now` reads.
    int _sleeps { 0 };                       ///< Polls so far.
};

} // namespace FastCache::Testing
