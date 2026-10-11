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

All of it lives in `_fc_cc_core`, so it is in `fastcache-cc-tests`. `main.cpp` is in no
test target (#909).

### `InputWitness.hpp` / `InputWitness.cpp`

- **`FileIdentity`**: `{ changeTime, size, fileId, volumeId }`.
  - **Windows:** `GetFileInformationByHandleEx` with `FileBasicInfo` (`ChangeTime`) and
    `FileIdInfo`. The handle is opened with `FILE_SHARE_READ | FILE_SHARE_WRITE |
    FILE_SHARE_DELETE`, so the read never contends with the compiler's own open.
  - **POSIX:** `stat`, giving `st_ctim`, `st_size`, `st_ino` and `st_dev`.
  - **A filesystem with no change time** (FAT, exFAT report zero) falls back to the
    last-write time instead of reporting zero.
  - Change time and file id, not last-write time, are the point. `copy`, `CopyFileW` and
    robocopy keep `LastWriteTime` (measured in #1650), and a rename-over changes the id.
- **`IFileIdentitySource`** is the seam: `Identify(path) -> std::optional<FileIdentity>`.
  `MakeDiskIdentitySource()` returns the disk implementation.
- **`IInstantSource`** gives T0, in the same units as `changeTime`. It is injected, so tests
  control the boundary.
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

- **`BuildManifest` stops hashing files it has a witness for.** `ManifestInputs` carries
  the witnessed `(resolved path -> T1′ hash)` set, and every entry takes its hash from
  there.
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

- **A new `Outcome::InputsMoved`, persisted as the token `moved`.** It means "compiled; an
  input changed during the invocation, so nothing was stored".
  - It is not `Miss`, which claims *compiled and stored*.
  - It is not `Uncacheable`, which claims a permanent property of the TU.
- `outcomeDetail` names the path and the window, and the report gets a row for the token,
  documentation included.

## Data flow in `RunCached`

1. **T0**, just before `Preprocess` (and after `TryDirectMode`, which records nothing): read
   the instant, then identify and hash the TU.
2. **T1′**, right after `Preprocess`:
   - Re-identify the TU and re-hash it.
   - For every reported dependency that `ClassifyResolved` calls `Project`, record its
     identity and hash. This is the same classification `BuildManifest` applies, so the
     witness covers exactly the manifest's entries.
   - Toolchain paths are not witnessed, as today: the toolchain stamp covers them.
   - The TU's T0 and T1′ readings must agree, and every header's change time must be older
     than T0. If not, the verdict is `MovedBeforeKey`.
3. **On a preprocessed hit:** the hit is served whatever the verdict. The backfill
   `RecordManifest` runs only if the verdict is `Stable`, and it uses the witness's hashes.
4. **T2**, after the compile returns and before `PlanStore`: re-identify every witnessed
   file and compare against T1′. This covers local and dispatched compiles alike, so
   dispatch's second preprocess and the whole remote compile are inside the window. Any
   difference makes the verdict `MovedDuringCompile`.
5. **The STORE and `RecordManifest`** run as the decision table says.

### The decision table

<!-- table-total: none -->
| `InputStability` | what moved | STORE | manifest (miss) | backfill (hit) | outcome |
|---|---|---|---|---|---|
| `Stable` | nothing | yes | yes, from the T1′ hashes | yes | `Miss` / `Hit` |
| `MovedDuringCompile` | a witnessed file's identity differs between T1′ and T2 | no | yes | (no compile on a hit) | `InputsMoved` |
| `MovedBeforeKey` | the TU differs between T0 and T1′, or a header's change time is ≥ T0 | no | no | no; the hit is still served | `InputsMoved` / `Hit` + detail |
| `Unwitnessed` | a project file could not be identified or hashed at T1′ | no | no | no | `InputsMoved` + detail |

Why each row is sound:

- **`MovedDuringCompile` keeps the manifest.** Its hashes are the T1′ content the key was
  derived from, and any object soundly stored under that key is right for that content. If
  no such object exists yet, a direct lookup misses the pointer and preprocesses.
- **`MovedBeforeKey` serves the hit.** The object under the key was compiled from exactly
  the text that was preprocessed, which is what a local compile racing the edit would
  produce. Only the backfill is unsound.
- **`Unwitnessed` fails closed for the STORE:** nothing proves the pairing. It occurs only
  where `BuildManifest` already refuses `Unreadable` today.

## What the operator sees

- **Under `FASTCACHE_VERBOSE`:**
  `fastcache-cc: an input changed during this compile (<path>, <window>); compiled, not stored`.
  For `MovedDuringCompile` it adds `; manifest recorded from the keyed content`.
- **The statistics log:** `moved`, with the detail.
- **No unconditional stderr line.** The build got a correct object; the cache only declined
  to keep it.

## Known limits, stated rather than closed

- **The preprocess window is the only clock comparison,** headers only. A header on a
  network share whose server clock lags this machine's can look older than T0, so a change
  there during the preprocess goes undetected. This fails open, in a sub-second window.
  Everything after T1′ is a snapshot comparison and is exact. Refusing to cache everything
  on a remote filesystem would remove a feature, so it is documented instead.
- **A header *created* during the invocation,** at an include path that shadows the one the
  preprocess resolved, is in no witness. ccache has the same gap. It gets its own issue,
  linked from `.agent/rules/compile-cache.md`'s `## Open work`.
- **Out-of-root headers (#1597)** are a different defect: a coverage gap rather than a timing
  window. Untouched here.

## Tests

### Unit (`fastcache-cc-tests`)

1. **`InputWitness_test.cpp`**, with a fake `IFileIdentitySource` and a fake `IInstantSource`
   over real scratch files (`src/tests/ScratchPath.hpp`):
   - one case per verdict, each asserting *which* verdict and which path it names;
   - **ABA:** same content but a new identity is `MovedDuringCompile`;
   - **the boundary:** a change time equal to T0 is moved, and one tick earlier is not.
2. **The disk identity source,** against this machine's filesystem:
   - a write changes the identity, and so does a rename-over;
   - on Windows, `CopyFileW` (which keeps `LastWriteTime`) changes it;
   - on POSIX, `utimensat` back to the old mtime still changes `st_ctim`.
3. **The decision table:** every row's STORE, manifest and backfill actions and the
   recorded outcome, asserted exactly.
4. **`DirectManifest_test.cpp`:**
   - witness V1, write V2, build the manifest: it carries the V1 hash, and
     `ValidateManifest` against V2 fails;
   - the `-MF` fallback with an entry whose change time is ≥ T0 is refused
     `ChangedDuringInvocation`, naming the path.
5. **The sharing invariant:** two scratch checkouts under different roots, with identical
   content but different identities and change times, produce byte-identical encoded
   manifests and manifest keys.
6. **`CacheKey_test.cpp`:** the pinned `manifest-v8` digest.
7. **`Stats_test.cpp`:** `moved` round-trips and has its report row.

### End to end (real launcher, private cache-only node)

A stub compiler forwards to the real one and, while a trigger file exists, saves V2 after
the compiler exits. The POSIX version is a shell stub in `scripts/compile-cache-e2e.sh`.
The Windows version is built in the fixture from the issue's `wrapcl.cpp`, in
`src/apps/fastcache-cc/run-launcher-e2e.ps1`.

<!-- table-total: none -->
| leg | expected |
|---|---|
| TU saved mid-compile | run 1 `moved`, no `STORED`, no `MANIFEST`; delete the object and rebuild: `MISS`, object V2 |
| header saved mid-compile | the same |
| a second checkout holding V2 | `MISS`, V2 (issue step 5, inverted) |
| control: same stub, no trigger | `MISS`, `STORED`, `MANIFEST`, then a direct `HIT` |
| control: a second checkout, nothing moved | a direct `HIT` from the first checkout's manifest |

**The dispatch leg** goes in `scripts/dist-compile-e2e.sh` and `.ps1`: a stub-gated save
during a dispatched compile ends in `moved`.

### Proving the tests can fail

- With the T2 re-identify neutered, the mid-compile e2e legs and unit test 3 go red, and the
  controls stay green.
- With `BuildManifest` hashing at record time again, unit test 4 goes red.

Both runs, and which cases failed, are recorded in the pull request.
