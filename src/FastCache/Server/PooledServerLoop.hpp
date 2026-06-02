// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IListener.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FastCache
{

/// Thread-pool-backed accept loop.
///
/// Spawns a fixed number of worker threads at startup; the accept loop
/// runs on the caller's thread. Accepted `ISocket`s are pushed into a
/// bounded blocking queue; workers loop popping sockets and driving the
/// per-connection `Connection::Run()` coroutine via `SyncRun`.
///
/// Why a fixed pool: under workloads like sccache (one connection per
/// compile job) a thousand-job build would otherwise pay a thousand
/// `pthread_create` / `_beginthreadex` round-trips on the hot path. The
/// pool's workers are created once and reused for every connection.
///
/// Shutdown: setting `shouldStop` and closing the listener causes the
/// accept loop to exit. The destructor of the internal pool then pushes
/// sentinels into the queue and joins every worker.
///
/// @param listener    Listener to drive. Caller closes it (e.g. from a
///                    watchdog thread) when the server should stop.
/// @param engine      Cache engine; lifetime must exceed every worker.
/// @param logger      Logger; same lifetime requirement.
/// @param shouldStop  Polled between accepts; set by a signal handler.
/// @param poolSize    Number of worker threads to create. Must be > 0;
///                    zero is replaced by `std::thread::hardware_concurrency()`
///                    (with a floor of 1 if hardware_concurrency returns 0).
/// @return Number of connections accepted before exit.
std::uint64_t RunPooledServerLoop(
    IListener& listener, CacheEngine& engine, ILogger& logger, std::atomic<bool>& shouldStop, std::size_t poolSize);

} // namespace FastCache
