// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Core/Clock.hpp>

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
    /// @param handle The suspended coroutine to resume once the deadline elapses.
    void await_suspend(std::coroutine_handle<> handle) const
    {
        reactor->Schedule(deadline, handle);
    }

    void await_resume() const noexcept {}
};

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
