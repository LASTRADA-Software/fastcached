// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/IListener.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace FastCache::Node
{

/// Accepts connections and answers each with one scheduler verb.
///
/// Shaped after `WorkerServer` rather than `Server`, and for the reason that governs
/// the whole node: `Connection` is built around a `CacheEngine`, and a scheduler has
/// no cache. Taking an `IListener&` and running its own loop keeps the node clear of
/// the cache stack while still reusing the reactor and the socket abstraction.
///
/// ## One request per connection
///
/// The same shape every other client of this wire already uses -- the launcher opens
/// a fresh connection per cache operation, and a worker per heartbeat. There is no
/// command loop because there is no per-connection state to justify one: every verb
/// here is answered from the scheduler's own tables, so a second request on the same
/// socket would buy nothing but a way for one client to hold a descriptor open.
///
/// ## The payload cap is small on purpose
///
/// Membership is checked *inside* the service, which means after the frame has been
/// read -- so an unauthenticated peer can make this endpoint buffer whatever it
/// declares. That is the same hole `OpDescriptor::maxPayload` closes for `AUTH` on
/// the cache port, and it is closed the same way: a scheduler verb carries a
/// fingerprint, an endpoint and a key, none of which is large, so the ceiling is
/// kilobytes rather than the cache's megabytes. A frame over it is refused with a
/// *reply* naming both numbers, not a close.
class SchedulerServer
{
  public:
    /// Largest request this endpoint will buffer.
    ///
    /// Reachable before membership is established, so it is sized for the verbs
    /// rather than for comfort: the longest is a REGISTER carrying a toolchain
    /// fingerprint, an endpoint and a codec list.
    static constexpr std::size_t MaxRequestBytes = 64ULL * 1024ULL;

    /// How often a parked `Accept()` returns so the loop can observe `Shutdown()`.
    ///
    /// POSIX does not unblock a parked `accept()` when another thread closes the
    /// listening socket, so this poll interval *is* the shutdown mechanism rather
    /// than a tuning knob -- the lesson `WorkerServer` records having learned as a
    /// 900-second CI timeout naming nothing.
    static constexpr std::chrono::milliseconds AcceptPoll { 250 };

    /// How long one request may take to arrive before its socket is abandoned.
    static constexpr std::chrono::milliseconds RequestTimeout { 5'000 };

    /// @param listener Bound listener; must outlive the run.
    /// @param protocol Answers each request; must outlive the run.
    /// @param membership Decides who may spend the fleet's capacity; must outlive the run.
    /// @param logger Shared logger.
    SchedulerServer(IListener& listener,
                    Distributed::SchedulerProtocol& protocol,
                    Distributed::IMembershipOracle const& membership,
                    ILogger& logger) noexcept;

    /// Accept loop; returns when the listener is closed via `Shutdown()`.
    [[nodiscard]] Task<void> Run();

    /// Close the listener to unblock `Run()`.
    void Shutdown() noexcept;

  private:
    IListener& _listener;
    Distributed::SchedulerProtocol& _protocol;
    Distributed::IMembershipOracle const& _membership;
    ILogger& _logger;
    std::atomic<bool> _shuttingDown { false };
};

/// The node's scheduler port: listener, server and the thread that serves them,
/// owned as one thing.
///
/// A class rather than three locals in `main()`, for the reason `AdminEndpoint`
/// records: the three have a *destruction order* -- the server must close its
/// listener before the thread serving it can be joined -- and holding them separately
/// expresses that as a `Shutdown()` somebody has to remember at every return path.
/// Here it is the destructor. And `main.cpp` in this binary is in no test target, so
/// wiring that lives there has no unit coverage at all.
class SchedulerEndpoint
{
  public:
    /// Bind the endpoint and start serving it.
    ///
    /// The error is a diagnostic string rather than one of the project's four error
    /// enums, the same deliberate departure `AdminEndpoint::Start` documents: this
    /// fails in two ways belonging to two taxonomies -- a malformed listen spec is a
    /// `ConfigError`, an address that will not bind is a `NetError` -- the caller's
    /// response is identical either way, and what it needs is the text.
    /// @param listenSpec `port`, `host:port` or `[v6]:port`.
    /// @param defaultHost What a bare port binds to.
    /// @param protocol Answers each request; must outlive the endpoint.
    /// @param membership Decides who may be scheduled; must outlive the endpoint.
    /// @param logger Where to announce the bound address.
    /// @return The running endpoint, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<SchedulerEndpoint>, std::string> Start(
        std::string_view listenSpec,
        std::string_view defaultHost,
        Distributed::SchedulerProtocol& protocol,
        Distributed::IMembershipOracle const& membership,
        ILogger& logger);

    /// Stop serving and join the thread.
    ~SchedulerEndpoint();

    SchedulerEndpoint(SchedulerEndpoint const&) = delete;
    SchedulerEndpoint& operator=(SchedulerEndpoint const&) = delete;

    // Neither movable, for the reason `AdminEndpoint` gives: the serving thread holds
    // a pointer to `_server`, and `_server` a reference to `_listener`.
    SchedulerEndpoint(SchedulerEndpoint&&) = delete;
    SchedulerEndpoint& operator=(SchedulerEndpoint&&) = delete;

    /// The address this endpoint actually bound.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

  private:
    SchedulerEndpoint(std::unique_ptr<BlockingListener> listener,
                      Distributed::SchedulerProtocol& protocol,
                      Distributed::IMembershipOracle const& membership,
                      std::string boundEndpoint,
                      ILogger& logger);

    std::unique_ptr<BlockingListener> _listener;
    std::unique_ptr<SchedulerServer> _server;
    std::string _boundEndpoint;
    std::jthread _thread;
};

} // namespace FastCache::Node
