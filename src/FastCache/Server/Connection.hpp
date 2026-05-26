// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <memory>

namespace FastCache
{

/// One client connection. Owns the socket, detects the protocol, hands off
/// to the matching protocol handler, and ends when that handler returns.
///
/// The Connection itself does not own a per-connection BufferPool yet —
/// allocations go through the global allocator. A later pass can add the
/// pool without changing this interface.
class Connection
{
  public:
    /// Construct over a freshly-accepted socket.
    /// @param socket Owned socket; closed on connection end.
    /// @param engine Shared cache engine.
    /// @param logger Shared logger.
    Connection(std::unique_ptr<ISocket> socket, CacheEngine& engine, ILogger& logger) noexcept;

    /// Run the connection's protocol loop to completion.
    /// @return Task that resolves when the connection closes.
    [[nodiscard]] Task<void> Run();

  private:
    std::unique_ptr<ISocket> _socket;
    CacheEngine& _engine;
    ILogger& _logger;
};

} // namespace FastCache
