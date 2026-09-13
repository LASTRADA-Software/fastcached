// SPDX-License-Identifier: Apache-2.0
#include "DashboardSampler.hpp"

#include <FastCache/Async/ResumeOn.hpp>

#include <utility>

namespace FastCache::Cli
{

Task<SampleOutcome> TakeSample(IStatsGatherer* gatherer, IExecutor* pool, IExecutor* resumeOn)
{
    auto outcome = SampleOutcome {};

    co_await ResumeOn { *pool };
    // Recorded INSIDE the pool hop and before the blocking call, so the value describes
    // where `Gather()` is about to run rather than where anything finished. Read after
    // the return it would be the same id by luck on a one-thread pool and wrong on any
    // other, which is the kind of test that passes for a reason nobody can state.
    outcome.gatheredOn = std::this_thread::get_id();
    outcome.attempts = gatherer->Gather();

    // And back, BEFORE the result is used. The caller touches dashboard state, which
    // belongs to the reactor thread; returning from the pool would hand it that state on
    // the wrong thread with nothing to say so.
    co_await ResumeOn { *resumeOn };
    outcome.resumedOn = std::this_thread::get_id();

    co_return outcome;
}

} // namespace FastCache::Cli
