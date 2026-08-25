// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/StorageTier.hpp>
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

    /// One per-tier metric: the series, and which `StorageStats` field feeds it.
    ///
    /// A projection rather than a value, because one row renders once per tier and
    /// the tiers are not known where the table is written. The alternative — a
    /// `Metric` per (series, tier) pair — would put the tier list in two places,
    /// and a tier added to one and not the other is a series that silently is not
    /// there, which is the failure `MetricsCatalog` exists to prevent.
    struct TierMetric
    {
        std::string_view name;                                  ///< Series name, without the label.
        std::string_view help;                                  ///< `# HELP` text.
        MetricType type;                                        ///< Counter or gauge.
        std::uint64_t (*project)(StorageStats const&) noexcept; ///< What to read off one tier.
    };

    /// The per-tier series, and only the ones that mean something per tier.
    ///
    /// **None of these is a fleet-wide total waiting to be summed**, and the
    /// `tier` label is the only honest way to read them. `LayeredStorage` mirrors
    /// every L2 entry it has touched into L1, so adding the two item counts counts
    /// the mirrored entries twice; adding the two hit counts is worse still, since
    /// L2 is consulted only when L1 missed. What each row answers is "how is THIS
    /// tier doing" — is the mirror populated, is the disk tier near its budget,
    /// which tier is evicting — and the cache's own totals stay on the
    /// unlabelled `fastcached_*` series `Snapshot()` feeds.
    ///
    /// The hit/miss split is left out entirely rather than published with that
    /// caveat: it is the number a dashboard is most likely to sum, and a cache
    /// serving every read would come out at 62%.
    constexpr std::array TierMetricTable {
        TierMetric { .name = "fastcached_tier_items",
                     .help = "Live entries this tier holds.",
                     .type = MetricType::Gauge,
                     .project = [](StorageStats const& s) noexcept { return static_cast<std::uint64_t>(s.itemCount); } },
        TierMetric { .name = "fastcached_tier_bytes_used",
                     .help = "Bytes this tier holds.",
                     .type = MetricType::Gauge,
                     .project = [](StorageStats const& s) noexcept { return static_cast<std::uint64_t>(s.bytesUsed); } },
        TierMetric { .name = "fastcached_tier_bytes_limit",
                     .help = "This tier's configured byte budget (0 = unbounded).",
                     .type = MetricType::Gauge,
                     .project = [](StorageStats const& s) noexcept { return static_cast<std::uint64_t>(s.bytesLimit); } },
        TierMetric { .name = "fastcached_tier_evictions_total",
                     .help = "Entries this tier dropped to stay within its budget.",
                     .type = MetricType::Counter,
                     .project = [](StorageStats const& s) noexcept { return s.evictions; } },
    };

} // namespace

/// Render what a machine's size looks like on the wire.
///
/// Gauges, every one: they describe a machine rather than count events, which is
/// why they arrive in a struct instead of through the counter-only sink.
/// @param out Destination.
/// @param host The machine's capacity.
static void AppendHostMetrics(std::string& out, HostCapacity const& host)
{
    using enum MetricType;

    auto const table = std::array {
        Metric { .name = "fastcache_node_logical_cores",
                 .help = "Schedulable hardware threads on this node.",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(host.logicalCores) },
        Metric { .name = "fastcache_node_memory_total_bytes",
                 .help = "Physical memory, or the container ceiling when that binds first.",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(host.totalMemoryBytes) },
        Metric { .name = "fastcache_node_disk_capacity_bytes",
                 .help = "Size of the filesystem this node compiles on.",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(host.diskCapacityBytes) },
        Metric { .name = "fastcache_node_disk_free_bytes",
                 .help = "Space on that filesystem an unprivileged process may still write.",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(host.diskFreeBytes) },
        Metric { .name = "fastcache_node_slots_configured",
                 .help = "Concurrent compiles this node advertises to the scheduler.",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(host.configuredSlots) },
        Metric { .name = "fastcache_node_slots_busy",
                 .help = "Compiles running right now. Sampled, not derived from the job "
                         "counters, which are incremented separately.",
                 .type = Gauge,
                 .value = static_cast<std::uint64_t>(host.busySlots) },
    };

    for (auto const& metric: table)
        Append(out, metric);
}

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

/// Render one series per tier that exists, each sample carrying a `tier` label.
///
/// `# HELP` and `# TYPE` are emitted once per series, with the samples following:
/// repeating them for every label value is what a parser rejects.
///
/// A tier the cache does not have contributes no line at all — not a zero. That
/// is the rule `MetricsSnapshot::storage` is optional for, one level down: a
/// dashboard reads `fastcached_tier_items{tier="disk"} 0` as a disk tier standing
/// empty, which is a different claim from a node that has no disk tier.
/// @param out Destination.
/// @param tiers The cache's per-tier statistics.
static void AppendTierMetrics(std::string& out, TieredStorageStats const& tiers)
{
    for (auto const& row: TierMetricTable)
    {
        auto wroteHeader = false;
        // Driven by the TIER table, not by a hand-written list of labels — the
        // rule this file already follows against `MetricsCatalog`, applied to the
        // other axis. A tier added to `StorageTier` appears here by being a row
        // rather than by somebody remembering a second place.
        for (auto const& tierRow: StorageTierTable)
        {
            auto const& stats = tiers[static_cast<std::size_t>(tierRow.tier)];
            if (!stats.has_value())
                continue;
            if (!wroteHeader)
            {
                out += std::format("# HELP {0} {1}\n# TYPE {0} {2}\n", row.name, row.help, TypeName(row.type));
                wroteHeader = true;
            }
            out += std::format("{}{{tier=\"{}\"}} {}\n", row.name, tierRow.name, row.project(*stats));
        }
    }
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

    // And the numbers that merged view had to discard. Independent of `storage`
    // rather than nested under it: a cache reports both, a process without one
    // reports neither, and neither field is derivable from the other.
    AppendTierMetrics(out, snapshot.storageTiers);

    // And only when the process is a compile node. The daemon leaves this absent
    // rather than reporting cores it does not schedule against.
    if (snapshot.host.has_value())
        AppendHostMetrics(out, *snapshot.host);

    // Every counter the sink knows, without exception. Exporting the *table*
    // rather than a hand-picked subset is the whole point: seven of the nine
    // live counters used to be absent here, including all five the distributed-
    // compilation guide tells an operator to read.
    for (auto const& row: CounterTable)
        Append(
            out,
            Metric { .name = row.prometheusName, .help = row.help, .type = row.type, .value = metrics.Read(row.counter) });

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
