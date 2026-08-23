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

The scheduler is a **compile node**, not the cache. Pick one machine and give it
`--listen-scheduler`:

```sh
fastcache-compile-node     --listen-scheduler=0.0.0.0:6675     --fleet-member=worker-01.internal     --fleet-member=worker-02.internal     --scheduler=127.0.0.1:6675     --advertise=scheduler.internal:6676     --toolchain=/usr/bin/g++
```

It used to be `fastcached --listen-dispatch=...`, and that flag is **gone** rather
than deprecated. The two jobs have opposite deployment shapes: a cache is shared
infrastructure somebody operates, while handing out capacity is a decision only
**one** node may make at a time — and nothing in the cache daemon can establish
which node that is. So scheduling moved to where cluster leadership lives.

A scheduling verb arriving at a `fastcached` listener is refused with a typed
error naming where the scheduler went, so a client configured for the old layout
tells you what to fix instead of failing mysteriously.

Note that the scheduler also registers as a worker. Every node is a peer; being
the one that schedules is a role, not a different program. If you do not want it
taking work, give it a `--toolchain` nothing in your fleet compiles with.

#### Who may use the fleet

`--fleet-member` lists the hosts that may be scheduled onto it, and it is matched
by **host**: a peer dials from an ephemeral source port, so an endpoint is not
something a connection can be compared against.

`--fleet-open` admits everybody, for one machine or a network that is already
your boundary. One of the two is **required** — a scheduler with no member list
refuses every caller, which is the right default and not a working configuration,
so it is refused at startup rather than left to be discovered as a fleet that
silently distributes nothing.

Non-members are refused the *fleet*, never the *cache*: they read and write cached
objects exactly as before. What membership pays for is CPU time.

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
| `fastcached_dispatch_leases_granted_total` | Work is being distributed. |
| `fastcached_dispatch_leases_no_worker_total` | The fleet is **misconfigured** — workers are up but nobody matches. |
| `fastcached_dispatch_leases_no_capacity_total` | The fleet is **too small** — full of your own build. |
| `fastcached_dispatch_leases_withdrawn_total` | The fleet is **unavailable** — slots free on paper, machines busy elsewhere or out of scratch space. |
| `fastcached_dispatch_leases_duplicate_total` | Duplicate-work suppression is doing its job. Not a problem. |
| `fastcached_dispatch_worker_registrations_total` | Workers registering. A steady rise means heartbeats are not arriving. |

The first three refusals are different operator problems and are deliberately
counted apart: summing them hides a misconfiguration behind a busy fleet, and
hides an unavailable fleet behind one that merely looks undersized.

On each **worker**, start it with `--admin-listen` and the same endpoint reports
what that machine is doing:

| Counter | Rising means |
|---------|--------------|
| `fastcache_worker_jobs_started_total` | Jobs accepted. |
| `fastcache_worker_jobs_completed_total` | Jobs that ran to an exit code — including a **non-zero** one, which is the client's answer rather than a worker failure. |
| `fastcache_worker_compile_milliseconds_total` | Compile wall time. Divide by `..._jobs_completed_total` for the mean; both are counters, so a rate over a window gives you the current one. |
| `fastcache_worker_jobs_refused_no_slot_total` | This worker is **full**. Pair it with the scheduler's `..._no_capacity_total`. |
| `fastcache_worker_jobs_refused_unknown_fingerprint_total` | Somebody is dispatching a toolchain this worker does not have. |
| `fastcache_worker_jobs_refused_rejected_argument_total` | A command line carrying something that could name a file. |
| `fastcache_worker_jobs_refused_scratch_unavailable_total` | The scratch disk is full or unwritable. |
| `fastcache_worker_jobs_refused_spawn_failed_total` | The toolchain is configured but cannot be executed. |
| `fastcache_worker_bytes_received_total` / `..._returned_total` | Link volume, counted at the socket. |

The refusals are split by reason for the same reason the scheduler's two are:
a full worker and a misconfigured one are different problems with different
fixes, and one number covering both tells you neither.

The worker also reports what the machine **is** — `fastcache_node_logical_cores`,
`fastcache_node_memory_total_bytes`, `fastcache_node_disk_capacity_bytes`,
`fastcache_node_disk_free_bytes`, `fastcache_node_slots_configured` and
`fastcache_node_slots_busy`. Those are gauges: "is this node pulling its weight"
is not answerable without knowing how big it is.

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
- **`--slots`** defaults to *derived*: hardware threads, clamped by what the
  memory supports, less what `--node-class` reserves (two cores on a
  workstation, none on a dedicated node). A number you give it overrides all
  three. Whatever it resolves to is advertised *and* enforced locally from one
  calculation, so a worker cannot end up fuller and slower than the scheduler
  believes at the same moment. See
  [the worker's own page](../tools/fastcache-compile-node.md#capacity).
- **`--node-class` defaults to `workstation`,** which is the safe answer rather
  than the common one: a node nobody classified is somebody's desktop until
  proven otherwise. Set `--node-class dedicated` on build servers, or the fleet
  quietly runs two cores short on every one of them.
- **The scheduler picks by free slots, not by fewest running jobs.** Absolute
  counts make a 64-slot server running 8 jobs look busier than a 4-slot laptop
  running 2, which sends work to the smallest machines first.
- **Workers withdraw capacity while their machines are busy elsewhere.** Each
  heartbeat carries host CPU, available memory and free scratch space; the
  fleet's own jobs are subtracted, so what is left is load that belongs to
  somebody else. A worker whose scratch filesystem fills up stops being picked
  entirely, and starts again when it drains.
- **Three lease refusals, never summed.** `no-worker` means a fingerprint
  nobody serves, `no-capacity` means the fleet is too small, and `withdrawn`
  means the machines are there and unavailable. Each has its own
  `fastcached_dispatch_leases_*_total`, because the fixes are different and
  folding `withdrawn` into `no-capacity` sends an operator to buy hardware they
  already own.
- **Workers can come and go.** A worker that stops heartbeating is dropped after
  90 s; a client that leases one in the gap finds it unreachable and compiles
  locally.

## Security

Start the scheduler with `--requirepass` and give the same secret to workers
(`--requirepass`) and clients (`FASTCACHE_TOKEN`).

Even without it, a node is closed by default: its compile port, its scheduler and
its own cache tier all admit **this machine and `--fleet-member` peers only**. That
matters most for the compile port, which binds `0.0.0.0` because peers have to dial
it — anybody who could route to it would otherwise have your machine run their
compiler on source they chose. `--requirepass` is still worth setting: membership is
about who you are and a credential is about what you know, and a build LAN where
addresses can be spoofed wants both.

Keep `--listen-scheduler` off any network you would not run a compiler for, and
put mTLS in front of both ports for anything beyond a trusted build network.
`--fleet-member` is a policy about contribution rather than a credential: it stops
a machine that is not part of your fleet from spending its capacity, and it is not
a substitute for `--requirepass`.

## Limits worth knowing before you adopt it

- **Preprocessing does not distribute** (see step 1). The ceiling is ~10–40×.
- **`-g` embeds the worker's scratch path** in DWARF. Use
  `-fdebug-prefix-map` / `-ffile-prefix-map` if that matters.
- **Diagnostics from a failed remote compile are not shown.** A worker reporting
  a non-zero exit is retried locally and the local result is what you see —
  which also regenerates the diagnostics with correct line numbers.
- **`--install-service` exists on Linux only.** macOS and Windows workers run in
  the foreground or under a supervisor you provide.
- **On Windows a dispatched object is not byte-identical to a local one — the code
  in it is.** Every MSVC-family driver stamps the clock into the COFF header, and
  `cl` also records the absolute path of the object file (in `.debug$S`) and a hash
  of the source file it opened (in `.chks64`), with no debug flag asked for. A
  worker compiles its own scratch file, so those three differ; every section
  carrying code or data is byte-identical, which is what the end-to-end test
  asserts. If your build compares object bytes across machines, compare sections.
- **Some compiles are never distributed, by design.** A C++ **module interface
  unit** and any compile that writes a precompiled header produce a second artefact
  beside the object, and only the object travels — so those are compiled locally and
  are not cached either. So is a command line that names its input language itself
  (`/TP`, `-x c++`), because the launcher has to state the language of the
  preprocessed text it sends and would otherwise silently override yours. Run with
  `FASTCACHE_VERBOSE=1` to see which of these applied.

## Reference

- [fastcache-compile-node](../tools/fastcache-compile-node.md) — every flag.
- [Compile cache protocol](../protocols/compile-cache.md) — the wire format.
