// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Cancellation.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/InterruptibleSleep.hpp>
#include <FastCache/Core/Clock.hpp>

#include <chrono>
#include <coroutine>
#include <memory>

namespace FastCache
{

/// A deadline that can be disarmed, built on a `Schedule` that cannot.
///
/// The shape an operation needs when a timeout has to **tear the operation
/// down** rather than merely stop waiting for it. That distinction is the reason
/// this is a timer with a callback and not a race between two tasks: a dial that
/// only stopped waiting would leave the connect attempt in flight for the
/// kernel's own retry schedule — minutes — and a caller that redials on a backoff
/// then accumulates descriptors. A timeout that does not close what it abandoned
/// is a leak with a retry loop behind it.
///
/// Two consequences of `IReactor::Schedule` having no cancellation, both handled
/// here so no caller has to:
///
/// - **A late fire must be harmless.** The state is shared with the timer's own
///   coroutine and outlives this object, so a fire after the owner is gone reads
///   valid memory and does nothing.
/// - **Disarming takes the wait back off the reactor rather than waiting it out.**
///   A dial that succeeds in 2 ms against a 30 s budget would otherwise leave a
///   frame on the reactor's timer heap, and `IReactor::Run` returns with its heap
///   exactly as it was — a coroutine nobody resumes and nobody frees, once per
///   dial. `IReactor::CancelPending` is what retires the frame immediately; the
///   bounded poll below stays as the fallback for a reactor that cannot retract a
///   pending resumption.
///
/// The callback is a function pointer plus a `void*` rather than a
/// `std::function`, matching `EpollFdHandler`, `IocpCompletion::dispatch` and
/// `IoAwaitable::SuspendCallback` — the house shape for a reactor-side callback,
/// and no allocation on a path that runs per dial.
///
/// Threading: the callback runs on the reactor's loop thread. Destroying the
/// timer disarms it, and doing so from inside its own callback is safe.
class DeadlineTimer
{
  public:
    /// Invoked at most once, on the reactor thread, when the deadline elapses
    /// without a `Disarm()`.
    using Callback = void (*)(void* state);

    /// Default upper bound on how long a disarmed timer's frame lingers.
    ///
    /// Small enough that a settled operation's frame is reclaimed promptly, large
    /// enough that an idle timer is a handful of wake-ups per second rather than
    /// a busy loop.
    static constexpr Duration DefaultPollInterval = std::chrono::milliseconds { 50 };

    /// Arm a deadline on `reactor`.
    ///
    /// @param reactor Reactor whose clock gates the deadline and whose thread
    ///        runs the callback.
    /// @param deadline Absolute instant at or after which `onExpired` runs,
    ///        unless disarmed first. A deadline already in the past fires on the
    ///        reactor's next turn rather than inline, so a caller is never
    ///        re-entered from its own constructor.
    /// @param onExpired What to do when it elapses. Must not be null.
    /// @param state Opaque pointer handed to `onExpired`; must outlive the timer
    ///        or the callback must tolerate it not doing so.
    /// @param pollInterval Upper bound on how long a disarmed timer's frame
    ///        lingers on the reactor.
    DeadlineTimer(
        IReactor& reactor, TimePoint deadline, Callback onExpired, void* state, Duration pollInterval = DefaultPollInterval);

    DeadlineTimer(DeadlineTimer const&) = delete;
    DeadlineTimer(DeadlineTimer&&) = delete;
    DeadlineTimer& operator=(DeadlineTimer const&) = delete;
    DeadlineTimer& operator=(DeadlineTimer&&) = delete;

    /// Disarms, which also ends the timer's own coroutine.
    ~DeadlineTimer();

    /// Prevent the callback from running.
    ///
    /// Idempotent, and safe after the callback has already run. Only meaningful
    /// on the reactor thread, which is where every user of this type lives.
    void Disarm() noexcept;

    /// @return true once the callback has run or `Disarm()` has been called.
    [[nodiscard]] bool IsSettled() const noexcept;

    /// Shared with the timer coroutine so a late fire reads live memory even
    /// after the owning `DeadlineTimer` is gone.
    ///
    /// Public only so the .cpp's timer coroutine can name it, the same reason
    /// and the same spelling as `EpollSocket::Impl`. Treat as private.
    struct State
    {
        Callback onExpired { nullptr };
        void* state { nullptr };
        /// The timer coroutine's own handle while it waits, so `Disarm()` can take
        /// it back off the reactor. Cleared by that coroutine on its way out.
        std::coroutine_handle<> parked {};
        /// Plain `bool`, not atomic: both writers (the owner and the timer
        /// coroutine) run on the reactor's thread, and saying so here is what
        /// makes that a stated precondition rather than an assumption.
        bool settled { false };
    };

  private:
    /// Only to cancel the wait on disarm. The timer must not outlive it, which
    /// every user satisfies by construction: the timer is a local of a coroutine
    /// running ON this reactor.
    IReactor* _reactor;
    CancellationSource _disarm;
    std::shared_ptr<State> _shared;
};

} // namespace FastCache
