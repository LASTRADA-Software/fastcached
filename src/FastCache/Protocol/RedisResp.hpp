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

/// Redis RESP protocol handler. Negotiates RESP2 or RESP3 per the client's
/// `HELLO <version>` (any other version replies `-NOPROTO`); a connection
/// starts in RESP2 and may upgrade once HELLO succeeds. `RESET` and a later
/// `HELLO 2` both downgrade the connection back to RESP2.
///
/// Under RESP3 the encoder uses the dedicated wire types — null `_`, boolean
/// `#t`/`#f`, double `,<n>` (with `inf`/`-inf`/`nan` spellings), big number
/// `(<digits>`, verbatim `=<len>\r\n<fmt>:<text>`, map `%`, set `~`, push
/// `>`, attribute `|` — and pub/sub deliveries arrive as out-of-band Push
/// frames. Under RESP2 the same replies fall back to the array/bulk-string
/// representations every RESP2 client expects (Map flattens to a `*<2N>`
/// array, Set to a `*<N>` array, attributes are dropped, etc.).
///
/// The authoritative supported-command list is the `CommandTable` dispatch
/// array in `RedisResp.cpp` (every row also feeds `COMMAND` introspection).
/// At time of writing it covers — non-exhaustively, in broad strokes — the
/// Redis surface that maps onto fastcached's engine primitives:
/// GET/SET (with EX/PX/NX/XX), SETEX/PSETEX, MGET/MSET/MSETNX, DEL/UNLINK,
/// EXISTS, EXPIRE/PEXPIRE/EXPIREAT/PEXPIREAT, TTL/PTTL, PERSIST,
/// INCR/DECR/INCRBY/DECRBY/INCRBYFLOAT, SADD/SREM/SCARD/SISMEMBER/SMEMBERS/
/// SMISMEMBER/SPOP, PING/ECHO/INFO/COMMAND/CLIENT/CONFIG/SELECT,
/// MULTI/EXEC/DISCARD/WATCH/UNWATCH, SUBSCRIBE/UNSUBSCRIBE/PSUBSCRIBE/
/// PUNSUBSCRIBE/PUBLISH, HELLO/RESET/QUIT, FLUSHDB/FLUSHALL, DEBUG PROTOCOL.
/// If this prose drifts from `CommandTable`, `CommandTable` is the source of
/// truth.
///
/// AUTH authenticates against the session's AuthPolicy when one is configured;
/// with no policy it replies the redis "no password is set" error. When a
/// policy is configured, every data command before a successful AUTH replies
/// `-NOAUTH`. The modern RESP3 handshake `HELLO 3 AUTH <user> <pass>`
/// authenticates and switches version in the same round trip.
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
