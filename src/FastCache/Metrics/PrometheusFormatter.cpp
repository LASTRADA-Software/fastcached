// SPDX-License-Identifier: Apache-2.0
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

    /// Prometheus metric kind. Counters are monotonic; gauges may go up or down.
    enum class MetricType : std::uint8_t
    {
        Counter,
        Gauge,
    };

    /// One rendered metric: fully-qualified name, help text, kind and value.
    struct Metric
    {
        std::string_view name;
        std::string_view help;
        MetricType type;
        std::uint64_t value;
    };

    [[nodiscard]] constexpr std::string_view TypeName(MetricType type) noexcept
    {
        return type == MetricType::Gauge ? "gauge" : "counter";
    }

} // namespace

std::string RenderPrometheus(IMetricsSink const& metrics, MetricsSnapshot const& snapshot)
{
    using enum MetricType;
    using Sink = IMetricsSink::Counter;
    auto const& stats = snapshot.storage;

    // Single source of truth: command/capacity metrics from the storage
    // snapshot, connection metrics from the sink. Adding a row here is the only
    // step needed to expose a new metric.
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
        Metric { .name = "fastcached_connections_total",
                 .help = "Connections accepted since start.",
                 .type = Counter,
                 .value = metrics.Read(Sink::ConnectionsTotal) },
        Metric { .name = "fastcached_connections_rejected_total",
                 .help = "Connections refused by admission control.",
                 .type = Counter,
                 .value = metrics.Read(Sink::ConnectionsAdmissionRejected) },
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
        Metric { .name = "fastcached_uptime_seconds",
                 .help = "Seconds since the daemon started.",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(snapshot.uptime.value.count()) },
    };

    std::string out;
    // Each metric renders ~3 lines (HELP/TYPE/value); ~200 bytes is a generous
    // per-row estimate, so one reserve avoids the handful of reallocations the
    // += loop would otherwise do on every scrape.
    out.reserve(table.size() * 200);
    for (auto const& metric: table)
        out += std::format(
            "# HELP {0} {1}\n# TYPE {0} {2}\n{0} {3}\n", metric.name, metric.help, TypeName(metric.type), metric.value);
    return out;
}

} // namespace FastCache
