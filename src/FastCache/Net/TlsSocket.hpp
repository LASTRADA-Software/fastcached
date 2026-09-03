// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/TlsContext.hpp>

#include <array>
#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Forward declarations so this header pulls in no OpenSSL headers.
extern "C" struct ssl_st;
extern "C" struct bio_st;

namespace FastCache
{

/// ISocket decorator that terminates TLS over a raw byte transport using
/// OpenSSL memory BIOs.
///
/// OpenSSL never touches the underlying socket: plaintext written by the
/// application is encrypted into an outgoing memory BIO and pumped to the raw
/// socket, while ciphertext read from the raw socket is fed into an incoming
/// memory BIO for OpenSSL to decrypt. This is the only model that fits the
/// reactor's completion-shaped, suspend-until-complete I/O — a socket-BIO would
/// either block the event loop or fight the WANT_READ/WANT_WRITE retry contract.
///
/// Each public operation (Read / Write / WriteVectored / HandshakeIfNeeded)
/// runs an internal pump coroutine that interleaves SSL_* calls with awaited
/// raw-socket I/O. Read/Write/WriteVectored bridge that pump to the single
/// IoAwaitable their signature must return by launching it as a detached task
/// that completes the awaitable when the pump resolves.
///
/// Concurrency contract: at most one operation may be in flight at a time. The
/// request/response protocol loops already guarantee this (a connection reads a
/// command, then writes its reply); the class is not safe for a concurrent
/// read and write. Per-direction scratch buffers are reused across ops.
class TlsSocket final: public ISocket
{
  public:
    /// Wrap `raw` with server-side TLS using `context`. Cheap: allocates the
    /// SSL object and memory BIOs but performs no I/O. Drive HandshakeIfNeeded()
    /// before application reads/writes.
    /// @param raw Owned underlying transport.
    /// @param context Shared server SSL_CTX provider.
    TlsSocket(std::unique_ptr<ISocket> raw, TlsContext& context);

    TlsSocket(TlsSocket const&) = delete;
    TlsSocket(TlsSocket&&) = delete;
    TlsSocket& operator=(TlsSocket const&) = delete;
    TlsSocket& operator=(TlsSocket&&) = delete;
    ~TlsSocket() override;

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override;
    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override;
    [[nodiscard]] IoAwaitable WaitReadable() override;
    [[nodiscard]] Task<std::expected<void, NetError>> HandshakeIfNeeded() override;
    void Close() noexcept override;

    /// @copydoc ISocket::ShutdownWrite
    ///
    /// Delegated to the raw transport, and **no `close_notify` is sent** -- for the
    /// same reason `Close()` sends none: flushing a TLS alert needs an *awaited*
    /// write and this signature cannot do one. So the peer sees a transport-level
    /// half-close on a stream that is still cryptographically open, which is what a
    /// caller wants here and is not a graceful TLS shutdown.
    ///
    /// **This is the one transport where the interface's "reads keep working" is
    /// qualified.** A plaintext half-close cannot affect the read direction; here it
    /// can, because TLS reads are not read-only at the transport layer. `PumpRead`'s
    /// `SSL_ERROR_WANT_READ` path calls `FlushOutgoing()` and propagates its failure,
    /// so if OpenSSL has anything queued outbound after the half-close -- an alert, a
    /// TLS 1.3 KeyUpdate response, any post-handshake message -- the underlying
    /// `Write` fails and `Read` answers `NetError` where a plaintext socket would have
    /// answered data or a clean `0`.
    ///
    /// Left as a documented consequence rather than papered over: swallowing that
    /// flush error to make the shapes match would hide genuine write failures on
    /// every other read, which is a worse trade than a caller on the one encrypted
    /// transport having to expect an error it can already get.
    void ShutdownWrite() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override;
    /// Forward the peer address of the wrapped transport so `--log-source`
    /// works identically for TLS and plaintext connections.
    /// @return The underlying socket's peer host, or "" when unknown.
    [[nodiscard]] std::string PeerAddress() const override
    {
        return _raw ? _raw->PeerAddress() : std::string {};
    }

  private:
    /// Decrypt up to out.size() bytes into `out`, feeding ciphertext from the
    /// raw socket as OpenSSL asks for it. Resolves with the plaintext byte
    /// count, 0 on clean close/EOF, or a NetError.
    Task<IoResult> PumpRead(std::span<std::byte> out);

    /// Encrypt and send all of `in`, flushing ciphertext to the raw socket.
    /// Resolves with in.size() on success or a NetError.
    Task<IoResult> PumpWrite(std::span<std::byte const> in);

    /// Drain the outgoing BIO to the raw socket until empty.
    Task<std::expected<void, NetError>> FlushOutgoing();

    /// Read one chunk of ciphertext from the raw socket into the incoming BIO.
    /// Resolves with the byte count, 0 on EOF, or a NetError.
    Task<IoResult> FeedIncoming();

    /// Launch the read/write pump as a detached task that completes `awaitable`.
    DetachedTask DriveRead(IoAwaitable* awaitable, std::span<std::byte> out);
    DetachedTask DriveWrite(IoAwaitable* awaitable, std::span<std::byte const> in);

    std::unique_ptr<ISocket> _raw;
    ssl_st* _ssl { nullptr };
    bio_st* _incoming { nullptr }; ///< network -> SSL (ciphertext in).
    bio_st* _outgoing { nullptr }; ///< SSL -> network (ciphertext out).

    // In-flight operands for the single outstanding op (see concurrency note).
    std::span<std::byte> _readView {};
    std::span<std::byte const> _writeView {};
    std::vector<std::byte> _writeBuffer; ///< Backing store for gathered writes.

    std::array<std::byte, 16384> _inScratch {};  ///< raw read staging.
    std::array<std::byte, 16384> _outScratch {}; ///< raw write staging.
};

} // namespace FastCache
