// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/SocketAddress.hpp>

#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <cerrno>

    #include <netdb.h>
    #include <unistd.h>

    #include <netinet/in.h>
    #include <netinet/tcp.h>
#endif

namespace FastCache
{

namespace
{
    // sockaddr_storage is the largest sockaddr; ResolvedEndpoint::storage must
    // hold one with correct alignment. Verified once, here, so the header can
    // stay free of platform socket headers.
    static_assert(sizeof(sockaddr_storage) <= ResolvedEndpoint::StorageSize,
                  "ResolvedEndpoint::StorageSize too small for sockaddr_storage");
    static_assert(alignof(sockaddr_storage) <= alignof(std::max_align_t),
                  "ResolvedEndpoint alignment insufficient for sockaddr_storage");

#if defined(_WIN32)
    /// Type of the address-length argument to ::bind on this platform.
    using AddrLen = int;

    /// @return The last socket-layer error code for diagnostics.
    [[nodiscard]] int LastSocketError() noexcept
    {
        return WSAGetLastError();
    }

    /// Close a raw socket handle, swallowing the result.
    void CloseSocket(Detail::NativeSocket s) noexcept
    {
        std::ignore = ::closesocket(static_cast<SOCKET>(s));
    }

    /// @return A human-readable string for a getaddrinfo error code.
    [[nodiscard]] std::string GaiMessage(int code)
    {
        return std::string { gai_strerrorA(code) };
    }
#else
    using AddrLen = socklen_t;

    [[nodiscard]] int LastSocketError() noexcept
    {
        return errno;
    }

    void CloseSocket(Detail::NativeSocket s) noexcept
    {
        std::ignore = ::close(static_cast<int>(s));
    }

    [[nodiscard]] std::string GaiMessage(int code)
    {
        return std::string { ::gai_strerror(code) };
    }
#endif
} // namespace

namespace Detail
{

    void ApplyHotSocketOptions(NativeSocket socket) noexcept
    {
        // TCP_NODELAY disables Nagle's algorithm so a small reply isn't held
        // back waiting for the peer's ACK of a previous segment. Best-effort.
        int const one = 1;
#if defined(_WIN32)
        ::setsockopt(
            static_cast<SOCKET>(socket), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&one), sizeof(one));
#else
        ::setsockopt(static_cast<int>(socket), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
    }

} // namespace Detail

std::expected<std::vector<ResolvedEndpoint>, std::string> SystemAddressResolver::Resolve(std::string_view host,
                                                                                         std::uint16_t port)
{
    Detail::EnsureNetworkInitialised();

    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;     // IPv4 and IPv6 both acceptable.
    hints.ai_socktype = SOCK_STREAM; // TCP listener.
    // AI_PASSIVE: a wildcard host ("0.0.0.0"/"::"/empty) yields a bind-able
    // any-address. AI_NUMERICSERV: the service is always our numeric port.
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    auto const service = std::to_string(port);
    std::string const hostCopy { host };
    // An empty host must be passed as nullptr so AI_PASSIVE picks the wildcard.
    char const* const node = hostCopy.empty() ? nullptr : hostCopy.c_str();

    addrinfo* head = nullptr;
    auto const rc = ::getaddrinfo(node, service.c_str(), &hints, &head);
    if (rc != 0)
        return std::unexpected(std::format("cannot resolve '{}': {}", host, GaiMessage(rc)));

    std::vector<ResolvedEndpoint> endpoints;
    for (addrinfo const* ai = head; ai != nullptr; ai = ai->ai_next)
    {
        if (ai->ai_addrlen == 0 || ai->ai_addrlen > ResolvedEndpoint::StorageSize)
            continue;
        ResolvedEndpoint endpoint;
        std::memcpy(endpoint.storage.data(), ai->ai_addr, ai->ai_addrlen);
        endpoint.length = static_cast<std::uint32_t>(ai->ai_addrlen);
        endpoint.family = ai->ai_family;
        endpoint.protocol = ai->ai_protocol;
        endpoints.push_back(endpoint);
    }
    ::freeaddrinfo(head);

    if (endpoints.empty())
        return std::unexpected(std::format("cannot resolve '{}': no usable address", host));
    return endpoints;
}

IAddressResolver& DefaultAddressResolver() noexcept
{
    static SystemAddressResolver resolver;
    return resolver;
}

namespace Detail
{

    std::expected<BoundListener, std::string> BindAndListen(IAddressResolver& resolver,
                                                            std::string_view host,
                                                            std::uint16_t port,
                                                            int backlog,
                                                            int extraTypeFlags,
                                                            ReusePort reusePort)
    {
        EnsureNetworkInitialised();

        auto resolved = resolver.Resolve(host, port);
        if (!resolved.has_value())
            return std::unexpected(std::move(resolved).error());

        std::string lastError;
        for (auto const& endpoint: *resolved)
        {
            auto const sock = ::socket(endpoint.family, SOCK_STREAM | extraTypeFlags, endpoint.protocol);
            // `InvalidSocket` is the platform sentinel; a direct compare avoids the
            // signed/unsigned mismatch a `< 0` check against an unsigned SOCKET hits.
            if (sock == static_cast<std::remove_const_t<decltype(sock)>>(InvalidSocket))
            {
                lastError = std::format("socket() failed: {}", LastSocketError());
                continue;
            }

            // SO_REUSEADDR so restart-after-crash rebinds without TIME_WAIT delay.
            int reuse = 1;
            ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char const*>(&reuse), sizeof(reuse));

            // SO_REUSEPORT lets N reactor threads each bind a listener on the
            // same port; the kernel then load-balances new connections across
            // them. POSIX only; absent on Windows.
#if defined(SO_REUSEPORT)
            if (reusePort == ReusePort::Yes)
            {
                int reusePortValue = 1;
                ::setsockopt(
                    sock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<char const*>(&reusePortValue), sizeof(reusePortValue));
            }
#else
            static_cast<void>(reusePort);
#endif

            // Force dual-stack on IPv6 sockets so a "::" wildcard accepts IPv4
            // clients too. The OS default is v6-only on Windows and Linux, which
            // makes "--bind=::" silently unreachable over IPv4 — surprising for a
            // cache addressed as 127.0.0.1. Best-effort: a failure leaves the OS
            // default and bind still proceeds. No effect on AF_INET sockets or on
            // a specific IPv6 literal such as "::1".
            if (endpoint.family == AF_INET6)
            {
                int v6only = 0;
                ::setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char const*>(&v6only), sizeof(v6only));
            }

            if (::bind(
                    sock, reinterpret_cast<sockaddr const*>(endpoint.storage.data()), static_cast<AddrLen>(endpoint.length))
                != 0)
            {
                lastError = std::format("bind({}:{}) failed: {}", host, port, LastSocketError());
                CloseSocket(static_cast<NativeSocket>(sock));
                continue;
            }

            if (::listen(sock, backlog) != 0)
            {
                lastError = std::format("listen({}:{}) failed: {}", host, port, LastSocketError());
                CloseSocket(static_cast<NativeSocket>(sock));
                continue;
            }

            return BoundListener { .socket = static_cast<NativeSocket>(sock), .family = endpoint.family };
        }

        return std::unexpected(lastError.empty() ? std::format("no usable address for '{}:{}'", host, port) : lastError);
    }

} // namespace Detail

} // namespace FastCache
