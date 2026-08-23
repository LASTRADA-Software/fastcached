// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

namespace FastCache::Node
{

/// The worker's `/metrics` and `/healthz` endpoint: listener, server and the
/// thread that serves them, owned as one thing.
///
/// A class rather than three locals in `main()` for two reasons, and the second is
/// the one that matters. The three have a *destruction order* -- the server must be
/// told to close its listener before the thread serving it can be joined -- and a
/// `main()` holding them separately expresses that as a `Shutdown()` call somebody
/// has to remember at every return path. Here it is the destructor, which is the
/// RAII rule this codebase applies to every other resource handle. And `main()` in
/// this binary is in no test target, so wiring that lives there has no unit
/// coverage at all -- the mistake `CacheProtocol.cpp` and `RootReconciler.cpp` are
/// each recorded as having been extracted to avoid.
class AdminEndpoint
{
  public:
    /// Bind the endpoint and start serving it.
    ///
    /// Fallible, and reported rather than warned about: an operator who asked a
    /// *worker* for an endpoint is almost always wiring a probe to it, so a worker
    /// that silently started without one looks healthy to the very thing that
    /// would otherwise have noticed it was not.
    ///
    /// The error is a diagnostic string rather than one of the project's four error
    /// enums, and that is a deliberate departure. This fails in two ways that belong
    /// to two different taxonomies -- a malformed `--admin-listen` is a
    /// `ConfigError`, an address that will not bind is a `NetError` -- and
    /// `ConfigErrorCode` has no enumerator that describes the second at all. The
    /// caller's response is identical either way (log it, refuse to start), so
    /// picking one enum would buy nothing and mislabel half the failures. What the
    /// caller *does* need is the text, because the flag may have been a bare port
    /// and the message has to say what that resolved to.
    /// @param listenSpec `port`, `host:port` or `[v6]:port`.
    /// @param defaultHost What a bare port binds to; loopback for a scrape surface.
    /// @param metrics The sink to render.
    /// @param snapshot What to report per scrape.
    /// @param logger Where to announce the bound address.
    /// @return The running endpoint, or why it could not be served.
    [[nodiscard]] static std::expected<std::unique_ptr<AdminEndpoint>, std::string> Start(
        std::string_view listenSpec,
        std::string_view defaultHost,
        IMetricsSink& metrics,
        AdminHttpServer::SnapshotProvider snapshot,
        ILogger& logger);

    /// Stop serving and join the thread.
    ~AdminEndpoint();

    AdminEndpoint(AdminEndpoint const&) = delete;
    AdminEndpoint& operator=(AdminEndpoint const&) = delete;

    // Neither movable: the serving thread holds a pointer to `_server`, and
    // `_server` a reference to `_listener`. Both survive a move of the owning
    // object only because they are behind `unique_ptr` -- but saying so and
    // relying on it are different things, and the type is always held by pointer.
    AdminEndpoint(AdminEndpoint&&) = delete;
    AdminEndpoint& operator=(AdminEndpoint&&) = delete;

    /// The address this endpoint actually bound.
    /// @return Host and port, as text.
    [[nodiscard]] std::string const& BoundEndpoint() const noexcept
    {
        return _boundEndpoint;
    }

  private:
    /// Take ownership of an already-bound listener and start serving it.
    /// @param listener The bound listener.
    /// @param metrics The sink to render.
    /// @param snapshot What to report per scrape.
    /// @param boundEndpoint What `BoundEndpoint()` reports.
    /// @param logger Where the server reports.
    AdminEndpoint(std::unique_ptr<BlockingListener> listener,
                  IMetricsSink& metrics,
                  AdminHttpServer::SnapshotProvider snapshot,
                  std::string boundEndpoint,
                  ILogger& logger);

    std::unique_ptr<BlockingListener> _listener;
    std::unique_ptr<AdminHttpServer> _server;
    std::string _boundEndpoint;
    std::jthread _thread;
};

} // namespace FastCache::Node
