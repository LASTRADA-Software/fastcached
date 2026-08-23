# fastcache-compile-node

A compile worker. It takes translation units that missed the cache and compiles
them, so a build is not limited to the cores of the machine running it.

It is **not** a cache and **not** a scheduler: it holds no keys, stores nothing,
and is given no cache credentials. The object it produces goes back to the client
that asked for it, and the *client* stores it.

## How the pieces fit

```
 fastcache-cc                 fastcached                  fastcache-compile-node
 ────────────                 ──────────                  ──────────────────────
                          :6674 cache                          :6676
                          :6675 dispatch  ◄──── register + heartbeat ────┘

 preprocess ─► key ─► FETCH ──hit──► done
        │
       miss
        ▼
   ask for a worker  ──────────►  match the toolchain exactly
   ◄── endpoint + lease token     pick the least-loaded one
        │                          (or refuse: compile locally)
        ├── send the preprocessed TU ──────────────────────────►
        ◄────────── object + diagnostics ───────────────────────
        │
        └─► write the object ─► STORE it (the client, not the worker)
```

**Every refusal ends in a local compile.** No matching toolchain, no free slot,
another client already compiling this key, an unreachable worker — all of them
fall back. Distribution cannot fail a build; that is what makes it safe to leave
switched on in a fleet where machines come and go.

## Quick start

Three processes. A cache:

```sh
fastcached --listen=0.0.0.0:6674
```

A scheduler, which is a compile node rather than the cache — handing out capacity
is a decision only one node may make at a time, and the cache cannot establish
which node that is:

```sh
fastcache-compile-node \
    --listen-scheduler=0.0.0.0:6675 --fleet-open \
    --scheduler=127.0.0.1:6675 \
    --advertise=scheduler.internal:6676 \
    --toolchain=/usr/bin/g++
```

One of `--fleet-member` or `--fleet-open` is required: a scheduler with no member
list refuses every caller, which is the right default and not a working
configuration.

A worker, on each machine that should take work:

```sh
fastcache-compile-node \
    --scheduler=build-cache.internal:6675 \
    --advertise=worker-01.internal:6676 \
    --toolchain=/usr/bin/g++
```

And the client, which is the launcher you already use:

```sh
export FASTCACHE_ADDR=build-cache.internal:6674
export FASTCACHE_SCHEDULER=build-cache.internal:6675
cmake -DCMAKE_CXX_COMPILER_LAUNCHER=fastcache-cc ...
```

Unset `FASTCACHE_SCHEDULER` and every miss compiles locally again, which is the
behaviour without this feature.

## `--advertise` is the flag to get right

The scheduler hands your string to clients **verbatim**. A worker that
advertises `127.0.0.1` is leased and then never answers, and the symptom is a
build that mysteriously falls back to local compiles on every machine but one.

It defaults to `--bind` and `--port`, which is correct only when those already
name an address other machines can reach.

## Toolchains

```sh
--toolchain=/usr/bin/g++          # this node computes the fingerprint
--toolchain=<fingerprint>=/usr/bin/g++   # or pin it explicitly
```

A job names a **fingerprint, never a program**. The worker maps that fingerprint
to a compiler from its own configuration and refuses one it does not have — which
is the difference between a build accelerator and a remote shell, and is why
there is deliberately no default compiler.

The fingerprint is a digest of the compiler's version banner **and its whole
include tree**, so two machines with the same compiler at different install
prefixes match, while two machines whose headers differ do not. Matching is
byte-identical and cannot be loosened: an over-strict match costs a local
compile, an over-loose one produces a silently wrong object that is then stored
under a key other machines fetch.

Computing it walks the include tree, which takes a few seconds the first time a
machine sees a toolchain and is cached afterwards.

### When no worker matches

The launcher reports `not dispatched (rejected (no-worker): ...)`. Ask both ends
what they think the fingerprint is:

```sh
fastcache-cc --print-toolchain-fingerprint /usr/bin/g++   # on the client
journalctl -u fastcache-compile-node | grep 'serving'     # on the worker
```

They must be identical. If they are not, the two machines really do have
different toolchains — different patch releases, different SDKs, a vendored
header that differs. `--print-toolchain-fingerprint` recomputes rather than
reading the cache, so it also repairs a stale entry on its way past.

## Running it as a service

### Linux

The package ships a socket-activated unit. Enable the **socket**, not the
service:

```sh
sudoedit /etc/fastcached/compile-node.env     # scheduler, advertise, toolchains
sudo systemctl enable --now fastcache-compile-node.socket
```

Socket activation means systemd owns the port: it answers from boot, so a client
that leases this worker never races its startup, and an idle worker costs
nothing — which suits a compile fleet, where misses on a warm shared cache are
bursty and rare.

The service runs as its own `fastcache-node` account, deliberately not
`fastcached`'s: a worker runs a compiler on input that arrived over the network,
while `fastcached` owns the cache storage, and sharing an account would let a
compromised compile rewrite every cached object.

`systemctl edit fastcache-compile-node` for local overrides; the shipped unit is
replaced on upgrade.

### macOS and Windows

```sh
fastcache-compile-node --install-service \
    --scheduler=cache.internal:6675 \
    --advertise=worker-01.internal:6676 \
    --toolchain=/usr/bin/c++ \
    --service-scope=user            # macOS: registers a launchd agent for you
```

Every other flag on that command line is **baked into the registration** and
reused at every start, so this is also where a wrong one is expensive. Three
things are therefore refused at install time rather than at the next boot:

| Missing | Why it is refused here |
|---|---|
| `--advertise` | Without it the registration bakes in `{--bind}:{--port}`, and the default `0.0.0.0` is not an address a client can dial. Such a worker registers, heartbeats, is leased out, and is never reached — with no error at either end. |
| `--scheduler` | The service would start and exit at every boot. |
| `--toolchain` | The worker would register and then refuse every job sent to it. |

`--requirepass` is refused too, for the reason it is on the daemon: a supervisor
records launch arguments where every local account can read them, and for a
worker that token is what the scheduler authenticates it *by*. Put it in a
config file the service account can read, or set it with a supervisor override.

**macOS scope.** `--service-scope=user` registers a LaunchAgent that runs as
you, which is the per-developer case and works today. `--service-scope=system`
registers a LaunchDaemon that must run as the unprivileged `fastcache-node`
account — the same one the Linux unit uses — and **is refused until that account
exists**, because a system job with no account named runs as *root*, and this
process compiles input that arrived over the network. Creating it is packaging
work that has not landed ([#87](https://github.com/LASTRADA-Software/fastcached/issues/87)).

**Windows** registers an SCM service (auto-start, left stopped; `sc start
FastCacheCompileNode`). The default service name is `FastCacheCompileNode`, not
the daemon's `FastCached`, so a machine can run both without one install
displacing the other.

Remove a registration with `--uninstall-service` (and the same
`--service-scope`, on macOS: which domain a job lives in is decided at install
time and re-probing would boot out one that was never there).

## Capacity

`--slots` bounds concurrent compiles (default: one per hardware thread). It is
advertised to the scheduler **and** enforced locally: a worker that accepted more
would be fuller and slower than the scheduler believes, at the same moment. A job
over the cap is refused rather than queued, because the client has a local
compile waiting either way and queueing only hides the overload from the
scheduler trying to route around it.

## Watching one

`--admin-listen` serves `/metrics` and `/healthz`, and is **off unless you ask
for it**: a scrape surface reachable from the network is a decision, not a
default. A bare port binds loopback, so `--admin-listen 6675` is reachable from
the machine and nowhere else; write `--admin-listen 0.0.0.0:6675` when you mean
the network.

```sh
fastcache-compile-node --scheduler cache.internal:6674 \
                       --toolchain /usr/bin/c++ \
                       --admin-listen 6675
curl -s localhost:6675/healthz     # 200 while the worker is answering
curl -s localhost:6675/metrics     # Prometheus exposition
```

It is the same endpoint and the same renderer the daemon serves — a worker has
no cache, so the cache series are **absent** rather than present and zero, which
a dashboard would otherwise read as an empty unbounded cache rather than as no
cache at all.

`/healthz` is worth wiring even if you never scrape: without it a supervisor can
tell that the process is alive but not that it is *answering*, which is the state
a wedged worker is in. It is what `systemd`'s and Kubernetes' probes want.

What the counters mean is tabulated under
[Distributed compilation](../getting-started/distributed-compilation.md#confirming-it-works).

## What a worker will not do

- **Run a program a client named.** The compiler comes from `--toolchain`.
- **Touch a path a client named.** The object path and the directory are the
  worker's own, inside a per-job scratch directory it creates and removes. A
  command line carrying anything that could name a file is refused outright, on
  both ends — the client's check protects an honest client from dispatching
  something that would not work, and the worker's protects it from a client that
  is not honest.

  The one thing a client does get to choose is what its translation unit is
  **called**, because a compiler records the name of the file it was handed and an
  object built under an invented name is gratuitously different from a locally
  built one. The name is reduced to a single component and an allow-listed shape
  before it becomes a path — no separators, no parent-directory segments, no
  drive letters, a bounded length, an extension from a fixed set, and never a
  Windows device name such as `CON` — and anything failing that is compiled as
  `tu.cpp` rather than refused. **The language never rides on it:** the client
  states the language explicitly (`-x c++-cpp-output`, `/TP`), so a name the
  worker had to invent cannot decide how the text is compiled.
- **Write to the cache.** Workers get no cache credentials.

## Security

The `0xFC` surface is authenticated: start the scheduler with `--requirepass` and
give workers and clients the same secret (`--requirepass` on the worker,
`FASTCACHE_TOKEN` on the client).

Keep `--listen-scheduler` off any network you would not run a compiler for. That
is why it is a separate process from the cache: the cache may reasonably be
reachable across a build LAN, while the surface that makes a compiler *run* on
another machine should be firewalled separately. A scheduling verb arriving at a
`fastcached` listener is refused with a typed reply naming where the scheduler
went.

`--fleet-member` restricts which hosts may spend the fleet's capacity. It is a
policy about contribution rather than a credential — a non-member still reads and
writes the cache — so it complements `--requirepass` and does not replace it.

For anything beyond a trusted build network, put mTLS in front of both ports.

## Known limitations

- **Preprocessing does not distribute.** The client must preprocess to compute
  the cache key before it knows there is a miss, so at roughly 45 ms against
  compiles of 300 ms–2 s the ceiling is about 10–40×, not linear.
- **`-g` embeds the worker's scratch path** in DWARF. Use
  `-fdebug-prefix-map`/`-ffile-prefix-map` if that matters to you.
- **Diagnostics from a failed remote compile are not shown.** A worker that
  reports a non-zero exit is retried locally and the *local* result is what you
  see, which also regenerates diagnostics with correct line numbers.
- **On MSVC a dispatched object is not byte-identical to a locally compiled
  one — the code in it is.** Measured on MSVC 14.51 and clang-cl, three things
  differ and no more:

    - every MSVC-family driver stamps the **clock** into the COFF header (two
      compiles of one file to one path two seconds apart differ in exactly byte
      4; `/Brepro` is what suppresses it);
    - `cl` records the **absolute path of the object file** in `.debug$S`, even
      without `/Zi`;
    - `cl` hashes the source file it opened into `.chks64`, and a worker opens
      its own scratch file.

    Everything carrying code or data is byte-identical: same compiler, same
    flags, same preprocessed input. What it affects is debugging, in the same way
    `-fdebug-prefix-map` addresses for GCC and clang — a debugger will need
    `/PDBALTPATH` or an equivalent source-path mapping to find your sources.

    So the Windows end-to-end fixture compares **section by section** against a
    per-driver table of what may differ, and clang-cl's table is *empty*: it
    records only the source's base name, which the worker is told, so its objects
    differ by the clock alone. The POSIX fixture asserts strict byte-identity and
    should — GCC and clang embed nothing path-dependent without `-g`. If your
    build compares object bytes across machines, compare sections.

## Not yet done

- The macOS package does not create the `fastcache-node` account, so
  `--install-service --service-scope=system` refuses there
  ([#87](https://github.com/LASTRADA-Software/fastcached/issues/87)).
  `--service-scope=user` works, and is the right answer on a developer machine
  anyway.
- Host **CPU and memory utilization** are not reported. Size, slots and disk are;
  what the machine is doing *right now* is what resource-aware scheduling will
  need, and it lands with the scheduling policy that consumes it rather than
  ahead of it.
