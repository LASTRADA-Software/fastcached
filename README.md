# fastcached

A small in-memory cache daemon written in C++23. It speaks a subset of three
wire protocols — the memcached text protocol, the memcached binary protocol,
and Redis RESP2 — and auto-detects which one the client is using from the
first bytes on the wire. Its concrete reason to exist is being a usable
backend for [sccache](https://github.com/mozilla/sccache); CI exercises that
case across all three protocols on every build.

This is a personal project, not a replacement for memcached or Redis. It has
not been benchmarked against them and has not been used in production.

## Status

All layers are wired and the daemon runs. The codebase covers an event-driven
reactor (IOCP / epoll / kqueue) with a blocking fallback, an LRU in-memory
storage with an append-only disk log option, CLI + YAML config with SIGHUP
reload, and a POSIX-daemon / Windows-service host. CI builds on Linux (clang +
gcc), macOS, and Windows (MSVC + clang-cl), and three separate jobs spin up
the daemon and assert sccache gets a cache hit using each wire protocol.

Protocol coverage is intentionally a subset:

- **memcached text:** `get`, `set`, `add`, `replace`, `append`, `prepend`,
  `cas`, `delete`, `incr`, `decr`, `flush_all`, `stats`, `version`, `quit`.
- **memcached binary:** the core opcodes (Get / Set / Add / Replace / Delete /
  Increment / Decrement / Append / Prepend / Flush / Quit / NoOp / Version /
  Stat, plus their quiet variants and the SASL handshake stubs).
- **Redis RESP2:** `GET`, `SET` (with `NX`/`XX`/`EX`/`PX`), `SETEX`,
  `DEL` / `UNLINK`, `EXISTS`, `PING`, `ECHO`, `INFO`, `COMMAND`,
  `FLUSHDB` / `FLUSHALL`, `QUIT`.

There is no clustering, no replication, and no authentication beyond the SASL
stubs the binary protocol requires.

## Use it as an sccache backend

This is the headline use case and the one CI actually exercises
(`.github/workflows/build.yml`).

```sh
fastcached --port=11211 &
export SCCACHE_MEMCACHED=tcp://127.0.0.1:11211

sccache g++ -std=c++23 -c hello.cpp -o hello.o   # miss
sccache g++ -std=c++23 -c hello.cpp -o hello.o   # hit
sccache --show-stats
```

sccache up to 0.7.x (the version Ubuntu ships) talks to memcached over the
**text** protocol; sccache ≥ 0.8 (the prebuilt from `mozilla-actions/sccache-action`)
talks **binary**. Both work against fastcached because the listener detects
the wire format from the first bytes the client sends. The same listener also
serves RESP2 — point sccache (or anything else) at it via `SCCACHE_REDIS` and
it works the same way.

## Talk to it directly

The transcripts below are copied from the actual unit tests, so they reflect
exactly what the server emits.

memcached text:

```
> set foo 0 0 5\r\nhello\r\n
< STORED\r\n
> get foo\r\n
< VALUE foo 0 5\r\nhello\r\nEND\r\n
```

Redis RESP2:

```
> *3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n
< +OK\r\n
> *2\r\n$3\r\nGET\r\n$1\r\nk\r\n
< $5\r\nhello\r\n
```

## Command-line flags

```
usage: fastcached [options]
  --config=<path>        YAML config file; CLI flags override file values
  --bind=<addr>          bind address (default 127.0.0.1)
  --port=<num>           TCP port (default 11211)
  --max-memory=<size>    in-memory budget; suffix k/m/g = KiB/MiB/GiB (default 64 MiB)
  --log-level=<level>    trace|debug|info|warn|error|fatal (default info)
  --daemon               daemonize (POSIX) / register as Windows service
  --pidfile=<path>       POSIX daemon mode only
  --service-name=<name>  Windows service name (default FastCached)
  --help, -h             show this help and exit
  --version, -V          show version and exit
```

`--max-memory` takes an integer with an optional 1024-based unit suffix:
`64m` is 64 × 1024² = 67108864 bytes. A plain integer is interpreted as bytes.

## YAML config (optional)

```yaml
# interface to bind on; 0.0.0.0 listens on all interfaces
bind: 0.0.0.0
# TCP port (1..65535); 11611 mirrors the value used in CI
port: 11611
# in-memory budget; suffix k/K/m/M/g/G = KiB/MiB/GiB (1024-based)
max_memory: 256m
# one of: trace | debug | info | warn | error | fatal
log_level: debug
```

CLI flags override YAML values. On POSIX, `SIGHUP` triggers a re-read of the
file; on Windows, the service control manager's `PARAMCHANGE` does the same.

## Building

Requires CMake ≥ 3.28, a C++23 compiler, and Ninja. The full set of presets
lives in `CMakePresets.json`; the three most useful ones are:

```sh
# Linux / macOS — Clang with ASan + UBSan + clang-tidy
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug

# Linux — GCC
cmake --preset gcc-debug
cmake --build --preset gcc-debug

# Windows — MSVC
cmake --preset cl-debug
cmake --build --preset cl-debug
```

Dependencies (`yaml-cpp`, `Catch2`) are pulled in via
[CPM.cmake](cmake/CPM.cmake) with `CPM_USE_LOCAL_PACKAGES=ON`, so
`find_package` is tried first and only unavailable packages are fetched from
GitHub.

## Repository layout

```
src/FastCache/        library code, organised by layer (Core, Async, Net,
                      Cache, Protocol, Server, Platform, Config, Metrics)
src/fastcached/       the daemon executable's main()
src/tests/            Catch2 entry point; *_test.cpp files live next to sources
cmake/                build helpers (PedanticCompiler, Sanitizers, CPM, ...)
.github/workflows/    CI
AGENT.md              design notes, conventions, and the things contributors
                      should read before touching this code
```

## Contributing

See [`AGENT.md`](AGENT.md) for the architecture, error taxonomy, live-reload
pipeline, and the contributor conventions this project follows.

## License

Licensed under the Apache License, Version 2.0. See [`LICENSE`](LICENSE).
