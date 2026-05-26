// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <mutex>

namespace FastCache
{

/// Steady-clock time point used throughout FastCache for TTLs, deadlines, and
/// reactor schedule deadlines. Wall-clock time is never used internally so
/// system clock skew cannot mutate cache semantics.
using TimePoint = std::chrono::steady_clock::time_point;

/// Duration alias matching TimePoint's clock.
using Duration = std::chrono::steady_clock::duration;

/// Time provider abstraction. Production code uses SteadyClock; tests use
/// ManualClock so TTL/timeout behaviour is deterministic. Every caller that
/// needs "now" — the reactor, the cache engine, the disk-storage compactor,
/// the logger timestamps — takes an IClock by reference.
class IClock
{
  public:
    IClock() = default;
    IClock(IClock const&) = delete;
    IClock(IClock&&) = delete;
    IClock& operator=(IClock const&) = delete;
    IClock& operator=(IClock&&) = delete;
    virtual ~IClock() = default;

    /// @return Current steady-clock time. Must be monotonic and thread-safe.
    [[nodiscard]] virtual TimePoint Now() const noexcept = 0;
};

/// Default IClock implementation wrapping std::chrono::steady_clock::now().
class SteadyClock final: public IClock
{
  public:
    [[nodiscard]] TimePoint Now() const noexcept override
    {
        return std::chrono::steady_clock::now();
    }
};

/// Test IClock whose value only advances when a test explicitly calls Advance
/// or SetNow. Thread-safe so reactor-driven tests with multiple coroutines
/// share one ManualClock without UB.
class ManualClock final: public IClock
{
  public:
    /// Construct with the given starting time.
    /// @param start Initial value returned by Now().
    explicit ManualClock(TimePoint start = TimePoint {}) noexcept: _now { start } {}

    [[nodiscard]] TimePoint Now() const noexcept override
    {
        std::lock_guard const lock { _mutex };
        return _now;
    }

    /// Move the clock forward by the given duration.
    /// @param delta Non-negative duration to advance the clock by.
    void Advance(Duration delta) noexcept
    {
        std::lock_guard const lock { _mutex };
        _now += delta;
    }

    /// Hard-set the clock to a specific value.
    /// @param when New value Now() will return.
    void SetNow(TimePoint when) noexcept
    {
        std::lock_guard const lock { _mutex };
        _now = when;
    }

  private:
    mutable std::mutex _mutex;
    TimePoint _now;
};

} // namespace FastCache
