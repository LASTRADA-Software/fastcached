# Redis RESP

fastcached implements a subset of the Redis command set over both RESP2
and RESP3. A connection starts in RESP2; `HELLO 3` upgrades it.

The authoritative list is `CommandTable` in
`src/FastCache/Protocol/RedisResp.cpp` (**66 CommandTable rows**) plus `AUTH`,
`QUIT` and `RESET`, which `Dispatch` handles ahead of the table because they
carry side-effects beyond a plain reply. The tables below are checked against that
table by `ctest -R resp-command-docs`, so a verb added to one and not the
other fails the build rather than drifting.

## Framing

RESP messages are length-prefixed text:

- `*N\r\n` — an array of N elements
- `$M\r\n<M bytes>\r\n` — a bulk string of M bytes
- `+<simple string>\r\n` — a simple OK-style reply
- `-<error>\r\n` — an error reply
- `:<integer>\r\n` — an integer reply

RESP3 adds `%N` (map), `~N` (set), `_` (null), `#` (boolean), `,` (double),
`(` (big number), `=` (verbatim string), `>N` (push) and `|N` (attribute).
A connection is sent RESP3 types only after `HELLO 3`; `DEBUG PROTOCOL
<type>` emits one of each for conformance clients.

Commands are issued as an array of bulk strings. Inline commands (a single
line of space-separated tokens) are also accepted for telnet debugging.

## Supported commands

### Strings and keys

| Command | Notes |
|---------|-------|
| [`GET <key>`](../commands/redis/string/get.md) | Bulk string, or nil on a miss |
| [`SET <key> <value> [EX\|PX <ttl>] [NX\|XX]`](../commands/redis/string/set.md) | Full options |
| [`SETEX <key> <ttl> <value>`](../commands/redis/string/setex.md) | TTL in seconds |
| [`PSETEX <key> <ttl_ms> <value>`](../commands/redis/string/psetex.md) | TTL in milliseconds |
| `MGET <key>...` | One reply element per key, nil per miss |
| `MSET <key> <value>...` | Always `+OK` |
| `MSETNX <key> <value>...` | `:1` only if every key was absent |
| [`DEL <key>...`](../commands/redis/keys/del.md) | Returns the count removed |
| [`UNLINK <key>...`](../commands/redis/keys/unlink.md) | Synonym for `DEL` |
| [`EXISTS <key>...`](../commands/redis/keys/exists.md) | Returns the count present |
| `INCR` / `DECR <key>` | 64-bit integer counters |
| `INCRBY` / `DECRBY <key> <delta>` | As above, by a delta |
| `INCRBYFLOAT <key> <delta>` | Double-valued; refuses a non-finite result |

### Expiry

| Command | Notes |
|---------|-------|
| `TTL` / `PTTL <key>` | Seconds / milliseconds; `:-1` no TTL, `:-2` no key |
| `EXPIRE` / `PEXPIRE <key> <ttl>` | Relative, seconds / milliseconds |
| `EXPIREAT` / `PEXPIREAT <key> <unix-time>` | Absolute, seconds / milliseconds |
| `PERSIST <key>` | Removes the TTL, leaving the value |

### Sets

A **partial** family: the seven verbs below and no others. `SINTER`,
`SUNION`, `SDIFF`, `SRANDMEMBER` and `SMOVE` return
`-ERR unknown command`.

| Command | Notes |
|---------|-------|
| `SADD <key> <member>...` | Returns the count actually added |
| `SREM <key> <member>...` | Returns the count actually removed |
| `SCARD <key>` | Cardinality |
| `SISMEMBER <key> <member>` | `:0` / `:1` |
| `SMISMEMBER <key> <member>...` | One `:0`/`:1` per member |
| `SMEMBERS <key>` | Every member; a RESP3 set after `HELLO 3` |
| `SPOP <key> [count]` | Removes and returns members |

### Streams

| Command | Notes |
|---------|-------|
| `XADD <key> <id\|*> <field> <value>...` | `*` generates the ID |
| `XLEN <key>` | Entry count |
| `XRANGE` / `XREVRANGE <key> <start> <end> [COUNT n]` | `-` and `+` accepted |
| `XREAD [COUNT n] [BLOCK ms] STREAMS <key>... <id>...` | `BLOCK` waits |
| `XDEL <key> <id>...` | Returns the count removed |
| `XTRIM <key> MAXLEN\|MINID ...` | Returns the count evicted |
| `XSETID <key> <id>` | Sets the last-delivered ID |
| `XGROUP CREATE\|SETID\|DESTROY\|CREATECONSUMER\|DELCONSUMER` | Consumer groups |
| `XREADGROUP GROUP <g> <c> ...` | Group reads, `>` for new entries |
| `XACK <key> <group> <id>...` | Acknowledges delivery |
| `XPENDING <key> <group> ...` | Summary and extended forms |
| `XCLAIM` / `XAUTOCLAIM <key> <group> ...` | Reassigns pending entries |
| `XINFO STREAM\|GROUPS\|CONSUMERS <key>` | Introspection |

### Pub / sub

| Command | Notes |
|---------|-------|
| `SUBSCRIBE` / `UNSUBSCRIBE <channel>...` | Exact-channel |
| `PSUBSCRIBE` / `PUNSUBSCRIBE <pattern>...` | Glob patterns |
| `PUBLISH <channel> <message>` | Returns the receiver count |

While a connection holds a subscription, only `SUBSCRIBE`,
`UNSUBSCRIBE`, `PSUBSCRIBE`, `PUNSUBSCRIBE`, `PING`, `QUIT`, `RESET` and
`HELLO` are accepted, as upstream requires. Keyspace notifications are
published for Redis write verbs; a memcached write dirties a `WATCH` but
publishes no event — see [Known
limitations](../operations/known-limitations.md).

### Transactions

| Command | Notes |
|---------|-------|
| `MULTI` | Opens a queue; commands reply `+QUEUED` |
| `EXEC` | Runs the queue, or aborts if a `WATCH`ed key changed |
| `DISCARD` | Drops the queue |
| `WATCH <key>...` | Optimistic locking; dirtied by writes on any protocol |
| `UNWATCH` | Clears the watch set |

### Connection and server

| Command | Notes |
|---------|-------|
| [`PING [msg]`](../commands/redis/connection/ping.md) | `+PONG` or bulk echo |
| [`ECHO <msg>`](../commands/redis/connection/echo.md) | Bulk echo |
| [`INFO`](../commands/redis/server/info.md) | Version plus basic stats (three sections) |
| [`HELLO [2\|3]`](../commands/redis/connection/hello.md) | `3` upgrades the connection to RESP3 |
| [`COMMAND [COUNT\|INFO\|DOCS]`](../commands/redis/server/command.md) | Real introspection over the 66 rows |
| [`FLUSHDB`](../commands/redis/server/flushdb.md) / [`FLUSHALL`](../commands/redis/server/flushall.md) | Drops all entries |
| [`QUIT`](../commands/redis/connection/quit.md) | `+OK` and close |
| `RESET` | `+RESET`; clears any transaction, watch set and subscription |
| [`AUTH [user] <pass>`](../commands/redis/connection/auth.md) | Against `--requirepass` |
| [`SELECT <index>`](../commands/redis/connection/select.md) | `+OK` no-op (single keyspace) |
| [`CLIENT <sub>`](../commands/redis/connection/client.md) | Connection-setup stub |
| [`CONFIG <sub>`](../commands/redis/server/config.md) | Stub: `CONFIG GET` reports `0` per param |
| `DEBUG PROTOCOL <type>` | Emits one reply of each RESP type, for conformance clients |
| `LOLWUT` | Version banner |

`SELECT`, `CLIENT` and `CONFIG` exist purely so the handshake a Redis
client library performs on connect succeeds; they hold no real state.

Only some of these verbs have a page of their own under
`docs/commands/redis/` — a missing page means nobody has written one, not
that the command is missing.

## What is not supported

Lists (`LPUSH`, `LRANGE`, …), hashes (`HSET`, `HGETALL`, …), sorted sets
(`ZADD`, `ZRANGE`, …), bitfields, scripting (`EVAL`, `SCRIPT *`), cluster
and replication commands, and the set verbs named above return
`-ERR unknown command`. See [Unsupported Redis
commands](../commands/redis/unsupported.md).

## Why a subset?

fastcached's storage engine is a key-value cache, and the surface grew
outward from what `sccache` and similar clients need. Sets and streams are
stored as tagged value blobs over that same engine, which is why those
families are partial rather than absent: a verb lands when its semantics
fit an engine that stores bytes under a key.
