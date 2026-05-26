// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace FastCache
{

namespace Detail
{

#if defined(_WIN32)
    using NativeSocket = std::uintptr_t; // SOCKET on Windows
    constexpr NativeSocket InvalidSocket = static_cast<NativeSocket>(~0ull);
#else
    using NativeSocket = int;
    constexpr NativeSocket InvalidSocket = -1;
#endif

    /// One-time Winsock startup / Linux SIGPIPE setup. Idempotent.
    void EnsureNetworkInitialised();

} // namespace Detail

/// Blocking-IO ISocket implementation. Reads/Writes call the OS socket
/// API directly and resolve their awaitable synchronously. Intended for the
/// MVP's thread-per-connection model — a real reactor will replace it.
class BlockingSocket final: public ISocket
{
  public:
    explicit BlockingSocket(Detail::NativeSocket native) noexcept;
    BlockingSocket(BlockingSocket const&) = delete;
    BlockingSocket(BlockingSocket&&) = delete;
    BlockingSocket& operator=(BlockingSocket const&) = delete;
    BlockingSocket& operator=(BlockingSocket&&) = delete;
    ~BlockingSocket() override;

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override;
    void Close() noexcept override;
    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }

  private:
    Detail::NativeSocket _native;
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
    /// @return A bound and listening listener, or one in an errored state
    ///         (Accept() will immediately yield the bind error).
    [[nodiscard]] static std::unique_ptr<BlockingListener> Bind(std::string_view bindAddress,
                                                                std::uint16_t port,
                                                                int backlog = 64);

    BlockingListener(BlockingListener const&) = delete;
    BlockingListener(BlockingListener&&) = delete;
    BlockingListener& operator=(BlockingListener const&) = delete;
    BlockingListener& operator=(BlockingListener&&) = delete;
    ~BlockingListener() override;

    [[nodiscard]] AcceptAwaitable Accept() override;
    void Close() noexcept override;

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
};

} // namespace FastCache
