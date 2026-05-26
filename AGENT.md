# fastcached - Fast Cache Daemon

## Project Architecture

A layered C++23 server. Each layer reaches its collaborators through a
narrow interface so the whole thing is testable end-to-end against an
in-memory transport.

```
src/FastCache/
  Core/         Errors taxonomy, Clock, Logger, BufferPool, Bytes, Endian, Crc32c
  Async/        Task<T>, DetachedTask, Cancellation, IReactor + TestReactor
  Net/          ISocket, IListener, IoAwaitable, IAdmissionControl,
                BlockingSocket (Winsock + POSIX),
                InMemoryTransport (paired pipes + InMemoryListener),
                Framing/ByteReader (line and length-prefixed)
  Cache/        IStorage atomic primitives, CacheEntry, CacheEngine,
                InMemoryLruStorage, DiskStorage (append-only log + CRC32C)
  Protocol/     IProtocolHandler, ProtocolAutodetect, MemcachedText,
                MemcachedBinary, RedisResp (RESP2)
  Server/       Connection (per-client coroutine), Server, BlockingServerLoop
                (production thread-per-connection driver of the same handlers)
  Platform/     IDaemonHost (ForegroundHost / PosixDaemonHost / WindowsServiceHost),
                ISignalSource, DaemonControls (process-wide stop/reload flags)
  Config/       Config, CliParser, YamlReader (yaml-cpp), ConfigReloader
  Metrics/      IMetricsSink + AtomicMetricsSink
```

Production flow: `main()` -> CLI -> optional YAML -> `ConfigReloader` ->
`CacheEngine` over `InMemoryLruStorage` -> `BlockingListener` ->
`RunBlockingServerLoop` (one `std::jthread` per connection driving the
coroutine handler via `SyncRun`).

## Design Patterns & Principles

### Error handling: `std::expected<T, E>`
Prefer `std::expected<T, E>` for fallible API surface. The error taxonomy
is split: `NetError`, `ProtocolError`, `StorageError`, `ConfigError`.
Chain monadically with `and_then`, `or_else`, `transform`,
`transform_error` rather than nested `if`s. Reserve exceptions for
programmer errors (precondition violation, contract misuse).

### Dependency injection
Anything that touches I/O, time, randomness, or the filesystem is reached
through an interface: `IClock`, `IReactor`, `ISocket`/`IListener`,
`IStorage`, `ILogger`, `IDaemonHost`, `ISignalSource`,
`IAdmissionControl`, `IMetricsSink`. Tests substitute deterministic fakes
(`ManualClock`, `TestReactor`, `InMemoryTransport`, `NullLogger`,
`CapturingLogger`, `ScriptedSignalSource`).

### Data-driven design
No magic literals scattered across the code: the CLI flag table is data,
the storage-record layout is documented in one place, the per-DBMS / per-
protocol dispatch lives in one switch each.

### RAII for resource handles
Sockets, listeners, log files, coroutine handles — every resource is
owned by an RAII wrapper. `PooledBuffer` returns to its `BufferPool` on
destruction; `Task<T>`'s `Awaiter` takes ownership of the coroutine
handle on construction so the temporary `Task` cannot tear the coroutine
down across a suspend point.

## C++ Coding Guidelines (self-contained — no external `cpp.md` required)

### Baseline (general C++23)
- **Data-driven design** — avoid hard-coded magic values; prefer tables/descriptors.
- **Dependency injection** — decouple components and improve testability.
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

