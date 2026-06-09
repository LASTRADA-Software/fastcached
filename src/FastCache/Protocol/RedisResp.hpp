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
/// AUTH authenticates against the session's AuthPolicy when one is configured;
/// with no policy it replies the redis "no password is set" error. When a
/// policy is configured, every data command before a successful AUTH replies
/// `-NOAUTH`.
/// Unknown commands reply `-ERR unknown command`.
class RedisRespHandler final: public IProtocolHandler
{
  public:
    /// Returned by INFO; mirrors the memcached `version` semantics.
    [[nodiscard]] static std::string_view ServerVersion() noexcept;

    [[nodiscard]] Task<void> Run(ISocket* socket,
                                 CacheEngine* engine,
                                 std::vector<std::byte> primingBytes,
                                 SessionContext session) override;

    /// Test seam: override the per-connection MULTI queue caps so unit tests
    /// can exercise the breach path without actually allocating the
    /// production default (256 MiB / 65 536 commands). A value of 0 means
    /// "keep the production default". Production code never calls this.
    /// @param maxCommands Replacement for `MaxQueuedCommands`; 0 = no change.
    /// @param maxBytes    Replacement for `MaxQueuedBytes`;     0 = no change.
    void OverrideMultiQueueCapsForTests(std::size_t maxCommands, std::size_t maxBytes) noexcept;

  private:
    /// Test-only overrides; zero means "use the module default".
    std::size_t _testMaxQueuedCommands { 0 };
    std::size_t _testMaxQueuedBytes { 0 };
};

} // namespace FastCache
