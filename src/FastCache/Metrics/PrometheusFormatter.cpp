// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace FastCache
{

namespace
{

    /// One rendered metric: fully-qualified name, help text, kind and value.
    ///
    /// Distinct from `CounterDescriptor` because the storage rows below have no
    /// `IMetricsSink::Counter` to be described by — they come from a
    /// `StorageStats` field instead. What the two share is the *rendering*, which
    /// is the loop at the bottom.
    struct Metric
    {
        std::string_view name;
        std::string_view help;
        MetricType type;
        std::uint64_t value;
    };

    /// How many rows `AppendStorageMetrics` emits, for the one `reserve`.
    /// A loose estimate by design — it only sizes a buffer.
    constexpr std::size_t StorageMetricCount = 24;

    /// Append one metric's three exposition lines to `out`.
    /// @param out Destination.
    /// @param metric What to render.
    void Append(std::string& out, Metric const& metric)
    {
        out += std::format(
            "# HELP {0} {1}\n# TYPE {0} {2}\n{0} {3}\n", metric.name, metric.help, TypeName(metric.type), metric.value);
    }

} // namespace

/// Render the metrics a cache's own statistics carry.
///
/// Separate from the sink's counters because the two have different *sources*,
/// not because they render differently: these come from `StorageStats`, which is
/// authoritative for command and capacity numbers, and a process without a cache
/// has none of them. The rendering itself is `Append` either way.
/// @param out Destination.
/// @param stats The cache's statistics.
static void AppendStorageMetrics(std::string& out, StorageStats const& stats)
{
    using enum MetricType;

    auto const table = std::array {
        Metric {
            .name = "fastcached_cmd_get_total", .help = "GET commands processed.", .type = Counter, .value = stats.cmdGet },
        Metric { .name = "fastcached_cmd_set_total",
                 .help = "SET-family commands processed.",
                 .type = Counter,
                 .value = stats.cmdSet },
        Metric { .name = "fastcached_cmd_touch_total",
                 .help = "TOUCH commands processed.",
                 .type = Counter,
                 .value = stats.cmdTouch },
        Metric { .name = "fastcached_cmd_flush_total",
                 .help = "FLUSH commands processed.",
                 .type = Counter,
                 .value = stats.cmdFlush },
        Metric { .name = "fastcached_get_hits_total",
                 .help = "GET requests that found a live entry.",
                 .type = Counter,
                 .value = stats.getHits },
        Metric { .name = "fastcached_get_misses_total",
                 .help = "GET requests that found nothing.",
                 .type = Counter,
                 .value = stats.getMisses },
        Metric { .name = "fastcached_delete_hits_total",
                 .help = "DELETE requests that removed an entry.",
                 .type = Counter,
                 .value = stats.deleteHits },
        Metric { .name = "fastcached_delete_misses_total",
                 .help = "DELETE requests with no matching key.",
                 .type = Counter,
                 .value = stats.deleteMisses },
        Metric { .name = "fastcached_incr_hits_total",
                 .help = "INCR requests against a present key.",
                 .type = Counter,
                 .value = stats.incrHits },
        Metric { .name = "fastcached_incr_misses_total",
                 .help = "INCR requests with no matching key.",
                 .type = Counter,
                 .value = stats.incrMisses },
        Metric { .name = "fastcached_decr_hits_total",
                 .help = "DECR requests against a present key.",
                 .type = Counter,
                 .value = stats.decrHits },
        Metric { .name = "fastcached_decr_misses_total",
                 .help = "DECR requests with no matching key.",
                 .type = Counter,
                 .value = stats.decrMisses },
        Metric { .name = "fastcached_touch_hits_total",
                 .help = "TOUCH requests against a present key.",
                 .type = Counter,
                 .value = stats.touchHits },
        Metric { .name = "fastcached_touch_misses_total",
                 .help = "TOUCH requests with no matching key.",
                 .type = Counter,
                 .value = stats.touchMisses },
        Metric { .name = "fastcached_cas_hits_total",
                 .help = "CAS requests that matched and stored.",
                 .type = Counter,
                 .value = stats.casHits },
        Metric { .name = "fastcached_cas_misses_total",
                 .help = "CAS requests with no matching key.",
                 .type = Counter,
                 .value = stats.casMisses },
        Metric { .name = "fastcached_cas_badval_total",
                 .help = "CAS requests rejected on a token mismatch.",
                 .type = Counter,
                 .value = stats.casBadval },
        Metric { .name = "fastcached_write_errors_total",
                 .help = "Value writes that failed to persist (disk full, I/O error, corruption, read-only).",
                 .type = Counter,
                 .value = stats.writeErrors },
        Metric { .name = "fastcached_evictions_total",
                 .help = "Entries evicted to stay within the memory budget.",
                 .type = Counter,
                 .value = stats.evictions },
        Metric { .name = "fastcached_evicted_unfetched_total",
                 .help = "Entries evicted before ever being read.",
                 .type = Counter,
                 .value = stats.evictedUnfetched },
        Metric { .name = "fastcached_expired_unfetched_total",
                 .help = "Entries that expired before ever being read.",
                 .type = Counter,
                 .value = stats.expiredUnfetched },
        Metric { .name = "fastcached_items",
                 .help = "Live entries currently stored.",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(stats.itemCount) },
        Metric { .name = "fastcached_bytes_used",
                 .help = "Bytes currently stored.",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(stats.bytesUsed) },
        Metric { .name = "fastcached_bytes_limit",
                 .help = "Configured byte budget (0 = unbounded).",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(stats.bytesLimit) },
    };

    for (auto const& metric: table)
        Append(out, metric);
}

std::string RenderPrometheus(IMetricsSink const& metrics, MetricsSnapshot const& snapshot)
{
    std::string out;
    // Each metric renders ~3 lines (HELP/TYPE/value); ~200 bytes is a generous
    // per-row estimate, so one reserve avoids the handful of reallocations the
    // += loop would otherwise do on every scrape.
    out.reserve((StorageMetricCount + CounterTable.size() + 1) * 200);

    // Only when there is a cache. A worker running this same endpoint would
    // otherwise report an empty, unbounded one — zeroes that read as facts.
    if (snapshot.storage.has_value())
        AppendStorageMetrics(out, *snapshot.storage);

    // Every counter the sink knows, without exception. Exporting the *table*
    // rather than a hand-picked subset is the whole point: seven of the eleven
    // live counters used to be absent here, including all five the distributed-
    // compilation guide tells an operator to read.
    for (auto const& row: CounterTable)
        Append(out,
               Metric { .name = row.prometheusName,
                        .help = row.help,
                        .type = row.type,
                        .value = metrics.Read(row.counter) });

    // Uptime is neither the cache's nor the sink's: every process that serves
    // this endpoint has one, and a worker's is as useful as a daemon's.
    Append(out,
           Metric { .name = "fastcached_uptime_seconds",
                    .help = "Seconds since the process started.",
                    .type = MetricType::Gauge,
                    .value = static_cast<std::uint64_t>(snapshot.uptime.value.count()) });

    return out;
}

} // namespace FastCache
