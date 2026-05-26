// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/KqueueSocket.hpp>

#if defined(__APPLE__)

    #include <FastCache/Async/KqueueReactor.hpp>
    #include <FastCache/Core/Errors/NetError.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>

    #include <sys/socket.h>

    #include <cstddef>
    #include <cstdint>
    #include <cstring>
    #include <expected>
    #include <memory>
    #include <span>
    #include <string>
    #include <string_view>
    #include <utility>

    #include <errno.h>
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
        std::span<std::byte const> writeRemaining {};
        std::size_t writeTotal { 0 };
    };

    Op readOp;
    Op writeOp;

    static void OnReadable(KqueueFdHandler* base);
    static void OnWritable(KqueueFdHandler* base);

    void UpdateInterest()
    {
        reactor.UpdateInterest(&handler, readOp.awaitable != nullptr, writeOp.awaitable != nullptr);
    }

    Impl(KqueueReactor& r, int fd):
        reactor { r }
    {
        handler.fd = fd;
        handler.onReadable = &OnReadable;
        handler.onWritable = &OnWritable;
    }
};

namespace
{

    KqueueSocket::Impl* ImplFromHandler(KqueueFdHandler* base) noexcept
    {
        return reinterpret_cast<KqueueSocket::Impl*>(reinterpret_cast<char*>(base) - offsetof(KqueueSocket::Impl, handler));
    }

} // namespace

void KqueueSocket::Impl::OnReadable(KqueueFdHandler* base)
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

KqueueSocket::KqueueSocket(KqueueReactor& reactor, int fd) noexcept:
    _impl { std::make_unique<Impl>(reactor, fd) },
    _fd { fd }
{
    SetNonBlocking(fd);
    reactor.Attach(&_impl->handler);
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
    IoAwaitable a;
    a.SetSuspendCallback(&KqueueSocketAwaitableSuspended, &_impl->readOp);
    _impl->reactor.UpdateInterest(&_impl->handler, true, _impl->writeOp.awaitable != nullptr);
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
    _impl->reactor.UpdateInterest(&_impl->handler, _impl->readOp.awaitable != nullptr, true);
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
        handler.onReadable = &OnReadable;
    }
};

namespace
{

    KqueueListener::Impl* ListenerImplFromHandler(KqueueFdHandler* base) noexcept
    {
        return reinterpret_cast<KqueueListener::Impl*>(reinterpret_cast<char*>(base)
                                                       - offsetof(KqueueListener::Impl, handler));
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

    sockaddr_in client {};
    socklen_t len = sizeof(client);
    auto const fd = ::accept(impl->handler.fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd < 0)
    {
        if (errno == EAGAIN || errno == EINTR)
            return;
        auto* awaitable = impl->pending;
        impl->pending = nullptr;
        impl->reactor.UpdateInterest(&impl->handler, false, false);
        awaitable->Complete(std::unexpected(MakePosixError(errno, "accept")));
        return;
    }
    SetNonBlocking(fd);
    auto* awaitable = impl->pending;
    impl->pending = nullptr;
    impl->reactor.UpdateInterest(&impl->handler, false, false);
    awaitable->Complete(AcceptResult { std::make_unique<KqueueSocket>(impl->reactor, fd) });
}

KqueueListener::KqueueListener() noexcept = default;
KqueueListener::~KqueueListener() = default;

std::unique_ptr<KqueueListener> KqueueListener::Bind(KqueueReactor& reactor,
                                                     std::string_view bindAddress,
                                                     std::uint16_t port,
                                                     int backlog)
{
    Detail::EnsureNetworkInitialised();

    std::unique_ptr<KqueueListener> listener { new KqueueListener {} };
    listener->_impl = std::make_unique<Impl>(reactor);

    auto fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        listener->_impl->bindError = "socket() failed";
        return listener;
    }
    SetNonBlocking(fd);

    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    std::string const addrCopy { bindAddress };
    if (::inet_pton(AF_INET, addrCopy.c_str(), &addr.sin_addr) != 1)
    {
        listener->_impl->bindError = "inet_pton failed";
        ::close(fd);
        return listener;
    }
    if (::bind(fd, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) != 0)
    {
        listener->_impl->bindError = "bind failed";
        ::close(fd);
        return listener;
    }
    if (::listen(fd, backlog) != 0)
    {
        listener->_impl->bindError = "listen failed";
        ::close(fd);
        return listener;
    }
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

    sockaddr_in client {};
    socklen_t len = sizeof(client);
    auto const fd = ::accept(_impl->handler.fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd >= 0)
    {
        SetNonBlocking(fd);
        return AcceptAwaitable { AcceptResult { std::make_unique<KqueueSocket>(_impl->reactor, fd) } };
    }
    if (errno != EAGAIN && errno != EINTR)
        return AcceptAwaitable { std::unexpected(MakePosixError(errno, "accept")) };

    _impl->pending = nullptr;
    AcceptAwaitable a;
    a.SetSuspendCallback(&ListenerAwaitableSuspended, _impl.get());
    _impl->reactor.UpdateInterest(&_impl->handler, true, false);
    return a;
}

} // namespace FastCache

#endif // __APPLE__
