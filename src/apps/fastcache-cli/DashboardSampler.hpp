// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CliEndpoint.hpp"
#include "LiveSubscriber.hpp"

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <expected>
#include <thread>

namespace FastCache::Cli
{

/// @file DashboardSampler.hpp
/// Reading a live-stats stream without stalling the dashboard.
///
/// **`ILiveSubscription::Open` and `Read` are synchronous and do socket I/O.** Run on the reactor
/// they would stall ticks, input AND quit together -- the loop would freeze at exactly the moment
/// an operator wants out, which is the worst time for a dashboard to stop answering. So each goes off
/// to a pool and the result comes back: `ResumeOn(pool)`, the call, `ResumeOn(reactor)`. That is the
/// same two-hop the compile surface already uses for the same reason.
///
/// **The hop back is invisible at every call site**, which is what makes it easy to get wrong and
/// impossible to notice: a frame arrives either way. So the property worth asserting is the THREAD
/// IDENTITY, not the reply -- `FrameOutcome` carries both, and a test reads them rather than inferring
/// anything from the frame.

/// Where one read of a stream ran, and what it produced.
///
/// The thread ids are here because a test cannot otherwise ask the question. A frame that arrived
/// proves nothing about where `Read()` ran, and an implementation that dropped the pool hop entirely
/// would keep every other assertion about this type true.
struct FrameOutcome
{
    /// The frame, or why none arrived.
    std::expected<NodeReply, ExchangeError> frame { std::unexpected(ExchangeError {}) };

    /// When it arrived, on the injected clock. See `TakeFrame` for where it is read.
    TimePoint takenAt {};

    /// The thread `Read()` actually ran on.
    std::thread::id readOn {};

    /// The thread the caller was resumed on.
    std::thread::id resumedOn {};
};

/// Read one frame of a stream off the reactor and come back.
///
/// Pointers rather than references, and not as a style preference: a coroutine frame outlives the
/// call that created it, so a reference parameter dangles the moment the referent dies before the
/// coroutine resumes. None may be null.
///
/// **The clock is read on the POOL, while the reactor may be parked, so it must read its source on
/// every call.** A clock cached by the loop -- `CachedClock`, or simply the reactor's own clock --
/// answers the time the dashboard reactor last woke, and during a `Read()` that reactor is asleep by
/// design. Every rate's denominator would then be off by up to one read, silently: `ManualClock` has
/// no cache, so no test can see it. Pass `SteadyClock`.
///
/// @param subscription The stream to read.
/// @param clock What stamps the frame. Must read its source on every `Now()`; see above.
/// @param pool Where the blocking read runs. Sized 1: one stream, whose frames stay in order by
///        construction rather than by arrangement.
/// @param resumeOn Where the caller is resumed before the result is used.
/// @return The frame, when it arrived, and where it was read.
[[nodiscard]] Task<FrameOutcome> TakeFrame(ILiveSubscription* subscription,
                                           IClock* clock,
                                           IExecutor* pool,
                                           IExecutor* resumeOn);

/// Open a stream off the reactor and come back: `TakeFrame`'s two hops, for the dial.
/// @param subscription The stream to open.
/// @param where Where to dial; by value, for the coroutine frame.
/// @param request What to subscribe to; by value, for the coroutine frame.
/// @param pool Where the blocking dial runs.
/// @param resumeOn Where the caller is resumed.
/// @return Nothing once the request is sent, or why it could not be.
[[nodiscard]] Task<std::expected<void, ExchangeError>> OpenStream(ILiveSubscription* subscription,
                                                                  Endpoint where,
                                                                  CompileCacheWire::SubscribeRequest request,
                                                                  IExecutor* pool,
                                                                  IExecutor* resumeOn);

} // namespace FastCache::Cli
