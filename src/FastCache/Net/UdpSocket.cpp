// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/SocketAddress.hpp>
#include <FastCache/Net/UdpSocket.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <utility>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <sys/time.h>

    #include <cerrno>

    #include <netdb.h>
    #include <unistd.h>

    #include <arpa/inet.h>
#endif

namespace FastCache
{

namespace
{
    /// The largest datagram this socket will receive.
    ///
    /// Comfortably above anything discovery sends and below the 64 KiB a UDP
    /// datagram can be, because a receive buffer is allocated per call and an
    /// unbounded one would let anything on the segment dictate this process's
    /// memory use. A datagram larger than this is truncated by the kernel and
    /// then fails to decode, which is the right outcome: it is not ours.
    constexpr std::size_t MaxDatagram = 8192;

    /// Read a sockaddr back as a host and a port.
    ///
    /// The two halves stay apart all the way out, so nothing here joins them --
    /// see `DatagramAddress`.
    ///
    /// The port comes from `Detail::PortOfSockaddr`, which is where this codebase
    /// already keeps the family switch, rather than from `getnameinfo`: the
    /// sockaddr holds it as a 16-bit field, so asking for it as decimal text and
    /// parsing it back would be a round trip through a string with a range check
    /// that cannot fail.
    ///
    /// The host still comes from `getnameinfo` rather than the `inet_ntop` behind
    /// `FormatPeerAddress`, and that is deliberate: `getnameinfo` appends the
    /// `%scope` suffix for a link-local IPv6 address and `inet_ntop` does not.
    /// This address is handed straight back to `Send`, so an address that lost its
    /// zone would resolve to something unroutable -- which is the one thing a
    /// reply-to-the-sender must not do.
    /// @param storage The address.
    /// @param length Its length.
    /// @return The address, with an empty host when it cannot be rendered.
    [[nodiscard]] DatagramAddress DatagramAddressOf(sockaddr_storage const& storage, socklen_t length)
    {
        std::array<char, NI_MAXHOST> host {};

        if (::getnameinfo(reinterpret_cast<sockaddr const*>(&storage),
                          length,
                          host.data(),
                          static_cast<unsigned>(host.size()),
                          nullptr,
                          0,
                          NI_NUMERICHOST)
            != 0)
            return {};

        return DatagramAddress { .host = std::string { host.data() },
                                 .port = Detail::PortOfSockaddr(&storage, static_cast<std::uint32_t>(length)) };
    }

    /// A blocking UDP socket.
    class UdpSocket final: public IDatagramSocket
    {
      public:
        /// Take ownership of an already-bound socket.
        /// @param socket The native handle.
        /// @param bound What it bound.
        UdpSocket(Detail::NativeSocket socket, DatagramAddress bound) noexcept:
            _socket { socket },
            _bound { std::move(bound) }
        {
        }

        ~UdpSocket() override
        {
            if (_socket != Detail::InvalidSocket)
                Detail::CloseNativeSocket(_socket);
        }

        UdpSocket(UdpSocket const&) = delete;
        UdpSocket(UdpSocket&&) = delete;
        UdpSocket& operator=(UdpSocket const&) = delete;
        UdpSocket& operator=(UdpSocket&&) = delete;

        std::expected<void, NetError> Send(std::span<std::byte const> payload, DatagramAddress const& to) override
        {
            // An empty host names nothing, and the two platforms disagree about
            // that rather than both refusing it: `getaddrinfo("", ...)` is
            // `EAI_NONAME` on glibc and **succeeds** on Winsock, resolving to the
            // local host. So a reply addressed to a sender this process could not
            // render would be refused here and quietly sent to loopback there.
            // Refused explicitly, which is also what splitting `host:port` used to
            // do for the same input on every platform.
            if (to.host.empty())
                return std::unexpected { NetError { .code = NetErrorCode::AddressNotAvail,
                                                    .context = "no host to send to" } };

            addrinfo hints {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            // The port arrives as a number now rather than as whatever text a
            // caller wrote, so saying so skips the /etc/services lookup this
            // would otherwise have to rule out first.
            hints.ai_flags = AI_NUMERICSERV;

            auto const service = std::to_string(to.port);
            addrinfo* resolved = nullptr;
            if (::getaddrinfo(to.host.c_str(), service.c_str(), &hints, &resolved) != 0 || resolved == nullptr)
                return std::unexpected { NetError { .code = NetErrorCode::AddressNotAvail,
                                                    .context =
                                                        std::format("cannot resolve {} port {}", to.host, to.port) } };

            auto const sent = ::sendto(_socket,
                                       reinterpret_cast<char const*>(payload.data()),
#if defined(_WIN32)
                                       static_cast<int>(payload.size()),
#else
                                       payload.size(),
#endif
                                       0,
                                       resolved->ai_addr,
                                       resolved->ai_addrlen);
            ::freeaddrinfo(resolved);

            if (sent < 0)
                return std::unexpected { Detail::MakeNetError(Detail::LastNetworkError(),
                                                              std::format("sendto {} port {}", to.host, to.port)) };

            // A short send on a datagram socket is not a partial write to retry:
            // the kernel places a datagram whole or not at all, so anything else
            // means the message was too large for the path.
            if (static_cast<std::size_t>(sent) != payload.size())
                return std::unexpected { NetError {
                    .code = NetErrorCode::SystemError,
                    .context = std::format("datagram truncated at {} of {} bytes", sent, payload.size()) } };
            return {};
        }

        std::expected<ReceivedDatagram, DatagramWait> Receive(std::chrono::milliseconds timeout) override
        {
            if (_closed.load(std::memory_order_acquire))
                return std::unexpected { DatagramWait::Closed };

            ApplyReceiveTimeout(timeout);

            std::vector<std::byte> buffer(MaxDatagram);
            sockaddr_storage from {};
            socklen_t fromLength = sizeof(from);

            auto const received = ::recvfrom(_socket,
                                             reinterpret_cast<char*>(buffer.data()),
#if defined(_WIN32)
                                             static_cast<int>(buffer.size()),
#else
                                             buffer.size(),
#endif
                                             0,
                                             reinterpret_cast<sockaddr*>(&from),
                                             &fromLength);

            // Checked AFTER the receive as well as before: Close() may have been
            // called while this call was parked, and the timeout is what let it
            // return at all.
            if (_closed.load(std::memory_order_acquire))
                return std::unexpected { DatagramWait::Closed };

            if (received < 0)
                return std::unexpected { DatagramWait::TimedOut };

            buffer.resize(static_cast<std::size_t>(received));
            return ReceivedDatagram { .payload = std::move(buffer), .from = DatagramAddressOf(from, fromLength) };
        }

        void Close() noexcept override
        {
            _closed.store(true, std::memory_order_release);
        }

        [[nodiscard]] DatagramAddress BoundAddress() const override
        {
            return _bound;
        }

      private:
        /// Set SO_RCVTIMEO, so a parked receive returns and the loop can stop.
        /// @param timeout How long a receive may park.
        void ApplyReceiveTimeout(std::chrono::milliseconds timeout) const noexcept
        {
#if defined(_WIN32)
            auto const millis = static_cast<DWORD>(timeout.count());
            ::setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char const*>(&millis), sizeof(millis));
#else
            timeval value {};
            value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count() / 1000);
            value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout.count() % 1000) * 1000);
            ::setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
#endif
        }

        Detail::NativeSocket _socket;
        DatagramAddress _bound;
        std::atomic<bool> _closed { false };
    };
} // namespace

std::unique_ptr<IDatagramSocket> OpenUdpSocket(std::string_view bindAddress,
                                               std::uint16_t port,
                                               BroadcastMode broadcast,
                                               PortSharing sharing)
{
    // Winsock refuses every call until WSAStartup has run, and there is no
    // diagnostic to distinguish "the stack is not up" from "this address will
    // not bind" -- both come back as a null socket. The TCP side already has
    // this; a second socket family reaching the network without it is how a
    // Windows-only null return with no message happens.
    Detail::EnsureNetworkInitialised();

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    // Numeric for the same reason `Send`'s is: the service is `std::to_string` of
    // a `std::uint16_t` and can never be a name.
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    auto const portText = std::to_string(port);
    addrinfo* resolved = nullptr;
    if (::getaddrinfo(std::string { bindAddress }.c_str(), portText.c_str(), &hints, &resolved) != 0)
        return nullptr;

    for (auto const* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next)
    {
        auto const handle = static_cast<Detail::NativeSocket>(
            ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol));
        if (handle == Detail::InvalidSocket)
            continue;

        if (sharing == PortSharing::Shared)
        {
            int const reuse = 1;
            ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&reuse), sizeof(reuse));

            // One intent, two spellings, because SO_REUSEADDR does not mean the
            // same thing everywhere. On Linux and on Windows it is what lets a
            // second socket bind an address a first one holds. On BSD and Darwin
            // it permits that only for a MULTICAST address; a second bind of the
            // same unicast address needs SO_REUSEPORT, and without it the second
            // node on a macOS host cannot bind the beacon port at all.
            //
            // Set wherever the option exists rather than behind a platform test,
            // and that is measured rather than assumed: on Ubuntu 24.04, all four
            // combinations of the two options across two sockets bind
            // successfully, so a node carrying this still shares a port with an
            // older node that does not. Undefined on Windows, where SO_REUSEADDR
            // already says it.
#if defined(SO_REUSEPORT)
            ::setsockopt(handle, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<char const*>(&reuse), sizeof(reuse));
#endif
        }
        // Exclusive is the default on POSIX and has to be asked for on Windows,
        // which is the same asymmetry `Detail::BindAndListen` records for the
        // stream side: there, SO_REUSEADDR on a *later* bind takes an address a
        // live socket already holds, and SO_EXCLUSIVEADDRUSE is the documented
        // way to refuse that.
        //
        // It is not hypothetical here just because most of these sockets ask the
        // kernel to choose a port. `--discovery-reply-port` binds a NAMED one
        // through this branch, and a socket answering discovery is precisely the
        // one whose datagrams must not be handed to a second process: that is
        // issue #126 again, arrived at from the other side.
        //
        // So it fails the candidate rather than being ignored the way
        // TCP_NODELAY is. A `setsockopt` carrying a security property is not
        // best-effort -- the rule this repository already paid for once.
#if defined(_WIN32)
        if (sharing == PortSharing::Exclusive)
        {
            int const exclusive = 1;
            if (::setsockopt(
                    handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char const*>(&exclusive), sizeof(exclusive))
                != 0)
            {
                Detail::CloseNativeSocket(handle);
                continue;
            }
        }
#endif

        if (broadcast == BroadcastMode::On)
        {
            int const enable = 1;
            ::setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char const*>(&enable), sizeof(enable));
        }

        if (::bind(handle, candidate->ai_addr, static_cast<socklen_t>(candidate->ai_addrlen)) == 0)
        {
            // Read back what was actually bound rather than echoing what was
            // asked for -- see IDatagramSocket::BoundAddress. Through the same
            // decoder the receive path uses, so the port comes from the one
            // family switch `Detail::BoundPortOf` also delegates to; calling that
            // instead would ask the kernel the same question a second time.
            sockaddr_storage actual {};
            socklen_t actualLength = sizeof(actual);
            auto bound = DatagramAddress {};
            if (::getsockname(handle, reinterpret_cast<sockaddr*>(&actual), &actualLength) == 0)
                bound = DatagramAddressOf(actual, actualLength);

            ::freeaddrinfo(resolved);
            return std::make_unique<UdpSocket>(handle, std::move(bound));
        }

        Detail::CloseNativeSocket(handle);
    }

    ::freeaddrinfo(resolved);
    return nullptr;
}

} // namespace FastCache
