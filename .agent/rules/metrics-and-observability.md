# Metrics and observability

Rules for `src/FastCache/Metrics/` and the admin/scrape endpoints the daemon and
the worker each serve.

Read this before adding a counter, adding a refusal code, or changing what
`/metrics` or `/healthz` reports.

The failure shape throughout: nothing fails. The process counts correctly, the
scrape parses, and the series an operator was told to read is simply not there —
which a dashboard renders as a gap and an alert as "no data" rather than as a
fault.

- **A counter is a row, so it cannot increment somewhere and export nothing.**
  `RenderPrometheus` listed the series it emitted by hand, and seven of the nine
  live counters were not on that list: both TLS splits, and **all five**
  `dispatch_*` -- which `docs/getting-started/distributed-compilation.md` names one
  by one as what to read off `/metrics` when distribution misbehaves. The guide sent
  an operator to an endpoint that had never carried a single one of them. Nothing
  fails: the daemon counts correctly, the scrape parses, and the series simply is not
  there, which a dashboard shows as a gap and an alert as "no data" rather than as a
  fault. `Metrics/MetricsCatalog.hpp` is now the one table -- enumerator, exported
  name, help text, type -- `static_assert`ed to hold one row per enumerator in
  enumerator order, and the renderer walks it. (The guide's names were wrong as well
  as absent -- it said `dispatch_leases_granted` where the series is
  `fastcached_dispatch_leases_granted_total` -- which is what documenting a series
  nobody could scrape lets you get away with.) Consequences that are each
  load-bearing:
  - **The test asserts over the TABLE, not against a list written out beside it.**
    A hand-written list of expected series is the very thing that went stale, and it
    would have to be updated by the same person who forgot the renderer. Each counter
    is incremented by a **distinct** value first, because a table of near-identical
    rows makes "this row renders its neighbour's value" the likely slip, and equal
    values would let it through.
  - **Eight enumerators were deleted rather than exported.** `CmdGet`, `CmdSet`,
    `CmdDelete`, `GetHits`, `GetMisses`, `Evictions`, `BytesIn` and `BytesOut` were
    incremented by nothing anywhere in the tree -- the real numbers come from
    `StorageStats`, which is authoritative for them -- so giving them rows would have
    published a permanent zero next to the true value under a similar name, which is
    worse than an absent series. The 17-arm `ToStringView` that named them went with
    them; it had no caller either.
  - **A duration is a `_sum`/`_count` pair, not a gauge.** The sink is counter-only
    by design, and a "last compile took N ms" gauge is a sample of one that a scrape
    lands on by luck. `WorkerCompileMillisTotal` beside `WorkerJobsCompleted` is what
    a Prometheus histogram already reports, and a rate over either window gives the
    mean for that window rather than for all time.
- **A refusal's wire code and its counter are one row, because they are one fact
  told to two audiences.** The client decides whether to retry from the code; the
  operator decides what to fix from the counter. Split across a `switch` and a second
  `switch` they drift, and a refusal counted under one reason while being reported as
  another is worse than not counting it at all -- `ScratchUnavailable` and
  `SpawnFailed` had both answered `StorageWriteFailed`, so a worker with no scratch
  disk told the client "storage write failed" and an unwritable disk was
  indistinguishable from a toolchain that is configured but cannot be executed, from
  either end. Found diagnosing a CI failure that reported the one thing it could not
  possibly be. `RefusalTable` carries no default member initializers, deliberately: a
  row answering two of the three questions is not a row, and `ErrorCode` has no zero
  enumerator for `{}` to name in the first place.
- **A process with no cache reports no cache, and absent is not zero.**
  `fastcache-compile-node` serves the daemon's own `AdminHttpServer` over the same
  renderer -- a second implementation for a process without storage is exactly how
  two endpoints come to disagree -- so `MetricsSnapshot::storage` and `::host` are
  `optional`. A default-constructed `StorageStats` would publish `fastcached_items 0`
  and `fastcached_bytes_limit 0`, which is not "no cache" but "an empty, unbounded
  cache", and a dashboard reads that as a fact. The daemon leaves `host` absent for
  the mirror-image reason, and **names the field to do it** (`.host = std::nullopt`)
  rather than letting it default: a designated initializer that skips a field added
  to the middle of a struct is a warning at best and a silent zero at worst.
- **A merged snapshot is one tier's answer standing in for all of them, so the
  split is reported too.** `LayeredStorage::Snapshot()` returns its canonical lower
  tier's item count, bytes and budget with the composite's own hit/miss patched over
  them and only the evictions summed — so with `--storage`, or with the node's
  `--cache-dir`, the in-memory tier an operator sized has never appeared on a scrape
  at all. `IStorage::SnapshotTiers()` answers per tier, and three rules travel with
  it:
  - **Absent is not zero, one level below `MetricsSnapshot::storage`.** A tier the
    cache does not have renders **no line**, not a zero and not a bare `# HELP` with
    no sample beneath it. `fastcached_tier_items{tier="disk"} 0` says a disk tier is
    standing empty, and a memory-only node has no such tier to be empty.
  - **The label values come from `StorageTierTable`, not from a hand-written list.**
    Same rule the counters follow against `MetricsCatalog`, applied to the other
    axis: a tier added to the enum reaches the scrape by being a row.
  - **Nothing per-tier is a total waiting to be summed, and the ones that would
    mislead are simply not published.** The memory tier mirrors what it reads out of
    the disk tier, so adding item counts double-counts; and a lower tier is consulted
    only when the one above it missed, so adding hit counts turns a cache serving
    every read into one at 62%. Items, bytes, budget and evictions carry the `tier`
    label; the hit/miss split stays on the unlabelled series, because it is the one a
    dashboard is likeliest to add up.
- **A worker's admin endpoint is off unless asked for, binds loopback for a bare
  port, and is FATAL when it cannot be served.** All three differ from the daemon's
  and each was chosen: a scrape surface reachable from the network is an operator's
  decision rather than something they get by typing a port; and an operator who asked
  a *worker* for an endpoint is almost always wiring a probe to it, so a worker that
  started without one looks healthy to the very thing that would have reported it was
  not. It also brings `/healthz`, which the worker has never had -- a supervisor could
  tell that the process was alive and not that it was *answering*, which is the state
  a wedged worker is in.
  - **It is a class in its own translation unit, and both halves of that are the
    point.** The listener, the server and the serving thread have a *destruction
    order* -- the server must close its listener before the thread can be joined,
    because POSIX does not unblock a parked `accept()` -- and three locals in
    `main()` express that as a `Shutdown()` somebody has to remember at every return
    path. Here it is the destructor. And `main.cpp` is in no test target, the lesson
    `CacheProtocol.cpp` and `RootReconciler.cpp` are each recorded as having been
    extracted for, so the wiring lives in `AdminEndpoint.cpp` where its tests can
    reach it.
  - **A bind failure is provoked with an address no host holds, not by taking a
    port twice.** RFC 5737 `192.0.2.1` is refused on every platform and needs no
    second listener kept alive for the duration; that a port already being served
    is refused belongs where the option refusing it is set, and is asserted there
    (`SocketAddress_test.cpp`). It was not always refused: `SocketAddress.cpp` set
    `SO_REUSEADDR` unconditionally, which on POSIX only skips `TIME_WAIT` but on
    **Windows lets a second socket bind an address a live one already holds** -- so
    until issue #85 any process on the box could take this endpoint's port and
    answer the scrapes an operator was told to trust.
  - **The stop test is bounded rather than allowed to hang.** A case that deadlocks
    when the destruction order is reversed reports the defect as a suite timeout
    naming nothing, which this repository has already paid for once
    (`dist-compile-e2e ***Timeout 900.10 sec`). It destroys the endpoint on another
    thread and fails in 15s saying what it waited for.
- **A fleet report is served by the leader, and anyone else says so rather than
  guessing where it is.** A follower's `WorkerRegistry` holds whatever registered
  against it, not the fleet, which is why `SchedulerService::Gate()` refuses every
  verb -- reads included -- from a node that does not lead. `/fleet` answers `503`
  naming the leader and its **scheduler** endpoint: that is `NotLeader` in HTTP's
  vocabulary, and 200 would present a partial registry as the whole picture.
  - **It is not a redirect, and it is not a link.** A dashboard address is local
    configuration on each node and nothing replicates it, so any URL built by the
    refusing node is a guess. Guessing here is the defect this project has already
    had once, in its other spelling: a follower redirected clients to the leader's
    *consensus* port, which speaks nothing a client understands. That is also why
    a `dashboardEndpoint` on `ClusterMember` was refused rather than added --
    `Cluster::ClusterState` is versioned and its decoder refuses any other version,
    so a field costs a `StateVersion` bump that makes every existing on-disk
    snapshot and log entry undecodable, and a half-upgraded cluster unable to apply
    each other's entries. That is a consensus-format migration bought for a
    diagnostic page.
- **The columns are a table both renderers walk, and the test asserts over the
  table.** The page and the JSON take one spelling per column -- it is the header
  cell *and* the key -- so the two cannot drift, and each row carries a projection
  rather than a value, the shape `TierMetric` already uses against
  `StorageTierTable`. A hand-written list of `<td>`s is the same defect as a
  hand-written list of series, and a list of *expected* columns written out beside
  the table is what goes stale, maintained by whoever forgot the renderer.
- **Absent is not zero, one level further in than a series: at the cell.** Every
  `optional` in `NodeLoad`, `NodeCacheLoad` and `NodeCacheCapacity` reaches a cell
  unflattened -- `null` in JSON, a dash on the page, the spelling `--cluster-status`
  already uses. `0` is a claim: `cores 0` says a machine has no CPU, and a cache
  that has served no reads has no hit rate rather than one of 0%. A table cannot
  omit one cell the way a scrape omits a line, so the granularity of the rule here
  is the **column**: a tier no member runs contributes no column at all, and a
  member lacking a tier others have renders an absence in it.
- **A fleet total is computed per machine, never per registry entry.** The registry
  keys on `(fingerprint, endpoint)`, so a node with two `--toolchain` flags is two
  entries carrying one machine's cores -- and a page listing workers would report a
  fleet twice the size of the one an operator owns. `WorkerRegistry::NodeReports()`
  is the grain, and it decides once which fields add across siblings: the slot
  count is the maximum (both were derived from the same cores), the in-flight count
  is the sum (those are different jobs).
- **The dashboard's credential is separate from `--requirepass`, and the surface
  is refused rather than served without one.** `--requirepass` is what a node
  *presents* to the scheduler and every member of the fleet holds it, so reusing it
  would let any worker read every other node's fleet map -- wrong direction and
  wrong grain. It is a file for the reason `--cluster-key-file` is one, and a
  non-loopback `--admin-listen` with `--dashboard` and no token file is a **startup
  refusal**: the page lists every member's hostname, endpoint and capacity, and
  HTTPS does not substitute, because TLS authenticates the server to the browser
  and says nothing about who the browser is. Loopback needs none -- reaching it
  already means being on the machine.
  - **Both `Basic` and `Bearer`, as a table.** Bearer alone would have made the
    page unopenable in the browser it exists for: there is no browser prompt for
    it, so a reader would have to paste a token into a URL or a cookie. The 401
    carries `WWW-Authenticate`, without which a browser shows the body and no
    prompt -- which reads as a broken page rather than as a credential being
    required. A scheme that cannot parse its parameter is a refusal, never a
    fall-through to the next row, or `Basic` with unreadable base64 would be
    compared verbatim as a bearer token.
  - **The credential gates the dashboard routes and nothing else.** `/metrics` and
    `/healthz` stay exactly as open as they were, so turning the dashboard on
    changes nothing for a scraper or a probe already pointed at that port. TLS is
    the same story: it is on by naming a certificate and a key, so a node that
    names neither keeps a plaintext admin port that behaves as it always did.
