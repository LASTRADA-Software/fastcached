// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Cancellation.hpp>
#include <FastCache/Async/IReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/ReclaimLog.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace FastCache
{

/// How often the active expiry cycle runs, and how much one turn of it may do.
struct ExpiryReaperOptions
{
    /// Entries one sweep may examine, per shard.
    ///
    /// The lock hold. Large enough that a cache of ordinary size is walked in a
    /// handful of cycles, small enough that no single cycle is measurable
    /// against a request.
    static constexpr std::size_t DefaultScanBudget = 512;

    /// Entries one sweep may reclaim.
    ///
    /// Deliberately below `ReclaimLog::DefaultCapacity`, and `static_assert`ed
    /// against it below. A tier names every key it reclaims in the reclaim log,
    /// `NotifyingStorage` drains that once the call returns, and the log drops
    /// past its capacity -- so a sweep permitted to reclaim more keys at once
    /// than the log holds would silently discard the very `expired` events it
    /// exists to produce. Two numbers, one fact.
    static constexpr std::size_t DefaultPurgeBudget = 1024;

    /// Interval between sweeps while there is work to do. Zero disables the
    /// cycle entirely, which is what an operator who wants expiry to stay
    /// purely access-driven asks for.
    Duration interval { std::chrono::seconds { 1 } };

    /// Interval the cycle backs off to while there is nothing to do.
    ///
    /// A cache with nothing to expire should not keep paying `interval` to be
    /// told so. Backing off does not stop the cycle -- stopping entirely would
    /// need a live count of entries carrying a finite TTL, maintained across
    /// every mutation path in every tier, where the one site that forgot to
    /// maintain it is a cache that silently never expires again.
    Duration maxInterval { std::chrono::seconds { 30 } };

    /// Entries one sweep may examine. See `DefaultScanBudget`.
    std::size_t scanBudget { DefaultScanBudget };

    /// Entries one sweep may reclaim. See `DefaultPurgeBudget`.
    std::size_t purgeBudget { DefaultPurgeBudget };

    /// Longest the cycle may take to notice it has been cancelled.
    ///
    /// The sweep sleeps in steps no longer than this and re-reads the token at
    /// each one, so a shutdown never waits out a whole `maxInterval` and --
    /// more importantly -- leaves no coroutine frame parked on a timer wheel
    /// that is about to stop. See `InterruptibleSleepUntil`.
    Duration stopWakeBound { std::chrono::milliseconds { 50 } };
};

static_assert(ExpiryReaperOptions::DefaultPurgeBudget <= ReclaimLog::DefaultCapacity,
              "one sweep must not reclaim more keys than the reclaim log can hold, or the sweep "
              "drops the expired events it runs to produce");

/// How long to wait before the sweep after the one that produced @p outcome.
///
/// Free and pure, so the pacing can be decided in a test without a reactor, a
/// clock or a storage tier -- and so the one interesting rule in the cycle is
/// somewhere a reader can find it.
///
/// Anything that looks like work -- a key reclaimed, or a pass that ran out of
/// budget and therefore has entries it has not looked at yet -- resets to the
/// base interval. Only a completed pass that found nothing backs off.
/// @param options Configured base and ceiling.
/// @param current The interval that was just waited.
/// @param outcome What the sweep at the end of that wait did.
/// @return The interval to wait next.
[[nodiscard]] constexpr Duration NextExpiryInterval(ExpiryReaperOptions const& options,
                                                    Duration current,
                                                    PurgeOutcome const& outcome) noexcept
{
    if (outcome.purged != 0 || !outcome.completedPass)
        return options.interval;
    auto const doubled = current * 2;
    return doubled > options.maxInterval ? options.maxInterval : doubled;
}

/// The active expiry cycle: a periodic, bounded `IStorage::PurgeExpired`.
///
/// ## Why this exists
///
/// Without it nothing ever calls `PurgeExpired`, so expiry is entirely
/// access-driven -- and with the default `Approximate` LRU recency a *read*
/// does not reclaim either. A key whose TTL lapses and which nobody touches
/// again therefore keeps its value bytes and its byte-budget contribution until
/// eviction happens to reach it, and no `__keyevent@0__:expired` is ever
/// published for it. A subscriber watching for `expired` heard nothing about
/// precisely the case that motivates subscribing.
///
/// ## What it is careful about
///
/// It runs on **one** reactor. The sweep takes each shard's exclusive lock in
/// turn, so a second cycle would contend with the first for no gain.
///
/// It is bounded on both axes (`ExpiryReaperOptions`), and every sweep resumes
/// where the last one stopped, so the cost per cycle is fixed rather than
/// proportional to how much is cached.
///
/// It backs off when idle, and it wakes in `stopWakeBound` steps so a stop is
/// prompt and leaves nothing parked on the reactor's timer wheel.
class ExpiryReaper
{
  public:
    /// Construct over the storage to sweep.
    /// @param storage Storage chain to sweep. Must outlive the reaper. Pass the
    ///        same object the engine writes through -- the notifying decorator
    ///        included, or the reclaimed keys are never published.
    /// @param logger  Where a swept cycle is reported at Debug.
    /// @param options Pacing and ceilings.
    /// @param metrics Counter sink, or nullptr.
    ExpiryReaper(IStorage& storage, ILogger& logger, ExpiryReaperOptions options, IMetricsSink* metrics = nullptr) noexcept;

    /// Sweep until `token` is cancelled.
    ///
    /// Returns immediately when `options.interval` is zero -- a disabled cycle
    /// is a coroutine that ends, not one that parks forever.
    /// @param reactor Reactor whose clock paces the cycle and whose thread the
    ///        sweeps run on.
    /// @param token  Observed at every wake; cancelling it ends the loop.
    /// @return A task that completes when the cycle stops.
    [[nodiscard]] Task<void> Run(IReactor& reactor, CancellationToken token);

    /// One sweep, with this reaper's budget. Exposed so the sweep can be
    /// exercised without a reactor.
    /// @param now Current clock value.
    /// @return What the sweep did.
    PurgeOutcome SweepOnce(TimePoint now);

    /// @return How many sweeps have run.
    [[nodiscard]] std::uint64_t Cycles() const noexcept
    {
        return _cycles;
    }

    /// @return How long the cycle is currently waiting between sweeps. Grows
    /// while there is nothing to reclaim; back to `options.interval` as soon as
    /// there is.
    [[nodiscard]] Duration CurrentInterval() const noexcept
    {
        return _interval;
    }

  private:
    IStorage& _storage;
    ILogger& _logger;
    IMetricsSink* _metrics;
    ExpiryReaperOptions _options;
    Duration _interval;
    std::uint64_t _cycles { 0 };
};

} // namespace FastCache
