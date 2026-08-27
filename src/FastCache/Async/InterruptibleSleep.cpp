// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/InterruptibleSleep.hpp>
#include <FastCache/Async/SleepUntil.hpp>

namespace FastCache
{

Task<WakeReason> InterruptibleSleepUntil(IReactor* reactor, CancellationToken token, TimePoint deadline, Duration wakeBound)
{
    // Asked before anything is scheduled: a caller that is already cancelled must
    // not touch the reactor at all, or a stop issued during teardown parks a
    // frame on a wheel that is about to stop turning.
    if (token.IsCancelled())
        co_return WakeReason::Cancelled;

    // No reactor means no timer wheel (the in-memory transport), and `SleepUntil`
    // resolves inline there. Reporting `Deadline` keeps the two consistent: the
    // wait is over and nothing cancelled it.
    if (reactor == nullptr)
        co_return WakeReason::Deadline;

    while (true)
    {
        auto const now = reactor->Clock().Now();
        if (now >= deadline)
            co_return WakeReason::Deadline;

        co_await SleepUntil { .reactor = reactor, .deadline = NextWakeStep(now, deadline, wakeBound) };

        if (token.IsCancelled())
            co_return WakeReason::Cancelled;
    }
}

} // namespace FastCache
