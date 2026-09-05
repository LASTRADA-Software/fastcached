// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/ISocket.hpp>

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
/// nobody looked at. `AdminHttpServer_test.cpp` had two such decorators whose
/// `Write`, `WriteVectored`, `Close` and `IsClosed` were byte-identical.
///
/// **What the hand-written copies both got wrong is the part they did not write.**
/// `ISocket` has four virtuals with default implementations -- `HandshakeIfNeeded`,
/// `WaitReadable`, `PeerAddress` and `ShutdownWrite` -- and a decorator that
/// overrides none of them silently answers from the BASE rather than from the
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
        if (_remaining == 0)
            return Failure();
        --_remaining;
        return SocketDecorator::Read(buffer);
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
        if (_remaining == 0)
            return Failure();
        --_remaining;
        return SocketDecorator::WaitReadable();
    }

  private:
    /// @return The configured failure, without touching the decorated socket.
    [[nodiscard]] IoAwaitable Failure() const noexcept
    {
        return IoAwaitable { IoResult { std::unexpected(NetError { .code = _code, .systemCode = 0, .context = {} }) } };
    }

    NetErrorCode _code;
    std::size_t _remaining;
};

} // namespace FastCache::Testing
