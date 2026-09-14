// SPDX-License-Identifier: Apache-2.0
#include "CompileCapacity.hpp"

#include <FastCache/Core/BoundedDrain.hpp>

#include <chrono>
#include <cstdlib>

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

SlotAdmission CompileCapacity::TryTakeSlot() noexcept
{
    // Tested and taken under ONE lock, the lock `Cordon` and `ReleaseSlot` hold. Between
    // a lock-free test and a take, another connection could take the last slot -- two
    // compiles on a worker that advertises one -- and, since #1303, an operator could
    // cordon a worker with nothing running, be told it had drained, and have a compile
    // start behind that report. A compile holds a slot for seconds; a mutex costs nothing
    // beside it.
    auto const guard = std::scoped_lock { _drainMutex };
    if (_shuttingDown.load(std::memory_order_acquire))
        return SlotAdmission::Stopping;
    if (_cordoned.load(std::memory_order_acquire))
        return SlotAdmission::Cordoned;
    if (_inFlight.load(std::memory_order_acquire) >= _slots)
        return SlotAdmission::Full;
    _inFlight.fetch_add(1, std::memory_order_acq_rel);
    return SlotAdmission::Taken;
}

void CompileCapacity::ReleaseSlot() noexcept
{
    // Under the drain's own mutex, because the wait it wakes is on a condition
    // variable: notifying outside the lock can leave a drain that has evaluated its
    // predicate and not yet slept waiting for a notification that has already
    // happened.
    auto const guard = std::scoped_lock { _drainMutex };
    auto const remaining = _inFlight.fetch_sub(1, std::memory_order_acq_rel) - 1;
    _drained.notify_all();

    // The LAST release under a cordon, and only that one. A release while compiles are
    // still running says nothing an operator waiting to reboot can act on.
    if (remaining == 0 && _cordoned.load(std::memory_order_acquire))
        ReportDrained();
}

CompileCacheWire::CordonFields CompileCapacity::Cordon(bool cordoned)
{
    auto const guard = std::scoped_lock { _drainMutex };
    auto const inFlight = _inFlight.load(std::memory_order_acquire);
    if (_cordoned.exchange(cordoned, std::memory_order_acq_rel) != cordoned)
    {
        if (!cordoned)
            _logger.Logf(LogLevel::Info, "worker: cordon lifted; taking compiles again");
        else if (inFlight != 0)
            _logger.Logf(LogLevel::Info,
                         "worker: cordoned; refusing new compiles and letting {} running compile(s) finish",
                         inFlight);
        else
            ReportDrained();
        // Under the mutex the waiter's predicate reads beside, so a change cannot fall
        // between its check and its sleep.
        _cordonMoved.notify_all();
    }
    return CompileCacheWire::CordonFields { .state = CordonState(), .inFlight = static_cast<std::uint32_t>(inFlight) };
}

HeartbeatWake CompileCapacity::WaitForHeartbeat(std::stop_token const& stop,
                                                bool announced,
                                                std::chrono::milliseconds interval)
{
    // The deadline on the clock the wait itself runs on, so the answer can say what ENDED
    // the wait rather than what the state is afterwards. A cordon that moved while the
    // interval ran out -- nobody woke this -- is `Elapsed`: the round runs either way, and
    // calling it `CordonChanged` would credit a wake that never happened.
    auto const deadline = std::chrono::steady_clock::now() + interval;
    auto guard = std::unique_lock { _drainMutex };
    auto const moved = _cordonMoved.wait_until(
        guard, stop, deadline, [this, announced] { return _cordoned.load(std::memory_order_acquire) != announced; });
    if (stop.stop_requested())
        return HeartbeatWake::Stopped;
    return moved && std::chrono::steady_clock::now() < deadline ? HeartbeatWake::CordonChanged : HeartbeatWake::Elapsed;
}

bool CompileCapacity::IsCordoned() const noexcept
{
    return _cordoned.load(std::memory_order_acquire);
}

CompileCacheWire::WireCordonState CompileCapacity::CordonState() const noexcept
{
    if (!_cordoned.load(std::memory_order_acquire))
        return CompileCacheWire::WireCordonState::Serving;
    return _inFlight.load(std::memory_order_acquire) == 0 ? CompileCacheWire::WireCordonState::Drained
                                                          : CompileCacheWire::WireCordonState::Draining;
}

void CompileCapacity::ReportDrained() const
{
    // The line an operator waiting to reboot this machine watches for. It says what the
    // state MEANS rather than what the counter reads, because that is the question being
    // asked: a slot is held until its reply has been written (see `CompileResponder`), so
    // nothing this worker admitted is still owed to anybody.
    _logger.Logf(LogLevel::Info,
                 "worker: cordoned and drained; no compile is running, so stopping this node now abandons nothing");
}

void CompileCapacity::BeginShutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
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
                std::_Exit(AbandonedDrainExitCode);

            case DrainAction::Last:
                break;
        }
    }
}

// --- moved with their declarations when the dedicated compile port went ---------
//
// Both were defined in `WorkerServer.cpp` because that is where the accept loop
// was. Neither is about accepting: the drain decision is arithmetic over this
// object's counters, and the membership refusal is what any door onto the compile
// verbs answers a stranger with -- `CompileResponder` on the merged surface is the
// only caller left.

DrainAction NextDrainAction(std::size_t outstanding,
                            std::chrono::steady_clock::duration waited,
                            std::chrono::seconds timeout) noexcept
{
    if (outstanding == 0)
        return DrainAction::Finished;
    if (timeout == std::chrono::seconds::zero())
        return DrainAction::Report;
    return waited >= timeout ? DrainAction::Abandon : DrainAction::Report;
}

std::optional<std::vector<std::byte>> RefuseUnlessMember(Distributed::IMembershipOracle const& membership,
                                                         IMetricsSink& metrics,
                                                         std::string_view peer)
{
    if (membership.Classify(peer) == Distributed::Membership::Member)
        return std::nullopt;

    // This machine and this cluster's members. Everyone else is refused as a *reply*
    // rather than by closing, so a misconfigured peer learns which of the two it is
    // instead of seeing a connection it cannot tell from a dead host. Without this the
    // port accepted anybody who could route to it and ran their compiler for them,
    // which on a widened `--listen-node` is the network.
    return Cc::Refuse(metrics, CompileRefusal::NotAMember, "this worker compiles for its own machine and its cluster");
}

} // namespace FastCache::Node
