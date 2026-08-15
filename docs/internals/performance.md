# Performance

Two benchmark suites answer two different questions, and confusing them is the
easiest way to reach a wrong conclusion:

| Suite | Driver | What it measures | What limits it |
| --- | --- | --- | --- |
| `bench/inproc_bench.py` | `fastcache-bench` (Catch2) | one cache operation inside the process, decomposed layer by layer | the storage stack itself |
| `bench/fastcached_bench.py` | Python clients over TCP | the whole daemon: syscalls, parsing, dispatch, storage | the network path, and the load generator |

A cached `GET` costs tens of *nanoseconds* inside the process and roughly two
*microseconds* of server CPU once a socket is involved. The wire suite
therefore cannot resolve the storage layer — a change worth 30 ns per operation
vanishes into its noise — while the in-process suite says nothing about what a
client sees. Both numbers are below, along with the one metric that connects
them.

All figures on this page were measured on an AMD Ryzen 9 9950X3D (16C/32T, two
L3 domains), Fedora Linux 44, clang 21 `-O3` (the `clang-release` preset),
`powersave` governor. Absolute numbers move with the machine; the shape does
not.

## The storage stack, layer by layer

`fastcache-bench` builds the same stack the daemon builds and adds one
production concern per row, so a nanosecond lands on a feature instead of on
guesswork. The workload is a deliberate replica of
[jitbit/FastCache](https://github.com/jitbit/FastCache)'s BenchmarkDotNet
suite — 1000 entries, four probed keys, all hits, a 10-minute TTL — because
that is the one layer at which a daemon and an in-process cache library can be
compared operation for operation.

150 Catch2 samples per row, median of 9 interleaved repetitions:

| Row | ns/op | What it adds |
| --- | ---: | --- |
| `control-unordered-map` | 5.4 | bare `std::unordered_map` — the container floor |
| `clock-now` | 15.6 | one injected `IClock::Now()`, for attribution (not a cache op) |
| `lru-unbounded` | 17.3 | `InMemoryLruStorage`, eviction off |
| `lru-bounded` | 17.3 | \+ byte budget and eviction bookkeeping |
| `lru-strict` | 15.9 | \+ exact LRU (promotes on every read) |
| `sharded` | 31.3 | \+ the shard index and the per-shard `shared_mutex` |
| `engine-steadyclock` | 53.9 | \+ the `CacheEngine` facade, reading the OS clock per operation |
| **`engine-cachedclock`** | **32.0** | the same, with the reactor-refreshed `CachedClock` — **what the daemon ships** |

Four things this table is for:

- **The byte budget is free.** `lru-bounded` and `lru-unbounded` are within
  noise of each other, so eviction accounting costs nothing on a read.
- **Exact LRU is not slower than approximate LRU on one thread** — it is
  marginally *faster*. `LruMode::Strict` takes the exclusive lock it needs
  anyway and then bumps plain counters, while `Approximate` serves reads under
  a *shared* lock and so must count with atomics. `Approximate` wins on
  concurrency, not on single-threaded cost, which is what `--lru-mode` selects
  between and why `approximate` is the default.
- **Sharding costs about 14 ns**, nearly all of it the `shared_mutex`
  acquire/release pair. That is the price of letting several reactors touch one
  keyspace, and it is why the shard count matters more than the hash does.
- **The clock is not free.** One `steady_clock::now()` costs half a sharded
  lookup here (15.6 ns against 31.3) and is a `QueryPerformanceCounter` on
  Windows, where it is dearer still — and `CacheEngine` needs one per command.
  Serving a value the reactor samples once per loop iteration removes it from
  the per-command path: the facade over bare sharded storage costs **0.7 ns**
  with `CachedClock` against **22.6 ns** with `SteadyClock`.

Reproduce with:

```sh
python bench/inproc_bench.py              # add --no-jitbit without a .NET SDK
```

## Read scaling across cores

A single-threaded ns/op figure says nothing about the axis a sharded cache
exists to win. The `[scaling]` tier measures aggregate read throughput over a
fixed window across N threads, 16 shards, keys spread over the whole set
(median of 25 repetitions):

| Threads | ops/s | vs 1 thread |
| ---: | ---: | ---: |
| 1 | 27.2M | 1.00× |
| 2 | 31.8M | 1.17× |
| 4 | 46.5M | 1.71× |
| 8 | 55.1M | 2.03× |
| 16 | 37.4M | 1.38× |

Scaling is sublinear and turns over past 8 threads. Two effects, neither of
them the shared lock on the read itself:

- With 16 shards and 16 threads, the chance that two threads want the same
  shard at once stops being small, and each sampled promotion takes that
  shard's lock exclusively for a moment. `--storage-shards` is the lever for a
  workload that lives here.
- The benchmark's threads are not pinned, so past the physical core count the
  scheduler starts co-locating them on SMT siblings and across both L3 domains.

The daemon does not stress this path the same way: each connection is pinned to
one reactor for its whole life, so the sharing is between reactors rather than
inside a request.

## What that buys over the wire

Storage nanoseconds become client throughput only when the server is the
bottleneck — and on a single host it usually is not. Driving one pinned reactor
with a pinned `redis-benchmark` (50 connections, pipeline depth 64, 4M `GET`s,
median of 7 interleaved repetitions per build) the daemon sits at **0.54 cores
busy**: the load generator saturates first, so ops/sec measures the client.

The metric that measures the *server* under those conditions is CPU time per
operation, taken from the daemon's own `/proc` counters:

| | Server CPU per `GET` | ops/sec (client-bound) |
| --- | ---: | ---: |
| Before the storage work | 1.88 µs | ~290k |
| After | 1.85 µs | ~290k |

About **30 ns of server CPU per request**, matching what the in-process
benchmark predicts — and roughly **2%** of the whole request path. The other
98% is the socket round trip, the protocol parse, and the reply write. That
ratio is the useful conclusion on this page: cache-layer nanoseconds are real,
they are bankable as headroom, and they are not where a slow deployment's time
goes.

Placement dominates everything else at this scale. An unpinned version of the
same experiment was bimodal — every run landed at either ~443k or ~363k ops/sec
regardless of build, depending on which L3 domain the daemon and the load
generator happened to share. Pin both before comparing anything.

## Against real servers

The wire suite runs fastcached, native `redis-server` and native `memcached`
through identical scenarios on one host. The headline lives in the
[README](https://github.com/LASTRADA-Software/fastcached#benchmarks): a tie at
one connection, ~3× redis and ~1.07× memcached at 16 connections, ~4.4× redis
and ~1.6× memcached at 64–256. The multi-core reactor architecture is what wins
there — not per-operation cost.

```sh
python bench/fastcached_bench.py --vs redis,memcached
```

## Where the time actually goes

For a cached `GET` served over TCP, in descending order:

1. **The socket round trip** — one `recvmsg` and one `sendmsg` per unpipelined
   request, plus the wakeup. Microseconds.
2. **Protocol parsing and the reply write** — framing, `ByteReader`,
   formatting.
3. **The cache operation itself** — ~32 ns of the ~1.9 µs above, and the only
   part the first table on this page can see.

So: pipeline where the client allows it, keep connections alive rather than
reconnecting, give `--threads` a value that matches the cores the daemon may
use, and raise `--storage-shards` before suspecting the map. Tuning below that
moves a number two orders of magnitude away from the one a client feels.

## Design decisions this measurement drove

Four properties of the current storage path exist because the numbers above
demanded them, and each is a constraint on future changes rather than an
accident:

- **The reactor owns the clock.** `CachedClock` wraps `SteadyClock` and is
  re-sampled by the event loop — once after the blocking wait returns, so
  resumed handlers see the instant the wait ended, and once before the next
  timeout is computed, so a batch's processing time does not overstate the
  sleep. Every command in between reads a stored value. Uptime and other
  out-of-loop readers deliberately keep the real clock, because a cached one
  freezes while the daemon is idle.
- **The shard index is a multiply-shift, not a modulo.** `%` compiles to a
  hardware divide on every storage operation. The reduction multiplies by a
  mixing constant first, because it consumes the *high* bits of `std::hash`,
  which no standard library promises to mix (MSVC's FNV-1a does not, and an
  unmixed fold put 2.08× the mean load on one shard). It is deliberately not a
  bit mask: `--storage-shards` accepts any count, and a mask would strand
  shards silently. With `--storage`, this function also decides which
  `shard-NN.cow` file a key lives in, so changing it repartitions an existing
  cache — one-time re-warm cost, and a reason not to change it casually.
- **A read never blocks for LRU bookkeeping.** Sampled promotion takes the
  exclusive lock with `try_lock` and skips on failure — `PromoteOnRead` is
  best-effort by contract, and blocking for it trades throughput for recency
  that is allowed to be approximate. This matters most on Windows, where
  `std::shared_mutex` is an SRWLOCK and a waiting writer blocks every
  subsequent reader on that shard, so one promotion in sixteen reads was enough
  to convoy all of them.
- **The read path bumps one counter, not two.** `cmd_get` is `get_hits +
  get_misses` by construction, so `Snapshot()` derives it; the two survivors sit
  on separate cache lines. Counting a third derivable number cost ~3.7 ns per
  lookup — enough that the shared-lock read path benchmarked slower than the
  exclusive-lock one.
