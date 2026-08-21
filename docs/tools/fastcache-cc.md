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
fastcache-cc <compiler> <args...>                       Front a compile (as CMAKE_<LANG>_COMPILER_LAUNCHER)
fastcache-cc --show-stats | -s [--prefetch-group <id>]  Report cache statistics for this machine
fastcache-cc --zero-stats | -z                          Discard the statistics log
fastcache-cc --help | -h | /?                           Flags and environment reference
fastcache-cc --version                                  Launcher version
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

The fastcached build does this for itself: `cmake/portable/CompileCache.cmake` picks
`fastcache-cc` up automatically whenever the binary is on `PATH` and a daemon
answers at `127.0.0.1:6674` — at any other daemon, local or remote, when
`FASTCACHE_ADDR` is exported, at `-DFASTCACHE_ADDR=host:port` ahead of even that,
nowhere if it is set empty — and injects `FASTCACHE_SOURCE_DIR` /
`FASTCACHE_BINARY_DIR` from the source and binary directories, so those two need
not be exported.

Exporting `FASTCACHE_ADDR` retargets an existing build tree on its next
configure, rather than being frozen at whatever the first configure saw, which is
what ordinary cache semantics would do to it. A `-DFASTCACHE_ADDR=` passed on the
current run still wins over the environment — including the empty value that opts
out — since it is the more deliberate of the two.

"Answers" is checked, not assumed: configure compiles one tiny translation unit
through the launcher with `FASTCACHE_VERBOSE=1` and accepts only a reported
`HIT`/`MISS`, since a launcher whose daemon is down still compiles fine and would
otherwise leave every TU paying a failed connect with nothing to show for it.
That costs about 0.1 s against a daemon that answers, runs on every configure so
that starting the daemon and reconfiguring is enough, and any other outcome —
`connect failed`, a version mismatch, no daemon at all — falls back to `sccache`
naming the address it tried; `-DUSE_COMPILER_CACHE=OFF` disables both.

The probe carries its own ten-second cap, which is what bounds a remote address
that drops packets rather than refusing them: `FASTCACHE_TIMEOUT_MS` bounds each
send and receive, not the TCP `connect()`, so a firewalled or vanished host
otherwise takes the kernel's connect timeout to fail (measured at 2m30s on
macOS) — once per configure here, but once per translation unit in a build.

### Installing it automatically

All of the above assumes `fastcache-cc` is already on `PATH`, which on a new
repository or a fresh machine is a manual step someone has to remember.
`-DFASTCACHE_AUTO_INSTALL=ON` removes it: when *no* launcher is installed —
neither `fastcache-cc` nor `sccache` nor `ccache` — the module downloads a
prebuilt `fastcache-cc` for the host's OS and architecture from the latest
stable release, checks the SHA-256 the release publishes, confirms the binary
runs here, and uses it.

It is off by default, because reaching out to the network during `cmake` is a
different thing from using what is installed. It fires only when nothing else is
available: a launcher you installed is a decision already made. And it cannot
fail a configure — an unreachable network, an unpublished platform, a download
that arrives corrupt each end in one status line and the same fall-through to
`sccache`, `ccache` or plain compilation that a missing launcher has always
produced.

The binary is staged per user, under version and platform, so every repository
and build tree on the machine shares one copy and later configures neither
download nor ask. The release lookup is cached for a day, and honours
`GITHUB_TOKEN` / `GH_TOKEN` where the unauthenticated limit of 60 requests an
hour per address would otherwise be shared out among CI runners. Pinning
`FASTCACHE_AUTO_INSTALL_VERSION` skips the lookup altogether, and pointing
`FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE` at a mirror installs without reaching
GitHub at all.

`cmake/portable/README.md` documents the full option set, and is written for projects
vendoring the module rather than building this one.

## Environment

Configuration is entirely environmental, so a launcher invocation stays a drop-in
prefix. An empty value counts as unset.

`fastcache-cc --help` documents the same set, generated from the table in
`LauncherCli.cpp` that is the launcher's single source of truth for these names.
This page is the prose version; if the two ever disagree, `--help` is right.

| Variable | Meaning | Default |
|----------|---------|---------|
| `FASTCACHE_ADDR` | `host:port` of the daemon. Hostnames, IPv4 literals, and bracketed IPv6 (`[::1]:6674`) all resolve. | unset — **no caching** |
| `FASTCACHE_SOURCE_DIR` | Checkout source root, used for keying and path canonicalization. | unset — **no caching** |
| `FASTCACHE_BINARY_DIR` | Build output root. | unset — **no caching** |
| `FASTCACHE_PREFETCH_GROUP` | Prefetch grouping id. **Not** part of the cache key, so it never partitions the cache. | `default` |
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

`ADDR`, `SOURCE_DIR` and `BINARY_DIR` must **all** be set. If any is missing
every compile runs uncached — the build still succeeds, which is exactly why
this is worth checking before concluding the cache does not help. With
`FASTCACHE_VERBOSE` set, that case reports
`missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR`.

## How it works

1. **Key.** Direct mode first: re-hash the project headers a previous compile
   recorded and look up a manifest — far cheaper than preprocessing (~18 ms
   versus ~1.4 s on a large translation unit). If that misses, preprocess the
   TU, relativize checkout-rooted arguments against `SOURCE_DIR`/`BINARY_DIR`,
   and hash `(compiler id + preprocessed text + relativized args + dependency
   paths)` into a 128-bit key. The dependency set comes from that same
   preprocess run — `-MD` into a scratch depfile for GNU drivers,
   `/showIncludes` for MSVC ones — which costs about 1.5% of it, because the
   compiler has already opened every one of those files.
2. **FETCH.** On a hit, write the object to the requested output path and replay
   the cached stdout and stderr on their own channels, with header paths
   localized to this machine so the build tool's dependency records stay valid.
3. **MISS.** Run the real compiler capturing stdout and stderr separately,
   STORE the canonicalized result, and pass the output through on the true
   streams.
4. **Any error.** Fall back to a plain real compile.

A compile that fails is never cached. The object is stored once, under the
preprocessed key; a manifest records that key rather than a second copy of the
object, so a direct hit follows one extra fetch instead of doubling the cached
volume (which, since the memory tier keeps values uncompressed, would land on
RAM where compression cannot help).

### Why the dependency paths are in the key

A hit reproduces two things: the object file, and the build system's dependency
record — a GNU depfile, or the `/showIncludes` notes Ninja reads as
`deps = msvc`. Suppressing line markers keeps every path out of the hashed text,
which is what makes a key portable across checkouts, and equally what once made
it identical after a header **moved**: same bytes, new path, so the object was
still correct and still served while the recorded paths were not. Replaying
those makes the build system rebuild that translation unit on every build,
forever, with a successful exit code each time.

Naming the dependencies in the key makes a move a different key, so the two
layouts hold two entries and moving a header back finds the original one intact.
Only machine-independent paths are hashed — those under the source root or build
tree, plus relative ones. Toolchain and system paths are left out on purpose:
they are the producing machine's spelling, the compiler identity in the key
already covers them, and hashing them would stop two machines with the same
compiler at different install prefixes from sharing anything at all.

That last exemption leaves one case open, so a hit is still checked before it is
written: every dependency path it records that this machine is answerable for
must exist. A hit that fails the check is discarded and the compile runs for
real, which re-stores the entry with a correct record.

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

all prefetch groups
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
| `missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR` | Configuration incomplete — the cache was never contacted. |
| `connect failed` | The daemon is unreachable at `FASTCACHE_ADDR`. |
| `preprocess failed` | The compiler rejected the preprocess probe; the line may use an unsupported option form. |
| `uses __TIME__/__DATE__/__TIMESTAMP__` | Deliberate: the TU is non-deterministic and would never hit. Reported as *uncacheable*, not as an error. |
| `fetch exchange failed`, `fetch decoded malformed` | Transport or protocol trouble mid-request. Also how a `FASTCACHE_TIMEOUT_MS` expiry surfaces: a daemon that accepted the connection and then went quiet. If these appear in bulk and each compile stalls for the full timeout first, suspect a wedged daemon rather than a flaky network. |
| `rejected (unsupported-version): …` | The daemon answered and declined: it does not speak this launcher's wire version. **The two binaries ship together, so this means a mixed install** — an old daemon still running against a new `fastcache-cc`, or vice versa. The message names the range the daemon does support. Restart the daemon from the same package as the launcher. |
| `rejected (payload-too-large): …` | The object exceeded the daemon's `--storage-max-value`. Raise it, or accept that this TU will not cache. |
| `rejected (…)` (other codes) | The daemon refused the command and said why; see [the error-code table](../protocols/compile-cache.md#error-codes). |
| `could not write object on hit` | The object output path was not writable. |

## Known limitations

- `__TIME__` / `__DATE__` / `__TIMESTAMP__` detection scans the source file
  text. Direct use is caught; use reached only *through a header* is not, so
  such a TU stays a permanent miss. Never incorrect — just never cached.
- Diagnostics-stream paths outside the include grammar are not yet localized.
- Toolchain and system dependency paths are in neither the key nor the
  existence check, by design: they are the producing machine's spelling, the
  compiler identity in the key already covers them, and including them would
  make two machines with different system include prefixes share nothing. So a
  cache shared between machines whose compilers print the *same* `--version`
  banner from *different* prefixes can still replay a dependency record naming a
  path the consumer lacks. Project headers — the ones that actually move — are
  covered by the key, so this is now confined to the toolchain.
- The cache key normalization is deliberately young (`objkey-v2`). Tune it
  against real developer↔CI hit rates before relying on it broadly. Bumping the
  schema re-keys the cache: existing entries miss once and are rewritten.
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
