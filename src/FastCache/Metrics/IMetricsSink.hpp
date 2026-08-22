// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Counter-style metrics sink. The cache engine, server, and protocol
/// handlers increment counters via this interface. Snapshot() is read by
/// the `stats` family of commands.
///
/// Designed thin on purpose: only counters today; histograms and gauges
/// come later if/when needed. Implementations must be thread-safe.
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
        CmdGet = 0,
        CmdSet,
        CmdDelete,
        GetHits,
        GetMisses,
        Evictions,
        ConnectionsTotal,
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
        BytesIn,
        BytesOut,
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

/// Convert a counter to its canonical stats-line name (used by memcached
/// `stats` and Redis `INFO`).
/// @param counter Counter id.
/// @return Lowercase canonical name (never empty).
[[nodiscard]] constexpr std::string_view ToStringView(IMetricsSink::Counter counter) noexcept
{
    switch (counter)
    {
        case IMetricsSink::Counter::CmdGet:
            return "cmd_get";
        case IMetricsSink::Counter::CmdSet:
            return "cmd_set";
        case IMetricsSink::Counter::CmdDelete:
            return "cmd_delete";
        case IMetricsSink::Counter::GetHits:
            return "get_hits";
        case IMetricsSink::Counter::GetMisses:
            return "get_misses";
        case IMetricsSink::Counter::Evictions:
            return "evictions";
        case IMetricsSink::Counter::ConnectionsTotal:
            return "connections_total";
        case IMetricsSink::Counter::ConnectionsAdmissionRejected:
            return "connections_rejected";
        case IMetricsSink::Counter::ConnectionsTotalTls:
            return "connections_total_tls";
        case IMetricsSink::Counter::ConnectionsAdmissionRejectedTls:
            return "connections_rejected_tls";
        case IMetricsSink::Counter::BytesIn:
            return "bytes_in";
        case IMetricsSink::Counter::BytesOut:
            return "bytes_out";
        case IMetricsSink::Counter::DispatchLeasesGranted:
            return "dispatch_leases_granted";
        case IMetricsSink::Counter::DispatchLeasesNoWorker:
            return "dispatch_leases_no_worker";
        case IMetricsSink::Counter::DispatchLeasesNoCapacity:
            return "dispatch_leases_no_capacity";
        case IMetricsSink::Counter::DispatchLeasesDuplicate:
            return "dispatch_leases_duplicate";
        case IMetricsSink::Counter::DispatchWorkerRegistrations:
            return "dispatch_worker_registrations";
        case IMetricsSink::Counter::Last:
            return "<last>";
    }
    return "<unknown>";
}

} // namespace FastCache
