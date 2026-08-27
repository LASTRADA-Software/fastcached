// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Cache/ExpiryReaper.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <format>
#include <tuple>

namespace FastCache
{

ExpiryReaper::ExpiryReaper(IStorage& storage, ILogger& logger, ExpiryReaperOptions options, IMetricsSink* metrics) noexcept:
    _storage { storage },
    _logger { logger },
    _metrics { metrics },
    _options { options },
    _interval { _options.interval }
{
}

void ExpiryReaper::Start(IReactor& reactor)
{
    _reactor = &reactor;
    _task = Run(&reactor, _source.Token());
    reactor.Submit(_task.Native());
}

void ExpiryReaper::Stop() noexcept
{
    if (_reactor == nullptr)
        return;
    _source.Cancel();

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
    std::ignore = _reactor->CancelPending(_task.Native());
    _reactor = nullptr;
}

PurgeOutcome ExpiryReaper::SweepOnce(TimePoint now)
{
    FC_ZONE_SCOPED_N("ExpiryReaper::SweepOnce");
    auto const outcome =
        _storage.PurgeExpired(now, PurgeBudget { .maxScanned = _options.scanBudget, .maxPurged = _options.purgeBudget });
    ++_cycles;
    if (_metrics != nullptr)
    {
        _metrics->Increment(IMetricsSink::Counter::ExpiryCycles);
        if (outcome.purged != 0)
            _metrics->Increment(IMetricsSink::Counter::ExpiryKeysReclaimed, outcome.purged);
    }
    return outcome;
}

Task<void> ExpiryReaper::Run(IReactor* reactor, CancellationToken token)
{
    // A disabled cycle ends rather than parking forever: a coroutine asleep on
    // a deadline nobody will move is a frame the reactor has to outlive.
    if (_options.interval <= Duration::zero())
    {
        _logger.Log(LogLevel::Debug, "expiry: active cycle disabled");
        co_return;
    }

    _logger.Logf(LogLevel::Debug,
                 "expiry: active cycle every {} ms (idle backoff to {} ms), {} entries scanned and at most {} "
                 "reclaimed per sweep",
                 std::chrono::duration_cast<std::chrono::milliseconds>(_options.interval).count(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(_options.maxInterval).count(),
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

        auto const outcome = SweepOnce(reactor->Clock().Now());
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
