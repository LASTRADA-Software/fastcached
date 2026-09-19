// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/InMemoryTransport.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
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

bool InMemoryPipe::CloseRead() noexcept
{
    _readClosed = true;
    return !_buffer.empty();
}

void InMemoryPipe::Reset(NetErrorCode code) noexcept
{
    _reset = true;
    _resetCode = code;
    _buffer.clear();
    if (_progressCallback)
        _progressCallback(_progressCallbackState);
}

std::size_t InMemoryPipe::TryPull(std::span<std::byte> into) noexcept
{
    auto const take = std::min(into.size(), _buffer.size());
    for (auto const i: std::views::iota(std::size_t { 0 }, take))
    {
        into[i] = _buffer.front();
        _buffer.pop_front();
    }
    return take;
}

// -- InMemorySocket --------------------------------------------------------

namespace
{
    /// The peer closed over bytes it had not read, so its stack sent the reset at once:
    /// `ECONNRESET` and `WSAECONNRESET`, which `ISocket` calls `ConnReset`.
    constexpr auto ResetByPeersClose = NetErrorCode::ConnReset;

    /// This end wrote after the peer's FIN, and that write drew the reset: `EPIPE` and
    /// `WSAECONNABORTED`, which `ISocket` calls `SystemError`. Measured on loopback (#1553).
    constexpr auto AbortedByOwnWrite = NetErrorCode::SystemError;

    /// The error a reset connection answers with, on both of its operations.
    /// @param code How the reset arrived, as the pipe recorded it.
    /// @return The error.
    [[nodiscard]] NetError ResetError(NetErrorCode code) noexcept
    {
        return NetError { .code = code, .systemCode = 0, .context = "InMemorySocket: the connection was reset" };
    }
} // namespace

InMemorySocket::InMemorySocket(std::shared_ptr<InMemoryPipe> inbound,
                               std::shared_ptr<InMemoryPipe> outbound,
                               std::string peerAddress) noexcept:
    _inbound { std::move(inbound) },
    _outbound { std::move(outbound) },
    _peerAddress { std::move(peerAddress) }
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

    // Detached before anything below can fire it, so this socket's own parked read is
    // answered by the `Cancelled` at the end and never by the reset it sends the peer.
    if (_inbound)
        _inbound->SetProgressCallback(nullptr, nullptr);

    // A close with the peer's bytes still unread is answered with an RST rather than a
    // FIN, by every stack measured (#1553): the peer's writes fail from the next one on,
    // and its reads report the reset. Delivered before the FIN, so a peer parked on a
    // read wakes to the reset and not to an EOF that a real socket would never report.
    if (_inbound && _inbound->CloseRead() && _outbound)
    {
        _inbound->Reset(ResetByPeersClose);
        _outbound->Reset(ResetByPeersClose);
    }
    if (_outbound)
        _outbound->CloseWrite();

    // **A parked Read is retrieved here or it is never retrieved at all.** Clearing
    // the progress callback above is what makes that final: nothing else can complete
    // this awaitable, so the awaiting coroutine is never resumed and its frame is
    // never freed. `EpollSocket::Close` has always done this; the fake every test's
    // socket runs on did not, and a fake nothing exercises does not report its own
    // bugs -- it went unseen because the in-memory `Read` only parks when the peer
    // has neither written nor closed, which most fixtures avoid by construction.
    //
    // Detached FIRST and completed LAST, with no member touched afterwards, for the
    // reason `EpollSocket::Close` records: completing resumes the awaiting coroutine,
    // and a coroutine that OWNS this socket runs to its end and destroys it before
    // `Complete` returns.
    auto* const parked = std::exchange(_pendingRead, nullptr);
    _pendingReadBuffer = {};
    if (parked != nullptr)
        parked->Complete(std::unexpected(NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = {} }));
}

void InMemorySocket::CancelRead() noexcept
{
    // Detached FIRST and completed LAST, exactly as `Close()` above does and for the
    // reason recorded there. Unlike `Close()` this leaves the socket usable: the
    // progress callback stays installed and a later `Read` works, which is the whole
    // difference between retiring an operation and tearing the connection down.
    auto* const parked = std::exchange(_pendingRead, nullptr);
    if (parked == nullptr)
        return;
    _pendingReadBuffer = {};
    parked->Complete(std::unexpected(NetError { .code = NetErrorCode::Cancelled, .systemCode = 0, .context = {} }));
}

void InMemorySocket::ShutdownWrite() noexcept
{
    // After `Close()` there is no write side left to shut, and each platform socket
    // checks `_closed` and returns; this one did the half-close a second time.
    if (_closed)
        return;
    if (_outbound)
        _outbound->CloseWrite();
}

IoAwaitable InMemorySocket::WaitReadable()
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // A reset is an error on every real reactor (#899), and not a readable byte.
    if (_inbound && _inbound->IsReset())
        return IoAwaitable { std::unexpected(ResetError(_inbound->ResetCode())) };

    // EOF is drained AND write-closed, which is the same pair `Read` uses to decide it
    // has reached the end -- taken from the inbound pipe, since that is the direction
    // this socket reads from.
    if (_inbound && _inbound->Buffered() == 0 && _inbound->IsWriteClosed())
        return IoAwaitable { IoResult { std::size_t { 0 } } };

    // Anything else reports data, which is the default's answer and the fail-safe one:
    // a caller that reads and finds nothing has lost a syscall, where a caller wrongly
    // told EOF has lost its peer.
    return IoAwaitable { IoResult { std::size_t { 1 } } };
}

IoAwaitable InMemorySocket::Read(std::span<std::byte> buffer)
{
    Detail::RequireReadBuffer(buffer);
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    if (_inbound->IsReset())
        return IoAwaitable { std::unexpected(ResetError(_inbound->ResetCode())) };

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

std::optional<IoResult> InMemorySocket::AnswerIfCannotWrite(std::size_t length) noexcept
{
    // This end's own half-close is `EPIPE` on POSIX and `WSAESHUTDOWN` on Windows, both
    // `SystemError`. The pipe used to answer it as backpressure -- `WouldBlock`, the one
    // failure a caller is entitled to retry.
    if (_outbound->IsWriteClosed())
        return std::unexpected(NetError { .code = NetErrorCode::SystemError,
                                          .systemCode = 0,
                                          .context = "InMemorySocket: a write after this end's ShutdownWrite" });
    if (_outbound->IsReset())
        return std::unexpected(ResetError(_outbound->ResetCode()));
    if (!_outbound->IsReadClosed() || length == 0)
        return std::nullopt;

    // The first write after a graceful close: the bytes reach a peer that has gone, and
    // are lost, and its stack answers with the RST that fails everything after. On a
    // real socket that answer arrives a round trip later -- on loopback, before the next
    // call, measured on Linux, macOS and Windows even with the writes back to back (#1553).
    _outbound->Reset(AbortedByOwnWrite);
    _inbound->Reset(AbortedByOwnWrite);
    return IoResult { length };
}

IoAwaitable InMemorySocket::Write(std::span<std::byte const> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };
    if (auto answer = AnswerIfCannotWrite(buffer.size()))
        return IoAwaitable { std::move(*answer) };

    auto const accepted = _outbound->Push(buffer);
    if (accepted < buffer.size())
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::WouldBlock, .systemCode = 0, .context = "InMemoryPipe backpressure" }) };
    return IoAwaitable { IoResult { accepted } };
}

IoAwaitable InMemorySocket::WriteVectored(std::span<std::span<std::byte const> const> segments,
                                          std::shared_ptr<void const> /*keepAlive*/)
{
    if (_closed)
        return IoAwaitable { std::unexpected(
            NetError { .code = NetErrorCode::BadFileHandle, .systemCode = 0, .context = {} }) };

    // One write on the wire, so one answer when the peer has gone: the vectored write is
    // the first after a close, or a later one, exactly as a contiguous one would be.
    std::size_t length = 0;
    for (auto const seg: segments)
        length += seg.size();
    if (auto answer = AnswerIfCannotWrite(length))
        return IoAwaitable { std::move(*answer) };

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

    if (self->_inbound->IsReset())
    {
        awaitable->Complete(std::unexpected(ResetError(self->_inbound->ResetCode())));
        return;
    }

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

InMemorySocketPair InMemorySocketPair::Create(std::size_t maxBytesInFlight, std::string serverPeerAddress)
{
    auto clientToServer = std::make_shared<InMemoryPipe>(maxBytesInFlight);
    auto serverToClient = std::make_shared<InMemoryPipe>(maxBytesInFlight);
    InMemorySocketPair pair;
    pair.client = std::make_unique<InMemorySocket>(serverToClient, clientToServer);
    pair.server = std::make_unique<InMemorySocket>(clientToServer, serverToClient, std::move(serverPeerAddress));
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
    //
    // Registered from inside `await_suspend`, exactly as `InMemorySocket::Read` does
    // and for the same reason: at that moment `self` is the awaitable living in the
    // AWAITING coroutine's frame. This local is returned by value and destroyed, so
    // recording `&awaitable` here left `_pendingAwaitable` naming storage that was
    // already gone -- and both `Close` and `TryCompletePendingAccept` then completed
    // through it. Latent only because every case in this file used the lazy `Task`
    // shape and reached `Shutdown()` before anything parked
    // ([#734](https://github.com/LASTRADA-Software/fastcached/issues/734)).
    AcceptAwaitable awaitable;
    awaitable.SetSuspendCallback(&InMemoryListener::OnAcceptSuspended, this);
    return awaitable;
}

void InMemoryListener::OnAcceptSuspended(AcceptAwaitable* awaitable, std::coroutine_handle<> /*handle*/) noexcept
{
    auto* const self = static_cast<InMemoryListener*>(awaitable->CallbackState());
    self->_pendingAwaitable = awaitable;
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

std::unique_ptr<InMemorySocket> InMemoryListener::ConnectClient(std::size_t maxBytesInFlight, std::string peerAddress)
{
    auto pair = InMemorySocketPair::Create(maxBytesInFlight, std::move(peerAddress));
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
