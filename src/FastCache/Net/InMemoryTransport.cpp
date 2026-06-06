// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/InMemoryTransport.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace FastCache
{

// -- InMemoryPipe ----------------------------------------------------------

InMemoryPipe::InMemoryPipe(std::size_t maxBytesInFlight) noexcept:
    _maxInFlight { maxBytesInFlight }
{
}

std::size_t InMemoryPipe::Push(std::span<std::byte const> bytes)
{
    if (_writeClosed)
        return 0;

    auto accepted = bytes.size();
    if (_maxInFlight != 0)
    {
        auto const headroom = _maxInFlight > _buffer.size() ? _maxInFlight - _buffer.size() : std::size_t { 0 };
        accepted = std::min(accepted, headroom);
    }

    for (auto const b: bytes.first(accepted))
        _buffer.push_back(b);

    if (accepted > 0 && _progressCallback)
        _progressCallback(_progressCallbackState);

    return accepted;
}

void InMemoryPipe::CloseWrite() noexcept
{
    _writeClosed = true;
    if (_progressCallback)
        _progressCallback(_progressCallbackState);
}

std::size_t InMemoryPipe::TryPull(std::span<std::byte> into) noexcept
{
    auto const take = std::min(into.size(), _buffer.size());
    for (std::size_t i = 0; i < take; ++i)
    {
        into[i] = _buffer.front();
        _buffer.pop_front();
    }
    return take;
}

// -- InMemorySocket --------------------------------------------------------

InMemorySocket::InMemorySocket(std::shared_ptr<InMemoryPipe> inbound, std::shared_ptr<InMemoryPipe> outbound) noexcept:
    _inbound { std::move(inbound) },
    _outbound { std::move(outbound) }
{
    _inbound->SetProgressCallback(&InMemorySocket::OnInboundProgress, this);
}

InMemorySocket::~InMemorySocket()
{
    InMemorySocket::Close();
}

void InMemorySocket::Close() noexcept
{
    if (_closed)
        return;
    _closed = true;
    if (_outbound)
        _outbound->CloseWrite();
    if (_inbound)
        _inbound->SetProgressCallback(nullptr, nullptr);
}

void InMemorySocket::ShutdownWrite() noexcept
{
    if (_outbound)
        _outbound->CloseWrite();
}

IoAwaitable InMemorySocket::Read(std::span<std::byte> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // Synchronous fast path: bytes already buffered.
    if (_inbound->Buffered() > 0)
    {
        auto const got = _inbound->TryPull(buffer);
        return IoAwaitable { IoResult { got } };
    }

    // EOF: peer closed and nothing left.
    if (_inbound->IsWriteClosed())
        return IoAwaitable { IoResult { 0 } };

    // Park: install a callback so a future Push wakes us up. The awaitable
    // must be registered from inside await_suspend — at that point `self` is
    // the awaitable living in the awaiting coroutine's frame, not this local
    // (which is moved out on return and then destroyed). Recording &awaitable
    // here would leave _pendingRead dangling and _handle unset.
    _pendingReadBuffer = buffer;
    IoAwaitable awaitable;
    awaitable.SetSuspendCallback(&InMemorySocket::OnReadSuspended, this);
    return awaitable;
}

IoAwaitable InMemorySocket::Write(std::span<std::byte const> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    auto const accepted = _outbound->Push(buffer);
    if (accepted < buffer.size())
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::WouldBlock, .systemCode = 0, .context = "InMemoryPipe backpressure" }) };
    return IoAwaitable { IoResult { accepted } };
}

IoAwaitable InMemorySocket::WriteVectored(std::span<std::span<std::byte const> const> segments,
                                          std::shared_ptr<void> /*keepAlive*/)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // Push each segment in order so the peer observes the exact same byte
    // stream a single contiguous Write would have produced. The keep-alive is
    // unnecessary here: Push copies the bytes into the pipe synchronously, so
    // nothing outlives this call.
    std::size_t total = 0;
    for (auto const seg: segments)
    {
        auto const accepted = _outbound->Push(seg);
        total += accepted;
        if (accepted < seg.size())
            return IoAwaitable { std::unexpected(
                NetError { .code = NetErrorCode::WouldBlock, .systemCode = 0, .context = "InMemoryPipe backpressure" }) };
    }
    return IoAwaitable { IoResult { total } };
}

void InMemorySocket::OnReadSuspended(IoAwaitable* awaitable, std::coroutine_handle<> /*handle*/) noexcept
{
    auto* const self = static_cast<InMemorySocket*>(awaitable->CallbackState());
    self->_pendingRead = awaitable;
}

void InMemorySocket::OnInboundProgress(void* state) noexcept
{
    auto* self = static_cast<InMemorySocket*>(state);
    if (!self->_pendingRead)
        return;

    auto* const awaitable = self->_pendingRead;
    auto const buffer = self->_pendingReadBuffer;
    self->_pendingRead = nullptr;
    self->_pendingReadBuffer = {};

    if (self->_inbound->Buffered() > 0)
    {
        auto const got = self->_inbound->TryPull(buffer);
        awaitable->Complete(IoResult { got });
        return;
    }

    if (self->_inbound->IsWriteClosed())
    {
        awaitable->Complete(IoResult { 0 });
        return;
    }
}

// -- InMemorySocketPair ----------------------------------------------------

InMemorySocketPair InMemorySocketPair::Create(std::size_t maxBytesInFlight)
{
    auto clientToServer = std::make_shared<InMemoryPipe>(maxBytesInFlight);
    auto serverToClient = std::make_shared<InMemoryPipe>(maxBytesInFlight);
    InMemorySocketPair pair;
    pair.client = std::make_unique<InMemorySocket>(serverToClient, clientToServer);
    pair.server = std::make_unique<InMemorySocket>(clientToServer, serverToClient);
    return pair;
}

// -- InMemoryListener ------------------------------------------------------

InMemoryListener::InMemoryListener() = default;
InMemoryListener::~InMemoryListener() = default;

AcceptAwaitable InMemoryListener::Accept()
{
    // Drain any pre-queued connections first — closing the listener with
    // queued connections still in the pipeline lets the server finish them
    // before observing the shutdown.
    if (!_ready.empty())
    {
        auto front = std::move(_ready.front());
        _ready.pop_front();
        return AcceptAwaitable { AcceptResult { std::move(front.socket) } };
    }

    if (_closed)
        return AcceptAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = {} }) };

    // No connection waiting — park until ConnectClient() or Close() fires.
    AcceptAwaitable awaitable;
    _pendingAwaitable = &awaitable;
    return awaitable;
}

void InMemoryListener::Close() noexcept
{
    _closed = true;
    if (_pendingAwaitable)
    {
        auto* const a = std::exchange(_pendingAwaitable, nullptr);
        a->Complete(std::unexpected(NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = {} }));
    }
}

std::unique_ptr<InMemorySocket> InMemoryListener::ConnectClient(std::size_t maxBytesInFlight)
{
    auto pair = InMemorySocketPair::Create(maxBytesInFlight);
    _ready.push_back(Pending { .socket = std::move(pair.server) });
    TryCompletePendingAccept();
    return std::move(pair.client);
}

void InMemoryListener::TryCompletePendingAccept()
{
    if (!_pendingAwaitable || _ready.empty())
        return;
    auto* const a = std::exchange(_pendingAwaitable, nullptr);
    auto front = std::move(_ready.front());
    _ready.pop_front();
    a->Complete(AcceptResult { std::move(front.socket) });
}

} // namespace FastCache
