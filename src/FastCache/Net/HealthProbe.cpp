// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/BlockingSocket.hpp> // Detail::EnsureNetworkInitialised / CloseNativeSocket
#include <FastCache/Net/HealthProbe.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <netdb.h>
    #include <unistd.h>
#endif

namespace FastCache
{

namespace
{

#if defined(_WIN32)
    using ProbeSocket = SOCKET;
    using SockLen = int; // Winsock connect() takes an int address length
    constexpr ProbeSocket InvalidProbeSocket = INVALID_SOCKET;
    [[nodiscard]] int SendLen(std::size_t n) noexcept
    {
        return static_cast<int>(n);
    }
#else
    using ProbeSocket = int;
    using SockLen = socklen_t; // POSIX connect() takes a socklen_t
    constexpr ProbeSocket InvalidProbeSocket = -1;
    [[nodiscard]] std::size_t SendLen(std::size_t n) noexcept
    {
        return n;
    }
#endif

    /// Close a raw probe socket on either platform.
    void CloseProbe(ProbeSocket socket) noexcept
    {
#if defined(_WIN32)
        ::closesocket(socket);
#else
        ::close(socket);
#endif
    }

} // namespace

/// Send/receive timeout for the probe socket. Generous for a loopback health
/// check, but bounded so a non-responsive peer cannot hang the probe forever.
constexpr std::chrono::milliseconds ProbeTimeout { 3000 };

bool HttpHealthProbe(std::string_view host, std::uint16_t port, std::string_view path) noexcept
{
    Detail::EnsureNetworkInitialised();

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string const hostStr { host };
    std::string const portStr = std::to_string(port);

    addrinfo* results = nullptr;
    if (::getaddrinfo(hostStr.c_str(), portStr.c_str(), &hints, &results) != 0 || results == nullptr)
        return false;

    auto socket = InvalidProbeSocket;
    for (addrinfo const* it = results; it != nullptr; it = it->ai_next)
    {
        socket = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (socket == InvalidProbeSocket)
            continue;
        if (::connect(socket, it->ai_addr, static_cast<SockLen>(it->ai_addrlen)) == 0)
            break;
        CloseProbe(socket);
        socket = InvalidProbeSocket;
    }
    ::freeaddrinfo(results);
    if (socket == InvalidProbeSocket)
        return false;

    // Bound the blocking send/recv so a peer that accepts the connection but
    // never replies (a wedged admin loop, or a foreign process squatting the
    // port) cannot hang the probe forever — important for `--healthcheck` run
    // standalone, where there is no external timeout. Reuses the same
    // SO_RCVTIMEO/SO_SNDTIMEO helper as the admin listener.
    Detail::SetIoTimeouts(static_cast<Detail::NativeSocket>(socket), ProbeTimeout, ProbeTimeout);

    auto const request =
        std::string { "GET " } + std::string { path } + " HTTP/1.0\r\nHost: " + hostStr + "\r\nConnection: close\r\n\r\n";
    bool ok = false;
    if (::send(socket, request.data(), SendLen(request.size()), 0) >= 0)
    {
        std::array<char, 256> buffer {};
        auto const received = ::recv(socket, buffer.data(), SendLen(buffer.size() - 1), 0);
        if (received > 0)
        {
            std::string_view const response { buffer.data(), static_cast<std::size_t>(received) };
            // Parse the HTTP status line strictly: "HTTP/1.x <code> <reason>".
            // A naive substring search for " 200 " would match a body that
            // happens to contain those bytes — e.g. a 5xx error page that
            // mentions "expected 200 OK" — and misreport an unhealthy peer as
            // healthy.
            if (response.starts_with("HTTP/1."))
            {
                auto const firstSpace = response.find(' ');
                if (firstSpace != std::string_view::npos && firstSpace + 4 <= response.size())
                {
                    auto const code = response.substr(firstSpace + 1, 3);
                    auto const after = response[firstSpace + 4];
                    ok = code == "200" && (after == ' ' || after == '\r' || after == '\n');
                }
            }
        }
    }
    CloseProbe(socket);
    return ok;
}

} // namespace FastCache
