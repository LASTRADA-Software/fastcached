# Unsupported Redis commands

Common Redis commands fastcached does not implement, with a hint
where one exists. Anything not listed here and not on the
[supported list](../../protocols/redis-resp.md#supported-commands)
returns `-ERR unknown command`.

The **supported** list is the authoritative one: it is derived from
`CommandTable` and checked against it by `ctest -R resp-command-docs`.
This page is prose, so read it as a hint about a family rather than as a
per-verb ruling.

## Data-structure commands

Lists (`LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LRANGE`, `LLEN`, …), hashes
(`HSET`, `HGET`, `HGETALL`, …), sorted sets (`ZADD`, `ZRANGE`,
`ZRANGEBYSCORE`, …) and bitfields are not supported.

**Sets and streams are, in part** — see the [supported
list](../../protocols/redis-resp.md#supported-commands). Sets carry
`SADD`, `SREM`, `SCARD`, `SISMEMBER`, `SMISMEMBER`, `SMEMBERS` and
`SPOP`; the set-algebra verbs are the ones still absent:

`SINTER`, `SINTERSTORE`, `SUNION`, `SUNIONSTORE`, `SDIFF`,
`SDIFFSTORE`, `SRANDMEMBER`, `SMOVE`, `SSCAN`.

Streams are complete enough for consumer groups (15 verbs, `XADD`
through `XAUTOCLAIM`).

If a key-value workflow currently uses a list or hash as a serialized
blob, store it via [SET](string/set.md) and unpack on the client.

## Key enumeration

`KEYS`, `SCAN`, `RANDOMKEY`, `DBSIZE` and `TYPE` are not implemented,
and neither is memcached's `stats cachedump`. There is no way to
enumerate the keyspace from a client on any protocol fastcached speaks.

## Pub / sub

Implemented: `SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`, `PUNSUBSCRIBE`
and `PUBLISH`, with keyspace notifications for the Redis write verbs.
`PUBSUB CHANNELS` / `NUMSUB` / `NUMPAT` and `SPUBLISH` / `SSUBSCRIBE`
(sharded pub/sub) are not.

## Scripting

`EVAL`, `EVALSHA`, `SCRIPT *` are not implemented.

## Transactions

Implemented: `MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH`. A `WATCH`
is dirtied by a write on **any** protocol, so a memcached `set` aborts a
Redis transaction watching that key.

For a single conditional write with no transaction, memcached's
[cas](../memcached/storage/cas.md) or meta `ms C(token)` is cheaper.

## Cluster / replication

Cluster commands (`CLUSTER *`), replication commands (`REPLICAOF`,
`SLAVEOF`, `SYNC`, `PSYNC`), and persistence commands (`BGSAVE`,
`BGREWRITEAOF`) are not implemented. fastcached is a single-node
cache with its own persistence story (see the persistent-storage
docs in the project source).

## Multiple databases

`SELECT` is accepted as a no-op for any index (it always replies
`+OK`) so client connection setup succeeds, but fastcached has a single
flat keyspace and the index is ignored. See [SELECT](connection/select.md).

## Server commands

`MONITOR`, `LASTSAVE`, `OBJECT *`, `MEMORY *`, `LATENCY *`,
`SLOWLOG *` are not implemented. Use fastcached's `stats` (memcached
text protocol), the `/metrics` endpoint, or `fastcache-cli stats` for
similar information.

`DEBUG` **is** implemented, for one sub-command: `DEBUG PROTOCOL <type>`
emits one reply of each RESP type so a conformance client can validate
the codec. `LOLWUT` answers with a version banner.

`CLIENT` and `CONFIG` are accepted as compatibility stubs rather than
fully implemented — they answer the handshake probes Redis client
libraries send on connect (`CLIENT SETNAME` / `SETINFO` / `ID` /
`GETNAME`, `CONFIG GET`) without holding any real state. See
[CLIENT](connection/client.md) and [CONFIG](server/config.md).
