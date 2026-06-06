// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
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

/// IOCP-backed ISocket. Read/Write submit WSARecv/WSASend with an
/// embedded OVERLAPPED; completions arrive on the reactor thread which
/// resumes the suspended coroutine via the awaitable.
///
/// Lifetime: the socket holds two persistent per-direction OVERLAPPED
/// structs (one for Read, one for Write) — only one of each may be in
/// flight at a time. The buffer passed to Read/Write must outlive the
/// awaitable.
class IocpSocket final: public ISocket
{
  public:
    /// Take ownership of a connected SOCKET (passed as std::uintptr_t for
    /// header portability) and attach it to the given reactor's IOCP.
    /// @param reactor Reactor that drives this socket's I/O.
    /// @param native Native SOCKET handle.
    IocpSocket(IocpReactor& reactor, std::uintptr_t native) noexcept;
    ~IocpSocket() override;

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override;
    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override;
    void Close() noexcept override;
    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }

    /// @return Native SOCKET handle.
    [[nodiscard]] std::uintptr_t Native() const noexcept
    {
        return _native;
    }

    /// @return True if the socket was successfully associated with the
    ///         reactor's completion port in the constructor. When false, no
    ///         I/O completion will ever be dequeued for this socket, so the
    ///         caller must abandon the connection rather than await on it.
    [[nodiscard]] bool IsAttached() const noexcept
    {
        return _attached;
    }

    /// Implementation detail; declared in the public section so the
    /// suspend-callback bridge in IocpSocket.cpp can reach it.
    struct Impl;

  private:
    std::unique_ptr<Impl> _impl;
    std::uintptr_t _native;
    bool _closed { false };
    bool _attached { false };
};

/// IOCP-backed IListener using AcceptEx. Accept() submits AcceptEx with a
/// pre-created accept SOCKET; the completion resumes the caller with the
/// new IocpSocket wrapper.
class IocpListener final: public IListener
{
  public:
    /// Bind to host:port and start listening.
    /// @return Owning listener on success; one in an errored state on
    ///         failure (Accept() yields the bind error).
    [[nodiscard]] static std::unique_ptr<IocpListener> Bind(IocpReactor& reactor,
                                                            std::string_view bindAddress,
                                                            std::uint16_t port,
                                                            int backlog = 511,
                                                            IAddressResolver& resolver = DefaultAddressResolver());

    ~IocpListener() override;

    [[nodiscard]] AcceptAwaitable Accept() override;
    void Close() noexcept override;

    [[nodiscard]] bool IsBound() const noexcept;
    [[nodiscard]] std::string_view BindError() const noexcept;

    /// Implementation detail; declared in the public section so the
    /// suspend-callback bridge in IocpSocket.cpp can reach it.
    struct Impl;

  private:
    IocpListener() noexcept;

    std::unique_ptr<Impl> _impl;
};

} // namespace FastCache

#endif // _WIN32
