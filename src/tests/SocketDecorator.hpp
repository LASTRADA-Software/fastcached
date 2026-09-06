// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/ISocket.hpp>

#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace FastCache::Testing
{

/// An `ISocket` that forwards every operation to another one.
///
/// ## Why this exists
///
/// A test that stages a socket CONDITION -- a short read, a read that fails, a
/// write that reports a partial count -- wants to change one method and leave the
/// rest alone. Written by hand each time, that is eight forwarding bodies per
/// decorator, of which seven are noise, and the copies then diverge in the part
/// nobody looked at. `AdminHttpServer_test.cpp`'s `ShortReadSocket` was one such
/// hand-written copy, and `FailingReadSocket` below would have been a second whose
/// `Write`, `WriteVectored`, `Close` and `IsClosed` were byte-identical to it.
///
/// **What the hand-written copies both got wrong is the part they did not write.**
/// `ISocket` has five virtuals with default implementations -- `HandshakeIfNeeded`,
/// `WaitReadable`, `PeerAddress`, `ShutdownWrite` and `CancelRead` -- and a decorator
/// that overrides none of them silently answers from the BASE rather than from the
/// socket it decorates. So a decorated TLS socket handshakes vacuously, a decorated
/// peer has no address, and a half-close reaches nothing. None of that is visible
/// at a call site: every one of those defaults succeeds. Forwarding them is a
/// property of the type here, so a decorator cannot omit it, and `ISocket` growing
/// a method is one edit rather than one per fake.
///
/// Same argument as `ScriptedSocket.hpp`'s, which this sits beside: a fake nothing
/// exercises does not report its own bugs, and the copies drift in silence.
class SocketDecorator: public ISocket
{
  public:
    /// @param inner The socket every operation is forwarded to; must outlive this.
    explicit SocketDecorator(ISocket& inner) noexcept:
        _inner { inner }
    {
    }

    /// @copydoc ISocket::Read
    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        return _inner.Read(buffer);
    }

    /// @copydoc ISocket::Write
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override
    {
        return _inner.Write(buffer);
    }

    /// @copydoc ISocket::WriteVectored
    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override
    {
        return _inner.WriteVectored(segments, std::move(keepAlive));
    }

    /// @copydoc ISocket::HandshakeIfNeeded
    [[nodiscard]] Task<std::expected<void, NetError>> HandshakeIfNeeded() override
    {
        return _inner.HandshakeIfNeeded();
    }

    /// @copydoc ISocket::WaitReadable
    [[nodiscard]] IoAwaitable WaitReadable() override
    {
        return _inner.WaitReadable();
    }

    /// @copydoc ISocket::PeerAddress
    [[nodiscard]] std::string PeerAddress() const override
    {
        return _inner.PeerAddress();
    }

    /// @copydoc ISocket::Close
    void Close() noexcept override
    {
        _inner.Close();
    }

    /// @copydoc ISocket::CancelRead
    void CancelRead() noexcept override
    {
        _inner.CancelRead();
    }

    /// @copydoc ISocket::ShutdownWrite
    void ShutdownWrite() noexcept override
    {
        _inner.ShutdownWrite();
    }

    /// @copydoc ISocket::IsClosed
    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _inner.IsClosed();
    }

  protected:
    /// The decorated socket, for a subclass that wants to reach it directly.
    /// @return The socket every un-overridden operation is forwarded to.
    [[nodiscard]] ISocket& Inner() const noexcept
    {
        return _inner;
    }

  private:
    ISocket& _inner;
};

/// A socket whose every read fails, the way an expired receive deadline does.
///
/// A deadline armed with `SO_RCVTIMEO` has no signal of its own: it reaches the
/// caller as a failed read, so this is the only way to stage one. An in-memory pipe
/// cannot -- its reads either deliver bytes or report EOF, and EOF is a *different*
/// fact about the peer, which is exactly the distinction
/// [#824](https://github.com/LASTRADA-Software/fastcached/issues/824) was about.
///
/// **The writes still succeed**, which is load-bearing rather than incidental: a
/// test asserting that a surface chose to answer nothing has to be able to tell
/// that from a surface that could not write. Decorator, therefore, and not a
/// standalone fake.
///
/// The code is a parameter because the platforms disagree about which one an expiry
/// is -- POSIX `EAGAIN`/`EWOULDBLOCK`, Winsock `WSAETIMEDOUT` -- and a fake pinning
/// one leaves the other's handling untested on the platform that uses it. See
/// `IsDeadlineExpiry` in `Net/NetError.hpp`.
class FailingReadSocket final: public SocketDecorator
{
  public:
    /// @param inner The socket writes, and the first `forwardFirst` reads, go to.
    /// @param code What every read after those fails with.
    /// @param forwardFirst How many reads are forwarded before the failures start.
    ///        `0` fails from the first. Any other value stages a peer that said
    ///        SOMETHING and then stopped, which is a different fact from a peer that
    ///        said nothing and a different answer on the wire.
    FailingReadSocket(ISocket& inner, NetErrorCode code, std::size_t forwardFirst = 0) noexcept:
        SocketDecorator { inner },
        _code { code },
        _remaining { forwardFirst }
    {
    }

    /// Forward while the allowance lasts, then fail.
    /// @param buffer Where a forwarded read puts its bytes.
    /// @return The decorated socket's answer, or the configured failure.
    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        return TakeAllowance() ? SocketDecorator::Read(buffer) : Failure();
    }

    /// The same answer `Read` would give.
    ///
    /// Overridden rather than inherited: `ISocket` documents `Read` and
    /// `WaitReadable` as one read operation sharing one slot, so a fake answering
    /// them from two different sockets parks a wait on a socket whose reads are
    /// never going to resolve or retrieve it. Nothing calls it on this fake today;
    /// the pairing is a property of the type, not of its current callers.
    /// @return The decorated socket's answer, or the configured failure.
    [[nodiscard]] IoAwaitable WaitReadable() override
    {
        return TakeAllowance() ? SocketDecorator::WaitReadable() : Failure();
    }

  private:
    /// Spend one forwarded read, if any are left.
    ///
    /// Shared by `Read` and `WaitReadable` because they are ONE read operation --
    /// the same reason `WaitReadable` is overridden at all. Written out twice, the
    /// allowance could be decremented in one and not the other, which is a fake that
    /// forwards a different number of reads depending on which spelling the code
    /// under test happens to use.
    /// @return Whether this call may be forwarded to the decorated socket.
    [[nodiscard]] bool TakeAllowance() noexcept
    {
        if (_remaining == 0)
            return false;
        --_remaining;
        return true;
    }

    /// @return The configured failure, without touching the decorated socket.
    [[nodiscard]] IoAwaitable Failure() const noexcept
    {
        return IoAwaitable { IoResult { std::unexpected(NetError { .code = _code, .systemCode = 0, .context = {} }) } };
    }

    NetErrorCode _code;
    std::size_t _remaining;
};

/// A socket whose `WaitReadable` PARKS, the way a reactor socket's does.
///
/// **The in-memory transport cannot stage this, and that is the whole reason this
/// exists.** `InMemorySocket::WaitReadable` answers `0` or `1` synchronously and is
/// right to: it holds both pipes and can always tell. A reactor socket cannot, so it
/// arms interest and suspends -- and a SUSPENDED readability wait is what occupies the
/// socket's single read-op slot, which is the entire subject of
/// [#663](https://github.com/LASTRADA-Software/fastcached/issues/663) and
/// [#710](https://github.com/LASTRADA-Software/fastcached/issues/710). A fixture on the
/// plain pair cannot reach either defect at all: every watch resolves inline and
/// nothing is ever left parked, which is why twelve blocking-read cases were green
/// while `RunBlockingRead` orphaned a coroutine per connection.
///
/// **It models the shared slot rather than asserting on it.** Arming a read verb while
/// a watch is parked ORPHANS that watch -- pointer dropped, never resumed, never freed
/// -- which is exactly what `EpollSocket::Read`'s `readOp.awaitable = nullptr` does.
/// `Detail::ClaimReadSlot` would instead abort the process, and its abort names the
/// SLOT rather than the caller (`Net/ReadSlot.hpp` says so, because two callers share
/// that assertion), so a case that wants to say WHICH caller double-armed has to be
/// able to observe it and carry on.
///
/// **`Close()` retires a parked watch and a cancel does too, and they are counted
/// apart.** Every real socket retires at `Close`, so a fake that did not would leak on
/// ordinary teardown and hide the defect behind its own bug -- but a watch retired by
/// the connection's teardown and one retired by the caller that armed it are opposite
/// answers about #710, and one counter would render them the same.
class ParkingReadableSocket final: public SocketDecorator
{
  public:
    /// @param inner The socket reads and writes are forwarded to; must outlive this.
    explicit ParkingReadableSocket(ISocket& inner) noexcept:
        SocketDecorator { inner }
    {
    }

    ParkingReadableSocket(ParkingReadableSocket const&) = delete;
    ParkingReadableSocket(ParkingReadableSocket&&) = delete;
    ParkingReadableSocket& operator=(ParkingReadableSocket const&) = delete;
    ParkingReadableSocket& operator=(ParkingReadableSocket&&) = delete;

    /// Retires a watch a case left parked. Not merely tidiness: an awaitable that is
    /// never completed is a coroutine frame that is never freed, which is the very
    /// thing these counters measure -- a fake that leaked one itself would report the
    /// defect against every build, including a fixed one.
    ~ParkingReadableSocket() override
    {
        ParkingReadableSocket::Close();
    }

    /// Park, always -- there is no synchronous answer, which is the point.
    /// @return An awaitable the test resolves with `ResolveReadable`, or that
    ///         `CancelRead`/`Close` retires.
    [[nodiscard]] IoAwaitable WaitReadable() override
    {
        ClaimSlot();
        ++_watchesArmed;
        IoAwaitable awaitable;
        // Recorded from inside `await_suspend`, never here: this local is returned by
        // value and destroyed, so its address is not the one the caller suspends on
        // (#734).
        awaitable.SetSuspendCallback(&OnWaitSuspended, this);
        return awaitable;
    }

    /// Forward, having first claimed the shared read slot the way a real socket does.
    /// @param buffer Where the forwarded read puts its bytes.
    /// @return The decorated socket's answer.
    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        ClaimSlot();
        return SocketDecorator::Read(buffer);
    }

    /// @copydoc ISocket::CancelRead
    void CancelRead() noexcept override
    {
        if (RetireParked())
            ++_watchesRetiredByCancel;
        SocketDecorator::CancelRead();
    }

    /// @copydoc ISocket::Close
    void Close() noexcept override
    {
        if (RetireParked())
            ++_watchesRetiredByClose;
        SocketDecorator::Close();
    }

    /// Resolve the parked watch as a reactor would when the socket becomes readable.
    /// @param count `0` for EOF, non-zero for bytes pending -- the distinction
    ///        `ISocket::WaitReadable` makes contractual.
    void ResolveReadable(std::size_t count) noexcept
    {
        auto* const parked = std::exchange(_parked, nullptr);
        if (parked == nullptr)
            return;
        ++_watchesResolved;
        parked->Complete(IoResult { count });
    }

    /// @return How many readability watches were armed.
    [[nodiscard]] std::size_t WatchesArmed() const noexcept
    {
        return _watchesArmed;
    }

    /// @return How many were dropped by a competing arm: never resumed, never freed.
    [[nodiscard]] std::size_t WatchesOrphaned() const noexcept
    {
        return _watchesOrphaned;
    }

    /// @return How many the CALLER retired through `CancelRead` -- #710's fix working.
    [[nodiscard]] std::size_t WatchesRetiredByCancel() const noexcept
    {
        return _watchesRetiredByCancel;
    }

    /// @return How many the connection's teardown swept up instead, which is a
    ///         different answer from the one above and not a substitute for it.
    [[nodiscard]] std::size_t WatchesRetiredByClose() const noexcept
    {
        return _watchesRetiredByClose;
    }

    /// @return How many resolved as readability rather than being retired.
    [[nodiscard]] std::size_t WatchesResolved() const noexcept
    {
        return _watchesResolved;
    }

    /// @return Whether a watch is parked on the read slot right now.
    [[nodiscard]] bool IsWatchParked() const noexcept
    {
        return _parked != nullptr;
    }

  private:
    /// @param awaitable The caller's awaitable, at its final address.
    static void OnWaitSuspended(IoAwaitable* awaitable, std::coroutine_handle<> /*handle*/) noexcept
    {
        static_cast<ParkingReadableSocket*>(awaitable->CallbackState())->_parked = awaitable;
    }

    /// Take the single read slot for a new operation, orphaning whatever held it --
    /// which is what every reactor socket does, and the defect the counter names.
    void ClaimSlot() noexcept
    {
        if (_parked == nullptr)
            return;
        ++_watchesOrphaned;
        _parked = nullptr;
    }

    /// Complete a parked watch with `Cancelled`, which is what both retirement routes
    /// deliver -- a caller's `CancelRead` and the connection's `Close`. Taking the code
    /// as a parameter would be a knob with one setting; WHICH route retired the watch
    /// is what the two counters record, and that is the distinction worth keeping.
    /// @return Whether there was a parked watch to retire.
    bool RetireParked() noexcept
    {
        auto* const parked = std::exchange(_parked, nullptr);
        if (parked == nullptr)
            return false;
        parked->Complete(
            IoResult { std::unexpected(NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = {} }) });
        return true;
    }

    IoAwaitable* _parked { nullptr };
    std::size_t _watchesArmed { 0 };
    std::size_t _watchesOrphaned { 0 };
    std::size_t _watchesRetiredByCancel { 0 };
    std::size_t _watchesRetiredByClose { 0 };
    std::size_t _watchesResolved { 0 };
};

} // namespace FastCache::Testing
