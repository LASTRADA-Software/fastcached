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

All layers are wired and the daemon runs. The codebase covers a thread-pool-
backed server (default) and a single-threaded reactor (IOCP / epoll / kqueue)
opt-in, an LRU in-memory storage, an optional copy-on-write persistent
backend (the sibling [`CowTree`](src/CowTree/README.md) library), key-hash
sharding for parallel writes, CLI + YAML config with SIGHUP reload, and a
POSIX-daemon / Windows-service host. CI builds on Linux (clang + gcc), macOS,
and Windows (MSVC + clang-cl), and three separate jobs spin up the daemon and
assert sccache gets a cache hit using each wire protocol.

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
  --max-memory=<size>    in-memory budget; k/m/g = KiB/MiB/GiB or N% of host RAM (default 64 MiB)
  --log-level=<level>    trace|debug|info|warn|error|fatal (default info)
  --storage=<path>       persist cache to a CoW-tree file (default: in-memory only)
  --storage-durability=<mode>  fsync|batched|none for --storage (default batched)
  --storage-max-value=<size>   per-value byte cap for --storage; k/m/g suffixes accepted (default 1m)
  --execution-model=<mode>     auto|threaded|reactor (default auto)
                                   auto: reactor for in-memory, threaded for --storage on disk
  --threads=<N>                worker thread count for threaded mode (default: hardware_concurrency)
  --storage-shards=<N>         shard storage into N partitions for write parallelism
                                   when N>1 and --storage is set, --storage must be a directory
  --daemon               daemonize (POSIX) / register as Windows service
  --pidfile=<path>       POSIX daemon mode only
  --service-name=<name>  Windows service name (default FastCached)
  --help, -h             show this help and exit
  --version, -V          show version and exit
```

`--max-memory` takes an integer with an optional unit suffix:
`64m` is 64 × 1024² = 67108864 bytes; a plain integer is interpreted as bytes;
a trailing `%` (e.g. `50%`) sets the budget to that fraction of the host's
total RAM, queried at startup.

`--storage=<path>` switches the cache from in-memory-only to a copy-on-write
B+tree backed by `<path>`. Every commit is crash-consistent (the file always
matches either the previous or the new transaction; a kill -9 at any instant
leaves no half-written state), and reopening the file picks up the previous
state. `--storage-durability` trades durability for throughput: `fsync`
flushes on every commit, `batched` flushes at commit boundaries only (the
default), `none` relies on the OS page cache.

### Run with 30% of host RAM and a persistent cache file

```sh
# Linux / macOS — persist to ~/.cache/fastcached/cache.cow,
# budget = 30% of total host RAM, fsync-on-commit durability.
mkdir -p ~/.cache/fastcached
fastcached \
    --port=11211 \
    --max-memory=30% \
    --storage=$HOME/.cache/fastcached/cache.cow \
    --storage-durability=fsync &

export SCCACHE_MEMCACHED=tcp://127.0.0.1:11211
sccache g++ -std=c++23 -c hello.cpp -o hello.o
```

```powershell
# Windows (PowerShell) — same idea, %LOCALAPPDATA% for the cache directory.
New-Item -ItemType Directory -Force "$env:LOCALAPPDATA\fastcached" | Out-Null
Start-Process fastcached -ArgumentList `
    '--port=11211', `
    '--max-memory=30%', `
    "--storage=$env:LOCALAPPDATA\fastcached\cache.cow", `
    '--storage-durability=batched'

$env:SCCACHE_MEMCACHED = 'tcp://127.0.0.1:11211'
```

The first run writes entries to disk; stop the daemon (Ctrl-C, kill, or a
power loss) and start it again with the same flags and the cache picks back
up from where it left off — no warm-up.

### Concurrency: thread pool + sharded storage

`--execution-model` defaults to `auto`, which picks **reactor** for the
in-memory cache (single thread is plenty when every operation is a hash
lookup) and **threaded** when `--storage=<path>` is set (disk I/O and
per-shard locks benefit from parallel workers). Pass `--execution-model=threaded`
or `=reactor` to force a choice.

In threaded mode `fastcached` serves connections on a **thread pool** sized
to `hardware_concurrency()` (override with `--threads=N`). One accept thread
feeds a bounded queue; pool workers loop popping connections and driving
each protocol coroutine to completion. Workers are created once at startup
and reused — no per-connection thread spawn, which matters on builds where
sccache opens hundreds of connections.

Storage is **sharded by key hash** when `--storage-shards>1`. Each shard
has its own `std::shared_mutex`: any number of `Get`s on the same shard
run in parallel (shared lock); writes take an exclusive lock that only
blocks operations on *that* shard, never across shards. For sccache's
read-heavy, well-hashed key space this scales reads linearly across cores.

```sh
# Pool + sharded persistent storage (--storage is treated as a directory
# whenever --storage-shards > 1):
./fastcached \
    --port=11211 \
    --max-memory=30% \
    --storage=$HOME/.cache/fastcached \
    --storage-shards=16 \
    --threads=16

# Fall back to single-threaded reactor if you need it for comparison:
./fastcached --execution-model=reactor --port=11211 &
```

`--storage` is interpreted as a regular file when `--storage-shards=1` (the
single-file mode introduced in the previous release) and as a directory
holding `shard-NN.cow` files when `--storage-shards>1`.

## YAML config (optional)

```yaml
# interface to bind on; 0.0.0.0 listens on all interfaces
bind: 0.0.0.0
# TCP port (1..65535); 11611 mirrors the value used in CI
port: 11611
# in-memory budget; k/K/m/M/g/G = KiB/MiB/GiB (1024-based),
# or "N%" to use N percent of the host's total RAM
max_memory: 30%
# one of: trace | debug | info | warn | error | fatal
log_level: debug
# optional: path to a CoW-tree file (single-shard) or directory (sharded).
storage_path: /var/lib/fastcached/cache
# optional: fsync | batched | none (default: batched)
storage_durability: batched
# optional: shard storage across N partitions for parallel writes (0 = auto,
# 1 = single file/instance, N>1 = directory with shard-NN.cow files)
storage_shards: 16
# optional: auto (default) | threaded | reactor
# auto picks reactor for in-memory storage, threaded for CoW on-disk storage
execution_model: auto
# optional: worker thread count for threaded mode (0 = hardware_concurrency)
threads: 0
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
src/CowTree/          standalone copy-on-write B+tree library (no dependency
                      on FastCache; can be lifted into other projects)
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
