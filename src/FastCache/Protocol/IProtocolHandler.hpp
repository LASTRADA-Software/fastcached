// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Net/ISocket.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace FastCache
{

/// Protocol handler abstraction: drives one client connection's command
/// loop from start to disconnect. The Connection layer constructs the
/// concrete handler based on protocol autodetection and then awaits its
/// Run() task.
class IProtocolHandler
{
  public:
    IProtocolHandler() = default;
    IProtocolHandler(IProtocolHandler const&) = delete;
    IProtocolHandler(IProtocolHandler&&) = delete;
    IProtocolHandler& operator=(IProtocolHandler const&) = delete;
    IProtocolHandler& operator=(IProtocolHandler&&) = delete;
    virtual ~IProtocolHandler() = default;

    /// Drive the connection's command loop. Returns when the client
    /// disconnects, the protocol declares the session done (e.g., `quit`),
    /// or a fatal protocol error occurs.
    /// @param socket Per-connection socket; lifetime exceeds the task.
    /// @param engine Cache engine shared between connections.
    /// @param primingBytes Bytes that were peeked during protocol
    ///        autodetection and must be replayed before any socket read.
    /// @return Task that completes when the session ends.
    [[nodiscard]] virtual Task<void> Run(ISocket& socket, CacheEngine& engine, std::vector<std::byte> primingBytes) = 0;
};

} // namespace FastCache
