# fastcached

A small, platform-independent cache daemon written in C++23. Speaks the
memcached text protocol, the memcached binary protocol, and a subset of the
Redis RESP2 protocol — enough to act as a backend for
[sccache](https://github.com/mozilla/sccache).

This is early-stage software. The architecture is laid out and the foundation
layers are in place; most of the protocol and storage code is still TBD. See
[`AGENT.md`](AGENT.md) for the design notes and contributor conventions.

## Status

| Area              | State                                                |
|-------------------|------------------------------------------------------|
| Build system      | CMake + CPM, presets for clang/gcc/MSVC/clang-cl     |
| Toolchain         | clang-format, clang-tidy (WarningsAsErrors), C++23   |
| Core/             | Errors, Clock, Logger, BufferPool, Bytes, Endian — landed with tests |
| Async/ (reactor)  | not yet                                              |
| Net/              | not yet                                              |
| Protocol/         | not yet (memcached text/binary + Redis RESP planned) |
| Cache/            | not yet (in-memory LRU + disk-persisted log planned) |
| Server/           | not yet                                              |
| Platform/         | not yet (POSIX daemon + Windows service planned)     |
| Config/           | not yet (CLI + YAML via yaml-cpp + SIGHUP reload)    |
| CI                | clang-format, Linux (clang/gcc), Windows (MSVC/clang-cl) |

Nothing here is production-ready yet.

## Building

Requires CMake ≥ 3.28, a C++23 compiler, and Ninja.

```sh
# Linux/macOS, Clang with ASan + UBSan + clang-tidy
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug

# Linux, GCC
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug

# Windows, MSVC
cmake --preset cl-debug
cmake --build --preset cl-debug
ctest --preset cl-debug

# Windows, clang-cl
cmake --preset clangcl-debug
cmake --build --preset clangcl-debug
ctest --preset clangcl-debug
```

Dependencies (`yaml-cpp`, `Catch2`) are pulled in via [CPM.cmake](cmake/CPM.cmake)
with `CPM_USE_LOCAL_PACKAGES=ON`, so `find_package` is tried first and only
unavailable packages are fetched from GitHub.

## Repository layout

```
src/FastCache/        library code, organised by layer
src/fastcached/       the daemon executable's main()
src/tests/            Catch2 entry point; *_test.cpp files live next to sources
cmake/                build helpers (PedanticCompiler, Sanitizers, CPM, ...)
.github/workflows/    CI
AGENT.md              design notes, conventions, and the things contributors
                      (or AI agents) should know before touching this code
```

## Contributing

Conventions worth reading before opening a PR:

- Function names are `CamelCase`; tests live next to sources (`Foo.cpp` →
  `Foo_test.cpp`).
- `clang-format` is mandatory; `clang-tidy` warnings break the build under the
  `clang-debug` preset, and we do not add `NOLINT` to silence them.
- Anything touching I/O, time, randomness, or storage is reached through a
  dependency-injection seam so it can be substituted in tests.
- `std::expected<T, E>` is preferred over throwing on public APIs.

The longer-form notes — including the layered architecture, the error
taxonomy, the live-reload pipeline, and the CI shape — are in
[`AGENT.md`](AGENT.md).

## License

Licensed under the Apache License, Version 2.0. See [`LICENSE`](LICENSE).
