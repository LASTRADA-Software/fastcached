// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/NetError.hpp>

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

    /// One-time Winsock startup. Idempotent, and a no-op away from Windows —
    /// SIGPIPE is handled per socket by `ArmNoSigPipe` below.
    void EnsureNetworkInitialised();

    /// The last error the socket API reported on this thread.
    ///
    /// `WSAGetLastError()` on Windows, `errno` elsewhere — which is why every
    /// caller has to go through a seam rather than reading one of them directly.
    /// @return The platform error code.
    [[nodiscard]] int LastNetworkError() noexcept;

    /// Map a platform socket error onto the shared taxonomy.
    ///
    /// Declared here rather than kept private to `BlockingSocket.cpp` because it
    /// is the **only** classification of these codes in the tree, and a second one
    /// is a second answer. `BlockingConnector` had that second answer: a
    /// three-condition ladder that reported `EACCES` — a firewall or a privileged
    /// port, the two most likely reasons an outbound connection is refused
    /// administratively — as an unclassified `SystemError`, so a caller matching
    /// on `PermissionDenied` never saw it. A code with no row maps to
    /// `SystemError`, and `systemCode` still carries the original either way.
    /// @param code A `WSAGetLastError()` / `errno` value.
    /// @return The category it belongs to.
    [[nodiscard]] NetErrorCode TranslateSocketError(int code) noexcept;

    /// Keep a write to this socket's broken pipe from raising a fatal signal.
    ///
    /// Declared here rather than kept private to `BlockingSocket.cpp` for the same
    /// reason `TranslateSocketError` is: there must be exactly one answer, and a
    /// second socket implementation needs it. Every stream socket this library
    /// hands out must be armed at the point it is constructed.
    ///
    /// **Per socket, never process-wide.** The short spelling is one
    /// `::signal(SIGPIPE, SIG_IGN)` at start-up, and it is wrong for any process
    /// that also spawns a child: an ignored disposition is *inherited across exec*,
    /// so it stops being a property of this program and becomes a property of every
    /// program it launches. `fastcache-compile-node` links this library, listens on
    /// a socket and then runs a compiler per job, so it was handing every one of
    /// those compilers a disposition they never asked for — the same defect
    /// `fastcache-cc` is documented as having to avoid, reached by another route.
    ///
    /// On Linux this is a no-op and `MSG_NOSIGNAL` on each `::send` carries the
    /// suppression instead, so a socket implementation that sends for itself must
    /// pass that flag rather than relying on this call.
    ///
    /// Best-effort: a `setsockopt` failure is ignored.
    /// @param socket The freshly accepted or connected stream socket.
    void ArmNoSigPipe(NativeSocket socket) noexcept;

    /// The `::send` flags a raw sender must pass to complete the suppression
    /// `ArmNoSigPipe` starts.
    ///
    /// The two halves are one rule and neither is sufficient alone: on macOS and
    /// the BSDs `SO_NOSIGPIPE` arms the descriptor and this is 0, while on Linux
    /// there is nothing to arm and `MSG_NOSIGNAL` per send is the whole of the
    /// protection. A caller that arms and then passes `0` is therefore correct on
    /// one platform and fatally wrong on the other -- which is what `HealthProbe`
    /// did, so `fastcached --healthcheck` against a peer that hung up mid-request
    /// died of signal 13 on Linux instead of reporting the peer unhealthy.
    ///
    /// A function rather than a constant because the value is a platform macro,
    /// and this header deliberately keeps the platform socket headers out --
    /// `NativeSocket` is a `uintptr_t` here for the same reason.
    ///
    /// `EpollSocket` passes `MSG_NOSIGNAL` literally instead, which is equivalent:
    /// it is compiled only where that macro exists.
    /// @return Flags to OR into every `::send` on a socket this layer owns.
    [[nodiscard]] int NoSignalSendFlags() noexcept;

    /// Put a socket into non-blocking mode.
    ///
    /// One definition because there were four -- two in `BlockingConnector.cpp`
    /// (one per platform) and one each in `EpollSocket.cpp` and
    /// `KqueueSocket.cpp` -- and the connectors would have made a fifth. The
    /// same argument `TranslateSocketError` records: there must be exactly one
    /// answer to a question the whole layer asks.
    ///
    /// @param socket The socket handle to switch.
    /// @return true when the mode was applied; false leaves the socket blocking,
    ///         which for a caller about to park on readiness is fatal rather
    ///         than cosmetic and must not be ignored.
    [[nodiscard]] bool SetNonBlocking(NativeSocket socket) noexcept;

    /// Put a socket back into blocking mode.
    /// @param socket The socket handle to switch.
    /// @return true when the mode was applied.
    [[nodiscard]] bool SetBlocking(NativeSocket socket) noexcept;

    /// Mark a socket close-on-exec, so a child process does not inherit it.
    ///
    /// It matters because `fastcache-compile-node` spawns a compiler for every job,
    /// and a compiler holding an open peer connection keeps it alive after the
    /// worker is gone -- the same shape of defect `AdoptInheritedListeners` records
    /// for the listening socket, reached from the accepted and dialled sides.
    ///
    /// **Neither platform gives this for free, and Windows is not exempt.** The
    /// `SOCK_CLOEXEC` the epoll and kqueue accept paths pass is theirs alone;
    /// `BlockingListener::Accept` uses a plain `::accept()` on both platforms and a
    /// dialled socket a plain `::socket`, and neither is close-on-exec. On Windows
    /// a handle has its own `HANDLE_FLAG_INHERIT` and a socket arrives with it SET
    /// -- this was documented here as a no-op on the belief that inheritance was
    /// opt-in per `CreateProcess`, which is what let every accepted connection reach
    /// every compiler the node spawned.
    ///
    /// Called from `ApplyHotSocketOptions`, which every socket this process owns
    /// passes through; there is no second place to remember. Best-effort.
    /// @param socket The socket handle to mark.
    void ArmCloseOnExec(NativeSocket socket) noexcept;

    /// Build a `NetError` from a platform code and a description of the attempt.
    /// @param code A `WSAGetLastError()` / `errno` value.
    /// @param context What was being attempted, for the message.
    /// @return The structured error.
    [[nodiscard]] NetError MakeNetError(int code, std::string context);

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

    /// Owns a native socket handle until it is either released or destroyed.
    ///
    /// A dial has half a dozen ways to fail between `::socket` and handing the
    /// handle to an `ISocket`, and every one of them has to close it. Written out
    /// by hand that is a `CloseNativeSocket` call per early return -- five of them
    /// in `BlockingConnector::DialOne` alone -- and the one that gets forgotten is
    /// a descriptor leak in the error path, which is the path that runs when a
    /// peer is down and therefore the path that runs repeatedly.
    ///
    /// Deliberately minimal: it is a scope guard for a handle mid-construction,
    /// not a socket abstraction. `ISocket` is that.
    class OwnedNativeSocket
    {
      public:
        /// @param socket Handle to take ownership of; may be `InvalidSocket`.
        explicit OwnedNativeSocket(NativeSocket socket) noexcept:
            _socket { socket }
        {
        }

        OwnedNativeSocket(OwnedNativeSocket const&) = delete;
        OwnedNativeSocket& operator=(OwnedNativeSocket const&) = delete;

        OwnedNativeSocket(OwnedNativeSocket&& other) noexcept:
            _socket { other.Release() }
        {
        }

        OwnedNativeSocket& operator=(OwnedNativeSocket&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                _socket = other.Release();
            }
            return *this;
        }

        ~OwnedNativeSocket()
        {
            Reset();
        }

        /// @return The handle, still owned here.
        [[nodiscard]] NativeSocket Get() const noexcept
        {
            return _socket;
        }

        /// @return Whether a real handle is held.
        [[nodiscard]] bool Valid() const noexcept
        {
            return _socket != InvalidSocket;
        }

        /// Give up ownership without closing.
        /// @return The handle, now the caller's responsibility.
        [[nodiscard]] NativeSocket Release() noexcept
        {
            auto const socket = _socket;
            _socket = InvalidSocket;
            return socket;
        }

        /// Close now, if anything is held. Idempotent.
        void Reset() noexcept
        {
            if (_socket != InvalidSocket)
            {
                CloseNativeSocket(_socket);
                _socket = InvalidSocket;
            }
        }

      private:
        NativeSocket _socket { InvalidSocket };
    };

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

    /// Take ownership of a descriptor that is already bound and listening.
    ///
    /// For socket activation, where a supervisor bound the port before this
    /// process existed. It performs no bind and no listen -- doing either on an
    /// already-listening socket fails -- and asks the socket nothing: whether the
    /// descriptor really is a listener is decided by `ParseSocketActivation`'s
    /// `LISTEN_PID` check, which is the only thing that can answer it.
    ///
    /// @param native An already-listening descriptor; ownership transfers, so the
    ///        returned listener closes it.
    /// @return A listener over that descriptor.
    [[nodiscard]] static std::unique_ptr<BlockingListener> Adopt(Detail::NativeSocket native);

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

    /// The port actually bound, which is what an ephemeral bind is for.
    ///
    /// Binding port 0 lets the OS choose a free one, and this is how the caller
    /// learns which. That matters beyond convenience: a test or fixture that
    /// hard-codes a port collides with whatever else is running on the machine,
    /// and the failure reads as the feature under test being broken rather than
    /// as the port being taken — the lesson `dist-compile-e2e` records for its
    /// own four ports.
    /// @return The bound port in host byte order, or 0 when not bound or when
    ///         the OS would not report it.
    [[nodiscard]] std::uint16_t BoundPort() const noexcept override;

  private:
    BlockingListener() = default;

    Detail::NativeSocket _native { Detail::InvalidSocket };
    std::string _bindError;
    /// Receive/send timeout applied to accepted sockets (0 = none). Set via
    /// SetTimeouts; the accept-poll timeout is applied directly to _native there.
    std::chrono::milliseconds _ioTimeout { 0 };
};

} // namespace FastCache
