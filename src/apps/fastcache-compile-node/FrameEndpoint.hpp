// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Logger.hpp>
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

/// Answers one framed request.
///
/// The seam that lets one accept loop serve every framed surface this node exposes.
/// A node runs a scheduler, a cache tier and a compile worker; each speaks the same
/// `0xFC` framing and differs only in what it answers and how much it will buffer.
/// A second accept loop per surface would be three near-copies of a listener, a
/// shutdown order and a poll timeout -- the copy-paste this codebase treats as a
/// defect rather than a coincidence.
///
/// An interface rather than a `std::function`, deliberately: a responder outlives
/// the endpoint and a closure would keep its whole enclosing scope alive with it.
class IFrameResponder
{
  public:
    virtual ~IFrameResponder() = default;

    IFrameResponder() = default;
    IFrameResponder(IFrameResponder const&) = default;
    IFrameResponder& operator=(IFrameResponder const&) = default;
    IFrameResponder(IFrameResponder&&) = default;
    IFrameResponder& operator=(IFrameResponder&&) = default;

    /// Answer one complete request frame.
    ///
    /// A `Task` because answering may now have to reach the network -- the cache
    /// surface consults an upstream, and that dial suspends rather than blocking
    /// the loop every other connection on this reactor is sharing. A responder
    /// that needs nothing is still free to `co_return` without suspending, which
    /// the scheduler's does; that costs a frame allocation and no round trip.
    ///
    /// @param frame The whole request, header included. A span rather than an
    ///        owning vector because a cache STORE carries an object file and
    ///        copying it here would double the peak footprint on the hot path of
    ///        a parallel build. It must outlive the returned task -- the same
    ///        contract `SendAll` states, and true by construction at the one call
    ///        site, where the backing vector is a local of the calling coroutine.
    /// @param peer The peer's host, for the surfaces whose policy needs one.
    ///        Owned rather than a view: it is short, and every policy-bearing
    ///        responder holds it across a suspension, so a view would make its
    ///        lifetime a rule at each implementation instead of a fact.
    /// @return The encoded reply, or empty to close without answering -- which is
    ///         only ever right when the peer is not speaking this protocol at all.
    [[nodiscard]] virtual Task<std::vector<std::byte>> Answer(std::span<std::byte const> frame, std::string peer) = 0;

    /// Largest request this surface will buffer.
    ///
    /// Per-responder rather than one constant, and the spread is the point: a
    /// scheduler verb carries a fingerprint and a key, while a cache STORE carries a
    /// whole object file. Sizing both for the larger would hand an unauthenticated
    /// peer a way to make the scheduler allocate megabytes.
    [[nodiscard]] virtual std::size_t MaxRequestBytes() const noexcept = 0;
};

/// Accepts connections and answers each with one framed request.
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
class FrameServer
{
  public:
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
    /// @param responder Answers each request; must outlive the run.
    /// @param what Names this surface in log lines, so three endpoints in one
    ///        process are distinguishable when one of them stops accepting.
    /// @param logger Shared logger.
    FrameServer(IListener& listener, IFrameResponder& responder, std::string_view what, ILogger& logger) noexcept;

    /// Accept loop; returns when the listener is closed via `Shutdown()`.
    [[nodiscard]] Task<void> Run();

    /// Close the listener to unblock `Run()`.
    void Shutdown() noexcept;

  private:
    IListener& _listener;
    IFrameResponder& _responder;
    std::string _what;
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
class FrameEndpoint
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
    /// @param defaultHost What a bare port binds to. Differs per surface and is the
    ///        caller's decision: a scheduler no peer can dial does nothing, while a
    ///        node's private cache reachable from the network is a decision.
    /// @param responder Answers each request; must outlive the endpoint.
    /// @param what Names this surface in log lines.
    /// @param logger Where to announce the bound address.
    /// @return The running endpoint, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<FrameEndpoint>, std::string> Start(std::string_view listenSpec,
                                                                                          std::string_view defaultHost,
                                                                                          IFrameResponder& responder,
                                                                                          std::string_view what,
                                                                                          ILogger& logger);

    /// Stop serving and join the thread.
    ~FrameEndpoint();

    FrameEndpoint(FrameEndpoint const&) = delete;
    FrameEndpoint& operator=(FrameEndpoint const&) = delete;

    // Neither movable, for the reason `AdminEndpoint` gives: the serving thread holds
    // a pointer to `_server`, and `_server` a reference to `_listener`.
    FrameEndpoint(FrameEndpoint&&) = delete;
    FrameEndpoint& operator=(FrameEndpoint&&) = delete;

    /// The address this endpoint actually bound.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

  private:
    FrameEndpoint(std::unique_ptr<BlockingListener> listener,
                  IFrameResponder& responder,
                  std::string_view what,
                  std::string boundEndpoint,
                  ILogger& logger);

    std::unique_ptr<BlockingListener> _listener;
    std::unique_ptr<FrameServer> _server;
    std::string _boundEndpoint;
    std::jthread _thread;
};

} // namespace FastCache::Node
