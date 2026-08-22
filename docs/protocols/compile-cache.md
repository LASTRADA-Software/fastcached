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

All multi-byte integers are unsigned 32-bit big-endian. A *field* is a length
followed by that many bytes.

Every request carries a fixed 7-byte header and then exactly the payload it
declares:

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 1 | magic | Always `0xFC`. |
| 1 | 1 | version | Protocol version. Current: **1**. |
| 2 | 1 | op | `0x01` STORE, `0x02` FETCH. |
| 3 | 4 | payloadLength | Bytes of payload following the header. |

Every reply carries a fixed 5-byte header and then exactly the payload it
declares — **uniformly for every status**, including a miss, which carries a
zero-length payload rather than no payload at all:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 | status |
| 1 | 4 | payloadLength |

Those two declared lengths are what make the protocol extensible. A receiver
that does not recognise an opcode can skip exactly `payloadLength` bytes, answer
with a typed error, and stay in sync — so adding a verb is not a breaking change,
and a refusal is a *reply* rather than a dropped connection. Both sides frame
through one shared module, `src/FastCache/Protocol/CompileCacheWire.hpp`, so the
daemon, the launcher, and the test client cannot disagree about the layout.

### Statuses

| Byte | Status | Meaning |
|------|--------|---------|
| `0x00` | Miss | FETCH found nothing. Payload empty. A legitimate negative, not an error. |
| `0x01` | Ok | Command succeeded; payload is the result, if any. |
| `0x02` | Error | Command refused; payload is `[u8 errorCode][message]`. |

Which statuses an op may be answered with is table data, not convention:

| Op | Legal statuses |
|----|----------------|
| STORE | Ok, Error |
| FETCH | Ok, Miss, Error |

A miss and a refusal being distinct is the point. When both were the byte `0x00`,
a client the daemon could not serve saw an endlessly cold cache and no
diagnostic — the build merely got slower, forever, with nothing to show for it.

### Error codes

| Byte | Name | Raised when |
|------|------|-------------|
| `0x01` | unsupported-version | The request version is outside the server's range. |
| `0x02` | unknown-opcode | The opcode is not one this build knows. |
| `0x03` | malformed-frame | The fields do not exactly fill the declared payload. |
| `0x04` | payload-too-large | The declared payload exceeds the session cap. |
| `0x05` | malformed-value | A STORE payload is not a decodable compile-value. |
| `0x06` | canonicalization-failed | A text region's paths could not be canonicalized. |
| `0x07` | storage-write-failed | The cache engine refused the write. |

### STORE

```
[0xFC][ver][0x01][u32 len]  payload: [key][prefetchGroup][srcRoot][buildTree][value]
```

The `srcRoot` and `buildTree` fields describe the *producer's* layout. The
server uses them to rewrite absolute paths inside the value's text regions into
canonical tokens before storing — so what lands in the cache is layout-neutral.

### FETCH

```
[0xFC][ver][0x02][u32 len]  payload: [key]
```

On a hit the reply payload is the stored value, in its **canonical** form — the
client localizes it to its own layout.

## Versioning

The version byte travels on every request, and the server pins it to the first
command's for the life of the connection: a stream that changes version
mid-flight is nonsensical rather than merely unsupported, and saying so is
cheaper than carrying two decoders.

Rejection policy, per command:

| Condition | Reply | Connection |
|-----------|-------|------------|
| bad magic | none possible | closed |
| version unsupported, or changed mid-connection | Error / unsupported-version | closed |
| payload over the session cap | Error / payload-too-large | closed |
| unknown opcode | Error / unknown-opcode | **stays open** |
| fields ≠ declared payload | Error / malformed-frame | stays open |

A wrong magic is the only case that still closes without a reply: the peer is
not speaking this protocol, so there is no framing in which an answer would be
meaningful. Every other refusal is a typed reply, and every one is also reported
through the daemon's connection logger, so a rejection is visible to the
operator as well as to the client.

An `unsupported-version` message names the offered version *and* the supported
range (`unsupported wire version 2; this server speaks 1..1`). A rejection that
does not say what would have worked cannot be acted on, and this is the only
message an operator with a mismatched install will ever see.

### Why there is no handshake

There is no HELLO and no negotiation round trip. `fastcache-cc` opens a **fresh
connection per operation** — manifest fetch, object fetch, object store,
manifest store — so a handshake would cost two to four extra round trips *per
translation unit*, on the hot path where this project has already measured
serious regressions. Instead the client optimistically sends its current version
and learns the server's range from the rejection if it is wrong: zero cost in the
common case, one wasted round trip in the case that is already broken.

Because both binaries ship in one package, version skew is an operator error — a
mixed install — so the goal here is a loud diagnostic, not automatic interop.

### Two independent version axes

The wire version describes the *framing*; `CompileValueVersion`, the first byte
of a stored blob, describes the *value format*. They are separate because a
stored blob outlives any connection: the wire version is agreed per request,
while the blob's version is discovered when it is decoded, however long after it
was written. The launcher's cache key additionally carries an `objkey-v4` schema
tag, so a future change to the value format or the canonicalization spec re-keys
the cache — stale entries then miss and are rewritten, rather than being served
under rules they were not written by.

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

## Prefetch groups

The `prefetchGroup` field on a STORE groups keys that tend to be needed together
— in practice, one build of one project. The server records the mapping, and when
a FETCH hits a key belonging to a group it warms the rest of that group into the
in-memory tier in the background.

This is automatic and has no CLI flag. It is debounced at two levels: per
connection, and per `(engine, group)` pair. Both are necessary because a
launcher opens a fresh connection per translation unit — with only per-connection
debouncing, one 60-hit build was measured issuing 27022 prefetches and 13969
disk reads. A group holds at most 100 000 keys.

Prefetch group membership never affects the cache key, so changing `FASTCACHE_PREFETCH_GROUP`
re-groups prefetching without partitioning the cache or invalidating anything.

## Operational notes

The per-value cap (`--storage-max-value`, default 256 MiB) also raises the wire
frame-payload cap for this protocol, and the default is already sized for object
files in a large codebase. See
[running a compile cache](../operations/compile-cache-server.md).
