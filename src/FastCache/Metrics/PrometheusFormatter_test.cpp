// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>

using namespace FastCache;
using namespace std::chrono_literals;

TEST_CASE("RenderPrometheus emits HELP/TYPE/value triples", "[metrics][prometheus]")
{
    AtomicMetricsSink metrics;
    metrics.Increment(IMetricsSink::Counter::ConnectionsTotal, 7);
    metrics.Increment(IMetricsSink::Counter::ConnectionsAdmissionRejected, 2);

    StorageStats stats;
    stats.cmdGet = 100;
    stats.getHits = 80;
    stats.getMisses = 20;
    stats.itemCount = 5;
    stats.bytesUsed = 4096;
    stats.bytesLimit = 65536;
    stats.evictions = 3;
    stats.writeErrors = 9;

    auto const body =
        RenderPrometheus(metrics, MetricsSnapshot { .storage = stats, .host = std::nullopt, .uptime = Uptime { 42s } });

    SECTION("counter from the storage snapshot")
    {
        CHECK(body.contains("# TYPE fastcached_cmd_get_total counter\n"));
        CHECK(body.contains("fastcached_cmd_get_total 100\n"));
        CHECK(body.contains("fastcached_get_hits_total 80\n"));
        CHECK(body.contains("fastcached_get_misses_total 20\n"));
        CHECK(body.contains("fastcached_evictions_total 3\n"));
        CHECK(body.contains("# TYPE fastcached_write_errors_total counter\n"));
        CHECK(body.contains("fastcached_write_errors_total 9\n"));
    }
    SECTION("gauges from the storage snapshot")
    {
        CHECK(body.contains("# TYPE fastcached_bytes_used gauge\n"));
        CHECK(body.contains("fastcached_bytes_used 4096\n"));
        CHECK(body.contains("fastcached_items 5\n"));
        CHECK(body.contains("fastcached_bytes_limit 65536\n"));
    }
    SECTION("connection counters from the sink")
    {
        CHECK(body.contains("fastcached_connections_total 7\n"));
        CHECK(body.contains("fastcached_connections_rejected_total 2\n"));
    }
    SECTION("uptime gauge")
    {
        CHECK(body.contains("# TYPE fastcached_uptime_seconds gauge\n"));
        CHECK(body.contains("fastcached_uptime_seconds 42\n"));
    }
    SECTION("every metric carries HELP text")
    {
        CHECK(body.contains("# HELP fastcached_cmd_get_total "));
    }
}

TEST_CASE("Every counter the sink knows reaches the scrape", "[metrics][prometheus]")
{
    // The case that would have caught the defect this table exists to prevent.
    // Seven of the nine live counters were absent from the renderer — both TLS
    // splits and all five `dispatch_*`, the last of which
    // docs/getting-started/distributed-compilation.md names one by one as what to
    // read off /metrics when distribution misbehaves. Every one of those series
    // was missing from the endpoint that guide sends an operator to.
    //
    // Asserted over `CounterTable` rather than against a list written out here,
    // because a hand-written list is the thing that went stale: it would have to
    // be updated by the same person who forgot the renderer.
    AtomicMetricsSink metrics;

    // A distinct value per counter, so a row rendering another row's value cannot
    // pass — which a table of near-identical rows makes the likely slip.
    for (auto const& row: CounterTable)
        metrics.Increment(row.counter, static_cast<std::uint64_t>(row.counter) + 1);

    auto const body = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = StorageStats {}, .host = std::nullopt, .uptime = Uptime { 0s } });

    for (auto const& row: CounterTable)
    {
        INFO("counter " << row.prometheusName);
        CHECK(body.contains(std::format("# HELP {} ", row.prometheusName)));
        CHECK(body.contains(std::format("# TYPE {} {}\n", row.prometheusName, TypeName(row.type))));
        CHECK(body.contains(std::format("\n{} {}\n", row.prometheusName, static_cast<std::uint64_t>(row.counter) + 1)));
    }
}

TEST_CASE("A counter's exported name is unique", "[metrics][prometheus]")
{
    // Two rows sharing a name render two samples for one series, which a scraper
    // rejects outright — the whole body is dropped, so one duplicated row takes
    // every other metric with it. Cheap to assert, and the natural slip when a row
    // is added by copying its neighbour.
    for (auto const& row: CounterTable)
    {
        auto matches = 0;
        for (auto const& other: CounterTable)
            if (other.prometheusName == row.prometheusName)
                ++matches;

        INFO("counter " << row.prometheusName);
        CHECK(matches == 1);
    }
}

TEST_CASE("A process with no cache renders no cache metrics", "[metrics][prometheus]")
{
    // `fastcache-compile-node` serves this same endpoint and has no storage. A
    // default-constructed `StorageStats` would report `fastcached_items 0` and
    // `fastcached_bytes_limit 0` -- an empty, unbounded cache -- which a dashboard
    // reads as a fact rather than as an absence. Absent means absent.
    AtomicMetricsSink metrics;
    metrics.Increment(IMetricsSink::Counter::WorkerJobsCompleted, 3);

    auto const body = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .uptime = Uptime { 9s } });

    CHECK_FALSE(body.contains("fastcached_items"));
    CHECK_FALSE(body.contains("fastcached_bytes_limit"));
    CHECK_FALSE(body.contains("fastcached_cmd_get_total"));

    // What it does carry: every sink counter, and the uptime every process has.
    CHECK(body.contains("fastcache_worker_jobs_completed_total 3\n"));
    CHECK(body.contains("fastcached_uptime_seconds 9\n"));
    for (auto const& row: CounterTable)
        CHECK(body.contains(std::format("# TYPE {} ", row.prometheusName)));
}

TEST_CASE("A compile node reports its size, and a cache daemon does not", "[metrics][prometheus]")
{
    // "Is this node pulling its weight" is unanswerable without knowing how big it
    // is, so a worker's capacity is what an operator most wants off its scrape --
    // and it is what PR 8's resource-aware scheduling will weigh.
    AtomicMetricsSink metrics;

    auto const withHost = RenderPrometheus(metrics,
                                           MetricsSnapshot { .storage = std::nullopt,
                                                             .host = HostCapacity { .logicalCores = 16,
                                                                                    .configuredSlots = 14,
                                                                                    .totalMemoryBytes = 68719476736ULL,
                                                                                    .diskCapacityBytes = 500107862016ULL,
                                                                                    .diskFreeBytes = 123456789ULL,
                                                                                    .busySlots = 3 },
                                                             .uptime = Uptime { 1s } });

    CHECK(withHost.contains("# TYPE fastcache_node_logical_cores gauge"));
    CHECK(withHost.contains("fastcache_node_logical_cores 16"));
    CHECK(withHost.contains("fastcache_node_memory_total_bytes 68719476736"));
    CHECK(withHost.contains("fastcache_node_disk_free_bytes 123456789"));
    CHECK(withHost.contains("fastcache_node_slots_configured 14"));
    CHECK(withHost.contains("fastcache_node_slots_busy 3"));

    // The daemon leaves it absent rather than reporting cores it does not schedule
    // against. Absent means the series is missing, not present and zero -- a zero
    // would read as a machine with no cores.
    auto const withoutHost = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = StorageStats {}, .host = std::nullopt, .uptime = Uptime { 1s } });
    CHECK_FALSE(withoutHost.contains("fastcache_node_logical_cores"));
    CHECK_FALSE(withoutHost.contains("fastcache_node_slots_busy"));
}

TEST_CASE("A tiered cache renders one labelled sample per tier", "[metrics][prometheus][storage-tier]")
{
    // The numbers `MetricsSnapshot::storage` cannot carry. `LayeredStorage`
    // reports its canonical lower tier's item count, bytes and budget with the
    // composite's hit/miss patched over them, so a node's whole in-memory tier is
    // missing from a scrape that reads only that field.
    AtomicMetricsSink metrics;

    TieredStorageStats tiers {};
    tiers[static_cast<std::size_t>(StorageTier::Memory)] =
        StorageStats { .itemCount = 11, .bytesUsed = 2048, .bytesLimit = 4096, .evictions = 7 };
    tiers[static_cast<std::size_t>(StorageTier::Disk)] =
        StorageStats { .itemCount = 900, .bytesUsed = 1'000'000, .bytesLimit = 0, .evictions = 1 };

    auto const body = RenderPrometheus(
        metrics,
        MetricsSnapshot { .storage = std::nullopt, .storageTiers = tiers, .host = std::nullopt, .uptime = Uptime { 1s } });

    SECTION("each tier's own numbers, under its own label")
    {
        // Every value distinct, so a row rendering its neighbour's field cannot
        // pass -- the slip a table of near-identical rows invites.
        CHECK(body.contains("fastcached_tier_items{tier=\"memory\"} 11\n"));
        CHECK(body.contains("fastcached_tier_items{tier=\"disk\"} 900\n"));
        CHECK(body.contains("fastcached_tier_bytes_used{tier=\"memory\"} 2048\n"));
        CHECK(body.contains("fastcached_tier_bytes_used{tier=\"disk\"} 1000000\n"));
        CHECK(body.contains("fastcached_tier_bytes_limit{tier=\"memory\"} 4096\n"));
        CHECK(body.contains("fastcached_tier_bytes_limit{tier=\"disk\"} 0\n"));
        CHECK(body.contains("fastcached_tier_evictions_total{tier=\"memory\"} 7\n"));
        CHECK(body.contains("fastcached_tier_evictions_total{tier=\"disk\"} 1\n"));
    }
    SECTION("HELP and TYPE appear once per series, not once per label")
    {
        // Repeating them per label value is what a scraper rejects, and it takes
        // the whole body with it rather than the one series.
        for (auto const& name: { "fastcached_tier_items",
                                 "fastcached_tier_bytes_used",
                                 "fastcached_tier_bytes_limit",
                                 "fastcached_tier_evictions_total" })
        {
            INFO("series " << name);
            auto count = 0;
            for (auto pos = body.find(std::format("# TYPE {} ", name)); pos != std::string::npos;
                 pos = body.find(std::format("# TYPE {} ", name), pos + 1))
                ++count;
            CHECK(count == 1);
        }
    }
}

TEST_CASE("A memory-only cache renders no disk tier at all", "[metrics][prometheus][storage-tier]")
{
    // Absent is not zero, one level below the rule `MetricsSnapshot::storage`
    // already follows: `fastcached_tier_items{tier="disk"} 0` says a disk tier is
    // standing empty, and a node configured without one has no such tier to be
    // empty. A dashboard cannot tell those apart from a zero.
    AtomicMetricsSink metrics;

    TieredStorageStats tiers {};
    tiers[static_cast<std::size_t>(StorageTier::Memory)] = StorageStats { .itemCount = 4 };

    auto const body = RenderPrometheus(
        metrics,
        MetricsSnapshot { .storage = std::nullopt, .storageTiers = tiers, .host = std::nullopt, .uptime = Uptime { 1s } });

    CHECK(body.contains("fastcached_tier_items{tier=\"memory\"} 4\n"));
    CHECK_FALSE(body.contains("tier=\"disk\""));
}

TEST_CASE("A process with no cache renders no tier series", "[metrics][prometheus][storage-tier]")
{
    // The default `MetricsSnapshot` leaves every tier absent, so a worker that
    // never started a cache tier emits no `fastcached_tier_*` line -- not even a
    // `# HELP` with nothing under it, which a scraper accepts and a dashboard
    // draws as a series that has stopped reporting.
    AtomicMetricsSink metrics;
    auto const body = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .uptime = Uptime { 1s } });

    CHECK_FALSE(body.contains("fastcached_tier_"));
}

TEST_CASE("Whether a node has a shared cache is named, not inferred from a counter", "[metrics][prometheus]")
{
    // #214's operator-facing half. The upstream store counters are cumulative, so a
    // node with no shared cache and a node with one it has not written to yet both
    // report zero -- neither counter can answer "is there an upstream at all".
    //
    // A gauge rather than omitting the counter rows: the counters are exported in
    // full on purpose, and a per-counter "does this apply?" predicate is the
    // mechanism that once left seven of nine live counters unexported.
    AtomicMetricsSink metrics;

    // Matched on the TYPE line rather than the bare name: the failure counter's own
    // help text names this gauge, so a substring check finds it in a scrape that
    // never rendered it.
    auto const absent = RenderPrometheus(metrics, MetricsSnapshot {});
    CHECK_FALSE(absent.contains("# TYPE fastcache_node_upstream_configured"));

    auto const without = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .upstreamConfigured = false });
    CHECK(without.contains("# TYPE fastcache_node_upstream_configured gauge"));
    CHECK(without.contains("fastcache_node_upstream_configured 0"));

    auto const with = RenderPrometheus(
        metrics, MetricsSnapshot { .storage = std::nullopt, .host = std::nullopt, .upstreamConfigured = true });
    CHECK(with.contains("fastcache_node_upstream_configured 1"));

    // The counters stay exported either way -- absent is modelled in the snapshot,
    // never by dropping a counter row.
    CHECK(absent.contains("fastcache_node_cache_upstream_store_failures_total"));
    CHECK(without.contains("fastcache_node_cache_upstream_store_failures_total"));
}
