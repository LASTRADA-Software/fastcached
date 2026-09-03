// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(__APPLE__)

    #include <FastCache/Async/KqueueReactor.hpp>
    #include <FastCache/Net/IListener.hpp>
    #include <FastCache/Net/ISocket.hpp>
    #include <FastCache/Net/SocketAddress.hpp>

    #include <cstddef>
    #include <cstdint>
    #include <memory>
    #include <string>
    #include <string_view>

namespace FastCache
{

/// kqueue-driven ISocket. Non-blocking; Read/Write try the syscall and
/// suspend on EAGAIN. EVFILT_READ / EVFILT_WRITE wake the per-fd handler
/// which retries the syscall and completes the awaitable.
class KqueueSocket final: public ISocket
{
  public:
    /// Wrap an accepted fd driven by `reactor`.
    /// @param reactor The reactor this socket is pinned to.
    /// @param fd The accepted socket fd.
    /// @param peerAddress Printable peer host captured at accept time, or ""
    ///        when unknown. Surfaced via PeerAddress() for `--log-source`.
    KqueueSocket(KqueueReactor& reactor, int fd, std::string peerAddress = {}) noexcept;
    ~KqueueSocket() override;

    KqueueSocket(KqueueSocket const&) = delete;
    KqueueSocket& operator=(KqueueSocket const&) = delete;
    KqueueSocket(KqueueSocket&&) = delete;
    KqueueSocket& operator=(KqueueSocket&&) = delete;

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override;
    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override;
    [[nodiscard]] IoAwaitable WaitReadable() override;
    void Close() noexcept override;

    /// @copydoc ISocket::ShutdownWrite
    void ShutdownWrite() noexcept override;

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }
    [[nodiscard]] std::string PeerAddress() const override
    {
        return _peerAddress;
    }

    [[nodiscard]] int Native() const noexcept
    {
        return _fd;
    }

    struct Impl;

  private:
    std::unique_ptr<Impl> _impl;
    int _fd;
    std::string _peerAddress;
    bool _closed { false };
};

/// kqueue-driven IListener using EVFILT_READ on the listening socket.
class KqueueListener final: public IListener
{
  public:
    [[nodiscard]] static std::unique_ptr<KqueueListener> Bind(KqueueReactor& reactor,
                                                              std::string_view bindAddress,
                                                              std::uint16_t port,
                                                              int backlog = 511,
                                                              IAddressResolver& resolver = DefaultAddressResolver(),
                                                              ReusePort reusePort = ReusePort::No);

    /// Take ownership of a descriptor that is already bound and listening, and
    /// drive its accepts from `reactor`.
    ///
    /// For socket activation, where a supervisor bound the port before this
    /// process existed. It performs no bind, no listen and no `SO_REUSEADDR`:
    /// the supervisor did all three, and repeating any of them on an
    /// already-listening socket fails. It also asks the socket nothing about
    /// what it *is* -- whether the descriptor really is a listener is decided by
    /// `ParseSocketActivation`'s `LISTEN_PID` check, which is the only thing that
    /// can answer it.
    ///
    /// The reactor-driven counterpart to `BlockingListener::Adopt`, and the
    /// reason both have to exist: a blocking listener handed to a reactor
    /// endpoint parks the reactor thread inside `::accept()`, and that thread
    /// carries every other connection this process is serving.
    ///
    /// **Ownership transfers.** The returned listener closes the descriptor in
    /// its destructor, exactly as a `Bind`-ed one closes the descriptor it
    /// created -- including on the failure paths below, so a refusal is not a
    /// descriptor leak in the caller.
    ///
    /// The descriptor is switched to non-blocking and marked close-on-exec here,
    /// which is exactly what `Bind` does to its own listening socket via
    /// `PrepareOwnedFd` -- macOS has no `SOCK_NONBLOCK` socket-type flag, so the
    /// bound path applies both after the fact too. The difference is that a
    /// failure to switch is **fatal** here rather than ignored: every accept path
    /// is written around `EAGAIN`, and on a blocking descriptor the first
    /// `::accept` parks the reactor thread instead, which is the one outcome this
    /// factory exists to avoid.
    ///
    /// `Detail::ApplyHotSocketOptions` is deliberately NOT applied to the
    /// listening descriptor, because `Bind` does not apply it to its own either:
    /// it tunes a *connected* socket, and sockets accepted from this listener go
    /// through the same two accept paths a bound one's do, so they end up tuned
    /// identically.
    ///
    /// macOS is not in fact reachable from `AdoptInheritedListeners` today --
    /// only systemd's protocol is implemented, and launchd's
    /// `launch_activate_socket` is not -- so this exists as the platform twin of
    /// the epoll factory rather than as a live path.
    ///
    /// @param reactor The reactor that will drive this listener's accepts.
    /// @param fd An already-bound, already-listening descriptor. Ownership
    ///        transfers to the returned listener.
    /// @return A listener over that descriptor; on failure one with
    ///         `IsBound() == false` and `BindError()` saying what failed, which
    ///         is the convention `Bind` uses rather than throwing.
    [[nodiscard]] static std::unique_ptr<KqueueListener> Adopt(KqueueReactor& reactor, int fd);

    ~KqueueListener() override;

    KqueueListener(KqueueListener const&) = delete;
    KqueueListener& operator=(KqueueListener const&) = delete;
    KqueueListener(KqueueListener&&) = delete;
    KqueueListener& operator=(KqueueListener&&) = delete;

    [[nodiscard]] AcceptAwaitable Accept() override;
    void Close() noexcept override;
    [[nodiscard]] std::uint16_t BoundPort() const noexcept override;

    [[nodiscard]] bool IsBound() const noexcept;
    [[nodiscard]] std::string_view BindError() const noexcept;

    struct Impl;

  private:
    KqueueListener() noexcept;
    std::unique_ptr<Impl> _impl;
};

} // namespace FastCache

#endif // __APPLE__
