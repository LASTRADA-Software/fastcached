// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IDatagramSocket.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace FastCache
{

/// A whole network segment, in one process.
///
/// The counterpart to `InMemoryTransport` for datagrams, and the reason the
/// discovery layer is testable at all: a broadcast fan-out, a lost datagram and
/// two fleets sharing a segment are each a scripted call here rather than a
/// second machine and a sleep.
///
/// Loss is **scripted, not simulated**. A test that dropped datagrams at random
/// would fail occasionally for reasons nobody could reproduce, and discovery's
/// whole contract is that it survives loss -- so the interesting cases are the
/// specific ones ("this beacon never arrives"), which is what `DropNext` states.
///
/// **Several sockets may hold one address**, because a UDP port is shareable and
/// discovery shares one: every node on the segment binds the beacon port, and two
/// nodes on one host bind it on the same machine. A double that gave one address
/// one inbox could not express that at all, which is why the defect it hides went
/// unnoticed -- see `Deliver` for what sharing does and does not buy.
class DatagramBus
{
  public:
    /// The address every broadcast is sent to, whatever port its listeners bound.
    ///
    /// A name rather than a real broadcast address: what matters to the layer
    /// above is "everyone on the segment sees this", and encoding a subnet here
    /// would make the double model a routing detail it has no opinion about.
    ///
    /// Port zero, meaning every inbox on the segment whatever port it bound: a
    /// test whose nodes each hold exactly one socket has nothing to say about
    /// which port a broadcast is scoped to, so naming one at every such call site
    /// would be noise around the half that matters. `BroadcastAddressOn` is for
    /// the tests that do.
    /// @return The sentinel destination.
    [[nodiscard]] static DatagramAddress const& BroadcastAddress()
    {
        static DatagramAddress const address { .host = "broadcast", .port = 0 };
        return address;
    }

    /// The address a broadcast to @p port is sent to.
    ///
    /// What a real broadcast does: `255.255.255.255:P` reaches the sockets bound
    /// to `P` and no others. It matters as soon as one node holds sockets on two
    /// ports -- a beacon must reach the socket listening for beacons and not the
    /// one waiting for the answer to one -- which is the shape
    /// `Net/SharedPortDatagram` gives every node.
    /// @param port Which port's sockets hear it.
    /// @return The sentinel destination, scoped to @p port.
    [[nodiscard]] static DatagramAddress BroadcastAddressOn(std::uint16_t port)
    {
        return DatagramAddress { .host = BroadcastAddress().host, .port = port };
    }

    /// Attach a socket at @p endpoint.
    /// @param endpoint The address other sockets reach it by; it need not be free.
    /// @return The socket; it detaches on destruction.
    [[nodiscard]] std::unique_ptr<IDatagramSocket> Open(DatagramAddress endpoint);

    /// Drop the next @p count datagrams sent to @p endpoint.
    ///
    /// Per destination, so a test can starve one peer while the rest of the
    /// segment carries on -- which is the partition shape discovery has to
    /// survive, and the one a global drop rate cannot express. Every socket
    /// sharing @p endpoint is starved: the address is what a test names, and
    /// which socket behind it a kernel would have picked is the thing this double
    /// deliberately does not promise.
    ///
    /// **Only sockets that are already open are starved**, and the count is
    /// returned rather than discarded so that scripting a loss against an address
    /// nobody holds cannot pass for having scripted one. A test that starved
    /// nobody and then asserted a datagram never arrived would be green for the
    /// wrong reason, which is worse than red.
    /// @param endpoint Whose inbox to starve.
    /// @param count How many datagrams to discard.
    /// @return How many sockets were starved.
    [[nodiscard]] std::size_t DropNext(DatagramAddress const& endpoint, std::size_t count);

    /// How many datagrams have been placed on the bus, delivered or dropped.
    /// @return The count.
    [[nodiscard]] std::size_t SendCount() const;

  private:
    friend class InMemoryDatagramSocket;

    /// One socket's queue.
    struct Inbox
    {
        DatagramAddress endpoint;              ///< What this socket bound; never changes.
        std::deque<ReceivedDatagram> queue {}; ///< Datagrams awaiting a receive.
        std::size_t dropsRemaining { 0 };      ///< Scripted losses still to apply.
        bool closed { false };                 ///< Whether its socket has been closed.
    };

    /// What `Attach` hands a socket to work through.
    ///
    /// The reference outlives every use of it, because only the socket's own
    /// destructor detaches -- so a socket destroyed while another thread is
    /// parked in its `Receive` is a use-after-free rather than the stuck wait it
    /// would have been when an address was a key. That is the lifetime rule every
    /// socket in this tree already has (`DiscoveryTier` joins its loop before the
    /// socket goes), stated here because this is where breaking it stopped being
    /// survivable.
    struct Attachment
    {
        std::size_t id; ///< What `Detach` takes back.
        Inbox& inbox;   ///< Stable until `Detach`; `std::map` does not move its values.
    };

    /// Place @p payload in the inboxes @p to names.
    ///
    /// **A broadcast reaches every socket on its port; a unicast reaches exactly
    /// one.** That asymmetry is the whole reason this double models a shared
    /// address, because it is what a real kernel does and what two nodes on one
    /// host fail on: both hear each other's beacons and only one of them is
    /// handed the challenge that answers one.
    ///
    /// Which one is the first to have attached. A real kernel's answer differs
    /// between platforms -- measured, Windows 11 hands a unicast to the
    /// first-bound socket and Linux to the last -- so there is no portable
    /// behaviour to copy. What every platform agrees on is that it is *one*, and
    /// that is the property under test; picking at random instead would fail
    /// unreproducibly, which is what this file's scripted-loss rule already says
    /// about the other source of nondeterminism.
    ///
    /// A closed inbox still receives, and still counts as the socket holding its
    /// address. That mirrors the real one: `UdpSocket::Close` sets a flag so a
    /// parked receive can return, and the descriptor stays bound until the socket
    /// is destroyed -- so the kernel goes on queueing datagrams nobody will read.
    /// @param payload The datagram.
    /// @param to A specific endpoint, or a broadcast address.
    /// @param from Who sent it.
    void Deliver(std::span<std::byte const> payload, DatagramAddress const& to, DatagramAddress const& from);

    /// Register an inbox.
    /// @param endpoint Its address, which other sockets may already hold.
    /// @return Its ordinal and its inbox.
    [[nodiscard]] Attachment Attach(DatagramAddress endpoint);

    /// Remove an inbox.
    /// @param id The ordinal `Attach` returned.
    void Detach(std::size_t id);

    mutable std::mutex _mutex;
    std::condition_variable _arrived;
    /// Keyed by the order sockets attached in, not by their address: an address
    /// may be held by several sockets, and one of them has to be nameable as
    /// "first" for `Deliver` to have a rule at all. An ordered map, so iterating
    /// it IS attach order.
    std::map<std::size_t, Inbox> _inboxes;
    std::size_t _nextId { 0 };
    std::size_t _sendCount { 0 };
};

} // namespace FastCache
