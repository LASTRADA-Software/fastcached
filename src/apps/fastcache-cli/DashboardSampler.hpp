// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "StatsSource.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>

#include <thread>
#include <vector>

namespace FastCache::Cli
{

/// @file DashboardSampler.hpp
/// Taking one stats reading without stalling the dashboard.
///
/// **`IStatsGatherer::Gather()` is synchronous and does socket I/O.** Run on the
/// reactor it would stall ticks, input AND quit together -- the loop would freeze at
/// exactly the moment an operator wants out, which is the worst time for a dashboard to
/// stop answering. So the gather goes off to a pool and the result comes back:
/// `ResumeOn(pool)`, gather, `ResumeOn(reactor)`. That is the same two-hop the compile
/// surface already uses for the same reason.
///
/// **The hop back is invisible at every call site**, which is what makes it easy to get
/// wrong and impossible to notice: a sample arrives either way. So the property worth
/// asserting is the THREAD IDENTITY, not the reply -- `SampleOutcome` carries both, and
/// a test reads them rather than inferring anything from the reading.

/// Where one sampling round ran, and what it produced.
///
/// The thread ids are here because a test cannot otherwise ask the question. A reading
/// that arrived proves nothing about where `Gather()` ran, and an implementation that
/// dropped the pool hop entirely would keep every other assertion about this type true.
struct SampleOutcome
{
    /// What each source said. Empty when the gather could not be attempted.
    std::vector<StatsAttempt> attempts {};

    /// When the sources answered, on the injected clock. See `TakeSample` for where it is read.
    TimePoint takenAt {};

    /// The thread `Gather()` actually ran on.
    std::thread::id gatheredOn {};

    /// The thread the caller was resumed on.
    std::thread::id resumedOn {};
};

/// Take one reading off the reactor and come back.
///
/// Pointers rather than references, and not as a style preference: a coroutine
/// frame outlives the call that created it, so a reference parameter dangles the
/// moment the referent dies before the coroutine resumes. Every production
/// coroutine in this tree takes pointers for that reason, and clang-tidy refuses
/// the alternative. None may be null.
///
/// `resumeOn` is an `IExecutor` rather than a reactor because `ResumeOn` needs
/// nothing more -- and naming a reactor claimed a dependency this function does not
/// have. It is what would have put two schedulers in one loop the day something
/// other than `IReactor` drives the dashboard; now the sampler names a place to come
/// back to rather than a scheduler.
///
/// **The clock is read on the POOL, while the reactor may be parked, so it must read its
/// source on every call.** A clock cached by the loop -- `CachedClock`, whose `Now()` returns
/// what the reactor's last wake-up `Refresh()`ed, or simply passing the reactor's own clock --
/// answers the time the dashboard reactor last woke, and during a `Gather()` that reactor is
/// asleep by design. Every rate's denominator would then be off by up to one gather, silently:
/// `ManualClock` has no cache, so no test can see it. `IClock` cannot say this in its type,
/// which leaves this sentence as the only guard. Pass `SteadyClock`.
///
/// @param gatherer The ladder to ask.
/// @param clock What stamps the reading. Must read its source on every `Now()`; see above.
/// @param pool Where the blocking gather runs. Sized 1: one sampler, and it keeps the
///        ordering of readings trivially rather than by arrangement.
/// @param resumeOn Where the caller is resumed before the result is used.
/// @return The reading, and where it was taken.
[[nodiscard]] Task<SampleOutcome> TakeSample(IStatsGatherer* gatherer, IClock* clock, IExecutor* pool, IExecutor* resumeOn);

} // namespace FastCache::Cli
