# Known limitations

A single page enumerating everything fastcached does not currently
do, so a reader evaluating the project can see the gaps in one
place.

## Protocol gaps

- **`watch` (memcached text)**: streaming event subscription is not
  implemented. Recognised commands return `OK` rather than starting
  the stream.
- **SASL authentication (memcached binary)**: the opcodes are
  recognised but always reply with `AuthError`. fastcached has no
  authentication backend.
- **`shutdown` (memcached text)**: not implemented. Stop the daemon
  via the OS (`Ctrl-C`, `SIGTERM`, systemd, Windows service control).
- **`stats conns` (memcached text)**: returns an empty result.
  fastcached does not track an active-connection registry.
- **`stats sizes` (memcached text)**: returns a single approximate
  bucket because the storage engine does not track per-entry sizes
  bucketed by size class.
- **`AUTH` (Redis)**: rejected. No auth backend.
- **`HELLO 3` (Redis)**: rejected with `-NOPROTO`. RESP3 is not
  supported.
- **`SELECT db` (Redis)**: only database `0` is accepted. fastcached
  is a single flat keyspace.

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
- No pub/sub, scripting (EVAL), or transactions (MULTI/EXEC).
- No replication or cluster commands.

## Operational gaps

- No live event subscription (`watch`) means observability is via
  the `stats` snapshot only.
- No built-in TLS termination. Run behind a TLS-terminating proxy
  if encryption-in-transit is required.

See also [Compatibility with upstream](../protocols/compatibility-with-upstream.md)
for a per-protocol completeness scorecard.
