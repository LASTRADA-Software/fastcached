// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/IocpSocket.hpp>

#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/NetError.hpp>
    #include <FastCache/Net/SocketAddress.hpp>

    #include <winsock2.h>

    #include <array>
    #include <cassert>
    #include <cstddef>
    #include <cstdint>
    #include <expected>
    #include <memory>
    #include <span>
    #include <string>
    #include <string_view>
    #include <utility>
    #include <vector>

    #include <mswsock.h>
    #include <ws2tcpip.h>

namespace FastCache
{

namespace
{

    [[nodiscard]] NetErrorCode TranslateWsa(int code) noexcept
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
            case ERROR_OPERATION_ABORTED:
                return NetErrorCode::Cancelled;
            default:
                return NetErrorCode::SystemError;
        }
    }

    [[nodiscard]] NetError MakeWsaError(int code, std::string ctx)
    {
        return NetError {
            .code = TranslateWsa(code),
            .systemCode = code,
            .context = std::move(ctx),
        };
    }

    /// Assert that a destructor clearing a pending awaitable cannot race the
    /// completion dispatch that would read it.
    ///
    /// `IocpSocket` and `IocpListener` null `awaitable` on teardown so a
    /// completion arriving afterwards does not resume a coroutine frame that is
    /// gone. That is a plain unsynchronised store, and it is safe for exactly one
    /// reason: dispatch runs on the reactor's single worker thread, so teardown on
    /// that thread cannot overlap it. `IocpReactor` has always documented one
    /// worker thread; #465 is what made the property load-bearing rather than
    /// descriptive, and a documented property that something now depends on needs
    /// a check rather than a citation.
    ///
    /// Tearing down while no thread is inside `Run()` is equally fine and common
    /// -- tests build a socket and drop it without ever running the reactor, and
    /// shutdown destroys listeners after `Run()` has returned. With nothing
    /// dequeuing there is no race to lose, so that case passes.
    ///
    /// Debug-only, by `assert`: a destructor cannot throw, and this is a contract
    /// violation by the caller rather than a runtime condition to report. The same
    /// shape `RaftPeerTransport::Stop` uses for its own thread rule.
    /// @param reactor The reactor whose worker thread the object belongs to.
    void AssertTeardownIsSerialisedWithDispatch([[maybe_unused]] IocpReactor const& reactor) noexcept
    {
        assert((!reactor.Running() || reactor.IsOnWorkerThread())
               && "an IOCP socket/listener must be destroyed on its reactor's worker thread, or with that "
                  "reactor stopped -- otherwise clearing a pending awaitable races the completion dispatch");
    }

} // namespace

struct IocpSocket::Impl
{
    IocpReactor& reactor;
    SOCKET native;

    /// Per-direction op state. We keep one of each so a Read and a Write
    /// can be in flight simultaneously; only one of each at a time is the
    /// caller's contract.
    struct Op
    {
        IocpCompletion completion;
        IoAwaitable* awaitable { nullptr };
        bool isWrite { false };
        // Vectored-write backing storage that must outlive the overlapped
        // completion: WSASend consumes the WSABUF array asynchronously, and the
        // payload bytes are referenced (not copied) until the completion is
        // dequeued.
        //
        // Released by `Dispatch`, which is the first moment the kernel is
        // provably done with them. This used to say "or at socket teardown",
        // which was a second, separate defect (#465): destroying a socket with a
        // write in flight freed bytes WSASend was still reading, and the failure
        // mode is corruption on the wire rather than a crash. `inFlight` below is
        // what makes the block survive to be released here.
        std::vector<WSABUF> writeBufs {};
        std::shared_ptr<void const> writeKeepAlive {};

        /// A strong reference to the enclosing `Impl`, taken when the overlapped
        /// operation is submitted and released by `Dispatch`.
        ///
        /// This is what makes a socket safe to destroy with I/O in flight. The
        /// kernel holds `&completion` -- and WRITES to it, since `Internal` and
        /// `InternalHigh` are updated at completion -- so closing the handle does
        /// not retract anything: it makes the operation complete with
        /// `ERROR_OPERATION_ABORTED`, and the completion is still dequeued
        /// afterwards. Freeing this block before that happens is a use-after-free
        /// on the read side and a write to freed memory on the kernel's side.
        ///
        /// Deliberately a reference cycle while an operation is outstanding
        /// (`Impl` -> `Op` -> `Impl`), broken by `Dispatch`. If a completion can
        /// never be dequeued -- a reactor destroyed without draining -- this block
        /// leaks instead of being freed under the kernel's feet. That trade is the
        /// point: a bounded leak on an abnormal shutdown path in exchange for
        /// removing a use-after-free from the normal one.
        ///
        /// Cost, measured rather than asserted, because "it is only an atomic" is
        /// how a hot path gets slow (`GetAdaptersAddresses` at 238x `getifaddrs`
        /// is this repository's standing reminder). One copy at submit and one
        /// move-out plus release at dispatch, against a real submit + completion
        /// round trip:
        ///
        ///   refcount pair                     ~8 ns
        ///   WSARecv + dequeued completion  ~4500 ns   (loopback, 1 byte, warm)
        ///
        /// So ~0.18% of one submitted operation, on this machine at /O2. A ratio
        /// rather than a bare nanosecond figure, since the number alone does not
        /// say whether it matters.
        std::shared_ptr<Impl> inFlight {};

        /// Whether this read op is a `WaitReadable` probe rather than a real read.
        ///
        /// **The completion cannot tell.** A zero-byte `WSARecv` fires on readability
        /// and reports `bytes == 0` whether the socket holds a megabyte or has been
        /// closed, so `Dispatch` was completing "0 bytes" for both -- which is how this
        /// platform came to report the opposite number from the POSIX ones for the same
        /// event (#677). Mirrors `EpollSocket`'s `readPeekOnly`.
        bool readPeekOnly { false };
    };

    Op readOp;
    Op writeOp;

    static void Dispatch(IocpCompletion* base, DWORD bytes, DWORD err)
    {
        auto* op = reinterpret_cast<Op*>(base);

        // Released only when this function returns, so every member touched below
        // -- and the OVERLAPPED the kernel just wrote into -- is still alive even
        // if the owning IocpSocket was destroyed while this was in flight.
        auto const keepAlive = std::move(op->inFlight);

        auto* awaitable = op->awaitable;
        op->awaitable = nullptr;

        // The write's backing storage is referenced by the kernel, not copied, so
        // the completion arriving is the first moment it is safe to let go -- see
        // the comment on `writeBufs`. Before `Complete`, because resuming the
        // waiter can issue the next write straight into this same op.
        op->writeBufs.clear();
        op->writeKeepAlive.reset();

        auto const wasPeek = std::exchange(op->readPeekOnly, false);

        if (!awaitable)
            return;
        if (err != 0)
        {
            awaitable->Complete(std::unexpected(MakeWsaError(static_cast<int>(err), op->isWrite ? "WSASend" : "WSARecv")));
            return;
        }
        if (!wasPeek)
        {
            awaitable->Complete(IoResult { static_cast<std::size_t>(bytes) });
            return;
        }

        // **A zero-byte receive completes with zero bytes whatever is waiting**, so the
        // distinction has to be measured here rather than read off `bytes`. One
        // `MSG_PEEK` on a socket the kernel has just reported readable, consuming
        // nothing -- the same thing the POSIX sockets do inline. A negative peek is a
        // spurious readiness or an error the caller's own `Read` will surface, so it
        // keeps the old answer.
        char probe = 0;
        auto const peeked = ::recv(keepAlive->native, &probe, 1, MSG_PEEK);
        awaitable->Complete(IoResult { peeked == 0 ? std::size_t { 0 } : std::size_t { 1 } });
    }

    Impl(IocpReactor& r, SOCKET s):
        reactor { r },
        native { s }
    {
        readOp.completion.dispatch = &Dispatch;
        writeOp.completion.dispatch = &Dispatch;
        readOp.isWrite = false;
        writeOp.isWrite = true;
    }
};

IocpSocket::IocpSocket(IocpReactor& reactor,
                       std::uintptr_t native,
                       std::string peerAddress,
                       IocpAttachment attachment) noexcept:
    _impl { std::make_shared<Impl>(reactor, static_cast<SOCKET>(native)) },
    _native { native },
    _peerAddress { std::move(peerAddress) }
{
    // Record whether the IOCP association succeeded. If it didn't, no
    // completion will ever be dequeued for this socket; callers must check
    // IsAttached() and abandon the connection instead of awaiting forever.
    //
    // A caller that already associated the handle says so rather than letting
    // this repeat it: a second CreateIoCompletionPort on the same handle fails,
    // and reporting that as "not attached" would condemn a working connection.
    // `ConnectEx` forces that order, because it requires the association before
    // the operation is issued.
    _attached = attachment == IocpAttachment::AlreadyAttached
                || reactor.AttachHandle(reinterpret_cast<void*>(static_cast<std::uintptr_t>(_impl->native)));
}

IocpSocket::~IocpSocket()
{
    if (_impl)
    {
        AssertTeardownIsSerialisedWithDispatch(_impl->reactor);

        // The awaitable lives in the AWAITING coroutine's frame, which is
        // normally being destroyed alongside this socket. `inFlight` keeps the
        // completion block alive, but nothing makes that pointer valid again --
        // resuming it would be a second use-after-free, in the caller rather
        // than here. Clearing it is safe only because the assertion above holds:
        // a completion cannot be dequeued while this destructor runs.
        _impl->readOp.awaitable = nullptr;
        _impl->writeOp.awaitable = nullptr;
    }

    // Closing is the cancellation: it makes any outstanding operation complete
    // with ERROR_OPERATION_ABORTED. The completion still arrives afterwards, and
    // `Op::inFlight` is what it arrives into.
    IocpSocket::Close();
}

void IocpSocket::ShutdownWrite() noexcept
{
    if (!_closed && _impl)
        Detail::HalfCloseWrite(static_cast<Detail::NativeSocket>(_impl->native));
}

void IocpSocket::Close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    if (_impl && _impl->native != INVALID_SOCKET)
    {
        ::closesocket(_impl->native);
        _impl->native = INVALID_SOCKET;
    }
}

namespace
{

    /// Suspend callback shared by Read and Write: when the caller's
    /// awaitable suspends, we know its final address in the caller's
    /// coroutine frame. Record it on the per-direction op so the IOCP
    /// completion handler can call Complete on the right object.
    void SocketAwaitableSuspended(IoAwaitable* self, std::coroutine_handle<> /*handle*/)
    {
        auto* op = static_cast<IocpSocket::Impl::Op*>(self->CallbackState());
        op->awaitable = self;
    }

} // namespace

IoAwaitable IocpSocket::Read(std::span<std::byte> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    auto& op = _impl->readOp;
    op.awaitable = nullptr; // populated by the suspend callback below
    op.readPeekOnly = false;
    op.completion.overlapped = OVERLAPPED {};

    WSABUF wsaBuf;
    wsaBuf.buf = reinterpret_cast<CHAR*>(buffer.data());
    wsaBuf.len = static_cast<ULONG>(buffer.size());
    DWORD bytesReceived = 0;
    DWORD flags = 0;
    op.inFlight = _impl; // the kernel now holds &op.completion; see Op::inFlight
    auto const rc = WSARecv(
        _impl->native, &wsaBuf, 1, &bytesReceived, &flags, reinterpret_cast<LPWSAOVERLAPPED>(&op.completion), nullptr);
    auto const lastErr = (rc == 0) ? 0 : WSAGetLastError();
    if (rc == 0 || lastErr == WSA_IO_PENDING)
    {
        IoAwaitable a;
        a.SetSuspendCallback(&SocketAwaitableSuspended, &op);
        return a;
    }
    op.inFlight.reset(); // failed synchronously, so no completion will arrive
    return IoAwaitable { std::unexpected(MakeWsaError(lastErr, "WSARecv")) };
}

IoAwaitable IocpSocket::WaitReadable()
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // Zero-byte WSARecv: a documented Winsock idiom for "wake when data is
    // pending without consuming a byte". The completion fires when the socket
    // is readable (data, EOF, or error) and carries `bytes == 0` in every one of
    // those cases, so `Dispatch` peeks to tell them apart -- see `Op::readPeekOnly`.
    // The caller is expected to issue a real Read next.
    auto& op = _impl->readOp;
    op.awaitable = nullptr;
    op.readPeekOnly = true;
    op.completion.overlapped = OVERLAPPED {};

    WSABUF wsaBuf;
    wsaBuf.buf = nullptr;
    wsaBuf.len = 0;
    DWORD bytesReceived = 0;
    DWORD flags = 0;
    op.inFlight = _impl; // the kernel now holds &op.completion; see Op::inFlight
    auto const rc = WSARecv(
        _impl->native, &wsaBuf, 1, &bytesReceived, &flags, reinterpret_cast<LPWSAOVERLAPPED>(&op.completion), nullptr);
    auto const lastErr = (rc == 0) ? 0 : WSAGetLastError();
    if (rc == 0 || lastErr == WSA_IO_PENDING)
    {
        IoAwaitable a;
        a.SetSuspendCallback(&SocketAwaitableSuspended, &op);
        return a;
    }
    op.inFlight.reset(); // failed synchronously, so no completion will arrive
    return IoAwaitable { std::unexpected(MakeWsaError(lastErr, "WSARecv")) };
}

IoAwaitable IocpSocket::Write(std::span<std::byte const> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    auto& op = _impl->writeOp;
    op.awaitable = nullptr;
    op.completion.overlapped = OVERLAPPED {};

    WSABUF wsaBuf;
    wsaBuf.buf = const_cast<CHAR*>(reinterpret_cast<CHAR const*>(buffer.data()));
    wsaBuf.len = static_cast<ULONG>(buffer.size());
    DWORD bytesSent = 0;
    op.inFlight = _impl; // the kernel now holds &op.completion; see Op::inFlight
    auto const rc = WSASend(_impl->native,
                            &wsaBuf,
                            1,
                            &bytesSent,
                            /*flags*/ 0,
                            reinterpret_cast<LPWSAOVERLAPPED>(&op.completion),
                            nullptr);
    auto const lastErr = (rc == 0) ? 0 : WSAGetLastError();
    if (rc == 0 || lastErr == WSA_IO_PENDING)
    {
        IoAwaitable a;
        a.SetSuspendCallback(&SocketAwaitableSuspended, &op);
        return a;
    }
    op.inFlight.reset(); // failed synchronously, so no completion will arrive
    return IoAwaitable { std::unexpected(MakeWsaError(lastErr, "WSASend")) };
}

IoAwaitable IocpSocket::WriteVectored(std::span<std::span<std::byte const> const> segments,
                                      std::shared_ptr<void const> keepAlive)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    auto& op = _impl->writeOp;
    op.awaitable = nullptr;
    op.completion.overlapped = OVERLAPPED {};

    // Build the WSABUF array from the non-empty segments. Both this array and
    // the referenced bytes must outlive the async completion, so the array
    // lives in `op.writeBufs` and the payload owner in `op.writeKeepAlive`.
    op.writeBufs.clear();
    op.writeBufs.reserve(segments.size());
    for (auto const seg: segments)
    {
        if (seg.empty())
            continue;
        WSABUF buf;
        buf.buf = const_cast<CHAR*>(reinterpret_cast<CHAR const*>(seg.data()));
        buf.len = static_cast<ULONG>(seg.size());
        op.writeBufs.push_back(buf);
    }
    op.writeKeepAlive = std::move(keepAlive);

    if (op.writeBufs.empty())
    {
        // Nothing to send; complete synchronously with zero bytes.
        op.writeKeepAlive.reset();
        return IoAwaitable { IoResult { 0 } };
    }

    DWORD bytesSent = 0;
    op.inFlight = _impl; // the kernel now holds &op.completion; see Op::inFlight
    auto const rc = WSASend(_impl->native,
                            op.writeBufs.data(),
                            static_cast<DWORD>(op.writeBufs.size()),
                            &bytesSent,
                            /*flags*/ 0,
                            reinterpret_cast<LPWSAOVERLAPPED>(&op.completion),
                            nullptr);
    auto const lastErr = (rc == 0) ? 0 : WSAGetLastError();
    if (rc == 0 || lastErr == WSA_IO_PENDING)
    {
        IoAwaitable a;
        a.SetSuspendCallback(&SocketAwaitableSuspended, &op);
        return a;
    }
    op.writeBufs.clear();
    op.writeKeepAlive.reset();
    op.inFlight.reset(); // failed synchronously, so no completion will arrive
    return IoAwaitable { std::unexpected(MakeWsaError(lastErr, "WSASend")) };
}

// -- IocpListener ----------------------------------------------------------

namespace
{

    // AcceptEx requires a per-address buffer of at least sizeof(sockaddr) + 16.
    // Use sockaddr_storage so an IPv6 peer/local address is never truncated.
    constexpr std::size_t AcceptAddrSize = sizeof(sockaddr_storage) + 16;

} // namespace

struct IocpListener::Impl
{
    IocpReactor& reactor;
    SOCKET listenSock { INVALID_SOCKET };
    LPFN_ACCEPTEX acceptExFn { nullptr };
    int family { AF_INET }; ///< Address family of the bound socket (for accept sockets).
    std::string bindError;

    /// Pending AcceptEx state. Only one Accept is in flight at a time
    /// (single-reactor contract).
    struct AcceptOp
    {
        IocpCompletion completion;
        AcceptAwaitable* awaitable { nullptr };
        SOCKET acceptSock { INVALID_SOCKET };
        std::array<std::byte, AcceptAddrSize * 2> addrBuf {};
        IocpReactor* reactor { nullptr };
        /// GetAcceptExSockaddrs, loaded once at Bind() and copied here so the
        /// static Dispatch can extract the peer address from `addrBuf` without
        /// reaching back into the listener Impl.
        LPFN_GETACCEPTEXSOCKADDRS getAcceptExSockaddrsFn { nullptr };

        /// Strong reference to the enclosing `Impl` while an AcceptEx is
        /// outstanding, released by `Dispatch`. Identical in purpose and in
        /// consequence to `IocpSocket::Impl::Op::inFlight`, which carries the
        /// full reasoning: `addrBuf` here is a second buffer the kernel writes
        /// into, on top of the OVERLAPPED itself.
        std::shared_ptr<Impl> inFlight {};
    };

    AcceptOp current;

    static void Dispatch(IocpCompletion* base, DWORD /*bytes*/, DWORD err)
    {
        auto* op = reinterpret_cast<AcceptOp*>(base);

        // Released only when this function returns, so the OVERLAPPED the kernel
        // just wrote into -- and `addrBuf`, which AcceptEx also wrote into -- are
        // still alive even if the owning IocpListener was destroyed while this
        // accept was outstanding.
        auto const keepAlive = std::move(op->inFlight);

        auto* awaitable = op->awaitable;
        op->awaitable = nullptr;

        if (err != 0 || op->acceptSock == INVALID_SOCKET)
        {
            if (op->acceptSock != INVALID_SOCKET)
            {
                ::closesocket(op->acceptSock);
                op->acceptSock = INVALID_SOCKET;
            }
            if (awaitable)
                awaitable->Complete(std::unexpected(MakeWsaError(static_cast<int>(err), "AcceptEx")));
            return;
        }

        // Hand the accepted SOCKET off into an IocpSocket wrapping it.
        Detail::ApplyHotSocketOptions(static_cast<Detail::NativeSocket>(op->acceptSock));

        // AcceptEx already wrote the local + remote sockaddrs into addrBuf;
        // GetAcceptExSockaddrs parses out the remote peer with no extra syscall.
        std::string peer;
        if (op->getAcceptExSockaddrsFn != nullptr)
        {
            sockaddr* localAddr = nullptr;
            sockaddr* remoteAddr = nullptr;
            int localLen = 0;
            int remoteLen = 0;
            op->getAcceptExSockaddrsFn(op->addrBuf.data(),
                                       0,
                                       static_cast<DWORD>(AcceptAddrSize),
                                       static_cast<DWORD>(AcceptAddrSize),
                                       &localAddr,
                                       &localLen,
                                       &remoteAddr,
                                       &remoteLen);
            if (remoteAddr != nullptr && remoteLen > 0)
                peer = FormatPeerAddress(Detail::EndpointFromSockaddr(remoteAddr, static_cast<std::uint32_t>(remoteLen)));
        }

        auto sock = std::make_unique<IocpSocket>(*op->reactor, static_cast<std::uintptr_t>(op->acceptSock), std::move(peer));
        op->acceptSock = INVALID_SOCKET;
        if (awaitable)
            awaitable->Complete(AcceptResult { std::move(sock) });
    }

    Impl(IocpReactor& r):
        reactor { r }
    {
    }

    ~Impl()
    {
        if (current.acceptSock != INVALID_SOCKET)
            ::closesocket(current.acceptSock);
        if (listenSock != INVALID_SOCKET)
            ::closesocket(listenSock);
    }
};

IocpListener::IocpListener() noexcept = default;
IocpListener::~IocpListener()
{
    if (!_impl)
        return;

    AssertTeardownIsSerialisedWithDispatch(_impl->reactor);

    // See ~IocpSocket: the awaitable is in the awaiting coroutine's frame, which
    // `AcceptOp::inFlight` does not and cannot keep alive.
    _impl->current.awaitable = nullptr;

    // `Impl::~Impl` closes `listenSock` and any half-built `acceptSock`, which is
    // what aborts an outstanding AcceptEx. The completion arrives afterwards and
    // lands on the block `inFlight` is holding, not on freed memory.
}

std::unique_ptr<IocpListener> IocpListener::Bind(
    IocpReactor& reactor, std::string_view bindAddress, std::uint16_t port, int backlog, IAddressResolver& resolver)
{
    std::unique_ptr<IocpListener> listener { new IocpListener {} };
    listener->_impl = std::make_shared<Impl>(reactor);

    // Shared resolve + create + bind + listen (IPv4/IPv6 literal or hostname).
    auto bound = Detail::BindAndListen(resolver, bindAddress, port, backlog, /*extraTypeFlags*/ 0);
    if (!bound.has_value())
    {
        listener->_impl->bindError = std::move(bound).error();
        return listener;
    }
    auto const sock = static_cast<SOCKET>(bound->socket);
    listener->_impl->family = bound->family;

    // Fetch the AcceptEx fn pointer via WSAIoctl.
    GUID guidAcceptEx = WSAID_ACCEPTEX;
    DWORD bytesReturned = 0;
    LPFN_ACCEPTEX fn = nullptr;
    if (WSAIoctl(sock,
                 SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guidAcceptEx,
                 sizeof(guidAcceptEx),
                 &fn,
                 sizeof(fn),
                 &bytesReturned,
                 nullptr,
                 nullptr)
        != 0)
    {
        listener->_impl->bindError = "WSAIoctl(AcceptEx) failed";
        ::closesocket(sock);
        return listener;
    }
    listener->_impl->acceptExFn = fn;

    // Fetch GetAcceptExSockaddrs the same way, to parse the peer address out of
    // the AcceptEx output buffer. A failure here is non-fatal: peer-address
    // capture (used only by --log-source) is simply disabled for this listener.
    GUID guidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
    LPFN_GETACCEPTEXSOCKADDRS getAddrsFn = nullptr;
    DWORD addrsBytesReturned = 0;
    if (WSAIoctl(sock,
                 SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guidGetAcceptExSockaddrs,
                 sizeof(guidGetAcceptExSockaddrs),
                 &getAddrsFn,
                 sizeof(getAddrsFn),
                 &addrsBytesReturned,
                 nullptr,
                 nullptr)
        != 0)
    {
        getAddrsFn = nullptr;
    }

    if (!reactor.AttachHandle(reinterpret_cast<void*>(sock)))
    {
        listener->_impl->bindError = "CreateIoCompletionPort failed";
        ::closesocket(sock);
        return listener;
    }

    listener->_impl->listenSock = sock;
    listener->_impl->current.completion.dispatch = &Impl::Dispatch;
    listener->_impl->current.reactor = &reactor;
    listener->_impl->current.getAcceptExSockaddrsFn = getAddrsFn;
    return listener;
}

bool IocpListener::IsBound() const noexcept
{
    return _impl && _impl->listenSock != INVALID_SOCKET;
}

std::string_view IocpListener::BindError() const noexcept
{
    return _impl ? std::string_view { _impl->bindError } : std::string_view {};
}

std::uint16_t IocpListener::BoundPort() const noexcept
{
    if (!_impl || _impl->listenSock == INVALID_SOCKET)
        return 0;
    return Detail::BoundPortOf(static_cast<Detail::NativeSocket>(_impl->listenSock));
}

void IocpListener::Close() noexcept
{
    if (!_impl)
        return;
    if (_impl->listenSock != INVALID_SOCKET)
    {
        ::closesocket(_impl->listenSock);
        _impl->listenSock = INVALID_SOCKET;
    }
}

namespace
{

    void ListenerAwaitableSuspended(AcceptAwaitable* self, std::coroutine_handle<> /*handle*/)
    {
        auto* op = static_cast<IocpListener::Impl::AcceptOp*>(self->CallbackState());
        op->awaitable = self;
    }

} // namespace

AcceptAwaitable IocpListener::Accept()
{
    if (!IsBound())
        return AcceptAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = std::string { BindError() } }) };

    auto& op = _impl->current;
    op.awaitable = nullptr;
    op.completion.overlapped = OVERLAPPED {};

    op.acceptSock = ::socket(_impl->family, SOCK_STREAM, IPPROTO_TCP);
    if (op.acceptSock == INVALID_SOCKET)
        return AcceptAwaitable { std::unexpected(MakeWsaError(WSAGetLastError(), "socket(accept)")) };

    DWORD bytesReceived = 0;
    op.inFlight = _impl; // the kernel now holds &op.completion and &op.addrBuf
    auto const ok = _impl->acceptExFn(_impl->listenSock,
                                      op.acceptSock,
                                      op.addrBuf.data(),
                                      0,
                                      static_cast<DWORD>(AcceptAddrSize),
                                      static_cast<DWORD>(AcceptAddrSize),
                                      &bytesReceived,
                                      reinterpret_cast<LPWSAOVERLAPPED>(&op.completion));
    auto const lastErr = ok ? 0 : WSAGetLastError();
    if (ok || lastErr == WSA_IO_PENDING)
    {
        AcceptAwaitable a;
        a.SetSuspendCallback(&ListenerAwaitableSuspended, &op);
        return a;
    }

    op.inFlight.reset(); // failed synchronously, so no completion will arrive
    ::closesocket(op.acceptSock);
    op.acceptSock = INVALID_SOCKET;
    return AcceptAwaitable { std::unexpected(MakeWsaError(lastErr, "AcceptEx")) };
}

} // namespace FastCache

#endif // _WIN32
