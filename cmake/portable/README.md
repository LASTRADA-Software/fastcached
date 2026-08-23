# Portable CMake modules

Everything in this directory is written to be copied. Each module uses nothing
but stock CMake, names no target of the project it sits in, and depends on no
other file here — so another project can vendor one and have it work, whether or
not it has ever heard of fastcached. That is the whole reason the directory
exists: `../` holds the modules that only make sense while building fastcached,
and this holds the ones that travel.

Anything added here has to keep that property. Nothing checks it automatically —
a module here that quietly grew a dependency on `../Version.cmake` would still
build fine in this repository and break in every other one, so it is worth
looking for when reviewing a change to this directory.

| Module | What it does |
| --- | --- |
| [`CompileCache.cmake`](#compilecachecmake) | Selects a compiler-cache launcher, and can install one. |
| `ClangTidy.cmake` | `-DENABLE_TIDY=ON` runs clang-tidy as part of the C++ compile. |
| `PedanticCompiler.cmake` | Turns on a wide warning set, each flag probed for support first, and optionally makes warnings errors. |
| `Sanitizers.cmake` | `-DENABLE_SANITIZER_{ADDRESS,UNDEFINED,THREAD}=ON` wires up the matching sanitizer. |

`CompileCache.cmake` is the one written *as* a drop-in: it is documented below
and covered by `ctest -R compile-cache`. The other three are ordinary build
configuration that happens to have no tie to this project — they are here
because they are copyable, not because anything about them is promised to stay
still.

## `CompileCache.cmake`

Picks a compiler-cache launcher — `fastcache-cc`, `sccache` or `ccache` — and,
when asked, installs one that is missing.

### Using it

Drop the file into your own `cmake/` directory and include it once, before
anything is compiled:

```cmake
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompileCache)
```

Include it *after* `project()` — it needs to know the compiler — and *before*
any dependency is fetched, so fetched dependencies are cached too.

It picks the first launcher that is both installed and usable:

1. **`fastcache-cc`**, but only when a `fastcached` daemon actually answers.
   Its cache entries are keyed independently of the checkout path, so a CI
   runner and a developer working from different directories share hits.
2. **`sccache`**
3. **`ccache`**
4. none — the build simply compiles.

"A daemon actually answers" is checked rather than assumed: configure compiles
one small file through the launcher and accepts only a reported cache outcome.
A launcher whose daemon is down still compiles fine, so without that check every
translation unit would pay a failed connection attempt in silence.

### Installing the launcher automatically

By default the module uses what is already installed. `FASTCACHE_AUTO_INSTALL`
lets it fetch `fastcache-cc` instead of leaving the build uncached:

```cmake
set(FASTCACHE_AUTO_INSTALL ON CACHE BOOL "" FORCE)   # before include(CompileCache)
```

or `-DFASTCACHE_AUTO_INSTALL=ON` on the command line.

It downloads a prebuilt binary for the host's OS and architecture from the
latest stable release, verifies the published SHA-256, checks the binary runs,
and stages it in a per-user directory shared by every repository and build tree
on the machine — so it is fetched once per machine and version, not once per
build.

It only does this when **none** of the three launchers is installed. A launcher
you installed yourself is a decision already made, and this will not override it.

#### Options

| Option | Default | Meaning |
| --- | --- | --- |
| `USE_COMPILER_CACHE` | `ON` | Use a launcher at all. |
| `FASTCACHE_ADDR` | `127.0.0.1:6674` | Where the daemon is. Empty opts out of `fastcache-cc` entirely. |
| `FASTCACHE_AUTO_INSTALL` | `OFF` | Fetch `fastcache-cc` when no launcher is installed. |
| `FASTCACHE_AUTO_INSTALL_VERSION` | *(empty)* | Install this exact version instead of resolving the latest. |
| `FASTCACHE_AUTO_INSTALL_DIR` | per-user cache dir | Where fetched binaries are staged. |
| `FASTCACHE_AUTO_INSTALL_REPO` | `LASTRADA-Software/fastcached` | Repository to fetch from. |
| `FASTCACHE_AUTO_INSTALL_API` | `https://api.github.com` | API used to resolve the latest release. |
| `FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE` | `https://github.com` | Where release archives are downloaded from. |
| `FASTCACHE_AUTO_INSTALL_TTL_HOURS` | `24` | How long a resolved "latest" is reused before asking again. |

#### What it will not do

**It will not fail your configure.** No published binary for your platform, no
network, a corrupt download, a binary that will not run on this host — each ends
in a single status line and a fall-through to the next launcher, or to no
caching at all. A project that vendors this file must keep building on a laptop
on a train.

**It will not reach the network unless asked.** `FASTCACHE_AUTO_INSTALL`
defaults to `OFF`. With it on, the API is consulted at most once per day per
machine (the resolved answer is cached), and not at all once the binary for that
version is staged.

**It will not silently overwrite your choice.** If a compiler launcher is
already set — by a preset, a toolchain file, or `-D` — the module says so and
leaves it alone.

#### Behind a proxy, or offline

Set `GITHUB_TOKEN` or `GH_TOKEN` and it will be used for the API call, which
raises GitHub's unauthenticated rate limit of 60 requests per hour per address —
worth doing on a CI fleet sharing one egress address.

To install without reaching GitHub at all, mirror the release archives and pin a
version:

```cmake
-DFASTCACHE_AUTO_INSTALL=ON
-DFASTCACHE_AUTO_INSTALL_VERSION=0.1.0
-DFASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE=https://mirror.example.com
```

The archive is then expected at
`<base>/<repo>/releases/download/v<version>/fastcached-<version>-<platform>.tar.gz`,
laid out exactly as the release archive is.

### Starting a daemon automatically

`FASTCACHE_AUTO_INSTALL` alone still leaves a genuinely clean machine
uncached: `fastcache-cc` caches nothing unless a `fastcached` daemon answers
at `FASTCACHE_ADDR`, and installing the launcher does not install one.
`FASTCACHE_AUTO_START` closes that gap:

```cmake
set(FASTCACHE_AUTO_START ON CACHE BOOL "" FORCE)   # before include(CompileCache)
```

or `-DFASTCACHE_AUTO_START=ON` on the command line. When nothing answers at
`FASTCACHE_ADDR`, the module stages a `fastcached` binary from the very same
release archive it fetched (or would fetch) the launcher from, and starts it
in the background — persistently, with its cache on disk under
`FASTCACHE_AUTO_START_STORAGE_DIR` so it survives being restarted. It keeps
running after the configure exits: on a developer machine that is the point,
and nothing here stops it for you.

It is a separate opt-in from `FASTCACHE_AUTO_INSTALL`, and defaults `OFF`
independently, for two reasons. Starting a long-lived background process is a
materially bigger side effect than downloading a file, so wanting one must not
imply wanting the other. And several projects rely on CI having **no**
`fastcached` reachable, so a build transparently falls back to `sccache` —
`FASTCACHE_AUTO_START` would remove that boundary if it defaulted on for
everyone rather than only those who ask for it. There is no CI-environment
detection backing this up; an explicit `-DFASTCACHE_AUTO_START=ON` is trusted
exactly like every other flag here.

Two configures racing on the same `FASTCACHE_ADDR` — two build trees on one
machine, say — do not both try to start one: before starting anything, the
module checks whether something is already answering there, and if so treats
it as its own. Losing that race is success, not failure.

| Option | Default | Meaning |
| --- | --- | --- |
| `FASTCACHE_AUTO_START` | `OFF` | Start a fetched `fastcached` in the background when none answers at `FASTCACHE_ADDR`. |
| `FASTCACHE_AUTO_START_STORAGE_DIR` | `<FASTCACHE_AUTO_INSTALL_DIR>/daemon-storage` | Where the auto-started daemon keeps its persistent cache. |

**It will not fail your configure either.** No daemon binary published for
this platform, the archive fetch failing, the process exiting immediately —
each ends in one status line and a fall-through to the probe finding no
daemon, exactly the behaviour without `FASTCACHE_AUTO_START` at all.

**It will not register a service.** This is a plain background process, not
something `--install-service` would set up under the SCM or launchd — those
are a deliberate, human-driven decision documented separately. Nor does it
install anything system-wide: the daemon binds `FASTCACHE_ADDR`'s own host
(loopback by default) and keeps its cache in a per-user directory.

### Caveats worth knowing

- A cache hit reproduces the object file and nothing else, so while a launcher is
  active the module turns off precompiled headers and C++20 module scanning, and
  forces MSVC to embed debug info (`/Z7`) rather than share a PDB. A second
  artefact no hit can reproduce would otherwise go missing.
- Prebuilt binaries exist only for the platforms the project publishes. Anything
  else falls back, and says so.
