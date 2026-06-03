# Compatibility with upstream

A per-protocol scorecard against the latest released versions of the
upstream projects.

## memcached (text + binary + meta)

Reference: [memcached 1.6.x][mc].

[mc]: https://github.com/memcached/memcached

| Area                                    | Status |
|-----------------------------------------|--------|
| Text storage commands                   | Full   |
| Text retrieval commands (`get` / `gets`) | Full  |
| Text TTL family (`touch` / `gat` / `gats`) | Full |
| Text deletion                            | Full  |
| Text arithmetic (`incr` / `decr`)        | Full  |
| Text `flush_all` (with delay)            | Full  |
| Text `stats` (no sub)                    | Full  |
| Text `stats settings`                    | Synthetic (subset of settings) |
| Text `stats items`                       | Synthetic (single LRU class) |
| Text `stats slabs`                       | Synthetic (one virtual class) |
| Text `stats sizes`                       | Synthetic (single bucket) |
| Text `stats conns`                       | Empty (no connection registry) |
| Text `stats reset`                       | Acknowledged, no-op |
| Text `cache_memlimit`                    | Full  |
| Text `verbosity`                         | No-op (fastcached uses ILogger) |
| Text `shutdown`                          | Not supported |
| Text `slabs reassign` / `slabs automove` | Stub returns OK |
| Text `lru tune` / `lru mode`             | Stub returns OK |
| Text `lru_crawler`                       | Stub returns OK |
| Text `watch`                             | Not supported (see [Known limitations](../operations/known-limitations.md)) |
| Binary `Get` / `GetQ` / `GetK` / `GetKQ` | Full |
| Binary `Set` / `Add` / `Replace` and Q variants | Full |
| Binary `Append` / `Prepend` and Q variants | Full |
| Binary `Delete` / `DeleteQ`              | Full |
| Binary `Increment` / `Decrement` (+ auto-vivify) | Full |
| Binary `Touch` / `GAT` / `GATQ` / `GATK` / `GATKQ` | Full |
| Binary `Flush` / `FlushQ`                | Full |
| Binary `Version` / `NoOp` / `Quit` / `QuitQ` | Full |
| Binary `Stat`                            | Full |
| Binary `Verbosity`                       | Accepted, no-op |
| Binary SASL                              | Rejected with `AuthError` |
| Meta `mg` / `ms` / `md` / `ma` / `me` / `mn` | Full |
| Meta flags (`b c C E F I J D N M O T R h k l q s t u v x`) | Full |

## Redis (RESP2)

Reference: [Redis 7.x][redis], RESP2 subset only.

[redis]: https://redis.io/

| Area                                     | Status |
|------------------------------------------|--------|
| `GET`                                    | Full   |
| `SET` (with `EX`, `PX`, `NX`, `XX`)      | Full   |
| `SETEX`                                  | Full   |
| `PSETEX`                                 | Full   |
| `DEL`                                    | Full   |
| `UNLINK`                                 | Full (synonym for `DEL`) |
| `EXISTS`                                 | Full   |
| `PING`                                   | Full   |
| `ECHO`                                   | Full   |
| `INFO`                                   | Full   |
| `HELLO 2`                                | Full   |
| `HELLO 3` (RESP3)                        | Rejected with `-NOPROTO` |
| `COMMAND`                                | Full   |
| `FLUSHDB` / `FLUSHALL`                   | Full   |
| `QUIT`                                   | Full   |
| `AUTH`                                   | Rejected (no auth backend) |
| `SELECT`                                 | Single keyspace; `SELECT 0` acknowledged |
| Other Redis commands                     | Not supported (see [Unsupported Redis commands](../commands/redis/unsupported.md)) |

This is a deliberate subset chosen to cover the `sccache` /
key-value-cache use case. Expanding the Redis surface is tracked as a
separate effort and not included in the current scope.

## What "full" means here

For an item marked **Full**, fastcached parses and honors the command
as the upstream spec describes — including the edge cases listed in
the per-command page. Where fastcached's storage model differs from
upstream's (no slabs, single keyspace, no SASL backend), the affected
commands are marked **Synthetic**, **Stub**, **Rejected**, or
**Not supported** rather than **Full**.
