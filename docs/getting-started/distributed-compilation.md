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

This page is how to set it up and run it. If you want the model first — what the
pieces are and how one compile flows through them — read
[How it works](../how-it-works.md), which is a shorter read and makes everything
below easier to place.

---

## Architecture

Three roles across two programs, with the shared cache behind them.
**Scheduler and worker are the same binary** — `fastcache-compile-node` —
because being the one that hands out capacity is a *role* a node takes, not a
different program.

| Role | Program | What it does |
|------|---------|--------------|
| **Client** | `fastcache-cc` | Fronts each compile. Checks the cache, and on a miss asks for a worker. |
| **Scheduler** | `fastcache-compile-node --listen-scheduler` | Tracks the fleet's workers and hands one out. Exactly one node at a time. |
| **Worker** | `fastcache-compile-node` | Compiles what it is sent. Also holds a cache tier of its own, unless you turn it off. |
| **Shared cache** | `fastcached` | Optional, and *not* the scheduler. Where objects end up so other machines get them. |

```
   ┌──────────────┐          ┌───────────────────────────────┐
   │ fastcache-cc │          │ fastcache-compile-node        │
   │  (client)    │          │  (this one leads: scheduler)  │
   └──────┬───────┘          └───────────────┬───────────────┘
          │                                  │
          │  1. FETCH ──────────────────────►│ :6674  its cache tier,
          │     ◄──────────── hit: done      │        reading through to
          │                                  │        fastcached upstream
          │  2. miss: "give me a worker" ───►│ :6675  scheduler
          │     ◄──────── endpoint + lease   │
          │                                  ▲
          │                                  │ register + heartbeat
          │                   ┌──────────────┴─────────┐
          │  3. compile this ►│ fastcache-compile-node │ :6676
          │     ◄──── object  │      (a worker)        │
          │                   └────────────────────────┘
          │
          └─ 4. STORE the object ─► :6674  ← the CLIENT stores, not the worker
```

`fastcached` is behind all of it as the fleet-wide store a node's `--upstream`
points at. It serves the cache verbs and nothing else: a scheduling verb arriving
at one of its listeners is refused with a typed `dispatch-not-permitted` naming
where the scheduler went.

### Why the scheduler knows what is already being compiled

A `LEASE` request names the **object key**, not just the toolchain — so the one
node handing out capacity sees the whole fleet's misses as they arrive, which is
something neither distcc nor sccache-dist is positioned to know.

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
the most **free slots** — ties broken by utilization, so between two machines
with four slots free the one with proportionally more of itself left takes the
job.

Free slots rather than fewest running jobs, because absolute counts treat every
machine as an identical box: a 64-slot server running 8 jobs looks busier than a
4-slot laptop running 2, when the server has 56 slots free and the laptop has
none. Across a fleet of mixed machines — the ordinary case — counting jobs sends
work to the smallest machines first and leaves the big ones idle.

The scheduler refuses rather than queues — `no-worker`, `no-capacity`,
`withdrawn`, `already-in-flight` — because the client is holding the source and
can simply compile it. Queueing would buy latency and nothing else. Each names a
different operator problem, which is why they are counted apart; see
[the sizing notes](#sizing-and-operations).

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
fingerprint to a compiler it serves and refuses one it does not have.

Which compilers those are is a fact the worker establishes for itself: it surveys
the machine at startup, so a package install is the whole setup. `--toolchain`
narrows that set rather than supplying it.

This is the difference between a build accelerator and a remote shell, and it is
why there is still deliberately no default *compiler*. "No default" and "no
discovery" are different claims — a default is how a job ends up running
against something nobody chose, while which compilers a machine holds is simply
a fact.

The worker also re-checks the argument list, because the client's check protects
an honest client from dispatching something that would not work, while the
worker's protects it from a client that is not honest.

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
fastcache-compile-node \
    --listen-scheduler=0.0.0.0:6675 \
    --fleet-member=worker-01.internal \
    --fleet-member=worker-02.internal \
    --scheduler=127.0.0.1:6675 \
    --advertise=scheduler.internal:6676
```

It used to be `fastcached --listen-dispatch=...`, and that flag is **gone** rather
than deprecated. The two jobs have opposite deployment shapes: a cache is shared
infrastructure somebody operates, while handing out capacity is a decision only
**one** node may make at a time — and nothing in the cache daemon can establish
which node that is. So scheduling moved to where cluster leadership lives.

A scheduling verb arriving at a `fastcached` listener is refused with a typed
error naming where the scheduler went, so a client configured for the old layout
tells you what to fix instead of failing mysteriously.

No `--toolchain` is needed: the node surveys the machine at startup and serves
what it finds. Note that the scheduler therefore also registers as a worker —
every node is a peer, and being the one that schedules is a role rather than a
different program. There is no configuration that offers **zero** slots
([#206](https://github.com/LASTRADA-Software/fastcached/issues/206)); if you want
this machine kept out of the work, give it an identity nothing dispatches to:

```sh
--no-toolchain-discovery --toolchain=scheduler-only=/nonexistent
```

An operator-pinned `<fingerprint>=<compiler>` is deliberately not probed, so that
registers a toolchain no client can match. One machine leading a fleet of many is
the only shape this is worth doing for.

Once several nodes run, exactly one of them must schedule at a time, which is
what consensus decides — `--node-id`, `--listen-raft` and `--raft-peer`. See
[a cluster, and who leads it](../tools/fastcache-compile-node.md#a-cluster-and-who-leads-it).

#### Who may use the fleet

`--fleet-member` lists the hosts that may be scheduled onto it, and it is matched
by **host**: a peer dials from an ephemeral source port, so an endpoint is not
something a connection can be compared against.

`--fleet-open` admits everybody, for one machine or a network that is already
your boundary. One of the two is **required** — a scheduler with no member list
refuses every caller, which is the right default and not a working configuration,
so it is refused at startup rather than left to be discovered as a fleet that
silently distributes nothing.

Membership gates **all three** of a node's surfaces — its scheduler, its compile
port, and its own cache tier — and this machine is always a member of its own
fleet whatever the list says. What it does not gate is the shared `fastcached`: a
non-member reads and writes objects there exactly as before, because that is
infrastructure somebody operates while a node's tier is a developer's own build
output. What membership pays for on a node is CPU time.

### The workers

On each machine that should take work:

```sh
fastcache-compile-node \
    --scheduler=build-cache.internal:6675 \
    --advertise=worker-01.internal:6676
```

That is the whole of it. The worker surveys the machine at startup and serves
every compiler it finds, naming each one and the layout it came from:

```
[INFO] found /usr/bin/g++ (usr)
[INFO] found /usr/bin/clang++ (usr)
[INFO] discovered 2 toolchain(s) on this machine; pass --toolchain to serve a narrower set
```

Add `--toolchain=<compiler>` to serve a **narrower** set — a build farm pinned to
a curated toolchain — or `--no-toolchain-discovery` to stop the survey entirely.
Naming any `--toolchain` pins the worker to exactly those.

A node started this way is **also a cache**, which is the part that surprises
people: `--listen-cache` defaults to `127.0.0.1:6674` and `--cache-memory` to a
quarter of host RAM within `[512m, 8g]`, so the machine gets a local tier without
asking for one. Point it at the shared cache with `--upstream` and a local rebuild
stops reaching the wire at all. `--cache-memory=0` with no `--cache-dir` turns the
tier off, which is what a machine that only compiles for others wants. The whole
of it is on
[the node's own page](../tools/fastcache-compile-node.md#a-cache-of-its-own).

On Linux the package ships a socket-activated unit; put the arguments in
`FASTCACHE_NODE_ARGS` in `/etc/fastcached/compile-node.env` and enable the
**socket**:

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
export FASTCACHE_SCHEDULER=scheduler.internal:6675
cmake -DCMAKE_CXX_COMPILER_LAUNCHER=fastcache-cc ...
```

`FASTCACHE_ADDR` is the **cache** — a `fastcached`, or the local node's own
`--listen-cache`. It defaults to `127.0.0.1:6674` when unset, which is where a
node on this machine already answers, so a developer running a node needs only
the scheduler line. `FASTCACHE_ADDR=` (set but empty) is the opt-out.

`FASTCACHE_SCHEDULER` is the **scheduler**, which is some node's
`--listen-scheduler`, never the cache port. Unset it and every miss compiles
locally again — the behaviour without this feature, and the way to turn it off
for one build.

!!! danger "Do not set `FASTCACHE_TOKEN` on a client that dispatches"

    A node's scheduler, compile and cache ports serve no `AUTH` verb, so a
    credential presented to them is refused `dispatch-not-permitted` — and the
    launcher surfaces that in place of the answer to the request it actually sent.
    Every `LEASE` is then declined and every compile happens locally, with a green
    build and nothing but a `FASTCACHE_VERBOSE` line to say so. The same applies
    to `--requirepass` on a worker: its `REGISTER` is refused and it never joins
    the fleet. Tracked as
    [#198](https://github.com/LASTRADA-Software/fastcached/issues/198); see
    [Security](#security) for what does protect a fleet today.

---

## Confirming it works

With `FASTCACHE_VERBOSE=1` the launcher says what happened to each translation
unit:

```
fastcache-cc: HIT key=…                                          served from cache
fastcache-cc: DISPATCHED to worker-01.internal:6676 key=…        compiled remotely
fastcache-cc: not dispatched (rejected (no-worker)); compiling locally
```

The scheduler is a compile node, so its `/metrics` endpoint is the node's:
start it with `--admin-listen` and these count the outcomes.

| Counter | Rising means |
|---------|--------------|
| `fastcached_dispatch_leases_granted_total` | Work is being distributed. |
| `fastcached_dispatch_leases_no_worker_total` | The fleet is **misconfigured** — workers are up but nobody matches. |
| `fastcached_dispatch_leases_no_capacity_total` | The fleet is **too small** — full of your own build. |
| `fastcached_dispatch_leases_withdrawn_total` | The fleet is **unavailable** — slots free on paper, machines busy elsewhere or out of scratch space. |
| `fastcached_dispatch_leases_duplicate_total` | Duplicate-work suppression is doing its job. Not a problem. |
| `fastcached_dispatch_worker_registrations_total` | Workers registering. A steady rise means heartbeats are not arriving. |
| `fastcached_dispatch_worker_registrations_malformed_total` | A peer named its toolchain, endpoint or version in bytes that are not UTF-8 and was refused. Any rise names a machine that is **not** in the fleet. |

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
| `fastcache_worker_jobs_refused_not_a_member_total` | Something with no claim on this machine tried to compile on it. Not a fault of yours; check who can reach `--port`. |
| `fastcache_worker_bytes_received_total` / `..._returned_total` | Link volume, counted at the socket. |

The refusals are split by reason for the same reason the scheduler's two are:
a full worker and a misconfigured one are different problems with different
fixes, and one number covering both tells you neither.

The worker also reports what the machine **is** — `fastcache_node_logical_cores`,
`fastcache_node_memory_total_bytes`, `fastcache_node_disk_capacity_bytes`,
`fastcache_node_disk_free_bytes`, `fastcache_node_slots_configured` and
`fastcache_node_slots_busy`. Those are gauges: "is this node pulling its weight"
is not answerable without knowing how big it is.

And what its **cache tier** is doing — `fastcache_node_cache_hits_total` and
`..._misses_total` for the tier itself, `..._upstream_hits_total` for what the
shared cache answered after a local miss, and `..._fill_failures_total`,
`..._store_failures_total`, `..._upstream_stores_total` and
`..._upstream_store_failures_total` for the writes. A high upstream-hit rate
against a low local one means the tier is too small for this machine's working
set, which is a different problem from a fleet that is missing a lot.

Beside them, what it is **holding** — `fastcached_items`, `fastcached_bytes_used`
and `fastcached_bytes_limit` for the cache as a whole, plus a
`fastcached_tier_*{tier="memory"|"disk"}` set for the split. A node whose cache is
effectively empty sends every rebuild to the wire; one whose evictions climb while
its hit rate falls has a budget too small for the tree it builds. Both are
invisible from the scheduler's own counters, which only see the work that reached
it. See [the node's own page](../tools/fastcache-compile-node.md#reading-it) for
why these must not be summed across tiers, and why a node running no tier reports
the series as **absent** rather than as zero.

The same figures reach the **leader**, on each node's REGISTER and HEARTBEAT, so
they are readable from one place rather than by scraping every machine. Note that
a node started with two `--toolchain` flags is two registry entries against one
machine and one cache: both carry the same figures, and anything summing them
counts that cache twice.

### The whole fleet on one page

`--dashboard`, on a leader that already has `--admin-listen` and
`--listen-scheduler`, serves `/fleet` and `/fleet.json` — every member's
hostname, endpoint, software version, capacity and cache, plus charts of the last
24 hours or 7 days:

```sh
fastcache-compile-node ... \
    --listen-scheduler=6675 --fleet-member=10.0.0.2 \
    --admin-listen=6677 \
    --dashboard --dashboard-token-file=/etc/fastcached/dashboard.token
```

Only the **leader** answers it in full; anyone else replies `503` naming the
leader, because a follower's registry holds whatever registered against it rather
than the fleet. The credential is required off loopback and is deliberately its
own file rather than `--requirepass`. `/metrics` and `/healthz` stay outside it,
so a scraper or a probe is unaffected — and `/metrics` remains the source of truth
for anything you alert on. The full account, including what each column means and
why a value nobody reported renders as `–` rather than `0`, is under
[looking at the whole fleet](../tools/fastcache-compile-node.md#looking-at-the-whole-fleet).

### "No worker matches this toolchain"

The commonest setup failure. A worker is matched only if its toolchain
fingerprint is byte-identical to the client's, so ask both ends:

```sh
fastcache-cc --print-toolchain-fingerprint /usr/bin/g++    # on a client
journalctl -u fastcache-compile-node | grep serving        # on a worker
```

If they differ, the machines really do have different toolchains — different
patch releases, different SDKs, a vendored header that is not the same.

The fingerprint is a digest of the compiler's version banner **and the include
tree that belongs to it**. That is what lets two machines with the same
toolchain at different install prefixes match, while two machines whose headers
differ do not. It cannot be loosened: an over-strict match costs one local
compile, an over-loose one produces a silently wrong object that is then stored
under a key every other machine fetches.

*Belongs to it* is the operative phrase on Windows. `cl` carries the
`VC\Tools\MSVC\<version>` toolset it lives inside and the newest Windows SDK;
`clang-cl` carries only the resource directory it names when asked
(`-print-resource-dir`), because it borrows the VC toolset and the SDK rather
than owning them — so two clang-cl machines with different SDKs installed still
match. Neither derives its answer
from `INCLUDE`, which is set per developer command prompt and never inherited by
a service — so a worker installed as a service matches the launchers that talk
to it.

### The fingerprint and the cache key are not the same string

They answer different questions, so one of them carries the compiler's **target**
and the other deliberately does not:

| | Decides | Carries the target? |
|---|---|---|
| **Toolchain fingerprint** | which *worker* may serve a client | no |
| **Object cache key** | which *object* may be served | yes |

A driver's code generation is not a function of the driver alone. `clang-cl`
detects the MSVC installation beside it and sets `-fms-compatibility-version` from
that, and clang's Microsoft C++ ABI gates version-specific code generation on the
value — so the same `clang-cl.exe` in a developer prompt and under a service
generates differently. Stock `g++` on x86_64 and aarch64 is the same shape with no
MSVC anywhere near it: one `--version` banner, two code generators.

The **key** therefore folds the target in, so those cases stop sharing entries.
The **fingerprint** must not, or a developer-prompt launcher would stop matching a
service-run worker — the mismatch that keeps a fleet answering `no-worker` for
reasons nobody can see. Dispatch closes the gap on the line instead: a dispatched
compile states `--target=<triple>` **ahead** of the build's own arguments, so the
worker generates for the client's target rather than re-deriving one from its own
machine, while a `--target=` or `-m32` the build states itself still wins.

Only a driver that can be *told* a target is pinned this way. `gcc` is
fixed-target and rejects the flag, so its target is identified for the key and
never stated on a dispatched line; `cl` is neither, because which code generator
runs is decided by which `cl.exe` you invoked and no command line can restate it.

The practical consequence is a hit-rate one: after this change two machines whose
compilers report one banner but generate for different targets no longer share
cache entries. That is a correction — they were sharing objects they should not
have — but it looks like a cold cache the first time a fleet upgrades past it.

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
- **A node's own cache tier is subtracted from what it can compile.** Capacity is
  one job per gigabyte of RAM, and a resident cache is memory that will not yield —
  so a 64-thread host with 32 GiB holding 8 GiB of cache offers 24 slots, not 32.
  What is subtracted is what the tier **actually holds**: a node with
  `--cache-memory=0` and no `--cache-dir` reserves nothing and offers the whole
  machine, and so does one whose cache never started.
- **Workers can come and go.** A worker heartbeats every 20 s and is dropped after
  90 s without one; a client that leases one in the gap finds it unreachable and
  compiles locally.

## Security

**Membership is what protects a fleet today, and it is the only thing that does.**
A node is closed by default: its compile port, its scheduler and its own cache tier
all admit **this machine and `--fleet-member` peers only** (or every caller, once
you say `--fleet-open`). Once a cluster exists, the agreed member set replaces that
list and all three surfaces follow it. That matters most for the compile port,
which binds `0.0.0.0` because peers have to dial it — anybody who could route to it
would otherwise have your machine run their compiler on source they chose.

A node has **no inbound credential**. Its three framed surfaces serve no `AUTH`
verb, so `--requirepass` on a node is only the secret it *presents* when it dials
somebody else — and it works in exactly one direction: against a `fastcached`
`--upstream`, which does serve `AUTH`. Presented to another node it is refused
`dispatch-not-permitted`, and because the launcher reports that in place of the
answer to the request it actually sent, the effect is silent:

| You set | What breaks |
|---|---|
| `--requirepass` on a worker | `REGISTER` is refused; the worker never joins the fleet. |
| `FASTCACHE_TOKEN` on a client with `FASTCACHE_SCHEDULER` set | Every `LEASE` is declined; every compile happens locally, and the build goes green. |
| `--requirepass` with `--cluster-status` and friends | Refused, naming a verb the operator never typed. |

That is [#198](https://github.com/LASTRADA-Software/fastcached/issues/198), and it
is a gap rather than a design: what an inbound credential should be spelled, and
what `AUTH` means against a node that has none configured, are the questions it is
open on. Until it closes, treat the fleet's boundary as **network reachability
plus membership**, and size the network accordingly.

So: keep `--listen-scheduler` off any network you would not run a compiler for,
and put mTLS in front of every port for anything beyond a trusted build network.
The two remaining credentials in this system are real and unaffected —
`--dashboard-token-file` guards the fleet page, and `fastcached`'s own
`--requirepass` guards the shared cache.

Also worth knowing: the node's inter-node gate matches on the peer's **source
address** alone ([#180](https://github.com/LASTRADA-Software/fastcached/issues/180)),
so a build LAN where addresses can be spoofed is not a boundary this can hold.

## Limits worth knowing before you adopt it

- **Preprocessing does not distribute** (see step 1). The ceiling is ~10–40×.
- **`-g` embeds the worker's scratch path** in DWARF. Use
  `-fdebug-prefix-map` / `-ffile-prefix-map` if that matters.
- **Diagnostics from a failed remote compile are not shown.** A worker reporting
  a non-zero exit is retried locally and the local result is what you see —
  which also regenerates the diagnostics with correct line numbers.
- **`--install-service` is Windows and macOS.** It registers an SCM service or a
  launchd job. On Linux the packages ship a socket-activated systemd unit instead,
  so there is nothing for the flag to do and it reports as much. A worker that
  needs `--requirepass` cannot be registered on any platform — a supervisor records
  launch arguments where every local account can read them — so on Linux that token
  goes in `FASTCACHE_NODE_ARGS`, and elsewhere such a worker runs in the foreground.
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

- **A node's `--requirepass` cannot reach another node** (see
  [Security](#security)). Until
  [#198](https://github.com/LASTRADA-Software/fastcached/issues/198) closes, the
  fleet's own traffic is unauthenticated and setting a token on it is worse than
  leaving it unset.

## Reference

- [fastcache-compile-node](../tools/fastcache-compile-node.md) — every flag, the
  node's cache tier, the cluster, and the fleet dashboard.
- [Cluster discovery](cluster-discovery.md) — how nodes find each other, and the
  pre-shared key that admits them.
- [Compile cache protocol](../protocols/compile-cache.md) — the wire format.
