// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/SocketAddress.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>

    // SIO_KEEPALIVE_VALS and `struct tcp_keepalive`. Winsock's keepalive knobs are
    // an ioctl rather than socket options; there is no TCP_KEEPIDLE here.
    #include <mstcpip.h>
#else
    #include <sys/socket.h>

    #include <cerrno>

    #include <fcntl.h>
    #include <netdb.h>
    #include <unistd.h>

    #include <arpa/inet.h>
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

    /// The socket option that keeps a listening address to the process that
    /// bound it. One intent, two spellings -- and each platform's *other*
    /// spelling means something actively wrong here:
    ///
    ///   POSIX    SO_REUSEADDR only lets a bind step over a TIME_WAIT left by a
    ///            DEAD socket; a live listener still holds the address alone.
    ///   Windows  SO_REUSEADDR lets a second socket bind an address a LIVE
    ///            socket already holds -- the documented reason
    ///            SO_EXCLUSIVEADDRUSE exists.
    ///
    /// Measured on Windows 11: with this option on both sockets the second bind
    /// is refused with WSAEADDRINUSE, and a fresh process still rebinds a
    /// listening port whose crashed predecessor's accepted connections are in
    /// TIME_WAIT -- so the restart the old SO_REUSEADDR comment reached for is
    /// not given up in exchange. Sharing a port on purpose stays a separate,
    /// opt-in question: SO_REUSEPORT below.
    ///
    /// Issue #85 and .agent/rules/wire-and-protocol.md carry what it cost.
#if defined(_WIN32)
    constexpr int ExclusiveBindOption = SO_EXCLUSIVEADDRUSE;
#else
    constexpr int ExclusiveBindOption = SO_REUSEADDR;
#endif
} // namespace

namespace Detail
{

    void ArmCloseOnExec(NativeSocket socket) noexcept
    {
#if defined(_WIN32)
        // Windows DOES have a per-handle inheritance flag, and a socket IS
        // inheritable unless something says otherwise -- both halves of what this
        // used to claim were wrong, which is why the leak below survived so long.
        // `::socket()` and `::accept()` hand back inheritable handles, so every
        // accepted client connection was being handed to every compiler this
        // process spawned. `IocpConnector` had it right for the dialled side all
        // along, with `WSA_FLAG_NO_HANDLE_INHERIT` and a comment saying why.
        std::ignore = ::SetHandleInformation(reinterpret_cast<HANDLE>(static_cast<SOCKET>(socket)), HANDLE_FLAG_INHERIT, 0);
#else
        auto const flags = ::fcntl(static_cast<int>(socket), F_GETFD, 0);
        if (flags < 0)
            return;
        std::ignore = ::fcntl(static_cast<int>(socket), F_SETFD, flags | FD_CLOEXEC);
#endif
    }

    void ApplyHotSocketOptions(NativeSocket socket) noexcept
    {
        // Every socket this process ends up owning passes through here -- accepted
        // and dialled, on all four backends -- which is what makes it the one place
        // this can be armed. `BlockingListener::Accept` uses a plain `::accept()`
        // on both platforms and got it from nowhere else, so `fastcache-compile-node`
        // handed each compiler it spawned every client connection then in flight.
        // Invisible while a worker served one at a time; `slots` of them at once
        // once it did not.
        ArmCloseOnExec(socket);

        // TCP_NODELAY disables Nagle's algorithm so a small reply isn't held
        // back waiting for the peer's ACK of a previous segment. Best-effort.
        int const one = 1;
        // Enlarge the socket send/receive buffers. A large value reply (e.g.
        // 64 KiB) overflows the default ~16-200 KiB send buffer, so the value
        // drains over several sendmsg calls with an EPOLLOUT re-arm between
        // each — extra syscalls that cap large-value throughput. A 1 MiB buffer
        // lets a typical large reply land in a single sendmsg. The kernel
        // clamps to its own max (net.core.wmem_max), so this is an upper hint.
        int const socketBufferBytes = 1 << 20; // 1 MiB
#if defined(_WIN32)
        ::setsockopt(
            static_cast<SOCKET>(socket), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char const*>(&one), sizeof(one));
        ::setsockopt(static_cast<SOCKET>(socket),
                     SOL_SOCKET,
                     SO_SNDBUF,
                     reinterpret_cast<char const*>(&socketBufferBytes),
                     sizeof(socketBufferBytes));
        ::setsockopt(static_cast<SOCKET>(socket),
                     SOL_SOCKET,
                     SO_RCVBUF,
                     reinterpret_cast<char const*>(&socketBufferBytes),
                     sizeof(socketBufferBytes));
#else
        ::setsockopt(static_cast<int>(socket), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_SNDBUF, &socketBufferBytes, sizeof(socketBufferBytes));
        ::setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_RCVBUF, &socketBufferBytes, sizeof(socketBufferBytes));
#endif
    }

    bool ArmKeepAlive(NativeSocket socket, KeepAliveSettings const& settings) noexcept
    {
#if defined(_WIN32)
        // One ioctl sets the flag and both intervals together, so there is no
        // partially-armed state to unwind. The probe COUNT is absent on purpose:
        // Windows fixes it at 10 and offers no way to set it -- see
        // `KeepAliveSettings`, which states what that does to the detection time
        // rather than pretending the parameter was applied.
        std::ignore = settings.count;

        // Milliseconds here, unlike every other platform in this function.
        tcp_keepalive request {};
        request.onoff = 1;
        request.keepalivetime = static_cast<ULONG>(settings.idle.count());
        request.keepaliveinterval = static_cast<ULONG>(settings.interval.count());

        DWORD returned = 0;
        return ::WSAIoctl(static_cast<SOCKET>(socket),
                          SIO_KEEPALIVE_VALS,
                          &request,
                          sizeof(request),
                          nullptr,
                          0,
                          &returned,
                          nullptr,
                          nullptr)
               == 0;
#else
        // Whole seconds, and never zero. These options take seconds, and a zero is
        // not "immediately" -- it is rejected, or read as "keep the default",
        // depending on the option and the platform. Rounding a sub-second request
        // down to nothing would leave the two-hour system default in place while
        // this function reported success, which is exactly the silently-unarmed
        // state `KeepAliveSettings` says is worth nothing.
        auto const seconds = [](std::chrono::milliseconds value) {
            auto const whole = std::chrono::ceil<std::chrono::seconds>(value);
            return whole.count() > 0 ? static_cast<int>(whole.count()) : 1;
        };

        auto const fd = static_cast<int>(socket);

        // The INTERVALS FIRST, and the flag last. Reversed, a socket whose intervals
        // could not be applied would be left probing on the system default -- two
        // hours on Linux -- which is indistinguishable from no keepalive at all for
        // every deadline this protects, while reading back as armed to anything that
        // checks the flag.
    #if defined(__APPLE__)
        // macOS spells the idle time `TCP_KEEPALIVE`; it is `TCP_KEEPIDLE`
        // everywhere else. The other two are spelled the same on both.
        constexpr int IdleOption = TCP_KEEPALIVE;
    #else
        constexpr int IdleOption = TCP_KEEPIDLE;
    #endif
        auto const idle = seconds(settings.idle);
        auto const interval = seconds(settings.interval);
        auto const count = static_cast<int>(settings.count);

        if (::setsockopt(fd, IPPROTO_TCP, IdleOption, &idle, sizeof(idle)) != 0)
            return false;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) != 0)
            return false;
        if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0)
            return false;

        int const on = 1;
        return ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) == 0;
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

            // Claim the address exclusively -- see ExclusiveBindOption for what
            // that is spelled as here and why the other spelling is a hole.
            // Unlike the best-effort options below, a failure is fatal to this
            // candidate: the option carries a security property, and a daemon
            // that silently came up shareable is worse than one that visibly
            // did not come up at all.
            int const exclusive = 1;
            if (::setsockopt(
                    sock, SOL_SOCKET, ExclusiveBindOption, reinterpret_cast<char const*>(&exclusive), sizeof(exclusive))
                != 0)
            {
                lastError = std::format("cannot claim {}:{} exclusively: {}", host, port, LastSocketError());
                CloseSocket(static_cast<NativeSocket>(sock));
                continue;
            }

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

    ResolvedEndpoint EndpointFromSockaddr(void const* sockaddr, std::uint32_t length) noexcept
    {
        ResolvedEndpoint endpoint;
        if (length == 0 || length > ResolvedEndpoint::StorageSize)
            return endpoint;
        std::memcpy(endpoint.storage.data(), sockaddr, length);
        endpoint.length = length;
        // The address family lives in the first field of every sockaddr variant.
        endpoint.family = reinterpret_cast<struct sockaddr const*>(sockaddr)->sa_family;
        return endpoint;
    }

    bool IsNumericHost(std::string_view host) noexcept
    {
        if (host.empty())
            return false;

        // inet_pton needs a NUL-terminated string, and a host is short.
        std::string const text { host };

        // The two families in a table rather than two ifs, so a third (there is
        // none today) is a row. Storage is sized for the larger of the two.
        constexpr std::array<int, 2> Families { AF_INET, AF_INET6 };
        std::array<std::byte, sizeof(in6_addr)> scratch {};
        return std::ranges::any_of(
            Families, [&](int family) noexcept { return ::inet_pton(family, text.c_str(), scratch.data()) == 1; });
    }

    std::uint16_t PortOfSockaddr(void const* sockaddr, std::uint32_t length) noexcept
    {
        if (sockaddr == nullptr || length < sizeof(struct sockaddr))
            return 0;

        // Read the port out of whichever family this actually is: the two
        // sockaddr layouts put it at different offsets, and reading the wrong one
        // yields a plausible-looking number rather than an error.
        auto const family = reinterpret_cast<struct sockaddr const*>(sockaddr)->sa_family;
        if (family == AF_INET && length >= sizeof(sockaddr_in))
            return ntohs(reinterpret_cast<sockaddr_in const*>(sockaddr)->sin_port);
        if (family == AF_INET6 && length >= sizeof(sockaddr_in6))
            return ntohs(reinterpret_cast<sockaddr_in6 const*>(sockaddr)->sin6_port);
        return 0;
    }

    std::uint16_t BoundPortOf(NativeSocket socket) noexcept
    {
        if (socket == InvalidSocket)
            return 0;

        sockaddr_storage storage {};
#if defined(_WIN32)
        auto length = static_cast<int>(sizeof(storage));
        auto const handle = static_cast<SOCKET>(socket);
#else
        auto length = static_cast<socklen_t>(sizeof(storage));
        auto const handle = static_cast<int>(socket);
#endif
        if (::getsockname(handle, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
            return 0;

        return PortOfSockaddr(&storage, static_cast<std::uint32_t>(length));
    }

    std::string PeerAddressOf(NativeSocket socket) noexcept
    {
        sockaddr_storage peer {};
        AddrLen length = sizeof(peer);
        if (::getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &length) != 0)
            return {};
        return FormatPeerAddress(EndpointFromSockaddr(&peer, static_cast<std::uint32_t>(length)));
    }

} // namespace Detail

std::string FormatPeerAddress(ResolvedEndpoint const& endpoint)
{
    if (endpoint.length == 0)
        return {};

    // The address pointer differs per family; the textual conversion does not.
    void const* addr = nullptr;
    switch (endpoint.family)
    {
        case AF_INET:
            addr = &reinterpret_cast<sockaddr_in const*>(endpoint.storage.data())->sin_addr;
            break;
        case AF_INET6:
            addr = &reinterpret_cast<sockaddr_in6 const*>(endpoint.storage.data())->sin6_addr;
            break;
        default:
            return {};
    }

    std::array<char, INET6_ADDRSTRLEN> text {};
    if (::inet_ntop(endpoint.family, addr, text.data(), text.size()) == nullptr)
        return {};
    return std::string { text.data() };
}

} // namespace FastCache
