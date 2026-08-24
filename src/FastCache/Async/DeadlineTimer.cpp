// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/DeadlineTimer.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/Task.hpp>

#include <algorithm>
#include <utility>

namespace FastCache
{

namespace
{

    /// Hands the running coroutine its own handle, without suspending it.
    ///
    /// `await_suspend` returning `false` means "carry on"; the handle is recorded on
    /// the way past. That is what lets `Disarm()` reach the timer's frame and take
    /// it back off the reactor instead of leaving it parked until the next poll.
    struct CaptureHandle
    {
        std::coroutine_handle<>* slot; ///< Where to leave the handle.

        /// @return False, so `await_suspend` runs and can record the handle.
        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        /// `[[nodiscard]]` and `const` because clang-tidy asks for both of a member
        /// like this, and neither is wrong: the coroutine machinery always reads the
        /// answer, and what this writes to is somebody else's storage.
        /// @param handle The running coroutine.
        /// @return False: recorded, do not actually suspend.
        [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle) const noexcept
        {
            *slot = handle;
            return false;
        }

        void await_resume() const noexcept {}
    };

    /// The timer's body. Detached because nothing awaits it; it owns a reference
    /// to the shared state, so it stays valid however the owner is destroyed.
    DetachedTask RunDeadline(std::shared_ptr<DeadlineTimer::State> shared,
                             IReactor* reactor,
                             CancellationToken disarm,
                             TimePoint deadline,
                             Duration pollInterval)
    {
        // Recorded before anything can park this frame, so `Disarm()` always has
        // something to name -- including in the window between the eager start and
        // the hop below, where the handle sits on the ready queue rather than on the
        // timer wheel. Does not suspend.
        co_await CaptureHandle { &shared->parked };

        // Hop onto the reactor before anything else. `DetachedTask` starts
        // eagerly, so without this the body runs on whichever thread constructed
        // the timer -- and a deadline already in the past would then invoke the
        // callback from inside the constructor, re-entering an object that does
        // not exist yet. It is also what makes the documented promise true that
        // the callback runs on the reactor's loop thread and nowhere else.
        co_await ResumeOn { *reactor };

        // The wait is written out here rather than delegated to
        // `InterruptibleSleepUntil`, and the reason is WHICH frame the reactor ends
        // up holding. Awaiting a nested `Task` parks the INNER coroutine's handle,
        // so the handle recorded above would not be the one `CancelPending` has to
        // name. Awaiting `SleepUntil` directly makes them the same frame.
        auto expired = false;
        while (true)
        {
            if (disarm.IsCancelled())
                break;
            auto const now = reactor->Clock().Now();
            if (now >= deadline)
            {
                expired = true;
                break;
            }
            // A non-positive bound means "do not poll", not "spin" -- the rule
            // `InterruptibleSleepUntil` states, kept identical here.
            auto const step = pollInterval > Duration::zero() ? std::min(deadline, now + pollInterval) : deadline;
            co_await SleepUntil { .reactor = reactor, .deadline = step };
        }

        // Cleared on the ONE exit path, before the callback runs: the callback is
        // allowed to destroy the timer, and a `Disarm()` from that destructor must
        // not go looking on the reactor for a frame that is about to end anyway.
        shared->parked = {};

        if (!expired || shared->settled)
            co_return;
        shared->settled = true;
        shared->onExpired(shared->state);
        co_return;
    }

} // namespace

DeadlineTimer::DeadlineTimer(IReactor& reactor, TimePoint deadline, Callback onExpired, void* state, Duration pollInterval):
    _reactor { &reactor },
    _shared { std::make_shared<State>(State { .onExpired = onExpired, .state = state, .parked = {}, .settled = false }) }
{
    RunDeadline(_shared, &reactor, _disarm.Token(), deadline, pollInterval);
}

DeadlineTimer::~DeadlineTimer()
{
    Disarm();
}

void DeadlineTimer::Disarm() noexcept
{
    if (_shared->settled)
        return;
    _shared->settled = true;
    _disarm.Cancel();

    // Take the timer's own coroutine back off the reactor and free it here. Without
    // this a disarmed timer leaves a frame parked for up to one poll interval, and a
    // reactor destroyed in that window never frees it -- one leaked frame per dial
    // and per cache exchange, which is what made an ASan build of `fastcache-cc`
    // exit non-zero and so turned every compile through it into a failure.
    //
    // `CancelPending` answering true is what transfers ownership: the reactor no
    // longer holds this handle, so nothing else can resume or free it. False means
    // the reactor still has it -- running, or already queued for resumption -- and
    // it will observe the cancellation on its own.
    //
    // DESTROYED rather than resumed, which is the difference between fixing the leak
    // and moving it: resuming only queues the frame, and the caller that disarms is
    // typically about to stop the reactor on its very next line (`ReactorExchange`
    // does exactly that), so the queued handle would never run. Destroying is sound
    // precisely because this coroutine awaits `SleepUntil` directly rather than a
    // nested `Task` -- the handle is the whole chain, and it is a `DetachedTask`, so
    // no awaiter is left holding it.
    if (auto const handle = _shared->parked; handle && _reactor->CancelPending(handle))
    {
        _shared->parked = {};
        handle.destroy();
    }
}

bool DeadlineTimer::IsSettled() const noexcept
{
    return _shared->settled;
}

} // namespace FastCache
