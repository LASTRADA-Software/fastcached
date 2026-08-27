// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/InterruptibleSleep.hpp>
#include <FastCache/Cache/ExpiryReaper.hpp>
#include <FastCache/Core/Profiling.hpp>

#include <format>
#include <utility>

namespace FastCache
{

ExpiryReaper::ExpiryReaper(IStorage& storage, ILogger& logger, ExpiryReaperOptions options, IMetricsSink* metrics) noexcept:
    _storage { storage },
    _logger { logger },
    _metrics { metrics },
    _options { std::move(options) },
    _interval { _options.interval }
{
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

Task<void> ExpiryReaper::Run(IReactor& reactor, CancellationToken token)
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
        // Bounded steps rather than one sleep to the deadline, so a stop is
        // noticed within `stopWakeBound` AND the sleeping frame is the wait
        // itself -- nothing is left parked on a timer wheel that is about to
        // stop. Same answer, for the same reason, as `RaftPeerTransport`'s
        // reconnect backoff.
        if (co_await InterruptibleSleepUntil(&reactor, token, reactor.Clock().Now() + _interval, _options.stopWakeBound)
            == WakeReason::Cancelled)
            break;

        auto const outcome = SweepOnce(reactor.Clock().Now());
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
