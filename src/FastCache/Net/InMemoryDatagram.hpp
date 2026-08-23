// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IDatagramSocket.hpp>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
class DatagramBus
{
  public:
    /// The address every broadcast is sent to.
    ///
    /// A name rather than a real broadcast address: what matters to the layer
    /// above is "everyone on the segment sees this", and encoding a subnet here
    /// would make the double model a routing detail it has no opinion about.
    static constexpr std::string_view BroadcastAddress = "broadcast:0";

    /// Attach a socket at @p endpoint.
    /// @param endpoint The address other sockets reach it by.
    /// @return The socket; it detaches on destruction.
    [[nodiscard]] std::unique_ptr<IDatagramSocket> Open(std::string endpoint);

    /// Drop the next @p count datagrams sent to @p endpoint.
    ///
    /// Per destination, so a test can starve one peer while the rest of the
    /// segment carries on -- which is the partition shape discovery has to
    /// survive, and the one a global drop rate cannot express.
    /// @param endpoint Whose inbox to starve.
    /// @param count How many datagrams to discard.
    void DropNext(std::string_view endpoint, std::size_t count);

    /// How many datagrams have been placed on the bus, delivered or dropped.
    /// @return The count.
    [[nodiscard]] std::size_t SendCount() const;

  private:
    friend class InMemoryDatagramSocket;

    /// Place @p payload in every inbox @p to names.
    /// @param payload The datagram.
    /// @param to A specific endpoint, or BroadcastAddress.
    /// @param from Who sent it.
    void Deliver(std::span<std::byte const> payload, std::string_view to, std::string_view from);

    /// Register an inbox.
    /// @param endpoint Its address.
    void Attach(std::string const& endpoint);

    /// Remove an inbox.
    /// @param endpoint Its address.
    void Detach(std::string const& endpoint);

    /// One socket's queue.
    struct Inbox
    {
        std::deque<ReceivedDatagram> queue; ///< Datagrams awaiting a receive.
        std::size_t dropsRemaining { 0 };   ///< Scripted losses still to apply.
        bool closed { false };              ///< Whether its socket has been closed.
    };

    mutable std::mutex _mutex;
    std::condition_variable _arrived;
    std::unordered_map<std::string, Inbox> _inboxes;
    std::size_t _sendCount { 0 };
};

} // namespace FastCache
