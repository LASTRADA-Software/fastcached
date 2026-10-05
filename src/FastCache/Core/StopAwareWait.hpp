// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

#include <core/platform/Clock.hpp>

namespace FastCache
{

/// How a stop-aware wait ended.
///
/// TRANSMITTED/PERSISTED: no. Private to this process; enumerators may be inserted.
enum class WaitEnd : std::uint8_t
{
    Elapsed, ///< The whole interval passed with no stop requested.
    Stopped, ///< A stop was requested, before the wait or during it.
};

/// Block the calling thread for @p interval, or until @p stop is requested, whichever is first.
///
/// **The stop token takes part in the wait rather than being checked between sleeps.** A loop that
/// sleeps in slices and polls the token between them observes a stop up to one slice late, so a
/// thread that should end at once holds its port, its sockets and its shutdown for that long; the
/// slice only trades that latency against idle wakeups. `std::condition_variable_any` registers a
/// stop callback for the duration of the wait, so a request wakes it immediately and the interval
/// stops being a teardown cost at all (#1339).
///
/// For a thread holding a `std::stop_token`. A coroutine waits through `InterruptibleSleepUntil`
/// (`Async/InterruptibleSleep.hpp`) instead, which answers the same two outcomes as `core::net::WakeReason`.
///
/// The mutex and the variable are locals: nothing else can notify them, and nothing else should.
/// The stop token is the one wakeup, which is also why the predicate is constant -- a spurious wakeup
/// resumes the wait instead of ending it early.
/// @param stop Participates in the wait; a request ends it at once.
/// @param interval How long to wait when nobody asks for a stop.
/// @return `Stopped` when a stop was requested, before or during the wait; `Elapsed` otherwise.
template <typename Rep, typename Period>
[[nodiscard]] WaitEnd WaitForStopOr(std::stop_token const& stop, std::chrono::duration<Rep, Period> interval)
{
    auto mutex = std::mutex {};
    auto wake = std::condition_variable_any {};
    auto guard = std::unique_lock { mutex };
    (void) wake.wait_for(guard, stop, interval, [] { return false; });
    return stop.stop_requested() ? WaitEnd::Stopped : WaitEnd::Elapsed;
}

/// A stop-aware wait as a seam: what a thread that paces itself by an interval waits through, so
/// the interval is read from a clock a test drives rather than from the wall.
///
/// **It hands out the clock its intervals pass on** (`Clock`), so a component that waits through it
/// and reads time beside it reads ONE clock: handed the two separately, a caller could pass a wait
/// over one clock and a different clock, and the component would read a time its waits never pass.
class IStopAwareWait
{
  public:
    IStopAwareWait() = default;
    IStopAwareWait(IStopAwareWait const&) = delete;
    IStopAwareWait(IStopAwareWait&&) = delete;
    IStopAwareWait& operator=(IStopAwareWait const&) = delete;
    IStopAwareWait& operator=(IStopAwareWait&&) = delete;
    virtual ~IStopAwareWait() = default;

    /// Block for @p interval, or until @p stop is requested, whichever is first.
    /// @param stop Participates in the wait; a request ends it at once.
    /// @param interval How long to wait when nobody asks for a stop.
    /// @return `Stopped` when a stop was requested, before or during the wait; `Elapsed` otherwise.
    [[nodiscard]] virtual WaitEnd WaitFor(std::stop_token const& stop, std::chrono::milliseconds interval) const = 0;

    /// @return The clock every interval this waits passes on; read time from it, never from another.
    [[nodiscard]] virtual core::platform::IClock const& Clock() const noexcept = 0;
};

/// The process's own: `WaitForStopOr`, on the steady clock.
class SystemStopAwareWait final: public IStopAwareWait
{
  public:
    /// @copydoc IStopAwareWait::WaitFor
    [[nodiscard]] WaitEnd WaitFor(std::stop_token const& stop, std::chrono::milliseconds interval) const override
    {
        return WaitForStopOr(stop, interval);
    }

    /// @copydoc IStopAwareWait::Clock
    [[nodiscard]] core::platform::IClock const& Clock() const noexcept override
    {
        return _clock;
    }

  private:
    core::platform::SteadyClock _clock; ///< The steady clock `WaitForStopOr`'s interval passes on.
};

} // namespace FastCache
