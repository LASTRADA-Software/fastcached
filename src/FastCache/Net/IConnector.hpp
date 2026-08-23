// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>

namespace FastCache
{

/// How a component opens an **outbound** connection.
///
/// The counterpart to `IListener`, and the seam this codebase was missing: every
/// server-side path reaches the network through `IListener`/`ISocket`, while the
/// one place that dialled out — `HttpHealthProbe` — did it through a free
/// function with the socket API inlined, which is exactly the shape the
/// dependency-injection rule exists to prevent. Raft is the first component that
/// must reach a peer rather than wait for one, so the seam is introduced here.
///
/// ## Why synchronous, and why that is not a reactor violation
///
/// `Connect` blocks the calling thread until it succeeds, fails, or times out.
/// That is deliberate and is the same reasoning `IRaftStorage` records: the
/// caller has nothing to do until the connection exists, so expressing it as a
/// coroutine would buy the ability to interleave work that does not exist.
///
/// The rule it must not break is that **a reactor thread never calls this**. The
/// reactor multiplexes thousands of client connections and a blocking dial would
/// stall all of them; a component that dials does so from a thread of its own,
/// which is what `RaftPeerTransport` gives each peer. A handful of peers is a
/// handful of threads, against the reactor's job of not needing one per client.
///
/// ## The timeout is a parameter, not a policy
///
/// Passed per call rather than fixed at construction, and it is load-bearing
/// rather than a nicety: the OS default connect timeout runs to minutes on some
/// systems, so a caller that wants to notice a dead peer and retry — or simply
/// to shut down — cannot be made to wait for it. That is the same lesson
/// `BlockingListener::SetTimeouts` records for the accept side, where a listener
/// that cannot be woken cannot be stopped.
class IConnector
{
  public:
    IConnector() = default;
    IConnector(IConnector const&) = delete;
    IConnector(IConnector&&) = delete;
    IConnector& operator=(IConnector const&) = delete;
    IConnector& operator=(IConnector&&) = delete;
    virtual ~IConnector() = default;

    /// Open a connection to `host:port`.
    /// @param host Hostname or literal address, IPv4 or IPv6.
    /// @param port TCP port in host byte order.
    /// @param timeout How long to wait before giving up. A non-positive value
    ///        leaves the platform default in place, which may be minutes.
    /// @return The connected socket, or why the attempt did not succeed.
    [[nodiscard]] virtual std::expected<std::unique_ptr<ISocket>, NetError> Connect(std::string_view host,
                                                                                    std::uint16_t port,
                                                                                    std::chrono::milliseconds timeout) = 0;
};

} // namespace FastCache
