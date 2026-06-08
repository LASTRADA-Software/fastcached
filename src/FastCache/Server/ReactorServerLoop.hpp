// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IAdmissionControl.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace FastCache
{

class TlsContext; // defined in Net/TlsContext.hpp; only used when TLS is built in

/// Options for the reactor-driven server loop. Bundled so we can pass the
/// same options across the three platform-specific implementations.
struct ReactorServerOptions
{
    std::string bindAddress { "127.0.0.1" };
    std::uint16_t port { 11211 };
    std::size_t maxConnections { 0 }; ///< 0 = unlimited.
    int listenBacklog { 511 };        ///< ::listen() backlog depth.

    /// Number of independent reactors to run, each single-threaded with its
    /// own connections (no coroutine migration). 1 is the classic single-loop
    /// server. >1 scales across cores: on Windows a single acceptor hands
    /// sockets round-robin to N IOCP reactors; on POSIX N listeners share the
    /// port via SO_REUSEPORT. With >1, the storage every connection reaches
    /// must be thread-safe (the caller wraps it in a ShardedStorage).
    unsigned reactorThreads { 1 };

    /// Pin each reactor thread to a distinct CPU core (reactor *i* → core
    /// *i % online_cpus*). Keeps a worker's hot state resident in one core's
    /// caches instead of migrating across cores. Best-effort: ignored when the
    /// platform doesn't support pinning. Only meaningful with reactorThreads>1.
    bool pinReactorsToCpu { false };

    /// Per-server session context forwarded to every connection (auth policy
    /// and other optional collaborators). Default-constructed = auth disabled.
    /// The referenced objects must outlive the server run.
    SessionContext session {};

    /// TLS context for terminating TLS on accepted connections, or nullptr for
    /// plaintext. Only honoured in TLS-enabled builds; must outlive the run.
    TlsContext* tlsContext { nullptr };
};

/// Run the reactor-driven server loop using the platform's native
/// reactor (IocpReactor / EpollReactor / KqueueReactor). Returns 0 on
/// clean shutdown, non-zero on bind failure.
///
/// The loop watches DaemonControls::Instance() for stop requests, so a
/// SIGINT/SIGTERM/SCM Stop will tear it down cleanly. Connection
/// metrics are routed through `metrics` (nullable); admission is gated
/// by `admission` (nullable; nullptr means unbounded).
///
/// @return Process exit code (0 on clean shutdown).
int RunReactorServer(ReactorServerOptions const& options,
                     CacheEngine& engine,
                     ILogger& logger,
                     IAdmissionControl* admission = nullptr,
                     IMetricsSink* metrics = nullptr);

} // namespace FastCache
