# Unsupported Redis commands

Common Redis commands fastcached does not implement, with a hint
where one exists. Anything not listed here and not on the
[supported list](../../protocols/redis-resp.md#supported-commands)
returns `-ERR unknown command`.

## Data-structure commands

Lists (`LPUSH`, `RPUSH`, `LPOP`, `RPOP`, `LRANGE`, `LLEN`, …), hashes
(`HSET`, `HGET`, `HGETALL`, …), sets (`SADD`, `SMEMBERS`, `SINTER`,
…), sorted sets (`ZADD`, `ZRANGE`, `ZRANGEBYSCORE`, …), streams
(`XADD`, `XREAD`, …) and bitfields are not supported. fastcached's
storage engine is a key-value cache; data-structure commands need a
different storage model.

If a key-value workflow currently uses a list or set as a
serialized blob, store it via [SET](string/set.md) and unpack on the
client.

## Pub / sub

`SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`, `PUNSUBSCRIBE`, `PUBLISH`
are not implemented. Pub/sub requires a fan-out subsystem that is
out of scope.

## Scripting

`EVAL`, `EVALSHA`, `SCRIPT *` are not implemented.

## Transactions

`MULTI`, `EXEC`, `DISCARD`, `WATCH`, `UNWATCH` are not implemented.
For atomic conditional writes, use memcached's [cas](../memcached/storage/cas.md)
or meta `ms C(token)`.

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

`DEBUG`, `MONITOR`, `LASTSAVE`, `OBJECT *`, `MEMORY *`, `LATENCY *`,
`SLOWLOG *` are not implemented. Use fastcached's `stats` (memcached
text protocol) for similar information.

`CLIENT` and `CONFIG` are accepted as compatibility stubs rather than
fully implemented — they answer the handshake probes Redis client
libraries send on connect (`CLIENT SETNAME` / `SETINFO` / `ID` /
`GETNAME`, `CONFIG GET`) without holding any real state. See
[CLIENT](connection/client.md) and [CONFIG](server/config.md).
