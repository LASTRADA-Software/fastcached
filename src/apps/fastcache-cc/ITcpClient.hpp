// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

/// A blocking TCP connection to the fastcached daemon.
///
/// This is the launcher's network seam. Sockets are ambient I/O with different
/// APIs per platform (Winsock vs BSD sockets), so the launcher reaches them
/// through this interface: the caching flow stays platform-free, and tests
/// drive it with an in-memory fake instead of a live daemon.
class ITcpClient
{
  public:
    ITcpClient() = default;
    virtual ~ITcpClient() = default;
    ITcpClient(ITcpClient const&) = delete;
    ITcpClient& operator=(ITcpClient const&) = delete;
    ITcpClient(ITcpClient&&) = delete;
    ITcpClient& operator=(ITcpClient&&) = delete;

    /// Send every byte of `bytes`, looping over partial writes.
    /// @param bytes The payload to write.
    /// @return True if all bytes were sent; false on any socket error.
    [[nodiscard]] virtual bool SendAll(std::span<std::byte const> bytes) = 0;

    /// Read exactly `count` bytes, looping over partial reads.
    /// @param count Number of bytes required.
    /// @return The bytes, or nullopt if the peer closed or errored first.
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> RecvExactly(std::size_t count) = 0;
};

/// Connect to `hostPort` ("host:port").
///
/// The host may be a hostname, an IPv4 literal, or a bracketed IPv6 literal
/// (`[::1]:11211`); resolution goes through getaddrinfo and every returned
/// address is tried in turn.
///
/// `ioTimeout` bounds each individual blocking send/recv on the returned
/// connection, so a daemon that accepts and then stalls mid-reply cannot hang
/// the build — the call fails and the caller falls back to a real compile.
/// Note this is a per-call deadline, not a deadline for a whole transfer: a
/// peer that dribbles bytes slower than the timeout can still take longer. It
/// bounds the failure mode that matters (a peer that stops entirely).
///
/// @param hostPort The endpoint, e.g. "127.0.0.1:11211".
/// @param ioTimeout Per-call send/recv deadline; zero or negative means no
///                  timeout (the OS default, i.e. block indefinitely).
/// @return A connected client, or nullptr if the endpoint is malformed or
///         unreachable — callers treat that as "cache unavailable" and fall
///         back to a real compile.
[[nodiscard]] std::unique_ptr<ITcpClient> ConnectTcp(std::string_view hostPort, std::chrono::milliseconds ioTimeout);

} // namespace FastCache::Cc
