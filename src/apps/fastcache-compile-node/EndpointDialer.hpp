// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Node
{

/// How this node opens a connection to an endpoint it was told to DIAL.
///
/// A seam at the DIAL rather than at the connector, and the altitude is the whole
/// point. The properties the callers own -- that a redirect budget bounds a
/// CONSECUTIVE chain, that an unreachable `--scheduler` falls back to the next one in
/// the SAME round -- are expressible only if a test can script what comes BACK.
/// Replies arrive on the socket, so the socket is what has to be scriptable; a
/// connector seam would let a test vary HOW the dial happens while the property is
/// about WHAT the other end answered.
///
/// Deliberately **not** an `IConnector`. `Cc::DialEndpointBlocking` takes a
/// `BlockingConnector&` by concrete type because its soundness rests on the connector
/// resolving inline and never leaving its task suspended -- there the type IS the
/// rule, and relaxing that parameter would delete a guard rather than widen one
/// (`apps/fastcache-cc/EndpointDial.hpp`). This seam keeps the concrete type inside
/// `BlockingEndpointDialer`, where that function still sees exactly what it requires.
///
/// Every caller dials from a thread that may block: the heartbeat thread, and the
/// one-shot verbs on the process main thread, where no reactor exists.
class IEndpointDialer
{
  public:
    IEndpointDialer() = default;
    IEndpointDialer(IEndpointDialer const&) = delete;
    IEndpointDialer(IEndpointDialer&&) = delete;
    IEndpointDialer& operator=(IEndpointDialer const&) = delete;
    IEndpointDialer& operator=(IEndpointDialer&&) = delete;
    virtual ~IEndpointDialer() = default;

    /// Dial one endpoint and hand back a connected socket.
    /// @param endpoint `host:port`; a bare port names no machine and is refused.
    /// @param options Ceiling on the dial.
    /// @return The connected socket, or nullptr when it could not be reached.
    [[nodiscard]] virtual std::unique_ptr<ISocket> Dial(std::string_view endpoint, DialOptions options) = 0;
};

/// The dialer production uses: a real `BlockingConnector`, constructed per dial.
///
/// Per dial rather than once: a redirect or a fallback moves the endpoint, and a
/// connector carries socket-level timeouts for the dial it is about.
class BlockingEndpointDialer final: public IEndpointDialer
{
  public:
    /// @param ioTimeout Per-call send/recv ceiling armed on every socket this hands
    ///        over, before it is handed over.
    explicit BlockingEndpointDialer(std::chrono::milliseconds ioTimeout) noexcept;

    /// @copydoc IEndpointDialer::Dial
    [[nodiscard]] std::unique_ptr<ISocket> Dial(std::string_view endpoint, DialOptions options) override;

  private:
    std::chrono::milliseconds _ioTimeout;
};

/// Per-call send/recv ceiling for an operator's one-shot verb.
///
/// Generous, because an operator typed this and is watching it: a slow answer costs a
/// person some seconds, while a short bound turns a loaded leader into a failed
/// command that has to be retyped.
inline constexpr std::chrono::milliseconds OneShotIoTimeout { 10'000 };

/// Process-singleton blocking dialer for the one-shot verbs, so a production path
/// does not have to carry a seam it has no reason to vary. Mirrors `DefaultDrainWait()`
/// in `Core/BoundedDrain.hpp`; tests pass their own.
/// @return Reference to a singleton `BlockingEndpointDialer` with `OneShotIoTimeout`.
[[nodiscard]] IEndpointDialer& DefaultOneShotDialer() noexcept;

/// A connection, and which of several endpoints it reached.
struct ReachedEndpoint
{
    std::unique_ptr<ISocket> socket; ///< Connected; never null.
    std::string endpoint;            ///< Which endpoint answered, for every later diagnostic.
};

/// Dial @p endpoints in order and hand back the first that connects.
///
/// **A fallback happens only where nothing was SENT.** A dial that did not connect
/// delivered no request, so trying the next endpoint cannot apply anything twice. A
/// connection that then fails mid-exchange is the caller's to report and never a
/// reason to ask somebody else: the request may already have been applied where it
/// landed, and the cluster verbs include `--cluster-admit` and `--enroll-approve`.
/// @param dialer How each endpoint is dialled.
/// @param endpoints Where to try, in order; typically `NodeConfig::schedulers`.
/// @param options Ceiling on each dial.
/// @return The first connection and its endpoint, or nothing when none connected.
[[nodiscard]] std::optional<ReachedEndpoint> DialFirstReachable(IEndpointDialer& dialer,
                                                                std::span<std::string const> endpoints,
                                                                DialOptions options);

/// Render a list of endpoints for a sentence an operator reads.
/// @param endpoints The endpoints, in the order they were tried.
/// @return `a:1, b:2`, or an empty string for an empty list.
[[nodiscard]] std::string JoinEndpoints(std::span<std::string const> endpoints);

} // namespace FastCache::Node
