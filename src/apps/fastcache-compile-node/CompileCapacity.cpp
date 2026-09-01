// SPDX-License-Identifier: Apache-2.0
#include "CompileCapacity.hpp"

// For `NextDrainAction`, `DrainAction` and `DrainAbandonedExitCode`. The header
// deliberately does not include this: `WorkerServer` holds a `CompileCapacity`,
// so the dependency runs one way and the drain decision is reached only here.
#include "WorkerServer.hpp"

#include <chrono>

namespace FastCache::Node
{

namespace
{
    /// How often a stop says what it is still waiting for.
    ///
    /// A stop that says nothing for the whole timeout is indistinguishable from one
    /// that has hung, which is the reading this whole change exists to prevent -- so
    /// the interval is short enough that an operator watching `systemctl stop` sees
    /// the count fall rather than a pause.
    constexpr std::chrono::seconds DrainReportInterval { 2 };

    /// What this process exits with when it abandons compiles to stop.
    ///
    /// Distinct from every ordinary failure, so a supervisor's log tells "stopped
    /// with compiles still running" from a crash. 75 is `EX_TEMPFAIL` from
    /// `sysexits.h` -- not a standard this project otherwise uses, but the closest
    /// thing to a shared vocabulary for "this was not clean, and retrying is
    /// reasonable", and unambiguous beside a compiler's own exit codes.
    constexpr int DrainAbandonedExitCode = 75;
} // namespace

bool CompileCapacity::TakeBytes(std::size_t want) noexcept
{
    // Compare-and-swap against the budget rather than "add, then check and undo":
    // the undo is visible to every other thread in between, so a second request
    // arriving inside that window sees a total that was never really held and is
    // refused for a budget that is not spent.
    //
    // `want > budget` first, because the subtraction below would wrap.
    if (want > _byteBudget)
        return false;

    auto current = _bytesInFlight.load(std::memory_order_acquire);
    while (current <= _byteBudget - want)
        if (_bytesInFlight.compare_exchange_weak(
                current, current + want, std::memory_order_acq_rel, std::memory_order_acquire))
            return true;

    return false;
}

std::optional<CompileCapacity::Bytes> CompileCapacity::TryTakeBytes(std::size_t want) noexcept
{
    if (!TakeBytes(want))
        return std::nullopt;
    return std::optional<Bytes> { std::in_place, this, want };
}

bool CompileCapacity::TryTakeSlot() noexcept
{
    // Taken first and given back on refusal, rather than tested and then taken:
    // between a test and a take, another connection can take the last slot, and two
    // compiles then run on a worker that advertises one.
    auto const before = _inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (before < _slots)
        return true;

    _inFlight.fetch_sub(1, std::memory_order_acq_rel);
    return false;
}

void CompileCapacity::ReleaseSlot() noexcept
{
    // Under the drain's own mutex, because the wait it wakes is on a condition
    // variable: notifying outside the lock can leave a drain that has evaluated its
    // predicate and not yet slept waiting for a notification that has already
    // happened.
    auto const guard = std::scoped_lock { _drainMutex };
    _inFlight.fetch_sub(1, std::memory_order_acq_rel);
    _drained.notify_all();
}

void CompileCapacity::BeginShutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
}

bool CompileCapacity::IsShuttingDown() const noexcept
{
    return _shuttingDown.load(std::memory_order_acquire);
}

std::size_t CompileCapacity::InFlight() const noexcept
{
    return _inFlight.load(std::memory_order_acquire);
}

void CompileCapacity::Drain()
{
    auto const idle = [this] {
        return _inFlight.load(std::memory_order_acquire) == 0;
    };

    auto const started = std::chrono::steady_clock::now();
    auto guard = std::unique_lock { _drainMutex };
    while (true)
    {
        (void) _drained.wait_for(guard, DrainReportInterval, idle);

        switch (NextDrainAction(
            _inFlight.load(std::memory_order_acquire), std::chrono::steady_clock::now() - started, _drainTimeout))
        {
            case DrainAction::Finished:
                return;

            case DrainAction::Report:
                _logger.Logf(LogLevel::Info,
                             "worker: waiting for {} compile(s) to finish before stopping",
                             _inFlight.load(std::memory_order_acquire));
                break;

            case DrainAction::Abandon:
                _logger.Logf(LogLevel::Error,
                             "worker: giving up after {}s with {} compile(s) still running; ending now rather than "
                             "waiting for the supervisor to kill this process without saying why (#239)",
                             _drainTimeout.count(),
                             _inFlight.load(std::memory_order_acquire));

                // NOT a return. A running compile holds a pointer into this object --
                // the counter, the protocol, the metrics sink, the logger, the byte
                // budget -- so unwinding out of here would free all of them underneath
                // it, trading a stop that waits for a crash on the way out. Ending the
                // process is the one exit that abandons those jobs without touching
                // what they are still using, and each one's client resolves its own
                // lease on every path out of a compile (#212).
                //
                // `_Exit`, not `exit`: static destructors would run the same teardown
                // this is avoiding.
                std::_Exit(DrainAbandonedExitCode);

            case DrainAction::Last:
                break;
        }
    }
}

} // namespace FastCache::Node
