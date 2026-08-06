# fastcache-cc

A drop-in, sccache-style **compiler launcher** that uses fastcached as a
cross-machine-portable compile cache over the custom `0xFC` protocol. It fronts
every compile, serves cache hits (reproducing the object and replaying compiler
output), and falls back safely to a real compile whenever the cache is
unavailable — so it can never break a build.

Built when `FASTCACHE_BUILD_LAUNCHER` is on (which follows
`FASTCACHE_BUILD_TESTCLIENT` by default):

```sh
cmake --preset clangcl-debug -DFASTCACHE_BUILD_TESTCLIENT=ON
cmake --build --preset clangcl-debug --target fastcache-cc
```

## Usage

Invoked as `fastcache-cc <compiler> <args...>`, i.e. as a compiler launcher.
Wire it into a CMake build:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_CXX_COMPILER_LAUNCHER=/path/to/fastcache-cc.exe \
  -DCMAKE_C_COMPILER_LAUNCHER=/path/to/fastcache-cc.exe
```

Configure via environment:

| Variable | Meaning |
|----------|---------|
| `FASTCACHE_ADDR` | `host:port` of the fastcached daemon (required to use the cache). |
| `FASTCACHE_SRCROOT` | Checkout source root — used for keying and path canonicalization. |
| `FASTCACHE_BUILDTREE` | Build output root. |
| `FASTCACHE_COHORT` | Optional cohort id for prefetch grouping (default `default`). |
| `FASTCACHE_VERBOSE` | If set, print `HIT`/`MISS` and fall-back diagnostics to stderr. |
| `FASTCACHE_NO_STATS` | If set, do not record invocations to the statistics log. |

## Statistics

Every invocation appends one line to a per-user log
(`%LOCALAPPDATA%\fastcache-cc\invocations.log`; `$XDG_STATE_HOME/fastcache-cc/`
elsewhere) recording outcome, cohort, payload size, wall time, and the source
file. Aggregation cannot live in memory — each compile is its own short-lived
process — so the log is the accumulator and reporting folds it on demand:

```bat
fastcache-cc --stats                           REM totals + per-cohort breakdown
fastcache-cc --stats --cohort LASTRADA_master   REM one cohort only
fastcache-cc --clear-stats                      REM discard the log
fastcache-cc --help                             REM flags + environment reference
```

The report gives the hit rate, an ASCII latency distribution for hits and misses
separately, a breakdown of *why* invocations fell back, and the translation units
that never cache. Distributions rather than averages because compile latency is
routinely multi-modal — a mean of 300 ms tells you nothing about whether that is
every TU or a fast majority plus a slow tail. Fall-back reasons are itemized so
`unavailable` is actionable (`connect failed` and a refused STORE call for very
different responses).

Writes use an atomic append (`FILE_APPEND_DATA` on Windows, `O_APPEND`
elsewhere) so the hundreds of concurrent compilers in one build interleave whole
lines rather than shredding each other's. Recording failures are swallowed:
statistics never break a build.

## How it works

1. **Key** — preprocesses the TU (compiler-native, no line markers), relativizes
   checkout-rooted path args against `FASTCACHE_SRCROOT`, and hashes
   `(compiler id + preprocessed + relativized args)` into a 128-bit key. Two
   machines with the same content at different checkout roots produce the
   **same** key, so they share entries.
2. **FETCH** — on a hit, writes the object to `/Fo` and replays the cached
   stdout/stderr with `/showIncludes` header paths **localized** to this
   machine's layout, so the build tool (Ninja) records valid local deps.
3. **MISS** — runs the real compiler (capturing stdout and stderr separately),
   STOREs the canonicalized result, and passes the output through on the true
   streams.
4. **Any cache error** — falls back to a plain real compile; with
   `FASTCACHE_VERBOSE`, prints one diagnostic line. The build is never blocked
   by cache unavailability.

## Validation

`run-launcher-e2e.ps1` drives `fastcache-cc` as the compiler with real `cl` and
`clang-cl`: first compile MISSes and stores; second (object deleted) HITs and
reproduces the object byte-identically; content stored from a deep checkout HITs
from a shallow checkout with `/showIncludes` localized.

```powershell
pwsh tools/fastcache-cc/run-launcher-e2e.ps1
```

## Validation against a real codebase

Exercised against a large real C++ codebase by replaying its
`compile_commands.json` (4223 translation units) through the launcher twice
(populate, then measure). On a 400-TU sample: **99.7% hit rate** on the second
pass (393/394 cacheable; the classification of the non-cacheable few is
identical across both passes, i.e. stable and correct). The residual:

- `.rc` resource files are not C/C++ TUs — the launcher parses them as
  non-cacheable and falls back to the real tool (correct).
- A small number of TUs reach `__TIME__`/`__DATE__` through a header; these are
  inherently uncacheable (see below).

**Operational note — value cap.** Object files in a large codebase can be big
(observed max ~122 MB). fastcached's default per-value cap is 16 MiB and its
frame-payload cap 64 MiB, so run the daemon with a raised cap or those TUs will
not cache:

```sh
fastcached --bind 127.0.0.1 --port 21713 --storage-max-value 256M
```

(`--storage-max-value` also raises the frame-payload cap.)

## Status / known tuning items

- The cache **key** (preprocess + relativized-args hash) is deliberately v1 and
  isolated in `CacheKey.*` — tune the normalization against real dev↔CI hit
  rates before relying on it broadly.
- Localized path separators may be normalized to `/` in some segments; Ninja
  matches deps separator-insensitively so this is cosmetic, not a correctness
  issue.
- Diagnostics-stream paths (non-`showIncludes`) are not yet localized; only the
  include grammar is rewritten. Add `MsvcDiagnostics` region tagging if needed.
- `__TIME__`/`__DATE__`/`__TIMESTAMP__` detection scans the source TU text.
  Direct use (the common case) is caught; use reached only through a header is
  not, so such a TU stays a permanent miss (never incorrect, just uncached).
  A header-aware scan or an opt-out list could close this if it matters.
- The determinism/key normalization is v1: the double `Preprocess` cost (one
  for the key) is on the hot path; caching the compiler-id probe and gating
  re-preprocessing per key are obvious optimizations.

## Privacy

Contains no project-specific data; it compiles whatever it is pointed at. The
E2E harness prints generic status only and cleans up its temp trees. Nothing it
produces is committed.
