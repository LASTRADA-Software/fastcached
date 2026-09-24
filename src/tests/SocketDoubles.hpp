// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <utility>

#include <core/net/ISocket.hpp>
#include <core/net/NetError.hpp>
#include <core/net/testing/SocketDecorator.hpp>

namespace FastCache::Testing
{

/// A socket whose reads FAIL with a chosen code, and whose writes still go through.
///
/// One of the two socket doubles this project's cases stage and core-cpp's `core::net::testing`
/// does not carry; both sit over its `SocketDecorator`, so every operation a double does not
/// stage is forwarded. They were `src/tests/SocketDecorator.hpp`, beside the decorator itself,
/// until #1596 moved the decorator and `ParkingReadableSocket` into core-cpp.
///
/// The writes are the point: a test asserting that a surface chose to answer nothing has to be
/// able to tell that from a surface that could not write. Decorator, therefore, and not a
/// standalone fake.
///
/// The code is a parameter because the platforms disagree about which one an expiry is -- POSIX
/// `EAGAIN`/`EWOULDBLOCK`, Winsock `WSAETIMEDOUT` -- and a fake pinning one leaves the other's
/// handling untested on the platform that uses it. See `core::net::isDeadlineExpiry`.
class FailingReadSocket final: public core::net::testing::SocketDecorator
{
  public:
    /// @param inner The socket writes, and the first `forwardFirst` reads, go to.
    /// @param code What every read after those fails with.
    /// @param forwardFirst How many reads are forwarded before the failures start. `0` fails from
    ///        the first. Any other value stages a peer that said SOMETHING and then stopped, which
    ///        is a different fact from a peer that said nothing and a different answer on the wire.
    FailingReadSocket(core::net::ISocket& inner, core::net::NetErrorCode code, std::size_t forwardFirst = 0) noexcept:
        SocketDecorator { inner },
        _code { code },
        _remaining { forwardFirst }
    {
    }

    /// Forward while the allowance lasts, then fail.
    /// @param buffer Where a forwarded read puts its bytes.
    /// @return The decorated socket's answer, or the configured failure.
    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        return TakeAllowance() ? SocketDecorator::read(buffer) : Failure();
    }

    /// The same answer `read` would give: `read` and `waitReadable` are one read operation
    /// sharing one slot, so a fake answering them from two different sockets parks a wait on a
    /// socket whose reads are never going to resolve or retrieve it.
    /// @return The decorated socket's answer, or the configured failure.
    [[nodiscard]] core::net::IoAwaitable waitReadable() override
    {
        return TakeAllowance() ? SocketDecorator::waitReadable() : Failure();
    }

  private:
    /// Spend one forwarded read, if any are left. Shared by `read` and `waitReadable` because
    /// they are ONE read operation.
    /// @return Whether this call may be forwarded to the decorated socket.
    [[nodiscard]] bool TakeAllowance() noexcept
    {
        if (_remaining == 0)
            return false;
        --_remaining;
        return true;
    }

    /// @return The configured failure, without touching the decorated socket.
    [[nodiscard]] core::net::IoAwaitable Failure() const
    {
        return core::net::IoAwaitable { std::unexpected(core::net::makeNetError(_code, 0, {})) };
    }

    core::net::NetErrorCode _code;
    std::size_t _remaining;
};

/// A socket whose writes PARK once a case says the peer has stopped reading.
///
/// **The in-memory pipe cannot stage this.** A full in-memory pipe refuses a write with
/// `WouldBlock` at once; a loop socket facing a peer that stopped reading ARMS the write and
/// suspends, and the suspended write is what a stall is. A bound on that suspension -- a hold a
/// push may not outlast -- is reachable only through a write that stays parked until something
/// retrieves it.
///
/// **Only `close()` completes a parked write**, which is the transports' own rule: there is no
/// `cancelWrite`. A flow that unwinds first retires it instead, which core-cpp's awaitable tells
/// the owner through its retire hook. Until `StopReading`, every write is forwarded.
class ParkingWritableSocket final: public core::net::testing::SocketDecorator
{
  public:
    /// @param inner The socket reads and unparked writes are forwarded to; must outlive this.
    explicit ParkingWritableSocket(core::net::ISocket& inner) noexcept:
        SocketDecorator { inner }
    {
    }

    ParkingWritableSocket(ParkingWritableSocket const&) = delete;
    ParkingWritableSocket(ParkingWritableSocket&&) = delete;
    ParkingWritableSocket& operator=(ParkingWritableSocket const&) = delete;
    ParkingWritableSocket& operator=(ParkingWritableSocket&&) = delete;

    /// Retires a write a case left parked: a frame never completed is a frame never freed.
    ~ParkingWritableSocket() override
    {
        ParkingWritableSocket::close();
    }

    /// From now on every write parks, as against a peer whose receive window has closed.
    void StopReading() noexcept
    {
        _stopped = true;
    }

    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return _stopped ? Park() : SocketDecorator::write(buffer);
    }

    [[nodiscard]] core::net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                       std::shared_ptr<void const> keepAlive = {}) override
    {
        return _stopped ? Park() : SocketDecorator::writeVectored(segments, std::move(keepAlive));
    }

    /// Completes the parked write LAST: completing resumes the awaiting coroutine inline, and a
    /// coroutine that owns the socket may destroy it before this returns.
    void close() noexcept override
    {
        auto* const parked = std::exchange(_parked, nullptr);
        SocketDecorator::close();
        if (parked == nullptr)
            return;
        ++_writesRetiredByClose;
        parked->complete(std::unexpected(core::net::makeNetError(core::net::NetErrorCode::Cancelled, 0, {})));
    }

    /// @return Whether a write is parked right now.
    [[nodiscard]] bool IsWriteParked() const noexcept
    {
        return _parked != nullptr;
    }

    /// @return How many parked writes `close` retrieved.
    [[nodiscard]] std::size_t WritesRetiredByClose() const noexcept
    {
        return _writesRetiredByClose;
    }

  private:
    /// @return A write that, once armed, only `close` completes.
    [[nodiscard]] core::net::IoAwaitable Park() noexcept
    {
        return core::net::IoAwaitable {
            [](void* owner, core::net::IoAwaitable& self) { static_cast<ParkingWritableSocket*>(owner)->_parked = &self; },
            [](void* owner, void* awaitable) noexcept {
                auto* const socket = static_cast<ParkingWritableSocket*>(owner);
                if (socket->_parked == awaitable)
                    socket->_parked = nullptr;
            },
            this,
        };
    }

    core::net::IoAwaitable* _parked { nullptr };
    bool _stopped { false };
    std::size_t _writesRetiredByClose { 0 };
};

} // namespace FastCache::Testing
