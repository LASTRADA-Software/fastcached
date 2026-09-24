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
#include <string>
#include <string_view>
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
    /// `SystemError`. A table, because the reader's question is "which code is which", not how a
    /// chain of comparisons happens to be ordered.
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
    };
#else
    constexpr auto SocketErrors = std::array {
        SocketErrorRow { .osError = EAGAIN, .code = core::net::NetErrorCode::Timeout },
        SocketErrorRow { .osError = EWOULDBLOCK, .code = core::net::NetErrorCode::Timeout },
        SocketErrorRow { .osError = EINTR, .code = core::net::NetErrorCode::Cancelled },
        SocketErrorRow { .osError = EBADF, .code = core::net::NetErrorCode::BadHandle },
        SocketErrorRow { .osError = ENOTSOCK, .code = core::net::NetErrorCode::BadHandle },
    };
#endif

    /// A socket failure as the NetError a listener answers with.
    [[nodiscard]] core::net::NetError SystemError(std::string_view context)
    {
        auto const osError = LastSocketError();
        // A range-for rather than `std::ranges::find`: the iterator is a raw pointer in one
        // standard library and a class in another, and no single spelling of it satisfies both
        // the compilers and clang-tidy's qualified-auto rule.
        for (auto const& row: SocketErrors)
            if (row.osError == osError)
                return core::net::makeNetError(row.code, osError, std::string { context });
        return core::net::makeNetError(core::net::NetErrorCode::SystemError, osError, std::string { context });
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

    /// Make @p socket non-blocking, which `core::net::adoptListener` asks of a listener it adopts.
    void SetNonBlocking(SocketValue socket) noexcept
    {
#if defined(_WIN32)
        u_long on = 1;
        std::ignore = ::ioctlsocket(socket, FIONBIO, &on);
#else
        auto const flags = ::fcntl(socket, F_GETFL, 0);
        if (flags >= 0)
            std::ignore = ::fcntl(socket, F_SETFL, flags | O_NONBLOCK);
#endif
    }
} // namespace

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
    constexpr auto SocketBufferBytes = 1 << 20;
    SetOption(socket, IPPROTO_TCP, TCP_NODELAY, 1);
    SetOption(socket, SOL_SOCKET, SO_SNDBUF, SocketBufferBytes);
    SetOption(socket, SOL_SOCKET, SO_RCVBUF, SocketBufferBytes);
}

std::expected<BoundSocket, std::string> BindAndListen(
    core::net::IAddressResolver& resolver, std::string_view host, std::uint16_t port, int backlog, ReusePort reusePort)
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
        auto const owned = ToHandle(sock);
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

#if defined(SO_REUSEPORT)
        if (reusePort == ReusePort::Yes)
            SetOption(sock, SOL_SOCKET, SO_REUSEPORT, 1);
#else
        std::ignore = reusePort;
#endif

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

std::expected<std::unique_ptr<core::net::IListener>, std::string> ListenOnSharedPort(core::net::EventLoop& loop,
                                                                                     std::string_view host,
                                                                                     std::uint16_t port,
                                                                                     int backlog,
                                                                                     core::net::IAddressResolver& resolver)
{
    auto bound = BindAndListen(resolver, host, port, backlog, ReusePort::Yes);
    if (!bound.has_value())
        return std::unexpected(std::move(bound).error());
    SetNonBlocking(ToSocket(bound->handle));
    auto adopted = core::net::adoptListener(loop, bound->handle);
    if (!adopted.has_value())
    {
        // On failure the caller still owns the handle (core::net::adoptListener's contract).
        CloseNativeSocket(bound->handle);
        return std::unexpected(adopted.error().toString());
    }
    return std::move(adopted).value();
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
    auto const handle = ToHandle(static_cast<SocketValue>(descriptor));
    SetNonBlocking(ToSocket(handle));
    auto adopted = core::net::adoptListener(loop, handle);
    if (!adopted.has_value())
    {
        CloseNativeSocket(handle);
        return std::unexpected(adopted.error().toString());
    }
    return std::move(adopted).value();
#endif
}

std::expected<AcceptedSocket, core::net::NetError> AcceptRaw(core::platform::NativeHandle listening)
{
    sockaddr_storage client {};
    auto length = static_cast<AddrLen>(sizeof(client));
    auto const accepted = ::accept(ToSocket(listening), reinterpret_cast<sockaddr*>(&client), &length);
    if (accepted == InvalidSocketValue)
        return std::unexpected(SystemError("accept"));

    auto const handle = ToHandle(accepted);
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
    auto bound = BindAndListen(resolver, bindAddress, port, backlog, ReusePort::No);
    if (bound.has_value())
        listener->_handle = bound->handle;
    else
        listener->_bindError = std::move(bound).error();
    return listener;
}

std::unique_ptr<BlockingListener> BlockingListener::Adopt(core::platform::NativeHandle handle)
{
    auto listener = std::unique_ptr<BlockingListener> { new BlockingListener {} };
    listener->_handle = handle;
    return listener;
}

BlockingListener::~BlockingListener()
{
    BlockingListener::close();
}

void BlockingListener::close() noexcept
{
    CloseNativeSocket(std::exchange(_handle, core::platform::InvalidHandle));
}

std::uint16_t BlockingListener::boundPort() const noexcept
{
    return BoundPortOf(_handle);
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
