// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/TlsWrap.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace FastCache
{

namespace
{

    /// Hard cap on the request head we will buffer before giving up. The admin
    /// surface only needs the request line and one header, so this is generous; it
    /// exists to stop a slow or hostile client from growing the buffer without bound.
    constexpr std::size_t MaxRequestBytes = 8192;

    Task<bool> WriteAll(ISocket* socket, std::string_view data)
    {
        if (data.empty())
            co_return true;
        auto const result = co_await socket->Write(AsBytes(data));
        co_return result.has_value() && *result == data.size();
    }

    /// Parsed HTTP request head. `ok` is false when no complete line arrived
    /// within the byte cap or the line was malformed.
    ///
    /// Holds the head it parsed, because `AdminRequest` is all `string_view`: the
    /// views point into `head`, so the two travel together or neither is valid.
    struct RequestHead
    {
        std::string head;
        std::string method;
        std::string target;
        std::string authorization;
        bool ok { false };
    };

    /// Case-insensitive comparison, for a header name.
    ///
    /// HTTP field names are case-insensitive and real clients disagree about the
    /// spelling -- curl sends `Authorization`, some proxies lower-case everything.
    /// A byte comparison here would make the credential work for some callers and
    /// not others, which is the shape of bug that gets diagnosed as "the token is
    /// wrong".
    /// @param a One name.
    /// @param b The other.
    /// @return Whether they name the same header.
    [[nodiscard]] bool EqualsIgnoringCase(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(a, b, [](char x, char y) noexcept {
            auto const lower = [](char c) noexcept {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            };
            return lower(x) == lower(y);
        });
    }

    /// Trim leading and trailing spaces and tabs.
    /// @param text The field value.
    /// @return The value without its optional surrounding whitespace.
    [[nodiscard]] std::string_view TrimFieldValue(std::string_view text) noexcept
    {
        constexpr std::string_view Blank = " \t";
        auto const first = text.find_first_not_of(Blank);
        if (first == std::string_view::npos)
            return {};
        return text.substr(first, text.find_last_not_of(Blank) - first + 1);
    }

    /// Read the whole request head — up to the blank line that ends the headers,
    /// or the byte cap — then split its first line into method and target and pick
    /// out the one header this surface reads.
    ///
    /// The headers were already **consumed** before any of them was used, and that
    /// is the point: this used to stop at the first CRLF, and a request split across
    /// TCP segments then left `Host:` and `Connection: close` unread in the receive
    /// queue. Closing a socket with unread received data makes the kernel send **RST
    /// instead of FIN**, so the client sees `Connection reset by peer` rather than a
    /// clean end of response — intermittently, depending only on how the request
    /// happened to be segmented.
    ///
    /// That is not a fixture problem. Every consumer of this endpoint is a monitoring
    /// probe, and an endpoint whose job is to report that a worker is healthy must not
    /// be the thing that looks unhealthy. Consuming the request before answering it is
    /// also simply what a correct HTTP server does.
    ///
    /// A client that never sends the blank line is bounded by the accepted socket's
    /// receive timeout, which is what a request timeout is for. A head that exceeds
    /// the cap is answered and then closed with whatever is left unread — that peer
    /// is malformed or hostile, and a reset is a fine thing for it to see.
    Task<RequestHead> ReadRequestHead(ISocket* socket)
    {
        std::string buffer;
        bool sawHeadEnd = false;
        while (!sawHeadEnd && buffer.size() < MaxRequestBytes)
        {
            std::array<std::byte, 1024> chunk {};
            auto const result = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
            if (!result.has_value() || *result == 0)
                break;
            // Rescan from three bytes before the freshly-appended region: the
            // terminator is four bytes and may straddle a chunk boundary in any of
            // three ways. That keeps the total scan linear rather than re-reading
            // the whole buffer each iteration; append in one call, not byte-by-byte.
            auto const scanFrom = buffer.size() < 3 ? 0 : buffer.size() - 3;
            buffer.append(reinterpret_cast<char const*>(chunk.data()), *result);
            sawHeadEnd = buffer.find("\r\n\r\n", scanFrom) != std::string::npos;
        }

        auto const eol = buffer.find("\r\n");
        if (eol == std::string::npos)
            co_return RequestHead {};
        std::string_view const line { buffer.data(), eol };

        auto const sp1 = line.find(' ');
        if (sp1 == std::string_view::npos)
            co_return RequestHead {};
        auto const rest = line.substr(sp1 + 1);
        auto const sp2 = rest.find(' ');
        auto const target = sp2 == std::string_view::npos ? rest : rest.substr(0, sp2);

        // Walk the remaining field lines for the one header this surface reads.
        // Stopping at the blank line rather than scanning the whole buffer keeps a
        // body -- which no admin route has, but which a hostile client may send --
        // from being mistaken for a header.
        std::string authorization;
        constexpr std::string_view AuthorizationField = "authorization";
        for (auto cursor = eol + 2; cursor < buffer.size();)
        {
            auto const lineEnd = buffer.find("\r\n", cursor);
            if (lineEnd == std::string::npos || lineEnd == cursor)
                break;
            std::string_view const field { buffer.data() + cursor, lineEnd - cursor };
            if (auto const colon = field.find(':'); colon != std::string_view::npos
                && EqualsIgnoringCase(field.substr(0, colon), AuthorizationField))
            {
                authorization = std::string { TrimFieldValue(field.substr(colon + 1)) };
                break;
            }
            cursor = lineEnd + 2;
        }

        co_return RequestHead { .head = std::move(buffer),
                                .method = std::string { line.substr(0, sp1) },
                                .target = std::string { target },
                                .authorization = std::move(authorization),
                                .ok = true };
    }

    Task<bool> WriteResponse(ISocket* socket, AdminResponse const& response)
    {
        auto head = std::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n",
                                response.status,
                                response.contentType,
                                response.body.size());
        for (auto const& extra: response.extraHeaders)
            head += std::format("{}\r\n", extra);
        head += "\r\n";
        if (!co_await WriteAll(socket, head))
            co_return false;
        co_return co_await WriteAll(socket, response.body);
    }

    /// The refusals this server answers without consulting a route.
    ///
    /// A table so that the three share one spelling of "status, type, body" with
    /// every route's own answer, rather than three `WriteResponse` calls whose
    /// argument order is checked by nothing.
    [[nodiscard]] AdminResponse PlainRefusal(std::string_view status, std::string_view body)
    {
        return AdminResponse { .status = status, .contentType = "text/plain", .body = std::string { body } };
    }

} // namespace

Task<void> ServeAdminHttp(ISocket* socket,
                          IMetricsSink const* metrics,
                          AdminHttpServer::SnapshotProvider snapshotProvider,
                          std::span<AdminRoute const> routes)
{
    // TLS terminates here rather than in the accept loop, so a handshake failure
    // costs the same detached task a slow request does and never the loop. A
    // plaintext socket's implementation is a no-op, so this is unconditional.
    if (!co_await socket->HandshakeIfNeeded())
        co_return;

    auto const request = co_await ReadRequestHead(socket);
    if (!request.ok)
    {
        (void) co_await WriteResponse(socket, PlainRefusal("400 Bad Request", "bad request\n"));
        co_return;
    }
    if (request.method != "GET")
    {
        (void) co_await WriteResponse(socket, PlainRefusal("405 Method Not Allowed", "method not allowed\n"));
        co_return;
    }

    // Strip any query string so `/metrics?foo=bar` still routes.
    std::string_view const target { request.target };
    auto const questionMark = target.find('?');
    auto const path = target.substr(0, questionMark);
    auto const query = questionMark == std::string_view::npos ? std::string_view {} : target.substr(questionMark + 1);

    if (path == "/metrics")
    {
        auto const body = RenderPrometheus(*metrics, snapshotProvider());
        (void) co_await WriteResponse(
            socket,
            AdminResponse { .status = "200 OK", .contentType = "text/plain; version=0.0.4", .body = body });
        co_return;
    }
    if (path == "/healthz")
    {
        (void) co_await WriteResponse(socket, PlainRefusal("200 OK", "OK\n"));
        co_return;
    }

    AdminRequest const routeRequest { .path = path, .query = query, .authorization = request.authorization };
    for (auto const& route: routes)
    {
        if (route.path != path || !route.handler)
            continue;
        (void) co_await WriteResponse(socket, route.handler(routeRequest));
        co_return;
    }

    (void) co_await WriteResponse(socket, PlainRefusal("404 Not Found", "not found\n"));
    co_return;
}

AdminHttpServer::AdminHttpServer(IListener& listener,
                                 IMetricsSink const& metrics,
                                 SnapshotProvider snapshotProvider,
                                 ILogger& logger,
                                 std::vector<AdminRoute> routes,
                                 TlsContext* tls) noexcept:
    _listener { listener },
    _metrics { metrics },
    _snapshotProvider { std::move(snapshotProvider) },
    _logger { logger },
    _routes { std::move(routes) },
    _tls { tls }
{
}

/// Serve one accepted admin connection as a detached coroutine. Owns the
/// socket (so the connection outlives the accept loop iteration) and
/// decrements the in-flight counter on exit. Free function so the accept
/// loop can spawn it without capturing this — `inFlight` is passed by
/// pointer with a lifetime that exceeds every spawned task (the
/// AdminHttpServer's destructor blocks elsewhere).
static DetachedTask ServeAdminConnection(std::unique_ptr<ISocket> socket,
                                         IMetricsSink const* metrics,
                                         AdminHttpServer::SnapshotProvider snapshotProvider,
                                         std::span<AdminRoute const> routes,
                                         std::atomic<std::size_t>* inFlight)
{
    co_await ServeAdminHttp(socket.get(), metrics, std::move(snapshotProvider), routes);
    socket->Close();
    inFlight->fetch_sub(1, std::memory_order_acq_rel);
}

Task<void> AdminHttpServer::Run()
{
    while (!_shuttingDown.load(std::memory_order_acquire))
    {
        auto accepted = co_await _listener.Accept();
        if (!accepted.has_value())
        {
            // A poll-timeout on the listening socket is how we wake to observe
            // Shutdown() on POSIX (where Close() does not unblock a parked
            // accept()); it is not a real failure, so loop and re-check the flag.
            auto const code = accepted.error().code;
            if (code == NetErrorCode::WouldBlock || code == NetErrorCode::Timeout)
                continue;
            _logger.Logf(LogLevel::Debug, "admin: accept loop ended ({})", accepted.error().ToString());
            co_return;
        }
        // Spawn the handler as a detached coroutine so a slow or hostile
        // client cannot tie up the accept loop and DoS /metrics and /healthz
        // for everyone else. The in-flight cap bounds peak memory: an
        // attacker that opens MaxConcurrentRequests slow sockets gets a
        // 503-and-close on the next accept rather than queueing in the
        // kernel until the scraper times out.
        auto const before = _inFlight.fetch_add(1, std::memory_order_acq_rel);
        if (before >= MaxConcurrentRequests)
        {
            _inFlight.fetch_sub(1, std::memory_order_acq_rel);
            // Written through the one response writer rather than as a literal
            // whose Content-Length was maintained by hand: the hand-counted one
            // said 32 for a 32-byte body and would have gone on saying 32 for the
            // first person who reworded it, which a client reads as a truncated
            // response or a hung connection rather than as a typo.
            //
            // Plaintext, and deliberately: this refusal happens before the
            // connection has a handler, so nothing has driven a TLS handshake and
            // a TLS client would not read it either way. It costs the attacker a
            // socket and tells an honest client something.
            (void) co_await WriteResponse((*accepted).get(),
                                          PlainRefusal("503 Service Unavailable", "admin: too many concurrent reqs\n"));
            (*accepted)->Close();
            continue;
        }
        ServeAdminConnection(WrapTls(std::move(*accepted), _tls), &_metrics, _snapshotProvider, _routes, &_inFlight);
    }
    co_return;
}

void AdminHttpServer::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
    // Detached request coroutines may still be in flight. They borrow the
    // metrics sink, snapshot provider and routes held on this object, so we must
    // wait for them to drain before letting Shutdown return — otherwise an admin
    // handler that suspends on a slow write could touch freed members after
    // the server object is destroyed. Bounded by the per-request socket I/O
    // timeout, plus a generous spin-cap so a stuck handler cannot block
    // forever.
    using namespace std::chrono_literals;
    constexpr auto MaxDrainTime = 5s;
    auto const start = std::chrono::steady_clock::now();
    while (_inFlight.load(std::memory_order_acquire) > 0 && std::chrono::steady_clock::now() - start < MaxDrainTime)
        std::this_thread::sleep_for(10ms);
}

} // namespace FastCache
