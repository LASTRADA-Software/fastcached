// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>

#include <algorithm>
#include <cassert>
#include <coroutine>

namespace FastCache
{

/// Awaitable that suspends the awaiting coroutine until a reactor's clock
/// reaches an absolute `deadline`. `co_await SleepUntil{&reactor, deadline}`
/// parks the coroutine via `IReactor::Schedule` (thread-safe) and resumes it on
/// that reactor's loop thread when the clock advances to or past `deadline`.
///
/// The timer twin of `ResumeOn`: where `ResumeOn` wraps `IReactor::Submit` to
/// re-schedule immediately, `SleepUntil` wraps `IReactor::Schedule` to
/// re-schedule at a deadline. It is the generic building block for any verb
/// that needs a deadline (e.g. a blocking-read BLOCK timeout).
///
/// `reactor` is a nullable pointer rather than a reference so non-reactor
/// transports (the in-memory test transport, which has no timer wheel) can pass
/// `nullptr`: the awaitable then resolves immediately as already-ready. A null
/// reactor or an already-elapsed deadline never suspends.
struct SleepUntil
{
    IReactor* reactor { nullptr }; ///< Reactor whose clock gates the deadline, or nullptr to resolve inline.
    TimePoint deadline {};         ///< Absolute instant at which to resume.

    /// @return true if there is no reactor or the deadline has already passed,
    ///         so the coroutine need not suspend.
    [[nodiscard]] bool await_ready() const noexcept
    {
        return reactor == nullptr || deadline <= reactor->Clock().Now();
    }

    /// Park the handle on the reactor's timer wheel for the deadline.
    ///
    /// Only ever reached after `await_ready()` returned false, which can only
    /// happen when `reactor != nullptr` — the assert states that precondition
    /// (and lets the static analyzer prune the impossible null-deref path the
    /// nullptr-reactor test would otherwise appear to take).
    ///
    /// **Templated on the promise, and that is not a generalisation for its own sake**:
    /// the parking coroutine's promise type is the only thing that can say whether any
    /// object owns this await chain, and a reactor torn down with this timer still
    /// parked has to know before it may free anything
    /// ([#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
    /// `Detail::ParkedWorkFor` answers it; nothing here decides.
    /// @tparam Promise The suspending coroutine's promise type.
    /// @param handle The suspended coroutine to resume once the deadline elapses.
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) const
    {
        assert(reactor != nullptr);
        reactor->Schedule(deadline, Detail::ParkedWorkFor(handle));
    }

    void await_resume() const noexcept {}
};

/// The next instant a bounded wait should sleep to on its way to `deadline`.
///
/// The one place the polling rule is written down. A `Schedule` can only be taken
/// back by whoever HOLDS the handle (`IReactor::CancelPending`), and the coroutine
/// doing the waiting is not in a position to hold its own -- so a wait that must ALSO
/// be woken by something else sleeps in steps no longer than `wakeBound` and re-reads
/// its own condition at each one, which is what makes the sleeping frame *be* the
/// wait and keeps the handle a teardown has to name the caller's own.
///
/// **It does NOT mean nothing is parked when the reactor stops.** That is what this
/// said, and it was the exact false belief
/// [#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025) is about: a
/// step in progress at that moment IS a frame in the timer heap, and until #1025
/// nothing resumed or freed it. What is true is the narrower claim above -- the frame
/// parked is this coroutine's own rather than some inner task's, which is what lets
/// `ParkedWork::abandon` name a root that covers it.
///
/// Three waits do this (`InterruptibleSleepUntil`, `DeadlineTimer`,
/// `ExpiryReaper`) and each has to inline its own loop rather than delegate it,
/// because awaiting a nested `Task` parks the INNER coroutine's handle and the
/// handle a teardown has to name would then not be the caller's own. The
/// arithmetic is the part they can share, and it had drifted into three copies.
///
/// A non-positive `wakeBound` means "do not poll", not "spin": a zero-length
/// step resolves as already-ready and would turn the loop into a busy reactor
/// thread, so it sleeps straight through to the deadline instead.
/// @param now       Current clock value.
/// @param deadline  Absolute instant the wait ends at.
/// @param wakeBound Longest single uninterruptible sleep; non-positive for one
///                  sleep straight through.
/// @return The instant to sleep to next; never past `deadline`.
[[nodiscard]] constexpr TimePoint NextWakeStep(TimePoint now, TimePoint deadline, Duration wakeBound) noexcept
{
    return wakeBound > Duration::zero() ? std::min(deadline, now + wakeBound) : deadline;
}

/// Build a `SleepUntil` for a relative delay measured from the reactor's
/// current clock value — the ergonomic form for callers that think in
/// durations rather than absolute instants.
/// @param reactor Reactor whose clock both anchors the delay and gates the wait.
/// @param delay   Non-negative duration to wait from `reactor.Clock().Now()`.
/// @return An awaitable resuming at `reactor.Clock().Now() + delay`.
[[nodiscard]] inline SleepUntil SleepFor(IReactor& reactor, Duration delay) noexcept
{
    return SleepUntil { .reactor = &reactor, .deadline = reactor.Clock().Now() + delay };
}

} // namespace FastCache
