// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Protocol/IProtocolHandler.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Redis RESP2 protocol handler. Implements the subset of Redis commands
/// that sccache actually exercises:
///   GET, SET (with EX/PX/NX/XX), SETEX, PSETEX, DEL, EXISTS,
///   PING, ECHO, INFO, COMMAND, HELLO, QUIT, FLUSHDB, FLUSHALL
///
/// RESP3 is not supported — `HELLO 3` replies `-NOPROTO`.
/// AUTH replies with an error since no auth backend is wired in.
/// Unknown commands reply `-ERR unknown command`.
class RedisRespHandler final: public IProtocolHandler
{
  public:
    /// Returned by INFO; mirrors the memcached `version` semantics.
    [[nodiscard]] static std::string_view ServerVersion() noexcept;

    [[nodiscard]] Task<void> Run(ISocket& socket, CacheEngine& engine, std::vector<std::byte> primingBytes) override;
};

} // namespace FastCache
