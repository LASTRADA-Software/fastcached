// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <atomic>
#include <cstddef>

#include <WorkerProtocol.hpp>

namespace FastCache::Node
{

/// Accepts connections and answers each with one compile.
///
/// Shaped after `AdminHttpServer` rather than `Server`, and for the reason that
/// governs the whole node: `Connection` is built around a `CacheEngine`, and a
/// worker has no cache. Taking an `IListener&` and running its own loop keeps the
/// node clear of the cache stack entirely while still reusing the reactor, the
/// socket abstraction and the TLS wrapper.
///
/// ## One request per connection
///
/// A client opens a connection, sends one `Compile`, reads one reply, and closes —
/// the same shape the launcher already uses for every cache operation. There is no
/// command loop here on purpose: a compile occupies a slot for seconds, so a
/// connection that could send a second one would let a single client hold a slot
/// indefinitely, and the slot accounting the scheduler does would stop meaning
/// anything.
///
/// ## The concurrency cap is the promise the scheduler relies on
///
/// A worker advertises `slots` at registration and the scheduler dispatches on that
/// number. If the worker accepted more, its compiles would contend for the same
/// cores and every one of them would get slower — the fleet would be *fuller* than
/// the scheduler thinks and *slower* than it thinks, in the same moment. So the cap
/// is enforced here as well as advertised, and a job over it is refused rather than
/// queued: refusing costs the client one local compile, while queueing hides the
/// overload from the scheduler that is trying to route around it.
class WorkerServer
{
  public:
    /// @param listener Bound listener; must outlive the run.
    /// @param protocol Answers each request; must outlive the run.
    /// @param slots Maximum concurrent compiles.
    /// @param logger Shared logger.
    WorkerServer(IListener& listener, Cc::WorkerProtocol& protocol, std::size_t slots, ILogger& logger) noexcept;

    /// Accept loop; returns when the listener is closed via `Shutdown()`.
    [[nodiscard]] Task<void> Run();

    /// Close the listener to unblock `Run()`.
    void Shutdown() noexcept;

    /// Compiles running right now, for the heartbeat.
    [[nodiscard]] std::size_t InFlight() const noexcept;

  private:
    IListener& _listener;
    Cc::WorkerProtocol& _protocol;
    std::size_t _slots;
    ILogger& _logger;
    std::atomic<bool> _shuttingDown { false };
    std::atomic<std::size_t> _inFlight { 0 };
};

} // namespace FastCache::Node
