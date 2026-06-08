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
    EpollSocket(EpollReactor& reactor, int fd) noexcept;
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

    [[nodiscard]] int Native() const noexcept
    {
        return _fd;
    }

    /// Implementation detail; public so the .cpp dispatch bridge can reach it.
    struct Impl;

  private:
    std::unique_ptr<Impl> _impl;
    int _fd;
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

    EpollListener(EpollListener const&) = delete;
    EpollListener(EpollListener&&) = delete;
    EpollListener& operator=(EpollListener const&) = delete;
    EpollListener& operator=(EpollListener&&) = delete;
    ~EpollListener() override;

    [[nodiscard]] AcceptAwaitable Accept() override;
    void Close() noexcept override;

    [[nodiscard]] bool IsBound() const noexcept;
    [[nodiscard]] std::string_view BindError() const noexcept;

    struct Impl;

  private:
    EpollListener() noexcept;
    std::unique_ptr<Impl> _impl;
};

} // namespace FastCache

#endif // __linux__
