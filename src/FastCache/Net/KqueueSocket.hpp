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
    KqueueSocket(KqueueReactor& reactor, int fd) noexcept;
    ~KqueueSocket() override;

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override;
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

    struct Impl;

  private:
    std::unique_ptr<Impl> _impl;
    int _fd;
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

    ~KqueueListener() override;

    [[nodiscard]] AcceptAwaitable Accept() override;
    void Close() noexcept override;

    [[nodiscard]] bool IsBound() const noexcept;
    [[nodiscard]] std::string_view BindError() const noexcept;

    struct Impl;

  private:
    KqueueListener() noexcept;
    std::unique_ptr<Impl> _impl;
};

} // namespace FastCache

#endif // __APPLE__
