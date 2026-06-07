// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/IocpSocket.hpp>

#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Core/Errors/NetError.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/SocketAddress.hpp>

    #include <winsock2.h>

    #include <array>
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
        // completion: WSASend consumes the WSABUF array asynchronously, and
        // the payload bytes are referenced (not copied) until the completion
        // is dequeued. Both are released by the next Read/Write/WriteVectored
        // that reuses this op, or at socket teardown.
        std::vector<WSABUF> writeBufs {};
        std::shared_ptr<void const> writeKeepAlive {};
    };

    Op readOp;
    Op writeOp;

    static void Dispatch(IocpCompletion* base, DWORD bytes, DWORD err)
    {
        auto* op = reinterpret_cast<Op*>(base);
        auto* awaitable = op->awaitable;
        op->awaitable = nullptr;
        if (!awaitable)
            return;
        if (err == 0)
            awaitable->Complete(IoResult { static_cast<std::size_t>(bytes) });
        else
            awaitable->Complete(std::unexpected(MakeWsaError(static_cast<int>(err), op->isWrite ? "WSASend" : "WSARecv")));
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

IocpSocket::IocpSocket(IocpReactor& reactor, std::uintptr_t native) noexcept:
    _impl { std::make_unique<Impl>(reactor, static_cast<SOCKET>(native)) },
    _native { native }
{
    // Record whether the IOCP association succeeded. If it didn't, no
    // completion will ever be dequeued for this socket; callers must check
    // IsAttached() and abandon the connection instead of awaiting forever.
    _attached = reactor.AttachHandle(reinterpret_cast<void*>(static_cast<std::uintptr_t>(_impl->native)));
}

IocpSocket::~IocpSocket()
{
    IocpSocket::Close();
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
    op.completion.overlapped = OVERLAPPED {};

    WSABUF wsaBuf;
    wsaBuf.buf = reinterpret_cast<CHAR*>(buffer.data());
    wsaBuf.len = static_cast<ULONG>(buffer.size());
    DWORD bytesReceived = 0;
    DWORD flags = 0;
    auto const rc = WSARecv(
        _impl->native, &wsaBuf, 1, &bytesReceived, &flags, reinterpret_cast<LPWSAOVERLAPPED>(&op.completion), nullptr);
    auto const lastErr = (rc == 0) ? 0 : WSAGetLastError();
    if (rc == 0 || lastErr == WSA_IO_PENDING)
    {
        IoAwaitable a;
        a.SetSuspendCallback(&SocketAwaitableSuspended, &op);
        return a;
    }
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
    };

    AcceptOp current;

    static void Dispatch(IocpCompletion* base, DWORD /*bytes*/, DWORD err)
    {
        auto* op = reinterpret_cast<AcceptOp*>(base);
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
        auto sock = std::make_unique<IocpSocket>(*op->reactor, static_cast<std::uintptr_t>(op->acceptSock));
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
IocpListener::~IocpListener() = default;

std::unique_ptr<IocpListener> IocpListener::Bind(
    IocpReactor& reactor, std::string_view bindAddress, std::uint16_t port, int backlog, IAddressResolver& resolver)
{
    std::unique_ptr<IocpListener> listener { new IocpListener {} };
    listener->_impl = std::make_unique<Impl>(reactor);

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

    if (!reactor.AttachHandle(reinterpret_cast<void*>(sock)))
    {
        listener->_impl->bindError = "CreateIoCompletionPort failed";
        ::closesocket(sock);
        return listener;
    }

    listener->_impl->listenSock = sock;
    listener->_impl->current.completion.dispatch = &Impl::Dispatch;
    listener->_impl->current.reactor = &reactor;
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

    ::closesocket(op.acceptSock);
    op.acceptSock = INVALID_SOCKET;
    return AcceptAwaitable { std::unexpected(MakeWsaError(lastErr, "AcceptEx")) };
}

} // namespace FastCache

#endif // _WIN32
