// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Transport/NativeListen.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

#include <core/net/BlockingSocket.hpp>
#include <core/net/Sockets.hpp>
#include <core/platform/WinsockInit.hpp>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <sys/time.h>

    #include <cerrno>

    #include <fcntl.h>
    #include <poll.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>

    #if defined(__APPLE__)
        #include <sys/proc_info.h>

        #include <libproc.h>
    #endif
#endif

namespace FastCache
{

namespace
{
#if defined(_WIN32)
    using AddrLen = int;
    using SocketValue = SOCKET;

    /// The option that keeps a listening address to the process that bound it. On Windows
    /// `SO_REUSEADDR` would let a second socket take an address a LIVE socket already holds (#85).
    constexpr int ExclusiveBindOption = SO_EXCLUSIVEADDRUSE;

    [[nodiscard]] int LastSocketError() noexcept
    {
        return WSAGetLastError();
    }

    [[nodiscard]] SocketValue ToSocket(core::platform::NativeHandle handle) noexcept
    {
        return reinterpret_cast<SocketValue>(handle);
    }

    [[nodiscard]] core::platform::NativeHandle ToHandle(SocketValue socket) noexcept
    {
        return reinterpret_cast<core::platform::NativeHandle>(socket);
    }

    constexpr SocketValue InvalidSocketValue = INVALID_SOCKET;
#else
    using AddrLen = socklen_t;
    using SocketValue = int;

    /// On POSIX `SO_REUSEADDR` only lets a bind step over a DEAD socket's TIME_WAIT; a live listener
    /// still holds the address alone.
    constexpr int ExclusiveBindOption = SO_REUSEADDR;

    [[nodiscard]] int LastSocketError() noexcept
    {
        return errno;
    }

    [[nodiscard]] SocketValue ToSocket(core::platform::NativeHandle handle) noexcept
    {
        return handle;
    }

    [[nodiscard]] core::platform::NativeHandle ToHandle(SocketValue socket) noexcept
    {
        return socket;
    }

    constexpr SocketValue InvalidSocketValue = -1;
#endif

    /// Set an integer socket option, ignoring the answer: every caller here is best-effort.
    void SetOption(SocketValue socket, int level, int name, int value) noexcept
    {
        std::ignore = ::setsockopt(socket, level, name, reinterpret_cast<char const*>(&value), sizeof(value));
    }

    /// The OS errors a listener names, each with the code it answers as; anything else is
    /// `SystemError`, which every accept loop reads as UNCLASSIFIED (`core::net::AcceptErrorPolicy`):
    /// it backs off on it, and a long enough run of it reports the loop degraded. A table, because
    /// the reader's question is "which code is which", not how a chain of comparisons happens to be
    /// ordered.
    ///
    /// **So a per-connection failure needs a row, or it costs the loop a backoff per connection.**
    /// Without one a flood of resets reads as a failure nobody classified, each reset buys up to a
    /// second of not accepting -- the failure mode the first cut of the accept policy had -- and a
    /// long enough flood calls the surface degraded. Running out of descriptors, buffer space or
    /// memory has rows of its own, `ResourceExhausted`, which a loop backs off on without ever
    /// calling itself degraded: nothing is wrong with the listener, only with what it has left.
    struct SocketErrorRow
    {
        int osError;                  ///< The platform's error number.
        core::net::NetErrorCode code; ///< What a caller is told.
    };

#if defined(_WIN32)
    /// `WSAEINTR` is a blocking `accept` whose socket was closed under it; `WSAENOTSOCK` one
    /// called on a socket already closed -- two facts, measured on Windows 11 (#1238).
    constexpr auto SocketErrors = std::array {
        SocketErrorRow { .osError = WSAETIMEDOUT, .code = core::net::NetErrorCode::Timeout },
        SocketErrorRow { .osError = WSAEWOULDBLOCK, .code = core::net::NetErrorCode::Timeout },
        SocketErrorRow { .osError = WSAEINTR, .code = core::net::NetErrorCode::Cancelled },
        SocketErrorRow { .osError = WSAENOTSOCK, .code = core::net::NetErrorCode::BadHandle },
        // A queued connection its client reset or abandoned before this `accept()` took it: one
        // connection's failure, never the listener's.
        SocketErrorRow { .osError = WSAECONNRESET, .code = core::net::NetErrorCode::ConnReset },
        SocketErrorRow { .osError = WSAECONNABORTED, .code = core::net::NetErrorCode::ConnReset },
        // Out of descriptors, buffer space or memory: the process's, never the listener's.
        SocketErrorRow { .osError = WSAEMFILE, .code = core::net::NetErrorCode::ResourceExhausted },
        SocketErrorRow { .osError = WSAENOBUFS, .code = core::net::NetErrorCode::ResourceExhausted },
        SocketErrorRow { .osError = WSA_NOT_ENOUGH_MEMORY, .code = core::net::NetErrorCode::ResourceExhausted },
    };
#else
    /// **`EINTR` is a signal, never a closed socket, and it answers `Timeout`.** POSIX does not wake
    /// a parked `poll()` or `accept()` when another thread closes the socket (the class comment of
    /// `BlockingListener`), so nothing here can mean *closed* by `EINTR` -- and a caught signal
    /// interrupts a parked `poll()` whatever `SA_RESTART` says. Answered `Cancelled`, one SIGHUP
    /// landing on the admin thread ended its accept loop and took `/healthz` with it while the
    /// process lived. `Timeout` is what the poll would have answered a moment later: the caller
    /// re-checks its own stop flag and polls again. Windows keeps `Cancelled` for `WSAEINTR`,
    /// which there IS the close, measured.
    constexpr auto SocketErrors = std::array {
        SocketErrorRow { .osError = EAGAIN, .code = core::net::NetErrorCode::Timeout },
        SocketErrorRow { .osError = EWOULDBLOCK, .code = core::net::NetErrorCode::Timeout },
        SocketErrorRow { .osError = EINTR, .code = core::net::NetErrorCode::Timeout },
        SocketErrorRow { .osError = EBADF, .code = core::net::NetErrorCode::BadHandle },
        SocketErrorRow { .osError = ENOTSOCK, .code = core::net::NetErrorCode::BadHandle },
        // One connection's failure, each of them. `ECONNABORTED` is a queued connection its client
        // abandoned; `EPERM` is Linux refusing that one connection by firewall rule; and the rest
        // are accept(2)'s list of network errors already pending on the new connection, which
        // Linux hands back from `accept()` itself and asks the caller to treat like `EAGAIN`.
        SocketErrorRow { .osError = ECONNABORTED, .code = core::net::NetErrorCode::ConnReset },
        SocketErrorRow { .osError = EPERM, .code = core::net::NetErrorCode::PermissionDenied },
        SocketErrorRow { .osError = EPROTO, .code = core::net::NetErrorCode::ConnReset },
        SocketErrorRow { .osError = ENOPROTOOPT, .code = core::net::NetErrorCode::ConnReset },
        // On accept(2)'s pending-error list for TCP/IP as well, so one connection's failure. accept(2)
        // also answers it, forever, for a listener that is not SOCK_STREAM; that one is refused where
        // it is known, at `BlockingListener::Adopt`, since answered `Stop` here one connection could
        // end the loop.
        SocketErrorRow { .osError = EOPNOTSUPP, .code = core::net::NetErrorCode::ConnReset },
        SocketErrorRow { .osError = ENETDOWN, .code = core::net::NetErrorCode::HostUnreach },
        SocketErrorRow { .osError = ENETUNREACH, .code = core::net::NetErrorCode::HostUnreach },
        SocketErrorRow { .osError = EHOSTDOWN, .code = core::net::NetErrorCode::HostUnreach },
        SocketErrorRow { .osError = EHOSTUNREACH, .code = core::net::NetErrorCode::HostUnreach },
    #if defined(ENONET)
        // Linux only.
        SocketErrorRow { .osError = ENONET, .code = core::net::NetErrorCode::HostUnreach },
    #endif
        // Out of descriptors -- the process's or the system's -- buffer space or memory: the
        // process's, never the listener's.
        SocketErrorRow { .osError = EMFILE, .code = core::net::NetErrorCode::ResourceExhausted },
        SocketErrorRow { .osError = ENFILE, .code = core::net::NetErrorCode::ResourceExhausted },
        SocketErrorRow { .osError = ENOBUFS, .code = core::net::NetErrorCode::ResourceExhausted },
        SocketErrorRow { .osError = ENOMEM, .code = core::net::NetErrorCode::ResourceExhausted },
    };
#endif

    /// A socket failure as the NetError a listener answers with.
    [[nodiscard]] core::net::NetError SystemError(std::string_view context)
    {
        auto const osError = LastSocketError();
        return core::net::makeNetError(SocketErrorCode(osError), osError, std::string { context });
    }

    /// Set the receive and send timeouts of @p socket; a non-positive value leaves one alone.
    void SetIoTimeouts(SocketValue socket, std::chrono::milliseconds receive, std::chrono::milliseconds send) noexcept
    {
        auto const apply = [socket](int option, std::chrono::milliseconds timeout) noexcept {
            if (timeout.count() <= 0)
                return;
#if defined(_WIN32)
            auto const millis = static_cast<DWORD>(timeout.count());
            std::ignore = ::setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<char const*>(&millis), sizeof(millis));
#else
            timeval tv {};
            tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / 1000);
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
            std::ignore = ::setsockopt(socket, SOL_SOCKET, option, &tv, sizeof(tv));
#endif
        };
        apply(SO_RCVTIMEO, receive);
        apply(SO_SNDTIMEO, send);
    }

    /// A socket type an adopted descriptor may have, by the name an operator would search for.
    struct SocketTypeName
    {
        int type;              ///< The `SO_TYPE` value.
        std::string_view name; ///< Its constant's spelling.
    };

    /// The types a refusal names; any other is reported by number.
    ///
    /// Not `SOCK_DGRAM`: `udp-opener` reads every use of it in first-party source as opening a UDP
    /// socket, which the compile node's any-port firewall rule would expose, and that guard is
    /// closed on purpose rather than taught which uses are only names. A datagram descriptor is
    /// reported by its number, which is what `SO_TYPE` answered.
    constexpr auto SocketTypeNames = std::array {
        SocketTypeName { .type = SOCK_STREAM, .name = "SOCK_STREAM" },
        SocketTypeName { .type = SOCK_RAW, .name = "SOCK_RAW" },
        SocketTypeName { .type = SOCK_SEQPACKET, .name = "SOCK_SEQPACKET" },
    };

    /// @param type An `SO_TYPE` value.
    /// @return Its constant's spelling, or the number when no row names it.
    [[nodiscard]] std::string SocketTypeText(int type)
    {
        for (auto const& row: SocketTypeNames)
            if (row.type == type)
                return std::string { row.name };
        return std::format("socket type {}", type);
    }

    /// What the OS said about the call that just failed, in its own words: @p call, the platform's
    /// message for the error and its number.
    ///
    /// **Never through `SocketErrors`**: that table answers what an ACCEPT LOOP does with a code,
    /// and read back as a description it named macOS's `ENOPROTOOPT` from `getsockopt` "connection
    /// reset" -- the accept(2) pending-error row it shares a number with -- in the one line that
    /// reported every adoption on macOS refused.
    /// @param call The call that failed, e.g. `getsockopt`.
    /// @return The text.
    [[nodiscard]] std::string OsErrorText(std::string_view call)
    {
        auto const osError = LastSocketError();
        return std::format("{}: {} [{}]", call, std::system_category().message(osError), osError);
    }

    /// One integer `SOL_SOCKET` option of @p socket.
    /// @param socket The descriptor to ask.
    /// @param option The option, e.g. `SO_TYPE`.
    /// @param what What the option is called in a refusal, e.g. "its socket type".
    /// @return The value, or the refusal naming what could not be asked.
    [[nodiscard]] std::expected<int, std::string> SocketOption(core::platform::NativeHandle socket,
                                                               int option,
                                                               std::string_view what)
    {
        int value = 0;
        auto length = static_cast<AddrLen>(sizeof(value));
        if (::getsockopt(ToSocket(socket), SOL_SOCKET, option, reinterpret_cast<char*>(&value), &length) != 0)
            return std::unexpected { std::format(
                "adopt: the descriptor could not be asked {} ({})", what, OsErrorText("getsockopt")) };
        return value;
    }

    /// Whether @p socket listens: non-zero when it does, as `SO_ACCEPTCONN` reads.
    ///
    /// **XNU does not answer `getsockopt(SO_ACCEPTCONN)`**: macOS refuses it with `ENOPROTOOPT`, so
    /// every adoption there was refused, a listening socket included -- the macOS job's socket
    /// activation and consensus cases, all at once. The flag is still the kernel's: `listen(2)` sets
    /// it in the socket's options, and libproc reports those options for a descriptor this process
    /// holds (`soi_options`), which is where macOS answers the same question.
    /// @param socket The descriptor being adopted.
    /// @return Non-zero when it listens, zero when it does not, or the refusal naming what could
    ///         not be asked.
    [[nodiscard]] std::expected<int, std::string> ListeningFlag(core::platform::NativeHandle socket)
    {
#if defined(__APPLE__)
        struct socket_fdinfo info {};

        auto const size = ::proc_pidfdinfo(::getpid(), socket, PROC_PIDFDSOCKETINFO, &info, sizeof(info));
        if (size != static_cast<int>(sizeof(info)))
            return std::unexpected { std::format("adopt: the descriptor could not be asked whether it listens ({})",
                                                 OsErrorText("proc_pidfdinfo")) };
        return (info.psi.soi_options & SO_ACCEPTCONN) != 0 ? 1 : 0;
#else
        return SocketOption(socket, SO_ACCEPTCONN, "whether it listens");
#endif
    }

    /// Why @p socket cannot be an accept loop's listener, or nothing when it can.
    ///
    /// **A listener that is not a LISTENING `SOCK_STREAM` socket is a CONFIGURATION fact, and it is
    /// refused where it is known.** accept(2) fails on either forever: `EOPNOTSUPP` on another type
    /// and `EINVAL` on a stream socket nobody called listen() on. But `EOPNOTSUPP` is also a
    /// network error already pending on ONE new TCP connection, which is why `SocketErrors`
    /// classifies it as a per-connection failure the loop steps past, and anything unclassified
    /// is backed off on and warned about, forever, and reported degraded. Classified as ending the
    /// loop instead, one connection could end it -- the defect every accept loop here was fixed
    /// for. So the descriptor is asked its type and whether it listens once, at adoption, and a
    /// loop over the wrong kind never starts.
    /// @param socket The descriptor being adopted.
    /// @return The refusal, naming what is wrong, or nothing for a listening stream socket.
    [[nodiscard]] std::optional<std::string> RefusalUnlessListeningStream(core::platform::NativeHandle socket)
    {
        auto const type = SocketOption(socket, SO_TYPE, "its socket type");
        if (!type.has_value())
            return type.error();
        if (*type != SOCK_STREAM)
            return std::format("adopt: the descriptor is {}, not SOCK_STREAM; accept() on it would fail "
                               "forever, so it cannot be a listener",
                               SocketTypeText(*type));
        auto const listening = ListeningFlag(socket);
        if (!listening.has_value())
            return listening.error();
        if (*listening == 0)
            return std::string { "adopt: the descriptor is a SOCK_STREAM socket nobody called listen() on "
                                 "(SO_ACCEPTCONN is 0); accept() on it would fail forever, so it cannot be a "
                                 "listener" };
        return std::nullopt;
    }
} // namespace

core::net::NetErrorCode SocketErrorCode(int osError) noexcept
{
    // A range-for rather than `std::ranges::find`: the iterator is a raw pointer in one standard
    // library and a class in another, and no single spelling of it satisfies both the compilers and
    // clang-tidy's qualified-auto rule.
    for (auto const& row: SocketErrors)
        if (row.osError == osError)
            return row.code;
    return core::net::NetErrorCode::SystemError;
}

void CloseNativeSocket(core::platform::NativeHandle socket) noexcept
{
    if (socket == core::platform::InvalidHandle)
        return;
#if defined(_WIN32)
    std::ignore = ::closesocket(ToSocket(socket));
#else
    std::ignore = ::close(ToSocket(socket));
#endif
}

void ApplyHotSocketOptions(core::platform::NativeHandle handle) noexcept
{
    auto const socket = ToSocket(handle);
#if defined(_WIN32)
    // A socket is inheritable unless something says otherwise, and `::accept()` hands back an
    // inheritable one: without this every client connection is handed to every compiler spawned.
    std::ignore = ::SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0);
#else
    auto const flags = ::fcntl(socket, F_GETFD, 0);
    if (flags >= 0)
        std::ignore = ::fcntl(socket, F_SETFD, flags | FD_CLOEXEC);
#endif
    // A small reply is not held back for the peer's ACK of an earlier one, and a large one leaves in
    // one write: the kernel clamps the buffer to its own maximum, so this is an upper hint.
    constexpr auto SocketBufferBytes = static_cast<int>(ClientSocketBufferBytes);
    SetOption(socket, IPPROTO_TCP, TCP_NODELAY, 1);
    SetOption(socket, SOL_SOCKET, SO_SNDBUF, SocketBufferBytes);
    SetOption(socket, SOL_SOCKET, SO_RCVBUF, SocketBufferBytes);
}

core::net::ListenOptions ClientListenOptions(std::string_view host,
                                             std::uint16_t port,
                                             int backlog,
                                             core::net::PortSharing sharing) noexcept
{
    return core::net::ListenOptions {
        .host = host,
        .port = port,
        .backlog = backlog,
        .sharing = sharing,
        .buffers = { .send = ClientSocketBufferBytes, .receive = ClientSocketBufferBytes },
    };
}

std::expected<BoundSocket, std::string> BindAndListen(core::net::IAddressResolver& resolver,
                                                      std::string_view host,
                                                      std::uint16_t port,
                                                      int backlog)
{
    core::platform::ensureWinsockInitialized();

    auto resolved = resolver.resolve(host, port);
    if (!resolved.has_value())
        return std::unexpected(std::move(resolved).error());

    auto lastError = std::string {};
    for (auto const& endpoint: *resolved)
    {
        auto const sock = ::socket(endpoint.family, SOCK_STREAM, endpoint.protocol);
        if (sock == InvalidSocketValue)
        {
            lastError = std::format("socket() failed: {}", LastSocketError());
            continue;
        }
        // Its own type, not `auto`: a pointer on Windows and an int on POSIX, so neither `auto`
        // (qualified-auto wants `auto*` on Windows) nor `auto*` (no pointer on POSIX) is right on both.
        core::platform::NativeHandle const owned = ToHandle(sock);
        ApplyHotSocketOptions(owned);

        // Fatal to this candidate: the option carries a security property, and a daemon that
        // silently came up shareable is worse than one that visibly did not come up at all.
        auto const exclusive = 1;
        if (::setsockopt(sock, SOL_SOCKET, ExclusiveBindOption, reinterpret_cast<char const*>(&exclusive), sizeof(exclusive))
            != 0)
        {
            lastError = std::format("cannot claim {}:{} exclusively: {}", host, port, LastSocketError());
            CloseNativeSocket(owned);
            continue;
        }

        // Dual-stack, so a "::" wildcard accepts IPv4 clients too; best-effort.
        if (endpoint.family == AF_INET6)
            SetOption(sock, IPPROTO_IPV6, IPV6_V6ONLY, 0);

        if (::bind(sock, reinterpret_cast<sockaddr const*>(endpoint.storage.data()), static_cast<AddrLen>(endpoint.length))
            != 0)
        {
            lastError = std::format("bind({}:{}) failed: {}", host, port, LastSocketError());
            CloseNativeSocket(owned);
            continue;
        }
        if (::listen(sock, backlog) != 0)
        {
            lastError = std::format("listen({}:{}) failed: {}", host, port, LastSocketError());
            CloseNativeSocket(owned);
            continue;
        }
        return BoundSocket { .handle = owned, .family = endpoint.family };
    }
    return std::unexpected(lastError.empty() ? std::format("no usable address for '{}:{}'", host, port) : lastError);
}

std::uint16_t BoundPortOf(core::platform::NativeHandle socket) noexcept
{
    if (socket == core::platform::InvalidHandle)
        return 0;
    sockaddr_storage address {};
    auto length = static_cast<AddrLen>(sizeof(address));
    if (::getsockname(ToSocket(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return 0;
    return core::net::detail::portOfSockaddr(&address, static_cast<std::uint32_t>(length));
}

namespace
{
    /// An address's bytes as `inet_ntop` spells them: the ONE spelling both `BoundAddressOf` and
    /// `CanonicalAddressLiteral` produce, so the kernel's answer and a configured literal compare.
    /// @param family `AF_INET` or `AF_INET6`. @param bytes The `in_addr` or `in6_addr`.
    /// @return The text, or empty when it does not convert.
    std::string FormatAddressBytes(int family, void const* bytes)
    {
        std::array<char, INET6_ADDRSTRLEN> text {};
        if (::inet_ntop(family, bytes, text.data(), text.size()) == nullptr)
            return {};
        return std::string { text.data() };
    }
} // namespace

std::string BoundAddressOf(core::platform::NativeHandle socket)
{
    if (socket == core::platform::InvalidHandle)
        return {};
    sockaddr_storage address {};
    auto length = static_cast<AddrLen>(sizeof(address));
    if (::getsockname(ToSocket(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return {};
    switch (address.ss_family)
    {
        case AF_INET:
            return FormatAddressBytes(AF_INET, &reinterpret_cast<sockaddr_in const*>(&address)->sin_addr);
        case AF_INET6:
            return FormatAddressBytes(AF_INET6, &reinterpret_cast<sockaddr_in6 const*>(&address)->sin6_addr);
        default:
            return {};
    }
}

std::optional<std::string> CanonicalAddressLiteral(std::string_view text)
{
    // `inet_pton` needs a NUL-terminated string, and is the platform's own definition of a
    // literal -- no character inspection here to disagree with it.
    auto const terminated = std::string { text };
    in_addr v4 {};
    if (::inet_pton(AF_INET, terminated.c_str(), &v4) == 1)
        return FormatAddressBytes(AF_INET, &v4);
    in6_addr v6 {};
    if (::inet_pton(AF_INET6, terminated.c_str(), &v6) == 1)
        return FormatAddressBytes(AF_INET6, &v6);
    return std::nullopt;
}

std::optional<BoundEndpoint> BoundEndpointOfDescriptor([[maybe_unused]] int descriptor)
{
#if defined(_WIN32)
    // A descriptor number names no socket here, for the reason `AdoptInheritedListener` gives.
    return std::nullopt;
#else
    if (descriptor < 0)
        return std::nullopt;
    // The address in `BoundAddressOf`'s spelling -- the one `CanonicalAddressLiteral` gives a
    // configured literal, so the two compare -- and the port as `BoundPortOf` reads it.
    auto const handle = ToHandle(static_cast<SocketValue>(descriptor));
    auto bound = BoundEndpoint { .host = BoundAddressOf(handle), .port = BoundPortOf(handle) };
    // A family with no address or no port (a UNIX socket) names no endpoint a worker could dial.
    if (bound.host.empty() || bound.port == 0)
        return std::nullopt;
    return bound;
#endif
}

std::expected<std::unique_ptr<core::net::IListener>, std::string> AdoptInheritedListener(core::net::EventLoop& loop,
                                                                                         int descriptor)
{
#if defined(_WIN32)
    // A supervisor hands over a descriptor NUMBER, which names no socket here: Windows has no
    // socket activation, so there is nothing this number could own or close.
    static_cast<void>(loop);
    static_cast<void>(descriptor);
    return std::unexpected(std::string { "socket activation is not available on this platform" });
#else
    if (descriptor < 0)
        // Refused before anything touches it, and nothing to close.
        return std::unexpected(std::string { "adopt: not a descriptor" });
    return AdoptBoundListener(loop, ToHandle(static_cast<SocketValue>(descriptor)));
#endif
}

std::expected<std::unique_ptr<core::net::IListener>, std::string> AdoptBoundListener(core::net::EventLoop& loop,
                                                                                     core::platform::NativeHandle handle)
{
    if (handle == core::platform::InvalidHandle)
        // Refused before anything touches it, and nothing to close.
        return std::unexpected(std::string { "adopt: not a socket" });
    // Asked before core-cpp takes it, which does not ask: a descriptor a supervisor handed over, or
    // a listener this process bound, that is not a LISTENING stream socket would otherwise start a
    // reactor loop whose every accept fails, forever.
    if (auto refusal = RefusalUnlessListeningStream(handle); refusal.has_value())
    {
        CloseNativeSocket(handle);
        return std::unexpected(std::move(*refusal));
    }
    // Made non-blocking and close-on-exec by core-cpp itself (`PosixListener::adopt`), which is
    // where the reactor's requirement lives; a second `fcntl` here was measured to change nothing
    // -- removed, every [adopt] and [consensus] case stayed green on Linux.
    auto adopted = core::net::adoptListener(loop, handle);
    if (!adopted.has_value())
    {
        CloseNativeSocket(handle);
        return std::unexpected(adopted.error().toString());
    }
    return std::move(adopted).value();
}

std::expected<AcceptedSocket, core::net::NetError> AcceptRaw(core::platform::NativeHandle listening)
{
    sockaddr_storage client {};
    auto length = static_cast<AddrLen>(sizeof(client));
    auto const accepted = ::accept(ToSocket(listening), reinterpret_cast<sockaddr*>(&client), &length);
    if (accepted == InvalidSocketValue)
        return std::unexpected(SystemError("accept"));

    // Its own type, for the reason `owned` in `BindAndListen` has one.
    core::platform::NativeHandle const handle = ToHandle(accepted);
    ApplyHotSocketOptions(handle);
    return AcceptedSocket {
        .handle = handle,
        .peer = core::net::formatPeerAddress(
            core::net::detail::endpointFromSockaddr(&client, static_cast<std::uint32_t>(length))),
    };
}

// -- BlockingListener -------------------------------------------------------------------------

std::unique_ptr<BlockingListener> BlockingListener::Bind(std::string_view bindAddress,
                                                         std::uint16_t port,
                                                         int backlog,
                                                         core::net::IAddressResolver& resolver)
{
    auto listener = std::unique_ptr<BlockingListener> { new BlockingListener {} };
    auto bound = BindAndListen(resolver, bindAddress, port, backlog);
    if (bound.has_value())
        listener->_handle = bound->handle;
    else
        listener->_bindError = std::move(bound).error();
    return listener;
}

std::unique_ptr<BlockingListener> BlockingListener::Adopt(core::platform::NativeHandle handle)
{
    auto listener = std::unique_ptr<BlockingListener> { new BlockingListener {} };
    if (auto refusal = RefusalUnlessListeningStream(handle); refusal.has_value())
    {
        // Owned from the call on, refused or not, so the caller never closes it itself.
        CloseNativeSocket(handle);
        listener->_bindError = std::move(*refusal);
        return listener;
    }
    listener->_handle = handle;
    return listener;
}

BlockingListener::~BlockingListener()
{
    // Inside this destructor `close()` still reaches this class's own `doClose()`.
    close();
}

void BlockingListener::doClose() noexcept
{
    CloseNativeSocket(std::exchange(_handle, core::platform::InvalidHandle));
}

std::uint16_t BlockingListener::boundPort() const noexcept
{
    return BoundPortOf(_handle);
}

std::string BlockingListener::BoundAddress() const
{
    return BoundAddressOf(_handle);
}

void BlockingListener::SetTimeouts(std::chrono::milliseconds acceptPoll, std::chrono::milliseconds ioTimeout) noexcept
{
    _ioTimeout = ioTimeout;
    _acceptPoll = acceptPoll;
    // A receive timeout makes `::accept()` return periodically where the platform honours it, and
    // `AcceptNow` polls first everywhere, because macOS and the BSDs do not apply SO_RCVTIMEO to
    // `accept()`.
    if (_handle != core::platform::InvalidHandle)
        SetIoTimeouts(ToSocket(_handle), acceptPoll, std::chrono::milliseconds { 0 });
}

core::async::Task<core::net::AcceptResult> BlockingListener::accept()
{
    co_return AcceptNow();
}

core::net::AcceptResult BlockingListener::AcceptNow()
{
    if (_handle == core::platform::InvalidHandle)
        return std::unexpected(core::net::makeNetError(core::net::NetErrorCode::BadHandle, 0, _bindError));

    auto const listening = ToSocket(_handle);
    if (_acceptPoll.count() > 0)
    {
#if defined(_WIN32)
        WSAPOLLFD pfd {};
        pfd.fd = listening;
        pfd.events = POLLRDNORM;
        auto const ready = ::WSAPoll(&pfd, 1, static_cast<INT>(_acceptPoll.count()));
#else
        pollfd pfd {};
        pfd.fd = listening;
        pfd.events = POLLIN;
        auto const ready = ::poll(&pfd, 1, static_cast<int>(_acceptPoll.count()));
#endif
        if (ready == 0)
            return std::unexpected(core::net::makeNetError(core::net::NetErrorCode::Timeout, 0, "accept poll timed out"));
        if (ready < 0)
            return std::unexpected(SystemError("poll"));
    }

    auto accepted = AcceptRaw(_handle);
    if (!accepted.has_value())
        return std::unexpected(std::move(accepted).error());
    // Bound the request read, so a stalled client cannot wedge a blocking recv() and a single-threaded
    // admin endpoint stays available under slowloris.
    if (_ioTimeout.count() > 0)
        SetIoTimeouts(ToSocket(accepted->handle), _ioTimeout, _ioTimeout);
    return core::net::AcceptResult { std::make_unique<core::net::BlockingSocket>(accepted->handle,
                                                                                 std::move(accepted->peer)) };
}

} // namespace FastCache
