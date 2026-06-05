// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/IAdmissionControl.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace FastCache
{

/// Options for the reactor-driven server loop. Bundled so we can pass the
/// same options across the three platform-specific implementations.
struct ReactorServerOptions
{
    std::string bindAddress { "127.0.0.1" };
    std::uint16_t port { 11211 };
    std::size_t maxConnections { 0 }; ///< 0 = unlimited.
    int listenBacklog { 511 };        ///< ::listen() backlog depth.

    /// Number of threads draining the reactor. 1 keeps strict single-threaded
    /// semantics (the in-memory fast path, where the unwrapped storage is not
    /// thread-safe). >1 is honoured only on platforms whose reactor is
    /// multi-thread safe (Windows IOCP today); elsewhere the loop runs
    /// single-threaded regardless. With >1 threads every IStorage the
    /// connections reach must be thread-safe (the disk backend is wrapped in a
    /// ShardedStorage by the caller).
    unsigned reactorThreads { 1 };
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
