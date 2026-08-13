# fastcached - Fast Cache Daemon

## Project Architecture

A layered C++23 server. Each layer reaches its collaborators through a
narrow interface so the whole thing is testable end-to-end against an
in-memory transport.

```
src/FastCache/
  Core/         Errors taxonomy, Clock, Logger, BufferPool, Bytes, Endian,
                Crc32c, StringHash, Owner, Profiling (Tracy wrappers)
  Async/        Task<T>, Cancellation, ResumeOn, IReactor + TestReactor and the
                platform reactors (EpollReactor / IocpReactor / KqueueReactor)
  Net/          ISocket, IListener, IoAwaitable, IAdmissionControl, SocketAddress,
                BlockingSocket (Winsock + POSIX),
                EpollSocket / IocpSocket / KqueueSocket (reactor-driven),
                InMemoryTransport (paired pipes + InMemoryListener),
                Framing/ByteReader (line and length-prefixed)
  Cli/          UsageDoc (usage text as data: sections of aligned rows and
                prose, rendered with an ANSI palette) and Options (OptionSpec
                row type, the matching rules, the one parse loop). Dependency-
                free by design — std plus the header-only ConfigError — so
                fastcache-cc can compile it in without linking the library
  Cache/        IStorage atomic primitives (incl. Prefetch — warm a tier with
                no read side effect), CacheEntry, CacheEngine,
                InMemoryLruStorage, CowTreeStorage (CoW B+tree, src/CowTree),
                LayeredStorage (L1 LRU over L2 disk), ShardedStorage
                (key-hash fan-out), TracingStorage (Tracy zones)
  CompileCache/ PathCanon (absolute<->canonical-token path rewriting +
                showIncludes/depfile region grammar), CompileValue (object-blob
                + tagged-text-region framing), CohortManifest (cohort-id ->
                key-set + reverse index) — the compile-cache executor's domain
                logic
  Protocol/     IProtocolHandler, ProtocolAutodetect, MemcachedText,
                MemcachedMeta (1.6 mg/ms/md/ma/me/mn), MemcachedBinary,
                RedisResp (RESP2), CompileCacheHandler (the executor: custom
                0xFC binary protocol, canonicalize-on-STORE / serve-canonical-
                on-FETCH, leading-key cohort prefetch)
  Server/       Connection (per-client coroutine), Server,
                ReactorServerLoop (the server driver)
  Platform/     IDaemonHost (ForegroundHost / PosixDaemonHost / WindowsServiceHost),
                ISignalSource, DaemonControls (process-wide stop/reload flags),
                CpuAffinity, HostMemory, ServiceControl, Terminal,
                Environment (the one place the process environment is read),
                FileTrust (could only an administrator have put a file here?)
  Config/       Config, CliParser, ByteSize, YamlReader (yaml-cpp), ConfigReloader,
                EnvExpand ($VAR/${VAR} in path settings), DefaultConfigPath
                (per-platform config lookup + --seed-config, behind IConfigPathProbe)
  Metrics/      IMetricsSink + AtomicMetricsSink
```

Every executable lives under `src/apps/<name>/` and declares its own target and
install rule there; `src/apps/CMakeLists.txt` holds the app table that gates
each one, so adding an executable is adding a row:

```
src/apps/
  fastcached/               the daemon (FASTCACHED_BUILD_DAEMON, default ON)
  fastcache-cc/             the compiler launcher (FASTCACHED_BUILD_LAUNCHER,
                            default ON) — a drop-in sccache-style launcher that
                            keys on preprocess+relativized-args, FETCHes and
                            hit-replays with include paths localized,
                            misses→compile→STORE, and falls back safely on any
                            cache error. Config via `FASTCACHE_*` env, wired
                            through `CMAKE_<LANG>_COMPILER_LAUNCHER`. Platform
                            work sits behind `IProcessRunner` / `ITcpClient`,
                            so main.cpp's flow logic is platform-free. Compiles
                            in `Cli/UsageDoc.cpp` plus `Platform/Environment.cpp`
                            and `Platform/Terminal.cpp` (see `_fc_cc_core`), so
                            its help renders and colorizes exactly like the
                            daemon's without linking the library. `Cli/Options`
                            is header-only, so including it costs no build row.
  compile-cache-testclient/ low-level `0xFC` protocol probe + cross-depth
                            validation (FASTCACHED_BUILD_TESTCLIENT, default
                            OFF — test infrastructure, never installed)
```

Platform service integration and OS packaging live under `packaging/`, which
follows the same table idiom — one descriptor row per installed asset, so a
new man page or logrotate snippet is a new row rather than a new
`install()` call:

```
packaging/
  CMakeLists.txt      the asset install table (source|destination|kind|name|
                      component); exports the config-file list reused by the
                      dpkg conffiles and rpm %config filelists
  linux/              system + user systemd units, sysusers.d/tmpfiles.d,
                      the commented /etc/fastcached/fastcached.yaml, and the
                      DEB/RPM maintainer-script templates (*.in)
  macos/              /etc/paths.d entry, the per-component postinstall
                      templates, the uninstaller, and the installer panes
  windows/            WiX fragment driving --install-service / --uninstall-service
```

`cmake/Packaging.cmake` turns that into `.deb`/`.rpm`/`.pkg`/`.msi` via CPack.
These constraints are load-bearing and have each already been a bug:

- **A flag is one row, and every binary's row table drives both parsing and
  help.** The daemon used to declare flags four ways — hand-written `if (arg ==
  …)`, a descriptor array, two inline `initializer_list<tuple<…>>` tables, and
  seventeen copy-pasted blocks in `HandleTypedFlag` — and then spell every name
  again in a separate help table, so a flag could be accepted but undocumented
  or documented but rejected. `CliOptions()` is now the single source of truth,
  and the help column is *derived* from `primary`/`alias`/`operand` rather than
  restated. Rendering lives in `Cli/UsageDoc`, which must stay dependency-free:
  `fastcache-cc` compiles it in rather than linking `FastCache`, so an include
  of anything from `Config/` there breaks the launcher's link, not just its
  build. Two remaining hand-written spellings are guarded by tests rather than
  generated: `BuildServiceArgv` (a `ServiceControl_test` case walks
  `CliOptions()` and requires each non-excluded flag to be emitted — the
  exclusions, `--requirepass` above all, are listed with their reasons) and the
  launcher's `FASTCACHE_*` oracle list in `LauncherCli_test`.
- **The supervisor's launch arguments must not pass `--daemon`.** The POSIX
  daemonize path double-forks and sends stdout/stderr to `/dev/null`, which
  silences journald; its pidfile is also written after both parents exit, racing
  `Type=forking`. launchd has the identical problem — it reaps the forked job
  instantly as "exited" — which is why `BuildServiceArgv` takes an
  `EmitDaemonFlag` rather than always emitting it.
- **A config the operator named is strict; one the daemon found is not.**
  Without `--config`, `DefaultConfigPath` walks a per-platform candidate table
  (user location before machine-wide) and takes the first entry that exists *and
  opens for reading* — `Config/DefaultConfigPath.cpp` is the single source of
  truth for that order, for what `--help` lists, and for where `--seed-config`
  writes. Readability, not mere existence, is the test: the macOS system config
  is mode `0640 root:_fastcached`, so a per-user agent has to fall through it
  rather than fail to start. A discovered file that is absent or unreadable is
  skipped silently; one that parses badly is still fatal, as is a missing file
  named with `--config`. The resolved path goes into a *local* in `main.cpp` and
  never back into `parsed->config` — that object is what `InstallService`
  registers, and a discovered path baked into `ProgramArguments` would outrank
  the file itself forever (see the next bullet) and make
  `InlineCredentialRejection` name a path nobody typed.
- **The lookup's two rules both turn on one question: is this process the
  machine-wide daemon, or somebody's own?** `probe.IsPrivilegedProcess()` (root;
  elevated administrator or LocalSystem on Windows, via `CheckTokenMembership`,
  which is false for the unelevated half of a split token) decides both halves,
  and neither is driven by the platform or by the row alone:
  - **Unprivileged runs skip every `ConfigScope::System` row.** The machine-wide
    file describes the system service, whose cache only the service account can
    write, so a `systemctl --user` instance that adopted its `storage_path:`
    would fail to open that directory and be respawned until the start limit
    tripped — the out-of-the-box restart loop the user unit's header says it
    exists to prevent. Readability is not enough of a filter here: the packaged
    `/etc/fastcached/fastcached.yaml` is `0644 root:root` and readable by all.
  - **Privileged runs trust-check *every* row, per-user ones included.** `$HOME`
    and `$XDG_CONFIG_HOME` are inputs an unprivileged account often controls and
    sudo does not always reset, so checking only `System` rows would leave
    `sudo -E fastcached` taking root's `storage_path:` from a file that account
    wrote. A path named with `--config` is never checked — that is the operator's
    assertion to make, and it is the escape hatch when a location is refused.
- **A machine-wide config is only obeyed when only an administrator could have
  written it.** `C:\ProgramData` grants `BUILTIN\Users` create-file on every
  subdirectory it hands down to, so moving the Windows config there made the
  configuration of a *LocalSystem* service plantable by any standard account —
  `storage_path:` alone turns that into arbitrary directory creation and file
  writes as SYSTEM. Two halves, and both are needed: the MSI owns
  `%ProgramData%\fastcached` and gives it a protected access list of its own
  (`PermissionEx`, not `Permission` or `util:PermissionEx` — those take an
  account *name* and are wrong on a non-English Windows), and `DefaultConfigPath`
  refuses a candidate whose directory fails `Platform/FileTrust`. The test there
  is the containing directory's owner and entries, never the file's owner: on
  Windows a new object belongs to its *creator*, so a config seeded by hand from
  an elevated shell is owned by that administrator's own account, while a file
  planted by a standard account is granted to that account's own SID through the
  inherited `CREATOR OWNER` entry — an owner whitelist rejects the first and an
  entry scan misses the second. A rejection goes to stderr *and* through the
  logger once there is one: a service started by the SCM has no console, so
  stderr alone would put the message nowhere in the one deployment where a
  machine-wide config is the norm, and a file that is present, readable and
  ignored anyway is the silent no-op this list exists to prevent.
- **`--seed-config` secures the directory before it looks for the file, not
  after.** The whole point of the repair is the case where something is *already*
  there: any standard account can create a `%ProgramData%` subdirectory and drop
  a config into it long before the installer runs, and the MSI cannot undo that
  on its own — `PermissionEx` replaces the access list but not the owner, who
  keeps `WRITE_DAC` regardless. So seeding secures the parent first (repairing a
  squat), then decides what to do about the file: seed-once keeps an operator's
  edits, but a file found in a directory that until that moment anybody could
  write is not established to be an operator's, and is reported rather than
  blessed by silence or destroyed by overwriting. Seeding refuses outright when
  it has the rights for none of this, and deletes a directory it created but
  could not secure — which would otherwise be the very shape being defended
  against, authored by the defence.
- **`ExecStart` still passes `--config` on Linux and macOS — by choice, not
  necessity.** It predates the lookup, where its absence made `ConfigReloader`
  have nothing to re-read and `systemctl reload` a silent no-op; the lookup now
  closes that hole for every daemon started without the flag. The packaged units
  keep it because the path is unambiguous there and CI asserts it. Windows goes
  the other way: its custom action registers *no* `--config`, so a seed that did
  not happen degrades to built-in defaults instead of a service that fails at
  every start. The per-user launchd agent and the systemd user unit pass none —
  the packaged config describes the system daemon, whose cache only the service
  account can write.
- **`--install-service` registers the *command-line* config, not the merged
  one.** A flag in `ProgramArguments` outranks the same key in YAML for the life
  of the registration, so baking merged values in froze every configured key at
  install time and made later edits to that same file silent no-ops — and copied
  `requirepass:` out of a mode-0640 file into a world-readable plist. Hence
  `main.cpp` hands `parsed->config` to `InstallService`, and
  `InlineCredentialRejection` refuses a `--requirepass` typed on the install
  command line rather than dropping or publishing it — *including* alongside
  `--config`, since nothing there can tell whether the named file carries the
  secret, and accepting it was the silent drop under another name.
- **What reaches a supervisor must survive its own parser.** Every registration
  flag is re-read by the daemon at the next start, so a value that cannot be
  spelled back is a service that registers cleanly and then fails forever:
  `--listen=[::]:11211` came back as `--listen=:::11211`, which the CLI rejects,
  and a Windows path ending in `\` escaped its own closing quote and swallowed
  the flags after it. `FormatListenHost` and `MaybeQuote` are where that round
  trip is kept honest. `ServiceNameRejection` covers the other direction: the
  name is concatenated into the directory launchd scans, so a separator writes a
  root-owned plist nothing knows how to remove.
- **Teardown must address every domain, not re-probe for one.** Which launchd
  domain a user agent lives in is decided at install time — `gui/<uid>` needs an
  Aqua session, so an SSH install lands in `user/<uid>`. Re-probing at uninstall
  booted out a job that was never there and reported success while the real one
  kept the port. `BootOutEverywhere` walks the whole `ScopeTraits::domains` row,
  and `fastcached-uninstall` mirrors it.
- **The package payload is rooted at `/`, not `/usr`.** `/etc` cannot sit under
  a `/usr` prefix, so `FASTCACHED_INSTALL_BINDIR`/`DOCDIR` spell their own
  `usr/` (and `opt/fastcached/` on macOS). A relative destination for the units
  would put them where systemd never looks — and on macOS an *absolute*
  `install(DESTINATION)` escapes CPack's staging tree and writes to the build
  host's real filesystem.
- **Neither a macOS `.pkg` nor an MSI has a conffile mechanism.** Both overwrite
  their payload on every install, so on both the live `fastcached.yaml` is
  deliberately not payload: only a `.default` template ships, and it is copied to
  the real location exactly once, when nothing is there. macOS does this in the
  Runtime postinstall (`seed-config.sh.inc`), Windows in a custom action running
  the daemon's own `--seed-config` — which takes its destination from
  `SystemConfigPath`, so the seeded file and the startup lookup cannot disagree.
  Only the DEB conffile and the RPM `%config(noreplace)` can ship the live file
  directly. Uninstalling leaves the config behind on every platform.
- **An HTML installer pane must begin with its doctype.** Installer.app decides
  HTML from plain text by sniffing the first bytes of the resource, so the
  `<!-- SPDX-License-Identifier -->` header that opens every other file in the
  tree made the welcome and read-me panes render with every tag visible — the
  `.txt` license pane looking right is what disguised it. `mime-type="text/html"`
  on the Distribution XML element does *not* override the sniff (tried, and the
  panes stayed raw), so the doctype goes first and the licence comment after it.
  The same files need an explicit `<meta charset="utf-8">`: without it the em
  dashes arrive as mojibake, a defect the raw markup was hiding.
- **Third-party `install()` rules must be excluded.** A CPM-fetched zstd brings
  its own, and with the payload rooted at `/` they put `zstd.h` and `libzstd.a`
  into `/include` and `/lib` on the user's machine. Hence `EXCLUDE_FROM_ALL`.
- **macOS binaries must link nothing outside `/usr/lib`.** `CPM_USE_LOCAL_PACKAGES`
  defaults ON and makes CPM prefer Homebrew's shared yaml-cpp, so the package job
  passes `-DCPM_USE_LOCAL_PACKAGES=OFF -DOPENSSL_USE_STATIC_LIBS=ON` and CI
  asserts the result with `otool -L`. `CMAKE_OSX_DEPLOYMENT_TARGET` is pinned to
  13.3 (the floor at which the system libc++ has floating-point `std::to_chars`,
  which `std::format` needs) and must be set *before* `project()`.

`fastcached` and `fastcache-cc` are both installed. Two things the launcher's
cache key depends on, both of which have already caused silent hit-rate
collapses and are now covered by regression tests:

- **Preprocessing must suppress line markers** (`/EP` on MSVC, `-E -P` on GNU).
  A `# 1 "/abs/path.cpp"` marker embeds the checkout path in the hashed text.
- **`/` introduces an option only on Windows.** On POSIX it starts an absolute
  path, and treating it as an option leaves absolute paths unrelativized.

Both break cross-checkout sharing while every unit test still passes, so
`scripts/compile-cache-e2e.sh` (POSIX) and `run-launcher-e2e.ps1` (Windows)
assert the property end-to-end in CI on both platforms.

Production flow: `main()` -> CLI -> optional YAML -> `ConfigReloader` ->
`CacheEngine` over `InMemoryLruStorage` (or, when `--storage` is set, a
`ShardedStorage` of `LayeredStorage(InMemoryLruStorage, CowTreeStorage)` —
an in-memory L1 over the on-disk B+tree L2) -> `RunReactorServer`. The
reactor (IOCP / epoll / kqueue) multiplexes every connection on its event
loop, so the number of concurrent clients is bounded by memory, not by a
worker count. `--threads` runs that many independent single-threaded
reactors, each pinned to a core, with every connection pinned to one reactor
for its lifetime. On Windows the persistent backend additionally drains the
IOCP reactor from several threads so a blocking page-store `fsync` overlaps
with serving other connections; the disk backend is therefore always wrapped
in a `ShardedStorage` for thread safety.

## Design Patterns & Principles

### Error handling: `std::expected<T, E>`
Prefer `std::expected<T, E>` for fallible API surface. The error taxonomy
is split: `NetError`, `ProtocolError`, `StorageError`, `ConfigError`.
Chain monadically with `and_then`, `or_else`, `transform`,
`transform_error` rather than nested `if`s. Reserve exceptions for
programmer errors (precondition violation, contract misuse).

### Dependency injection
**This is a load-bearing principle, not a nice-to-have.** Anything that
touches I/O, time, randomness, the filesystem, the network, or any other
ambient/global resource is reached through an interface — never through a
concrete type, a singleton, or a free function with hidden state. The
existing seams are `IClock`, `IReactor`, `ISocket`/`IListener`,
`IStorage`, `ILogger`, `IDaemonHost`, `ISignalSource`,
`IAdmissionControl`, `IMetricsSink`. Collaborators are passed in (usually
by reference or `unique_ptr` at construction), so every layer can be
exercised in isolation: tests substitute deterministic fakes
(`ManualClock`, `TestReactor`, `InMemoryTransport`, `NullLogger`,
`CapturingLogger`, `ScriptedSignalSource`) and the whole server runs
end-to-end without a real socket or a real clock.

When you add a component that does I/O or depends on the environment,
**define the interface first and inject it** — do not reach for the
concrete type directly. If you find yourself wanting a global, a `static`
mutable, or a direct `::time()`/`::read()`/`new ConcreteThing` call in
business logic, that is the signal to introduce (or reuse) a seam instead.
Deviate from this only with a *strong, explicitly stated* reason (e.g. a
genuinely pure leaf computation with no environment coupling); the default
answer is "inject it".

### Data-driven design
**Behaviour is described by data; code interprets that data.** This is
equally load-bearing and goes well beyond "no magic numbers". The aim is
that adding a flag, a protocol verb, a storage backend, or an error code
is a matter of *adding a row to a table*, not editing logic scattered
across the codebase. Concretely:

- **One source of truth per concept.** The CLI flag table is data; the
  storage-record layout is documented and derived in one place; the
  per-DBMS / per-protocol dispatch lives in a single switch each. There is
  exactly one place to change when the concept changes.
- **No naive, hand-rolled repetition.** If two branches differ only by a
  value, lift the value into a descriptor/table and write the logic once.
  Copy-pasted blocks that diverge only in constants, names, or types are a
  defect — replace them with a data table the code iterates over, or a
  small generic helper.
- **Built for extension.** Prefer designs where the next case
  (flag, verb, backend, metric, signal) is a new table entry or a new
  interface implementation, not a new `if`/`else` arm threaded through
  existing functions. Open for extension, closed for invasive modification.
- **Tables over conditionals.** A `switch`/`if` ladder that mirrors a fixed
  set of named things is usually a table in disguise; express it as data
  (a descriptor array, a lookup map, a dispatch table) and drive it with a
  range-based loop or `std::ranges` pipeline.

As with DI, **adhere to this unless there is a very strong, explicitly
justified reason not to.** When in doubt, ask: "if a sixth case showed up
tomorrow, how many places would I edit?" If the answer is more than one,
the design is not data-driven enough yet.

### RAII for resource handles
Sockets, listeners, log files, coroutine handles — every resource is
owned by an RAII wrapper. `PooledBuffer` returns to its `BufferPool` on
destruction; `Task<T>`'s `Awaiter` takes ownership of the coroutine
handle on construction so the temporary `Task` cannot tear the coroutine
down across a suspend point.

## C++ Coding Guidelines (self-contained — no external `cpp.md` required)

### Baseline (general C++23)
- **Data-driven design (non-negotiable)** — describe behaviour as data and let code interpret it. No hard-coded magic values; no copy-pasted branches that differ only by a constant/name/type; new cases should be a new table row or descriptor, not a new hand-written `if`. Prefer tables/descriptors and `std::ranges` over conditional ladders. See the "Data-driven design" principle above; deviate only with a strong, stated reason.
- **Dependency injection (non-negotiable)** — reach every I/O / time / randomness / filesystem / environment dependency through an injected interface, never a singleton, global, or direct concrete call. Define the seam first, then inject it. See the "Dependency injection" principle above; deviate only with a strong, stated reason.
- **Doxygen** on every new public function (params, return), class, struct, and member:
  ```cpp
  /// Short description.
  /// @param name Description.
  /// @return Description.
  ```
- **`const` correctness** throughout (refs, pointers, member functions).
- **C++23 features** — `constexpr`, `std::ranges`, `std::format`, `std::expected` and its monadic methods (`and_then`, `or_else`, `transform`, `transform_error`).
- **C-style loops are forbidden.** Use range-based `for`, `std::views::iota`, and other range views for generation/transformation.
- **`std::span`** for arrays and contiguous sequences.
- **`auto` type deduction** for readability; **structured bindings** for tuple-like returns.
- **`clang-format` after every change** — use the project `.clang-format`.
- **`clang-tidy` reports must be fixed at the source.** Never silence with `NOLINT` — address the underlying issue. The `clang-debug` preset enables `clang-tidy` automatically.
- **No `k`-prefix on identifiers.** Do not use the Google-style `kFoo` prefix for constants, enumerators, or any other symbol — it violates the project `.clang-tidy` naming convention. Use `Foo` (PascalCase) for constants/enumerators and `foo`/`fooBar` for locals and members instead.
- **All changes covered by unit tests.** Aim to **increase** coverage with every PR.
- **No raw owning pointers.** Use `std::unique_ptr` / `std::shared_ptr` for ownership; RAII for resources.
- **No new third-party dependencies** without strong justification.

### Project-specific additions
- Public headers must be **self-contained** (compile standalone, no PCH dependency).
- Public symbols live in the `FastCache` namespace.
- Mark `std::expected`-returning APIs `[[nodiscard]]`.
- Prefer `std::expected<T, SomeError>` over throwing on the public API surface.

## Building

CMake presets live in `CMakePresets.json`. Common entry points:

```sh
# Clang Debug with PEDANTIC + ASan + UBSan + clang-tidy (the default agent preset; Linux + macOS)
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug

# Linux — GCC Debug
cmake --preset gcc-debug && cmake --build --preset gcc-debug

# Linux — Coverage (HTML in out/build/clang-coverage/)
cmake --preset clang-coverage
cmake --build --preset clang-coverage

# Linux — sanitizer-only presets
cmake --preset clang-asan-ubsan
cmake --preset clang-tsan

# Linux/macOS — RelWithDebInfo + Tracy profiler (see "Profiling with Tracy")
cmake --preset clang-tracy
cmake --build --preset clang-tracy

# Windows — MSVC CL Debug (requires VCPKG_ROOT in env)
cmake --preset cl-debug
cmake --build --preset cl-debug

# Windows — clang-cl Debug
cmake --preset clangcl-debug
cmake --build --preset clangcl-debug
```

`PEDANTIC_COMPILER_WERROR=ON` is the default for Windows presets — warnings break the build, fix them at the source.

`USE_COMPILER_CACHE` (default ON, `cmake/CompileCache.cmake`) fronts the compiler
with our own `fastcache-cc` when it is on `PATH` and `FASTCACHE_ADDR=host:port`
names a daemon — `FASTCACHE_SRCROOT`/`FASTCACHE_BUILDTREE` are injected from the
source and build trees — and otherwise falls back to `sccache`. A cache hit
reproduces only the object file, so with either launcher active the module scan
and precompiled headers are turned off and MSVC debug info is forced to `/Z7`
(a modmap flag makes the launcher's preprocess step fail, and a PCH or shared
PDB is a second artefact no hit can reproduce).

## Testing

Catch2 tests live next to the implementation files, so `Foo.cpp` has a `Foo_test.cpp`. A `test_main.cpp` serves as the entry point.

## Profiling with Tracy

[Tracy](https://github.com/wolfpld/tracy) instrumentation is **opt-in and
zero-cost when off**: it is gated behind the `TRACY_ENABLE` CMake option
(default `OFF`). When off, no Tracy header is included, nothing is linked, and
every profiling macro in `FastCache/Core/Profiling.hpp` collapses to
`(void) 0` — the default `clang-debug`/`clang-release` binaries are unchanged
and link zero Tracy symbols.

### Building the profiling daemon

```sh
cmake --preset clang-tracy        # RelWithDebInfo, TRACY_ENABLE=ON, TRACY_ON_DEMAND=ON
cmake --build --preset clang-tracy
# -> out/build/clang-tracy/target/fastcached
```

`TRACY_ON_DEMAND=ON` means the daemon buffers nothing until a profiler
connects, so it is safe to leave running; you only pay the cost while
capturing.

### Adding zones

Instrument code through the wrapper macros, never Tracy directly:

```cpp
#include <FastCache/Core/Profiling.hpp>

FC_ZONE_SCOPED;                         // zone named by source location
FC_ZONE_SCOPED_N("CacheEngine::Get");   // zone with a compile-time literal name
FC_FRAME_MARK;                          // one logical request/frame boundary
FC_THREAD_NAME("fc-worker-0");          // name the calling OS thread
FC_PLOT("lru.bytesUsed", value);        // scalar timeline (value is numeric)
```

**Coroutine constraint (must be observed):** `FC_ZONE_SCOPED*` declares a
thread-local stack-RAII guard and **must not straddle a `co_await`** — under the
reactor model the await resumes on a later frame and the guard's destructor
would corrupt Tracy's per-thread zone stack. Place zones only in synchronous
leaf functions or in `{ }` blocks containing no `co_await`. `FC_FRAME_MARK` is a
stackless timeline event and is safe anywhere, including inside coroutine loops.
Macro arguments must be free of side-effects the program relies on (when Tracy
is off they are discarded unevaluated). `FC_ZONE_SCOPED_N` requires a
compile-time string literal; for a runtime label, annotate the current zone with
`FC_ZONE_NAME(ptr, len)` / `FC_ZONE_TEXT(ptr, len)` instead.

### Analyzing a capture

Build a Tracy viewer once (sources are fetched into the build tree under
`out/build/clang-tracy/_deps/tracy-src/`), or grab a prebuilt **v0.11.x** viewer
from the Tracy releases — the client and viewer protocol is version-locked.

```sh
# Interactive GUI (needs glfw/freetype/capstone/gtk3/dbus dev packages):
cmake -S out/build/clang-tracy/_deps/tracy-src/profiler -B /tmp/tracy-gui -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/tracy-gui -j        # -> /tmp/tracy-gui/tracy-profiler

# Headless capture (no GUI deps; writes a .tracy file to open later):
cmake -S out/build/clang-tracy/_deps/tracy-src/capture -B /tmp/tracy-cap -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/tracy-cap -j        # -> /tmp/tracy-cap/tracy-capture
```

Workflow — the client listens on TCP **8086**:

1. Start the daemon: `./out/build/clang-tracy/target/fastcached --bind 127.0.0.1`.
2. Connect the profiler (GUI **Connect**, or `tracy-capture -o out.tracy -a 127.0.0.1`).
   On-demand mode records only from the moment of connection, so connect **before**
   driving load.
3. Drive traffic through the hot path, e.g.
   `memtier_benchmark -s 127.0.0.1 -p 6674 -P memcache_text --ratio=1:4 -n 50000`,
   `redis-benchmark -p 6674 -t set,get -n 100000`, or a quick
   `printf 'set foo 0 0 3\r\nbar\r\nget foo\r\nquit\r\n' | nc 127.0.0.1 6674`.

What the instrumentation surfaces: thread rows named `fastcached-main` /
`fc-worker-N` / `fc-reactor`; one frame per request; the nested zone breakdown
`socket.read → LineReader.TryExtractLine → memcached.Handle*.dispatch →
CacheEngine::* → ShardedStorage::* → LruStorage::* / EvictToFit → socket.write`;
and the `lru.bytesUsed` plot for memory pressure. Use the **Statistics** window
sorted by self-time to find hotspots; a gap between a `ShardedStorage::*` zone
and its inner `LruStorage::*` zone is shard-mutex wait time.

