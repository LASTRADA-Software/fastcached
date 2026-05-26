// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IAdmissionControl.hpp>
#include <FastCache/Net/IListener.hpp>

#include <atomic>

namespace FastCache
{

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
    Server(IListener& listener,
           CacheEngine& engine,
           ILogger& logger,
           IAdmissionControl* admission = nullptr,
           IMetricsSink* metrics = nullptr) noexcept;

    /// Run the accept loop until Shutdown() is called or the listener closes.
    /// @return Task that resolves when the accept loop exits.
    [[nodiscard]] Task<void> Run();

    /// Ask the accept loop to stop. The next failed Accept() (closed
    /// listener) breaks the loop; current connections continue under their
    /// own coroutines.
    void Shutdown() noexcept;

    /// @return Number of connections accepted since construction.
    [[nodiscard]] std::uint64_t AcceptedCount() const noexcept { return _accepted.load(std::memory_order_relaxed); }

  private:
    IListener& _listener;
    CacheEngine& _engine;
    ILogger& _logger;
    IAdmissionControl* _admission;
    IMetricsSink* _metrics;
    std::atomic<std::uint64_t> _accepted { 0 };
    std::atomic<bool> _shuttingDown { false };
};

} // namespace FastCache
