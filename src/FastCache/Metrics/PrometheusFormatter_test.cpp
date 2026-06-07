// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

    auto const body = RenderPrometheus(metrics, MetricsSnapshot { .storage = stats, .uptime = Uptime { 42s } });

    SECTION("counter from the storage snapshot")
    {
        CHECK(body.contains("# TYPE fastcached_cmd_get_total counter\n"));
        CHECK(body.contains("fastcached_cmd_get_total 100\n"));
        CHECK(body.contains("fastcached_get_hits_total 80\n"));
        CHECK(body.contains("fastcached_get_misses_total 20\n"));
        CHECK(body.contains("fastcached_evictions_total 3\n"));
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
