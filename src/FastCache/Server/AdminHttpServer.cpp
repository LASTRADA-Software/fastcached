// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

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
    /// surface only needs the request line, so this is generous; it exists to
    /// stop a slow or hostile client from growing the buffer without bound.
    constexpr std::size_t MaxRequestBytes = 8192;

    Task<bool> WriteAll(ISocket* socket, std::string_view data)
    {
        if (data.empty())
            co_return true;
        auto const result = co_await socket->Write(AsBytes(data));
        co_return result.has_value() && *result == data.size();
    }

    /// Parsed HTTP request line. `ok` is false when no complete line arrived
    /// within the byte cap or the line was malformed.
    struct RequestLine
    {
        std::string method;
        std::string target;
        bool ok { false };
    };

    /// Read bytes until the first CRLF (the end of the request line) or the
    /// byte cap, then split it into method and target. Headers and body are
    /// ignored — the admin routes are all parameterless GETs.
    Task<RequestLine> ReadRequestLine(ISocket* socket)
    {
        std::string buffer;
        bool sawCrlf = false;
        while (!sawCrlf && buffer.size() < MaxRequestBytes)
        {
            std::array<std::byte, 1024> chunk {};
            auto const result = co_await socket->Read(std::span<std::byte> { chunk.data(), chunk.size() });
            if (!result.has_value() || *result == 0)
                break;
            // Only rescan from one byte before the freshly-appended region (a CR
            // may have ended the previous chunk and the LF start this one), so the
            // total scan stays linear rather than re-reading the whole buffer each
            // iteration; append in one call rather than byte-by-byte.
            auto const scanFrom = buffer.empty() ? 0 : buffer.size() - 1;
            buffer.append(reinterpret_cast<char const*>(chunk.data()), *result);
            sawCrlf = buffer.find("\r\n", scanFrom) != std::string::npos;
        }

        auto const eol = buffer.find("\r\n");
        if (eol == std::string::npos)
            co_return RequestLine {};
        std::string_view const line { buffer.data(), eol };

        auto const sp1 = line.find(' ');
        if (sp1 == std::string_view::npos)
            co_return RequestLine {};
        auto const rest = line.substr(sp1 + 1);
        auto const sp2 = rest.find(' ');
        auto const target = sp2 == std::string_view::npos ? rest : rest.substr(0, sp2);
        co_return RequestLine { .method = std::string { line.substr(0, sp1) },
                                .target = std::string { target },
                                .ok = true };
    }

    Task<bool> WriteResponse(ISocket* socket, std::string_view status, std::string_view contentType, std::string_view body)
    {
        auto const head = std::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                                      status,
                                      contentType,
                                      body.size());
        if (!co_await WriteAll(socket, head))
            co_return false;
        co_return co_await WriteAll(socket, body);
    }

} // namespace

Task<void> ServeAdminHttp(ISocket* socket, IMetricsSink const* metrics, AdminHttpServer::SnapshotProvider snapshotProvider)
{
    auto const request = co_await ReadRequestLine(socket);
    if (!request.ok)
    {
        (void) co_await WriteResponse(socket, "400 Bad Request", "text/plain", "bad request\n");
        co_return;
    }
    if (request.method != "GET")
    {
        (void) co_await WriteResponse(socket, "405 Method Not Allowed", "text/plain", "method not allowed\n");
        co_return;
    }

    // Strip any query string so `/metrics?foo=bar` still routes.
    std::string_view const target { request.target };
    auto const path = target.substr(0, target.find('?'));

    if (path == "/metrics")
    {
        auto const body = RenderPrometheus(*metrics, snapshotProvider());
        (void) co_await WriteResponse(socket, "200 OK", "text/plain; version=0.0.4", body);
    }
    else if (path == "/healthz")
    {
        (void) co_await WriteResponse(socket, "200 OK", "text/plain", "OK\n");
    }
    else
    {
        (void) co_await WriteResponse(socket, "404 Not Found", "text/plain", "not found\n");
    }
    co_return;
}

AdminHttpServer::AdminHttpServer(IListener& listener,
                                 IMetricsSink const& metrics,
                                 SnapshotProvider snapshotProvider,
                                 ILogger& logger) noexcept:
    _listener { listener },
    _metrics { metrics },
    _snapshotProvider { std::move(snapshotProvider) },
    _logger { logger }
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
                                         std::atomic<std::size_t>* inFlight)
{
    co_await ServeAdminHttp(socket.get(), metrics, std::move(snapshotProvider));
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
            (void) co_await WriteAll((*accepted).get(),
                                     "HTTP/1.1 503 Service Unavailable\r\n"
                                     "Content-Type: text/plain\r\n"
                                     "Content-Length: 32\r\n"
                                     "Connection: close\r\n\r\n"
                                     "admin: too many concurrent reqs\n");
            (*accepted)->Close();
            continue;
        }
        ServeAdminConnection(std::move(*accepted), &_metrics, _snapshotProvider, &_inFlight);
    }
    co_return;
}

void AdminHttpServer::Shutdown() noexcept
{
    _shuttingDown.store(true, std::memory_order_release);
    _listener.Close();
    // Detached request coroutines may still be in flight. They borrow the
    // metrics sink and snapshot provider held on this object, so we must wait
    // for them to drain before letting Shutdown return — otherwise an admin
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
