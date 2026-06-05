// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/EpollSocket.hpp>

#if defined(__linux__)

    #include <FastCache/Async/EpollReactor.hpp>
    #include <FastCache/Core/Errors/NetError.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/SocketAddress.hpp>

    #include <sys/socket.h>

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
    }

} // namespace

// -- EpollSocket ------------------------------------------------------------

struct EpollSocket::Impl
{
    EpollReactor& reactor;
    EpollFdHandler handler;

    struct Op
    {
        IoAwaitable* awaitable { nullptr };
        // For Read: a writable view we fill in-place.
        std::span<std::byte> readBuffer {};
        // For Write: bytes still to send.
        std::span<std::byte const> writeRemaining {};
        std::size_t writeTotal { 0 };
    };

    Op readOp;
    Op writeOp;

    static void OnReadable(EpollFdHandler* base);
    static void OnWritable(EpollFdHandler* base);

    void UpdateInterest()
    {
        std::ignore = reactor.UpdateInterest(&handler, readOp.awaitable != nullptr, writeOp.awaitable != nullptr);
    }

    Impl(EpollReactor& r, int fd):
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

    EpollSocket::Impl* ImplFromHandler(EpollFdHandler* base) noexcept
    {
        return static_cast<EpollSocket::Impl*>(base->owner);
    }

} // namespace

void EpollSocket::Impl::OnReadable(EpollFdHandler* base)
{
    auto* impl = ImplFromHandler(base);
    if (!impl->readOp.awaitable)
        return;

    auto buf = impl->readOp.readBuffer;
    auto* awaitable = impl->readOp.awaitable;
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
        return; // wait for the next readable event
    impl->readOp.awaitable = nullptr;
    impl->readOp.readBuffer = {};
    impl->UpdateInterest();
    awaitable->Complete(std::unexpected(MakePosixError(errno, "recv")));
}

void EpollSocket::Impl::OnWritable(EpollFdHandler* base)
{
    auto* impl = ImplFromHandler(base);
    if (!impl->writeOp.awaitable)
        return;

    auto* awaitable = impl->writeOp.awaitable;
    while (!impl->writeOp.writeRemaining.empty())
    {
        auto const wrote =
            ::send(impl->handler.fd, impl->writeOp.writeRemaining.data(), impl->writeOp.writeRemaining.size(), MSG_NOSIGNAL);
        if (wrote > 0)
        {
            impl->writeOp.writeRemaining = impl->writeOp.writeRemaining.subspan(static_cast<std::size_t>(wrote));
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EINTR))
            return; // wait for the next writable event
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

EpollSocket::EpollSocket(EpollReactor& reactor, int fd) noexcept:
    _impl { std::make_unique<Impl>(reactor, fd) },
    _fd { fd }
{
    SetNonBlocking(fd);
    std::ignore = reactor.Attach(&_impl->handler);
}

EpollSocket::~EpollSocket()
{
    EpollSocket::Close();
}

void EpollSocket::Close() noexcept
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

    void EpollSocketAwaitableSuspended(IoAwaitable* self, std::coroutine_handle<> /*handle*/)
    {
        auto* op = static_cast<EpollSocket::Impl::Op*>(self->CallbackState());
        op->awaitable = self;
    }

} // namespace

IoAwaitable EpollSocket::Read(std::span<std::byte> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // Fast path: try recv synchronously.
    auto const got = ::recv(_fd, buffer.data(), buffer.size(), 0);
    if (got >= 0)
        return IoAwaitable { IoResult { static_cast<std::size_t>(got) } };
    if (errno != EAGAIN && errno != EINTR)
        return IoAwaitable { std::unexpected(MakePosixError(errno, "recv")) };

    // Park: arm EPOLLIN and wait for the reactor to deliver readability.
    _impl->readOp.awaitable = nullptr;
    _impl->readOp.readBuffer = buffer;
    IoAwaitable a;
    a.SetSuspendCallback(&EpollSocketAwaitableSuspended, &_impl->readOp);
    std::ignore =
        _impl->reactor.UpdateInterest(&_impl->handler, /*read*/ true, /*write*/ _impl->writeOp.awaitable != nullptr);
    return a;
}

IoAwaitable EpollSocket::Write(std::span<std::byte const> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // Try send synchronously; cycle until EAGAIN or completion.
    auto remaining = buffer;
    while (!remaining.empty())
    {
        auto const wrote = ::send(_fd, remaining.data(), remaining.size(), MSG_NOSIGNAL);
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

    // Park.
    _impl->writeOp.awaitable = nullptr;
    _impl->writeOp.writeRemaining = remaining;
    _impl->writeOp.writeTotal = buffer.size();
    IoAwaitable a;
    a.SetSuspendCallback(&EpollSocketAwaitableSuspended, &_impl->writeOp);
    std::ignore =
        _impl->reactor.UpdateInterest(&_impl->handler, /*read*/ _impl->readOp.awaitable != nullptr, /*write*/ true);
    return a;
}

// -- EpollListener ----------------------------------------------------------

struct EpollListener::Impl
{
    EpollReactor& reactor;
    EpollFdHandler handler;
    AcceptAwaitable* pending { nullptr };
    std::string bindError;

    static void OnReadable(EpollFdHandler* base);

    Impl(EpollReactor& r):
        reactor { r }
    {
        handler.owner = this;
        handler.onReadable = &OnReadable;
    }
};

namespace
{

    EpollListener::Impl* ListenerImplFromHandler(EpollFdHandler* base) noexcept
    {
        return static_cast<EpollListener::Impl*>(base->owner);
    }

    void ListenerAwaitableSuspended(AcceptAwaitable* self, std::coroutine_handle<> /*handle*/)
    {
        auto* impl = static_cast<EpollListener::Impl*>(self->CallbackState());
        impl->pending = self;
    }

} // namespace

void EpollListener::Impl::OnReadable(EpollFdHandler* base)
{
    auto* impl = ListenerImplFromHandler(base);
    if (!impl->pending)
        return;

    sockaddr_storage client {};
    socklen_t len = sizeof(client);
    auto const fd = ::accept4(impl->handler.fd, reinterpret_cast<sockaddr*>(&client), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
    {
        if (errno == EAGAIN || errno == EINTR)
            return;
        auto* awaitable = impl->pending;
        impl->pending = nullptr;
        std::ignore = impl->reactor.UpdateInterest(&impl->handler, false, false);
        awaitable->Complete(std::unexpected(MakePosixError(errno, "accept4")));
        return;
    }
    Detail::ApplyHotSocketOptions(static_cast<Detail::NativeSocket>(fd));
    auto* awaitable = impl->pending;
    impl->pending = nullptr;
    std::ignore = impl->reactor.UpdateInterest(&impl->handler, false, false);
    awaitable->Complete(AcceptResult { std::make_unique<EpollSocket>(impl->reactor, fd) });
}

EpollListener::EpollListener() noexcept = default;
EpollListener::~EpollListener() = default;

std::unique_ptr<EpollListener> EpollListener::Bind(
    EpollReactor& reactor, std::string_view bindAddress, std::uint16_t port, int backlog, IAddressResolver& resolver)
{
    std::unique_ptr<EpollListener> listener { new EpollListener {} };
    listener->_impl = std::make_unique<Impl>(reactor);

    // Shared resolve + create + bind + listen; epoll wants the accept socket
    // (and thus the listener) non-blocking and close-on-exec.
    auto bound = Detail::BindAndListen(resolver, bindAddress, port, backlog, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (!bound.has_value())
    {
        listener->_impl->bindError = std::move(bound).error();
        return listener;
    }

    auto const fd = static_cast<int>(bound->socket);
    listener->_impl->handler.fd = fd;
    if (!reactor.Attach(&listener->_impl->handler))
    {
        listener->_impl->bindError = "epoll_ctl ADD failed";
        ::close(fd);
        listener->_impl->handler.fd = -1;
        return listener;
    }

    return listener;
}

bool EpollListener::IsBound() const noexcept
{
    return _impl && _impl->handler.fd >= 0;
}

std::string_view EpollListener::BindError() const noexcept
{
    return _impl ? std::string_view { _impl->bindError } : std::string_view {};
}

void EpollListener::Close() noexcept
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

AcceptAwaitable EpollListener::Accept()
{
    if (!IsBound())
        return AcceptAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = std::string { BindError() } }) };

    // Fast path: try accept synchronously.
    sockaddr_storage client {};
    socklen_t len = sizeof(client);
    auto const fd = ::accept4(_impl->handler.fd, reinterpret_cast<sockaddr*>(&client), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd >= 0)
        return AcceptAwaitable { AcceptResult { std::make_unique<EpollSocket>(_impl->reactor, fd) } };
    if (errno != EAGAIN && errno != EINTR)
        return AcceptAwaitable { std::unexpected(MakePosixError(errno, "accept4")) };

    _impl->pending = nullptr;
    AcceptAwaitable a;
    a.SetSuspendCallback(&ListenerAwaitableSuspended, _impl.get());
    std::ignore = _impl->reactor.UpdateInterest(&_impl->handler, /*read*/ true, /*write*/ false);
    return a;
}

} // namespace FastCache

#endif // __linux__
