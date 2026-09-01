// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/EpollSocket.hpp>

#if defined(__linux__)

    #include <FastCache/Async/EpollReactor.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/NetError.hpp>
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

    /// Non-blocking plus close-on-exec, for a descriptor this reactor will own.
    ///
    /// Both halves come from `Detail`. They used to be a local copy here and
    /// another in `KqueueSocket.cpp`, and the two had already drifted -- only the
    /// kqueue one set FD_CLOEXEC -- which is the drift a single definition
    /// exists to stop. Close-on-exec matters because a process that both listens
    /// and spawns children (the compile node spawns a compiler per job) otherwise
    /// hands each child every open connection.
    void PrepareOwnedFd(int fd) noexcept
    {
        std::ignore = Detail::SetNonBlocking(static_cast<Detail::NativeSocket>(fd));
        Detail::ArmCloseOnExec(static_cast<Detail::NativeSocket>(fd));
    }

    /// Per-`sendmsg` iovec batch cap. `IOV_MAX` is the kernel limit (1024 on
    /// Linux); a single GET reply needs only a handful, but a multi-key `get`
    /// can exceed it, so the send loop batches and re-issues.
    constexpr std::size_t MaxIovBatch = 64;

    /// Outcome of pushing a vectored-write cursor forward with `sendmsg`.
    enum class SendProgress : std::uint8_t
    {
        Completed,  ///< Every segment fully sent.
        WouldBlock, ///< Kernel buffer full (EAGAIN); arm EPOLLOUT and retry.
        Error,      ///< Fatal send error; `errno` carries the cause.
    };

    /// Send as much of the segments from the cursor `[segIndex, segOffset]`
    /// onward as the kernel accepts, advancing the cursor in place. Coalesces
    /// up to `MaxIovBatch` segments per `sendmsg` and loops until the kernel
    /// blocks, errors, or all bytes are gone.
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
                // A zero-length segment carries no bytes; skip it so it never
                // occupies an iovec slot (and is stepped over by the advance
                // loop below once the batch is sent).
                if (seg.size() <= skip)
                    continue;
                iov[count].iov_base = const_cast<std::byte*>(seg.data() + skip);
                iov[count].iov_len = seg.size() - skip;
                ++count;
            }
            if (count == 0)
            {
                // Only empty/exhausted segments remained in this window; step
                // the cursor past them and continue (or finish).
                segIndex = segments.size();
                segOffset = 0;
                break;
            }

            msghdr msg {};
            msg.msg_iov = iov.data();
            msg.msg_iovlen = count;
            auto const wrote = ::sendmsg(fd, &msg, MSG_NOSIGNAL);
            if (wrote < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                    return SendProgress::WouldBlock;
                return SendProgress::Error;
            }

            sentTotal += static_cast<std::size_t>(wrote);
            // Advance the cursor by `wrote` bytes across the segment list.
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
        // For WaitReadable: arm EPOLLIN but do NOT consume bytes when readable —
        // just complete the awaitable so the caller can decide to Read or not.
        bool readPeekOnly { false };
        // For a scalar Write: bytes still to send.
        std::span<std::byte const> writeRemaining {};
        std::size_t writeTotal { 0 };

        // For a vectored Write (WriteVectored): an owned copy of the segment
        // list (so the caller's `segments` span need not outlive the call),
        // plus a cursor into it. The cursor names the first not-yet-fully-sent
        // segment and the byte offset already consumed within it.
        std::vector<std::span<std::byte const>> writeSegments {};
        std::size_t writeSegIndex { 0 };
        std::size_t writeSegOffset { 0 };
        // Owner pinning the segments' backing bytes for the op's lifetime
        // (e.g. the GetResult holding the reference-counted value). Released
        // when the op completes.
        std::shared_ptr<void const> writeKeepAlive {};

        /// @return True once every vectored segment has been fully sent.
        [[nodiscard]] bool VectoredDone() const noexcept
        {
            return writeSegIndex >= writeSegments.size();
        }

        /// Reset all write state after a vectored op completes or fails.
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

    // WaitReadable: the fd is readable; report readiness without consuming.
    if (impl->readOp.readPeekOnly)
    {
        auto* const peekAwaitable = impl->readOp.awaitable;
        impl->readOp.awaitable = nullptr;
        impl->readOp.readPeekOnly = false;
        impl->UpdateInterest();
        peekAwaitable->Complete(IoResult { std::size_t { 1 } });
        return;
    }

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

    // Vectored write in flight: drive the cursor with sendmsg.
    if (!impl->writeOp.writeSegments.empty())
    {
        auto const progress = SendFromCursor(impl->handler.fd,
                                             impl->writeOp.writeSegments,
                                             impl->writeOp.writeSegIndex,
                                             impl->writeOp.writeSegOffset,
                                             impl->writeOp.writeTotal);
        if (progress == SendProgress::WouldBlock)
            return; // wait for the next writable event
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

EpollSocket::EpollSocket(EpollReactor& reactor, int fd, std::string peerAddress) noexcept:
    _impl { std::make_unique<Impl>(reactor, fd) },
    _fd { fd },
    _peerAddress { std::move(peerAddress) }
{
    PrepareOwnedFd(fd);
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
        // Fail whatever was parked. The interest is gone, so nothing will ever
        // complete these awaitables; leaving them set leaks the suspended
        // coroutine on abrupt teardown.
        //
        // Detached from the ops FIRST and completed afterwards, which is not
        // tidiness: `Complete` resumes the parked coroutine, and a coroutine that
        // owns this socket -- `ServePeer` holds it in a by-value `unique_ptr`
        // parameter -- runs to its end and destroys it before `Complete` returns.
        // So `this`, `_impl` and everything in it are freed part-way through this
        // loop. Completing inside it read `_impl->writeOp` after the free, which
        // ASan reports as a heap-use-after-free from `RaftPeerServer::Shutdown`;
        // it went unseen for as long as the sanitizer preset was silently building
        // without sanitizers.
        std::array<IoAwaitable*, 2> parked { nullptr, nullptr };
        std::size_t parkedCount = 0;
        for (auto* op: { &_impl->readOp, &_impl->writeOp })
        {
            if (op->awaitable == nullptr)
                continue;
            parked.at(parkedCount++) = op->awaitable;
            op->awaitable = nullptr;
            op->readBuffer = {};
            op->writeRemaining = {};
            op->ClearVectored();
        }
        _fd = -1;

        // Past this point nothing may touch a member. The two awaitables live in
        // their own coroutines' frames rather than in `_impl`, so they stay valid
        // even once the first resume has taken the socket down.
        for (auto* awaitable: parked)
            if (awaitable != nullptr)
                awaitable->Complete(
                    std::unexpected(NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = {} }));
        return;
    }
    _fd = -1;
}

namespace
{

    /// The awaitable to return when the reactor refuses to arm interest for an
    /// operation that is about to park.
    ///
    /// Parking on interest the kernel did not register is unrecoverable: the
    /// event never fires, so the coroutine never resumes and the connection
    /// stalls forever holding whatever it had left to transfer. A reported
    /// error tears the connection down instead, which the peer can observe.
    /// @return An awaitable already resolved to a NetError.
    [[nodiscard]] IoAwaitable ArmFailed()
    {
        return IoAwaitable { std::unexpected(NetError {
            .code = NetErrorCode::SystemError, .systemCode = errno, .context = "epoll_ctl: could not arm interest" }) };
    }

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
    _impl->readOp.readPeekOnly = false;
    IoAwaitable a;
    a.SetSuspendCallback(&EpollSocketAwaitableSuspended, &_impl->readOp);
    if (!_impl->reactor.UpdateInterest(&_impl->handler, /*read*/ true, /*write*/ _impl->writeOp.awaitable != nullptr))
    {
        _impl->readOp.readBuffer = {};
        return ArmFailed();
    }
    return a;
}

IoAwaitable EpollSocket::WaitReadable()
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // Probe readability with a zero-length MSG_PEEK recv: returns 0 at EOF, >0
    // when data is pending, or EAGAIN when nothing is ready yet. Either of the
    // first two means "go ahead and Read"; only EAGAIN parks on EPOLLIN.
    std::array<std::byte, 1> probe {};
    auto const got = ::recv(_fd, probe.data(), probe.size(), MSG_PEEK);
    if (got >= 0)
        return IoAwaitable { IoResult { std::size_t { 1 } } };
    if (errno != EAGAIN && errno != EINTR)
        return IoAwaitable { std::unexpected(MakePosixError(errno, "recv")) };

    // Park: arm EPOLLIN, completing without consuming when readable.
    _impl->readOp.awaitable = nullptr;
    _impl->readOp.readBuffer = {};
    _impl->readOp.readPeekOnly = true;
    IoAwaitable a;
    a.SetSuspendCallback(&EpollSocketAwaitableSuspended, &_impl->readOp);
    if (!_impl->reactor.UpdateInterest(&_impl->handler, /*read*/ true, /*write*/ _impl->writeOp.awaitable != nullptr))
    {
        _impl->readOp.readPeekOnly = false;
        return ArmFailed();
    }
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
    // Drop any vectored state before parking a scalar write: OnWritable checks
    // writeSegments first, so a leftover cursor would make it re-send the
    // previous operation's bytes and then report THIS buffer's size as sent.
    _impl->writeOp.ClearVectored();
    _impl->writeOp.writeRemaining = remaining;
    _impl->writeOp.writeTotal = buffer.size();
    IoAwaitable a;
    a.SetSuspendCallback(&EpollSocketAwaitableSuspended, &_impl->writeOp);
    if (!_impl->reactor.UpdateInterest(&_impl->handler, /*read*/ _impl->readOp.awaitable != nullptr, /*write*/ true))
    {
        _impl->writeOp.writeRemaining = {};
        _impl->writeOp.writeTotal = 0;
        return ArmFailed();
    }
    return a;
}

IoAwaitable EpollSocket::WriteVectored(std::span<std::span<std::byte const> const> segments,
                                       std::shared_ptr<void const> keepAlive)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // Own a copy of the segment list so the caller's `segments` span need not
    // outlive this call; the owned vector lives in writeOp until completion.
    std::vector<std::span<std::byte const>> owned { segments.begin(), segments.end() };
    std::size_t segIndex = 0;
    std::size_t segOffset = 0;
    std::size_t sent = 0;

    // Fast path: try to drain synchronously without parking.
    auto const progress = SendFromCursor(_fd, owned, segIndex, segOffset, sent);
    if (progress == SendProgress::Error)
        return IoAwaitable { std::unexpected(MakePosixError(errno, "sendmsg")) };
    if (progress == SendProgress::Completed)
        return IoAwaitable { IoResult { sent } };

    // Park: stash the cursor + keep-alive and arm EPOLLOUT.
    _impl->writeOp.awaitable = nullptr;
    // Symmetric to the scalar path: a stale writeRemaining must not outlive the
    // operation that set it.
    _impl->writeOp.writeRemaining = {};
    _impl->writeOp.writeSegments = std::move(owned);
    _impl->writeOp.writeSegIndex = segIndex;
    _impl->writeOp.writeSegOffset = segOffset;
    _impl->writeOp.writeTotal = sent;
    _impl->writeOp.writeKeepAlive = std::move(keepAlive);
    IoAwaitable a;
    a.SetSuspendCallback(&EpollSocketAwaitableSuspended, &_impl->writeOp);
    if (!_impl->reactor.UpdateInterest(&_impl->handler, /*read*/ _impl->readOp.awaitable != nullptr, /*write*/ true))
    {
        _impl->writeOp.ClearVectored();
        return ArmFailed();
    }
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
    auto peer = FormatPeerAddress(Detail::EndpointFromSockaddr(&client, static_cast<std::uint32_t>(len)));
    awaitable->Complete(AcceptResult { std::make_unique<EpollSocket>(impl->reactor, fd, std::move(peer)) });
}

EpollListener::EpollListener() noexcept = default;

EpollListener::~EpollListener()
{
    // The listener owns its descriptor and, while attached, is the `data.ptr`
    // the kernel hands back for it. Leaving both to a `= default` destructor
    // leaked the fd AND left the epoll set pointing at freed memory, so a
    // connection arriving after the listener went away resumed through a
    // dangling handler. `Close()` is idempotent, so an explicit close first --
    // which is what every current owner does -- costs nothing here.
    EpollListener::Close();
}

std::unique_ptr<EpollListener> EpollListener::Bind(EpollReactor& reactor,
                                                   std::string_view bindAddress,
                                                   std::uint16_t port,
                                                   int backlog,
                                                   IAddressResolver& resolver,
                                                   ReusePort reusePort)
{
    std::unique_ptr<EpollListener> listener { new EpollListener {} };
    listener->_impl = std::make_unique<Impl>(reactor);

    // Shared resolve + create + bind + listen; epoll wants the accept socket
    // (and thus the listener) non-blocking and close-on-exec.
    auto bound = Detail::BindAndListen(resolver, bindAddress, port, backlog, SOCK_NONBLOCK | SOCK_CLOEXEC, reusePort);
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

std::unique_ptr<EpollListener> EpollListener::Adopt(EpollReactor& reactor, int fd)
{
    std::unique_ptr<EpollListener> listener { new EpollListener {} };
    listener->_impl = std::make_unique<Impl>(reactor);

    if (fd < 0)
    {
        listener->_impl->bindError = "adopt: not a descriptor";
        return listener;
    }

    // No bind, no listen, no SO_REUSEADDR -- the supervisor did all three, and
    // repeating any of them on an already-listening socket fails. What is left
    // is the pair of descriptor properties `Bind` gets from
    // SOCK_NONBLOCK | SOCK_CLOEXEC and a handed-over descriptor arrives without.
    //
    // `PrepareOwnedFd` would be the obvious call here and is deliberately not
    // used: it ignores the non-blocking result, and on a listening descriptor
    // that is the failure this whole factory exists to prevent -- a blocking
    // accept parks the reactor thread that carries every other connection. Fatal
    // rather than best-effort, so the descriptor is closed and the listener says
    // why instead of coming up as a latent stall.
    if (!Detail::SetNonBlocking(static_cast<Detail::NativeSocket>(fd)))
    {
        listener->_impl->bindError = "adopt: cannot switch the inherited descriptor to non-blocking";
        ::close(fd);
        return listener;
    }
    Detail::ArmCloseOnExec(static_cast<Detail::NativeSocket>(fd));

    listener->_impl->handler.fd = fd;
    if (!reactor.Attach(&listener->_impl->handler))
    {
        listener->_impl->bindError = "adopt: epoll_ctl ADD failed";
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

std::uint16_t EpollListener::BoundPort() const noexcept
{
    if (!_impl || _impl->handler.fd < 0)
        return 0;
    return Detail::BoundPortOf(static_cast<Detail::NativeSocket>(_impl->handler.fd));
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
    {
        // Same tuning as the reactor-driven accept path (Impl::OnReadable):
        // without it, connections accepted through this fast path keep the
        // default send buffer and Nagle, so large replies park far sooner and
        // behave differently depending on which path happened to accept them.
        Detail::ApplyHotSocketOptions(static_cast<Detail::NativeSocket>(fd));
        auto peer = FormatPeerAddress(Detail::EndpointFromSockaddr(&client, static_cast<std::uint32_t>(len)));
        return AcceptAwaitable { AcceptResult { std::make_unique<EpollSocket>(_impl->reactor, fd, std::move(peer)) } };
    }
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
