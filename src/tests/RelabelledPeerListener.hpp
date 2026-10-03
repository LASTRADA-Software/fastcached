// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>

namespace FastCache::Testing
{

/// An accepted connection that says it comes from a fixed host, and is otherwise the real socket.
///
/// Every operation is forwarded unchanged -- the reads, the writes, the deadlines, the half-close and
/// the cancellation are the kernel's -- so what a case over it exercises is the endpoint's real
/// framing, deadlines and AUTH state machine. Only `peerAddress()` answers differently, and it is the
/// one thing every admission question starts from.
class RelabelledPeerSocket final: public core::net::ISocket
{
  public:
    /// @param inner The accepted socket (owned).
    /// @param host What `peerAddress()` reports, e.g. `"10.0.0.7"`.
    RelabelledPeerSocket(std::unique_ptr<core::net::ISocket> inner, std::string host) noexcept:
        _inner { std::move(inner) },
        _host { std::move(host) }
    {
    }

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        return _inner->read(buffer);
    }

    [[nodiscard]] core::net::ResultAwaitable<core::net::ReadWithFd> readWithFd(std::span<std::byte> buffer) override
    {
        return _inner->readWithFd(buffer);
    }

    [[nodiscard]] core::net::IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return _inner->write(buffer);
    }

    [[nodiscard]] core::net::IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                                       std::shared_ptr<void const> keepAlive) override
    {
        return _inner->writeVectored(segments, std::move(keepAlive));
    }

    [[nodiscard]] core::net::ResultAwaitable<void> handshakeIfNeeded() override
    {
        return _inner->handshakeIfNeeded();
    }

    [[nodiscard]] core::net::IoAwaitable waitReadable() override
    {
        return _inner->waitReadable();
    }

    void cancelRead() noexcept override
    {
        _inner->cancelRead();
    }

    [[nodiscard]] core::net::ResultAwaitable<void> shutdownWrite() override
    {
        return _inner->shutdownWrite();
    }

    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override
    {
        _inner->setReceiveDeadline(deadline);
    }

    /// @return The host this connection was relabelled with, never the real peer's.
    [[nodiscard]] std::string peerAddress() const override
    {
        return _host;
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
    std::unique_ptr<core::net::ISocket> _inner;
    std::string _host;
};

/// A bound listener whose accepted connections say they come from `host`.
///
/// **What makes the #235 arrangement in-process.** Loopback is admitted before any other route is
/// asked, so a fixture whose client is loopback cannot test who is admitted at all; a real socket
/// that REPORTS a non-loopback peer reaches every other route, with the endpoint's real framing,
/// deadlines and AUTH state machine in between. A case using it keeps a control on the SAME
/// fixture with the listener not relabelled, which is served as loopback: that the refusal legs
/// depend on the relabelling is then shown rather than assumed.
class RelabelledPeerListener final: public core::net::IListener
{
  public:
    /// @param inner The bound listener (owned).
    /// @param host What every accepted connection's `peerAddress()` reports.
    RelabelledPeerListener(std::unique_ptr<core::net::IListener> inner, std::string host) noexcept:
        _inner { std::move(inner) },
        _host { std::move(host) }
    {
    }

    [[nodiscard]] core::async::Task<core::net::AcceptResult> accept() override
    {
        auto accepted = co_await _inner->accept();
        if (!accepted.has_value())
            co_return accepted;
        co_return core::net::AcceptResult { std::make_unique<RelabelledPeerSocket>(std::move(*accepted), _host) };
    }

    void close() noexcept override
    {
        _inner->close();
    }

    [[nodiscard]] std::uint16_t boundPort() const noexcept override
    {
        return _inner->boundPort();
    }

  private:
    std::unique_ptr<core::net::IListener> _inner;
    std::string _host;
};

} // namespace FastCache::Testing
