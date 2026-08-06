# Running fastcached as a compile cache

Serving [`fastcache-cc`](../tools/fastcache-cc.md) needs no special mode: the
[compile-cache protocol](../protocols/compile-cache.md) is detected from the
first byte, so any listener serves it. What does need attention is sizing —
compile-cache values are far larger than the small values the defaults assume.

## Raise the value cap

**This is the one setting you must change.** The default per-value cap is
16 MiB. Object files in a large C++ codebase routinely exceed that; one
measured codebase peaked at ~122 MB for a single object. Values above the cap
are rejected, so those translation units silently never cache.

```sh
fastcached --port=11211 --storage-max-value=256M
```

`--storage-max-value` also raises the wire frame-payload cap, so a single flag
covers both limits. Set it above your largest expected object file.

To find that number for your project:

```sh
find build -name '*.o' -printf '%s %p\n' | sort -rn | head -5
```

## Persist the cache

Without `--storage` the cache is memory-only and evaporates on restart, which
for a compile cache usually means throwing away hours of compiles.

```sh
fastcached --port=11211 \
           --storage=/var/lib/fastcached/cache.cow \
           --storage-max-value=256M \
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
fastcached --bind=0.0.0.0 --port=11211 \
           --storage=/var/lib/fastcached/cache \
           --storage-shards=16 \
           --threads=16 \
           --storage-max-value=256M \
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

## Cohort prefetch

Prefetching is automatic and needs no configuration. When a FETCH hits a key
that belongs to a cohort, the rest of that cohort is warmed into memory in the
background, so the compiles that follow in the same build are served from RAM.

Clients control the grouping with `FASTCACHE_COHORT`. Since it is not part of
the cache key, regrouping never invalidates anything — set it per project and
branch (`myproject-main`) for the tightest prefetch locality.

## Monitoring

`--metrics` serves Prometheus metrics on `/metrics` and a liveness probe on
`/healthz` on a dedicated port (default 9259). For a compile cache the useful
signals are the hit ratio and the eviction rate: sustained evictions with a
falling hit ratio mean the working set no longer fits and `--max-memory` (or
`--storage-max-disk`) needs raising.

The client side reports independently — `fastcache-cc --stats` gives the hit
rate as each builder sees it, which is what actually determines build times.

## A note on client configuration

If hit rates are low, check the client before tuning the server. Cache keys
incorporate the compiler identity and the relativized command line, so mismatched
compiler versions or differing flags between CI and developer machines produce
different keys by design. `fastcache-cc --stats` distinguishes a genuine miss
from a cache that was never reached.
