// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
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

    /// Render a sockaddr as `host:port`.
    /// @param storage The address.
    /// @param length Its length.
    /// @return Text, empty when it cannot be rendered.
    [[nodiscard]] std::string FormatPeer(sockaddr_storage const& storage, socklen_t length)
    {
        std::array<char, NI_MAXHOST> host {};
        std::array<char, NI_MAXSERV> service {};

        if (::getnameinfo(reinterpret_cast<sockaddr const*>(&storage),
                          length,
                          host.data(),
                          static_cast<unsigned>(host.size()),
                          service.data(),
                          static_cast<unsigned>(service.size()),
                          NI_NUMERICHOST | NI_NUMERICSERV)
            != 0)
            return {};

        // Bracketed when the host itself contains a colon, so the result round
        // trips through `SplitHostPort` -- which is the whole reason that parser
        // is shared rather than an `rfind(':')` at each caller.
        std::string const text { host.data() };
        return text.contains(':') ? std::format("[{}]:{}", text, service.data())
                                  : std::format("{}:{}", text, service.data());
    }

    /// A blocking UDP socket.
    class UdpSocket final: public IDatagramSocket
    {
      public:
        /// Take ownership of an already-bound socket.
        /// @param socket The native handle.
        /// @param bound What it bound, as text.
        UdpSocket(Detail::NativeSocket socket, std::string bound) noexcept:
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

        std::expected<void, NetError> Send(std::span<std::byte const> payload, std::string_view to) override
        {
            auto const split = SplitHostPort(to);
            if (!split.has_value())
                return std::unexpected { NetError { .code = NetErrorCode::AddressNotAvail,
                                                    .context = std::format("not host:port: {}", to) } };

            addrinfo hints {};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;

            addrinfo* resolved = nullptr;
            if (::getaddrinfo(split->first.c_str(), split->second.c_str(), &hints, &resolved) != 0 || resolved == nullptr)
                return std::unexpected { NetError { .code = NetErrorCode::AddressNotAvail,
                                                    .context = std::format("cannot resolve {}", to) } };

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
                return std::unexpected { Detail::MakeNetError(Detail::LastNetworkError(), std::format("sendto {}", to)) };

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
            return ReceivedDatagram { .payload = std::move(buffer), .from = FormatPeer(from, fromLength) };
        }

        void Close() noexcept override
        {
            _closed.store(true, std::memory_order_release);
        }

        [[nodiscard]] std::string BoundEndpoint() const override
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
        std::string _bound;
        std::atomic<bool> _closed { false };
    };
} // namespace

std::unique_ptr<IDatagramSocket> OpenUdpSocket(std::string_view bindAddress, std::uint16_t port, BroadcastMode broadcast)
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
    hints.ai_flags = AI_PASSIVE;

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

        int const reuse = 1;
        ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&reuse), sizeof(reuse));

        if (broadcast == BroadcastMode::On)
        {
            int const enable = 1;
            ::setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char const*>(&enable), sizeof(enable));
        }

        if (::bind(handle, candidate->ai_addr, static_cast<socklen_t>(candidate->ai_addrlen)) == 0)
        {
            // Read back what was actually bound rather than echoing what was
            // asked for: port 0 means "the kernel chooses", and a caller that has
            // to tell a peer where to answer needs the answer.
            sockaddr_storage actual {};
            socklen_t actualLength = sizeof(actual);
            auto bound = std::string {};
            if (::getsockname(handle, reinterpret_cast<sockaddr*>(&actual), &actualLength) == 0)
                bound = FormatPeer(actual, actualLength);

            ::freeaddrinfo(resolved);
            return std::make_unique<UdpSocket>(handle, std::move(bound));
        }

        Detail::CloseNativeSocket(handle);
    }

    ::freeaddrinfo(resolved);
    return nullptr;
}

} // namespace FastCache
