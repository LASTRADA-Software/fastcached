// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/DeadlineTimer.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>

#include <utility>

namespace FastCache
{

namespace
{

    /// The timer's body. Detached because nothing awaits it; it owns a reference
    /// to the shared state, so it stays valid however the owner is destroyed.
    DetachedTask RunDeadline(std::shared_ptr<DeadlineTimer::State> shared,
                             IReactor* reactor,
                             CancellationToken disarm,
                             TimePoint deadline,
                             Duration pollInterval)
    {
        // Hop onto the reactor before anything else. `DetachedTask` starts
        // eagerly, so without this the body runs on whichever thread constructed
        // the timer -- and a deadline already in the past would then invoke the
        // callback from inside the constructor, re-entering an object that does
        // not exist yet. It is also what makes the documented promise true that
        // the callback runs on the reactor's loop thread and nowhere else.
        co_await ResumeOn { *reactor };

        auto const reason = co_await InterruptibleSleepUntil(reactor, disarm, deadline, pollInterval);
        if (reason != WakeReason::Deadline)
            co_return;
        if (shared->settled)
            co_return;
        shared->settled = true;
        shared->onExpired(shared->state);
        co_return;
    }

} // namespace

DeadlineTimer::DeadlineTimer(IReactor& reactor, TimePoint deadline, Callback onExpired, void* state, Duration pollInterval):
    _shared { std::make_shared<State>(State { .onExpired = onExpired, .state = state, .settled = false }) }
{
    RunDeadline(_shared, &reactor, _disarm.Token(), deadline, pollInterval);
}

DeadlineTimer::~DeadlineTimer()
{
    Disarm();
}

void DeadlineTimer::Disarm() noexcept
{
    _shared->settled = true;
    _disarm.Cancel();
}

bool DeadlineTimer::IsSettled() const noexcept
{
    return _shared->settled;
}

} // namespace FastCache
