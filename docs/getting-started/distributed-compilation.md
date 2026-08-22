# Distributed compilation

A cache makes the *second* build of a commit fast. It can do nothing about the
first one: on a miss, `fastcache-cc` runs the real compiler locally and stores
the result.

Distributed compilation absorbs those misses. Machines that would otherwise sit
idle register as workers, and a client that misses the cache hands the
translation unit to one of them instead of compiling it itself.

It is **opt-in at both ends** and cannot break a build: every refusal, every
unreachable worker, every mismatch falls back to the local compile that would
have happened anyway.

---

## Architecture

Three roles. Two of them are programs you already have.

| Role | Program | What it does |
|------|---------|--------------|
| **Client** | `fastcache-cc` | Fronts each compile. Checks the cache, and on a miss asks for a worker. |
| **Scheduler** | `fastcached` | The cache daemon, with a second listener. Tracks workers and hands one out. |
| **Worker** | `fastcache-compile-node` | Compiles what it is sent. Holds no cache. |

```
   ┌──────────────┐                     ┌──────────────────────┐
   │ fastcache-cc │                     │ fastcached           │
   │  (client)    │                     │  (cache + scheduler) │
   └──────┬───────┘                     └───────┬──────────────┘
          │                                     │
          │  1. FETCH ─────────────────────────►│ :6674  cache
          │     ◄──────────── hit: done         │
          │                                     │
          │  2. miss: "give me a worker" ──────►│ :6675  dispatch
          │     ◄──────── endpoint + lease      │
          │                                     ▲
          │                                     │ register + heartbeat
          │                        ┌────────────┴─────────┐
          │  3. compile this ─────►│ fastcache-compile-node│ :6676
          │     ◄──── object       │      (worker)         │
          │                        └───────────────────────┘
          │
          └─ 4. STORE the object ─►  :6674   ← the CLIENT stores, not the worker
```

### Why the scheduler is the cache daemon

Not for convenience. Because it is both, it knows something neither distcc nor
sccache-dist can know: **which keys are already being compiled right now.**

When a header changes and sixty parallel clients miss the same key — the
ordinary shape of a miss on a shared cache, not an exotic one — the scheduler
dispatches *one* job and tells the other fifty-nine to compile locally. Without
that, sixty machines compile the same translation unit and fifty-nine of the
results are thrown away.

### Why the client stores the result, never the worker

A `STORE` is trusted today because whoever stored it compiled it themselves —
they could only poison their own key space with something they would have gotten
anyway. If workers stored, one rogue worker could poison the shared cache for
everyone.

Routing the result back through the client keeps that model exactly as it was.
Workers are given **no cache credentials at all**.

---

## How it works

### 1. The client preprocesses, and that is the part that cannot be distributed

`fastcache-cc` must preprocess to compute the cache key *before* it can know
whether there is a miss. That work is unavoidable and always local — roughly
45 ms against compiles of 300 ms–2 s, which is where the ceiling of about
10–40× comes from. Distribution is not linear scaling and should not be sized as
if it were.

### 2. On a miss, the client asks for a worker

The request names a **toolchain fingerprint** and the object key. The scheduler
matches the fingerprint **byte-identically** and picks the matching worker with
the fewest jobs outstanding.

Least-outstanding rather than round-robin, because compile times within one
build vary by an order of magnitude: distributing *arrivals* rather than *load*
queues a forty-second translation unit behind another one while a worker idles.

The scheduler refuses rather than queues — `no-worker`, `no-capacity`,
`already-in-flight` — because the client is holding the source and can simply
compile it. Queueing would buy latency and nothing else.

### 3. The client sends preprocessed text, not files

The worker receives the translation unit already preprocessed, so there are no
headers to ship and no sysroot to replicate. This is distcc's non-pump model,
i.e. the one that works.

One subtlety with real consequences: the text sent to a worker is **not** the
text the cache key hashed. The key's copy has `#line` markers suppressed so no
checkout path can reach the key; a compiler needs those markers to know which
lines came from a system header. Without them every warning inside libc++ or the
CRT is re-reported against your own file, and under `-Werror` that is a failed
compile. So the client preprocesses a second time for dispatch.

### 4. The worker compiles it — with a compiler *it* chose

The job names a fingerprint, **never a program**. The worker maps that
fingerprint to a compiler from its own `--toolchain` configuration and refuses
one it does not have.

This is the difference between a build accelerator and a remote shell, and it is
why there is deliberately no default compiler. The worker also re-checks the
argument list, because the client's check protects an honest client from
dispatching something that would not work, while the worker's protects it from a
client that is not honest.

### 5. The client writes the object, reproduces the dependency record, and stores

The worker compiled preprocessed text, which has no `#include` left in it, so it
reports no dependencies. The client already knows them — its own preprocess pass
opened every one — so it writes the depfile (or `/showIncludes` notes) itself.
Skipping that would leave the build with no header dependencies for that
translation unit, so it would stop rebuilding when those headers change.

A dispatched compile is then shaped to look exactly like a local one, so the
STORE, the manifest and the statistics all take one path.

---

## Setting it up

### The scheduler

An ordinary `fastcached` with a second listener:

```sh
fastcached --listen=0.0.0.0:6674 --listen-dispatch=0.0.0.0:6675
```

Or in `/etc/fastcached/fastcached.yaml`:

```yaml
listeners:
  - address: 0.0.0.0
    port: 6674
  - address: 0.0.0.0
    port: 6675
    roles: [dispatch]
```

The dispatch endpoint is deliberately separate. The cache may reasonably be
reachable across a build LAN; the surface that makes a compiler **run** on
another machine should be something you switch on and firewall on purpose. A
dispatch request arriving on a cache-only listener is refused with a typed
error, not served.

### The workers

On each machine that should take work:

```sh
fastcache-compile-node \
    --scheduler=build-cache.internal:6675 \
    --advertise=worker-01.internal:6676 \
    --toolchain=/usr/bin/g++ \
    --toolchain=/usr/bin/clang++
```

On Linux the package ships a socket-activated unit; put the arguments in
`/etc/fastcached/compile-node.env` and enable the **socket**:

```sh
sudo systemctl enable --now fastcache-compile-node.socket
```

!!! warning "`--advertise` is the flag to get wrong"

    The scheduler hands your string to clients verbatim. A worker advertising
    `127.0.0.1` is leased and then never answers — and the symptom is a build
    that falls back to local compiles on every machine except the one running
    the worker.

### The clients

```sh
export FASTCACHE_ADDR=build-cache.internal:6674
export FASTCACHE_SCHEDULER=build-cache.internal:6675
cmake -DCMAKE_CXX_COMPILER_LAUNCHER=fastcache-cc ...
```

Unset `FASTCACHE_SCHEDULER` and every miss compiles locally again — the
behaviour without this feature, and the way to turn it off for one build.

---

## Confirming it works

With `FASTCACHE_VERBOSE=1` the launcher says what happened to each translation
unit:

```
fastcache-cc: HIT key=…                                    served from cache
fastcache-cc: DISPATCHED to worker-01.internal:6676 key=…  compiled remotely
fastcache-cc: not dispatched (rejected (no-worker): …)      compiled locally
```

On the scheduler, the `/metrics` endpoint counts the outcomes:

| Counter | Rising means |
|---------|--------------|
| `dispatch_leases_granted` | Work is being distributed. |
| `dispatch_leases_no_worker` | The fleet is **misconfigured** — workers are up but nobody matches. |
| `dispatch_leases_no_capacity` | The fleet is **too small**. |
| `dispatch_leases_duplicate` | Duplicate-work suppression is doing its job. Not a problem. |
| `dispatch_worker_registrations` | Workers registering. A steady rise means heartbeats are not arriving. |

The first two are different operator problems and are deliberately counted
apart: summing them hides a misconfiguration behind a busy fleet.

### "No worker matches this toolchain"

The commonest setup failure. A worker is matched only if its toolchain
fingerprint is byte-identical to the client's, so ask both ends:

```sh
fastcache-cc --print-toolchain-fingerprint /usr/bin/g++    # on a client
journalctl -u fastcache-compile-node | grep serving        # on a worker
```

If they differ, the machines really do have different toolchains — different
patch releases, different SDKs, a vendored header that is not the same.

The fingerprint is a digest of the compiler's version banner **and its whole
include tree**. That is what lets two machines with the same toolchain at
different install prefixes match, while two machines whose headers differ do
not. It cannot be loosened: an over-strict match costs one local compile, an
over-loose one produces a silently wrong object that is then stored under a key
every other machine fetches.

---

## Sizing and operations

- **Misses are bursty.** With a warm shared cache, hit rates run 90 %+, so a
  fleet is idle most of the time and the value is concentrated in the first
  build of a commit, developer branches and toolchain bumps. Socket activation
  exists for exactly this shape.
- **`--slots`** defaults to one per hardware thread. It is advertised *and*
  enforced locally, so a worker cannot end up fuller and slower than the
  scheduler believes at the same moment.
- **Workers can come and go.** A worker that stops heartbeating is dropped after
  90 s; a client that leases one in the gap finds it unreachable and compiles
  locally.

## Security

Start the scheduler with `--requirepass` and give the same secret to workers
(`--requirepass`) and clients (`FASTCACHE_TOKEN`). Without it, anything that can
reach the dispatch port can queue work onto your fleet.

Keep `--listen-dispatch` off any network you would not run a compiler for, and
put mTLS in front of both ports for anything beyond a trusted build network.

## Limits worth knowing before you adopt it

- **Preprocessing does not distribute** (see step 1). The ceiling is ~10–40×.
- **`-g` embeds the worker's scratch path** in DWARF. Use
  `-fdebug-prefix-map` / `-ffile-prefix-map` if that matters.
- **Diagnostics from a failed remote compile are not shown.** A worker reporting
  a non-zero exit is retried locally and the local result is what you see —
  which also regenerates the diagnostics with correct line numbers.
- **`--install-service` exists on Linux only.** macOS and Windows workers run in
  the foreground or under a supervisor you provide.

## Reference

- [fastcache-compile-node](../tools/fastcache-compile-node.md) — every flag.
- [Compile cache protocol](../protocols/compile-cache.md) — the wire format.
