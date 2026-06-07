// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IAdmissionControl.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <atomic>
#include <exception>

namespace FastCache
{

/// Log the in-flight exception caught by a per-connection firewall, then
/// swallow it. MUST be called only from within a `catch` block — it rethrows
/// internally to classify the active exception. Centralizes the
/// drop-connection log policy shared by every per-connection DetachedTask
/// driver (the single- and multi-reactor accept loops), so the wording lives
/// in exactly one place.
/// @param logger Sink for the drop-connection diagnostic.
inline void LogConnectionFirewallException(ILogger& logger) noexcept
{
    try
    {
        throw;
    }
    catch (std::exception const& e)
    {
        logger.Logf(LogLevel::Error, "connection dropped on exception: {}", e.what());
    }
    catch (...)
    {
        logger.Logf(LogLevel::Error, "connection dropped on unknown exception");
    }
}

class TlsContext; // defined in Net/TlsContext.hpp; only used when TLS is built in

/// Top-level accept loop. Owns a listener, accepts connections, and spawns
/// a Connection coroutine per client. Tears down cleanly on Shutdown().
///
/// Optional collaborators (nullptr = disabled):
///   - IAdmissionControl: consulted before each accept; if AllowAccept()
///     returns false, the just-accepted socket is closed immediately and
///     IMetricsSink::ConnectionsAdmissionRejected is bumped.
///   - IMetricsSink: tracks ConnectionsTotal and rejection counts.
class Server
{
  public:
    /// Construct over the given collaborators; all references must outlive
    /// the server. Admission/metrics may be null.
    /// @param session Per-server session context (auth policy etc.) forwarded
    ///        to every connection. Copied by value; defaults to no auth.
    /// @param tls TLS context to wrap accepted sockets with, or nullptr for
    ///        plaintext. Only honoured in TLS-enabled builds.
    Server(IListener& listener,
           CacheEngine& engine,
           ILogger& logger,
           IAdmissionControl* admission = nullptr,
           IMetricsSink* metrics = nullptr,
           SessionContext session = {},
           TlsContext* tls = nullptr) noexcept;

    /// Run the accept loop until Shutdown() is called or the listener closes.
    /// @return Task that resolves when the accept loop exits.
    [[nodiscard]] Task<void> Run();

    /// Ask the accept loop to stop. The next failed Accept() (closed
    /// listener) breaks the loop; current connections continue under their
    /// own coroutines.
    void Shutdown() noexcept;

    /// @return Number of connections accepted since construction.
    [[nodiscard]] std::uint64_t AcceptedCount() const noexcept
    {
        return _accepted.load(std::memory_order_relaxed);
    }

  private:
    IListener& _listener;
    CacheEngine& _engine;
    ILogger& _logger;
    IAdmissionControl* _admission;
    IMetricsSink* _metrics;
    SessionContext _session;
    TlsContext* _tls;
    std::atomic<std::uint64_t> _accepted { 0 };
    std::atomic<bool> _shuttingDown { false };
};

} // namespace FastCache
