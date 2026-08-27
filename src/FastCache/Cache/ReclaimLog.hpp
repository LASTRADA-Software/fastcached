// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/IReclaimLog.hpp>
#include <FastCache/Cache/IStorageMutationObserver.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string_view>
#include <vector>

namespace FastCache
{

/// The `IReclaimLog` the daemon runs: a mutex-guarded, bounded buffer between
/// the tier that reclaims an entry and the decorator that publishes the event.
///
/// Thread-safe by construction. `ShardedStorage` fans writes across shards and,
/// on Windows, drains its IOCP reactor from several threads, so two shards can
/// evict concurrently into one log.
///
/// **Bounded on purpose.** In the steady state the log holds almost nothing:
/// reclaims happen *inside* a storage call and `NotifyingStorage` drains as soon
/// as that call returns. One call can still reclaim without bound, though —
/// `Resize()` to a smaller budget evicts until it fits, and on a large cache that
/// is millions of keys in a single `EvictToFit`. Buffering all of them to publish
/// events nobody asked for would be a memory spike at exactly the moment the
/// operator was trying to *reduce* memory. Past the cap, entries are dropped and
/// counted: keyspace notifications are best-effort in redis too, and a bounded
/// loss the daemon can account for beats an unbounded buffer it cannot. Which end
/// goes is the same choice `AsyncQueueOverflow::DropNewest` names — the entries
/// already queued are the ones a subscriber is closest to receiving.
class ReclaimLog final: public IReclaimLog
{
  public:
    /// How many pending entries the log holds before it starts dropping.
    static constexpr std::size_t DefaultCapacity = 4096;

    /// Construct over the observer whose interest gates recording.
    /// @param observer Decides `IsRecording()`; nullptr records nothing.
    /// @param metrics  Where drops are reported as
    ///                 `fastcached_keyspace_reclaim_events_dropped_total`;
    ///                 nullptr keeps the count readable only via `Dropped()`.
    /// @param capacity Maximum pending entries; beyond it, records are dropped
    ///                 and counted.
    explicit ReclaimLog(IStorageMutationObserver* observer,
                        IMetricsSink* metrics = nullptr,
                        std::size_t capacity = DefaultCapacity) noexcept;

    void Record(MutationKind kind, std::string_view key) noexcept override;
    void Drain(std::vector<ReclaimedKey>& out) noexcept override;
    [[nodiscard]] bool HasPending() const noexcept override;
    [[nodiscard]] bool IsRecording() const noexcept override;

    /// Entries the log refused because it was full, or could not allocate for.
    /// Monotonic for the life of the log, so "no events fired" stays
    /// distinguishable from "events fired and were dropped". Also exported as
    /// `fastcached_keyspace_reclaim_events_dropped_total` when a metrics sink
    /// was supplied — a count only this class can read is one no operator can.
    /// @return The cumulative dropped count.
    [[nodiscard]] std::size_t Dropped() const noexcept;

  private:
    /// Record one dropped entry, on both the local tally and the metrics sink.
    void CountDrop() noexcept;

    mutable std::mutex _mu;
    std::vector<ReclaimedKey> _pending;
    /// Mirrors `_pending.size()`, so both fast probes — is there anything to
    /// drain, and is there room to record — answer without the lock. Written
    /// under `_mu`; read outside it, where a stale value costs at most one
    /// deferred drain or one extra entry over the bound, which the re-check
    /// inside `Record` then rejects.
    std::atomic<std::size_t> _pendingCount { 0 };
    std::atomic<std::size_t> _dropped { 0 };
    IStorageMutationObserver* _observer;
    IMetricsSink* _metrics;
    std::size_t _capacity;
};

} // namespace FastCache
