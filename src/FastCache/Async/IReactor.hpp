// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <coroutine>

namespace FastCache
{

/// Reactor abstraction. Owns the event loop on one thread; coroutines that
/// need to suspend until I/O is ready or until a deadline elapses post
/// themselves to the reactor via Submit / Schedule respectively.
///
/// Threading contract: a single reactor instance is single-threaded. Submit
/// and Schedule are safe to call from any thread (they may go through a
/// cross-thread wake mechanism), but Run() must be called from exactly one
/// thread for the lifetime of the loop, and resumed coroutines run on that
/// thread.
class IReactor: public IExecutor
{
  public:
    IReactor() = default;
    IReactor(IReactor const&) = delete;
    IReactor(IReactor&&) = delete;
    IReactor& operator=(IReactor const&) = delete;
    IReactor& operator=(IReactor&&) = delete;
    ~IReactor() override = default;

    /// Block on the event loop until Stop() is called and the ready queue
    /// drains. Re-entry is undefined behaviour.
    virtual void Run() = 0;

    /// Ask Run() to exit gracefully. Idempotent. May be called from any
    /// thread (including from inside the reactor's own thread).
    virtual void Stop() noexcept = 0;

    /// Post a coroutine handle for resumption on the reactor's thread.
    /// Order between Submit() calls from a single thread is preserved
    /// (FIFO). Calls from multiple threads interleave deterministically per
    /// implementation (TestReactor: FIFO of arrival; production reactors:
    /// best-effort FIFO).
    /// @param handle Coroutine to resume. Must remain alive until resumed.
    void Submit(std::coroutine_handle<> handle) override = 0;

    /// Resume a coroutine handle when the reactor's clock advances to or
    /// past the given deadline. Ordering between concurrently-scheduled
    /// timers with the same deadline is FIFO.
    /// @param deadline Absolute time at which to resume.
    /// @param handle Coroutine to resume.
    virtual void Schedule(TimePoint deadline, std::coroutine_handle<> handle) = 0;

    /// Take a handle back off this reactor while it is still waiting to be resumed.
    ///
    /// The counterpart `Submit` and `Schedule` were missing, and its absence had a
    /// price: a wait built on `Schedule` could not be ended early, so anything that
    /// wanted to stop waiting had to poll in bounded steps and leave a frame parked
    /// in between -- and a reactor destroyed during one of those steps never freed
    /// it. One leaked coroutine frame per disarmed deadline, which is per dial and
    /// per cache exchange.
    ///
    /// Ownership is the whole contract: returning `true` means THIS CALL removed the
    /// handle, so the caller is now the only one who may resume or destroy it.
    /// `false` means the reactor no longer had it -- it is running, already resumed,
    /// or was never here -- and the caller must not touch it. That makes the race
    /// against a timer firing concurrently decidable rather than a guess.
    /// @param handle A handle previously given to `Submit` or `Schedule`.
    /// @return True when this call took it back; false when it was not there to
    ///         take. An implementation that cannot retract a submission (IOCP posts
    ///         it to the kernel) answers false for that case and still cancels
    ///         timers.
    [[nodiscard]] virtual bool CancelPending(std::coroutine_handle<> handle) noexcept = 0;

    /// @return The clock used by this reactor for all deadline checks. Tests
    /// can downcast to ManualClock and Advance() to drive timers; production
    /// code uses SteadyClock.
    [[nodiscard]] virtual IClock& Clock() noexcept = 0;
};

} // namespace FastCache
