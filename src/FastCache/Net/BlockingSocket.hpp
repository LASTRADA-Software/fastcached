// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Errors/NetError.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace FastCache
{

// Forward declarations for the bind-time resolver DI seam (defined in
// SocketAddress.hpp, which includes this header — so only a forward
// declaration is possible here).
class IAddressResolver;
[[nodiscard]] IAddressResolver& DefaultAddressResolver() noexcept;

namespace Detail
{

#if defined(_WIN32)
    using NativeSocket = std::uintptr_t; // SOCKET on Windows
    constexpr NativeSocket InvalidSocket = static_cast<NativeSocket>(~0ULL);
#else
    using NativeSocket = int;
    constexpr NativeSocket InvalidSocket = -1;
#endif

    /// One-time Winsock startup / Linux SIGPIPE setup. Idempotent.
    void EnsureNetworkInitialised();

    /// Apply latency-critical socket options to a freshly accepted or
    /// connected stream socket. Currently sets TCP_NODELAY so that small
    /// request/response writes are not delayed by Nagle's algorithm — which,
    /// combined with delayed ACK, can add tens (Linux) to hundreds (Windows)
    /// of milliseconds per round trip on the request hot path. Best-effort:
    /// a failure is ignored and leaves the OS default in place.
    /// @param socket The connected stream-socket handle to tune.
    void ApplyHotSocketOptions(NativeSocket socket) noexcept;

    /// Accept one connection from a listening socket and return its raw native
    /// handle (with TCP_NODELAY applied, and non-blocking on POSIX). Used by
    /// the multi-reactor server loop: a single acceptor accepts here, then
    /// hands the raw handle to one reactor which wraps it. The handle is NOT
    /// associated with any reactor yet.
    /// @param listenSocket A bound, listening socket.
    /// @return The accepted socket handle, or a NetError (Cancelled-like when
    ///         the listening socket was closed to unblock the accept).
    [[nodiscard]] std::expected<NativeSocket, NetError> AcceptRaw(NativeSocket listenSocket) noexcept;

    /// Close a raw native socket handle — e.g. the listening socket, to unblock
    /// a thread parked in AcceptRaw().
    /// @param socket The handle to close.
    void CloseNativeSocket(NativeSocket socket) noexcept;

    /// Apply receive/send timeouts (SO_RCVTIMEO / SO_SNDTIMEO) to a socket.
    /// A non-positive duration leaves the OS default (no timeout) in place.
    /// Best-effort: a setsockopt failure is ignored. Used to bound blocking
    /// recv()/accept() so a stalled peer cannot park a thread indefinitely.
    /// @param socket The socket handle to tune.
    /// @param recvTimeout Receive timeout (also bounds ::accept() on POSIX).
    /// @param sendTimeout Send timeout.
    void SetIoTimeouts(NativeSocket socket,
                       std::chrono::milliseconds recvTimeout,
                       std::chrono::milliseconds sendTimeout) noexcept;

} // namespace Detail

/// Blocking-IO ISocket implementation. Reads/Writes call the OS socket
/// API directly and resolve their awaitable synchronously. Intended for the
/// MVP's thread-per-connection model — a real reactor will replace it.
class BlockingSocket final: public ISocket
{
  public:
    /// Wrap a native socket handle.
    /// @param native The accepted/connected socket handle.
    /// @param peerAddress Printable peer host captured at accept time, or ""
    ///        when unknown. Surfaced via PeerAddress() for `--log-source`.
    explicit BlockingSocket(Detail::NativeSocket native, std::string peerAddress = {}) noexcept;
    BlockingSocket(BlockingSocket const&) = delete;
    BlockingSocket(BlockingSocket&&) = delete;
    BlockingSocket& operator=(BlockingSocket const&) = delete;
    BlockingSocket& operator=(BlockingSocket&&) = delete;
    ~BlockingSocket() override;

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override;
    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override;
    void Close() noexcept override;
    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }
    [[nodiscard]] std::string PeerAddress() const override
    {
        return _peerAddress;
    }

  private:
    Detail::NativeSocket _native;
    std::string _peerAddress;
    bool _closed { false };
};

/// Blocking-IO TCP IListener. Binds to host:port on construction; Accept()
/// blocks the calling thread until a peer connects (or Close() unblocks it
/// via socket teardown).
class BlockingListener final: public IListener
{
  public:
    /// Bind and listen.
    /// @param bindAddress IPv4/IPv6 host or "0.0.0.0".
    /// @param port TCP port number.
    /// @param backlog ::listen backlog.
    /// @param resolver Resolver for the bind host (DI seam over getaddrinfo);
    ///        defaults to the process-wide system resolver.
    /// @return A bound and listening listener, or one in an errored state
    ///         (Accept() will immediately yield the bind error).
    [[nodiscard]] static std::unique_ptr<BlockingListener> Bind(std::string_view bindAddress,
                                                                std::uint16_t port,
                                                                int backlog = 511,
                                                                IAddressResolver& resolver = DefaultAddressResolver());

    BlockingListener(BlockingListener const&) = delete;
    BlockingListener(BlockingListener&&) = delete;
    BlockingListener& operator=(BlockingListener const&) = delete;
    BlockingListener& operator=(BlockingListener&&) = delete;
    ~BlockingListener() override;

    [[nodiscard]] AcceptAwaitable Accept() override;
    void Close() noexcept override;

    /// Enable bounded shutdown + slowloris protection (off by default).
    /// @param acceptPoll Receive timeout for the listening socket so Accept()
    ///        returns NetErrorCode::WouldBlock periodically, letting the accept
    ///        loop re-check a shutdown flag (POSIX does not unblock a parked
    ///        accept() on Close()). Zero leaves accept() fully blocking.
    /// @param ioTimeout Receive/send timeout applied to every accepted socket so
    ///        a stalled client cannot wedge a blocking recv(). Zero disables it.
    void SetTimeouts(std::chrono::milliseconds acceptPoll, std::chrono::milliseconds ioTimeout) noexcept;

    /// @return true if the listener bound and listens cleanly.
    [[nodiscard]] bool IsBound() const noexcept
    {
        return _native != Detail::InvalidSocket;
    }

    /// @return Error context recorded at Bind() time if binding failed.
    [[nodiscard]] std::string_view BindError() const noexcept
    {
        return _bindError;
    }

  private:
    BlockingListener() = default;

    Detail::NativeSocket _native { Detail::InvalidSocket };
    std::string _bindError;
    /// Receive/send timeout applied to accepted sockets (0 = none). Set via
    /// SetTimeouts; the accept-poll timeout is applied directly to _native there.
    std::chrono::milliseconds _ioTimeout { 0 };
};

} // namespace FastCache
