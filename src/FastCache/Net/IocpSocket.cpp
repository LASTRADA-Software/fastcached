// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/IocpSocket.hpp>

#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Async/ReactorTeardown.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/IocpStatus.hpp>
    #include <FastCache/Net/NetError.hpp>
    #include <FastCache/Net/ReadSlot.hpp>
    #include <FastCache/Net/SocketAddress.hpp>
    #include <FastCache/Net/WriteSlot.hpp>

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

// The typed sentinel in the header must be the platform's, or every comparison
// below silently stops meaning what it says. Both operands are uintptr_t here, so
// this assertion is not itself a mixed-sign comparison.
static_assert(InvalidSocketValue == static_cast<std::uintptr_t>(INVALID_SOCKET),
              "InvalidSocketValue must equal the platform's INVALID_SOCKET");

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

        /// A strong reference to this node itself, taken with `inFlight` and released
        /// by `Dispatch`.
        ///
        /// **Read ops only, and that asymmetry is the whole of #884.** A read op can be
        /// RETRACTED while the kernel still owns its `OVERLAPPED`: `CancelRead` retires
        /// the waiter and the caller may issue the next read in the same turn, so the
        /// node the kernel is still writing into has to outlive its place in
        /// `Impl::readOp`. `inFlight` cannot do that job -- it keeps the enclosing
        /// `Impl` alive, not this node -- so without this the next `Read` would either
        /// reuse the retracted node's `OVERLAPPED` or free it under the kernel.
        ///
        /// A write op has no such moment: nothing retracts a write short of `Close()`,
        /// which sets `_closed` and makes every arm site refuse before it reaches the
        /// `OVERLAPPED`. So `writeOp` stays a plain member and this stays empty for it.
        /// See `CancelRead` for the full argument and for what would change it.
        ///
        /// Deliberately a self-cycle while an operation is outstanding, broken by
        /// `Dispatch`, on exactly the terms `inFlight` above states: a completion that
        /// can never be dequeued leaks this node rather than freeing it under the
        /// kernel's feet.
        std::shared_ptr<Op> self {};

        /// Set when `CancelRead` could not retract the operation because it had ALREADY
        /// COMPLETED, so its waiter is deliberately left attached to receive the real
        /// result rather than a `Cancelled` that would throw the bytes away.
        ///
        /// It is what lets `ArmRead` tell this apart from #663's double-arm. Both look
        /// like "a waiter is attached to an in-flight node", and they are opposite
        /// facts: a double-arm is a caller bug and must abort, while this is a caller
        /// doing exactly what `CancelRead` invites and must not.
        bool settling { false };

        /// Whether this read op is a `WaitReadable` probe rather than a real read.
        ///
        /// **The completion cannot tell.** A zero-byte `WSARecv` fires on readability
        /// and reports `bytes == 0` whether the socket holds a megabyte or has been
        /// closed, so `Dispatch` was completing "0 bytes" for both -- which is how this
        /// platform came to report the opposite number from the POSIX ones for the same
        /// event (#677). Mirrors `EpollSocket`'s `readPeekOnly`.
        bool readPeekOnly { false };
    };

    /// The read node the NEXT read will use, replaced rather than reused whenever the
    /// kernel still owns the current one's `OVERLAPPED`. See `Op::self` and #884.
    std::shared_ptr<Op> readOp { MakeReadOp() };
    Op writeOp;

    /// Read nodes stood down by `ArmRead` while still waiting for a completion that had
    /// already happened when `CancelRead` ran. Erased by `Dispatch` once the completion
    /// arrives; walked by `~IocpSocket` so no waiter is left reachable across teardown.
    /// Bounded by the number of cancels between two reactor turns, which is one per
    /// retired read.
    std::vector<std::shared_ptr<Op>> settlingNodes;

    /// A fresh read node, wired to the shared dispatcher.
    /// @return The new node.
    [[nodiscard]] static std::shared_ptr<Op> MakeReadOp()
    {
        auto op = std::make_shared<Op>();
        op->completion.dispatch = &Dispatch;
        op->isWrite = false;
        return op;
    }

    /// Claim the read slot for a new operation, replacing the node when the current one
    /// is still in the kernel's hands.
    ///
    /// The order is load-bearing. `ClaimReadSlot` is asked FIRST and of the CURRENT
    /// node, because #663's question -- is a read already parked here -- is about the
    /// node a waiter is registered on, and replacing first would ask it of a fresh node
    /// and always pass. Only then does an in-flight node get stood down, which is
    /// #884's question: has the kernel finished with this `OVERLAPPED`.
    ///
    /// @return The node the caller should arm.
    [[nodiscard]] Op& ArmRead()
    {
        // A node `CancelRead` left settling is stood down WITHOUT asking
        // `ClaimReadSlot`, because its waiter is attached on purpose and the assert
        // would fire on a legal call. It is parked in `settlingNodes` rather than
        // simply dropped so the destructor can still reach its awaitable: the frame
        // that waiter lives in is normally destroyed alongside this socket, and a
        // completion resuming it afterwards is a use-after-free.
        if (readOp->settling)
        {
            settlingNodes.push_back(readOp);
            readOp = MakeReadOp();
            return *readOp;
        }

        Detail::ClaimReadSlot(readOp->awaitable); // repopulated by the suspend callback
        if (readOp->inFlight)
            readOp = MakeReadOp();

        // **An ordinary read allocates nothing, and that rests on a fact in another
        // function.** `Dispatch` moves `inFlight` and `self` out at its TOP, before it
        // resumes the waiter -- so a coroutine that arms its next read inline from the
        // resume finds this node free and REUSES it. A fresh node is taken only when an
        // operation is genuinely still outstanding, which after #884 means a
        // `CancelRead` that retired a parked read. Move either `std::move` in `Dispatch`
        // below its `Complete` and this silently becomes an allocation per read, with
        // every test still green.

        // Taken HERE rather than beside `inFlight` at the two arm sites, for the reason
        // `ClaimReadSlot` itself exists: a guard folded into the operation is
        // self-enforcing, and one written alongside it is a line the third arm site
        // forgets. Released by `Dispatch`, or by the arm site when the submit fails
        // outright and no completion will ever arrive.
        readOp->self = readOp;
        return *readOp;
    }

    static void Dispatch(IocpCompletion* base, DWORD bytes, IocpStatus status)
    {
        auto* op = reinterpret_cast<Op*>(base);

        // Released only when this function returns, so every member touched below
        // -- and the OVERLAPPED the kernel just wrote into -- is still alive even
        // if the owning IocpSocket was destroyed while this was in flight.
        auto const keepAlive = std::move(op->inFlight);

        // Released with `inFlight` and for the same reason: this node may already have
        // been replaced in `Impl::readOp` by a read armed after a `CancelRead`, and the
        // kernel was still writing into it until the completion this call is handling.
        auto const selfKeepAlive = std::move(op->self);

        // If this node was stood down waiting for exactly this completion, its entry is
        // done. Erased here rather than in `CancelRead` because this is the moment the
        // kernel is provably finished with it -- the same moment `inFlight` is released,
        // and for the same reason.
        if (keepAlive && op->settling)
        {
            op->settling = false;
            auto& nodes = keepAlive->settlingNodes;
            for (auto it = nodes.begin(); it != nodes.end(); ++it)
            {
                if (it->get() == op)
                {
                    nodes.erase(it);
                    break;
                }
            }
        }

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

        // The NTSTATUS the reactor read becomes a WSA code here, where the socket is in
        // reach. Below the early return above deliberately: a node stood down by
        // `CancelRead` has already answered its waiter, so asking Winsock about it would
        // be a syscall whose result nobody reads. `keepAlive->native` is
        // `INVALID_SOCKET` exactly when `Close()` got there first, which `WsaErrorOf`
        // reads as the abort it is.
        // `keepAlive` is non-null on every path that reaches here, and this line and the
        // `MSG_PEEK` below now say so ALIKE. They did not: the guard was here and a bare
        // dereference was twenty lines down, which is an author holding both positions
        // at once, and whichever reading is right one of the two lines was wrong.
        //
        // The invariant: `inFlight` is taken at submit and released only here, and a
        // completion is dequeued exactly once per submit, so it is set on entry. It is
        // also what keeps `Impl` alive -- so if it really were null, `op` itself may
        // already be freed and the guard would be protecting the second dereference
        // while the first had already lost. A guard that cannot fire, in front of a
        // hazard it could not contain, is worse than the invariant stated plainly.
        auto const err = Detail::WsaErrorOf(keepAlive->native, op->completion, status);
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
        // `readOp` is wired by `MakeReadOp`, since every replacement needs the same
        // wiring and a constructor cannot reach the ones made later.
        writeOp.completion.dispatch = &Dispatch;
        writeOp.isWrite = true;
    }
};

IocpSocket::IocpSocket(IocpReactor& reactor,
                       std::uintptr_t native,
                       std::string peerAddress,
                       IocpAttachment attachment) noexcept:
    _impl { std::make_shared<Impl>(reactor, static_cast<SOCKET>(native)) },
    _native { native },
    _peerAddress { std::move(peerAddress) },
    // Whether the IOCP association succeeded. If it did not, no completion will
    // ever be dequeued for this socket; callers must check IsAttached() and
    // abandon the connection instead of awaiting forever.
    //
    // A caller that already associated the handle says so rather than letting
    // this repeat it: a second CreateIoCompletionPort on the same handle fails,
    // and reporting that as "not attached" would condemn a working connection.
    // `ConnectEx` forces that order, because it requires the association before
    // the operation is issued.
    //
    // Reads the `native` PARAMETER rather than `_impl->native`: they are the same
    // value, and taking it from the parameter removes a dependency on this
    // initialiser running after `_impl`'s -- which member ORDER guarantees today
    // and would silently stop guaranteeing if the members were reordered.
    _attached { attachment == IocpAttachment::AlreadyAttached || reactor.AttachHandle(reinterpret_cast<void*>(native)) }
{
}

// The teardown rule lives in `Async/ReactorTeardown.hpp`, not here.
//
// `IocpSocket` and `IocpListener` null `awaitable` on teardown so a completion
// arriving afterwards does not resume a coroutine frame that is gone. That is a
// plain unsynchronised store, and it is safe for exactly one reason: dispatch
// runs on the reactor's single worker thread, so teardown on that thread cannot
// overlap it. Tearing down with nobody inside `Run()` is equally fine and common
// -- tests build a socket and drop it without ever running the reactor.
//
// Those are the two arms of `IReactor::TeardownIsSerialisedWithDispatch()`, and
// this file used to spell them out itself, in `#if defined(_WIN32)`, where no
// analyser and four of five CI legs could reach it
// ([#668](https://github.com/LASTRADA-Software/fastcached/issues/668)). Two live
// spellings of one rule is two things to reword, and the gate that reads the
// assertion's own words could only match the tail they happened to share -- so
// rewording either left the gate green and the other unmatched
// ([#840](https://github.com/LASTRADA-Software/fastcached/issues/840)).
IocpSocket::~IocpSocket()
{
    if (_impl)
    {
        Detail::AssertTeardownIsSerialisedWithDispatch(_impl->reactor);

        // The awaitable lives in the AWAITING coroutine's frame, which is
        // normally being destroyed alongside this socket. `inFlight` keeps the
        // completion block alive, but nothing makes that pointer valid again --
        // resuming it would be a second use-after-free, in the caller rather
        // than here. Clearing it is safe only because the assertion above holds:
        // a completion cannot be dequeued while this destructor runs.
        //
        // Only the CURRENT read node can carry a waiter. A node stood down by
        // `CancelRead` had its awaitable detached there, and nothing re-registers one
        // on a node that is no longer `readOp` -- so clearing this one clears all of
        // them, and the retired nodes stay alive on their own `Op::self` until their
        // completions arrive.
        _impl->readOp->awaitable = nullptr;
        for (auto const& node: _impl->settlingNodes)
            node->awaitable = nullptr;
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
    if (_impl && _impl->native != InvalidSocketValue)
    {
        ::closesocket(_impl->native);
        _impl->native = INVALID_SOCKET;
    }
}

void IocpSocket::CancelRead() noexcept
{
    if (_closed || !_impl || _impl->native == InvalidSocketValue)
        return;

    auto& op = *_impl->readOp;

    // Read, not yet detached: whether the waiter is detached at all depends on what kind
    // of operation this is, and detaching one that must keep its result is the data loss
    // this function exists to avoid.
    if (op.awaitable == nullptr)
        return;

    // Retract the operation. Best-effort by definition, and its RESULT is deliberately
    // not consulted -- see below for why there is nothing to consult it for.
    static_cast<void>(::CancelIoEx(reinterpret_cast<HANDLE>(_impl->native), reinterpret_cast<LPOVERLAPPED>(&op.completion)));

    // **A real read is never answered before its own operation has answered.**
    //
    // `CancelIoEx` cannot un-receive. It reports `ERROR_NOT_FOUND` for an operation that
    // had ALREADY completed -- its bytes out of the TCP stream and in the caller's
    // buffer -- and TRUE for one it merely marked, which is a request rather than a
    // result: that one can still complete with bytes before the mark takes effect.
    // Answering `Cancelled` in either case strands those bytes, because the queued
    // completion then reaches `Dispatch`, finds the awaitable detached, and drops them.
    //
    // **Neither case is distinguishable here, which is why this is unconditional rather
    // than a check.** Measured, instrumenting both paths deliberately:
    //
    //     receive genuinely PENDING     CancelIoEx=TRUE  err=0     GOR=FALSE gorerr=996
    //     receive ALREADY COMPLETED     CancelIoEx=FALSE err=1168  GOR=FALSE gorerr=996
    //
    // 996 is `WSA_IO_INCOMPLETE` in both. On an IOCP-bound socket the status is not
    // published to the `OVERLAPPED` until the completion is DEQUEUED, so
    // `WSAGetOverlappedResult` cannot tell a pending receive from a completed one, and
    // waiting for it (`fWait == TRUE`) would block the one thread that drains the port.
    // There is no discriminator at this instant. A version of this that branched on
    // `ERROR_NOT_FOUND` closed the half a test can force and left the other half open,
    // undetectable and silent -- the same defect one level down, wearing a fix.
    //
    // So the waiter stays attached, its node is marked `settling`, and it resolves with
    // whatever its operation actually did on the next reactor turn: the bytes, the real
    // error, or `Cancelled` from the aborted completion.
    //
    // **What is still synchronous, and it is what #884 is named for: the read SLOT is
    // free when this returns.** `ArmRead` stands a settling node down rather than reusing
    // it, so no `OVERLAPPED` the kernel owns is ever armed over. The stronger promise
    // that the WAITER is resolved inline was an acceptance criterion written from a code
    // read on a machine that could not build Windows, and the platform cannot honour it
    // without dropping bytes. `ISocket::CancelRead` states both promises separately.
    if (!op.readPeekOnly)
    {
        op.settling = true;
        return;
    }

    // **A `WaitReadable` probe IS retired inline, and the exemption is not a shortcut.**
    // Its receive is ZERO-BYTE, so it can carry no data and there is nothing a settle
    // could preserve. Completing it from here also keeps it away from the `MSG_PEEK` in
    // `Dispatch` that makes a zero-byte completion mean *readable* rather than *end of
    // stream* (#677) -- a settled probe would be answered by that path and could report
    // an EOF nobody observed.
    auto* const awaitable = std::exchange(op.awaitable, nullptr);
    awaitable->Complete(std::unexpected(NetError {
        .code = NetErrorCode::Cancelled,
        .systemCode = static_cast<int>(ERROR_OPERATION_ABORTED),
        .context = "CancelRead",
    }));
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
    Detail::RequireReadBuffer(buffer);
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    auto& op = _impl->ArmRead();
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
    op.self.reset();     // ... so `Dispatch` will not be the one to release this
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
    auto& op = _impl->ArmRead();
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
    op.self.reset();     // ... so `Dispatch` will not be the one to release this
    return IoAwaitable { std::unexpected(MakeWsaError(lastErr, "WSARecv")) };
}

IoAwaitable IocpSocket::Write(std::span<std::byte const> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    auto& op = _impl->writeOp;
    Detail::ClaimWriteSlot(op.awaitable);
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
    Detail::ClaimWriteSlot(op.awaitable);
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

    static void Dispatch(IocpCompletion* base, DWORD /*bytes*/, IocpStatus status)
    {
        auto* op = reinterpret_cast<AcceptOp*>(base);

        // Released only when this function returns, so the OVERLAPPED the kernel
        // just wrote into -- and `addrBuf`, which AcceptEx also wrote into -- are
        // still alive even if the owning IocpListener was destroyed while this
        // accept was outstanding.
        auto const keepAlive = std::move(op->inFlight);

        // `AcceptEx`'s result is asked of the LISTENING socket, not of the half-born
        // accepted one: the operation was issued on the listener, and `acceptSock` has
        // no completed operation to report on.
        auto const err = Detail::WsaErrorOf(keepAlive ? keepAlive->listenSock : INVALID_SOCKET, op->completion, status);

        auto* awaitable = op->awaitable;
        op->awaitable = nullptr;

        if (err != 0 || op->acceptSock == InvalidSocketValue)
        {
            if (op->acceptSock != InvalidSocketValue)
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

    // It owns two sockets and closes both, so a copy or a move would double-close.
    // It is held by shared_ptr and never copied; deleting these keeps that true by
    // construction rather than by nobody having tried yet.
    Impl(Impl const&) = delete;
    Impl& operator=(Impl const&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    ~Impl()
    {
        if (current.acceptSock != InvalidSocketValue)
            ::closesocket(current.acceptSock);
        if (listenSock != InvalidSocketValue)
            ::closesocket(listenSock);
    }
};

IocpListener::IocpListener() noexcept = default;
IocpListener::~IocpListener()
{
    if (!_impl)
        return;

    Detail::AssertTeardownIsSerialisedWithDispatch(_impl->reactor);

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
                 // Explicit: `&fn` is a pointer to a FUNCTION pointer, and letting that
                 // reach `LPVOID` implicitly is a multilevel conversion the analyser
                 // refuses -- the arity is easy to get wrong and WSAIoctl cannot check it.
                 static_cast<void*>(&fn),
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
                 // Explicit: `&getAddrsFn` is a pointer to a FUNCTION pointer, and letting that
                 // reach `LPVOID` implicitly is a multilevel conversion the analyser
                 // refuses -- the arity is easy to get wrong and WSAIoctl cannot check it.
                 static_cast<void*>(&getAddrsFn),
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
    return _impl && _impl->listenSock != InvalidSocketValue;
}

std::string_view IocpListener::BindError() const noexcept
{
    return _impl ? std::string_view { _impl->bindError } : std::string_view {};
}

std::uint16_t IocpListener::BoundPort() const noexcept
{
    if (!_impl || _impl->listenSock == InvalidSocketValue)
        return 0;
    return Detail::BoundPortOf(static_cast<Detail::NativeSocket>(_impl->listenSock));
}

void IocpListener::Close() noexcept
{
    if (!_impl)
        return;
    if (_impl->listenSock != InvalidSocketValue)
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
    if (op.acceptSock == InvalidSocketValue)
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
