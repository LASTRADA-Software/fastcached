// SPDX-License-Identifier: Apache-2.0
//
// The real TCP client: Winsock on Windows, BSD sockets elsewhere. The two
// differ only in the handle type, the close call, and Windows' one-time
// WSAStartup, so the socket loops themselves are written once.

#include "ITcpClient.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <sys/time.h>

    #include <netdb.h>
    #include <unistd.h>
#endif

namespace FastCache::Cc
{

namespace
{

#if defined(_WIN32)
    using NativeSocket = SOCKET;
    constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
    /// Winsock types its buffer and address lengths as int, POSIX as size_t /
    /// socklen_t; aliasing them keeps the socket loops themselves identical.
    using SendSize = int;
    using ConnectLen = int;

    /// Close a native socket handle.
    /// @param fd The socket to close.
    void CloseSocket(NativeSocket fd)
    {
        ::closesocket(fd);
    }

    /// Initialise Winsock once per process. The matching WSACleanup is deliberately
    /// omitted: the launcher is a short-lived process that exits right after, and
    /// an unbalanced cleanup from a destructor could tear the stack down while
    /// another connection is still open.
    /// @return True if Winsock is usable.
    [[nodiscard]] bool EnsureWinsock()
    {
        static bool const ready = [] {
            WSADATA wsa {};
            return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        }();
        return ready;
    }
#else
    using NativeSocket = int;
    constexpr NativeSocket InvalidSocket = -1;
    /// @see the Windows branch — POSIX sizes buffers with size_t and addresses
    /// with socklen_t.
    using SendSize = std::size_t;
    using ConnectLen = ::socklen_t;

    /// Close a native socket handle.
    /// @param fd The socket to close.
    void CloseSocket(NativeSocket fd)
    {
        ::close(fd);
    }

    /// No initialisation is needed for BSD sockets.
    /// @return Always true.
    [[nodiscard]] bool EnsureWinsock()
    {
        return true;
    }
#endif

    /// Bound how long a single blocking send/recv on `fd` may stall.
    ///
    /// Without this the launcher waits forever on a daemon that accepted the
    /// connection and then went quiet mid-reply, which turns an optional cache
    /// into a build-stopping dependency. A timed-out call returns -1, which the
    /// send/recv loops already treat as "give up", so the existing fall-back to
    /// a real compile does the rest.
    ///
    /// Deliberately duplicated from `FastCache::Detail::SetIoTimeouts` rather
    /// than shared: the launcher compiles only a handful of FastCache sources so
    /// it stays free of third-party dependencies and can link the CRT
    /// statically. `CloseSocket`/`EnsureWinsock` above are duplicated for the
    /// same reason.
    ///
    /// Best-effort: a setsockopt failure leaves the OS default in place.
    /// @param fd The connected socket to tune.
    /// @param timeout Per-call deadline; zero or negative leaves no timeout.
    void SetIoTimeouts(NativeSocket fd, std::chrono::milliseconds timeout) noexcept
    {
        if (timeout.count() <= 0)
            return;
        for (auto const option: { SO_RCVTIMEO, SO_SNDTIMEO })
        {
#if defined(_WIN32)
            // Windows SO_RCVTIMEO/SO_SNDTIMEO take a DWORD of milliseconds.
            auto const millis = static_cast<DWORD>(timeout.count());
            std::ignore = ::setsockopt(fd, SOL_SOCKET, option, reinterpret_cast<char const*>(&millis), sizeof(millis));
#else
            // POSIX SO_RCVTIMEO/SO_SNDTIMEO take a struct timeval.
            constexpr auto MillisPerSecond = 1000;
            timeval tv {};
            tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / MillisPerSecond);
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % MillisPerSecond) * MillisPerSecond);
            std::ignore = ::setsockopt(fd, SOL_SOCKET, option, &tv, sizeof(tv));
#endif
        }
    }

    /// Split "host:port" into its parts, honouring the bracketed IPv6 form
    /// `[::1]:6674` — an unbracketed rfind(':') would split an IPv6 literal at
    /// its last group separator instead.
    /// @param hostPort The endpoint string.
    /// @return (host, port) or nullopt when no port is present.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> SplitHostPort(std::string_view hostPort)
    {
        if (!hostPort.empty() && hostPort.front() == '[')
        {
            auto const close = hostPort.find(']');
            if (close == std::string_view::npos || close + 1 >= hostPort.size() || hostPort[close + 1] != ':')
                return std::nullopt;
            return std::pair { std::string { hostPort.substr(1, close - 1) }, std::string { hostPort.substr(close + 2) } };
        }

        auto const colon = hostPort.rfind(':');
        if (colon == std::string_view::npos || colon + 1 >= hostPort.size())
            return std::nullopt;
        return std::pair { std::string { hostPort.substr(0, colon) }, std::string { hostPort.substr(colon + 1) } };
    }

    /// A connected socket, closed on destruction.
    class TcpClient final: public ITcpClient
    {
      public:
        explicit TcpClient(NativeSocket fd) noexcept:
            _fd { fd }
        {
        }

        ~TcpClient() override
        {
            if (_fd != InvalidSocket)
                CloseSocket(_fd);
        }

        TcpClient(TcpClient const&) = delete;
        TcpClient& operator=(TcpClient const&) = delete;
        TcpClient(TcpClient&&) = delete;
        TcpClient& operator=(TcpClient&&) = delete;

        [[nodiscard]] bool SendAll(std::span<std::byte const> bytes) override
        {
            std::size_t sent = 0;
            while (sent < bytes.size())
            {
                auto const remaining = bytes.subspan(sent);
                auto const n =
                    ::send(_fd, reinterpret_cast<char const*>(remaining.data()), static_cast<SendSize>(remaining.size()), 0);
                if (n <= 0)
                    return false;
                sent += static_cast<std::size_t>(n);
            }
            return true;
        }

        [[nodiscard]] std::optional<std::vector<std::byte>> RecvExactly(std::size_t count) override
        {
            std::vector<std::byte> out(count);
            std::size_t got = 0;
            while (got < count)
            {
                auto const remaining = std::span { out }.subspan(got);
                auto const n =
                    ::recv(_fd, reinterpret_cast<char*>(remaining.data()), static_cast<SendSize>(remaining.size()), 0);
                if (n <= 0)
                    return std::nullopt;
                got += static_cast<std::size_t>(n);
            }
            return out;
        }

      private:
        NativeSocket _fd { InvalidSocket };
    };

} // namespace

std::unique_ptr<ITcpClient> ConnectTcp(std::string_view hostPort, std::chrono::milliseconds ioTimeout)
{
    if (!EnsureWinsock())
        return nullptr;

    auto const parts = SplitHostPort(hostPort);
    if (!parts.has_value())
        return nullptr;
    auto const& [host, port] = *parts;

    // AF_UNSPEC so a host that resolves to both families works either way; the
    // daemon may be listening on IPv4, IPv6, or a dual-stack socket.
    ::addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ::addrinfo* resolved = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &resolved) != 0 || resolved == nullptr)
        return nullptr;

    for (::addrinfo const* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next)
    {
        NativeSocket const fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd == InvalidSocket)
            continue;
        if (::connect(fd, candidate->ai_addr, static_cast<ConnectLen>(candidate->ai_addrlen)) == 0)
        {
            // Arm the deadline before the first send: from here on every
            // blocking call on this socket is bounded.
            SetIoTimeouts(fd, ioTimeout);
            ::freeaddrinfo(resolved);
            return std::make_unique<TcpClient>(fd);
        }
        CloseSocket(fd);
    }

    ::freeaddrinfo(resolved);
    return nullptr;
}

} // namespace FastCache::Cc
