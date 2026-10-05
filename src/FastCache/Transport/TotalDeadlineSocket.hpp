// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <core/net/ISocket.hpp>
#include <core/net/NetError.hpp>
#include <core/net/SocketContract.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache
{

/// A socket whose every read is bounded by what is left of ONE deadline over the whole exchange.
///
/// **A per-call receive timeout is not a bound on an exchange**: a peer that sends one byte just
/// inside it, forever, keeps the reader waiting forever, one call at a time. So this re-arms the
/// decorated socket's receive deadline before EVERY read to exactly the time remaining until one
/// instant, read off an INJECTED clock, and refuses a read or a write outright once that instant
/// has passed -- so however the peer paces its bytes, the exchange ends by the deadline plus one
/// timer's granularity. A blocking socket turns the re-arm into its `SO_RCVTIMEO`; a reactor socket
/// into its loop's timer.
///
/// **What it does not bound is a write already blocking**: a request is written before the reply
/// is awaited, and an exchange whose request is a few dozen bytes is not one a peer can stall that
/// way. `Expired()` says whether the deadline is what ended the exchange, which a caller needs in
/// order to say *too slow* rather than *gone*.
class TotalDeadlineSocket final: public core::net::ISocket
{
  public:
    /// @param inner The connected socket every operation is forwarded to; owned.
    /// @param clock Where "now" comes from; must outlive this.
    /// @param deadline When the whole exchange must be over.
    TotalDeadlineSocket(std::unique_ptr<core::net::ISocket> inner,
                        core::platform::IClock const& clock,
                        core::platform::SteadyTimePoint deadline) noexcept:
        _inner { std::move(inner) },
        _clock { clock },
        _deadline { deadline }
    {
    }

    TotalDeadlineSocket(TotalDeadlineSocket const&) = delete;
    TotalDeadlineSocket(TotalDeadlineSocket&&) = delete;
    TotalDeadlineSocket& operator=(TotalDeadlineSocket const&) = delete;
    TotalDeadlineSocket& operator=(TotalDeadlineSocket&&) = delete;
    ~TotalDeadlineSocket() override = default;

    /// Whether the deadline has refused an operation, and so is what ended the exchange.
    /// @return True once a read or a write found no time left.
    [[nodiscard]] bool Expired() const noexcept
    {
        return _expired;
    }

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        core::net::contract::requireReadBuffer(buffer);
        if (!ArmForRemaining())
            return core::net::IoAwaitable { std::unexpected(TimedOut()) };
        return _inner->read(buffer);
    }

    [[nodiscard]] core::net::ResultAwaitable<core::net::ReadWithFd> readWithFd(std::span<std::byte> buffer) override
    {
        core::net::contract::requireReadBuffer(buffer);
        if (!ArmForRemaining())
            return core::net::ResultAwaitable<core::net::ReadWithFd> { std::unexpected(TimedOut()) };
        return _inner->readWithFd(buffer);
    }

    [[nodiscard]] core::net::IoAwaitable waitReadable() override
    {
        if (!ArmForRemaining())
            return core::net::IoAwaitable { std::unexpected(TimedOut()) };
        return _inner->waitReadable();
    }

    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        if (!TimeLeft())
            return core::net::IoAwaitable { std::unexpected(TimedOut()) };
        return _inner->write(buffer);
    }

    [[nodiscard]] core::net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                       std::shared_ptr<void const> keepAlive = {}) override
    {
        if (!TimeLeft())
            return core::net::IoAwaitable { std::unexpected(TimedOut()) };
        return _inner->writeVectored(segments, std::move(keepAlive));
    }

    /// A TLS handshake READS from the peer, so it is bounded as a read is: armed with the time left,
    /// and refused once none is. Forwarded bare, a peer dribbling its half of the handshake held the
    /// exchange past the one deadline meant to bound all of it.
    [[nodiscard]] core::net::ResultAwaitable<void> handshakeIfNeeded() override
    {
        if (!ArmForRemaining())
            return core::net::ResultAwaitable<void> { std::unexpected(TimedOut()) };
        return _inner->handshakeIfNeeded();
    }

    void cancelRead() noexcept override
    {
        _inner->cancelRead();
    }

    [[nodiscard]] core::net::ResultAwaitable<void> shutdownWrite() override
    {
        return _inner->shutdownWrite();
    }

    /// A caller's own receive deadline, kept and applied only where it is TIGHTER than the time
    /// left: it may shorten a read, never lengthen one past the exchange's deadline.
    /// @param deadline How long a read may wait; non-positive lifts the caller's own bound.
    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override
    {
        _callerDeadline = deadline;
    }

    [[nodiscard]] std::string peerAddress() const override
    {
        return _inner->peerAddress();
    }

    void close() noexcept override
    {
        _inner->close();
    }

    [[nodiscard]] bool isClosed() const noexcept override
    {
        return _inner->isClosed();
    }

  private:
    /// Whether any time is left, recording the expiry when none is.
    /// @return True while the deadline lies ahead.
    [[nodiscard]] bool TimeLeft() noexcept
    {
        if (_clock.now() < _deadline)
            return true;
        _expired = true;
        return false;
    }

    /// Bound the next read to the time left -- or to the caller's own deadline where tighter.
    /// @return False, arming nothing, when no time is left.
    [[nodiscard]] bool ArmForRemaining() noexcept
    {
        auto const now = _clock.now();
        if (now >= _deadline)
        {
            _expired = true;
            return false;
        }
        // Rounded UP to whole milliseconds and never zero, which the socket reads as "no bound".
        auto remaining =
            std::max(std::chrono::ceil<std::chrono::milliseconds>(_deadline - now), std::chrono::milliseconds { 1 });
        if (_callerDeadline > std::chrono::milliseconds::zero())
            remaining = std::min(remaining, _callerDeadline);
        _inner->setReceiveDeadline(remaining);
        return true;
    }

    /// What a refused operation answers.
    /// @return A timeout naming the exchange's deadline.
    [[nodiscard]] static core::net::NetError TimedOut()
    {
        return core::net::NetError { .code = core::net::NetErrorCode::Timeout,
                                     .systemCode = 0,
                                     .context = "the exchange's deadline has passed" };
    }

    std::unique_ptr<core::net::ISocket> _inner;
    core::platform::IClock const& _clock;
    core::platform::SteadyTimePoint _deadline;
    std::chrono::milliseconds _callerDeadline { 0 };
    bool _expired { false };
};

} // namespace FastCache
