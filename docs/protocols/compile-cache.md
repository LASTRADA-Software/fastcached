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
| AUTH | Ok, Error |

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
| `0x08` | unauthenticated | A credential is required and has not been accepted on this connection. |

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

### AUTH

```
[0xFC][ver][0x03][u32 len]  payload: [username][secret]
```

An empty `username` asks to be verified against the secret alone — the redis
`requirepass` form, and the usual one. The field is always present so the frame
arity does not depend on which credential style a client uses.

## Authentication

When the daemon runs with `--requirepass`, every verb except AUTH is refused with
`unauthenticated` until an AUTH frame has been accepted on that connection.

This handler was for a long time the only one in the tree that did not check the
configured credential: memcached text, memcached binary and RESP all did, so a
daemon started with `--requirepass` gated those three and served the compile
cache to anyone who could open a socket.

Which verbs are reachable before a credential is a **column of the opcode table**
(`OpDescriptor::preAuth`), not a condition written into the handler. A verb added
without a thought about it defaults to closed, and the gate reports "not allowed"
for an opcode it does not recognise at all.

### Why it costs no round trip

Authentication is per-connection state, and the launcher opens a fresh connection
per *operation* — so sending AUTH, awaiting its reply, and then sending the real
command would double the round trips of every translation unit in a build. That
is exactly the cost [the no-handshake decision](#why-there-is-no-handshake)
exists to avoid.

It does not have to be spelled that way. Replies are strictly ordered and
one-per-request, so a client **pipelines**: AUTH and the real command go out in a
single write, and the two replies are read in order afterwards. The credential
costs a few dozen bytes in a segment that was being sent anyway. `fastcache-cc`
does exactly this, and asserts it — a unit test checks the write *count*, because
the bytes are identical either way and only the call count distinguishes a
pipelined credential from one that waited.

A client must still read the AUTH reply. Skipping it on the assumption it
succeeded strands a whole frame in the socket, and the next command on that
connection reads the previous one's answer.

### Refusals and reloads

A failed AUTH is answered and the connection kept, as every other handler here
does: a refusal is a reply, not a close. Closing would not slow an attacker
down — reconnecting is free — while costing every honest launcher its pipelining.

The policy is resolved once per command from the live auth source, so a `SIGHUP`
that **enables** `requirepass` gates connections that are already open, and one
that disables it releases them. A connection that has actually *verified* a
credential keeps its access across a secret **rotation**, as redis does:
re-gating on rotation would fail every in-flight build at the moment an operator
rotates, which is what makes rotation something nobody dares do.

Against a daemon with no credential configured, an AUTH frame is answered `Ok`
and ignored, so setting `FASTCACHE_TOKEN` is safe in a mixed fleet. It does not
mark the connection as verified, though — nothing was checked — so a later reload
that enables auth gates it like any other.

### Against a daemon that predates AUTH

The AUTH opcode was added **without** bumping `CurrentVersion`, because the
framing was built precisely so a receiver can step over a verb it does not know:
an older daemon answers `unknown-opcode`, skips the payload, and serves the
pipelined command behind it perfectly well.

A client must therefore treat `unknown-opcode` **on AUTH** as "this daemon has no
authentication", not as a failed exchange. Returning it as the outcome instead
gives a token-configured launcher a permanent 0% hit rate against every
not-yet-upgraded daemon, reported as `rejected (unknown-opcode)` — a regression
with a plausible-looking error message and no obvious cause. `fastcache-cc` falls
through to the command's own reply and sets `credentialIgnored`, which surfaces
once per build as a verbose note: the operator asked for authentication and did
not get it, and a cache that silently does less than it was told to is worse than
one that says so. Every *other* refusal is about the credential itself and is
still reported.

### Payload ceilings

AUTH is the one verb reachable before authentication, which makes it the one hole
in the gate above: without a bound of its own it would be read with the session's
`maxPayloadBytes` (256 MiB by default), handing an unauthenticated peer exactly
the allocation the gate exists to deny. So the opcode table carries a
`maxPayload` column — `MaxAuthPayload` (4 KiB) for AUTH, and `0` ("the session
cap") for STORE and FETCH, which carry object files and are read only after the
peer has authenticated. A `static_assert` requires every `preAuth` row to declare
a non-zero ceiling, so a future pre-auth verb cannot reopen the hole by omission.
An over-ceiling frame is drained and answered like any other refusal, and names
the verb whose cap it hit rather than the session's.

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
