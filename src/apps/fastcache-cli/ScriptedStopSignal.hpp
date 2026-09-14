// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/AsyncQueue.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Platform/StopSignal.hpp>

namespace FastCache::Cli::Testing
{

/// A stop signal a case fires by hand.
///
/// **It parks.** A wait with nothing fired suspends on a queue that resumes it through the
/// reactor, so a case PLACES the stop between two other events rather than having it
/// answered inline -- the rule `ScriptedDashboardEvents` states for the same reason. The
/// `waiter` a caller names is not used: there is no blocking wait to run anywhere, and the
/// reactor stands in for both hops.
///
/// Each answer is taken by one wait, which is how a session uses it: one watch, ending
/// once. The production signal is sticky across waits; nothing here needs that.
class ScriptedStopSignal final: public IStopSignal
{
  public:
    /// @param reactor Where a parked wait is resumed.
    /// @param released Set when this signal is destroyed, which is when production puts the
    ///        previous disposition back; null when the case does not ask.
    explicit ScriptedStopSignal(IReactor& reactor, bool* released = nullptr):
        _wakes { reactor, AsyncQueueOptions {} },
        _released { released }
    {
    }

    ScriptedStopSignal(ScriptedStopSignal const&) = delete;
    ScriptedStopSignal(ScriptedStopSignal&&) = delete;
    ScriptedStopSignal& operator=(ScriptedStopSignal const&) = delete;
    ScriptedStopSignal& operator=(ScriptedStopSignal&&) = delete;

    ~ScriptedStopSignal() override
    {
        if (_released != nullptr)
            *_released = true;
    }

    /// The operator asks to stop.
    void Fire()
    {
        (void) _wakes.Push(StopWake::Stopped);
    }

    /// The wait itself fails.
    void Fail()
    {
        (void) _wakes.Push(StopWake::Failed);
    }

    [[nodiscard]] Task<StopWake> Stopped(IExecutor* waiter, IExecutor* resumeOn) override
    {
        static_cast<void>(waiter);
        static_cast<void>(resumeOn);
        ++_waits;
        auto const wake = co_await _wakes.Pop();
        co_return wake.value_or(StopWake::Cancelled);
    }

    void Cancel() noexcept override
    {
        _wakes.Close();
    }

    /// How many waits were started.
    /// @return The count.
    [[nodiscard]] int Waits() const noexcept
    {
        return _waits;
    }

  private:
    AsyncQueue<StopWake> _wakes;
    bool* _released;
    int _waits { 0 };
};

} // namespace FastCache::Cli::Testing
