// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Core/Profiling.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/SocketAddress.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <sys/time.h> // struct timeval for SO_RCVTIMEO/SO_SNDTIMEO

    #include <cerrno>
    #include <csignal>

    #include <fcntl.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

namespace FastCache
{

namespace Detail
{

#if defined(_WIN32)
    /// Type of the `len` argument to recv()/send() on this platform.
    /// Windows uses `int`; POSIX uses `size_t`. Picked here so callers can
    /// write the cast once and not trip [clang-diagnostic-sign-conversion].
    using IoLen = int;
#else
    using IoLen = std::size_t;
#endif

#if defined(_WIN32)

    namespace
    {
        std::atomic<bool> g_winsockInitialised { false };
        std::atomic<bool> g_winsockInitialising { false };

        [[nodiscard]] int LastNetworkError() noexcept
        {
            return WSAGetLastError();
        }

        [[nodiscard]] NetErrorCode TranslateError(int code) noexcept
        {
            switch (code)
            {
                case WSAECONNRESET:
                    return NetErrorCode::ConnReset;
                case WSAECONNREFUSED:
                    return NetErrorCode::ConnRefused;
                case WSAEHOSTUNREACH:
                    return NetErrorCode::HostUnreach;
                case WSAEADDRINUSE:
                    return NetErrorCode::AddressInUse;
                case WSAEADDRNOTAVAIL:
                    return NetErrorCode::AddressNotAvail;
                case WSAEACCES:
                    return NetErrorCode::PermissionDenied;
                case WSAEBADF:
                case WSAENOTSOCK:
                    return NetErrorCode::BadFileHandle;
                case WSAEINTR:
                    return NetErrorCode::Cancelled;
                case WSAEWOULDBLOCK:
                    return NetErrorCode::WouldBlock;
                case WSAETIMEDOUT:
                    return NetErrorCode::Timeout;
                default:
                    return NetErrorCode::SystemError;
            }
        }

        [[nodiscard]] int CloseNative(NativeSocket s) noexcept
        {
            return ::closesocket(static_cast<SOCKET>(s));
        }
    } // namespace

    void EnsureNetworkInitialised()
    {
        if (g_winsockInitialised.load(std::memory_order_acquire))
            return;
        bool expected = false;
        if (!g_winsockInitialising.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            // Another thread is busy initialising; spin briefly.
            while (!g_winsockInitialised.load(std::memory_order_acquire))
            {
            }
            return;
        }
        WSADATA data {};
        WSAStartup(MAKEWORD(2, 2), &data);
        g_winsockInitialised.store(true, std::memory_order_release);
    }

#else

    namespace
    {
        std::atomic<bool> g_posixInitialised { false };

        [[nodiscard]] int LastNetworkError() noexcept
        {
            return errno;
        }

        [[nodiscard]] NetErrorCode TranslateError(int code) noexcept
        {
            switch (code)
            {
                case ECONNRESET:
                    return NetErrorCode::ConnReset;
                case ECONNREFUSED:
                    return NetErrorCode::ConnRefused;
                case EHOSTUNREACH:
                    return NetErrorCode::HostUnreach;
                case EADDRINUSE:
                    return NetErrorCode::AddressInUse;
                case EADDRNOTAVAIL:
                    return NetErrorCode::AddressNotAvail;
                case EACCES:
                    return NetErrorCode::PermissionDenied;
                case EBADF:
                case ENOTSOCK:
                    return NetErrorCode::BadFileHandle;
                case EINTR:
                    return NetErrorCode::Cancelled;
                case EWOULDBLOCK:
    #if EAGAIN != EWOULDBLOCK
                case EAGAIN:
    #endif
                    return NetErrorCode::WouldBlock;
                case ETIMEDOUT:
                    return NetErrorCode::Timeout;
                default:
                    return NetErrorCode::SystemError;
            }
        }

        [[nodiscard]] int CloseNative(NativeSocket s) noexcept
        {
            return ::close(s);
        }
    } // namespace

    void EnsureNetworkInitialised()
    {
        if (g_posixInitialised.load(std::memory_order_acquire))
            return;
        // Ignore SIGPIPE — broken-pipe writes should surface as EPIPE, not
        // kill the process.
        ::signal(SIGPIPE, SIG_IGN);
        g_posixInitialised.store(true, std::memory_order_release);
    }

#endif

} // namespace Detail

namespace
{
    [[nodiscard]] NetError MakeSystemError(std::string_view context)
    {
        auto const code = Detail::LastNetworkError();
        return NetError {
            .code = Detail::TranslateError(code),
            .systemCode = code,
            .context = std::string { context },
        };
    }
} // namespace

namespace Detail
{

    std::expected<NativeSocket, NetError> AcceptRaw(NativeSocket listenSocket) noexcept
    {
#if defined(_WIN32)
        auto const accepted = ::accept(static_cast<SOCKET>(listenSocket), nullptr, nullptr);
        if (accepted == INVALID_SOCKET)
            return std::unexpected(MakeSystemError("accept"));
        ApplyHotSocketOptions(static_cast<NativeSocket>(accepted));
        return static_cast<NativeSocket>(accepted);
#else
        auto const accepted = ::accept(listenSocket, nullptr, nullptr);
        if (accepted < 0)
            return std::unexpected(MakeSystemError("accept"));
        ApplyHotSocketOptions(static_cast<NativeSocket>(accepted));
        // epoll/kqueue reactors require non-blocking sockets.
        auto const flags = ::fcntl(accepted, F_GETFL, 0);
        if (flags >= 0)
            std::ignore = ::fcntl(accepted, F_SETFL, flags | O_NONBLOCK);
        return static_cast<NativeSocket>(accepted);
#endif
    }

    void CloseNativeSocket(NativeSocket socket) noexcept
    {
        std::ignore = CloseNative(socket);
    }

    void SetIoTimeouts(NativeSocket socket,
                       std::chrono::milliseconds recvTimeout,
                       std::chrono::milliseconds sendTimeout) noexcept
    {
        auto const apply = [socket](int option, std::chrono::milliseconds timeout) noexcept {
            if (timeout.count() <= 0)
                return; // 0 / negative: leave the OS default (no timeout) in place.
#if defined(_WIN32)
            // Windows SO_RCVTIMEO/SO_SNDTIMEO take a DWORD of milliseconds.
            auto const millis = static_cast<DWORD>(timeout.count());
            std::ignore = ::setsockopt(
                static_cast<SOCKET>(socket), SOL_SOCKET, option, reinterpret_cast<char const*>(&millis), sizeof(millis));
#else
            // POSIX SO_RCVTIMEO/SO_SNDTIMEO take a struct timeval.
            timeval tv {};
            tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / 1000);
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
            std::ignore = ::setsockopt(socket, SOL_SOCKET, option, &tv, sizeof(tv));
#endif
        };
        apply(SO_RCVTIMEO, recvTimeout);
        apply(SO_SNDTIMEO, sendTimeout);
    }

} // namespace Detail

// -- BlockingSocket --------------------------------------------------------

BlockingSocket::BlockingSocket(Detail::NativeSocket native) noexcept:
    _native { native }
{
}

BlockingSocket::~BlockingSocket()
{
    BlockingSocket::Close();
}

void BlockingSocket::Close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    if (_native != Detail::InvalidSocket)
    {
        std::ignore = Detail::CloseNative(_native);
        _native = Detail::InvalidSocket;
    }
}

IoAwaitable BlockingSocket::Read(std::span<std::byte> buffer)
{
    FC_ZONE_SCOPED_N("socket.read");
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    auto const got = ::recv(
        static_cast<int>(_native), reinterpret_cast<char*>(buffer.data()), static_cast<Detail::IoLen>(buffer.size()), 0);
    if (got < 0)
        return IoAwaitable { std::unexpected(MakeSystemError("recv")) };
    return IoAwaitable { IoResult { static_cast<std::size_t>(got) } };
}

IoAwaitable BlockingSocket::Write(std::span<std::byte const> buffer)
{
    FC_ZONE_SCOPED_N("socket.write");
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    std::size_t written = 0;
    while (written < buffer.size())
    {
        auto const n = ::send(static_cast<int>(_native),
                              reinterpret_cast<char const*>(buffer.data()) + written,
                              static_cast<Detail::IoLen>(buffer.size() - written),
                              0);
        if (n < 0)
            return IoAwaitable { std::unexpected(MakeSystemError("send")) };
        written += static_cast<std::size_t>(n);
    }
    return IoAwaitable { IoResult { written } };
}

IoAwaitable BlockingSocket::WriteVectored(std::span<std::span<std::byte const> const> segments,
                                          std::shared_ptr<void const> /*keepAlive*/)
{
    FC_ZONE_SCOPED_N("socket.writev");
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // A blocking socket sends everything before returning, so no keep-alive is
    // needed: the segments outlive the call by construction. Send each segment
    // fully, in order. (A scatter `writev`/`WSASend` is a possible refinement,
    // but the threaded driver is the legacy path; the reactor's EpollSocket
    // carries the zero-copy fast path.)
    std::size_t total = 0;
    for (auto const seg: segments)
    {
        std::size_t written = 0;
        while (written < seg.size())
        {
            auto const n = ::send(static_cast<int>(_native),
                                  reinterpret_cast<char const*>(seg.data()) + written,
                                  static_cast<Detail::IoLen>(seg.size() - written),
                                  0);
            if (n < 0)
                return IoAwaitable { std::unexpected(MakeSystemError("send")) };
            written += static_cast<std::size_t>(n);
        }
        total += written;
    }
    return IoAwaitable { IoResult { total } };
}

// -- BlockingListener ------------------------------------------------------

std::unique_ptr<BlockingListener> BlockingListener::Bind(std::string_view bindAddress,
                                                         std::uint16_t port,
                                                         int backlog,
                                                         IAddressResolver& resolver)
{
    std::unique_ptr<BlockingListener> listener { new BlockingListener {} };

    // Resolve (IPv4/IPv6 literal or hostname) + create + bind + listen, all in
    // the shared routine. On success store the listening socket; on failure
    // record the diagnostic for Accept() to surface as a NetError.
    auto bound = Detail::BindAndListen(resolver, bindAddress, port, backlog, /*extraTypeFlags*/ 0);
    if (bound.has_value())
        listener->_native = bound->socket;
    else
        listener->_bindError = std::move(bound).error();
    return listener;
}

BlockingListener::~BlockingListener()
{
    BlockingListener::Close();
}

void BlockingListener::Close() noexcept
{
    if (_native != Detail::InvalidSocket)
    {
        std::ignore = Detail::CloseNative(_native);
        _native = Detail::InvalidSocket;
    }
}

void BlockingListener::SetTimeouts(std::chrono::milliseconds acceptPoll, std::chrono::milliseconds ioTimeout) noexcept
{
    _ioTimeout = ioTimeout;
    // A receive timeout on the listening socket makes ::accept() return
    // periodically (POSIX honours SO_RCVTIMEO for accept), so the accept loop can
    // wake to re-check a shutdown flag: POSIX does NOT unblock a parked accept()
    // when another thread closes the socket. Windows ignores SO_RCVTIMEO for
    // accept, but there closesocket() does unblock a parked accept(), so a clean
    // shutdown works on both platforms.
    if (_native != Detail::InvalidSocket)
        Detail::SetIoTimeouts(_native, acceptPoll, std::chrono::milliseconds { 0 });
}

AcceptAwaitable BlockingListener::Accept()
{
    if (_native == Detail::InvalidSocket)
        return AcceptAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = _bindError }) };

    // sockaddr_storage holds either an IPv4 or IPv6 peer address without
    // truncation, since the listener may be bound to either family.
    sockaddr_storage client {};
#if defined(_WIN32)
    int addrLen = sizeof(client);
#else
    socklen_t addrLen = sizeof(client);
#endif
    auto const acceptedRaw = ::accept(static_cast<int>(_native), reinterpret_cast<sockaddr*>(&client), &addrLen);
    if (acceptedRaw == static_cast<std::remove_const_t<decltype(acceptedRaw)>>(Detail::InvalidSocket))
        return AcceptAwaitable { std::unexpected(MakeSystemError("accept")) };

    Detail::ApplyHotSocketOptions(static_cast<Detail::NativeSocket>(acceptedRaw));
    // Bound the request read so a stalled client cannot wedge a blocking recv()
    // (and so the single-threaded admin endpoint stays available under slowloris).
    if (_ioTimeout.count() > 0)
        Detail::SetIoTimeouts(static_cast<Detail::NativeSocket>(acceptedRaw), _ioTimeout, _ioTimeout);
    return AcceptAwaitable { AcceptResult {
        std::make_unique<BlockingSocket>(static_cast<Detail::NativeSocket>(acceptedRaw)) } };
}

} // namespace FastCache
