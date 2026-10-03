// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/StopAwareWait.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stop_token>

#include <core/platform/Clock.hpp>

/// @file ManualClockWait.hpp
/// A stop-aware wait whose interval passes on a `ManualClock`: a thread that paces itself through it
/// moves exactly when a case advances the clock, and never on the wall's time.

namespace FastCache::Testing
{

/// `IStopAwareWait` over a `ManualClock` it OWNS. A wait ends once the clock reads its deadline, which
/// only `Advance` moves, or at a stop request.
///
/// The clock is this object's rather than a reference to the case's, and read-only (`Clock`): a paced
/// runtime reads its time FROM the wait it sleeps through (`IStopAwareWait::Clock`), so the two cannot
/// disagree, and `Advance` -- which wakes every wait -- is the only way that time moves. A rig holding
/// the clock itself could advance it without waking the beat.
class ManualClockWait final: public IStopAwareWait
{
  public:
    /// @copydoc IStopAwareWait::Clock
    ///
    /// Read-only: move it only through `Advance`, which wakes every wait.
    [[nodiscard]] core::platform::IClock const& Clock() const noexcept override
    {
        return _clock;
    }

    /// @copydoc IStopAwareWait::WaitFor
    [[nodiscard]] WaitEnd WaitFor(std::stop_token const& stop, std::chrono::milliseconds interval) const override
    {
        auto lock = std::unique_lock { _mutex };
        auto const deadline = _clock.now() + interval;
        ++_waits;
        _wake.notify_all();
        auto const elapsed = _wake.wait(lock, stop, [&] { return _clock.now() >= deadline; });
        return elapsed ? WaitEnd::Elapsed : WaitEnd::Stopped;
    }

    /// Move the clock on by @p delta and wake every wait, so each re-reads its deadline.
    /// @param delta How far.
    void Advance(core::platform::SteadyDuration delta)
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _clock.advance(delta);
        }
        _wake.notify_all();
    }

    /// @return How many waits have BEGUN: a paced thread's count of the intervals it entered.
    [[nodiscard]] std::size_t Waits() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _waits;
    }

  private:
    core::platform::ManualClock _clock;        ///< What deadlines are read from; see `Clock`.
    mutable std::mutex _mutex;                 ///< Guards the count and orders the wake.
    mutable std::condition_variable_any _wake; ///< Woken by every `Advance` and every new wait.
    mutable std::size_t _waits { 0 };          ///< See `Waits`.
};

} // namespace FastCache::Testing
