# Input witness: an edit during a compile must not poison the cache (#1650)

## Problem

`fastcache-cc` reads the filesystem at three different moments and never checks that they
agree:

- **T1, the key.** `RunCached` preprocesses the TU (`main.cpp:2861`) and derives the object
  key from that text (`main.cpp:2959`).
- **T2, the object.** The compile runs locally or is dispatched (`main.cpp:3192-3196`). A
  dispatched compile preprocesses a second time (`main.cpp:2382`).
- **T3, the manifest.** `RecordManifest` (`main.cpp:3394`, and the hit backfill at
  `main.cpp:3121`) calls `BuildManifest`, which hashes the TU and every project header *now*
  (`DirectManifest.cpp:790-846`) and pairs those hashes with the T1 key.

A save between T1 and T3 records the new content's hashes against the old object. Every
later compile of the new content then direct-hits the stale object: `HIT`, exit 0, and it
persists through deleting the object, touching the source and clean builds. Because the
manifest key is portable, the stale object is also served to every other checkout, and
every other fleet machine, whose content equals the edited version. A save between T1 and
T2 is worse again: the new text's object is stored under the old text's preprocessed key.
That second case is in the code but nobody has reproduced it.

## Goal and constraints

- **Fix both windows.** A manifest must assert only the content its key was derived from,
  and an object may be stored under a key only if it was compiled from that key's inputs.
- **Reduce no feature.** Direct mode stays on by default. Dispatch, the hit backfill,
  cross-checkout and cross-machine manifest sharing, and root binding all keep working.
  When an input moves, the launcher gives up only what is unsound (decided: Option A).
  The build always gets its compile result.
- **A quiet invocation costs only a few milliseconds:** one extra TU hash and two identity
  reads per witnessed file. Header hashing moves from after the compile to before it; the
  total is unchanged.

## Invariants

1. **Nothing machine-local travels.** `FileIdentity`, the instant T0 and `InputWitness` have
   no encoder and never reach a key, a manifest, a `CompileValue` or the wire. Keys and
   manifests are byte-for-byte what a launcher without this change would produce for the
   same content. So cross-checkout and cross-machine hits are unchanged.
2. **No clock is compared across machines.** T0 and every change time it is compared with
   are read on the client, from the client's own filesystem.
3. **A manifest's hashes are the content the key was derived from,** never content read
   after the compile.
4. **A STORE happens only when every witnessed input's identity at T2 equals its identity
   at T1′.**

## Components

All of it lives in the launcher's own `_fc_cc_sources`, so it is in `fastcache-cc-tests`.
`main.cpp` is in no test target (#909).

### `InputFiles.hpp` / `InputFiles.cpp`, and `InputWitness.hpp` / `InputWitness.cpp`

- **`FileIdentity`**: `{ changeTime, size, fileId, volumeId }`.
  - **Windows:** `GetFileInformationByHandleEx` with `FileBasicInfo` (`ChangeTime`) and
    `FileIdInfo`. The handle is opened with `FILE_SHARE_READ | FILE_SHARE_WRITE |
    FILE_SHARE_DELETE`, so the read never contends with the compiler's own open.
  - **POSIX:** `stat`, giving `st_ctim`, `st_size`, `st_ino` and `st_dev`.
  - **A filesystem with no change time** (FAT, exFAT report zero) falls back to the
    last-write time instead of reporting zero.
  - Change time and file id, not last-write time, are the point: a rename-over changes the
    id, and an in-place write moves the change time. **`CopyFileW` is the exception, measured
    while implementing:** on NTFS it copies the SOURCE's change time onto the destination, so
    an older file copied over a newer one carries an older change time. The identity still
    differs from the destination's previous one, which is what every comparison after T1′ and
    the TU's own T0/T1′ comparison read; only the T0 clock rule for a header cannot see it
    (see Known limits).
- **`IInputFiles`** is the one seam: `Identify(path)` returns an `optional<FileIdentity>`,
  `Hash(path)` returns the content hash, `Now()` reads the clock in the same units as
  `changeTime`, and `Fence()` (Ruling 11) waits for the clock to cross one tick and returns
  T0. Every change made before the call is stamped strictly below T0, and every change after
  it returns is stamped at or above, so "saved just before the launch" is never mistaken for
  "saved during the preprocess". Measured: without the fence, 5 of 30 compiles launched right
  after a write refused to store on a 4 ms-tick kernel. Cost: up to one tick per invocation
  that preprocesses. They are one seam because the witness only ever compares
  their answers with each other. `MakeDiskInputFiles()` returns the disk implementation, and
  tests inject a fake that controls every boundary.
  - **Every comparison with T0 is `ChangedAtOrAfter(changeTime, T0)`, never a raw `>=`.** It
    floors T0 to the granularity the stamp itself shows: the largest of 2 s, 1 s, 100 ms,
    10 ms, 1 ms, 100 µs, 10 µs, 1 µs, 100 ns and 10 ns that divides the stamp exactly, or none.
    The trailing decimal zeros are the only evidence of a stamp's grid, and exFAT and vfat stamp
    at 10 ms. The cost is a fail-closed window no wider than the inferred unit.
    A filesystem that stamps whole seconds (HFS+, ext4 with 128-byte inodes) stamps an edit at
    5.9 s as 5.0 s, which a raw comparison with T0 = 5.7 s would call older than the
    invocation. A blanket floor (ccache's "include file too new") is rejected: it would stop
    caching every consumer of a header generated in the same build second, so a clean build
    with generated headers would never store those translation units. Where the platform has
    no coarse realtime clock (macOS), `Now()` subtracts a named 10 ms safety tick.
  - **The clock is read COARSELY:** `GetSystemTimeAsFileTime`, or `CLOCK_REALTIME_COARSE`
    where it exists. A filesystem stamps a change with the kernel's tick-granular time, so a
    finer clock can read an instant LATER than the stamp of a change that follows it, and the
    T0 rule would then call that change older than the invocation.
- **`InputWitness`**, a pure value:
  - T0;
  - the TU's identity and content hash at T0;
  - one entry per witnessed project dependency at T1′: canonical token, resolved path,
    identity and content hash.
- **`InputStability`**, the verdict. It is a private enum: never transmitted or persisted,
  so its enumerators have no explicit values, and it ends in a trailing `Last`.
  - `Stable`
  - `MovedDuringCompile`
  - `MovedBeforeKey`
  - `Unwitnessed`

  Each non-`Stable` verdict carries the path and the window, for the operator's note.
- **The decision table:** `EnumTable<InputStability, StabilityRow>` plus
  `RowsInEnumeratorOrder`, following `CacheDecisionTable`. `main.cpp` reads the row and
  writes no `switch`.

### Changes to `DirectManifest`

- **`BuildManifest` stops reading files.** It takes each entry's hash from an
  `IManifestContent`, a required reference in `ManifestInputs` with no default. In production
  that is the witness, which answers with the T1′ hashes; tests that are about classification
  use a test-only source that hashes when asked.
- **`ProjectDependencyPaths`** resolves and filters the reported dependencies exactly as
  `BuildManifest` does. The witness is taken over its output, so it answers every lookup
  `BuildManifest` makes, and a test pins that the two ask about the same paths.
- **The `-MF` fallback** in `RecordManifest` reads the build's own depfile after the compile,
  so it can name paths the probe never reported. Those entries are hashed at record time,
  and only if each file's change time is older than T0.
  - Otherwise the manifest is refused with the new
    **`ManifestFault::ChangedDuringInvocation`**, which names the path and gets a row in the
    fault label table.
- **`ComputeManifestKey`'s tag moves from `manifest-v7` to `manifest-v8`.**
  - `v7` shipped in `v0.4.0` (`3d2ad202`), the latest tag, and has not moved since. The bump
    is made only on that condition.
  - It flushes every manifest an older launcher recorded, the poisoned ones included, at
    the cost of one preprocess per TU fleet-wide. The objects still hit under their
    unchanged preprocessed keys.
  - It also isolates unupgraded launchers: they write under `v7`, which no upgraded launcher
    reads.
  - **`objkey-v*` does not move.** The rulebook does not require it to ("the reverse is not
    required"). Moving it would force a fleet-wide recompile to flush objects damaged in
    the unreproduced T1→T2 window.

### Changes to `Stats`

- **A new `Outcome::InputsMoved`, persisted as the token `INPUTS-MOVED`.** That is uppercase,
  like every existing token (`VERIFY-MISMATCH` is the precedent). It means "compiled; an
  input changed during the invocation, so nothing was stored".
  - It is not `Miss`, which claims *compiled and stored*.
  - It is not `Uncacheable`, which claims a permanent property of the TU.
- `outcomeDetail` is the verdict's fixed sentence, never the path, so `--show-stats` ranks one
  row per cause (Ruling 12); the path is on the `FASTCACHE_VERBOSE` line. The report gets a row for the token,
  documentation included.

## Data flow in `RunCached`

1. **T0**, just before `Preprocess` (and after `TryDirectMode`, which records nothing): read
   the instant, then identify and hash the TU. With direct mode off nothing is hashed, here or
   at T1′: no manifest will be recorded, and identity alone guards the STORE.
2. **T1′**, right after `Preprocess`:
   - Re-identify the TU and re-hash it.
   - For every reported dependency that `ClassifyResolved` calls `Project`, record its
     identity and hash. This is the same classification `BuildManifest` applies, so the
     witness covers exactly the manifest's entries.
   - Toolchain paths are not witnessed, as today: the toolchain stamp covers them.
   - The TU's T0 and T1′ readings must agree, every file's (the TU's included) change time
     must be older than T0 (compared by `ChangedAtOrAfter`, below), and each file's identity
     must not move while it is hashed. If
     not, the verdict is `MovedBeforeKey`.
3. **On a preprocessed hit:** the hit is served whatever the verdict. The backfill
   `RecordManifest` runs only if the verdict is `Stable`, and it uses the witness's hashes.
4. **T2**, after the compile returns and before `PlanStore`: re-identify every witnessed
   file and compare against T1′. This covers local and dispatched compiles alike, so
   dispatch's second preprocess and the whole remote compile are inside the window. Any
   difference makes the verdict `MovedDuringCompile`.
   - **No clock comparison is needed here.** Every witnessed file's T1′ change time is older
     than T0 floored to its stamp's granularity, and any rewrite after T1′ is stamped at or
     above that floor, so the identity always
     differs. That includes a same-size rewrite in the same clock tick.
   - **Paths only the build's own `-MF` names,** read after the compile when the probe
     reported nothing, have no T1′ snapshot. They are judged by the clock alone: a change
     time at or after T0 is `MovedDuringCompile`, and an unreadable one is `Unwitnessed`.
5. **The STORE and `RecordManifest`** run as the decision table says.

### The decision table

<!-- table-total: none -->
| `InputStability` | what moved | STORE | manifest (miss) | backfill (hit) | outcome |
|---|---|---|---|---|---|
| `Stable` | nothing | yes | yes, from the T1′ hashes | yes | `Miss` / `Hit` |
| `MovedDuringCompile` | a witnessed file's identity differs between T1′ and T2, or an `-MF`-only path changed after T0 | no | yes | (no compile on a hit) | `InputsMoved` |
| `MovedBeforeKey` | the TU differs between T0 and T1′, a file's change time is ≥ T0, or a file moved while it was hashed | no | no | no; the hit is still served | `InputsMoved` / `Hit` |
| `Unwitnessed` | a project file could not be identified or hashed | no | no | no | `Uncacheable` (Ruling 10: usually persistent, so not `INPUTS-MOVED`, whose text promises the next build caches) |

Why each row is sound:

- **`MovedDuringCompile` keeps the manifest.** Its hashes are the T1′ content the key was
  derived from, and any object soundly stored under that key is right for that content. If
  no such object exists yet, a direct lookup misses the pointer and preprocesses.
- **`MovedBeforeKey` serves the hit.** The object under the key was compiled from exactly
  the text that was preprocessed, which is what a local compile racing the edit would
  produce. Only the backfill is unsound, and the hit stays a `Hit`, with a verbose note.
- **`Unwitnessed` fails closed for the STORE:** nothing proves the pairing. It occurs only
  where `BuildManifest` already refuses `Unreadable` today.

## What the operator sees

- **Under `FASTCACHE_VERBOSE`:**
  `fastcache-cc: an input changed during the compile: <path>; compiled, not stored`. For
  `MovedDuringCompile` in direct mode the manifest is recorded FIRST and the line then says
  what happened (Ruling 15): `; manifest recorded from the keyed content`, or `; no manifest
  recorded` when the recording declined (its own note, on the line before, says why). The other
  two sentences are `an input changed while the key was taken` and `an input could not be
  observed`. On a hit whose backfill was refused, the line ends `; served the hit, recorded
  no manifest`.
- **The statistics log:** `INPUTS-MOVED` (or `UNCACHEABLE` for an unobservable input), with
  the fixed sentence in `detail`; the path is on the verbose line.
  `--show-stats` reports `inputs moved : N` and names the files.
- **No unconditional stderr line.** The build got a correct object; the cache only declined
  to keep it.

## Documentation and the rulebook

- **`.agent/rules/compile-cache.md`** gets a section, "The key, the object and the manifest
  describe ONE moment", with an `AGENT.md` tripwire. `AGENT.md`'s line budget is an equality
  ratchet, so the bullet's lines are reclaimed by re-wrapping, never by raising the budget.
- **Its `## Open work`** gets the shadowing-header issue.
- **`docs/operations/upgrading-a-fleet.md`** says, under "Coming from 0.4.0", that every
  direct-mode manifest is recorded once more, and that nothing recompiles.
- **`docs/tools/fastcache-cc.md`** documents `INPUTS-MOVED`.

## Known limits, stated rather than closed

- **The only clock comparison is against T0.** On a network share whose server clock lags
  this machine's, a header changed during the preprocess can look older than T0, so the
  change goes undetected. The TU's own T0/T1′ comparison still holds there; a header's does
  not. This fails open, in a sub-second window. After T1′ every SNAPSHOTTED input is
  compared by identity and the comparison is exact; a path only the build's `-MF` names has
  no T1′ snapshot and is judged by the clock alone until its manifest entry is hashed, so
  the lag reaches it there too. Refusing to cache everything on a remote filesystem would remove a
  feature, so it is documented instead.
- **FAT and exFAT keep no change time,** so their last-write time stands in for it. That
  weakens the T0 rule there: `CopyFile` keeps a last-write time, and FAT's is 2-second
  granular. The T0/T1′ comparison of the TU and the identity comparison after T1′ still hold.
- **A header replaced DURING THE KEY'S PREPROCESS by a copy that keeps an older change time**
  (`CopyFileW` on NTFS copies the source's change time) reads as older than T0, so the T0
  rule cannot see it, and nothing earlier snapshots that header: the set of headers is
  unknown until the preprocess has run. For a snapshotted header the window is the
  preprocess (a copy after T1′ changes the identity and is caught); for a path only the
  build's `-MF` names it runs until that path's manifest entry is hashed. #1653.
- **A header *created* during the invocation,** at an include path that shadows the one the
  preprocess resolved, is in no witness when the probe reported its set, for the whole
  invocation. ccache has the same gap. #1654.
- **Out-of-root headers (#1597)** are a different defect: a coverage gap rather than a timing
  window. Untouched here.

## Tests

### Unit (`fastcache-cc-tests`)

1. **`InputFiles_test.cpp`**, against this machine's filesystem:
   - change times are on the seam's own clock and unit;
   - a file written after an instant is stamped at or after it;
   - on Windows, `CopyFileW` over a file changes its identity while carrying the source's
     last-write time (it copies the source's change time too, measured; Ruling 13);
   - on POSIX, `utimensat` back to the old mtime still moves `st_ctim`, and a retargeted
     symlink changes the identity read through it;
   - a rename-over changes the file id;
   - a missing file has no identity, and a non-ASCII path is identified.
2. **`InputWitness_test.cpp`**, with a fake `IInputFiles`:
   - one case per verdict, each asserting *which* verdict and which path it names;
   - **ABA:** same content but a new identity is `MovedDuringCompile`;
   - **the boundary:** a change time equal to T0 is moved, and one tick earlier is not;
   - **the TU's hash alone:** a TU whose identity is unchanged and whose content moved is
     `MovedBeforeKey`, without any clock;
   - the decision table, row by row;
   - the witnessed content answers the keyed hash after a save, and refuses a late path
     changed after T0 as `ChangedDuringInvocation`.
3. **The sharing invariant:** two scratch checkouts under different roots, with identical
   content but different identities and change times, produce byte-identical encoded
   manifests and manifest keys.
4. **`DirectManifest_test.cpp`:**
   - `BuildManifest` takes every hash from its content source, never from the disk, and
     asks about exactly the TU plus `ProjectDependencyPaths`;
   - a content source's refusal names its path and fault;
   - the pinned digest moves to `manifest-v8`, and `manifest-v7` is retired.
5. **`Stats_test.cpp`:** `INPUTS-MOVED` round-trips and has its report row.

### End to end (real launcher)

**`mid-compile-edit-stub`** is a test executable built in `src/tests`, never installed. It
forwards to the real compiler and, while a trigger file exists, overwrites a source either
after a real compile or before a dispatch preprocess. Each fixture copies it under the real
compiler's own file name, because the launcher picks its driver grammar by that name. It runs
in `scripts/compile-cache-e2e.sh` and `src/apps/fastcache-cc/run-launcher-e2e.ps1`.

<!-- table-total: none -->
| leg | expected |
|---|---|
| TU saved mid-compile | run 1 `INPUTS-MOVED`, no `STORED`; delete the object and rebuild: `MISS`, object V2 |
| header saved mid-compile | the same |
| a second checkout holding V2 | object V2 (issue step 5, inverted) |
| control: same stub, no trigger | `STORED`, `MANIFEST stored`, then a direct `HIT` |
| control: a second checkout, nothing moved | a direct `HIT` from the first checkout's manifest |

**The dispatch leg** goes in `scripts/dist-compile-e2e.sh` and `.ps1`. The save lands before
the dispatch preprocess, which is #1650's T1→T2 window. The compile is `DISPATCHED` and
recorded `INPUTS-MOVED`, and a rebuild of the original content is a `MISS` that builds it.
Before the fix, that rebuild was a `HIT` serving the worker's object of the saved content.
The leg first asserts that the stub leaves the toolchain fingerprint unchanged; otherwise
nothing through it could be dispatched, and the leg would be testing a local compile.

### Proving the tests can fail

- With the T2 re-identify neutered, the during-compile unit cases and e2e legs go red, and
  the controls and the before-key cases stay green.
- With `BuildManifest` reading the disk again, its content-source cases go red.

Both runs, and which cases failed, are recorded in the pull request.
