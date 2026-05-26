// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/BlockingSocket.hpp>

#include <FastCache/Core/Errors/NetError.hpp>

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

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <errno.h>
    #include <netinet/in.h>
    #include <signal.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace FastCache
{

namespace Detail
{

#if defined(_WIN32)

    namespace
    {
        std::atomic<bool> g_winsockInitialised { false };
        std::atomic<bool> g_winsockInitialising { false };

        [[nodiscard]] int LastNetworkError() noexcept { return WSAGetLastError(); }

        [[nodiscard]] NetErrorCode TranslateError(int code) noexcept
        {
            switch (code)
            {
                case WSAECONNRESET:    return NetErrorCode::ConnReset;
                case WSAECONNREFUSED:  return NetErrorCode::ConnRefused;
                case WSAEHOSTUNREACH:  return NetErrorCode::HostUnreach;
                case WSAEADDRINUSE:    return NetErrorCode::AddressInUse;
                case WSAEADDRNOTAVAIL: return NetErrorCode::AddressNotAvail;
                case WSAEACCES:        return NetErrorCode::PermissionDenied;
                case WSAEBADF:
                case WSAENOTSOCK:      return NetErrorCode::BadFileHandle;
                case WSAEINTR:         return NetErrorCode::Cancelled;
                default:               return NetErrorCode::SystemError;
            }
        }

        [[nodiscard]] int CloseNative(NativeSocket s) noexcept { return ::closesocket(static_cast<SOCKET>(s)); }
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

        [[nodiscard]] int LastNetworkError() noexcept { return errno; }

        [[nodiscard]] NetErrorCode TranslateError(int code) noexcept
        {
            switch (code)
            {
                case ECONNRESET:    return NetErrorCode::ConnReset;
                case ECONNREFUSED:  return NetErrorCode::ConnRefused;
                case EHOSTUNREACH:  return NetErrorCode::HostUnreach;
                case EADDRINUSE:    return NetErrorCode::AddressInUse;
                case EADDRNOTAVAIL: return NetErrorCode::AddressNotAvail;
                case EACCES:        return NetErrorCode::PermissionDenied;
                case EBADF:
                case ENOTSOCK:      return NetErrorCode::BadFileHandle;
                case EINTR:         return NetErrorCode::Cancelled;
                default:            return NetErrorCode::SystemError;
            }
        }

        [[nodiscard]] int CloseNative(NativeSocket s) noexcept { return ::close(s); }
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

BlockingSocket::BlockingSocket(Detail::NativeSocket native) noexcept: _native { native } {}

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
        Detail::CloseNative(_native);
        _native = Detail::InvalidSocket;
    }
}

IoAwaitable BlockingSocket::Read(std::span<std::byte> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(NetError { .code = NetErrorCode::BadFileHandle }) };

    auto const got = ::recv(static_cast<int>(_native),
                            reinterpret_cast<char*>(buffer.data()),
                            static_cast<int>(buffer.size()),
                            0);
    if (got < 0)
        return IoAwaitable { std::unexpected(MakeSystemError("recv")) };
    return IoAwaitable { IoResult { static_cast<std::size_t>(got) } };
}

IoAwaitable BlockingSocket::Write(std::span<std::byte const> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(NetError { .code = NetErrorCode::BadFileHandle }) };

    std::size_t written = 0;
    while (written < buffer.size())
    {
        auto const n = ::send(static_cast<int>(_native),
                              reinterpret_cast<char const*>(buffer.data()) + written,
                              static_cast<int>(buffer.size() - written),
                              0);
        if (n < 0)
            return IoAwaitable { std::unexpected(MakeSystemError("send")) };
        written += static_cast<std::size_t>(n);
    }
    return IoAwaitable { IoResult { written } };
}

// -- BlockingListener ------------------------------------------------------

std::unique_ptr<BlockingListener>
BlockingListener::Bind(std::string_view bindAddress, std::uint16_t port, int backlog)
{
    Detail::EnsureNetworkInitialised();

    std::unique_ptr<BlockingListener> listener { new BlockingListener {} };

    auto const sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 || sock == static_cast<int>(Detail::InvalidSocket))
    {
        listener->_bindError = std::format("socket() failed: {}", Detail::LastNetworkError());
        return listener;
    }

    // SO_REUSEADDR so restart-after-crash works on Linux.
    int reuse = 1;
    ::setsockopt(sock,
                 SOL_SOCKET,
                 SO_REUSEADDR,
                 reinterpret_cast<char const*>(&reuse),
                 sizeof(reuse));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string const addrCopy { bindAddress };
    if (::inet_pton(AF_INET, addrCopy.c_str(), &addr.sin_addr) != 1)
    {
        listener->_bindError = std::format("inet_pton({}) failed", addrCopy);
        Detail::CloseNative(static_cast<Detail::NativeSocket>(sock));
        return listener;
    }

    if (::bind(sock, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) != 0)
    {
        listener->_bindError = std::format("bind({}:{}) failed: {}", addrCopy, port, Detail::LastNetworkError());
        Detail::CloseNative(static_cast<Detail::NativeSocket>(sock));
        return listener;
    }

    if (::listen(sock, backlog) != 0)
    {
        listener->_bindError = std::format("listen() failed: {}", Detail::LastNetworkError());
        Detail::CloseNative(static_cast<Detail::NativeSocket>(sock));
        return listener;
    }

    listener->_native = static_cast<Detail::NativeSocket>(sock);
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
        Detail::CloseNative(_native);
        _native = Detail::InvalidSocket;
    }
}

AcceptAwaitable BlockingListener::Accept()
{
    if (_native == Detail::InvalidSocket)
        return AcceptAwaitable { std::unexpected(NetError {
            .code = NetErrorCode::BadFileHandle, .context = _bindError }) };

    sockaddr_in client {};
#if defined(_WIN32)
    int addrLen = sizeof(client);
#else
    socklen_t addrLen = sizeof(client);
#endif
    auto const acceptedRaw =
        ::accept(static_cast<int>(_native), reinterpret_cast<sockaddr*>(&client), &addrLen);
    if (acceptedRaw < 0 || acceptedRaw == static_cast<int>(Detail::InvalidSocket))
        return AcceptAwaitable { std::unexpected(MakeSystemError("accept")) };

    return AcceptAwaitable { AcceptResult {
        std::make_unique<BlockingSocket>(static_cast<Detail::NativeSocket>(acceptedRaw)) } };
}

} // namespace FastCache
