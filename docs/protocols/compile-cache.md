# Compile cache (`0xFC`)

A small binary protocol for storing and retrieving compiler output, used by
[`fastcache-cc`](../tools/fastcache-cc.md). Unlike the memcached and Redis
protocols, which move opaque bytes, this one is **structured**: the server
understands that a value contains an object blob plus tagged text regions, and
rewrites the paths inside those regions.

## Detection

The first byte a client sends selects the protocol. `0xFC` selects the compile
cache, alongside the memcached and Redis flavors — see
[Autodetection](autodetect.md). There is no dedicated port and no flag to
enable it: every `fastcached` listener serves it.

## Framing

All lengths are unsigned 32-bit big-endian. A field is a length followed by that
many bytes.

### STORE

```
[0xFC][0x01][key][cohort][srcRoot][buildTree][value]
```

The `srcRoot` and `buildTree` fields describe the *producer's* layout. The
server uses them to rewrite absolute paths inside the value's text regions into
canonical tokens before storing — so what lands in the cache is layout-neutral.

Reply is a single status byte: `0x01` on success, `0x00` on failure followed by
a typed error message.

### FETCH

```
[0xFC][0x02][key]
```

On a hit the reply is `[0x01][u32 length][value bytes]`; on a miss it is the
single byte `0x00`. The value is served in its **canonical** form — the client
localizes it to its own layout.

Any other opcode ends the session. A malformed frame is not negotiated: the
connection is simply closed, since the peer is not a well-behaved client.

## The value format

A stored value is an object blob plus zero or more text regions, each tagged
with the grammar that identifies path spans inside it:

```
CompileValue {
    objectBlob:   bytes          // the .o / .obj, stored untouched
    textRegions:  [ { grammar, bytes } ]
}
```

The object blob is opaque and never rewritten. Text regions are the compiler's
captured stdout and stderr, and their grammar tells the server where the paths
are — `/showIncludes` notes for MSVC drivers, Makefile depfile syntax for GNU
ones. Only recognised path spans are rewritten; every other byte, including
diagnostics the grammar does not match, is preserved verbatim.

This asymmetry is the whole design: canonicalize on STORE, serve canonical on
FETCH, localize on the client. The server stores exactly one representation of
an entry no matter how many differently-rooted machines produce it.

## Cohort prefetch

The `cohort` field on a STORE groups keys that tend to be needed together — in
practice, one build of one project. The server records the mapping, and when a
FETCH hits a key belonging to a cohort it warms the rest of that cohort into the
in-memory tier in the background.

This is automatic and has no CLI flag. It is debounced at two levels: per
connection, and per `(engine, cohort)` pair. Both are necessary because a
launcher opens a fresh connection per translation unit — with only per-connection
debouncing, one 60-hit build was measured issuing 27022 prefetches and 13969
disk reads. A cohort holds at most 100 000 keys.

Cohort membership never affects the cache key, so changing `FASTCACHE_COHORT`
re-groups prefetching without partitioning the cache or invalidating anything.

## Operational notes

The per-value cap (`--storage-max-value`, default 256 MiB) also raises the wire
frame-payload cap for this protocol, and the default is already sized for object
files in a large codebase. See
[running a compile cache](../operations/compile-cache-server.md).
