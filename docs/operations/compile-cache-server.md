# Running fastcached as a compile cache

Serving [`fastcache-cc`](../tools/fastcache-cc.md) needs no special mode: the
[compile-cache protocol](../protocols/compile-cache.md) is detected from the
first byte, so any listener serves it.

## Check the value cap

The per-value cap defaults to 256 MiB, chosen for exactly this workload, so most
deployments need no flag here. It matters because a value above the cap is
rejected outright — that translation unit then never caches, which looks like a
poor hit rate rather than an error.

The rejection is at least diagnosable: the daemon answers with a typed
`payload-too-large` error naming the declared size and the cap, so it appears in
`fastcache-cc --show-stats` under fall-back reasons and in the daemon log, rather
than as a dropped connection.

Object files in a large C++ codebase routinely exceed 16 MiB; one measured
codebase peaked at ~122 MB for a single object. If your largest object goes past
256 MiB, raise it:

```sh
fastcached --storage-max-value=512M
```

`--storage-max-value` also raises the wire frame-payload cap, so a single flag
covers both limits — for the compile-cache and Redis protocols. The two
memcached framings keep a fixed 16 MiB ceiling that this flag does not move.

To find your largest object:

```sh
find build -name '*.o' -printf '%s %p\n' | sort -rn | head -5
```

## Persist the cache

Without `--storage` the cache is memory-only and evaporates on restart, which
for a compile cache usually means throwing away hours of compiles.

```sh
fastcached --storage=/var/lib/fastcached/cache.cow \
           --max-memory=8g
```

`--max-memory` sizes the in-memory L1 tier; the on-disk L2 grows as the
workload needs. Reads consult L1 first and fall through to disk on a miss, so a
warm working set is served from RAM while the long tail stays durable.

Cap the disk footprint with `--storage-max-disk`; the tree then evicts its LRU
tail to fit rather than growing without bound.

## Scale it

For a shared cache serving many builders concurrently:

```sh
fastcached --bind=0.0.0.0 \
           --storage=/var/lib/fastcached/cache \
           --storage-shards=16 \
           --threads=16 \
           --requirepass=<secret> \
           --metrics --metrics-bind=0.0.0.0
```

- `--storage-shards=N` (N>1) makes `--storage` a **directory** of `shard-NN.cow`
  files, so writes to different shards never block each other.
- `--threads=N` runs N pinned single-threaded reactors.
- Binding `0.0.0.0` exposes the cache to the network — always pair it with
  `--requirepass`. See [Deployment](deployment.md).

## Compression

The on-disk tier compresses values by default (`--compression=zstd`). Object
files compress well, so this is usually worth keeping; reads always return
plaintext because each record decodes by its own tag. Lower
`--compression-level` (default 3) if CPU is the constraint rather than disk.

Note this applies to the L2 disk tier only — L1 holds values uncompressed.

## Prefetch groups

Prefetching is automatic and needs no configuration. When a FETCH hits a key
that belongs to a group, the rest of that group is warmed into memory in the
background, so the compiles that follow in the same build are served from RAM.

Clients control the grouping with `FASTCACHE_PREFETCH_GROUP`. Since it is not
part of the cache key, regrouping never invalidates anything — set it per
project and branch (`myproject-main`) for the tightest prefetch locality.

## Monitoring

`--metrics` serves Prometheus metrics on `/metrics` and a liveness probe on
`/healthz` on a dedicated port (default 9259). For a compile cache the useful
signals are the hit ratio and the eviction rate: sustained evictions with a
falling hit ratio mean the working set no longer fits and `--max-memory` (or
`--storage-max-disk`) needs raising.

The client side reports independently — `fastcache-cc --show-stats` gives the hit
rate as each builder sees it, which is what actually determines build times.

## A note on client configuration

If hit rates are low, check the client before tuning the server. Cache keys
incorporate the compiler identity and the relativized command line, so mismatched
compiler versions or differing flags between CI and developer machines produce
different keys by design. `fastcache-cc --show-stats` distinguishes a genuine miss
from a cache that was never reached.
