// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IDatagramSocket.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace FastCache
{

/// Whether a socket may address the subnet broadcast.
///
/// An `enum class` rather than a `bool` because the call site is stating a
/// capability the kernel refuses by default, and `true` says nothing about which.
enum class BroadcastMode : std::uint8_t
{
    Off, ///< Unicast only.
    On,  ///< `SO_BROADCAST`, so a beacon may reach the segment.
};

/// Whether other sockets may hold this address at the same time.
///
/// An `enum class` for the reason `BroadcastMode` is one: the call site is
/// stating who owns a port, and `true` says nothing about which way round.
///
/// The distinction is load-bearing rather than a tidying. A shared port is what
/// lets every node on a segment hear the same broadcast; it is also what makes a
/// unicast to that port arrive at only one of them, so a socket that must be
/// *reachable* -- one whose whole job is that an answer addressed to it is handed
/// to it -- has to be exclusive. `Cluster/DiscoveryService` needs one of each.
enum class PortSharing : std::uint8_t
{
    /// This socket alone holds the address it bound.
    Exclusive,

    /// Several sockets may hold it, and each hears what is broadcast to it.
    Shared,
};

/// A UDP socket, as discovery uses it.
///
/// The only part of discovery that touches the network; everything above it is
/// pure and tested against `DatagramBus`. Blocking rather than reactor-driven on
/// purpose: discovery sends a handful of datagrams a minute on its own thread,
/// so the reactor buys nothing here and the loop stays readable.
///
/// The receive timeout is not a convenience. POSIX does not unblock a parked
/// `recvfrom()` when another thread closes the socket, so a poll timeout is the
/// only portable way the loop observes a shutdown -- the same rule
/// `BlockingListener::SetTimeouts` exists for, and whose omission this repository
/// has already paid for once as a `systemctl stop` that hung until the supervisor
/// escalated to SIGKILL.
///
/// @param bindAddress Address to bind, e.g. `0.0.0.0`.
/// @param port Port to bind; 0 lets the kernel choose, which is what a client
///        side of a handshake wants.
/// @param broadcast Whether this socket may send to a broadcast address.
/// @param sharing Whether other sockets may hold the same address. Exclusive by
///        default, because that is what a socket expecting to be answered needs
///        and sharing is the exception a caller states on purpose.
/// @return The socket, or nullptr when it could not be bound.
[[nodiscard]] std::unique_ptr<IDatagramSocket> OpenUdpSocket(std::string_view bindAddress,
                                                             std::uint16_t port,
                                                             BroadcastMode broadcast,
                                                             PortSharing sharing = PortSharing::Exclusive);

} // namespace FastCache
