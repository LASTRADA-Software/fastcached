// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <format>
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

    auto const body = RenderPrometheus(metrics, MetricsSnapshot { .storage = stats, .uptime = Uptime { 42s } });

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
    // Seven of the eleven live counters were absent from the renderer — both TLS
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

    auto const body = RenderPrometheus(metrics, MetricsSnapshot { .storage = StorageStats {}, .uptime = Uptime { 0s } });

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
