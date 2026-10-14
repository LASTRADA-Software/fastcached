// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/BoundedDrain.hpp>

#include <chrono>
#include <cstddef>

#include <core/platform/Clock.hpp>

namespace FastCache::Testing
{

/// An `IDrainWait` whose sleeps cost more than they asked for, by a factor the
/// test names. It never blocks: its clock advances by what each sleep cost.
///
/// This is the whole apparatus for testing a bounded wait. The defect
/// `DrainWithin` exists to prevent is a loop that accumulates the *requested*
/// poll and calls the sum "elapsed", and that loop is indistinguishable from a
/// correct one on any host where a sleep costs what it asked for. Windows' ~15 ms
/// timer granularity is what made the two shipped copies enforce 15 s and 7.5 s
/// against a stated 5 s; a fake that can overrun on demand reproduces it on every
/// platform, and deterministically.
///
/// A shared fake, for `ScriptedSocket.hpp`'s reason: a copy per file drifts in
/// silence. The factor is REQUIRED, so no case gets an overrun it did not ask
/// for. `ServiceControl_test.cpp`'s fixed-cost wait is a different fake: its
/// sleeps cost a constant, not a multiple of the request.
class OverrunningDrainWait final: public IDrainWait
{
  public:
    /// @param overrun How many times longer a sleep costs than it requested.
    explicit OverrunningDrainWait(int overrun) noexcept:
        _overrun { overrun }
    {
    }

    [[nodiscard]] core::platform::SteadyTimePoint Now() const noexcept override
    {
        return _now;
    }

    void Sleep(std::chrono::milliseconds requested) noexcept override
    {
        ++_sleeps;
        _now += requested * _overrun;
    }

    /// @return How many times `Sleep` was called.
    [[nodiscard]] std::size_t Sleeps() const noexcept
    {
        return _sleeps;
    }

    /// @return How much time this wait has handed out since construction.
    [[nodiscard]] std::chrono::milliseconds Elapsed() const noexcept
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(_now - core::platform::SteadyTimePoint {});
    }

  private:
    int _overrun;                            ///< How many times its request a sleep costs.
    std::size_t _sleeps { 0 };               ///< Sleeps so far.
    core::platform::SteadyTimePoint _now {}; ///< The clock `Now` reads.
};

} // namespace FastCache::Testing
