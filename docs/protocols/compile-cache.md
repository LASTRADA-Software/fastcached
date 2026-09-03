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
| 1 | 1 | version | Protocol version. Current: **2**. |
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
| REGISTER | Ok, Error |
| HEARTBEAT | Ok, Error |
| LEASE | Ok, Error |
| RELEASE | Ok, Error |
| COMPILE | Ok, Error |

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
| `0x05` | malformed-value | A STORE payload is not a compile value at all — it does not decode. Narrower than it looks: a value that decodes and names a generation this build does not implement is `foreign-value-generation`, not this. |
| `0x06` | *(reserved)* | Never sent. Was `canonicalization-failed`, for a failure `PathCanon` could not produce; removed in issues #59/#69. The number is burnt so it is never reused with a different meaning. |
| `0x07` | storage-write-failed | The cache engine refused the write. |
| `0x08` | unauthenticated | A credential is required and has not been accepted on this connection. |
| `0x09` | no-worker | No registered worker matches the requested toolchain. |
| `0x0a` | no-capacity | Every matching worker is at its slot limit. |
| `0x0b` | already-in-flight | Another client is already compiling this key. |
| `0x0c` | dispatch-not-permitted | This listener does not serve distributed execution. |
| `0x0d` | unknown-lease | The lease token is unknown or has expired. |
| `0x0e` | fingerprint-mismatch | The worker does not serve the toolchain the job names. |
| `0x0f` | unsupported-codec | No codec in common with what the request offered. |
| `0x10` | worker-scratch-unavailable | The worker could not prepare a scratch directory, or could not write the translation unit into it. |
| `0x11` | worker-spawn-failed | The worker could not *start* the compiler. Not "the compiler rejected the code" — that is a successful exchange carrying a non-zero exit code. |
| `0x12` | not-leader | This node does not lead the cluster. The message carries the leader's endpoint when one is known, and is empty during an election. |
| `0x13` | not-a-member | The caller is not a member of this cluster, so it may not spend the fleet's capacity. It is still served the cache. |
| `0x14` | withdrawn | Matching workers have slots free on paper and have withdrawn them: their machines are busy with something other than this fleet, or out of scratch space. Distinct from `no-capacity`, which means the fleet is full of this build's own work. |
| `0x15` | no-cluster | This node runs no cluster, so there is nothing to administer. Distinct from `not-leader`, which names somewhere else to ask. |
| `0x16` | invalid-cluster-change | The cluster cannot accept that change — a setting nobody has heard of, a member named with no address, a field a verb ignores. The message says which. |
| `0x17` | endpoint-busy | This endpoint has reached its own concurrent-request cap or in-flight byte budget. A statement about one node's front door, never about the fleet. |
| `0x18` | malformed-registration | A REGISTER named its toolchain, its endpoint or its version in bytes that are not valid UTF-8. The message says which field. Refused rather than repaired: a fingerprint is matched byte for byte, so a worker admitted under a cleaned-up name would match nothing and never be picked. |
| `0x19` | lease-unauthorized | The lease token is not one this cluster issued: its MAC does not verify under the shared key, or it is not a lease token at all. Deliberately one code for both — a receiver cannot tell a forgery from a random string. Distinct from `unknown-lease`, which names a lease the scheduler *did* issue and has since forgotten. Nothing the token claimed is echoed back. |
| `0x1a` | lease-endpoint-mismatch | An authentic lease, presented to a worker it was not issued for. Only ever reported once the MAC has verified, so it is a diagnostic rather than a hint: the message names both endpoints, because the common cause is a worker registered under an address clients do not dial, not a replay. |
| `0x1b` | lease-expired | An authentic lease, presented past its expiry and the clock-skew slack. Not a capacity statement — a worker's slots bound what it runs, the expiry bounds how long a *captured* token is worth replaying. |
| `0x1c` | worker-toolchain-survey-in-flight | The worker is still identifying its toolchains and serves nothing yet. Distinct from `fingerprint-mismatch`: that one says this worker serves a different toolchain, this one says the same request will succeed shortly. Reachable only by dialling the node directly — a node registers nothing until its survey finishes, so the scheduler never offers it to anyone who asked the fleet. |
| `0x1d` | request-deadline-exceeded | The request was admitted and outran the window this surface allows for answering it. Not `endpoint-busy`, which says the node is momentarily full and to come back: this one says the work was abandoned on time, and for a compile it is a question about the lease timeout rather than about the worker. Sent only when the server can still reach the client — a peer swept while the connection is parked on the socket gets the close alone. |
| `0x1e` | foreign-value-generation | A STORE whose value *is* a compile value, well formed, written under a canonicalization generation this build does not implement. Emphatically not `malformed-value`, which says the bytes are not a compile value at all: this one is the normal, expected answer to a peer of a *different* generation during a rolling upgrade, and reporting it as malformed tells an operator their cache is damaged when the fleet is merely mixed. Either direction — a producer behind this server answers it exactly as one ahead of it — so the message names both generations rather than a direction, and that is the whole diagnostic. |

Every one of these is a **refusal the client answers by compiling locally**,
never by failing. They are distinct codes rather than one "no" because they mean
different things to an operator: `not-a-member` is a policy decision somebody
made, `no-worker` is a fingerprint nobody in the fleet serves, `no-capacity` is a
fleet that is too small, and `already-in-flight` is none of the three.

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

## Distributed execution

Five more verbs turn the same wire into a scheduler and a worker protocol. None
of them is served by `fastcached`: the scheduler is `fastcache-compile-node
--serve-scheduler` and the worker is the same binary serving compiles, both
answering on its one `--listen-node`.

A scheduling verb arriving at a cache listener is answered `dispatch-not-permitted`
with a message naming where the scheduler went. It is a **reply**, not a dropped
connection, and it keeps its opcode rather than becoming unknown — a client built
against an older daemon has to learn *why* its scheduling stopped, and a close is
indistinguishable from a dead host while `unknown-opcode` would say this daemon is
too old when it is in fact too new.

`REGISTER` carries **one** fingerprint, so a worker serving several toolchains
registers once per toolchain. The scheduler keys a worker on
(fingerprint, endpoint), so those are separate entries — but every one of them
heartbeats the same machine-wide in-flight count, so the entries fill up together
and the pool behaves as one rather than advertising N times the machine.

`REGISTER`, `HEARTBEAT`, `LEASE` and `RELEASE` go to the scheduler, along with the
four cluster-administration verbs (`CLUSTER-STATUS` `0x08`, `CLUSTER-SET` `0x09`,
`CLUSTER-FORGET` `0x0a`, `CLUSTER-ADMIT` `0x0b`), which the **leader** answers and
only to a member. `COMPILE` goes to a worker on the **same** `--listen-node` port
that carries its cache verbs and, with `--serve-scheduler`, the scheduler's:
[#290](https://github.com/LASTRADA-Software/fastcached/issues/290) merged what were
three ports into one `0xFC` surface, so the listener is no longer the policy. Which
caller is admitted to which verb is a property of the **verb**, asked of the
component that owns it, and never of the port a frame arrived on. A verb this node
runs no component for is refused `unknown-opcode`: that is the honest code, because
this endpoint really does not implement it, and a client learns so rather than
seeing a dropped connection it cannot tell from a dead host. It is emphatically not
`dispatch-not-permitted`, which says the verb is served **somewhere else** — that is
what `fastcached` answers a scheduling verb, two paragraphs up. The distinction is
the client's: `unknown-opcode` is the one refusal a launcher steps over before
carrying on, and `dispatch-not-permitted` is one it treats as fatal, so the wrong
code here is a worker that never joins and a cache that never hits, behind a green
build. A node with no cache tier and no scheduler is the ordinary shape, not a
misconfiguration, so this is what a **healthy** build answers.
`AUTH` is the exception across a node's three verb families — the scheduler's, the
compile verbs and the cache tier's, all of them now on that one port — none of which
implements it, so each answers `unknown-opcode`, the one refusal a client steps over
before carrying on unauthenticated. `fastcached` does
implement `AUTH`: it is the only server on this wire that checks a credential, and
`--requirepass` there refuses the gated verbs `unauthenticated` rather than stepping
over anything.

```
REGISTER   [fingerprint][endpoint][slots][codecs][capacity] -> [workerId]
HEARTBEAT  [workerId][inFlight][load]                       -> Ok
LEASE      [fingerprint][objectKey][codecs]                 -> [endpoint][leaseToken][workerCodecs]
COMPILE    [leaseToken][fingerprint][args][source][codecs][sourceName]
                                              -> [exitCode][object][stdout][stderr][correlation]
RELEASE    [leaseToken][objectKey]                          -> Ok
```

`capacity` and `load` are **nested** records rather than fields of their own, and
that is a compatibility property rather than tidiness: the top-level field count
of each verb is exact and fixed forever, so a fact added there would make two
builds of one fleet unable to speak at all. The nested records are read with the
variable-arity split, so a node built before a field existed simply reports
nothing for it and a newer node registering with an older leader has it skipped.
`capacity` is what the machine *is* — cores, memory, node class, reserve, the
software version and the cache budgets; `load` is what it is *doing* — CPU busy,
available memory, free scratch and what its cache holds. `inFlight` stays outside
the nested record, because it is the one number a worker can never fail to have.

`sourceName` is the **base name** of the client's translation unit, so the worker
can name its scratch file the same way: a compiler records the name of the file it
was handed, and an object built under an invented name is gratuitously different
from a locally built one. It is sanitized before it becomes a path, and it never
decides the language — the client states that explicitly.

`correlation` is what ties a reply to the request that asked for it, and it is the
one field on this wire that exists only to catch a defect. Everything else here is
upstream of the reply — the key covers the inputs, the fingerprint covers the
toolchain, the lease covers the authorization — so before it existed a client sent a
job and accepted whatever object came back on that connection. Any defect that
crossed two jobs therefore produced a build that succeeded with the **wrong object
under a correct key**: silent, stored, and then served to every other machine that
fetched that key.

It is a digest of what the worker **actually compiled** — the preprocessed text it
wrote to scratch, the client's own argument slice as the worker decoded it, the
fingerprint and the source name — taken inside the worker's runner from the values
it was about to spawn with, never recomputed from the decoded request. A digest
taken at the wire layer would agree with whatever it was compared against, because
at that layer both of two crossed requests are still pristine. The client recomputes
the same digest from what it asked for and **refuses** a reply that does not match,
before the object envelope is opened. There is no best-effort match and no fallback
to using the object anyway: a mismatch means the translation unit is compiled
locally, and the launcher says so unconditionally on stderr rather than only under
`FASTCACHE_VERBOSE`. `fastcache-cc --show-stats` ranks it as a fall-back reason.

It is integrity against **accident**, not against a hostile worker: the digest is
unkeyed, so a worker that can return a wrong object can return a wrong digest just
as easily. It also cannot see a runner that fed the right bytes and read back the
wrong object file — there the metadata is honest and only the object is foreign, and
the worker's exclusive scratch claim is what closes that.

Two rules carry the weight and neither is configurable. A job goes only to a
worker whose fingerprint is **byte-identical**: an over-strict match costs a
local compile, an over-loose one produces a silently wrong object stored under a
key other machines fetch, and those errors are not symmetric. And the scheduler
picks the worker with the most **free slots**, ties broken by utilization —
not the one running the fewest jobs, which treats every machine as an identical
box and sends work to the smallest ones first.

The scheduler also suppresses duplicate work: when many clients miss the same key
at once — the ordinary shape of a miss after a header change — only the first is
dispatched and the rest compile locally. That check runs **before** the capacity
check, so a second client asking for a key already in flight at a busy fleet is
told `already-in-flight` rather than `no-capacity`: both are true, but only one of
them is something an operator can act on.

### A lease has three transitions, and only two of them are automatic

`LEASE` takes one, `RELEASE` resolves it, and the scheduler expires whatever is
left. The client sends `RELEASE` on **every** way its job can end — an object
built, a worker that refused it, a worker that could not be reached — because the
client is who the lease was issued to and the only party that sees all three.

Expiry is the safety net for a client that **died**, `Ctrl-C` on a build being the
ordinary case, and is not the ordinary path. It used to be: there was no `RELEASE`,
so every key stayed suppressed for the full lease lifetime — ten minutes — and
recompiling the same translation unit inside that window fell back to a local
compile. The same applies to a machine leaving the fleet: dropping a worker
releases the leases held against it, rather than leaving its keys pinned until each
one times out.

`RELEASE` names the **key** as well as the token, and that is not redundant. A
token is a small integer the scheduler minted, and its counter starts again at one
in a scheduler that has just restarted — so a client reporting a job it began
before the restart would otherwise resolve whatever lease the new instance had
since issued under the same number, freeing a key somebody is building. Naming
both makes a release resolve the client's own lease or nothing.

A `RELEASE` the scheduler cannot match is refused
`unknown-lease` rather than accepted quietly. That is the diagnostic for a job
that outlived its lease, which means the fleet's lease timeout is shorter than its
slowest translation unit — and there is nowhere else that fact could be observed.
The client does nothing about it either way: it has its object already.

### Bulk fields carry a codec envelope

A preprocessed translation unit and an object file are both large, so those
fields travel as `[u8 codec][u32 rawLen][bytes]` using the same codec ids the
cache already documents. The request carries the list of codecs the sender
accepts and the reply picks one from it, so the two ends agree with **no extra
round trip** — the same reasoning that keeps `AUTH` free. `rawLen` is what lets
a receiver reject a declared expansion before decompressing a byte, and nothing
in common falls back to `Identity` rather than refusing, so a build never loses
its cache because two peers were compiled with different codec sets.

### Control verbs have a lower ceiling

`REGISTER`, `HEARTBEAT`, `LEASE` and `RELEASE` are capped at 64 KiB rather than the
session cap. That listener is meant to be reachable by a whole fleet, and a scheduler
that can be made to allocate 256 MiB per frame by anything that authenticated
once is a scheduler that stops scheduling. `COMPILE` is the deliberate exception,
since it carries a whole translation unit.

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
range (`unsupported wire version 1; this server speaks 2..2`). A rejection that
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
was written. The launcher's cache key additionally carries an `objkey-v6` schema
tag; bumping it re-keys the cache, so stale entries miss and are rewritten rather
than being served under rules they were not written by.

Those two tags move **independently**, and it matters which one is load-bearing
here. Nothing couples a `CompileValueVersion` bump to an `objkey-v*` bump — the
lock-step this project does enforce is `manifest-v*` behind `objkey-v*`, a
different pair — so a canonicalization change need not re-key anything, and two
generations can therefore meet over one key. Re-keying is an optimisation that
makes them meet less often; the refusal below is the protection that does not
depend on anybody having remembered to bump a second tag.

`CompileValueVersion` names the **canonicalization spec**, not only the byte
layout. Canonical text travels nowhere but inside a stored value, and every server
on this wire has to rewrite one identically — including servers at different
builds, since a fleet is permanently mid-upgrade. So the byte is pinned to the
behaviour rather than maintained by hand: a conformance corpus is run through the
canonicalizer and its inverse, digested, and matched against the row for the live
generation. Change how a path span is found, rewritten or framed and that test
fails naming the bump.

A reader that meets a generation it does not implement **refuses the value**. It
cannot canonicalize it, and storing or serving it uncanonicalized would put the
producing checkout's absolute paths into a shared cache under a key every machine
computes — so a store is declined as `foreign-value-generation` (`0x1e`), naming
both generations in the message, and a fetch is a miss the launcher reports
under its own `--show-stats` reason rather than as a malformed value. That code
is the point rather than a detail: the refusal is what a *healthy* fleet does
midway through a rolling upgrade, and answering it as `malformed-value` told an
operator their cache was damaged — the same conflation the storage layer already
avoids on disk, where a store written by another build is
`UnsupportedFormatVersion` and never `Corrupt`, because the code is what
monitoring reads and `Corrupt` is what makes somebody delete a healthy cache.

Refusing costs the hits of one upgrade window; the alternative costs every
consumer that replays those paths into its dependency graph, where no edit in its
own checkout can invalidate them.

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

!!! note "The region count is a claim about bytes, and it is checked against them"

    `textRegions` is length-prefixed with a 32-bit count, and a decoder must treat
    that number as an **assertion the frame either backs up or does not** — never as
    a size to allocate from. A region costs five bytes on the wire at the very least
    (its grammar tag and its length prefix), so a frame declaring more regions than
    `remaining / 5` is refused as `malformed-value` before anything is reserved.

    Unchecked, a **nine-byte** STORE payload declaring `0xFFFFFFFF` regions asked for
    roughly 172 GB — reachable on the daemon's STORE path, and from a worker's reply
    to the launcher
    ([#267](https://github.com/LASTRADA-Software/fastcached/issues/267)). Validating
    the count is necessary and not sufficient: a validated count is still an amplifier
    whenever the in-memory element is bigger than its wire minimum, so any capacity a
    decoder reserves is sized from something *it* owns — the bytes already in hand, or
    its own configured ceiling — never from the peer's number. The same rule governs
    the prefetch-group manifest's key list and the launcher's direct-mode manifest.

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
