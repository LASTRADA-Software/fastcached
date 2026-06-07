// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <atomic>
#include <cstddef>
#include <functional>

namespace FastCache
{

/// Tiny read-only HTTP/1.1 admin surface, served on a dedicated port so it
/// never collides with the cache wire protocols (a leading `GET` would
/// otherwise be misrouted to the memcached text autodetector). Routes:
///   - `GET /metrics` — Prometheus text exposition of the counters + storage
///     snapshot (see RenderPrometheus).
///   - `GET /healthz` — `200 OK` liveness probe for containers / k8s.
///   - anything else — `404`; non-GET — `405`; malformed — `400`.
/// Each connection answers exactly one request, then closes (`Connection:
/// close`); the request head is bounded so a slow or oversized client cannot
/// tie the server up. Admission traffic is trivial, so connections are served
/// one at a time on the owning thread.
class AdminHttpServer
{
  public:
    /// Provider for a fresh metrics snapshot (storage stats + uptime), so
    /// `/metrics` reflects live state on each scrape rather than a stale copy.
    /// Computing uptime here keeps the server itself clock-agnostic.
    using SnapshotProvider = std::function<MetricsSnapshot()>;

    /// Construct over its collaborators; all must outlive the server.
    /// @param listener Bound listener for the admin port.
    /// @param metrics Connection-level counter sink to expose.
    /// @param snapshotProvider Returns a fresh storage snapshot + uptime per scrape.
    /// @param logger Shared logger.
    AdminHttpServer(IListener& listener,
                    IMetricsSink const& metrics,
                    SnapshotProvider snapshotProvider,
                    ILogger& logger) noexcept;

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
/// @return Task that resolves when the response has been written.
[[nodiscard]] Task<void> ServeAdminHttp(ISocket* socket,
                                        IMetricsSink const* metrics,
                                        AdminHttpServer::SnapshotProvider snapshotProvider);

} // namespace FastCache
