# Design: the compile-cache executor

Date: 2026-07-23
Status: approved design (pre-implementation)
Supersedes the open questions in `docs/specs/executor.md` §5–§6 with concrete decisions.

## Context

`docs/specs/executor.md` records the full history of making a shared compile
cache (fastcached standing in as an `sccache` redis backend) correct across
machines. Two problems are separable there:

1. **Key sharing** — solved client-side (relativize shim + debug-dir flags);
   the same source hashes to the same key from any checkout.
2. **Value portability** — *unsolved*. On a hit, sccache replays the producing
   machine's captured compiler stdout verbatim, including `/showIncludes`
   header paths. Ninja records those into `.ninja_deps`. When the producer's
   build-tree→header relative depth differs from the consumer's (canonically, a
   CI runner nested one level deeper), the replayed paths do not exist on the
   consumer, Ninja marks everything dirty, and no-op builds recompile hundreds
   of objects. The client-side repair (`SCCACHE_RECACHE` storm + `.ninja_deps`
   rebuild) is a last-writer-wins treadmill: CI re-poisons the shared entry
   after every developer recache.

This design moves the correctness burden into a purpose-built,
**compile-cache-aware** component in fastcached — the *executor* — and does it
in two phases.

## Direction and phasing

fastcached becomes more than "a redis-compatible byte cache sccache happens to
talk to": a purpose-built compile-cache daemon. Three linked moves, sequenced
so the domain intelligence is designed first and the storage foundation is
derived from what that intelligence needs (not guessed in a vacuum):

- **Phase 2 — the executor (designed first).** A new custom protocol and a
  compile-cache-aware handler that canonicalizes machine-specific paths out of
  stored compile results and localizes them per consumer. Cohort-detect-and-
  prefetch lives *inside* the executor, not as a generic backend feature.
- **Phase 1 — storage hooks (built first, derived from Phase 2).** The minimal
  additions to the storage tier that the executor drives: a warm-into-L1
  `Prefetch` primitive and a cohort manifest. Nothing speculative — only what
  the executor requires.

Phase 1 ships against the existing stack; Phase 2 layers the executor on top.

## Non-goals

- No generic, backend-agnostic "prefetch" or "cohort" feature. That intelligence
  is compile-cache-specific and belongs to the executor alone.
- No coupling of fastcached to sccache's *internal* cache-entry byte format.
- No change to the existing RESP2/memcached handlers or the `IStorage`
  value/CAS semantics for ordinary keys. The executor is opt-in and additive.

---

## Decision 1 — Locus: server canonicalizes on store, client localizes on fetch

A compile-cache value has two parts: the object bytes and the captured compiler
text output. The machine-relative paths that cause poisoning live in the text
output (`/showIncludes`, diagnostics, depfiles), not in the object bytes — so
the design rewrites only the text and treats the object blob as opaque. (Any
path leakage *into* the object's own debug info is a separate, key-level concern
already handled client-side by the relativize shim and debug-dir flags in
`executor.md` §2.1/§2.3; it is out of scope here.)

The core asymmetry: **on store, only the producer knows its own layout; on
fetch, only the consumer knows theirs.** A canonical, machine-neutral form is
the middle both agree on. The transform is split to match where the knowledge
and trust already sit:

- **On STORE** (cold write path): the producer sends its layout; the **server**
  strips machine paths → canonical tokens and stores the canonical form. The
  server *owns and guarantees* the stored form is portable. No client can
  poison the shared entry — the stored value has no machine identity, so there
  is no "poisoned winner". This is the exact invariant the current
  last-writer-wins treadmill violates.
- **On FETCH** (hot read path): the server ships the canonical form **verbatim**
  (no per-consumer work, no consumer layout on the server); the **client**
  re-expands canonical tokens → its own local paths before handing output to
  the build tool.

Rejected alternatives:

- *Client-adjacent, dumb server* (all rewriting in a shim): the server cannot
  guarantee the stored form is canonical, so one misconfigured client poisons
  every consumer — the current mitigation dressed up.
- *Server does both directions* (client sends layout on every store and fetch):
  puts the server on the hot per-consumer path and forces it to trust consumer
  layout strings on every fetch — the largest injection surface.

The chosen split keeps the risky rewrite on the cold path, keeps the server off
per-consumer work, and shrinks the injection surface (server validates only
canonicalization; the client re-expands against its own trusted layout).

## Decision 2 — Architecture: a new protocol handler over the unchanged storage stack

The executor is a new `IProtocolHandler` — `CompileCacheProtocol` — a peer of
`RedisRespHandler` / `MemcachedText`, plugged in at the existing seam
`IProtocolHandler::Run(socket, engine, primingBytes, session)`. It reaches
storage through `CacheEngine` / `IStorage` exactly as the other handlers do.

Canonicalization runs in the handler, where the custom protocol's layout fields
naturally arrive. Storage stays a dumb byte store of already-canonical values;
`IStorage`'s method signatures and CAS-token semantics are unchanged.

Rejected alternatives:

- *Value-transforming `IStorage` decorator*: `IStorage` methods have nowhere to
  carry per-request layout, so it would require smuggling metadata through
  thread-local state (a smell the codebase already tolerates only reluctantly
  for `StorageSourceTag`), and a value transform in a decorator collides with
  the storage-issued CAS-token contract.
- *Standalone service beside fastcached*: discards the storage tiers and reactor
  we want to build on and adds a network hop.

This preserves the repo's established split: protocol handlers own wire
semantics and translation; storage owns bytes.

## Decision 3 — Value framing: client-tagged regions, object bytes never touched

The STORE frame separates the value into distinct fields so the server never
guesses structure and never touches binary:

```
STORE key
      object_blob                                  # binary; stored verbatim, never rewritten
      text_regions: [ { stream, grammar, bytes }, … ]   # every captured text stream
      producer_layout                              # { srcroot, buildtree_offset, cohort_id, … }
```

- `object_blob` is a separate field; no scan or transform can corrupt machine
  code.
- `text_regions` is **all** captured text outputs, not just stdout. The
  compiler's captured **stderr** also carries absolute paths in diagnostics, so
  the frame carries one region per captured stream, each tagged with the
  `grammar` that applies to it (`showIncludes`, msvc-diagnostics, `-MF`
  depfile, …). stdin is not a stored output and is not canonicalized.
- The server canonicalizes each region's `bytes` using the declared `grammar`
  to tokenize paths — format knowledge applied only to a client-declared text
  region, never to raw blob bytes.

Rejected: server parsing sccache's internal entry format (brittle across
sccache versions; welds us to one client's internals), and whole-blob path
scanning (the injection surface: cannot be certain a pathish byte range is text
and not object code).

## Decision 4 — Canonicalization authority: shared versioned spec + conformance suite

The rule mapping *absolute path ↔ canonical token* must be identical on server
(canonicalize) and client (localize); a one-edge-case disagreement silently
re-poisons. The normative authority is a **written canonicalization spec in
this repo**, versioned, with a **shared conformance test suite** as the parity
guarantee. Server and client each implement the spec; the shared test vectors
(`(path, layout) → token` and the inverse) prove they agree. Either side can be
re-implemented in any language and must pass the same vectors.

Canonical token form (per `executor.md` §5.3.1): `<SRCROOT>` / `<BUILDTREE>`
sentinels + a POSIX-normalized relative tail, re-expanded per consumer from the
consumer's own layout. Exact grammar lives in the canonicalization spec, not
here.

---

## Data flow

```
producer:  compile → split(object, texts) → STORE(+producer_layout)
                                                   │
                                            [server canonicalize]
                                                   ▼
                                          storage(canonical form)

consumer:  FETCH ──▶ storage(canonical form) ──▶ ship verbatim
                                                   │
                                          [client localize(own layout)]
                                                   ▼
                                              build tool
```

Three operations:

- **STORE** — canonicalize each text region (cold path), store
  `{ object_blob, canonical_regions }` under `key`.
- **FETCH** — return `{ object_blob, canonical_regions }` verbatim (hot path,
  allocation-light, no per-consumer transform).
- **PREFETCH** (optional, Phase-1 hook + later verb) — warm a cohort's keys into
  L1; see below.

## Error handling / failure contract

- **Unknown canon-spec version** on either side → server rejects STORE; client
  refuses to localize a FETCH and falls back to a plain cache miss (recompile).
  A wrong path is worse than a miss — never localize under a mismatched rule.
- **Malformed region** under its declared grammar → STORE rejected with a typed
  `ProtocolError`; nothing partial is stored.
- **`object_blob` is never in the transform path** → no transform bug can
  corrupt machine code; it round-trips whole or the op fails.
- **Totality over owned segments** → a path the canonicalizer cannot confidently
  tokenize is left as an opaque literal token the client passes through
  unchanged. Worst case is today's poison-style miss on a differently-laid-out
  consumer, never a corrupt path.

The risky work (canonicalize) is confined to the cold STORE path; the hot FETCH
path does no per-consumer transform — honouring `executor.md` §5.2's "never on
the hot GET path" constraint.

---

## Phase 1 — storage hooks (build first)

Derived strictly from what the executor needs. Two additions:

1. **`Prefetch(key)` on `IStorage`.** Load an entry from L2 (disk) into the L1
   (memory) mirror *without* observable client-read side effects — no LRU
   promotion counted as a genuine hit, no hit/miss stat change. Precedent
   already exists: `Peek` is the side-effect-free read primitive
   (`IStorage.hpp:191`) and `LayeredStorage::LoadFromL2AndMirror`
   (`LayeredStorage.hpp:172`) already performs the L2→L1 mirror. Default
   implementation = `Peek`-then-mirror; `LayeredStorage` overrides to do the
   real warm. Prefetch runs as a background reactor task so it overlaps serving
   other connections (same pattern as the Windows IOCP fsync drain in AGENT.md).

2. **Cohort manifest.** A cohort is defined by **explicit STORE metadata**
   (`producer_layout.cohort_id`), *not* inferred by the generic storage layer
   from access timing. The executor writes a cohort-id → key-set manifest,
   stored as an ordinary key (no new `IStorage` interface — it is just bytes).
   Cohort iteration = read the manifest, walk its keys.

That is the entire Phase-1 storage scope. No new tier, no eviction policy
change, no generic prefetch feature.

## Cohort prefetch (executor-owned, Phase 2)

- **Definition:** explicit — every stored entry is tagged with its cohort at
  write time via `producer_layout.cohort_id`; the manifest records the set.
- **Default trigger — leading-key demand (A):** when a FETCH arrives for a key
  the manifest maps to cohort *C*, the executor kicks off a background
  `Prefetch` of the rest of *C* into L1. The first real fetches of a build prime
  the remainder. Self-tuning; exact (manifest-driven, not heuristic). Cost: the
  leading edge of a cohort is still cold.
- **Optional add-on — explicit PREFETCH verb (B):** the client warms cohort *C*
  at build start. Zero cold fetches; composes with A (A is the fallback when no
  explicit PREFETCH arrived). Both ride the same manifest + `Prefetch` hook, so
  Phase 1 need not choose between them.
- **Rejected:** connection-open warm of the last cohort (coarse; wrong when a
  connection builds a different environment than last time).

---

## Testing strategy

- **Canonicalization conformance suite (centerpiece).** Shared fixtures
  `(path, layout) → token` and the inverse, run against *both* server
  canonicalize and client localize, so parity is proven, not assumed. Versioned
  alongside the canonicalization spec.
- **Executor handler end-to-end** over `InMemoryTransport` with `ManualClock`
  (the repo's standard deterministic seams): STORE then FETCH round-trips the
  object blob whole and the text regions through canonical form.
- **Poisoning regression test:** STORE from "layout CI", FETCH from "layout
  dev", assert the localized paths exist for the consumer (the exact scenario
  `executor.md` §3.1 describes).
- **`Prefetch` test** against a `LayeredStorage` over a fake L2: asserts L1 warms
  without any hit/miss stat change or client-visible LRU promotion.
- **Cohort manifest test:** STORE tags entries with a cohort id; a leading-key
  FETCH triggers a background prefetch of the rest; assert the cohort lands in
  L1.

## Open items to resolve during implementation planning

- Exact canonical-token grammar and the versioning scheme for the canon spec.
- Wire encoding of the STORE/FETCH frames (length-prefixed binary via
  `Net/Framing/ByteReader`); field layout of `producer_layout`.
- Where the client-side localize + framing logic ships (extends the existing
  launcher shim in the LASTRADA repo).
- Whether `executor.md` §6's empirical diff (which include channel actually
  produces the machine-dependent `/showIncludes` line) changes the grammar the
  canonicalizer must recognize. That diff remains the recommended first
  empirical task before Phase 2 grammar is frozen.
