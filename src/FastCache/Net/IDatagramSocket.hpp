// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/SocketAddress.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace FastCache
{

/// One datagram, and who sent it.
struct ReceivedDatagram
{
    std::vector<std::byte> payload; ///< The bytes, exactly as they arrived.

    /// The sender's address as text, `host:port`.
    ///
    /// Text rather than a parsed structure because every consumer of it either
    /// logs it or hands it back to a connector, and both want the string. What a
    /// beacon *claims* about itself travels in the payload; this is what the
    /// kernel says, and the two are deliberately separate -- a peer that
    /// advertises an address it does not answer on is the failure discovery
    /// exists to make visible, not one to paper over by trusting the payload.
    std::string from;
};

/// Why a receive returned nothing.
enum class DatagramWait : std::uint8_t
{
    TimedOut, ///< Nothing arrived within the deadline. Ordinary, not a fault.
    Closed,   ///< The socket was shut down; the loop should end.
};

/// A connectionless socket, as the discovery layer needs it.
///
/// The seam for the one piece of discovery that touches the network. Everything
/// above it -- what a beacon says, which peers are remembered, when one expires,
/// whether a join is authentic -- is pure and testable against a scripted "LAN"
/// in one process, which is the same split `WorkerRegistry` and `LeaseTable`
/// already have and for the same reason.
///
/// Deliberately not `ISocket`. A datagram socket has no connection, no stream and
/// no partial read: a receive either yields one whole message or nothing, and a
/// send either places one whole message or fails. Modelling that through an
/// interface built for byte streams would mean every implementation and every
/// test double carrying framing that datagrams already have.
class IDatagramSocket
{
  public:
    IDatagramSocket() = default;
    IDatagramSocket(IDatagramSocket const&) = delete;
    IDatagramSocket(IDatagramSocket&&) = delete;
    IDatagramSocket& operator=(IDatagramSocket const&) = delete;
    IDatagramSocket& operator=(IDatagramSocket&&) = delete;
    virtual ~IDatagramSocket() = default;

    /// Send one datagram.
    ///
    /// A datagram that does not arrive is not an error here and never will be:
    /// this reports only that the local stack accepted it. Discovery is built to
    /// survive loss -- beacons repeat -- so a caller that treated a successful
    /// send as delivery would be wrong about the one thing UDP does not promise.
    /// @param payload The whole message.
    /// @param to Destination as `host:port`.
    /// @return Nothing, or why the local stack refused it.
    [[nodiscard]] virtual std::expected<void, NetError> Send(std::span<std::byte const> payload, std::string_view to) = 0;

    /// Wait for one datagram.
    ///
    /// Bounded by @p timeout, and that is load-bearing rather than a convenience:
    /// POSIX does not unblock a parked receive when another thread closes the
    /// socket, so a poll timeout is the only portable way a discovery loop ever
    /// observes a shutdown. This repository has already paid for the equivalent
    /// omission on `accept()` -- a `systemctl stop` that hung until the supervisor
    /// escalated to SIGKILL.
    /// @param timeout How long to wait.
    /// @return The datagram, or why none was returned.
    [[nodiscard]] virtual std::expected<ReceivedDatagram, DatagramWait> Receive(std::chrono::milliseconds timeout) = 0;

    /// Stop the socket, so a parked Receive returns Closed at its next poll.
    virtual void Close() noexcept = 0;

    /// The address this socket actually bound, as `host:port`.
    /// @return The bound address, empty when it is not bound.
    [[nodiscard]] virtual std::string BoundEndpoint() const = 0;
};

} // namespace FastCache
