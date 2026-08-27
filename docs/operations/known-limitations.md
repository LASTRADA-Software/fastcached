# Known limitations

A single page enumerating everything fastcached does not currently
do, so a reader evaluating the project can see the gaps in one
place.

## Protocol gaps

- **`watch` (memcached text)**: streaming event subscription is not
  implemented. Recognised commands return `OK` rather than starting
  the stream.
- **SASL authentication (memcached binary)**: the PLAIN mechanism is
  implemented and authenticates against the `--requirepass` secret. When
  no secret is configured the opcodes reply `AuthError` (as before), so
  non-authing clients fall back to the plain path. Other mechanisms
  (CRAM-MD5, SCRAM) are not supported.
- **`shutdown` (memcached text)**: not implemented. Stop the daemon
  via the OS (`Ctrl-C`, `SIGTERM`, systemd, Windows service control).
- **`stats conns` (memcached text)**: returns an empty result.
  fastcached does not track an active-connection registry.
- **`stats sizes` (memcached text)**: returns a single approximate
  bucket because the storage engine does not track per-entry sizes
  bucketed by size class.
- **`AUTH` (Redis)**: supported when `--requirepass` is set (single
  shared secret, redis-style); with no secret configured it replies the
  redis-compatible "no password is set" error. Per-user ACLs beyond the
  single optional `--auth-username` are not supported.
- **`HELLO 3` (Redis)**: rejected with `-NOPROTO`. RESP3 is not
  supported.
- **`SELECT db` (Redis)**: accepted as a no-op for any index — the
  reply is always `+OK`, but fastcached is a single flat keyspace, so
  the index is ignored rather than selecting a distinct database.
- **Keyspace notifications for memcached writes**: not fired. A memcached
  `set`/`delete`/`incr` dirties a Redis `WATCH` (the storage layer does
  that for every protocol), but publishes no `__keyevent@0__:*` frame.
  Memcached has no convention for keyspace events, and the event names
  belong to the Redis verbs that own them.

## Keyspace notification timing

`notify-keyspace-events` accepts the full redis flag set, `x` (expired) and
`e` (evicted) included, and both events are published — but **when** an
`expired` event fires differs from redis, and a subscriber depending on it
should know how.

**The rule: `expired` fires when the entry is *reclaimed*, not when its TTL
lapses.** Everything below follows from that, and the two backends reclaim
at different moments.

- **There is no active expiry cycle.** redis samples keys on a timer and
  expires them in the background. Nothing here sweeps periodically, so a
  lapsed entry sits until something reaches it.
- **A plain `SET` over a lapsed key does not fire `expired`.** It overwrites
  the record without looking up the old one, so nothing is reclaimed — you
  get `set` and nothing else. The verbs that *do* reclaim are the ones that
  must find the key first: `DEL`, `EXPIRE`/`PERSIST`, `APPEND`,
  `INCR`/`DECR`, `REPLACE`, `GETSET`, and a CAS.
- **With the default `Approximate` LRU recency, a read does not reclaim
  either.** The in-memory read path is deliberately non-mutating so reads on
  one shard run concurrently, so a `GET` of a lapsed key returns a miss and
  leaves the entry in place. `--lru-recency strict` reclaims on reads too, at
  the cost of serialising them per shard.
- **The persistent backend (`--storage`) reclaims later still.** Its read and
  write paths reject a lapsed record without erasing it — a read holds only a
  shared lock, and mutating the tree there would break the single-writer
  contract — so on disk only `DEL` and the (uncalled) sweep reclaim. Expect
  `expired` to be rarer under `--storage` than in memory for the same
  workload.
- **The practical consequence:** a key that lapses and is never touched again
  is never reported. If you subscribe to `__keyevent@0__:expired` to learn
  that a key is gone, poll it; do not read silence as "still alive".
- **`evicted` is immediate in memory and absent on disk.** In-memory eviction
  happens during the write that caused the memory pressure, so the event goes
  out with it. Under `--storage` no `evicted` event is emitted at all: that
  configuration is an in-memory tier mirroring a disk tier, and neither tier's
  eviction removes the key on its own — an L1 drop leaves it on disk, and a
  disk drop leaves it in the mirror, which serves reads without consulting
  disk. An event for a key the next `GET` returns would be worse than none.
- When one call reclaims more keys at once than the notification buffer
  holds — a `maxmemory` shrink on a large cache is the realistic case —
  the surplus events are dropped and counted in
  `fastcached_keyspace_reclaim_events_dropped_total`. A non-zero value
  there is the difference between "nothing expired" and "you were not told".

## Storage model differences from memcached

- No slab allocator. fastcached uses a flat LRU. `slabs *`,
  `lru *`, and `lru_crawler *` commands are recognised as synthetic
  stubs and return `OK` without changing state.
- `stats items`, `stats slabs`, `stats sizes` therefore return
  synthetic output describing a single virtual class rather than
  real per-class breakdowns.

## Storage model differences from Redis

- No data-structure commands (lists, sets, hashes, sorted sets,
  streams, bitfields). fastcached's engine is key-value only.
- No scripting (EVAL). Pub/sub (`SUBSCRIBE`, `PSUBSCRIBE`, `PUBLISH`,
  and the `__keyspace@0__` / `__keyevent@0__` channels) and transactions
  (`MULTI` / `EXEC` / `WATCH` / `DISCARD`) are implemented.
- No replication or cluster commands.

## Operational gaps

- No live event subscription (`watch`). Observability is via the `stats`
  snapshot and, when `--metrics` is set, a Prometheus `/metrics` endpoint
  (plus a `/healthz` liveness probe) served on a dedicated HTTP port.
- TLS termination is opt-in and only present in builds compiled with
  `-DFASTCACHED_ENABLE_TLS=ON` (links OpenSSL). Enable at runtime with
  `--tls --tls-cert <pem> --tls-key <pem>`. The default build links no
  OpenSSL and `--tls` then exits with a clear error; run behind a
  TLS-terminating proxy in that case. Client-certificate (mutual TLS)
  authentication is not yet implemented.

See also [Compatibility with upstream](../protocols/compatibility-with-upstream.md)
for a per-protocol completeness scorecard.
