// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/InMemoryTransport.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

#include <tests/SocketDecorator.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
///
/// Local because the CONDITION is this file's. The forwarding is not, and lives on
/// `SocketDecorator` -- including the four defaulted `ISocket` virtuals this used
/// to answer from the base rather than from the socket it decorates.
class ShortReadSocket final: public FastCache::Testing::SocketDecorator
{
  public:
    ShortReadSocket(FastCache::ISocket& inner, std::size_t limit) noexcept:
        SocketDecorator { inner },
        _limit { limit }
    {
    }

    /// Hand up no more than `limit` bytes, whatever the caller asked for.
    /// @param buffer Where to put them.
    /// @return What the decorated socket delivered into the narrowed span.
    [[nodiscard]] FastCache::IoAwaitable Read(std::span<std::byte> buffer) override
    {
        return SocketDecorator::Read(buffer.subspan(0, std::min(buffer.size(), _limit)));
    }

  private:
    std::size_t _limit;
};

/// Serve one request over a fresh pair, and return what reached the client.
///
/// `Exchange` above always writes a request first, which is precisely the shape
/// that cannot reach #824: every fixture in this file sent immediately, and that is
/// why a surface answering `400` to a silent peer went unnoticed for as long as it
/// did. This one allows an EMPTY request and returns whatever the server wrote --
/// **including nothing**, which is the assertion the silent outcomes need.
/// @param request What the client sends before the server is driven; may be empty.
/// @return Every byte the server wrote, which is empty when it wrote none.
std::string ExchangeMaybeSilent(std::string_view request)
{
    FastCache::AtomicMetricsSink metrics;
    auto pair = FastCache::InMemorySocketPair::Create();
    if (!request.empty())
        REQUIRE(FastCache::SyncRun(WriteString(pair.client.get(), request)));
    pair.client->ShutdownWrite();
    using namespace std::chrono_literals;
    auto provider = [] {
        return FastCache::MetricsSnapshot { .storage = {}, .host = std::nullopt, .uptime = FastCache::Uptime { 7s } };
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

TEST_CASE("AdminHttp: non-GET method returns 405 naming the methods it does serve", "[metrics][http]")
{
    FastCache::AtomicMetricsSink metrics;
    auto const response = Exchange("POST /metrics HTTP/1.1\r\n\r\n", metrics, {});
    REQUIRE(response.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    // RFC 9110 §15.5.6 makes this a MUST, and it is what separates "wrong verb"
    // from "wrong path" for whatever is probing this port.
    CHECK(response.contains("Allow: GET\r\n"));
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
          .handler =
              [](FastCache::AdminRequest const&) {
                  return FastCache::AdminResponse { .status = "200 OK",
                                                    .contentType = "text/html",
                                                    .body = "<h1>fleet</h1>" };
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
        { .path = "/fleet", .handler = [](FastCache::AdminRequest const&) { return FastCache::AdminResponse {}; } },
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
        { .path = "/fleet",
          .handler =
              [](FastCache::AdminRequest const&) {
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
                                          .body = std::string { request.Header(FastCache::AdminHeader::Authorization) } };
    };
    std::vector<FastCache::AdminRoute> const routes { { .path = "/echo", .handler = echo } };

    CHECK(
        ExchangeWithRoutes("GET /echo HTTP/1.1\r\nAuthorization: Bearer s3cret\r\n\r\n", routes).contains("Bearer s3cret"));
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

TEST_CASE("AdminHttp: every readable header arrives, in either order", "[admin][http][routes]")
{
    // The regression this exists for: the walk used to stop at the first header it
    // recognised. With one readable header that is correct; with two it means
    // whichever the client happened to send first is the only one that arrives, so
    // a conditional GET would silently stop revalidating whenever a browser put
    // `Authorization` above `If-None-Match`.
    auto const echo = [](FastCache::AdminRequest const& request) {
        return FastCache::AdminResponse { .status = "200 OK",
                                          .contentType = "text/plain",
                                          .body = std::format("[{}][{}]",
                                                              request.Header(FastCache::AdminHeader::Authorization),
                                                              request.Header(FastCache::AdminHeader::IfNoneMatch)) };
    };
    std::vector<FastCache::AdminRoute> const routes { { .path = "/echo", .handler = echo } };

    CHECK(ExchangeWithRoutes("GET /echo HTTP/1.1\r\nAuthorization: Bearer t\r\nIf-None-Match: \"abc\"\r\n\r\n", routes)
              .contains("[Bearer t][\"abc\"]"));
    CHECK(ExchangeWithRoutes("GET /echo HTTP/1.1\r\nif-none-match: \"abc\"\r\nauthorization: Bearer t\r\n\r\n", routes)
              .contains("[Bearer t][\"abc\"]"));
}

TEST_CASE("AdminHttp: a 304 carries its validators and no content at all", "[admin][http][routes]")
{
    std::vector<FastCache::AdminRoute> const routes {
        { .path = "/chart.svg",
          .handler =
              [](FastCache::AdminRequest const& request) {
                  if (request.Header(FastCache::AdminHeader::IfNoneMatch) == "\"g-7\"")
                      return FastCache::AdminResponse { .status = "304 Not Modified",
                                                        .extraHeaders = { "ETag: \"g-7\"", "Cache-Control: max-age=42" } };
                  return FastCache::AdminResponse { .status = "200 OK",
                                                    .contentType = "image/svg+xml",
                                                    .body = "<svg/>",
                                                    .extraHeaders = { "ETag: \"g-7\"" } };
              } },
    };

    auto const fresh = ExchangeWithRoutes("GET /chart.svg HTTP/1.1\r\n\r\n", routes);
    CHECK(fresh.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(fresh.contains("Content-Type: image/svg+xml\r\n"));
    CHECK(fresh.ends_with("<svg/>"));

    auto const cached = ExchangeWithRoutes("GET /chart.svg HTTP/1.1\r\nIf-None-Match: \"g-7\"\r\n\r\n", routes);
    CHECK(cached.starts_with("HTTP/1.1 304 Not Modified\r\n"));
    // RFC 9110 SS15.4.5 forbids content on a 304, and SS8.6 forbids a Content-Length
    // that does not match what a 200 would have sent. A client that read one and
    // then found the connection closed would report a truncated response.
    CHECK_FALSE(cached.contains("Content-Length:"));
    CHECK_FALSE(cached.contains("Content-Type:"));
    // The validators are the whole point of the response: without them the client
    // has nothing to revalidate against next time.
    CHECK(cached.contains("ETag: \"g-7\"\r\n"));
    CHECK(cached.contains("Cache-Control: max-age=42\r\n"));
    CHECK(cached.ends_with("\r\n\r\n"));
}

TEST_CASE("AdminHttp: a prefix route answers its tail and never shadows an exact one", "[admin][http][routes]")
{
    auto const named = [](std::string_view what) {
        return [what](FastCache::AdminRequest const& request) {
            return FastCache::AdminResponse { .status = "200 OK",
                                              .contentType = "text/plain",
                                              .body = std::format("{}:{}", what, request.path) };
        };
    };
    // The prefix route is registered *first*, which is what makes this a test of the
    // table rather than of the caller's vector order.
    std::vector<FastCache::AdminRoute> const routes {
        { .path = "/fleet/", .handler = named("prefix"), .match = FastCache::AdminRouteMatch::Prefix },
        { .path = "/fleet", .handler = named("exact") },
    };

    CHECK(ExchangeWithRoutes("GET /fleet HTTP/1.1\r\n\r\n", routes).contains("exact:/fleet"));
    CHECK(ExchangeWithRoutes("GET /fleet/chart/refusals.svg HTTP/1.1\r\n\r\n", routes)
              .contains("prefix:/fleet/chart/refusals.svg"));
    // A prefix route is not a catch-all: a path that does not begin with it still
    // reaches the 404, so a typo is refused rather than quietly served.
    CHECK(ExchangeWithRoutes("GET /fleetx HTTP/1.1\r\n\r\n", routes).starts_with("HTTP/1.1 404 Not Found\r\n"));
}


// ---------------------------------------------------------------------------
// #824: four outcomes behind one `bool ok`, all of them answered `400`.
//
// The pair that matters is the first two against the third. A test that only
// asserted "an idle connection is not answered 400" would pass just as well on a
// server that had stopped answering 400 to *anything*, which would delete the one
// case where 400 is the right answer. Silence and the refusal are asserted against
// each other, or one passing test covers both and proves neither.
// ---------------------------------------------------------------------------

TEST_CASE("AdminHttp: a peer that sent nothing before its deadline is closed, not refused",
          "[metrics][http][admin][idle]")
{
    // The reported bug, at the seam: Chrome opens a preconnect socket, sends
    // nothing, and the request deadline expires. Nothing was asked, so nothing is
    // owed -- and the 400 this used to answer is what reached a real browser as a
    // broken dashboard.
    auto const code = GENERATE(FastCache::NetErrorCode::WouldBlock, FastCache::NetErrorCode::Timeout);
    INFO("read failed with NetErrorCode " << static_cast<unsigned>(code));

    auto pair = FastCache::InMemorySocketPair::Create();
    // Reads fail; writes still go through. So an empty response is the server
    // declining to answer, never the server failing to.
    FastCache::Testing::FailingReadSocket stalled { *pair.server, code };

    FastCache::AtomicMetricsSink metrics;
    using namespace std::chrono_literals;
    auto provider = [] {
        return FastCache::MetricsSnapshot { .storage = {}, .host = std::nullopt, .uptime = FastCache::Uptime { 7s } };
    };
    FastCache::SyncRun(FastCache::ServeAdminHttp(&stalled, &metrics, provider));
    pair.server->Close();

    REQUIRE(FastCache::SyncRun(ReadAvailable(pair.client.get())).empty());
}

TEST_CASE("AdminHttp: a peer that closed without sending is closed, not refused", "[metrics][http][admin][idle]")
{
    // The other silence: an abandoned preconnect, a TCP liveness probe, a port
    // scan. `ExchangeMaybeSilent` half-closes with nothing written, so the first read
    // is EOF rather than an error -- the state `FailingReadSocket` cannot
    // produce and the one the case above must not be mistaken for.
    REQUIRE(ExchangeMaybeSilent({}).empty());
}

TEST_CASE("AdminHttp: a malformed request is still refused 400", "[metrics][http][admin][idle]")
{
    // The control, and the half that was never covered at all: before #824 this
    // file had sixteen cases and not one of them asserted a 400, so the outcome
    // that legitimately deserves one was as untested as the three that did not.
    auto const response = ExchangeMaybeSilent("nonsense\r\n\r\n");
    REQUIRE(response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
}

TEST_CASE("AdminHttp: a head that overruns the byte cap is refused 431, not served truncated",
          "[metrics][http][admin][idle]")
{
    // Not a missing refusal but a wrong answer: the read loop stopped at the cap
    // and the *prefix* was then parsed and routed, so every field past it vanished
    // and a browser whose `Authorization` fell beyond the cap was told `401`.
    //
    // The padding sits between the request line and the credential, so the request
    // line -- which is at the front and always survives truncation -- still parses.
    // That is what makes this case fail under the old code by being *served* rather
    // than by being unreadable.
    std::string request = "GET /healthz HTTP/1.1\r\nHost: x\r\n";
    request += "X-Pad: " + std::string(9000, 'p') + "\r\n";
    request += "\r\n";
    auto const response = ExchangeMaybeSilent(request);
    INFO("response was: " << response.substr(0, 64));
    REQUIRE(response.starts_with("HTTP/1.1 431 Request Header Fields Too Large\r\n"));
}

TEST_CASE("AdminHttp: a large but complete head under the cap still routes", "[metrics][http][admin][idle]")
{
    // The control for the case above. Without it, "refuse an oversize head" and
    // "refuse a large head" are the same passing test, and the cap could drift down
    // to nothing with every assertion still green.
    std::string request = "GET /healthz HTTP/1.1\r\nHost: x\r\n";
    request += "X-Pad: " + std::string(4000, 'p') + "\r\n";
    request += "\r\n";
    REQUIRE(ExchangeMaybeSilent(request).starts_with("HTTP/1.1 200 OK\r\n"));
}

TEST_CASE("A tolerated admin bind failure is reported at Warn and says the daemon carries on",
          "[server][admin][bind-verdict]")
{
    // #603. `fastcached` serves the cache on without an admin endpoint -- that is a
    // policy, not an accident -- and it reported the condition at `Error`, which is
    // what a site pages on. Two costs pointing opposite ways: a page for a working
    // daemon, and an operator trained to ignore the level.
    //
    // The message is asserted alongside the level because half of the acceptance is
    // that a reader should not have to INFER whether the process survived.
    auto const report = FastCache::DescribeToleratedAdminBindFailure("0.0.0.0", 9259, "address already in use");

    CHECK(report.level == FastCache::LogLevel::Warn);
    CHECK(report.level != FastCache::LogLevel::Error);
    CHECK(report.message.contains("0.0.0.0:9259"));
    CHECK(report.message.contains("address already in use"));
    CHECK(report.message.contains("continuing without"));
    // Naming the surfaces, because "the metrics endpoint" is not what an operator
    // has a probe pointed at -- `/healthz` is, and it goes away too.
    CHECK(report.message.contains("/metrics"));
    CHECK(report.message.contains("/healthz"));
}

TEST_CASE("The tolerated admin verdict is not the verdict for a bind that stops the process",
          "[server][admin][bind-verdict]")
{
    // The half #603 explicitly asks for: whatever asserts the severity has to
    // DISTINGUISH the tolerated site from the fatal ones, or somebody "fixes" all
    // four to one level and the information is gone.
    //
    // The fatal side is asserted where it lives, against the real listener paths, by
    // `ReactorServerLoop_test`'s "reports a bind it cannot make at Error and refuses
    // to start" -- those return EXIT_FAILURE, so `Error` is correct there and this
    // function must never be reached for them. What is left to pin here is that the
    // two verdicts do not collapse into one level.
    auto const tolerated =
        FastCache::DescribeToleratedAdminBindFailure("192.0.2.1", 9259, "cannot assign requested address");

    CHECK(tolerated.level == FastCache::LogLevel::Warn);
    // A tolerated failure must not describe itself as the process stopping. Refusal
    // vocabulary here would read as fatal whatever the level said, which is the same
    // defect one layer up.
    CHECK_FALSE(tolerated.message.contains("refusing to start"));
    CHECK_FALSE(tolerated.message.contains("exiting"));
}
