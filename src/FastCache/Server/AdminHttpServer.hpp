// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

class TlsContext;

/// One request header this server reads.
///
/// A closed set rather than a header map, deliberately: a map would invite routes
/// that branch on headers this server never bounds or normalises. A table is what
/// makes the set *extensible without being open* -- the second entry here arrived
/// as a row, and the parse loop that used to stop at the first match had to be
/// fixed once and now cannot regress per header.
enum class AdminHeader : std::uint8_t
{
    /// The credential every gated route checks.
    Authorization = 0,
    /// What a client already holds, for the conditional GETs the charts serve.
    IfNoneMatch,
    Last
};

/// What one readable header is called on the wire.
struct AdminHeaderRow
{
    AdminHeader header;    ///< The header this row describes.
    std::string_view name; ///< Its field name, lower-cased for the comparison.
};

/// Every header this server reads, in enumerator order.
inline constexpr EnumTable<AdminHeader, AdminHeaderRow> AdminHeaderTable {
    AdminHeaderRow { .header = AdminHeader::Authorization, .name = "authorization" },
    AdminHeaderRow { .header = AdminHeader::IfNoneMatch, .name = "if-none-match" },
};
static_assert(RowsInEnumeratorOrder(AdminHeaderTable, &AdminHeaderRow::header));

/// One admin request, as far as this server understands one.
struct AdminRequest
{
    /// Path with the query string stripped, so `/metrics?foo=1` routes as `/metrics`.
    std::string_view path;
    /// Whatever followed the `?`, empty when there was none.
    std::string_view query;
    /// One slot per `AdminHeaderTable` row; empty where the client sent none.
    EnumTable<AdminHeader, std::string_view> headers {};

    /// One header's value.
    /// @param which Which header.
    /// @return Its value, or empty when the client sent none.
    [[nodiscard]] std::string_view Header(AdminHeader which) const noexcept
    {
        return headers[static_cast<std::size_t>(which)];
    }
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
    ///
    /// A status that carries no content -- `304`, `204` -- suppresses `Content-Type`
    /// and `Content-Length` on its own, because whether a body is allowed is a
    /// property of the status rather than something each route must remember.
    std::string_view status { "200 OK" };
    /// What `body` is.
    std::string_view contentType { "text/plain" };
    /// The body, already rendered. Empty for a status that carries no content.
    std::string body {};
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

/// How a route's `path` is compared against the request's.
///
/// A column and not a special case: the charts are one resource per series, and a
/// route per series would put the series table's contents in two places. Dispatch
/// tries the kinds in enumerator order, so an exact route is never shadowed by a
/// prefix one however the caller happened to order its vector.
enum class AdminRouteMatch : std::uint8_t
{
    /// The whole path, and nothing else.
    Exact = 0,
    /// The path begins with `AdminRoute::path`; the handler reads the tail.
    Prefix,
    Last
};

/// One route: the path it answers, how that path is matched, and what answers it.
///
/// A table rather than an `if`/`else` ladder, for the reason every other table in
/// this codebase is one -- a route added by editing a chain of comparisons is a
/// route somebody adds to the chain and forgets to test, and the ladder this
/// replaced had already grown a third arm that only differed by a string.
struct AdminRoute
{
    /// The path, matched per `match`.
    std::string_view path;
    /// What renders it.
    AdminRouteHandler handler;
    /// Whole path or leading path. Exact unless a route says otherwise.
    AdminRouteMatch match { AdminRouteMatch::Exact };
};

/// One log record: the level an operator alerts on, and the sentence they read.
///
/// Returned as a pair rather than logged in place because the level is the thing
/// under test. A severity spelled at a call site inside `main.cpp` is reachable
/// from no test in this tree, which is how `fastcached` came to report a
/// deliberately-tolerated condition at `Error` for as long as it did.
struct AdminBindFailureReport
{
    LogLevel level;      ///< Severity that matches the verdict, not the syscall.
    std::string message; ///< What the operator reads, including what happens next.
};

/// Describe an admin endpoint (`/metrics`, `/healthz`) the DAEMON could not bind.
///
/// ## The severity follows the verdict, and the verdicts differ
///
/// `fastcached` **tolerates** this: the admin surface is a scrape and probe
/// convenience rather than the service, so the daemon serves the cache on without
/// one, permanently and by design. `fastcache-compile-node` answers the same
/// failure by refusing to start, and is right to -- an operator who asked a *worker*
/// for an endpoint is almost always wiring a probe to it, so a worker that started
/// without one looks healthy to the very thing that would have reported it was not.
///
/// The level is where that decision reaches an operator, and it was contradicting
/// it ([#603](https://github.com/LASTRADA-Software/fastcached/issues/603)). `Error`
/// is what a site pages on and what a log scraper counts, and it costs in both
/// directions at once: a site that alerts on `Error` gets paged for a working
/// daemon, and a site that has learned to ignore this particular `Error` has been
/// trained to ignore the level. A condition the process decided to continue past,
/// forever, on purpose, is a `Warn`.
///
/// **This is not the rule for every bind in the daemon.** `RunReactorServer`'s three
/// listener failures keep `Error`, because those paths return `EXIT_FAILURE`: the
/// process stops, so the severity and the verdict already agree. Skipped, absent and
/// failed are different states, and the log level is one of the few places an
/// operator can tell them apart.
///
/// The message says the daemon is continuing **without** the endpoint rather than
/// only that a bind failed, so a reader does not have to infer whether the process
/// survived.
/// @param address Address that was asked for.
/// @param port Port that was asked for.
/// @param error What the bind reported, or a stand-in when there is no listener.
/// @return The level and the sentence to log.
[[nodiscard]] AdminBindFailureReport DescribeToleratedAdminBindFailure(std::string_view address,
                                                                       std::uint16_t port,
                                                                       std::string_view error);

/// Tiny read-only HTTP/1.1 admin surface, served on a dedicated port so it
/// never collides with the cache wire protocols (a leading `GET` would
/// otherwise be misrouted to the memcached text autodetector). Built-in routes:
///   - `GET /metrics` — Prometheus text exposition of the counters + storage
///     snapshot (see RenderPrometheus).
///   - `GET /healthz` — `200 OK` liveness probe for containers / k8s.
///   - anything else — `404`; non-GET — `405`; a bad request line — `400`; a
///     request head over the byte cap — `431`.
/// A peer that sends NOTHING before the request deadline is closed without any
/// response at all: it asked nothing, so it is owed nothing, and answering it is
/// what made a browser's speculative preconnect render `400` (#824). See
/// `AdminHeadOutcome` in the implementation for the full set and its dispositions.
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
