// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/TlsSocket.hpp>

#include <cstddef>
#include <limits>
#include <span>
#include <utility>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace FastCache
{

namespace
{

    /// Clamp a size to the positive `int` range OpenSSL's length parameters use.
    [[nodiscard]] int ClampInt(std::size_t value) noexcept
    {
        constexpr auto Max = static_cast<std::size_t>(std::numeric_limits<int>::max());
        return static_cast<int>(value > Max ? Max : value);
    }

    [[nodiscard]] NetError SslFailure(NetErrorCode code, std::string context)
    {
        return NetError { .code = code, .systemCode = 0, .context = std::move(context) };
    }

} // namespace

TlsSocket::TlsSocket(std::unique_ptr<ISocket> raw, TlsContext& context):
    _raw { std::move(raw) },
    _ssl { SSL_new(context.Native()) },
    _incoming { BIO_new(BIO_s_mem()) },
    _outgoing { BIO_new(BIO_s_mem()) }
{
    if (_ssl != nullptr && _incoming != nullptr && _outgoing != nullptr)
    {
        // SSL takes ownership of both BIOs; SSL_free will free them.
        SSL_set_bio(_ssl, _incoming, _outgoing);
        SSL_set_accept_state(_ssl);
        return;
    }

    // Partial allocation (OOM): the objects were never wired together, so
    // SSL_free would NOT free the BIOs (SSL_set_bio was not called). Free each
    // individually and null every handle, leaving a cleanly-dead instance:
    // nothing leaks, and HandshakeIfNeeded()'s `_ssl == nullptr` check reports
    // the failure instead of driving SSL_accept on a BIO-less SSL.
    if (_ssl != nullptr)
        SSL_free(_ssl);
    if (_incoming != nullptr)
        BIO_free(_incoming);
    if (_outgoing != nullptr)
        BIO_free(_outgoing);
    _ssl = nullptr;
    _incoming = nullptr;
    _outgoing = nullptr;
}

TlsSocket::~TlsSocket()
{
    if (_ssl != nullptr)
    {
        SSL_free(_ssl); // also frees the BIOs handed to SSL_set_bio
    }
    else
    {
        if (_incoming != nullptr)
            BIO_free(_incoming);
        if (_outgoing != nullptr)
            BIO_free(_outgoing);
    }
}

Task<std::expected<void, NetError>> TlsSocket::FlushOutgoing()
{
    while (true)
    {
        int const n = BIO_read(_outgoing, _outScratch.data(), ClampInt(_outScratch.size()));
        if (n <= 0)
            co_return std::expected<void, NetError> {}; // BIO drained (memory BIO returns <0 when empty)
        auto const written =
            co_await _raw->Write(std::span<std::byte const> { _outScratch.data(), static_cast<std::size_t>(n) });
        if (!written.has_value())
            co_return std::unexpected(written.error());
    }
}

Task<IoResult> TlsSocket::FeedIncoming()
{
    auto const read = co_await _raw->Read(std::span<std::byte> { _inScratch.data(), _inScratch.size() });
    if (!read.has_value())
        co_return std::unexpected(read.error());
    if (*read == 0)
        co_return IoResult { std::size_t { 0 } };
    BIO_write(_incoming, _inScratch.data(), ClampInt(*read));
    co_return IoResult { *read };
}

Task<IoResult> TlsSocket::PumpRead(std::span<std::byte> out)
{
    if (out.empty())
        co_return IoResult { std::size_t { 0 } };

    while (true)
    {
        ERR_clear_error(); // accurate SSL_get_error classification (esp. on a reactor thread shared by many connections)
        int const n = SSL_read(_ssl, out.data(), ClampInt(out.size()));
        if (n > 0)
            co_return IoResult { static_cast<std::size_t>(n) };

        switch (SSL_get_error(_ssl, n))
        {
            case SSL_ERROR_ZERO_RETURN: // peer sent close_notify
                co_return IoResult { std::size_t { 0 } };
            case SSL_ERROR_WANT_READ: {
                if (auto const flushed = co_await FlushOutgoing(); !flushed.has_value())
                    co_return std::unexpected(flushed.error());
                auto const fed = co_await FeedIncoming();
                if (!fed.has_value())
                    co_return std::unexpected(fed.error());
                if (*fed == 0)
                    co_return IoResult { std::size_t { 0 } }; // raw EOF before a full record
                break;
            }
            case SSL_ERROR_WANT_WRITE: {
                if (auto const flushed = co_await FlushOutgoing(); !flushed.has_value())
                    co_return std::unexpected(flushed.error());
                break;
            }
            default:
                co_return std::unexpected(SslFailure(NetErrorCode::ConnReset, "SSL_read failed"));
        }
    }
}

Task<IoResult> TlsSocket::PumpWrite(std::span<std::byte const> in)
{
    std::size_t total = 0;
    while (total < in.size())
    {
        ERR_clear_error(); // accurate SSL_get_error classification across multiplexed connections
        int const n = SSL_write(_ssl, in.data() + total, ClampInt(in.size() - total));
        if (n > 0)
        {
            total += static_cast<std::size_t>(n);
            if (auto const flushed = co_await FlushOutgoing(); !flushed.has_value())
                co_return std::unexpected(flushed.error());
            continue;
        }

        auto const err = SSL_get_error(_ssl, n);
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
        {
            if (auto const flushed = co_await FlushOutgoing(); !flushed.has_value())
                co_return std::unexpected(flushed.error());
            if (err == SSL_ERROR_WANT_READ)
            {
                auto const fed = co_await FeedIncoming();
                if (!fed.has_value())
                    co_return std::unexpected(fed.error());
                if (*fed == 0)
                    co_return std::unexpected(SslFailure(NetErrorCode::Eof, "EOF during SSL_write"));
            }
            continue;
        }
        co_return std::unexpected(SslFailure(NetErrorCode::ConnReset, "SSL_write failed"));
    }
    co_return IoResult { in.size() };
}

Task<std::expected<void, NetError>> TlsSocket::HandshakeIfNeeded()
{
    if (_ssl == nullptr)
        co_return std::unexpected(SslFailure(NetErrorCode::SystemError, "TLS not initialised"));

    while (true)
    {
        ERR_clear_error(); // accurate SSL_get_error classification for this handshake step
        int const r = SSL_accept(_ssl);
        if (r == 1)
            co_return co_await FlushOutgoing(); // push the final handshake flight

        switch (SSL_get_error(_ssl, r))
        {
            case SSL_ERROR_WANT_READ: {
                if (auto const flushed = co_await FlushOutgoing(); !flushed.has_value())
                    co_return std::unexpected(flushed.error());
                auto const fed = co_await FeedIncoming();
                if (!fed.has_value())
                    co_return std::unexpected(fed.error());
                if (*fed == 0)
                    co_return std::unexpected(SslFailure(NetErrorCode::Eof, "EOF during TLS handshake"));
                break;
            }
            case SSL_ERROR_WANT_WRITE: {
                if (auto const flushed = co_await FlushOutgoing(); !flushed.has_value())
                    co_return std::unexpected(flushed.error());
                break;
            }
            default:
                co_return std::unexpected(SslFailure(NetErrorCode::ConnReset, "TLS handshake failed"));
        }
    }
}

DetachedTask TlsSocket::DriveRead(IoAwaitable* awaitable, std::span<std::byte> out)
{
    auto const result = co_await PumpRead(out);
    awaitable->Complete(result);
    co_return;
}

DetachedTask TlsSocket::DriveWrite(IoAwaitable* awaitable, std::span<std::byte const> in)
{
    auto const result = co_await PumpWrite(in);
    awaitable->Complete(result);
    co_return;
}

IoAwaitable TlsSocket::Read(std::span<std::byte> buffer)
{
    _readView = buffer;
    IoAwaitable awaitable;
    awaitable.SetSuspendCallback(
        [](IoAwaitable* self, std::coroutine_handle<>) {
            auto* const tls = static_cast<TlsSocket*>(self->CallbackState());
            tls->DriveRead(self, tls->_readView);
        },
        this);
    return awaitable;
}

IoAwaitable TlsSocket::WaitReadable()
{
    // If OpenSSL already has decoded plaintext buffered (a previous record was
    // larger than the application's last read), report ready synchronously so
    // the caller's Read returns immediately. Otherwise the next readable edge
    // is at the raw transport — defer to it.
    if (_ssl != nullptr && SSL_pending(_ssl) > 0)
        return IoAwaitable { IoResult { std::size_t { 1 } } };
    return _raw->WaitReadable();
}

IoAwaitable TlsSocket::Write(std::span<std::byte const> buffer)
{
    _writeView = buffer;
    IoAwaitable awaitable;
    awaitable.SetSuspendCallback(
        [](IoAwaitable* self, std::coroutine_handle<>) {
            auto* const tls = static_cast<TlsSocket*>(self->CallbackState());
            tls->DriveWrite(self, tls->_writeView);
        },
        this);
    return awaitable;
}

IoAwaitable TlsSocket::WriteVectored(std::span<std::span<std::byte const> const> segments,
                                     std::shared_ptr<void const> keepAlive)
{
    // TLS cannot cheaply gather-encrypt arbitrary segments, so we copy them into
    // one contiguous buffer up front (the zero-copy GET fast path is only lost
    // on TLS connections). Copying synchronously means the caller's segments and
    // keepAlive need not outlive this call.
    static_cast<void>(keepAlive);
    _writeBuffer.clear();
    std::size_t total = 0;
    for (auto const segment: segments)
        total += segment.size();
    _writeBuffer.reserve(total);
    for (auto const segment: segments)
        _writeBuffer.insert(_writeBuffer.end(), segment.begin(), segment.end());
    _writeView = std::span<std::byte const> { _writeBuffer.data(), _writeBuffer.size() };

    IoAwaitable awaitable;
    awaitable.SetSuspendCallback(
        [](IoAwaitable* self, std::coroutine_handle<>) {
            auto* const tls = static_cast<TlsSocket*>(self->CallbackState());
            tls->DriveWrite(self, tls->_writeView);
        },
        this);
    return awaitable;
}

void TlsSocket::Close() noexcept
{
    // Deliberately no SSL_shutdown / close_notify: Close() is a synchronous
    // noexcept teardown, but flushing a close_notify alert needs an *awaited*
    // write to the raw socket (the alert lands in the outgoing BIO), which this
    // signature cannot do under the reactor I/O model. The peer therefore sees a
    // transport-level FIN/RST without close_notify — a truncated TLS stream. For
    // a cache request/response protocol that is acceptable: every reply is fully
    // written and flushed before Close(), so no application data is lost; the
    // only effect is that strict clients may log a truncation warning and cannot
    // resume the TLS session. Sending close_notify cleanly would require an
    // async Shutdown() coroutine, which is out of scope here.
    _raw->Close();
}

bool TlsSocket::IsClosed() const noexcept
{
    return _raw->IsClosed();
}

} // namespace FastCache
