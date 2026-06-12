// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/KqueueSocket.hpp>

#if defined(__APPLE__)

    #include <FastCache/Async/KqueueReactor.hpp>
    #include <FastCache/Core/Errors/NetError.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/SocketAddress.hpp>

    #include <sys/socket.h>
    #include <sys/uio.h>

    #include <array>
    #include <cerrno>
    #include <cstddef>
    #include <cstdint>
    #include <cstring>
    #include <expected>
    #include <memory>
    #include <span>
    #include <string>
    #include <string_view>
    #include <tuple>
    #include <utility>
    #include <vector>

    #include <fcntl.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>

namespace FastCache
{

namespace
{

    [[nodiscard]] NetErrorCode TranslatePosix(int code) noexcept
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
            case EAGAIN:
                return NetErrorCode::WouldBlock;
            default:
                return NetErrorCode::SystemError;
        }
    }

    [[nodiscard]] NetError MakePosixError(int code, std::string ctx)
    {
        return NetError {
            .code = TranslatePosix(code),
            .systemCode = code,
            .context = std::move(ctx),
        };
    }

    void SetNonBlocking(int fd) noexcept
    {
        auto const flags = ::fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        auto const fdflags = ::fcntl(fd, F_GETFD, 0);
        if (fdflags >= 0)
            ::fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
    }

    /// Per-`sendmsg` iovec batch cap (see EpollSocket for the rationale).
    constexpr std::size_t MaxIovBatch = 64;

    /// Outcome of pushing a vectored-write cursor forward with `sendmsg`.
    enum class SendProgress : std::uint8_t
    {
        Completed,
        WouldBlock,
        Error,
    };

    /// Send segments from cursor `[segIndex, segOffset]` onward, advancing the
    /// cursor in place. macOS lacks `MSG_NOSIGNAL`; SIGPIPE is suppressed via
    /// the `SO_NOSIGPIPE` option applied at accept, so the flags stay 0.
    /// @param fd Socket file descriptor.
    /// @param segments Ordered payload segments.
    /// @param segIndex In/out: index of the first unsent segment.
    /// @param segOffset In/out: bytes already sent from `segments[segIndex]`.
    /// @param sentTotal In/out: running count of bytes sent across the op.
    /// @return Whether the cursor reached the end, would block, or errored.
    [[nodiscard]] SendProgress SendFromCursor(int fd,
                                              std::span<std::span<std::byte const> const> segments,
                                              std::size_t& segIndex,
                                              std::size_t& segOffset,
                                              std::size_t& sentTotal) noexcept
    {
        while (segIndex < segments.size())
        {
            std::array<iovec, MaxIovBatch> iov {};
            std::size_t count = 0;
            for (auto i = segIndex; i < segments.size() && count < MaxIovBatch; ++i)
            {
                auto const seg = segments[i];
                auto const skip = (i == segIndex) ? segOffset : std::size_t { 0 };
                if (seg.size() <= skip)
                    continue;
                iov[count].iov_base = const_cast<std::byte*>(seg.data() + skip);
                iov[count].iov_len = seg.size() - skip;
                ++count;
            }
            if (count == 0)
            {
                segIndex = segments.size();
                segOffset = 0;
                break;
            }

            msghdr msg {};
            msg.msg_iov = iov.data();
            msg.msg_iovlen = static_cast<int>(count);
            auto const wrote = ::sendmsg(fd, &msg, 0);
            if (wrote < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                    return SendProgress::WouldBlock;
                return SendProgress::Error;
            }

            sentTotal += static_cast<std::size_t>(wrote);
            auto advance = static_cast<std::size_t>(wrote);
            while (advance > 0 && segIndex < segments.size())
            {
                auto const seg = segments[segIndex];
                auto const avail = seg.size() - segOffset;
                if (advance < avail)
                {
                    segOffset += advance;
                    advance = 0;
                }
                else
                {
                    advance -= avail;
                    ++segIndex;
                    segOffset = 0;
                }
            }
        }
        return segIndex >= segments.size() ? SendProgress::Completed : SendProgress::WouldBlock;
    }

} // namespace

// -- KqueueSocket -----------------------------------------------------------

struct KqueueSocket::Impl
{
    KqueueReactor& reactor;
    KqueueFdHandler handler;

    struct Op
    {
        IoAwaitable* awaitable { nullptr };
        std::span<std::byte> readBuffer {};
        /// When true the read awaitable is a "wake on readability" probe
        /// (WaitReadable): OnReadable must complete it WITHOUT consuming any
        /// bytes from the socket. Mirrors EpollSocket's readPeekOnly.
        bool readPeekOnly { false };
        std::span<std::byte const> writeRemaining {};
        std::size_t writeTotal { 0 };

        // Vectored-write state (see EpollSocket::Impl::Op for the contract).
        std::vector<std::span<std::byte const>> writeSegments {};
        std::size_t writeSegIndex { 0 };
        std::size_t writeSegOffset { 0 };
        std::shared_ptr<void const> writeKeepAlive {};

        void ClearVectored() noexcept
        {
            writeSegments.clear();
            writeSegIndex = 0;
            writeSegOffset = 0;
            writeKeepAlive.reset();
            writeTotal = 0;
        }
    };

    Op readOp;
    Op writeOp;

    static void OnReadable(KqueueFdHandler* base);
    static void OnWritable(KqueueFdHandler* base);

    void UpdateInterest()
    {
        std::ignore = reactor.UpdateInterest(&handler, readOp.awaitable != nullptr, writeOp.awaitable != nullptr);
    }

    Impl(KqueueReactor& r, int fd):
        reactor { r }
    {
        handler.fd = fd;
        handler.owner = this;
        handler.onReadable = &OnReadable;
        handler.onWritable = &OnWritable;
    }
};

namespace
{

    KqueueSocket::Impl* ImplFromHandler(KqueueFdHandler* base) noexcept
    {
        return static_cast<KqueueSocket::Impl*>(base->owner);
    }

} // namespace

void KqueueSocket::Impl::OnReadable(KqueueFdHandler* base)
{
    auto* impl = ImplFromHandler(base);
    if (!impl->readOp.awaitable)
        return;
    auto* awaitable = impl->readOp.awaitable;
    // WaitReadable parked: complete without consuming any bytes. Used by
    // RESP3 subscribers, which need to wake on a readable edge but cannot
    // consume the client's command bytes here.
    if (impl->readOp.readPeekOnly)
    {
        impl->readOp.awaitable = nullptr;
        impl->readOp.readBuffer = {};
        impl->readOp.readPeekOnly = false;
        impl->UpdateInterest();
        awaitable->Complete(IoResult { std::size_t { 1 } });
        return;
    }
    auto buf = impl->readOp.readBuffer;
    auto const got = ::recv(impl->handler.fd, buf.data(), buf.size(), 0);
    if (got >= 0)
    {
        impl->readOp.awaitable = nullptr;
        impl->readOp.readBuffer = {};
        impl->UpdateInterest();
        awaitable->Complete(IoResult { static_cast<std::size_t>(got) });
        return;
    }
    if (errno == EAGAIN || errno == EINTR)
        return;
    impl->readOp.awaitable = nullptr;
    impl->readOp.readBuffer = {};
    impl->UpdateInterest();
    awaitable->Complete(std::unexpected(MakePosixError(errno, "recv")));
}

void KqueueSocket::Impl::OnWritable(KqueueFdHandler* base)
{
    auto* impl = ImplFromHandler(base);
    if (!impl->writeOp.awaitable)
        return;
    auto* awaitable = impl->writeOp.awaitable;

    // Vectored write in flight: drive the cursor with sendmsg.
    if (!impl->writeOp.writeSegments.empty())
    {
        auto const progress = SendFromCursor(impl->handler.fd,
                                             impl->writeOp.writeSegments,
                                             impl->writeOp.writeSegIndex,
                                             impl->writeOp.writeSegOffset,
                                             impl->writeOp.writeTotal);
        if (progress == SendProgress::WouldBlock)
            return;
        if (progress == SendProgress::Error)
        {
            impl->writeOp.awaitable = nullptr;
            impl->writeOp.ClearVectored();
            impl->UpdateInterest();
            awaitable->Complete(std::unexpected(MakePosixError(errno, "sendmsg")));
            return;
        }
        auto const total = impl->writeOp.writeTotal;
        impl->writeOp.awaitable = nullptr;
        impl->writeOp.ClearVectored();
        impl->UpdateInterest();
        awaitable->Complete(IoResult { total });
        return;
    }

    while (!impl->writeOp.writeRemaining.empty())
    {
        auto const wrote =
            ::send(impl->handler.fd, impl->writeOp.writeRemaining.data(), impl->writeOp.writeRemaining.size(), 0);
        if (wrote > 0)
        {
            impl->writeOp.writeRemaining = impl->writeOp.writeRemaining.subspan(static_cast<std::size_t>(wrote));
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        impl->writeOp.awaitable = nullptr;
        impl->writeOp.writeRemaining = {};
        impl->UpdateInterest();
        awaitable->Complete(std::unexpected(MakePosixError(errno, "send")));
        return;
    }
    auto const total = impl->writeOp.writeTotal;
    impl->writeOp.awaitable = nullptr;
    impl->writeOp.writeTotal = 0;
    impl->UpdateInterest();
    awaitable->Complete(IoResult { total });
}

KqueueSocket::KqueueSocket(KqueueReactor& reactor, int fd, std::string peerAddress) noexcept:
    _impl { std::make_unique<Impl>(reactor, fd) },
    _fd { fd },
    _peerAddress { std::move(peerAddress) }
{
    SetNonBlocking(fd);
    std::ignore = reactor.Attach(&_impl->handler);
}

KqueueSocket::~KqueueSocket()
{
    KqueueSocket::Close();
}

void KqueueSocket::Close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    if (_impl)
    {
        _impl->reactor.Detach(&_impl->handler);
        if (_impl->handler.fd >= 0)
        {
            ::close(_impl->handler.fd);
            _impl->handler.fd = -1;
        }
    }
    _fd = -1;
}

namespace
{

    void KqueueSocketAwaitableSuspended(IoAwaitable* self, std::coroutine_handle<> /*handle*/)
    {
        auto* op = static_cast<KqueueSocket::Impl::Op*>(self->CallbackState());
        op->awaitable = self;
    }

} // namespace

IoAwaitable KqueueSocket::Read(std::span<std::byte> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    auto const got = ::recv(_fd, buffer.data(), buffer.size(), 0);
    if (got >= 0)
        return IoAwaitable { IoResult { static_cast<std::size_t>(got) } };
    if (errno != EAGAIN && errno != EINTR)
        return IoAwaitable { std::unexpected(MakePosixError(errno, "recv")) };

    _impl->readOp.awaitable = nullptr;
    _impl->readOp.readBuffer = buffer;
    _impl->readOp.readPeekOnly = false;
    IoAwaitable a;
    a.SetSuspendCallback(&KqueueSocketAwaitableSuspended, &_impl->readOp);
    std::ignore = _impl->reactor.UpdateInterest(&_impl->handler, true, _impl->writeOp.awaitable != nullptr);
    return a;
}

IoAwaitable KqueueSocket::WaitReadable()
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // Probe readability without consuming bytes: zero-length MSG_PEEK returns 0
    // at EOF, >0 when data is pending, EAGAIN when nothing is ready. EOF/data
    // means "go ahead and Read"; only EAGAIN parks on EVFILT_READ.
    std::array<std::byte, 1> probe {};
    auto const got = ::recv(_fd, probe.data(), probe.size(), MSG_PEEK);
    if (got >= 0)
        return IoAwaitable { IoResult { std::size_t { 1 } } };
    if (errno != EAGAIN && errno != EINTR)
        return IoAwaitable { std::unexpected(MakePosixError(errno, "recv")) };

    _impl->readOp.awaitable = nullptr;
    _impl->readOp.readBuffer = {};
    _impl->readOp.readPeekOnly = true;
    IoAwaitable a;
    a.SetSuspendCallback(&KqueueSocketAwaitableSuspended, &_impl->readOp);
    std::ignore = _impl->reactor.UpdateInterest(&_impl->handler, true, _impl->writeOp.awaitable != nullptr);
    return a;
}

IoAwaitable KqueueSocket::Write(std::span<std::byte const> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    auto remaining = buffer;
    while (!remaining.empty())
    {
        auto const wrote = ::send(_fd, remaining.data(), remaining.size(), 0);
        if (wrote > 0)
        {
            remaining = remaining.subspan(static_cast<std::size_t>(wrote));
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EINTR))
            break;
        return IoAwaitable { std::unexpected(MakePosixError(errno, "send")) };
    }
    if (remaining.empty())
        return IoAwaitable { IoResult { buffer.size() } };

    _impl->writeOp.awaitable = nullptr;
    _impl->writeOp.writeRemaining = remaining;
    _impl->writeOp.writeTotal = buffer.size();
    IoAwaitable a;
    a.SetSuspendCallback(&KqueueSocketAwaitableSuspended, &_impl->writeOp);
    std::ignore = _impl->reactor.UpdateInterest(&_impl->handler, _impl->readOp.awaitable != nullptr, true);
    return a;
}

IoAwaitable KqueueSocket::WriteVectored(std::span<std::span<std::byte const> const> segments,
                                        std::shared_ptr<void const> keepAlive)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    std::vector<std::span<std::byte const>> owned { segments.begin(), segments.end() };
    std::size_t segIndex = 0;
    std::size_t segOffset = 0;
    std::size_t sent = 0;

    auto const progress = SendFromCursor(_fd, owned, segIndex, segOffset, sent);
    if (progress == SendProgress::Error)
        return IoAwaitable { std::unexpected(MakePosixError(errno, "sendmsg")) };
    if (progress == SendProgress::Completed)
        return IoAwaitable { IoResult { sent } };

    _impl->writeOp.awaitable = nullptr;
    _impl->writeOp.writeSegments = std::move(owned);
    _impl->writeOp.writeSegIndex = segIndex;
    _impl->writeOp.writeSegOffset = segOffset;
    _impl->writeOp.writeTotal = sent;
    _impl->writeOp.writeKeepAlive = std::move(keepAlive);
    IoAwaitable a;
    a.SetSuspendCallback(&KqueueSocketAwaitableSuspended, &_impl->writeOp);
    std::ignore = _impl->reactor.UpdateInterest(&_impl->handler, _impl->readOp.awaitable != nullptr, true);
    return a;
}

// -- KqueueListener ---------------------------------------------------------

struct KqueueListener::Impl
{
    KqueueReactor& reactor;
    KqueueFdHandler handler;
    AcceptAwaitable* pending { nullptr };
    std::string bindError;

    static void OnReadable(KqueueFdHandler* base);

    Impl(KqueueReactor& r):
        reactor { r }
    {
        handler.owner = this;
        handler.onReadable = &OnReadable;
    }
};

namespace
{

    KqueueListener::Impl* ListenerImplFromHandler(KqueueFdHandler* base) noexcept
    {
        return static_cast<KqueueListener::Impl*>(base->owner);
    }

    void ListenerAwaitableSuspended(AcceptAwaitable* self, std::coroutine_handle<> /*handle*/)
    {
        auto* impl = static_cast<KqueueListener::Impl*>(self->CallbackState());
        impl->pending = self;
    }

} // namespace

void KqueueListener::Impl::OnReadable(KqueueFdHandler* base)
{
    auto* impl = ListenerImplFromHandler(base);
    if (!impl->pending)
        return;

    sockaddr_storage client {};
    socklen_t len = sizeof(client);
    auto const fd = ::accept(impl->handler.fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd < 0)
    {
        if (errno == EAGAIN || errno == EINTR)
            return;
        auto* awaitable = impl->pending;
        impl->pending = nullptr;
        std::ignore = impl->reactor.UpdateInterest(&impl->handler, false, false);
        awaitable->Complete(std::unexpected(MakePosixError(errno, "accept")));
        return;
    }
    SetNonBlocking(fd);
    Detail::ApplyHotSocketOptions(static_cast<Detail::NativeSocket>(fd));
    auto* awaitable = impl->pending;
    impl->pending = nullptr;
    std::ignore = impl->reactor.UpdateInterest(&impl->handler, false, false);
    auto peer = FormatPeerAddress(Detail::EndpointFromSockaddr(&client, static_cast<std::uint32_t>(len)));
    awaitable->Complete(AcceptResult { std::make_unique<KqueueSocket>(impl->reactor, fd, std::move(peer)) });
}

KqueueListener::KqueueListener() noexcept = default;
KqueueListener::~KqueueListener() = default;

std::unique_ptr<KqueueListener> KqueueListener::Bind(KqueueReactor& reactor,
                                                     std::string_view bindAddress,
                                                     std::uint16_t port,
                                                     int backlog,
                                                     IAddressResolver& resolver,
                                                     ReusePort reusePort)
{
    std::unique_ptr<KqueueListener> listener { new KqueueListener {} };
    listener->_impl = std::make_unique<Impl>(reactor);

    // Shared resolve + create + bind + listen. macOS has no SOCK_NONBLOCK
    // socket-type flag, so the listening socket is switched to non-blocking
    // afterwards (matching how accepted sockets are handled).
    auto bound = Detail::BindAndListen(resolver, bindAddress, port, backlog, /*extraTypeFlags*/ 0, reusePort);
    if (!bound.has_value())
    {
        listener->_impl->bindError = std::move(bound).error();
        return listener;
    }

    auto const fd = static_cast<int>(bound->socket);
    SetNonBlocking(fd);
    listener->_impl->handler.fd = fd;
    return listener;
}

bool KqueueListener::IsBound() const noexcept
{
    return _impl && _impl->handler.fd >= 0;
}

std::string_view KqueueListener::BindError() const noexcept
{
    return _impl ? std::string_view { _impl->bindError } : std::string_view {};
}

void KqueueListener::Close() noexcept
{
    if (!_impl || _impl->handler.fd < 0)
        return;
    _impl->reactor.Detach(&_impl->handler);
    ::close(_impl->handler.fd);
    _impl->handler.fd = -1;
    if (_impl->pending)
    {
        auto* awaitable = _impl->pending;
        _impl->pending = nullptr;
        awaitable->Complete(std::unexpected(NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = {} }));
    }
}

AcceptAwaitable KqueueListener::Accept()
{
    if (!IsBound())
        return AcceptAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = std::string { BindError() } }) };

    sockaddr_storage client {};
    socklen_t len = sizeof(client);
    auto const fd = ::accept(_impl->handler.fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd >= 0)
    {
        SetNonBlocking(fd);
        auto peer = FormatPeerAddress(Detail::EndpointFromSockaddr(&client, static_cast<std::uint32_t>(len)));
        return AcceptAwaitable { AcceptResult { std::make_unique<KqueueSocket>(_impl->reactor, fd, std::move(peer)) } };
    }
    if (errno != EAGAIN && errno != EINTR)
        return AcceptAwaitable { std::unexpected(MakePosixError(errno, "accept")) };

    _impl->pending = nullptr;
    AcceptAwaitable a;
    a.SetSuspendCallback(&ListenerAwaitableSuspended, _impl.get());
    std::ignore = _impl->reactor.UpdateInterest(&_impl->handler, true, false);
    return a;
}

} // namespace FastCache

#endif // __APPLE__
