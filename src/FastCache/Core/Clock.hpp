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
    explicit ManualClock(TimePoint start = TimePoint {}) noexcept:
        _now { start }
    {
    }

    [[nodiscard]] TimePoint Now() const noexcept override
    {
        std::scoped_lock const lock { _mutex };
        return _now;
    }

    /// Move the clock forward by the given duration.
    /// @param delta Non-negative duration to advance the clock by.
    void Advance(Duration delta) noexcept
    {
        std::scoped_lock const lock { _mutex };
        _now += delta;
    }

    /// Hard-set the clock to a specific value.
    /// @param when New value Now() will return.
    void SetNow(TimePoint when) noexcept
    {
        std::scoped_lock const lock { _mutex };
        _now = when;
    }

  private:
    mutable std::mutex _mutex;
    TimePoint _now;
};

/// Wall-clock provider abstraction — distinct from IClock because
/// fastcached's storage TTLs are anchored to `std::chrono::steady_clock`
/// (immune to system-clock skew, see the TimePoint alias above), but the
/// redis EXPIREAT/PEXPIREAT family carries an absolute UNIX timestamp
/// that the protocol layer must translate against the wall clock. Tests
/// use ManualWallClock for determinism; production uses SystemWallClock,
/// which delegates to std::chrono::system_clock::now().
class IWallClock
{
  public:
    IWallClock() = default;
    IWallClock(IWallClock const&) = delete;
    IWallClock(IWallClock&&) = delete;
    IWallClock& operator=(IWallClock const&) = delete;
    IWallClock& operator=(IWallClock&&) = delete;
    virtual ~IWallClock() = default;

    /// @return Current wall-clock time (system_clock). Need not be
    ///         monotonic and may jump under NTP adjustments.
    [[nodiscard]] virtual std::chrono::system_clock::time_point Now() const noexcept = 0;
};

/// Default IWallClock implementation wrapping
/// std::chrono::system_clock::now(). Production code injects this; tests
/// inject ManualWallClock.
class SystemWallClock final: public IWallClock
{
  public:
    [[nodiscard]] std::chrono::system_clock::time_point Now() const noexcept override
    {
        return std::chrono::system_clock::now();
    }
};

/// Test IWallClock with a manually-driven value. Mirrors ManualClock for
/// the steady seam: deterministic, thread-safe so reactor-driven tests
/// can share one across coroutines.
class ManualWallClock final: public IWallClock
{
  public:
    /// Construct with the given starting wall time.
    /// @param start Initial value returned by Now().
    explicit ManualWallClock(std::chrono::system_clock::time_point start = {}) noexcept:
        _now { start }
    {
    }

    [[nodiscard]] std::chrono::system_clock::time_point Now() const noexcept override
    {
        std::scoped_lock const lock { _mutex };
        return _now;
    }

    /// Move the wall clock forward by the given duration.
    /// @param delta Non-negative duration to advance the wall clock by.
    void Advance(std::chrono::system_clock::duration delta) noexcept
    {
        std::scoped_lock const lock { _mutex };
        _now += delta;
    }

    /// Hard-set the wall clock to a specific value.
    /// @param when New value Now() will return.
    void SetNow(std::chrono::system_clock::time_point when) noexcept
    {
        std::scoped_lock const lock { _mutex };
        _now = when;
    }

  private:
    mutable std::mutex _mutex;
    std::chrono::system_clock::time_point _now;
};

/// Process-singleton SystemWallClock for callers (e.g. CacheEngine's
/// default constructor) that want a wall-clock source without requiring
/// every test to inject one. Tests that need determinism construct a
/// ManualWallClock locally and pass it in.
/// @return Reference to a singleton SystemWallClock with static storage.
[[nodiscard]] inline IWallClock& DefaultSystemWallClock() noexcept
{
    static SystemWallClock instance;
    return instance;
}

} // namespace FastCache
