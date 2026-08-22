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

Three processes. The scheduler is an ordinary `fastcached` with a second
listener:

```sh
fastcached --listen=0.0.0.0:6674 --listen-dispatch=0.0.0.0:6675
```

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

Run it in the foreground, or under whatever supervisor you already use. There is
no `--install-service` yet on either platform — see *Not yet done* below.

## Capacity

`--slots` bounds concurrent compiles (default: one per hardware thread). It is
advertised to the scheduler **and** enforced locally: a worker that accepted more
would be fuller and slower than the scheduler believes, at the same moment. A job
over the cap is refused rather than queued, because the client has a local
compile waiting either way and queueing only hides the overload from the
scheduler trying to route around it.

## What a worker will not do

- **Run a program a client named.** The compiler comes from `--toolchain`.
- **Touch a path a client named.** The object path, the source path and the
  working directory are all the worker's, inside a per-job scratch directory it
  creates and removes. A command line carrying anything that could name a file is
  refused outright, on both ends — the client's check protects an honest client
  from dispatching something that would not work, and the worker's protects it
  from a client that is not honest.
- **Write to the cache.** Workers get no cache credentials.

## Security

The `0xFC` surface is authenticated: start the scheduler with `--requirepass` and
give workers and clients the same secret (`--requirepass` on the worker,
`FASTCACHE_TOKEN` on the client).

Keep `--listen-dispatch` off any network you would not run a compiler for. That
is why it is a separate listener from the cache: the cache may reasonably be
reachable across a build LAN, while the surface that makes a compiler *run* on
another machine should be firewalled separately. A dispatch verb arriving on a
cache-only listener is refused with a typed reply.

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
  one.** `cl` embeds the source path in the object even without `/Zi`, and a
  worker compiles from its own scratch directory — so the object carries that
  path instead of yours. The code is the same: same compiler, same flags, same
  preprocessed input. What it affects is debugging, in the same way
  `-fdebug-prefix-map` addresses for GCC and clang. A debugger will need
  `/PDBALTPATH` or an equivalent source-path mapping to find your sources.

    This is why the Windows end-to-end fixture asserts that a dispatched object
    is a *plausible* compile — right compiler, right flags, sane size — while the
    POSIX one asserts strict byte-identity. GCC and clang embed nothing
    path-dependent without `-g`; `cl` does, and no tolerance can make a byte
    comparison meaningful across two different source paths.

## Not yet done

- `--install-service` on macOS and Windows. The Linux units are shipped; the
  other two need the daemon shell (a launchd agent needs a service account the
  installer creates, and a Windows service needs a service-control handler).
- The worker exposes no metrics of its own. The **scheduler** does — see
  [Distributed compilation](../getting-started/distributed-compilation.md#confirming-it-works).
