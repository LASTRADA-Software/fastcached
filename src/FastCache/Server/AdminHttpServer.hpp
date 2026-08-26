// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

class TlsContext;

/// One admin request, as far as this server understands one.
///
/// Deliberately three fields rather than a header map. The admin surface answers
/// parameterless GETs and one authenticated route, so what a handler can ask about
/// a request is exactly what some handler already needs -- and a general header map
/// would invite routes that depend on headers this server does not bound.
struct AdminRequest
{
    /// Path with the query string stripped, so `/metrics?foo=1` routes as `/metrics`.
    std::string_view path;
    /// Whatever followed the `?`, empty when there was none.
    std::string_view query;
    /// The `Authorization` header's value, empty when the client sent none.
    ///
    /// The one header this server reads. It is here rather than in a map because a
    /// credential is the only thing a read-only surface can legitimately branch on,
    /// and naming it makes every route that ignores it visibly unauthenticated.
    std::string_view authorization;
};

/// What a route answered.
///
/// The status is part of the *response* rather than a column of the route table,
/// because one route legitimately answers several: `/fleet` is a page when this
/// node leads and a 401 when the caller brought no credential. A table column would
/// have to be a status per route, which is the one thing that is not fixed per route.
struct AdminResponse
{
    /// Status line without the version, e.g. `200 OK`.
    std::string_view status { "200 OK" };
    /// What `body` is.
    std::string_view contentType { "text/plain" };
    /// The body, already rendered.
    std::string body;
    /// Extra header lines, each without its trailing CRLF.
    ///
    /// Empty for almost every response. It exists because a 401 that does not carry
    /// `WWW-Authenticate` is one a browser cannot prompt for, which turns "you need a
    /// credential" into "this page is broken".
    std::vector<std::string> extraHeaders {};
};

/// Renders one route, or refuses it.
///
/// A `std::function` supplied by the caller rather than a member function, and that
/// is a layering decision rather than a style one: the fleet dashboard reads
/// `Distributed/` and `Cluster/`, and a handler declared here would drag both into
/// `Server/` for the benefit of one route. The server owns HTTP; what a route means
/// belongs to whoever registered it.
using AdminRouteHandler = std::function<AdminResponse(AdminRequest const&)>;

/// One route: the path it answers, and what answers it.
///
/// A table rather than an `if`/`else` ladder, for the reason every other table in
/// this codebase is one -- a route added by editing a chain of comparisons is a
/// route somebody adds to the chain and forgets to test, and the ladder this
/// replaced had already grown a third arm that only differed by a string.
struct AdminRoute
{
    /// Exact match against `AdminRequest::path`.
    std::string_view path;
    /// What renders it.
    AdminRouteHandler handler;
};

/// Tiny read-only HTTP/1.1 admin surface, served on a dedicated port so it
/// never collides with the cache wire protocols (a leading `GET` would
/// otherwise be misrouted to the memcached text autodetector). Built-in routes:
///   - `GET /metrics` — Prometheus text exposition of the counters + storage
///     snapshot (see RenderPrometheus).
///   - `GET /healthz` — `200 OK` liveness probe for containers / k8s.
///   - anything else — `404`; non-GET — `405`; malformed — `400`.
/// A caller may register further routes; see `AdminRoute`.
/// Each connection answers exactly one request, then closes (`Connection:
/// close`); the request head is bounded so a slow or oversized client cannot
/// tie the server up. Admission traffic is trivial, so connections are served
/// one at a time on the owning thread.
class AdminHttpServer
{
  public:
    /// How long an `accept()` may park before the loop looks at its stop flag.
    ///
    /// POSIX does not unblock a parked `accept()` when another thread closes the
    /// listening socket, so this poll is the only portable way `Shutdown()` is ever
    /// observed -- the mechanism whose *absence* this repository already records as
    /// a `systemctl stop` that hung until the supervisor escalated to SIGKILL.
    static constexpr auto AcceptPoll = std::chrono::milliseconds { 500 };

    /// How long one request read may take before the connection is dropped.
    ///
    /// The endpoint serves one connection at a time on its owning thread, so an
    /// idle client that never finishes its request head would otherwise wedge it
    /// for everybody (slowloris).
    static constexpr auto RequestTimeout = std::chrono::milliseconds { 2000 };

    /// Provider for a fresh metrics snapshot (storage stats + uptime), so
    /// `/metrics` reflects live state on each scrape rather than a stale copy.
    /// Computing uptime here keeps the server itself clock-agnostic.
    using SnapshotProvider = std::function<MetricsSnapshot()>;

    /// Construct over its collaborators; all must outlive the server.
    /// @param listener Bound listener for the admin port.
    /// @param metrics Connection-level counter sink to expose.
    /// @param snapshotProvider Returns a fresh storage snapshot + uptime per scrape.
    /// @param logger Shared logger.
    /// @param routes Routes beyond `/metrics` and `/healthz`; copied.
    /// @param tls Server TLS context, or nullptr to serve plaintext.
    AdminHttpServer(IListener& listener,
                    IMetricsSink const& metrics,
                    SnapshotProvider snapshotProvider,
                    ILogger& logger,
                    std::vector<AdminRoute> routes = {},
                    TlsContext* tls = nullptr) noexcept;

    /// Accept loop; returns when the listener is closed via Shutdown().
    /// @return Task that resolves when the accept loop exits.
    [[nodiscard]] Task<void> Run();

    /// Close the listener to unblock Run() and stop accepting.
    void Shutdown() noexcept;

    /// Maximum number of concurrently-in-flight admin requests. The accept
    /// loop spawns each request as a detached coroutine; once this cap is
    /// reached the loop replies 503 and closes the connection rather than
    /// queueing it. Default sized for "more than every typical scraper plus a
    /// handful of healthcheck probes" but small enough that a buggy or hostile
    /// client cannot exhaust memory.
    static constexpr std::size_t MaxConcurrentRequests = 32;

  private:
    IListener& _listener;
    IMetricsSink const& _metrics;
    SnapshotProvider _snapshotProvider;
    ILogger& _logger;
    std::vector<AdminRoute> _routes;
    /// Server TLS context, or null for plaintext. Not owned.
    TlsContext* _tls { nullptr };
    std::atomic<bool> _shuttingDown { false };
    /// Number of admin requests currently being served as detached tasks.
    /// Bumped in the accept loop, decremented at the end of each request.
    std::atomic<std::size_t> _inFlight { 0 };

  public:
    /// Test hook: live count of concurrently-in-flight admin requests.
    /// @return Detached requests currently being serviced.
    [[nodiscard]] std::size_t InFlight() const noexcept
    {
        return _inFlight.load(std::memory_order_acquire);
    }
};

/// Serve a single admin HTTP request on `socket`: read one request, route it,
/// write the response. Exposed separately from the accept loop so it can be
/// driven directly over an in-memory socket in tests.
/// Pointer/by-value parameters (not references) because this is a coroutine —
/// a reference parameter would dangle across a suspension point.
/// @param socket Connected client socket.
/// @param metrics Counter sink for `/metrics`.
/// @param snapshotProvider Storage-stats + uptime provider for `/metrics`.
/// @param routes Caller-registered routes, consulted before the 404.
/// @return Task that resolves when the response has been written.
[[nodiscard]] Task<void> ServeAdminHttp(ISocket* socket,
                                        IMetricsSink const* metrics,
                                        AdminHttpServer::SnapshotProvider snapshotProvider,
                                        std::span<AdminRoute const> routes = {});

} // namespace FastCache
