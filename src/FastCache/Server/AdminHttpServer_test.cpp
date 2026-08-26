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
#include <memory>
#include <optional>
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
        return FastCache::MetricsSnapshot { .storage = stats, .host = std::nullopt, .uptime = FastCache::Uptime { 7s } };
    };
    FastCache::SyncRun(FastCache::ServeAdminHttp(pair.server.get(), &metrics, provider));
    pair.server->Close();
    return FastCache::SyncRun(ReadAvailable(pair.client.get()));
}

/// An `ISocket` that never returns more than `limit` bytes from one `Read`.
///
/// The condition this exists for cannot be created over `InMemorySocketPair`,
/// which is a byte pipe: two writes coalesce, so one 1024-byte read always gets
/// the whole request. On a real socket the request routinely arrives in more than
/// one segment, and a server that stops reading at the request line then leaves
/// the headers unread in the receive queue -- which makes `close()` send RST
/// instead of FIN. Stating the segmentation directly is the same device the
/// launcher's fake `IPathResolver` uses for aliasing it cannot create on the host.
class ShortReadSocket final: public FastCache::ISocket
{
  public:
    ShortReadSocket(FastCache::ISocket& inner, std::size_t limit) noexcept:
        _inner { inner },
        _limit { limit }
    {
    }

    [[nodiscard]] FastCache::IoAwaitable Read(std::span<std::byte> buffer) override
    {
        return _inner.Read(buffer.subspan(0, std::min(buffer.size(), _limit)));
    }

    [[nodiscard]] FastCache::IoAwaitable Write(std::span<std::byte const> buffer) override
    {
        return _inner.Write(buffer);
    }

    [[nodiscard]] FastCache::IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                                       std::shared_ptr<void const> keepAlive = {}) override
    {
        return _inner.WriteVectored(segments, std::move(keepAlive));
    }

    void Close() noexcept override
    {
        _inner.Close();
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _inner.IsClosed();
    }

  private:
    FastCache::ISocket& _inner;
    std::size_t _limit;
};

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

TEST_CASE("AdminHttp: a request split across reads is consumed to the end", "[metrics][http]")
{
    // The defect this pins: `ReadRequestHead` used to stop at the FIRST CRLF, so a
    // request that arrived in more than one segment left `Host:` and `Connection:`
    // unread in the receive queue. Closing a socket with unread received data makes
    // the kernel send RST rather than FIN, so a monitoring probe scraping /metrics
    // saw `Connection reset by peer` -- intermittently, decided by nothing but how
    // the request happened to be segmented. An endpoint whose whole job is to report
    // that a worker is healthy must not be the thing that looks unhealthy.
    //
    // Observed here as leftover bytes, which is the same fact one layer up from the
    // reset: what is left unread is exactly what makes the kernel choose RST.
    auto pair = FastCache::InMemorySocketPair::Create();
    REQUIRE(
        FastCache::SyncRun(WriteString(pair.client.get(), "GET /healthz HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")));
    pair.client->ShutdownWrite();

    // One read can see no more than the request line, so the headers must be picked
    // up by a later one -- or left behind, which is the bug.
    ShortReadSocket shortReads { *pair.server, std::string_view { "GET /healthz HTTP/1.1\r\n" }.size() };

    FastCache::AtomicMetricsSink metrics;
    using namespace std::chrono_literals;
    auto provider = [] {
        return FastCache::MetricsSnapshot { .storage = std::nullopt,
                                            .host = std::nullopt,
                                            .uptime = FastCache::Uptime { 7s } };
    };
    FastCache::SyncRun(FastCache::ServeAdminHttp(&shortReads, &metrics, provider));

    auto const response = FastCache::SyncRun(ReadAvailable(pair.client.get()));
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));

    // Nothing of the request is left: the server read it to the end before answering.
    CHECK(FastCache::SyncRun(ReadAvailable(pair.server.get())).empty());
}

namespace
{

/// Drive one request against a caller-supplied route list.
///
/// Separate from `Exchange` rather than a defaulted parameter on it, so every
/// assertion that a built-in route is unchanged keeps calling the two-argument
/// form and cannot accidentally start depending on a contributed row.
/// @param request The raw request bytes.
/// @param routes What the caller registered.
/// @return Everything the server wrote back.
std::string ExchangeWithRoutes(std::string_view request, std::vector<FastCache::AdminRoute> const& routes)
{
    FastCache::AtomicMetricsSink metrics;
    auto pair = FastCache::InMemorySocketPair::Create();
    REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), request)));
    pair.client->ShutdownWrite();
    using namespace std::chrono_literals;
    auto provider = [] {
        return FastCache::MetricsSnapshot { .storage = std::nullopt,
                                            .host = std::nullopt,
                                            .uptime = FastCache::Uptime { 1s } };
    };
    FastCache::SyncRun(FastCache::ServeAdminHttp(pair.server.get(), &metrics, provider, routes));
    pair.server->Close();
    return FastCache::SyncRun(ReadAvailable(pair.client.get()));
}

} // namespace

TEST_CASE("AdminHttp: a contributed route answers on its own path", "[admin][http][routes]")
{
    std::vector<FastCache::AdminRoute> const routes {
        { .path = "/fleet",
          .handler = [](FastCache::AdminRequest const&) {
              return FastCache::AdminResponse { .status = "200 OK", .contentType = "text/html", .body = "<h1>fleet</h1>" };
          } },
    };

    auto const response = ExchangeWithRoutes("GET /fleet HTTP/1.1\r\nHost: x\r\n\r\n", routes);
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(response.contains("Content-Type: text/html\r\n"));
    CHECK(response.contains("<h1>fleet</h1>"));
}

TEST_CASE("AdminHttp: the built-in routes still answer alongside a contributed one", "[admin][http][routes]")
{
    // The regression that matters most here: `/metrics` and `/healthz` are what
    // every existing scraper and probe is pointed at, and a route table that
    // shadowed or reordered them would be found in production rather than here.
    std::vector<FastCache::AdminRoute> const routes {
        { .path = "/fleet",
          .handler = [](FastCache::AdminRequest const&) { return FastCache::AdminResponse {}; } },
    };

    CHECK(ExchangeWithRoutes("GET /healthz HTTP/1.1\r\n\r\n", routes).starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(ExchangeWithRoutes("GET /metrics HTTP/1.1\r\n\r\n", routes).contains("text/plain; version=0.0.4"));
    CHECK(ExchangeWithRoutes("GET /nope HTTP/1.1\r\n\r\n", routes).starts_with("HTTP/1.1 404 Not Found\r\n"));
}

TEST_CASE("AdminHttp: a route may refuse, and its extra headers reach the client", "[admin][http][routes]")
{
    // A 401 that does not carry `WWW-Authenticate` is one a browser cannot prompt
    // for, which turns "you need a credential" into "this page is broken".
    std::vector<FastCache::AdminRoute> const routes {
        { .path = "/fleet", .handler = [](FastCache::AdminRequest const&) {
             return FastCache::AdminResponse { .status = "401 Unauthorized",
                                               .contentType = "text/plain",
                                               .body = "credential required\n",
                                               .extraHeaders = { R"(WWW-Authenticate: Basic realm="fleet")" } };
         } },
    };

    auto const response = ExchangeWithRoutes("GET /fleet HTTP/1.1\r\n\r\n", routes);
    CHECK(response.starts_with("HTTP/1.1 401 Unauthorized\r\n"));
    CHECK(response.contains(R"(WWW-Authenticate: Basic realm="fleet")"));
    CHECK(response.contains("credential required\n"));
}

TEST_CASE("AdminHttp: a handler is told the Authorization header, whatever its case", "[admin][http][routes]")
{
    // Field names are case-insensitive and real clients disagree about the
    // spelling. A byte comparison would make the credential work for curl and
    // not for a proxy, which gets diagnosed as "the token is wrong".
    auto const echo = [](FastCache::AdminRequest const& request) {
        return FastCache::AdminResponse { .status = "200 OK",
                                          .contentType = "text/plain",
                                          .body = std::string { request.authorization } };
    };
    std::vector<FastCache::AdminRoute> const routes { { .path = "/echo", .handler = echo } };

    CHECK(ExchangeWithRoutes("GET /echo HTTP/1.1\r\nAuthorization: Bearer s3cret\r\n\r\n", routes)
              .contains("Bearer s3cret"));
    CHECK(ExchangeWithRoutes("GET /echo HTTP/1.1\r\nauthorization:   Basic QUJD\r\n\r\n", routes).contains("Basic QUJD"));

    // No header at all is an empty view, not a missing one: a handler that reads
    // it before checking would otherwise be reading uninitialised storage.
    auto const none = ExchangeWithRoutes("GET /echo HTTP/1.1\r\nHost: x\r\n\r\n", routes);
    CHECK(none.contains("Content-Length: 0\r\n"));
}

TEST_CASE("AdminHttp: a query string reaches the handler and does not defeat routing", "[admin][http][routes]")
{
    auto const echo = [](FastCache::AdminRequest const& request) {
        return FastCache::AdminResponse { .status = "200 OK",
                                          .contentType = "text/plain",
                                          .body = std::string { request.query } };
    };
    std::vector<FastCache::AdminRoute> const routes { { .path = "/echo", .handler = echo } };

    CHECK(ExchangeWithRoutes("GET /echo?tier=disk HTTP/1.1\r\n\r\n", routes).contains("tier=disk"));
}
