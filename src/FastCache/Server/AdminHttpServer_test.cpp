// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

FastCache::Task<bool> WriteString(FastCache::ISocket* socket, std::string_view payload)
{
    auto const result = co_await socket->Write(FastCache::AsBytes(payload));
    co_return result.has_value();
}

FastCache::Task<std::string> ReadAvailable(FastCache::ISocket* socket)
{
    std::string out;
    while (true)
    {
        std::vector<std::byte> chunk(512);
        auto const result = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
        if (!result.has_value() || *result == 0)
            break;
        for (std::size_t i = 0; i < *result; ++i)
            out.push_back(static_cast<char>(chunk[i]));
        if (*result < chunk.size())
            break;
    }
    co_return out;
}

std::string Exchange(std::string_view request, FastCache::IMetricsSink const& metrics, FastCache::StorageStats stats)
{
    auto pair = FastCache::InMemorySocketPair::Create();
    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), request)));
    pair.client->ShutdownWrite();
    using namespace std::chrono_literals;
    auto provider = [&stats] {
        return FastCache::MetricsSnapshot { .storage = stats, .uptime = FastCache::Uptime { 7s } };
    };
    FastCache::SyncRun(FastCache::ServeAdminHttp(pair.server.get(), &metrics, provider));
    pair.server->Close();
    return FastCache::SyncRun(ReadAvailable(pair.client.get()));
}

} // namespace

TEST_CASE("AdminHttp: GET /healthz returns 200 OK", "[metrics][http]")
{
    FastCache::AtomicMetricsSink metrics;
    auto const response = Exchange("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n", metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(response.ends_with("OK\n"));
}

TEST_CASE("AdminHttp: GET /metrics returns Prometheus body", "[metrics][http]")
{
    FastCache::AtomicMetricsSink metrics;
    metrics.Increment(FastCache::IMetricsSink::Counter::ConnectionsTotal, 4);
    FastCache::StorageStats stats;
    stats.cmdGet = 42;
    auto const response = Exchange("GET /metrics HTTP/1.1\r\n\r\n", metrics, stats);
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
    REQUIRE(response.contains("Content-Type: text/plain; version=0.0.4\r\n"));
    REQUIRE(response.contains("fastcached_cmd_get_total 42\n"));
    REQUIRE(response.contains("fastcached_connections_total 4\n"));
    REQUIRE(response.contains("fastcached_uptime_seconds 7\n"));
}

TEST_CASE("AdminHttp: /metrics with a query string still routes", "[metrics][http]")
{
    FastCache::AtomicMetricsSink metrics;
    auto const response = Exchange("GET /metrics?foo=bar HTTP/1.1\r\n\r\n", metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_CASE("AdminHttp: unknown path returns 404", "[metrics][http]")
{
    FastCache::AtomicMetricsSink metrics;
    auto const response = Exchange("GET /nope HTTP/1.1\r\n\r\n", metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 404 Not Found\r\n"));
}

TEST_CASE("AdminHttp: non-GET method returns 405", "[metrics][http]")
{
    FastCache::AtomicMetricsSink metrics;
    auto const response = Exchange("POST /metrics HTTP/1.1\r\n\r\n", metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
}
