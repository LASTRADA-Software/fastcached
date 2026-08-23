// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Counter-style metrics sink for facts the *connection* layer knows.
///
/// Designed thin on purpose: only counters today; histograms and gauges come
/// later if/when needed. Implementations must be thread-safe.
///
/// ## What belongs here, and what does not
///
/// Command counts, hit/miss splits and evictions do **not**: those are
/// `StorageStats`, produced by the storage that actually performs them, and that
/// is what `stats`, `INFO` and `/metrics` all report. This enum used to carry a
/// second set of enumerators for the same concepts — `CmdGet`, `CmdSet`,
/// `CmdDelete`, `GetHits`, `GetMisses`, `Evictions`, `BytesIn`, `BytesOut` — and
/// not one of them was incremented anywhere in the tree, so each exported a
/// permanent zero under a name an operator would reasonably read as the real
/// count. They are removed rather than wired up: a second source of truth for a
/// number `StorageStats` already owns is the thing to avoid, not to complete.
///
/// Every remaining enumerator needs a row in `Metrics/MetricsCatalog.hpp`, which
/// `static_assert`s that it has one — that is what makes a new counter reach
/// `/metrics` by construction rather than by somebody remembering.
class IMetricsSink
{
  public:
    IMetricsSink() = default;
    IMetricsSink(IMetricsSink const&) = delete;
    IMetricsSink(IMetricsSink&&) = delete;
    IMetricsSink& operator=(IMetricsSink const&) = delete;
    IMetricsSink& operator=(IMetricsSink&&) = delete;
    virtual ~IMetricsSink() = default;

    enum class Counter : std::uint8_t
    {
        ConnectionsTotal = 0,
        ConnectionsAdmissionRejected,
        /// Subset of `ConnectionsTotal` that came in on a TLS-flagged
        /// bind. Lets operators attribute traffic to plaintext vs TLS
        /// without a per-bind label dimension (the IMetricsSink
        /// interface is intentionally counter-only, no labels). Sum:
        ///   connections_total_plaintext = ConnectionsTotal − ConnectionsTotalTls
        ConnectionsTotalTls,
        /// Subset of `ConnectionsAdmissionRejected` that came in on a
        /// TLS-flagged bind. Pairs with ConnectionsTotalTls.
        ConnectionsAdmissionRejectedTls,
        /// Lease requests the scheduler answered with a worker. The numerator of
        /// "is distribution actually happening", and meaningless without the
        /// refusals below it -- a fleet where every lease is granted and a fleet
        /// nobody asks look identical on this counter alone.
        DispatchLeasesGranted,
        /// Lease requests refused because no registered worker matched the
        /// toolchain. The counter that says a fleet is MISCONFIGURED rather than
        /// busy: it rises when workers are up but nobody can use them, which is
        /// the failure mode a fingerprint mismatch produces and the one that is
        /// otherwise invisible from both ends.
        DispatchLeasesNoWorker,
        /// Lease requests refused because every matching worker was full. Rising
        /// here means the fleet is too small, which is a different decision from
        /// the line above and must not be summed with it.
        DispatchLeasesNoCapacity,
        /// Lease requests refused because another client already held a lease for
        /// this key. Not a failure: it is duplicate-work suppression doing its
        /// job, and the clients it refuses compile locally.
        DispatchLeasesDuplicate,
        /// Workers currently registered, as a running total of registrations
        /// accepted. A gauge would be the better shape and this interface is
        /// counter-only by design, so this counts events rather than membership:
        /// it rises on every re-registration, which is itself the signal worth
        /// watching -- a fleet that re-registers constantly is a fleet whose
        /// heartbeats are not arriving.
        DispatchWorkerRegistrations,

        /// Compiles a worker began. With `WorkerJobsCompleted` this is also the
        /// in-flight count — two monotone counters rather than a gauge, which this
        /// interface deliberately does not have, and their difference is what
        /// "slots in use" means. Slots *configured* is not here at all: it is
        /// configuration rather than a measurement, and pushing it through a
        /// counter would mean incrementing to its value at startup.
        WorkerJobsStarted,
        /// Compiles that finished, whatever the compiler concluded. A compiler
        /// that ran and rejected the code did its job; that is the client's
        /// answer, not a worker failure, and it is deliberately not a refusal.
        WorkerJobsCompleted,
        /// Total wall time spent compiling, in milliseconds.
        ///
        /// The `_sum` half of a duration, with `WorkerJobsCompleted` as `_count` —
        /// which is exactly how a Prometheus histogram reports one, so
        /// `rate(sum)/rate(count)` is the average compile time and this interface
        /// stays counter-only. A gauge would answer a different and less useful
        /// question: the duration of whichever compile happened to finish last.
        WorkerCompileMillisTotal,

        /// Jobs refused because no compiler here matches the client's fingerprint.
        /// The worker's own half of `DispatchLeasesNoWorker`: rising here means
        /// the fleet is misconfigured, and it is the commonest setup failure.
        WorkerJobsRefusedUnknownFingerprint,
        /// Jobs refused over an argument this worker will not pass to a compiler.
        WorkerJobsRefusedRejectedArgument,
        /// Jobs refused because the scratch directory could not be prepared.
        /// An operational fault — a full or read-only disk — and nothing the
        /// client or the fleet's configuration can fix.
        WorkerJobsRefusedScratchUnavailable,
        /// Jobs refused because the compiler could not be spawned at all.
        /// Distinct from a compiler that ran and failed: this one says the
        /// toolchain this worker advertises is not actually usable here.
        WorkerJobsRefusedSpawnFailed,
        /// Jobs refused because every slot was busy.
        ///
        /// Not a fault and deliberately its own counter: it is the worker's half
        /// of `DispatchLeasesNoCapacity`, and summing it with the four above would
        /// hide a misconfigured toolchain behind a busy machine — the same reason
        /// the scheduler splits no-worker from no-capacity.
        WorkerJobsRefusedNoSlot,

        /// Bytes of request payload read from clients, and of reply written back.
        /// The pair is what says whether a codec negotiation is doing anything:
        /// preprocessed text in against object bytes out.
        WorkerBytesReceived,
        WorkerBytesReturned,

        /// The node's own cache tier, which exists so a local rebuild on a slow or
        /// bad network does not go to the wire at all.
        ///
        /// Hits and misses are the node's OWN tier, before any upstream is asked;
        /// `NodeCacheUpstreamHits` is how often the shared cache answered what this
        /// node could not. The three together are what say whether the local tier is
        /// earning its disk: a high upstream-hit rate against a low local-hit rate is
        /// a tier too small to hold this machine's working set, which is a different
        /// problem from a fleet that is missing a lot.
        NodeCacheHits,
        NodeCacheMisses,
        NodeCacheUpstreamHits,

        /// A value the shared cache supplied that the local tier then refused. Costs
        /// one future round trip rather than a build, so it is counted rather than
        /// reported -- but a rate that stays high means the tier is misconfigured
        /// (unwritable path, a cap below the objects being stored) and is silently
        /// doing nothing.
        NodeCacheFillFailures,

        /// A local write that failed. Distinct from the fill failure above because
        /// this one IS reported to the client: it is the write that must not be lost.
        NodeCacheStoreFailures,

        /// What the fleet got. Best-effort by contract -- the local write already
        /// succeeded -- so a failure here costs the fleet a shared entry and costs
        /// this machine nothing. Split from the local counters precisely so an
        /// operator can tell "my node is fine, the fleet is unreachable" from "my
        /// node is broken", which are different things to go and fix.
        NodeCacheUpstreamStores,
        NodeCacheUpstreamStoreFailures,

        Last,
    };

    /// Increment the named counter by 1 (or `by`).
    virtual void Increment(Counter counter, std::uint64_t by = 1) noexcept = 0;

    /// Read the current value of a counter.
    [[nodiscard]] virtual std::uint64_t Read(Counter counter) const noexcept = 0;
};

/// Default atomic-counter sink.
class AtomicMetricsSink final: public IMetricsSink
{
  public:
    void Increment(Counter counter, std::uint64_t by = 1) noexcept override
    {
        auto const idx = static_cast<std::size_t>(counter);
        if (idx >= static_cast<std::size_t>(Counter::Last))
            return;
        _counters[idx].fetch_add(by, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t Read(Counter counter) const noexcept override
    {
        auto const idx = static_cast<std::size_t>(counter);
        if (idx >= static_cast<std::size_t>(Counter::Last))
            return 0;
        return _counters[idx].load(std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> _counters[static_cast<std::size_t>(Counter::Last)] {};
};

} // namespace FastCache
