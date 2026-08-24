// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Cancellation.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>

#include <cstdint>

namespace FastCache
{

/// Why an `InterruptibleSleepUntil` ended.
enum class WakeReason : std::uint8_t
{
    Deadline,  ///< The deadline arrived with the token still uncancelled.
    Cancelled, ///< The token was cancelled first.
};

/// Sleep until `deadline`, or until `token` is cancelled — whichever comes first.
///
/// ## This is a poll, and the bound is the whole design
///
/// `IReactor::Schedule` cannot be cancelled: it hands the coroutine to the timer
/// wheel and nothing takes it back. So a wait that must ALSO be woken by
/// something else has exactly two shapes.
///
/// It can park a second, detached coroutine on the wheel to fire at the deadline
/// and check a won/lost flag. That works, and it leaves that frame parked for up
/// to the full interval *after* the wait has already ended — so a reactor stopped
/// in between returns with a frame nobody resumes and nobody ever frees, which is
/// the leak `IReactor::Run`'s return semantics make possible and which this
/// codebase has already paid for once.
///
/// Or it can sleep in steps no longer than `wakeBound` and re-read the token at
/// each one, which leaves nothing behind at all because the sleeping frame *is*
/// the wait. That is what this does, and it is the same answer
/// `RaftDriver::Run`'s bounded sleep gives to the same constraint, for the same
/// stated reason: a wait nothing can interrupt is bounded rather than left to be
/// woken. The cost is one wake-up per bound per waiting coroutine, each of which
/// loads one atomic and re-parks.
///
/// `wakeBound` has **no default**. A caller that has not chosen it has not
/// decided how long its own teardown may lag, and a default here would make that
/// decision silently on its behalf.
///
/// @param reactor Timer wheel and clock. A null pointer resolves immediately as
///        `Deadline`, mirroring `SleepUntil`'s nullable-reactor contract for
///        transports that have no timer wheel.
/// @param token Observed before the first sleep and after every step. Taken **by
///        value**: this is a coroutine, so a reference parameter is bound before
///        the first suspension and then outlives the expression that produced it.
/// @param deadline Absolute instant to wake at if nothing cancels first.
/// @param wakeBound Longest single uninterruptible sleep. A non-positive value
///        means one sleep straight through to the deadline — i.e. not
///        interruptible at all, which is a legitimate thing to ask for and is
///        spelled explicitly rather than by omission.
/// @return Why the wait ended.
[[nodiscard]] Task<WakeReason> InterruptibleSleepUntil(IReactor* reactor,
                                                       CancellationToken token,
                                                       TimePoint deadline,
                                                       Duration wakeBound);

} // namespace FastCache
