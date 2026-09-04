# On-disk storage

Rules for `Cache/CowTreeStorage` and the CoW tree beneath it — specifically for
the part an operator cannot recover from by restarting: what is written to disk,
and what happens when a build meets a store it did not write.

Every rule here has already been a bug, and the expensive ones are silent. The
theme is that this layer holds the only copy of something: a cache is cheap to
lose but expensive to rebuild, and a store destroyed by the code meant to rescue
it is the worst outcome available.

## The format version

**A store's record layout is stamped, and a build reads exactly one version.**
`CowTreeStorage::CurrentFormatVersion` is what this build writes;
`FormatMarkerKey` is where it is recorded. Both live on the class rather than in
the `.cpp`, because they are the on-disk contract and anything that has to build
or inspect a store of a known vintage needs the same number rather than a second
copy of it.

**A store of another vintage is refused as `UnsupportedFormatVersion`, never as
`Corrupt`.** The two call for opposite responses — `Corrupt` means the bytes are
damaged and there is nothing to recover, this means a healthy store an operator
can still convert — and the *code* is what monitoring and every programmatic
caller sees, so the distinction cannot live only in the message text. It was
`Corrupt` once, and `Corrupt` is what tells somebody to `rm -rf` a cache that was
fine.

**`Corrupt` means the BYTES ARE DAMAGED, and nothing a client can send may reach
it** (#296). The rule above was written about a rare migration edge; the same code
was also reachable **on demand, by an unprivileged client**, which is strictly
worse. A set or a stream is an ordinary value blob distinguished only by its `flags`
word, and the memcached text verbs let a client choose that word -- so six planted
bytes read back with SMEMBERS reported disk corruption against a store whose every
record still verified.

- **A `bool` cannot carry an outcome with more than two meanings.** `SetCodec::Decode`
  and `StreamCodec::Decode` both returned one, and each of the four callers picked a
  code; three picked `Corrupt`. The codecs now return
  `std::expected<void, StorageError>` carrying `MalformedValue` and a reason, so the
  fifth caller has nothing left to pick. Fixed at the seam, never at the call site
  that noticed -- a per-caller correction leaves the next caller free to choose again.
- **`Corrupt` from a codec would be dishonest anyway.** Integrity is verified BELOW
  them -- `CowTreeStorage`'s CRC32C, and the compression codec -- both of which report
  `Corrupt` themselves before a byte reaches a value decoder. A failure there is
  always "these bytes are not a well-formed value of that type".
- **Taking the code away is half the fix; the other half is that something still
  counts.** `MalformedValue` is deliberately not a persistence failure, so it no
  longer moves `fastcached_write_errors_total` nor writes a `storage write failed`
  warning -- which on its own would trade a wrong signal for **no** signal.
  `CacheMalformedValues` is the row that keeps "clients are sending nonsense"
  visible and separate from "this disk is failing".
- **The regression test has two halves and the second is the one that gets
  forgotten.** A malformed client value must report `MalformedValue`, AND genuine
  damage must still report `Corrupt`. Widening the new code until real corruption
  stops being reported is worse than the original bug, so `CowTreeStorage_test`
  asserts the damaged-marker case is `Corrupt` *and not* `MalformedValue`.

**The refusal names the remedy.** An operator reading a refusal with no next step
reaches for deletion. The sentence says the store is intact and names the
conversion.

**A marker too short to hold its own `u32` really is damage**, and keeps
`Corrupt`. There is no version to report and nothing to convert.

## When the bytes really are damaged

Everything above is about keeping `Corrupt` narrow, so that it stays worth
believing. The other half is what an operator does when it fires, and a rule that
only prevents a wrong action is half a rule while the right one is unstated
(#484). The operator-facing answer is `docs/operations/corrupt-store.md`; what
belongs here is the part that constrains the code.

**One code, two events, and WHERE decides which.** `Open` is not a scan: it reads
the two meta slots, walks the free list and looks up two reserved keys — the
in-flight-conversion marker and the format marker — and `Replay()` is deliberately
a no-op. So damage inside that reach refuses the process to start, and damage
anywhere else is found per key while the process serves. Both `fastcached` and
`fastcache-compile-node` treat a store that will not open as **fatal**, on purpose
— the operator named a path, and coming up without it delivers less than was
configured — which makes the first an outage and the second a degradation.
Anything that makes `Open` touch more of the store converts the second into the
first for the pages it newly reads, and that is a decision rather than an
optimisation.

**Nothing repairs a damaged store, and that is the answer rather than an
absence.** An operator who is told nothing goes looking for a tool, and the
nearest thing to hand is the conversion — which is not a repair, refuses damage,
and would waste the outage. Say "there is no salvage" out loud; do not let it be
inferred.

**Both halves are pinned, and the second is the one that gets forgotten.**
`CowTreeStorage_test` asserts that damage to both meta pages refuses at `Open`
with `Corrupt` — and, separately, that damage below them leaves the store **open**
with *some* keys still readable. A test that only counted corrupt keys would pass
against a store that had lost everything. That second assertion is what the whole
operator answer rests on: partial loss is the normal shape, so deleting the store
is both the correct procedure and the end of any chance of finding the cause, and
a page that says "copy it first" has to be describing something true.

**ONE damaged meta slot is a THIRD state, not a weaker version of the first**
(#632). `Corrupt` at open means both slots failed, so a fixture that only ever
damages both cannot tell *the alternation saved us* from *the damage happened to
land somewhere harmless*. It needs a real FILE, and that is not a preference:
`WriteMeta` encodes and CRCs whatever it is handed, so damaged meta bytes have no
API route at all, and an `InMemoryPageStore` `FaultPlan` reaches them only as a
torn write happening NOW rather than as the bytes an operator's file is already
sitting on when the process starts. Three separate things are pinned, each red
under a different break:

- The store **opens on the surviving slot's commit**, in BOTH directions. Damage
  only the live slot and a build that always took the older valid slot passes;
  damage only the stale one and a build that always took the newer passes. And
  what the live slot's loss costs is a **miss**, never `Corrupt` — the store is
  consistent, just with an earlier moment, and reporting damage for a key that
  was merely never committed in that view is the "delete a healthy cache"
  failure again.
- It says **nothing** while doing it. "Survived silently" is a claim about the
  log, and every assertion about the store itself holds under a build that
  opened and then complained. Nothing here takes an `ILogger` or a metrics sink,
  so a diagnostic would have to be a direct stream write and that is what the
  capture sees; it cannot see `fprintf`, which is stated rather than left to be
  assumed.
- The next commit lands in the **damaged** slot. A build that recovered
  correctly and then wrote over its one good slot passes everything above while
  leaving the store one torn write from unopenable, and that is the only state
  where getting the alternation wrong is unrecoverable rather than wasteful.

It is pinned twice, in two suites, and not by accident: `CowTreeStorage_test`
asks the operator's question (a daemon's store opens, serves, stays quiet, keeps
its surviving slot) and `FilePageStore_test` asks the library's, because
`src/CowTree/` is a standalone sibling and a guarantee held only by the FastCache
suite does not travel with it. They cannot share a helper — `CowTreeTests` has
only `src/CowTree` on its include path, which is the boundary that keeps the
library standalone.

**The signals are not the same on both binaries.** `WriteErrorReportingStorage` is
constructed only by `fastcached`, so `fastcached_write_errors_total` and the
`storage write failed` warning cannot move on a compile node — its tier is wrapped
in `ShardedStorage` and nothing else. A counter that is exported and can never rise
is the shape the metrics rules already name, so anything written about reacting to
damage says which binary it is about.

## Converting a store

**A format is convertible exactly as long as its reader is in
`RecordFormats()`.** That table *is* the migration policy stated as data: bumping
`CurrentFormatVersion` without adding a row is the decision to discard every
existing store, and a decision made by deleting a line is one somebody makes on
purpose. Two `static_assert`s hold the table to the version — it ends at
`CurrentFormatVersion`, and every row sits at its own index from the first,
checked row by row. Arithmetic on the ends is not enough: `{3, 5, 5}` has the
right first version, last version and length, and silently cannot read v4.

**Every reader returns the same `ParsedRecord` the current encoders consume.** A
reader for an older layout fills in what that layout *meant* — v3 never
compressed, so `Identity` and `original == stored` are facts, not defaults. That
is what keeps `Migrate` from ever learning which version it is converting from,
and what makes the next format a new parser plus a row rather than new code in
the conversion.

**Which reader to use is an INFERENCE, so validate before writing anything.** A
store with no marker at all is *assumed* to be the last version that did not
stamp one. The layout before that had no kind byte, so its first byte is a flags
byte that reads as an inline-record tag whenever flags are zero — and its value
bytes then land where the length field is read from. A reader that only asks "are
there at least that many bytes left" accepts it and returns an empty value, which
an in-place conversion then writes over the real one. Two guards, both
load-bearing: an old reader requires a record to end **exactly** where its value
does, and nothing is written until a read-only pass has parsed every record.
A store this build cannot read must come back **unmodified**.

**The conversion commits in slices, and that is the design.** One transaction
would be atomic and would also inflate the store permanently: a CoW commit frees
replaced pages only *at* the commit, so a single transaction allocates a page per
record per tree level and reuses none of them — measured at 3 pages per record, a
20× file inflation — and `CommitTxn` writes `freeRoot = PageId::None()`, so the
next process cannot reclaim any of it either.

**Committing is not enough to get those pages back.** A batching page store holds
a freed page until the commit that freed it is durable, which it defers to a
fixed commit interval. Without an explicit `IPageStore::Flush()` per slice the
growth stays per-record and the slicing buys nothing — 4× the records cost 5.3×
the growth, measured.

**What slicing costs is atomicity, and the progress marker pays for it.** Each
slice records the source version and its last converted key in the *same*
transaction as the records it converted, so an interrupted run leaves a store
that says what it is: `Open` refuses it by name, and running the conversion again
resumes from the last committed slice. Nothing is converted twice — which matters
because a current-format record fed to an older reader parses, and silently
yields wrong flags and a wrong length.

**An overflow record keeps its chain.** Only the leaf descriptor is rewritten, so
converting a 50 GB cache does not need 100 GB to do it.

**Conversion is offline, and is not something `Open` does for itself.** It
rewrites every leaf record, which on a cache worth migrating is long enough that
a supervisor would time out the start it was hiding inside.

**It refuses a path that names no store**, rather than creating one and reporting
"nothing to convert" over it — which would turn a typo into a stray file while
the real store stayed refused at every start.

## The tree beneath it

**`ReadTxn::ForEach` is an administrative scan, not a lookup path.** It reads
every page, so it costs the size of the store rather than its depth. `Get` is
what a lookup uses.

**A walk is bounded by `IPageStore::PageCount()`.** A tree reaches each of its
pages once, so a walk that has read more is following a cycle — and a cycle is
precisely what a per-page CRC cannot catch, since every page around the loop is
individually valid. Without the budget it is not a wrong answer but an unbounded
one.

**A walk must not overlap a commit.** A commit frees the pages it replaced and
`Allocate` recycles them, so a scan pinned across one either fails with
`OutOfRange` or — once an index has been handed out again — reads a page that now
belongs somewhere else and reports keys that are wrong rather than merely stale.
This is a property of the page stores, not of the walk: `Get` on a snapshot
pinned across a commit fails identically. The isolation `ReadTxn`'s class comment
describes stops at the first commit that reclaims a page.

Staging into an *uncommitted* write transaction from inside a walk is fine, and
is what the conversion does.

## Open work

- [#135](https://github.com/LASTRADA-Software/fastcached/issues/135) — a store
  file takes no inter-process lock, so nothing stops a conversion running against
  a store a daemon has open. Both `--migrate-storage` and `--migrate-cache`
  document "run it stopped"; that is documentation standing in for a guard.
- [#633](https://github.com/LASTRADA-Software/fastcached/issues/633) — the
  startup diagnostics `docs/operations/corrupt-store.md` quotes verbatim. The
  half this layer composes is pinned: `CowTreeStorage_test` asserts a
  `Corrupt`-at-open carries `context=FilePageStore::Open` and renders as the
  exact line the page quotes. The two sentences that WRAP it are built in
  `src/apps/` — `failed to open shard '<path>'` and the node's `--cache-dir
  cannot open ...; refusing to start` — and are still asserted by nothing.
