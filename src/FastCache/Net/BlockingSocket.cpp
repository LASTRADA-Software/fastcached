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
        std::atomic<bool> winsockInitialised { false };
        std::atomic<bool> winsockInitialising { false };

        [[nodiscard]] NetErrorCode TranslateErrorImpl(int code) noexcept
        {
            switch (code)
            {
                case WSAECONNRESET:
                    return NetErrorCode::ConnReset;
                case WSAECONNREFUSED:
                    return NetErrorCode::ConnRefused;
                case WSAEHOSTUNREACH:
                // A route that does not exist and a host that does not answer are
                // one category here: both mean this endpoint is unreachable from
                // where we are, and neither is retryable at this layer.
                case WSAENETUNREACH:
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

        /// Extra `::send` flags needed to keep a write to a broken pipe from
        /// raising a signal. Windows has no SIGPIPE, so there is nothing to add.
        constexpr int SendFlags = 0;
    } // namespace

    void ArmNoSigPipe(NativeSocket /*socket*/) noexcept
    {
        // Windows has no SIGPIPE: a write to a broken pipe is reported through
        // WSAGetLastError like any other failure.
    }

    void EnsureNetworkInitialised()
    {
        if (winsockInitialised.load(std::memory_order_acquire))
            return;
        bool expected = false;
        if (!winsockInitialising.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            // Another thread is busy initialising; spin briefly.
            while (!winsockInitialised.load(std::memory_order_acquire))
            {
            }
            return;
        }
        WSADATA data {};
        WSAStartup(MAKEWORD(2, 2), &data);
        winsockInitialised.store(true, std::memory_order_release);
    }

#else

    namespace
    {
        [[nodiscard]] NetErrorCode TranslateErrorImpl(int code) noexcept
        {
            switch (code)
            {
                case ECONNRESET:
                    return NetErrorCode::ConnReset;
                case ECONNREFUSED:
                    return NetErrorCode::ConnRefused;
                case EHOSTUNREACH:
                // A route that does not exist and a host that does not answer are
                // one category here: both mean this endpoint is unreachable from
                // where we are, and neither is retryable at this layer.
                case ENETUNREACH:
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

        // Writing to a socket whose peer has closed raises SIGPIPE, and SIGPIPE's
        // default disposition terminates the process. Suppressed PER SOCKET, and
        // that is the whole content of this block: the obvious spelling is one
        // `::signal(SIGPIPE, SIG_IGN)` at start-up, which is what this file used to
        // do, and it is wrong for any process that also spawns a child. An ignored
        // disposition is *inherited across exec*, so a process-wide ignore silently
        // changes how every program the process launches behaves --
        // `fastcache-compile-node` links this library, opens sockets and then runs a
        // compiler per job, so it was handing every one of those compilers a SIGPIPE
        // disposition they never asked for. The suppression belongs to the socket
        // that needs it.
        //
        // Which mechanism, in order of preference:
        //   * `SO_NOSIGPIPE` (macOS, the BSDs) -- a socket option set once, so every
        //     later send is covered without threading a flag through. Present since
        //     macOS 10.2, so it is the safe choice there even on an SDK new enough
        //     to also declare MSG_NOSIGNAL: that macro comes from the SDK headers
        //     while `CMAKE_OSX_DEPLOYMENT_TARGET` lets the binary run on an older
        //     kernel, and a flag the kernel does not know fails the send.
        //   * `MSG_NOSIGNAL` (Linux, Solaris, AIX) -- a per-send flag; there is no
        //     socket option to set.
        //   * neither: fall back to the process-wide ignore, with the cost above.
        //     No platform this builds on lands here.
    #if defined(SO_NOSIGPIPE)
        constexpr int SendFlags = 0;
    #elif defined(MSG_NOSIGNAL)
        constexpr int SendFlags = MSG_NOSIGNAL;
    #else
        constexpr int SendFlags = 0;
    #endif
    } // namespace

    void ArmNoSigPipe([[maybe_unused]] NativeSocket socket) noexcept
    {
    #if defined(SO_NOSIGPIPE)
        int const on = 1;
        std::ignore = ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    #elif defined(MSG_NOSIGNAL)
            // Nothing to arm: the suppression rides on each `::send` instead, through
            // `SendFlags`. A reactor socket that does its own sending has to pass that
            // flag itself -- `EpollSocket` does.
    #else
        ::signal(SIGPIPE, SIG_IGN);
    #endif
    }

    void EnsureNetworkInitialised()
    {
        // Nothing to do: BSD sockets need no process-wide start-up, and SIGPIPE is
        // suppressed per socket by ArmNoSigPipe above rather than by a disposition
        // this process would then leak to every child it execs. The function still
        // exists because Winsock genuinely does need it, and a caller that had to
        // ask which platform it was on would be the defect this seam removes.
    }

#endif

    int LastNetworkError() noexcept
    {
#if defined(_WIN32)
        return WSAGetLastError();
#else
        return errno;
#endif
    }

    NetErrorCode TranslateSocketError(int code) noexcept
    {
        return TranslateErrorImpl(code);
    }

    NetError MakeNetError(int code, std::string context)
    {
        return NetError { .code = TranslateSocketError(code), .systemCode = code, .context = std::move(context) };
    }

} // namespace Detail

namespace
{
    [[nodiscard]] NetError MakeSystemError(std::string_view context)
    {
        return Detail::MakeNetError(Detail::LastNetworkError(), std::string { context });
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

BlockingSocket::BlockingSocket(Detail::NativeSocket native, std::string peerAddress) noexcept:
    _native { native },
    _peerAddress { std::move(peerAddress) }
{
    // Both routes to a connected socket -- BlockingListener::Accept and
    // BlockingConnector::Connect -- construct through here, so this is the one
    // place the per-socket SIGPIPE suppression has to be applied.
    Detail::ArmNoSigPipe(_native);
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
                              Detail::SendFlags);
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
                                  Detail::SendFlags);
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

std::unique_ptr<BlockingListener> BlockingListener::Adopt(Detail::NativeSocket native)
{
    std::unique_ptr<BlockingListener> listener { new BlockingListener {} };
    listener->_native = native;
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

std::uint16_t BlockingListener::BoundPort() const noexcept
{
    if (_native == Detail::InvalidSocket)
        return 0;

    sockaddr_storage storage {};
#if defined(_WIN32)
    auto length = static_cast<int>(sizeof(storage));
    auto const handle = static_cast<SOCKET>(_native);
#else
    auto length = static_cast<socklen_t>(sizeof(storage));
    auto const handle = static_cast<int>(_native);
#endif
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
        return 0;

    // Read the port out of whichever family the socket actually is: the two
    // sockaddr layouts put it at different offsets, and reading the wrong one
    // yields a plausible-looking number rather than an error.
    if (storage.ss_family == AF_INET)
        return ntohs(reinterpret_cast<sockaddr_in const*>(&storage)->sin_port);
    if (storage.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<sockaddr_in6 const*>(&storage)->sin6_port);
    return 0;
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
    auto peer = FormatPeerAddress(Detail::EndpointFromSockaddr(&client, static_cast<std::uint32_t>(addrLen)));
    return AcceptAwaitable { AcceptResult {
        std::make_unique<BlockingSocket>(static_cast<Detail::NativeSocket>(acceptedRaw), std::move(peer)) } };
}

} // namespace FastCache
