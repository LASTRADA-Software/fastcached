// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Metrics/IMetricsSink.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace FastCache
{

/// Prometheus metric kind. Counters are monotonic; gauges may go up or down.
enum class MetricType : std::uint8_t
{
    Counter,
    Gauge,
};

/// The kind's name, as the exposition format spells it.
/// @param type The metric kind.
/// @return "counter" or "gauge".
[[nodiscard]] constexpr std::string_view TypeName(MetricType type) noexcept
{
    return type == MetricType::Gauge ? "gauge" : "counter";
}

/// Everything that is true of a counter regardless of its value.
///
/// ## Why this table exists
///
/// A counter used to be described in two unrelated places: the
/// `IMetricsSink::Counter` enumerator, and — only if somebody remembered — a row
/// in `PrometheusFormatter`'s render table giving its exported name, help text
/// and kind. Nothing tied the two together, and a counter present in one but not
/// the other is not a build error. It is a counter that increments and is never
/// exported.
///
/// That is not hypothetical, and it was not a corner case. **Seven of the eleven
/// live counters were missing from that table**: both TLS splits, and all five
/// `dispatch_*` — which `docs/getting-started/distributed-compilation.md`
/// documents as the things to read off `/metrics` when distribution misbehaves,
/// with a table naming each one. An operator working the commonest setup failure
/// ("no worker matches this toolchain") followed that guide to an endpoint that
/// had never carried a single one of those series.
///
/// So the row is the definition. The renderer walks this table, and
/// `CoversEveryCounter()` is `static_assert`ed below — a counter added to the enum
/// without a row here fails to compile, which is the only version of this that
/// cannot drift again.
struct CounterDescriptor
{
    IMetricsSink::Counter counter {};        ///< The enumerator this row describes.
    std::string_view prometheusName;         ///< Fully-qualified exported name.
    std::string_view help;                   ///< One-line `# HELP` text.
    MetricType type { MetricType::Counter }; ///< `# TYPE`.
};

/// One row per `IMetricsSink::Counter`, in enumerator order.
///
/// The order is not load-bearing for correctness — `DescriptorOf` indexes — but
/// keeping it aligned with the enum is what makes a missing row visible to a
/// reader as well as to the `static_assert`.
inline constexpr std::array<CounterDescriptor, static_cast<std::size_t>(IMetricsSink::Counter::Last)> CounterTable { {
    { .counter = IMetricsSink::Counter::ConnectionsTotal,
      .prometheusName = "fastcached_connections_total",
      .help = "Connections accepted since start.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::ConnectionsAdmissionRejected,
      .prometheusName = "fastcached_connections_rejected_total",
      .help = "Connections refused by admission control.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::ConnectionsTotalTls,
      .prometheusName = "fastcached_connections_total_tls",
      .help = "Subset of connections_total that arrived on a TLS-flagged bind; "
              "plaintext traffic is the difference between the two.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::ConnectionsAdmissionRejectedTls,
      .prometheusName = "fastcached_connections_rejected_tls",
      .help = "Subset of connections_rejected that arrived on a TLS-flagged bind.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesGranted,
      .prometheusName = "fastcached_dispatch_leases_granted_total",
      .help = "Lease requests answered with a worker; work is being distributed.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesNoWorker,
      .prometheusName = "fastcached_dispatch_leases_no_worker_total",
      .help = "Lease requests refused because no registered worker matched the "
              "toolchain: the fleet is misconfigured rather than busy.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesNoCapacity,
      .prometheusName = "fastcached_dispatch_leases_no_capacity_total",
      .help = "Lease requests refused because every matching worker was full: "
              "the fleet is too small. Never sum this with no_worker.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchLeasesDuplicate,
      .prometheusName = "fastcached_dispatch_leases_duplicate_total",
      .help = "Lease requests refused because another client already held a lease "
              "for this key; duplicate-work suppression, not a failure.",
      .type = MetricType::Counter },
    { .counter = IMetricsSink::Counter::DispatchWorkerRegistrations,
      .prometheusName = "fastcached_dispatch_worker_registrations_total",
      .help = "Worker registrations accepted. A steady rise means heartbeats are "
              "not arriving.",
      .type = MetricType::Counter },
} };

/// Whether `CounterTable` has exactly one row per enumerator, in order.
///
/// Checked at compile time below rather than by a test, because the failure this
/// prevents is a counter that exports nothing — which no test asserts the absence
/// of unless somebody thinks to write one for the counter they just added, and
/// that is precisely the step that was missed.
/// @return True when every enumerator has its own row.
[[nodiscard]] consteval bool CoversEveryCounter() noexcept
{
    for (std::size_t index = 0; index < CounterTable.size(); ++index)
        if (static_cast<std::size_t>(CounterTable[index].counter) != index)
            return false;
    return true;
}

static_assert(CoversEveryCounter(), "CounterTable must hold one row per IMetricsSink::Counter, in enumerator order");

/// The row describing `counter`.
///
/// `Last` is not a metric and has no row; it yields nullptr rather than a
/// placeholder, so a caller that reached here with it fails visibly.
/// @param counter The counter to look up.
/// @return Its descriptor, or nullptr for `Last` or an out-of-range value.
[[nodiscard]] constexpr CounterDescriptor const* DescriptorOf(IMetricsSink::Counter counter) noexcept
{
    auto const index = static_cast<std::size_t>(counter);
    if (index >= CounterTable.size())
        return nullptr;
    return &CounterTable[index];
}

} // namespace FastCache
