// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Cache/ExpiryReaper.hpp>
#include <FastCache/Cli/Duration.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <coroutine>
#include <format>
#include <thread>
#include <tuple>
#include <utility>

namespace FastCache
{

namespace
{

    /// The hop out to the executor, opening the trip `AwayFromReactor()` measures.
    ///
    /// The count rises on the reactor thread BEFORE the hand-over, because once the frame is
    /// on the executor's queue nothing on the reactor can take it back (#1397). And it falls
    /// again if the hand-over THROWS -- an executor's `Submit` allocates -- because then the
    /// frame never left: the exception resumes it right here, on the reactor. Left raised, the
    /// count would describe a trip nobody is on, and every later `Stop` would wait out its
    /// ceiling and end the process.
    ///
    /// Only the throwing path touches the count after the call, and on that path the frame
    /// was never handed over, so reading the local is all it does.
    struct HopOffReactor
    {
        IExecutor& executor;
        /// The count to raise for the trip, or null when the executor is the reactor.
        std::atomic<std::uint32_t>* away;

        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle) const
        {
            auto* const count = away;
            if (count != nullptr)
                count->fetch_add(1, std::memory_order_acq_rel);
            try
            {
                ResumeOn { executor }.await_suspend(handle);
            }
            catch (...)
            {
                if (count != nullptr)
                    count->fetch_sub(1, std::memory_order_acq_rel);
                throw;
            }
        }

        void await_resume() const noexcept {}
    };

    /// The hop back to the reactor, closing the trip `AwayFromReactor()` measures.
    ///
    /// `ResumeOn` plus one statement, and the statement's POSITION is the fix (#1397): the
    /// count falls only once the hand-over has returned, so while it is raised the frame is
    /// on the executor or still being handed back -- never on neither side with the count down.
    ///
    /// **The counter's address is copied to a local BEFORE the hand-over.** This awaiter is a
    /// temporary in the frame, and once `Submit` returns the reactor may already be running
    /// that frame past this expression -- or `Stop` may be about to free it -- so nothing
    /// after the call may read `this`. What it touches instead is the reaper's counter,
    /// which `Stop` outlives only by waiting for exactly this decrement.
    ///
    /// **A hand-over that throws leaves the count raised, and that is the TRUE answer**, not a
    /// leak: the exception resumes the frame on the EXECUTOR thread, so it has not come back.
    /// Lowering the count there would let `Stop` free a frame that thread is still running.
    /// `Run` tries the hop again instead, which is what eventually lowers it.
    struct HopBackToReactor
    {
        IReactor& reactor;
        /// The count to lower after the hand-over, or null when the frame never left.
        std::atomic<std::uint32_t>* away;

        [[nodiscard]] bool await_ready() const noexcept
        {
            return false;
        }

        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> handle) const
        {
            auto* const count = away;
            ResumeOn { reactor }.await_suspend(handle);
            if (count != nullptr)
                count->fetch_sub(1, std::memory_order_acq_rel);
        }

        void await_resume() const noexcept {}
    };

} // namespace

ExpiryReaper::ExpiryReaper(IStorage& storage,
                           ILogger& logger,
                           ExpiryReaperOptions options,
                           IMetricsSink* metrics,
                           IDrainAbandonment& abandonment,
                           IDrainWait& drainWait) noexcept:
    _storage { storage },
    _logger { logger },
    _metrics { metrics },
    _abandonment { abandonment },
    _drainWait { drainWait },
    _options { options },
    _interval { _options.interval },
    _scanBudget { _options.scanBudget }
{
}

void ExpiryReaper::Start(IReactor& reactor, IExecutor& sweepOn)
{
    _reactor = &reactor;
    _task = Run(&reactor, &sweepOn, _source.Token());
    reactor.Submit(_task.Native());
}

void ExpiryReaper::Stop() noexcept
{
    auto* const reactor = std::exchange(_reactor, nullptr);
    if (reactor == nullptr)
        return;
    _source.Cancel();

    // **Waited for BEFORE anything is reclaimed**, because a frame away from the reactor
    // is one the reactor does not hold: `CancelPending` would answer false and `~Task`
    // would then destroy a coroutine another thread is still executing or handing over.
    // Cancelling the token does not end the trip -- `PurgeExpired` does not observe it --
    // so this waits for the whole of it, which needs no reactor: the count falls on the
    // executor thread when the hop back's `Submit` returns, so this stays safe after
    // `IReactor::Run` has returned (#1397). Once it is down the frame is queued or parked
    // on the reactor, done, or never started, and the retraction below covers all three.
    //
    // Bounded, and it says what it abandoned: an unbounded wait here hands the
    // choice to the supervisor, which answers SIGKILL with no diagnostic. Through
    // `DrainWithin` rather than a hand-rolled loop, because a `waited += poll` count
    // measures the sleep it ASKED for and a sleep costs what the host's timer
    // granularity says. Past the bound the frame is ABANDONED, never destroyed; the
    // declaration of `Stop` says why.
    if (DrainWithin([this] { return AwayFromReactor(); }, _options.stopDrain, _drainWait) == DrainResult::Ceiling)
    {
        _logger.Logf(LogLevel::Error,
                     "expiry: the sweep did not come back from its executor within {} ms of the cycle stopping; "
                     "ending the process rather than freeing a coroutine another thread is still inside (#1397)",
                     _options.stopDrain.ceiling.count());
        _abandonment.Abandon();

        // Reached only through a seam that RETURNS, which only a test supplies. Released --
        // not destroyed, and not retracted, since the reactor never held it -- so `~Task`
        // cannot free it, and whoever supplied that seam now owns it.
        std::ignore = _task.Release();
        return;
    }

    // Taken back off the timer wheel rather than left there: by the time this
    // runs the loop has usually already stopped, so a parked frame would never
    // be resumed and never freed. `~Task` then destroys it, and the answer is
    // deliberately ignored because all three ways it can be `false` are already
    // safe to destroy:
    //
    //   * the coroutine ran to `co_return` -- a disabled cycle does that
    //     immediately -- so the handle is `done()` and nobody holds it;
    //   * it was never parked, because the reactor never ran it;
    //   * the reactor cannot retract it (IOCP answers false for a completion
    //     packet already posted to the kernel). Its port is closed without
    //     being drained, so nothing will ever dereference the handle again --
    //     and leaving the frame behind to be safe would be a leak per daemon
    //     run, which is the failure this whole method exists to prevent.
    //
    // What is NOT optional is that the handle be this task's own, and it is
    // only because `Run` awaits `SleepUntil` directly rather than a nested
    // `Task` -- see the comment there.
    std::ignore = reactor->CancelPending(_task.Native());
}

PurgeOutcome ExpiryReaper::SweepOnce(TimePoint now)
{
    FC_ZONE_SCOPED_N("ExpiryReaper::SweepOnce");
    auto const outcome =
        _storage.PurgeExpired(now, PurgeBudget { .maxScanned = _scanBudget, .maxPurged = _options.purgeBudget });
    ++_cycles;
    if (_metrics != nullptr)
    {
        _metrics->Increment(IMetricsSink::Counter::ExpiryCycles);
        if (outcome.purged != 0)
            _metrics->Increment(IMetricsSink::Counter::ExpiryKeysReclaimed, outcome.purged);
    }
    return outcome;
}

/// Cut the scan budget when a sweep overran its ceiling, and grow it back while it
/// did not.
///
/// **Halve down, step up.** The two directions are deliberately asymmetric, and it is
/// the same argument a congestion window makes: overrunning the ceiling is a stall an
/// operator can feel, so it is corrected at once; being under it is merely slower
/// reclamation, so recovery is gradual and cannot oscillate a busy tier between two
/// extremes every cycle.
///
/// Never above `options.scanBudget`, which stays the operator's ceiling -- this only
/// ever takes budget AWAY from what was configured. And never below `MinScanBudget`,
/// because a budget of zero is `PurgeBudget`'s spelling of *no ceiling* and would
/// invert the whole mechanism into an unbounded scan.
///
/// A sweep that scanned NOTHING is not evidence about cost: an empty tier finishes
/// instantly whatever the budget, and treating that as headroom would grow the budget
/// on a cache that has told us nothing.
/// @param elapsed How long the sweep held the reactor.
void ExpiryReaper::AdaptScanBudget(Duration elapsed) noexcept
{
    if (_options.sweepStallCeiling <= Duration::zero())
        return; // Adaptation disabled; the configured budget stands.

    if (elapsed > _options.sweepStallCeiling)
    {
        auto const halved = _scanBudget / 2;
        _scanBudget = halved < MinScanBudget ? MinScanBudget : halved;
        return;
    }
    if (_scanBudget >= _options.scanBudget)
        return;
    auto const grown = _scanBudget + (_scanBudget / 4) + 1;
    _scanBudget = grown > _options.scanBudget ? _options.scanBudget : grown;
}

Task<void> ExpiryReaper::Run(IReactor* reactor, IExecutor* sweepOn, CancellationToken token)
{
    // Whether the sweep LEAVES the reactor, decided once, by the identity of the executor
    // object -- see `AwayFromReactor()` for why that is the safe side of the question.
    auto* const away = sweepOn == static_cast<IExecutor*>(reactor) ? nullptr : &_awayFromReactor;

    // A disabled cycle ends rather than parking forever: a coroutine asleep on
    // a deadline nobody will move is a frame the reactor has to outlive.
    if (_options.interval <= Duration::zero())
    {
        _logger.Log(LogLevel::Debug, "expiry: active cycle disabled");
        co_return;
    }

    _logger.Logf(LogLevel::Debug,
                 "expiry: active cycle every {} (idle backoff to {}), {} entries scanned and at most {} "
                 "reclaimed per sweep",
                 FormatDuration(std::chrono::duration_cast<std::chrono::milliseconds>(_options.interval)),
                 FormatDuration(std::chrono::duration_cast<std::chrono::milliseconds>(_options.maxInterval)),
                 _options.scanBudget,
                 _options.purgeBudget);

    while (!token.IsCancelled())
    {
        // Bounded steps rather than one sleep straight to the deadline, so a
        // stop is noticed within `stopWakeBound` rather than after a whole
        // backed-off interval.
        //
        // Written out here rather than delegated to `InterruptibleSleepUntil`,
        // and the reason is WHICH frame the reactor ends up holding: awaiting a
        // nested `Task` parks the INNER coroutine's handle, so the handle a
        // shutdown has to name would not be this task's. Awaiting `SleepUntil`
        // directly makes them the same frame, which is what lets the owner take
        // it back with `CancelPending` instead of leaking it. `DeadlineTimer`
        // inlines its wait for exactly this reason.
        auto const deadline = reactor->Clock().Now() + _interval;
        while (!token.IsCancelled())
        {
            auto const now = reactor->Clock().Now();
            if (now >= deadline)
                break;
            co_await SleepUntil { .reactor = reactor, .deadline = NextWakeStep(now, deadline, _options.stopWakeBound) };
        }
        if (token.IsCancelled())
            break;

        // Timed, because `scanBudget` counts ENTRIES and entries are not work
        // (#946). The reactor is held for the whole of `SweepOnce` -- it suspends
        // nowhere -- so this elapsed time IS how long this reactor was unavailable,
        // and it is the only quantity the ceiling can honestly be expressed in.
        auto const startedAt = reactor->Clock().Now();

        // --- Off the reactor for the sweep body (#946). ---
        //
        // `SweepOnce` suspends nowhere, so on the reactor it holds the loop for its
        // whole duration and every connection pinned there is unserved. #964 bounded
        // how long that is; this is what stops it being the reactor's time at all.
        //
        // The same two-hop the compile surface uses, and `IReactor` IS an `IExecutor`
        // -- so a caller that passes the reactor gets exactly the previous behaviour
        // through the same statements, rather than through a second code path.
        //
        // The trip opens in `HopOffReactor`, on the reactor thread and before the hand-over
        // (#1397). A hand-over that throws resumes this frame HERE, on the reactor, with the
        // count already lowered again: the sweep is skipped rather than ending the cycle over
        // one failed allocation, and the next interval tries again.
        auto handedOver = true;
        try
        {
            co_await HopOffReactor { .executor = *sweepOn, .away = away };
        }
        catch (...)
        {
            handedOver = false;
        }
        if (!handedOver)
            continue;

        PurgeOutcome outcome {};
        try
        {
            outcome = SweepOnce(startedAt);
        }
        catch (...)
        {
            // Swallowed, and the hop below still runs. An exception here would
            // otherwise be rethrown where the awaiter resumes -- on the POOL thread
            // if it escaped before the hop -- which is the mistake
            // `CompileResponder` documents at length. A sweep that threw has
            // reclaimed nothing; the cycle continues and the next one tries again.
            outcome = PurgeOutcome {};
        }

        // --- Back, ALWAYS, before anything reads the reactor again. ---
        //
        // Unconditional even when the token is cancelled, and that is the whole rule:
        // a `co_return` from the pool leaves this frame owned by nobody, which is the
        // shape `TeardownIsSerialisedWithDispatch()` exists to catch (#668, #737,
        // #840, #875). The loop's own `IsCancelled()` check is one statement below
        // and runs on the reactor, where ending is safe.
        //
        // And it closes the trip only after its hand-over has returned -- see
        // `HopBackToReactor` for why that position, and not the end of the body, is the one
        // `Stop` can rely on.
        //
        // Retried when the hand-over throws, because that resumes this frame on the EXECUTOR
        // with the count still raised. Letting the exception end the coroutine would finish
        // the frame off the reactor -- the ownerless shape above -- and strand the count, so
        // every later `Stop` would end the process. Yielding between attempts gives whatever
        // failed a moment; a `Stop` meanwhile waits, and past its ceiling says so.
        auto back = false;
        while (!back)
        {
            try
            {
                co_await HopBackToReactor { .reactor = *reactor, .away = away };
                back = true;
            }
            catch (...)
            {
                std::this_thread::yield();
            }
        }

        AdaptScanBudget(reactor->Clock().Now() - startedAt);
        if (outcome.purged != 0)
            _logger.Logf(LogLevel::Debug,
                         "expiry: reclaimed {} lapsed entr{} ({} examined)",
                         outcome.purged,
                         outcome.purged == 1 ? "y" : "ies",
                         outcome.scanned);

        _interval = NextExpiryInterval(_options, _interval, outcome);
    }
    _logger.Logf(LogLevel::Debug, "expiry: active cycle stopped after {} sweep(s)", _cycles);
    co_return;
}

} // namespace FastCache
