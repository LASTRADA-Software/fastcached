// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace FastCache
{

class InMemoryTransport;

/// Shared FIFO byte pipe between two InMemorySockets. One side's Write
/// places bytes into the pipe; the other side's Read drains them. Reads
/// suspend when no bytes are available and a peer write completes them.
class InMemoryPipe
{
  public:
    /// Construct an empty pipe.
    /// @param maxBytesInFlight Soft cap; once reached, Writes refuse more
    /// bytes until Reads drain. Zero disables backpressure.
    explicit InMemoryPipe(std::size_t maxBytesInFlight = 0) noexcept;

    /// Push bytes into the pipe; if a Read awaitable is parked it gets
    /// resumed with the data.
    /// @param bytes Source data.
    /// @return Number of bytes accepted (may be less than bytes.size() if
    /// the in-flight cap is non-zero and would be exceeded).
    std::size_t Push(std::span<std::byte const> bytes);

    /// Mark the writing end as closed. Any blocked Read resumes with EOF.
    void CloseWrite() noexcept;

    /// Try to pull bytes synchronously. Returns the number copied; 0 with
    /// IsWriteClosed() means EOF.
    /// @param into Destination span.
    /// @return Bytes copied; <= into.size().
    std::size_t TryPull(std::span<std::byte> into) noexcept;

    /// @return true if the writing end has been closed.
    [[nodiscard]] bool IsWriteClosed() const noexcept { return _writeClosed; }

    /// @return Number of bytes currently buffered.
    [[nodiscard]] std::size_t Buffered() const noexcept { return _buffer.size(); }

    /// Register a callback to be invoked when a Push or CloseWrite happens.
    /// Used by InMemorySocket::Read to wire its awaitable to the pipe's
    /// progress signal.
    using ProgressCallback = void (*)(void* state);
    void SetProgressCallback(ProgressCallback callback, void* state) noexcept
    {
        _progressCallback = callback;
        _progressCallbackState = state;
    }

  private:
    std::size_t _maxInFlight;
    std::deque<std::byte> _buffer;
    bool _writeClosed { false };
    ProgressCallback _progressCallback { nullptr };
    void* _progressCallbackState { nullptr };
};

/// In-process ISocket: one end of a paired InMemoryPipe pair. Reads pull
/// from the inbound pipe; writes push into the outbound pipe.
class InMemorySocket: public ISocket
{
  public:
    /// Construct over two pipes (one for each direction).
    /// @param inbound Pipe this socket reads from (peer's outbound).
    /// @param outbound Pipe this socket writes into (peer's inbound).
    InMemorySocket(std::shared_ptr<InMemoryPipe> inbound, std::shared_ptr<InMemoryPipe> outbound) noexcept;
    ~InMemorySocket() override;

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override;
    void Close() noexcept override;
    [[nodiscard]] bool IsClosed() const noexcept override { return _closed; }

    /// Half-close: signal EOF to the peer on the outbound pipe but leave
    /// the inbound (this socket's read side) open. Used by tests that need
    /// to send a complete request, let the server consume it, and still
    /// read the response back.
    void ShutdownWrite() noexcept;

  private:
    static void OnInboundProgress(void* state) noexcept;

    std::shared_ptr<InMemoryPipe> _inbound;
    std::shared_ptr<InMemoryPipe> _outbound;
    IoAwaitable* _pendingRead { nullptr };
    std::span<std::byte> _pendingReadBuffer {};
    bool _closed { false };
};

/// Bidirectional helper: create a pair of InMemorySockets that talk to each
/// other. Convenient for protocol tests.
struct InMemorySocketPair
{
    std::unique_ptr<InMemorySocket> client;
    std::unique_ptr<InMemorySocket> server;

    /// Construct a pair with the given backpressure cap (0 = unlimited).
    /// @param maxBytesInFlight Per-direction in-flight byte cap.
    /// @return Two sockets sharing a pair of pipes.
    [[nodiscard]] static InMemorySocketPair Create(std::size_t maxBytesInFlight = 0);
};

/// Test listener: tests Push() pre-made server sockets onto its accept
/// queue; Server::Run() pulls them in turn via Accept(). Closing the
/// listener resumes any pending Accept with NetErrorCode::Cancelled.
class InMemoryListener final: public IListener
{
  public:
    InMemoryListener();
    ~InMemoryListener() override;

    [[nodiscard]] AcceptAwaitable Accept() override;
    void Close() noexcept override;

    /// Make a connected pair, retain the server side internally so the next
    /// Accept() resolves to it, and return the client side for the test.
    /// @param maxBytesInFlight Per-direction backpressure cap.
    /// @return Client-side socket the test drives.
    [[nodiscard]] std::unique_ptr<InMemorySocket> ConnectClient(std::size_t maxBytesInFlight = 0);

  private:
    struct Pending
    {
        std::unique_ptr<ISocket> socket;
    };

    /// If a pending Accept awaitable is parked AND we have a queued socket,
    /// complete the awaitable.
    void TryCompletePendingAccept();

    std::deque<Pending> _ready;
    AcceptAwaitable* _pendingAwaitable { nullptr };
    bool _closed { false };
};

} // namespace FastCache
