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
