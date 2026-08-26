// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/SharedPortDatagram.hpp>
#include <FastCache/Net/UdpSocket.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace FastCache
{

namespace
{
    /// How long a receive spends looking at the half it is not waiting on.
    ///
    /// Long enough to be a real poll on every platform and short enough that a
    /// backlog on either half drains at the rate the caller asks. It is also the
    /// floor on the wait itself, because `SO_RCVTIMEO` of zero means block
    /// forever.
    constexpr std::chrono::milliseconds Glance { 1 };

    /// A shared listening socket and a private answering one, as one socket.
    class SharedPortDatagramSocket final: public IDatagramSocket
    {
      public:
        /// @param shared Where broadcasts are heard.
        /// @param own Where this node is answered.
        SharedPortDatagramSocket(std::unique_ptr<IDatagramSocket> shared, std::unique_ptr<IDatagramSocket> own):
            _shared { std::move(shared) },
            _own { std::move(own) }
        {
        }

        /// Send from the address only this node holds -- **always**, and that is
        /// the whole rule this class exists to state.
        ///
        /// A datagram sent from the shared socket carries the shared address as
        /// its sender, so the reply it provokes is aimed at a port a co-hosted
        /// node may hold too, and the kernel decides which of them gets it.
        /// @param payload The whole message.
        /// @param to Destination; a broadcast address is fine, since it is this
        ///        socket that carries the broadcast capability.
        /// @return Nothing, or why the local stack refused it.
        std::expected<void, NetError> Send(std::span<std::byte const> payload, DatagramAddress const& to) override
        {
            return _own->Send(payload, to);
        }

        /// Wait for one datagram, on either socket.
        ///
        /// **A glance at one and the wait on the other**, rather than a wait on
        /// both at once -- `IDatagramSocket` offers no way to wait on two, and
        /// giving it one would mean a `select` in the platform socket and a
        /// second answer in the in-memory one: two implementations of one idea,
        /// for a loop that handles a handful of datagrams a minute.
        ///
        /// **Not half the timeout each**, which is the shape this started as and
        /// which quietly caps throughput. Splitting it means that whenever the
        /// half polled first is idle, every datagram queued on the other costs a
        /// full half-timeout to collect -- eight a second against `DiscoveryTier`'s
        /// 250ms poll. A segment large enough to exceed that backs up for good,
        /// and what the kernel then drops includes the proofs. Glancing costs a
        /// millisecond instead, so a backlog on either half drains as fast as the
        /// caller can ask.
        ///
        /// Which half is glanced at alternates, so each gets the real wait every
        /// other call. That is also what stops a flood of broadcasts -- which
        /// anything on the segment can send, a beacon being unauthenticated --
        /// from starving the answers this node is waiting for.
        ///
        /// Every wait is at least a millisecond, which is not a rounding nicety:
        /// `SO_RCVTIMEO` of zero means *block forever*, so a caller's `0ms` or
        /// `1ms` must not become a parked loop. Such a call costs 2ms.
        /// @param timeout How long to wait, across both.
        /// @return The datagram, or why none was returned.
        std::expected<ReceivedDatagram, DatagramWait> Receive(std::chrono::milliseconds timeout) override
        {
            _ownFirst = !_ownFirst;
            auto* const glanced = _ownFirst ? _own.get() : _shared.get();
            auto* const waited = _ownFirst ? _shared.get() : _own.get();

            auto const order = std::array {
                std::pair { glanced, Glance },
                std::pair { waited, std::max(Glance, timeout - Glance) },
            };

            for (auto const& [socket, slice]: order)
            {
                auto received = socket->Receive(slice);
                if (received.has_value())
                    return received;

                // Closed ends the wait rather than falling through to the other
                // socket: they are closed together, and a loop told to stop
                // should not spend the rest of its timeout finding out again.
                if (received.error() == DatagramWait::Closed)
                    return std::unexpected { DatagramWait::Closed };
            }

            return std::unexpected { DatagramWait::TimedOut };
        }

        void Close() noexcept override
        {
            _shared->Close();
            _own->Close();
        }

        /// Where a peer must send to reach this node.
        ///
        /// The private address, which is what `IDatagramSocket` documents this
        /// as: the shared one names a port, not a node, and telling a peer to
        /// answer there is the defect this class exists to remove.
        /// @return The address only this node holds.
        [[nodiscard]] DatagramAddress BoundAddress() const override
        {
            return _own->BoundAddress();
        }

      private:
        std::unique_ptr<IDatagramSocket> _shared;
        std::unique_ptr<IDatagramSocket> _own;

        /// Which socket the next receive polls first. Touched only from the
        /// thread that calls `Receive`, which is the one loop that drives this.
        bool _ownFirst { false };
    };
} // namespace

std::unique_ptr<IDatagramSocket> AnswerFromOwnAddress(std::unique_ptr<IDatagramSocket> shared,
                                                      std::unique_ptr<IDatagramSocket> own)
{
    // A missing half comes out as a missing whole, which is the shape
    // `OpenUdpSocket` -- the thing that produces these -- already reports a bind
    // failure in. Wrapping one anyway would move the failure to the first
    // datagram, on the discovery thread, as a null dereference.
    if (shared == nullptr || own == nullptr)
        return nullptr;

    return std::make_unique<SharedPortDatagramSocket>(std::move(shared), std::move(own));
}

std::unique_ptr<IDatagramSocket> OpenSharedPortUdpSocket(std::string_view bindAddress,
                                                         std::uint16_t sharedPort,
                                                         std::uint16_t ownPort)
{
    // The listener sends nothing, so it needs no broadcast capability; the
    // private socket sends everything, so it carries it. That reads backwards
    // until it is taken literally -- hearing a broadcast needs no permission,
    // sending one does -- which is exactly why it is spelled once, here.
    return AnswerFromOwnAddress(OpenUdpSocket(bindAddress, sharedPort, BroadcastMode::Off, PortSharing::Shared),
                                OpenUdpSocket(bindAddress, ownPort, BroadcastMode::On, PortSharing::Exclusive));
}

} // namespace FastCache
