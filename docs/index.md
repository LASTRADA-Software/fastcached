# fastcached

fastcached is a cache daemon that speaks four wire protocols off a
single storage engine: memcached ASCII text, memcached binary,
memcached meta (1.6+), and Redis RESP2. The protocols are detected
automatically from the first bytes a client sends, so any one
listening port serves clients of every supported flavor without
configuration.

The project is a re-implementation. The protocols and their semantics
are defined by the [memcached](https://memcached.org/) and
[Redis](https://redis.io/) projects, and fastcached aims to be
compatible with well-behaved clients written against those references.

## What's in the box

- The full memcached ASCII command set, including the
  TTL-refresh family (`touch`, `gat`, `gats`).
- The full memcached binary opcode set, including the `Touch`,
  `GAT`, `GATQ`, `GATK`, `GATKQ` family and arithmetic with auto-vivify.
- The memcached meta protocol (`mg`, `ms`, `md`, `ma`, `me`, `mn`)
  with the spec's flag matrix.
- A subset of Redis RESP2 covering the key-value commands used by
  `sccache` and similar clients.
- An LRU storage engine with optional persistent backing via a
  copy-on-write B-tree.
- Stats counters compatible with `memcached-tool` and similar
  monitoring scripts.

## What's not in the box (yet)

- Real SASL authentication. The opcodes are recognised so probing
  clients fail fast rather than hang.
- Redis RESP3. `HELLO 3` is rejected with `-NOPROTO`.
- The `watch` command's streaming event subscription.
- A single flat keyspace; no Redis `SELECT` databases.

See [Known limitations](operations/known-limitations.md) for the full
list.

## Where to start

- New to the project? Read [Quickstart](getting-started/quickstart.md).
- Wondering whether your client will work? Check the
  [Coverage matrix](protocols/coverage-matrix.md).
- Looking up a command? Browse the [commands index](commands/index.md).
- Comparing to upstream? See
  [Compatibility with upstream](protocols/compatibility-with-upstream.md).
