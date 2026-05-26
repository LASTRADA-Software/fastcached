// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IListener.hpp>

#include <atomic>

namespace FastCache
{

/// Synchronous production accept loop. Blocks on listener.Accept() and
/// spawns a detached std::jthread per accepted connection that drives the
/// connection's coroutine via SyncRun. Returns when:
///   - the listener is closed externally (Accept returns Cancelled/BadFile)
///   - shouldStop becomes true between accepts
///
/// @param listener Listener to drive.
/// @param engine Cache engine; lifetime must exceed every spawned thread.
/// @param logger Logger; same lifetime requirement.
/// @param shouldStop Polled between accepts; set from a signal handler.
/// @return Number of connections accepted before exit.
std::uint64_t RunBlockingServerLoop(IListener& listener,
                                    CacheEngine& engine,
                                    ILogger& logger,
                                    std::atomic<bool>& shouldStop);

} // namespace FastCache
