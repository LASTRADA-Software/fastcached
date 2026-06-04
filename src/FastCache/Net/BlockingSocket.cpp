// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/SocketAddress.hpp>

#include <atomic>
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

    #include <cerrno>
    #include <csignal>

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

    return AcceptAwaitable { AcceptResult {
        std::make_unique<BlockingSocket>(static_cast<Detail::NativeSocket>(acceptedRaw)) } };
}

} // namespace FastCache
