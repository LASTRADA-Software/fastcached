# fastcache-cc

A compiler launcher in the style of ccache and sccache, backed by a
`fastcached` daemon over the [compile-cache protocol](../protocols/compile-cache.md).

It fronts every compile: on a hit it writes the object file and replays the
compiler's captured output; on a miss it runs the real compiler and stores the
canonicalized result. On **any** cache error it falls back to a plain real
compile. Caching is an optimization — an unreachable or broken cache slows a
build down, it never breaks one.

## Why not just ccache or sccache?

Cache entries are **portable across checkout paths**. Before the key is
computed, every path under the configured source root and build tree is
rewritten to a token; on a hit, the stored output is rewritten back to the
consuming machine's layout. Two machines with the same content at different
checkout paths produce the *same* key and share entries.

That matters because a CI runner's checkout is rarely at the same path as a
developer's. With a path-sensitive cache the two populations never share hits;
here they do. It also keeps the replayed dependency information valid: header
paths in `/showIncludes` output and in `-MF` depfiles are localized on the way
out, so the build tool records dependencies that exist on *this* machine.

## Requirements

- A running `fastcached` daemon, reachable over TCP.
- A daemon value cap above your largest object file. The 256 MiB default covers
  the usual case — a large C++ codebase was measured at ~122 MB for its biggest
  object — so this normally needs no flag. Past that, `--storage-max-value`
  raises both the per-value cap and the wire frame-payload cap:

```sh
fastcached --storage-max-value=512M
```

## Supported compilers

| Driver | Recognised as | Object flag | Dependencies |
|--------|---------------|-------------|--------------|
| `gcc`, `g++`, `cc`, `c++` | GNU | `-o` | `-MD -MF <path>` depfile |
| `clang`, `clang++` | GNU | `-o` | `-MD -MF <path>` depfile |
| `cl` | MSVC | `/Fo` | `/showIncludes` on **stderr** |
| `clang-cl` | MSVC | `/Fo` | `/showIncludes` on **stdout** |

Version-suffixed (`g++-14`, `clang-18`) and `.exe` forms are recognised. Any
other `argv[0]` is treated as unknown and passed straight through uncached.

A command line is cacheable only when it compiles exactly one translation unit
to an object file. Link steps, compile-and-link steps, preprocess-only runs
(`-E`, `/EP`), and multi-source lines all fall back to the real tool.

## Usage

```
fastcache-cc <compiler> <args...>               Front a compile (as CMAKE_<LANG>_COMPILER_LAUNCHER)
fastcache-cc --show-stats | -s [--cohort <id>]  Report cache statistics for this machine
fastcache-cc --zero-stats | -z                  Discard the statistics log
fastcache-cc --help | -h | /?                   Flags and environment reference
fastcache-cc --version                          Launcher version
```

`--help` is generated from the same table the launcher dispatches on, so it
always lists exactly what the binary accepts.

**Renamed flags.** The statistics flags now use sccache's names: `--stats` is
`--show-stats` and `--clear-stats` is `--zero-stats`. `--reset` is gone with no
replacement — it read as if it reset the cache itself, when it only discarded
this machine's statistics log. All three old spellings are now rejected with an
"unknown option" diagnostic and exit 2, rather than being silently treated as a
compiler to run.

Wire it into CMake:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER_LAUNCHER=fastcache-cc \
  -DCMAKE_CXX_COMPILER_LAUNCHER=fastcache-cc
```

The fastcached build does this for itself: `cmake/CompileCache.cmake` picks
`fastcache-cc` up automatically whenever the binary is on `PATH` and a daemon
address is known — exported as `FASTCACHE_ADDR` or passed as
`-DFASTCACHE_ADDR=host:port` — and injects `FASTCACHE_SRCROOT` /
`FASTCACHE_BUILDTREE` from the source and binary directories, so those two need
not be exported. Without an address it falls back to `sccache`;
`-DUSE_COMPILER_CACHE=OFF` disables both.

## Environment

Configuration is entirely environmental, so a launcher invocation stays a drop-in
prefix. An empty value counts as unset.

`fastcache-cc --help` documents the same set, generated from the table in
`LauncherCli.cpp` that is the launcher's single source of truth for these names.
This page is the prose version; if the two ever disagree, `--help` is right.

| Variable | Meaning | Default |
|----------|---------|---------|
| `FASTCACHE_ADDR` | `host:port` of the daemon. Hostnames, IPv4 literals, and bracketed IPv6 (`[::1]:6674`) all resolve. | unset — **no caching** |
| `FASTCACHE_SRCROOT` | Checkout source root, used for keying and path canonicalization. | unset — **no caching** |
| `FASTCACHE_BUILDTREE` | Build output root. | unset — **no caching** |
| `FASTCACHE_COHORT` | Prefetch grouping id. **Not** part of the cache key, so it never partitions the cache. | `default` |
| `FASTCACHE_VERBOSE` | Print `HIT`/`MISS` and fall-back diagnostics to stderr. | unset (quiet) |
| `FASTCACHE_NO_STATS` | Do not record invocations to the statistics log. | unset (recording on) |
| `FASTCACHE_NO_DIRECT` | Disable direct mode, always preprocessing to derive the key. | unset (direct on) |
| `FASTCACHE_TIMEOUT_MS` | Per-call deadline, in milliseconds, for every send/recv to the daemon. `0` disables it. A daemon that accepts and then stalls mid-reply would otherwise block the compile forever. Bounds each call, not the whole invocation — see below. | `10000` |

The statistics log is located from the usual per-user state variables rather than
one of the launcher's own. These are read but never written:

| Variable | Meaning | Default |
|----------|---------|---------|
| `LOCALAPPDATA` | (Windows) Base for the log directory, `%LOCALAPPDATA%\fastcache-cc`. | unset — **no statistics recorded** |
| `XDG_STATE_HOME` | (POSIX) Base for the log directory, `$XDG_STATE_HOME/fastcache-cc`. Preferred over `HOME`. | unset — fall back to `HOME` |
| `HOME` | (POSIX) Base for `$HOME/.local/state/fastcache-cc`, used when `XDG_STATE_HOME` is unset. | unset — **no statistics recorded** |

With no usable state directory there is nowhere to append to, so statistics are
silently disabled. Caching itself is unaffected.

`ADDR`, `SRCROOT` and `BUILDTREE` must **all** be set. If any is missing every
compile runs uncached — the build still succeeds, which is exactly why this is
worth checking before concluding the cache does not help. With
`FASTCACHE_VERBOSE` set, that case reports
`missing FASTCACHE_ADDR/SRCROOT/BUILDTREE`.

## How it works

1. **Key.** Direct mode first: re-hash the project headers a previous compile
   recorded and look up a manifest — far cheaper than preprocessing (~18 ms
   versus ~1.4 s on a large translation unit). If that misses, preprocess the
   TU, relativize checkout-rooted arguments against `SRCROOT`/`BUILDTREE`, and
   hash `(compiler id + preprocessed text + relativized args)` into a 128-bit
   key.
2. **FETCH.** On a hit, write the object to the requested output path and replay
   the cached stdout and stderr on their own channels, with header paths
   localized to this machine so the build tool's dependency records stay valid.
3. **MISS.** Run the real compiler capturing stdout and stderr separately,
   STORE the canonicalized result, and pass the output through on the true
   streams.
4. **Any error.** Fall back to a plain real compile.

A compile that fails is never cached. Objects are stored under both the
preprocessed key and the manifest-derived key, so the direct path never depends
on the preprocessed path having run on this machine.

## Statistics

Each compile is its own short-lived process, so aggregation cannot live in
memory. Every invocation appends one line to a per-user log and reporting folds
it on demand:

- Windows: `%LOCALAPPDATA%\fastcache-cc\invocations.log`
- Elsewhere: `$XDG_STATE_HOME/fastcache-cc/invocations.log`, falling back to
  `~/.local/state/fastcache-cc/`

Writes use an atomic append (`FILE_APPEND_DATA` / `O_APPEND`) so the hundreds of
concurrent compilers in one build interleave whole lines instead of shredding
each other's. Recording failures are swallowed: statistics never break a build.

```
$ fastcache-cc --show-stats

all cohorts
  compiles     : 4
  hits         : 2  (66.7% of 3 cacheable)
    via direct : 1
  misses       : 1
  unavailable  : 1  (25.0% of all compiles -- CACHE NOT REACHED)
  fall-back reasons
    1x  connect failed

  hit latency    2 samples  p50 12ms  p95 70ms  max 70ms
    preprocess   1 samples  all 65ms
    cache i/o    2 samples  p50 0ms   p95 1ms   max 1ms
  miss latency   1 samples  all 200ms

never cached (1 translation units)
  1x  volatile.cpp
```

The hit rate is computed over **cacheable** compiles (hits + misses), not all
invocations, so an unreachable daemon does not silently look like a low hit
rate — it is reported separately as `CACHE NOT REACHED`.

Latency is shown as a distribution rather than an average because compile
latency is routinely multi-modal: a mean of 300 ms tells you nothing about
whether that is every TU or a fast majority plus a slow tail. Fall-back reasons
are itemized so `unavailable` is actionable — a refused connection and a
rejected STORE call for very different responses.

## Exit codes

| Situation | Code |
|-----------|------|
| Cache hit | `0` |
| Miss, fall-back, or non-cacheable line | the real compiler's exit code, verbatim |
| Compiler could not be spawned | `1` |
| `--help`, `--version`, `--show-stats` | `0` |
| `--zero-stats` failed | `1` |
| No arguments at all, or an unknown option | `2` (diagnostic and usage printed to stderr) |

## Fall-back reasons

Every reason that appears under `fall-back reasons`, and what to do about it:

| Reason | Meaning |
|--------|---------|
| `missing FASTCACHE_ADDR/SRCROOT/BUILDTREE` | Configuration incomplete — the cache was never contacted. |
| `connect failed` | The daemon is unreachable at `FASTCACHE_ADDR`. |
| `preprocess failed` | The compiler rejected the preprocess probe; the line may use an unsupported option form. |
| `uses __TIME__/__DATE__/__TIMESTAMP__` | Deliberate: the TU is non-deterministic and would never hit. Reported as *uncacheable*, not as an error. |
| `fetch send/recv failed`, `fetch decoded malformed` | Transport or protocol trouble mid-request. Also how a `FASTCACHE_TIMEOUT_MS` expiry surfaces: a daemon that accepted the connection and then went quiet. If these appear in bulk and each compile stalls for the full timeout first, suspect a wedged daemon rather than a flaky network. |
| `could not write object on hit` | The object output path was not writable. |

## Known limitations

- `__TIME__` / `__DATE__` / `__TIMESTAMP__` detection scans the source file
  text. Direct use is caught; use reached only *through a header* is not, so
  such a TU stays a permanent miss. Never incorrect — just never cached.
- Diagnostics-stream paths outside the include grammar are not yet localized.
- The cache key normalization is deliberately v1. Tune it against real
  developer↔CI hit rates before relying on it broadly.
- Localized path separators may be normalized to `/` in some segments. Ninja
  matches dependencies separator-insensitively, so this is cosmetic.
- Non-C/C++ inputs (for example Windows `.rc` resource files) are correctly
  classified as non-cacheable and passed through.
- `FASTCACHE_TIMEOUT_MS` bounds each individual send/recv, not a whole
  invocation. Direct mode makes a separate manifest round-trip before the object
  fetch, so a compile against a daemon that accepts and then goes silent can wait
  up to twice the timeout before falling back. It is also not a total-transfer
  deadline: a peer dribbling bytes slower than the timeout can still take longer.
  It bounds the failure mode that matters — a peer that stops entirely.

## Measured behaviour

Validated against a large real C++ codebase by replaying its
`compile_commands.json` (4223 translation units) through the launcher twice —
once to populate, once to measure. On a 400-TU sample the second pass reached a
**99.7% hit rate** (393 of 394 cacheable), with the classification of the
non-cacheable remainder identical across both passes: stable and correct.
