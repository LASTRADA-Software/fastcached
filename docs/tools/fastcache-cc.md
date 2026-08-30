# fastcache-cc

A compiler launcher in the style of ccache and sccache, backed by a
`fastcached` daemon over the [compile-cache protocol](../protocols/compile-cache.md).

It fronts every compile: on a hit it writes the object file and replays the
compiler's captured output; on a miss it has a worker compile it when
`FASTCACHE_SCHEDULER` names one, otherwise runs the real compiler, and stores the
canonicalized result. Caching is an optimization — an unreachable or broken cache
slows a build down, it never breaks one.

**A cache failure does not switch distribution off.** A cache and a compile fleet
are two services, usually on two machines, so a daemon that refused this launcher
or could not be reached says nothing about whether a worker can build the
translation unit: the compile is still dispatched, and only the caching of it is
lost. The reason is still reported and still counted under `unavailable`, and
nothing further is offered to that daemon for the rest of the invocation — there
is no point spending an object-sized transfer to be refused twice.

What a build against a broken cache *does* pay is the dispatch itself: a
translation unit that would have been a hit is now preprocessed a second time --
dispatch sends `#line` markers, which the cache key deliberately suppresses -- and
sent to a worker. That is the trade the fleet exists to make and it is still the
right one, but it is not free, and it is the reason a wrong `FASTCACHE_ADDR` is
worth fixing rather than living with.

This page is the reference: every flag, every environment variable, every
fall-back reason by name. For the story of what happens between your build system
and your compiler — direct mode, the key, the lookup, the dispatch — read
[How it works](../how-it-works.md#one-compile-start-to-finish).

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

The stream column describes a *compile* run, which is what the replay path
reproduces. It does not describe the key probe, and nothing keys off it: a
preprocess-only run moves the notes (clang-cl puts them on stderr under `/EP`,
so that they do not corrupt the preprocessed stdout), so the probe splits stdout
unconditionally and reads notes from both streams rather than guessing.

Version-suffixed (`g++-14`, `clang-18`) and `.exe` forms are recognised. Any
other `argv[0]` is treated as unknown and passed straight through uncached.

A command line is cacheable only when it compiles exactly one translation unit
to an object file. Link steps, compile-and-link steps, preprocess-only runs
(`-E`, `/EP`), and multi-source lines all fall back to the real tool.

## Usage

```
fastcache-cc <compiler> <args...>                       Front a compile (as CMAKE_<LANG>_COMPILER_LAUNCHER)
fastcache-cc --show-stats | -s [--prefetch-group <id>]  Report cache statistics for this machine
fastcache-cc --html-stats [--prefetch-group <id>]
                          [--out <path>]                Render those statistics as a self-contained HTML dashboard
fastcache-cc --zero-stats | -z                          Discard the statistics log
fastcache-cc --print-toolchain-fingerprint <compiler>   Print the fingerprint a dispatched compile would use
fastcache-cc --help | -h | /?                           Flags and environment reference
fastcache-cc --version                                  Launcher version
```

`--print-toolchain-fingerprint` is the diagnostic for a fleet whose scheduler
answers `no-worker`: run it on a client and compare with what the worker logs as
`serving`. It recomputes rather than reading the cache, so it also repairs a stale
entry on its way past.

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

The fallback is not equivalent, and the configure says so: selecting sccache
under MSVC or clang-cl emits a `CMake Warning` naming the hazard below, because a
build opted into it in silence is one whose symptom arrives hours later and
somewhere else. `-DUSE_COMPILER_CACHE=OFF` opts out of caching entirely, and
`ctest -R compile-cache-caveat` pins both the warning and the silence for the
launchers that have no hazard.

--8<-- "sccache-backend-caveat.md"

The probe carries its own ten-second cap, which is what bounds a remote address
that drops packets rather than refusing them: `FASTCACHE_TIMEOUT_MS` bounds the
exchange, not the TCP `connect()`, so a firewalled or vanished host otherwise
takes the kernel's connect timeout to fail (measured at 2m30s on macOS) — once
per configure here, but once per translation unit in a build.

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

Auto-installing the launcher alone still leaves a genuinely clean machine
uncached, since `fastcache-cc` has nothing to talk to: `-DFASTCACHE_AUTO_START=ON`
additionally stages and starts a `fastcached` daemon in the background — from
the same release archive, persistently, off by default and independently of
`FASTCACHE_AUTO_INSTALL` — when nothing answers at `FASTCACHE_ADDR`.
`cmake/portable/README.md` covers it in full, including why it defaults off in
CI as well as on a developer machine that has not opted in.

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
| `FASTCACHE_ADDR` | `host:port` of the cache — a `fastcached`, or a `fastcache-compile-node`'s `--listen-cache`. Hostnames, IPv4 literals, and bracketed IPv6 (`[::1]:6674`) all resolve. Set but **empty** is the opt-out and means no caching. | `127.0.0.1:6674` |
| `FASTCACHE_SOURCE_DIR` | Checkout source root, used for keying and path canonicalization. | unset — **no caching** |
| `FASTCACHE_BINARY_DIR` | Build output root. | unset — **no caching** |
| `FASTCACHE_PREFETCH_GROUP` | Prefetch grouping id. **Not** part of the cache key, so it never partitions the cache. | `default` |
| `FASTCACHE_VERBOSE` | Print `HIT`/`MISS` and fall-back diagnostics to stderr. | unset (quiet) |
| `FASTCACHE_NO_STATS` | Do not record invocations to the statistics log. | unset (recording on) |
| `FASTCACHE_NO_DIRECT` | Disable direct mode, always preprocessing to derive the key. | unset (direct on) |
| `FASTCACHE_CONNECT_TIMEOUT_MS` | Deadline, in milliseconds, for *opening* a connection — name resolution included. `0` leaves the platform's own, which runs to minutes. Short on purpose: a cache that has not accepted within a second is one the build is better off without, and a wedged resolver would otherwise stall every translation unit. | `1000` |
| `FASTCACHE_TIMEOUT_MS` | Deadline, in milliseconds, for one **whole** exchange with the daemon — or with a scheduler's `LEASE`/`RELEASE` — from the request to the last byte of the reply. `0` removes the bound. A daemon that accepts and then stalls, or dribbles one byte at a time, would otherwise block the compile forever. Bounds one exchange, not the whole invocation — see below — and **not** a remote compile. | `10000` |
| `FASTCACHE_DISPATCH_TIMEOUT_MS` | Deadline, in milliseconds, for one whole **`COMPILE`** exchange with a worker. `0` removes the bound. Far larger than `FASTCACHE_TIMEOUT_MS` because it bounds a different shape of conversation: a worker writes nothing until the compiler has finished, so the client waits out the entire remote compile in one read. Ten minutes because that is the scheduler's own lease timeout — waiting longer means waiting on a lease it has already reclaimed. See [Distributed compilation](../getting-started/distributed-compilation.md). | `600000` (10 min) |
| `FASTCACHE_MAX_STORE_BYTES` | Largest compiled result the launcher will offer to the daemon; `0` means no limit. A bigger result is simply left uncached. Matches the daemon's `--storage-max-value` default by construction rather than by negotiation — there is no handshake, so raise **both** or the other keeps refusing. | `268435456` (256 MiB) |
| `FASTCACHE_SCHEDULER` | `host:port` of a fleet scheduler — some `fastcache-compile-node`'s `--listen-scheduler` port, never a cache port. On a miss the launcher asks it for a worker and sends that worker the preprocessed translation unit. Every refusal falls back to a local compile, with one exception: `not-leader` is an instruction rather than an answer about the fleet, so the launcher retries against the endpoint the refusal names (up to two hops, then it compiles locally). This value therefore only has to be **a** member of the cluster, not the current leader — no launcher needs re-pointing after an election. Note that this is the *client* half: a `fastcache-compile-node` still registers only with its own `--scheduler`, which has to be the leader for the fleet to have any workers at all. A cache that is unreachable or refuses counts as a miss for this purpose — it does not disable dispatch. See [Distributed compilation](../getting-started/distributed-compilation.md). | unset — **every miss compiles locally** |
| `FASTCACHE_TOKEN` | Shared secret presented to a **daemon** started with `--requirepass`. Costs no round trip — it is pipelined ahead of the real command, not awaited. Safe against a daemon that requires none: such a daemon accepts it and ignores it. **Not safe with `FASTCACHE_SCHEDULER`** — a compile node serves no `AUTH` verb, so the credential is refused and dispatch stops working entirely ([#198](https://github.com/LASTRADA-Software/fastcached/issues/198)). | unset — **no credential sent** |
| `FASTCACHE_USER` | Username to accompany `FASTCACHE_TOKEN`. Unset (the usual case) authenticates against the secret alone, which is what `--requirepass` configures. Ignored without a token — a username on its own is a misconfiguration, not a request to authenticate, and sending an empty secret would be refused by every server that wants one. | unset |

The statistics log is located from the usual per-user state variables rather than
one of the launcher's own. These are read but never written:

| Variable | Meaning | Default |
|----------|---------|---------|
| `LOCALAPPDATA` | (Windows) Base for the log directory, `%LOCALAPPDATA%\fastcache-cc`. | unset — **no statistics recorded** |
| `XDG_STATE_HOME` | (POSIX) Base for the log directory, `$XDG_STATE_HOME/fastcache-cc`. Preferred over `HOME`. | unset — fall back to `HOME` |
| `HOME` | (POSIX) Base for `$HOME/.local/state/fastcache-cc`, used when `XDG_STATE_HOME` is unset. | unset — **no statistics recorded** |

With no usable state directory there is nowhere to append to, so statistics are
silently disabled. Caching itself is unaffected.

`ADDR`, `SOURCE_DIR` and `BINARY_DIR` must **all** be non-empty to cache. `ADDR`
has a default and the other two do not, so in practice it is the roots that are
missing when nothing caches — the build still succeeds, which is exactly why this
is worth checking before concluding the cache does not help. With
`FASTCACHE_VERBOSE` set, that case reports
`missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR`.

`FASTCACHE_ADDR` defaults to `127.0.0.1:6674` rather than to nothing, so the
launcher caches with no configuration at all against whichever of `fastcached` or
`fastcache-compile-node` is running on this machine — the node's `--listen-cache`
defaults to the same address for exactly that reason. A *remote* default would be
indefensible, since every translation unit on a machine with nothing listening
would pay a connect timeout in silence; a closed loopback port refuses
immediately, so a machine running neither pays microseconds per compile.

## How it works

1. **Key.** Direct mode first: re-hash the project headers a previous compile
   recorded and look up a manifest — far cheaper than preprocessing (~18 ms
   versus ~1.4 s on a large translation unit). If that misses, preprocess the
   TU, relativize checkout-rooted arguments against `SOURCE_DIR`/`BINARY_DIR`,
   and hash `(compiler id + preprocessed text + relativized args + dependency
   paths)` into a 128-bit key — MurmurHash3 x64_128, which is 128 bits of
   *strength* and not merely 128 bits wide; see the note in `.agent/rules/compile-cache.md` on why
   the four-CRC construction it replaced was not. **Compiler id is the driver's
   banner *and* the target it generates for**, where the driver can be asked: a
   banner alone identifies the driver, and a driver's code generation is not a
   function of the driver alone — `clang-cl` takes `-fms-compatibility-version`
   from the MSVC install beside it, and one stock `g++` banner covers x86_64 and
   aarch64. A driver that states no target keeps its entries unchanged. Note this
   is the *key*, not the toolchain fingerprint a dispatched compile matches
   workers on, which deliberately omits the target — see
   [Distributed compilation](../getting-started/distributed-compilation.md#the-fingerprint-and-the-cache-key-are-not-the-same-string).
   The dependency set comes from
   that same preprocess run — `-MD` into a scratch depfile for GNU drivers,
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

### Reading the `dependency set:` line

Every preprocessed compile reports, under `FASTCACHE_VERBOSE`, what became of the
paths the driver named:

```
fastcache-cc: dependency set: 61 of 635 reported path(s) keyed (476 toolchain; 60 filesystem call(s))
```

`M` is what the probe reported, `N` is the key's own dependency set, and the words
before the semicolon say why the rest did not reach it.

**They will usually not add up, and that is not a fault.** `M` and the drop counts
are per reported *occurrence*; `N` is the set after deduplication. `/showIncludes`
names a header once per inclusion site, so the 159 occurrences left after the 476
drops above are 61 distinct files. A GNU depfile names each header once, so on
those drivers the numbers do sum. What always holds is that the reasons account
for every path that did not reach the key:

| Reason | What it means |
|--------|---------------|
| `toolchain` | Under neither root, or inside a vendored tree. The ordinary bulk of any compile: the compiler identity in the key already covers this content collectively. That identity is the compiler's own **version banner** — `Microsoft (R) C/C++ Optimizing Compiler Version 19.51.36252 for x64`, `clang version 22.1.3 (…)` — so two MSVC toolsets, or the x86 and x64 driver of one, key apart even though their headers are the same files. |
| `drive-relative` | A Windows `C:foo` path under neither root. It resolves against that drive's own current directory — per-process state on the producing machine that no cache entry can record. One under a drive-relative *root* is keyed like any other, or counted `toolchain` if it is vendored content. |
| `unanchored` | Relative, with no working directory to resolve it against. |
| `no canonical form` | Under a root by character prefix but not by path segment: a root spelled almost right, such as `/x/build-other` against `/x/build`. |
| `empty` | The driver named an empty path. |

`0 of M` with M non-zero is the line to act on, and the reason says which repair.
Every path counted `toolchain` means the configured roots match nothing the
compiler echoed back — on Windows usually an 8.3 short name in one of them, which
is otherwise entirely silent: the key is empty, the replay guard has nothing to
check, and the stored value keeps this machine's absolute paths. `drive-relative`
or `unanchored` point at the build's own spelling of its include paths instead.
`0 of 0` is a different fault: the driver reported nothing at all on the
preprocess line.

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
    1x  fetch exchange failed

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

Every one of them still produces a correct object. Which of them leave
distribution running is a narrower claim than that, and worth reading precisely:
the two that describe a daemon **failing to serve the lookup** — an unreachable
one (`fetch exchange failed`) and one that answered and refused (`rejected (…)`)
— carry on and dispatch, which is the note at the top of this page. The rest end
the invocation at a local compile, because each of them is a reason there is
nothing to dispatch *with*: the key is not computed yet (the configuration and
path reasons), the preprocess itself failed, the translation unit is one the
launcher deliberately steps over, or the object could not be written on this
machine. The ones marked *uncacheable* are the launcher's own refusals and are
not about the daemon at all.

| Reason | Meaning |
|--------|---------|
| `missing FASTCACHE_ADDR/SOURCE_DIR/BINARY_DIR` | Configuration incomplete — the cache was never contacted, and neither was a scheduler. Distribution is off here deliberately, and not merely for want of a key: `FASTCACHE_ADDR=` (set but empty) is how a build opts out of the launcher altogether, and without the two roots there is no portable key for a scheduler to suppress duplicates on. Set all three to use either. |
| `preprocess failed` | The compiler rejected the preprocess probe; the line may use an unsupported option form. |
| `uses __TIME__/__DATE__/__TIMESTAMP__` | Deliberate: the TU is non-deterministic and would never hit. Reported as *uncacheable*, not as an error. |
| `a command-line path is drive-relative under no root`, `a reported dependency path is drive-relative under no root` | Deliberate, and Windows-only. A path like `C:foo\bar.hpp` resolves against drive `C:`'s **own** current directory, which no cache entry records — so the launcher can neither key it (a header moved inside it would not re-key) nor check it on replay (there is no directory to `stat` it against). Caching such a compile could serve a stale dependency record under a zero exit code, so it is not cached at all. Reported as *uncacheable*, not as an error. Spell the path absolutely (`C:\foo\bar.hpp`), make it relative, or bring it under `FASTCACHE_SOURCE_DIR`/`FASTCACHE_BINARY_DIR`. The first is the rule applied to the command line, the second to what the compiler reported; `FASTCACHE_VERBOSE` names the offending path itself. |
| `daemon does not support authentication; the configured credential was ignored` | `FASTCACHE_TOKEN` is set but the daemon predates the AUTH verb. Caching works normally — the daemon steps over the verb it does not know and serves the command — but this traffic is **not** authenticated. Said once per invocation rather than per exchange. Upgrade the daemon, or unset the token if it was not meant to apply here. |
| `rejected (unauthenticated): ...` | The daemon requires a credential. `authentication required` means none was sent — set `FASTCACHE_TOKEN`. `authentication failed` means one was sent and was wrong. The two are deliberately different messages because they are different mistakes. Either way the build succeeds and only the caching is lost — the compile is still dispatched if `FASTCACHE_SCHEDULER` names a scheduler, and runs locally otherwise. |
| `fetch exchange failed`, `fetch decoded malformed` | Transport or protocol trouble mid-request. Also how a `FASTCACHE_TIMEOUT_MS` expiry surfaces: a daemon that accepted the connection and then went quiet. If these appear in bulk and each compile stalls for the full timeout first, suspect a wedged daemon rather than a flaky network. `fetch exchange failed` is also what a plainly wrong `FASTCACHE_ADDR` looks like — every compile, at once, with the fleet still doing the work. The two differ in what happens next: `fetch exchange failed` carries on and dispatches, while `fetch decoded malformed` — a daemon that answered with a value this launcher cannot read, which in practice means a mixed install — ends the invocation at a local compile. |
| `rejected (unsupported-version): …` | The daemon answered and declined: it does not speak this launcher's wire version. **The two binaries ship together, so this means a mixed install** — an old daemon still running against a new `fastcache-cc`, or vice versa. The message names the range the daemon does support. Restart the daemon from the same package as the launcher. |
| `rejected (payload-too-large): …` | The object exceeded the daemon's `--storage-max-value`. Raise it, or accept that this TU will not cache. |
| `rejected (…)` (other codes) | The daemon refused the command and said why; see [the error-code table](../protocols/compile-cache.md#error-codes). |
| `could not write object on hit` | The object output path was not writable. |
| `a reported dependency path is not text this host can read`, `a captured region names a path that is not text this host can read` | Deliberate, and Windows-only. `cl.exe` writes the paths in `/showIncludes` in the **console output** code page, while this launcher's own roots arrive as UTF-8 -- so a header under a non-ASCII directory can reach it as bytes it cannot read as text. Such a path prefix-matches no root, which would key a project header as toolchain content and serve a stale object under a zero exit code, so the compile is not cached at all. Reported as *uncacheable*, not as an error. The fix is the console: `chcp 65001` makes `cl` emit UTF-8 and this stops appearing. |

## Known limitations

- `__TIME__` / `__DATE__` / `__TIMESTAMP__` detection scans the source file
  text. Direct use is caught; use reached only *through a header* is not, so
  such a TU stays a permanent miss. Never incorrect — just never cached.
- A Windows **drive-relative** path (`C:foo`) reaching a compile takes that
  translation unit out of the cache entirely, rather than being ignored. Only
  `clang-cl` echoes such a path back unresolved (`cl` resolves it through the
  filesystem), and no common generator emits one, so in practice this costs
  nothing — but where it does fire it costs the whole TU rather than one path,
  deliberately: a partial answer here is the stale-dependency serve it exists to
  prevent. The refusal is silent unless `FASTCACHE_VERBOSE` is set, and shows in
  `--show-stats` as *uncacheable*.
- Diagnostics-stream paths outside the include grammar are not yet localized.
- Toolchain and system dependency paths are in neither the key nor the
  existence check, by design: they are the producing machine's spelling, the
  compiler identity in the key already covers them, and including them would
  make two machines with different system include prefixes share nothing. So a
  cache shared between machines whose compilers print the *same* version banner
  *and target the same triple*, from *different* prefixes, can still replay a
  dependency record naming a path the consumer lacks. Project headers — the ones
  that actually move — are covered by the key, so this is now confined to the
  toolchain.
- The cache key normalization is deliberately young (`objkey-v6`). Tune it
  against real developer↔CI hit rates before relying on it broadly. Bumping the
  schema re-keys the cache: existing entries miss once and are rewritten.
- Localized path separators may be normalized to `/` in some segments. Ninja
  matches dependencies separator-insensitively, so this is cosmetic.
- Non-C/C++ inputs (for example Windows `.rc` resource files) are correctly
  classified as non-cacheable and passed through.
- `FASTCACHE_TIMEOUT_MS` bounds one exchange, not a whole invocation. Direct mode
  makes a separate manifest round-trip before the object fetch, so a compile
  against a daemon that accepts and then goes silent can wait up to twice the
  timeout before falling back.
- A dispatched compile exceeding `FASTCACHE_DISPATCH_TIMEOUT_MS` is abandoned by
  the client, which hands the lease back and compiles locally — so the build still
  succeeds and the key is not pinned for the scheduler's lease timeout. The
  **worker** does not learn about it: it runs the compile to completion and writes
  back an object nobody reads, so that CPU is spent twice.
- **That deadline is also how long a genuinely dead worker goes unnoticed**, and it
  is ten minutes rather than the ten seconds a dispatch used to get. A flat ceiling
  cannot separate a worker that is still compiling from one whose machine has
  vanished, so sizing it for the slowest legitimate translation unit — which is the
  only safe choice — makes hard-failure detection sixty times slower. Both halves
  are what a periodic progress frame from the worker would close
  ([#245](https://github.com/LASTRADA-Software/fastcached/issues/245)).

## Measured behaviour

Validated against a large real C++ codebase by replaying its
`compile_commands.json` (4223 translation units) through the launcher twice —
once to populate, once to measure. On a 400-TU sample the second pass reached a
**99.7% hit rate** (393 of 394 cacheable), with the classification of the
non-cacheable remainder identical across both passes: stable and correct.
