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
  Cache/        IStorage atomic primitives, CacheEntry, CacheEngine,
                InMemoryLruStorage, CowTreeStorage (CoW B+tree, src/CowTree),
                LayeredStorage (L1 LRU over L2 disk), ShardedStorage
                (key-hash fan-out), TracingStorage (Tracy zones)
  Protocol/     IProtocolHandler, ProtocolAutodetect, MemcachedText,
                MemcachedMeta (1.6 mg/ms/md/ma/me/mn), MemcachedBinary,
                RedisResp (RESP2)
  Server/       Connection (per-client coroutine), Server,
                ReactorServerLoop (the server driver)
  Platform/     IDaemonHost (ForegroundHost / PosixDaemonHost / WindowsServiceHost),
                ISignalSource, DaemonControls (process-wide stop/reload flags),
                CpuAffinity, HostMemory, ServiceControl, Terminal
  Config/       Config, CliParser, ByteSize, YamlReader (yaml-cpp), ConfigReloader
  Metrics/      IMetricsSink + AtomicMetricsSink
```

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

1. Start the daemon: `./out/build/clang-tracy/target/fastcached --bind 127.0.0.1:11211`.
2. Connect the profiler (GUI **Connect**, or `tracy-capture -o out.tracy -a 127.0.0.1`).
   On-demand mode records only from the moment of connection, so connect **before**
   driving load.
3. Drive traffic through the hot path, e.g.
   `memtier_benchmark -s 127.0.0.1 -p 11211 -P memcache_text --ratio=1:4 -n 50000`,
   `redis-benchmark -p 11211 -t set,get -n 100000`, or a quick
   `printf 'set foo 0 0 3\r\nbar\r\nget foo\r\nquit\r\n' | nc 127.0.0.1 11211`.

What the instrumentation surfaces: thread rows named `fastcached-main` /
`fc-worker-N` / `fc-reactor`; one frame per request; the nested zone breakdown
`socket.read → LineReader.TryExtractLine → memcached.Handle*.dispatch →
CacheEngine::* → ShardedStorage::* → LruStorage::* / EvictToFit → socket.write`;
and the `lru.bytesUsed` plot for memory pressure. Use the **Statistics** window
sorted by self-time to find hotspots; a gap between a `ShardedStorage::*` zone
and its inner `LruStorage::*` zone is shard-mutex wait time.

