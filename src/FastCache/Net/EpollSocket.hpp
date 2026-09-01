// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(__linux__)

    #include <FastCache/Async/EpollReactor.hpp>
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

/// epoll-driven ISocket. Non-blocking; Read/Write try the syscall first
/// and only suspend on EAGAIN. Per-direction op state holds the awaitable
/// and the buffer span; on EPOLLIN/OUT the reactor calls back into the
/// socket which retries the syscall and completes the awaitable.
class EpollSocket final: public ISocket
{
  public:
    /// Wrap an accepted, non-blocking fd driven by `reactor`.
    /// @param reactor The reactor this socket is pinned to.
    /// @param fd The accepted socket fd.
    /// @param peerAddress Printable peer host captured at accept time, or ""
    ///        when unknown. Surfaced via PeerAddress() for `--log-source`.
    EpollSocket(EpollReactor& reactor, int fd, std::string peerAddress = {}) noexcept;
    EpollSocket(EpollSocket const&) = delete;
    EpollSocket(EpollSocket&&) = delete;
    EpollSocket& operator=(EpollSocket const&) = delete;
    EpollSocket& operator=(EpollSocket&&) = delete;
    ~EpollSocket() override;

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable WaitReadable() override;
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

    [[nodiscard]] int Native() const noexcept
    {
        return _fd;
    }

    /// Implementation detail; public so the .cpp dispatch bridge can reach it.
    struct Impl;

  private:
    std::unique_ptr<Impl> _impl;
    int _fd;
    std::string _peerAddress;
    bool _closed { false };
};

/// epoll-driven IListener. accept(4) is non-blocking; the listener fd is
/// armed for EPOLLIN. When readable, the reactor calls back into the
/// listener which accept()s the connection and completes the awaitable.
class EpollListener final: public IListener
{
  public:
    [[nodiscard]] static std::unique_ptr<EpollListener> Bind(EpollReactor& reactor,
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
    /// the unit did all three, and repeating any of them on an already-listening
    /// socket fails. It also asks the socket nothing about what it *is* --
    /// whether the descriptor really is a listener is decided by
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
    /// Two descriptor properties are applied here rather than assumed, because
    /// they are precisely what `Bind`'s `SOCK_NONBLOCK | SOCK_CLOEXEC` gives the
    /// socket it creates, and systemd hands a descriptor over with neither:
    ///
    ///  - **Non-blocking**, and a failure to apply it fails the adopt rather
    ///    than being ignored. Every accept path here is written around `EAGAIN`;
    ///    on a blocking descriptor the first `accept4` parks the reactor thread
    ///    instead, which is the one outcome this factory exists to avoid.
    ///  - **Close-on-exec**, best-effort, as `Detail::ArmCloseOnExec` is
    ///    everywhere else. A worker spawns a compiler per job, and a child left
    ///    holding the listening socket keeps the port after the worker is gone.
    ///
    /// `Detail::ApplyHotSocketOptions` is deliberately NOT applied to the
    /// listening descriptor, because `Bind` does not apply it to its own either:
    /// it tunes a *connected* socket, and sockets accepted from this listener go
    /// through the same two accept paths a bound one's do, so they end up tuned
    /// identically.
    ///
    /// @param reactor The reactor that will drive this listener's accepts.
    /// @param fd An already-bound, already-listening descriptor. Ownership
    ///        transfers to the returned listener.
    /// @return A listener over that descriptor; on failure one with
    ///         `IsBound() == false` and `BindError()` saying what failed, which
    ///         is the convention `Bind` uses rather than throwing.
    [[nodiscard]] static std::unique_ptr<EpollListener> Adopt(EpollReactor& reactor, int fd);

    EpollListener(EpollListener const&) = delete;
    EpollListener(EpollListener&&) = delete;
    EpollListener& operator=(EpollListener const&) = delete;
    EpollListener& operator=(EpollListener&&) = delete;
    ~EpollListener() override;

    [[nodiscard]] AcceptAwaitable Accept() override;
    void Close() noexcept override;
    [[nodiscard]] std::uint16_t BoundPort() const noexcept override;

    [[nodiscard]] bool IsBound() const noexcept;
    [[nodiscard]] std::string_view BindError() const noexcept;

    struct Impl;

  private:
    EpollListener() noexcept;
    std::unique_ptr<Impl> _impl;
};

} // namespace FastCache

#endif // __linux__
