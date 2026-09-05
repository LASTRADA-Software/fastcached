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
- **A bounded listing carries its own total, sampled with it.** The outstanding
  leases the fleet page shows are the *oldest fifty* -- a fleet at full tilt holds
  thousands and a page that rendered all of them is one nobody can read -- so the
  section says which it is showing (`the 50 oldest of 900`) and the JSON key is
  named `leases-outstanding-oldest` rather than leaving a reader to take a window
  for the whole set. The count comes back from the SAME call under the SAME lock as
  the rows (`LeaseListing`): asked separately they are two samples, and a lease
  resolved between them makes a window look complete -- the truncation notice
  wrong in exactly the situation it exists for. Which end is not presentation
  either: the oldest are the leases that have stopped moving, and a lease still
  outstanding after minutes is a client that died mid-build rather than one waiting
  to age out, because a client resolves its own lease when the job ends.
- **A pill's threshold belongs to what it is measuring.** `CellDecor::Freshness`
  turns amber after fifteen seconds, which is right for a heartbeat and wrong for
  everything else: a fifteen-second-old lease is an ordinary compile still running,
  so reusing that decor for a lease age would paint every row amber and the colour
  would stop meaning anything at all. `LeaseAge` is its own row, at half
  `LeaseTable::DefaultLeaseTimeout` -- past which a lease is closer to expiring than
  to having been taken -- and it is *derived* from that constant rather than typed
  as a number, so a threshold cannot silently stop tracking the timeout it is about.
  Both draw through one helper, because two near-identical `case` bodies is how a
  pair comes to differ for no reason anybody intended.
- **An outcome that can be "not attempted" is not a `bool`, and the converse of
  absent-is-not-zero is that an absence gets counted as an event.**
  `ICacheUpstream::Store` returned `bool`, and `NoUpstream` -- the honest, named
  shape for a machine with no shared cache -- returned an honest `false`.
  `LocalCache` read that as *the shared cache declined this* and incremented
  `NodeCacheUpstreamStoreFailures` on **every local store**, so a single-machine
  install reported a 100 % upstream store failure rate: 1800 failures against 1800
  sets, indistinguishable from a fleet whose cache is down. An operator alerting on
  that counter alerts permanently on every laptop, which is how a counter stops
  being read at all. The fix is at the seam -- `UpstreamStore { Stored, Declined,
  NotConfigured }` -- because a `false` meaning two things is a defect wherever it is
  read, and telling the one caller which it was would leave the next one to make the
  same mistake. Which counter each outcome moves is then a table with
  `NotConfigured` naming **none**, a legitimate row the way `RefusalTable` already
  has rows that move nothing.
- **A cumulative counter is not where an absence is modelled; the snapshot is.**
  The temptation on the above is to render no upstream lines at all, the way
  `SnapshotTiers()` omits a tier the cache does not have. Every counter is exported,
  without exception, and a per-counter "does this apply?" predicate is precisely the
  mechanism that once left seven of nine live counters unexported. The distinction:
  a *snapshot* value is a reading, so a zero is a claim about the world and absence
  must be spellable -- a *counter* is a tally of events, and zero is the truth about
  events that never happened. What the counters genuinely cannot answer, because
  they are cumulative, is whether there is an upstream at all: a node with none and
  a node with one it has not written to both read zero. So that is named, as
  `MetricsSnapshot::upstreamConfigured` beside `host` and `storage`, and rendered
  only by a process the question applies to.
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
    the same story: it is on because material was named or asked for, so a node
    that does neither keeps a plaintext admin port that behaves as it always did.
    **Plain HTTP is a supported way to run this**, not a fallback -- on loopback,
    or behind something that terminates TLS -- which is why nothing anywhere
    requires a certificate.
- **TLS is on by naming material or by asking for material to be MADE, never by a
  bare boolean.** `--tls-self-signed` looks like the boolean that rule forbids and
  is the same rule kept: what the rule prevents is a configuration that asks for
  TLS this node cannot then serve, and asking for a generated certificate
  *produces* the material. The two spellings contradict each other and are refused
  together, rather than one silently winning and serving an identity nobody chose.
  - **A generated certificate encrypts; it does not identify.** Nothing signs it,
    so a client that has not been told its fingerprint out of band cannot tell
    this node from anything else answering on that address. Two consequences are
    load-bearing: the credential is still required off loopback, because TLS
    authenticates the *server* to the browser and says nothing about who the
    browser is; and the fingerprint is logged at startup, because it is the only
    thing an operator can compare.
  - **Its subject names decide whether it is usable at all.** Every modern client
    ignores the common name, so a certificate whose SAN omits the name an operator
    types matches nothing however it is labelled -- and a name mismatch is a second
    browser warning on top of the unknown issuer, much harder to click past than
    the first. Each name is classified by what it *parses as* rather than by how it
    is spelled: guessing from a leading digit or a colon gets a host called
    `10things` and the address `2001:db8::1` wrong in opposite directions.
  - **The fixture greps `-checkhost`'s output rather than its exit code, and
    carries a negative control.** `openssl x509 -checkhost` reports its answer in
    the text and exits 0 either way, so an exit-code check passes for a mismatch.
    That was found by the control -- a name nobody asked for, asserted NOT to
    match -- which is why one is there: without it the whole block passed while
    asserting nothing.

- **A history stores a counter RAW; a rate is the difference taken at render.**
  Storing the delta at sample time throws away the one thing that distinguishes a
  restart from a quiet fleet. A counter that returns to zero produces a negative
  step, and only a raw series can see it -- a stored delta is unsigned and has
  already turned that step into an enormous spike nobody can explain. Raw, the
  bucket is a **gap**, which is the truth: the step across a restart is unknowable,
  not zero and not large.
  - **A bucket nobody sampled is absent, not zero**, for the reason every cell on
    the page already is. Zero says the fleet did nothing; absent says nobody was
    watching, and a leader that has just taken over means the second. The same
    goes for a rate taken *across* a gap: two readings ten minutes apart with the
    middle five unobserved would spread the work over minutes nobody saw, so the
    delta is only ever taken between **adjacent present** buckets.
  - **A share with a zero denominator delta is absent, not 0%.** A bucket that
    served no reads has no hit rate. Rendering 0% says the cache missed
    everything, which is a different and much more alarming claim -- and the one
    an operator would act on.
  - **A share folded over a range is taken over that range's whole traffic**, never
    as the mean of the per-bucket shares. The average weights a bucket that served
    four reads exactly as heavily as one that served forty thousand, so a single
    idle bucket at 0% drags a headline figure the chart beside it visibly
    contradicts.
- **History is a convenience: no state of its file may keep a node from starting.**
  Missing, short, wrong version, bad checksum -- every one of them starts empty and
  logs one line. It is written whole to a temporary and renamed, so a crash
  mid-write leaves the previous file rather than half of this one. Where it lives
  follows the directories the node already has (`--cluster-dir`, else
  `--cache-dir`, else memory only) rather than a flag of its own: a third place to
  say "put state here" is a third place to point at the wrong disk -- and every one
  of those files is a row of one table, or the third gets derived by string surgery
  on the second's answer.
  - **A file a LATER build wrote is kept and never written over**, and that rule
    belongs to the ENVELOPE rather than to each store. Magic, version, length,
    CRC32C, temp-then-rename and the refusal are one `FileEnvelope` descriptor
    precisely because the second store was written by copying the first and lost
    exactly that clause: it started empty on a newer file and then overwrote it,
    which for the store holding *every other machine's* year is a bigger loss than
    for a node's own, not a smaller one.
- **A node records ITSELF always; only the FLEET-wide slots are leader-only.** A
  follower's registry holds whatever registered against *it*, so a fleet-wide sample
  taken there records a fraction as though it were the whole, and the chart then
  shows the fleet shrinking every time leadership moves. What a machine can claim
  about itself -- its cache, its slots, its own compiles -- is true whoever leads,
  and a node that stopped recording it on losing an election gave its year to
  whichever peer happened to be leading. Which of the two a slot is is the `scope`
  column of `FleetMetricTable`, never a conditional, and a `static_assert` requires
  both scopes to be populated: all-`Fleet` reads exactly like the guard this
  replaced, all-`Node` has followers reporting counters they never produced, and
  neither is visible in any other check.
  - **Every node samples, whatever surfaces it serves.** The sampler is not part of
    the admin surface: `StartAdminSurfaceOrExplain` returns before building anything
    when `--admin-listen` is empty, so a sampler owned there left a *pure worker* --
    the machine actually doing the compiles -- recording nothing at all, with the
    hole invisible because the leader's own series stayed complete for every window
    it was elected for.
- **A BACKFILLED window answers for a machine, never for the scheduler.** A window
  the leader was not elected for is assembled by summing what the nodes handed over,
  and no machine can answer for a dispatch outcome -- so `BackfillInto` fills the
  node-scoped slots and leaves the rest at zero. **Zero is a legitimate reading for
  every slot here**, so a renderer that takes it draws a fleet that granted nothing
  between one that granted a thousand and one that granted two thousand: a rate
  running backwards, swallowed as a restart, then a spike of two thousand. Neither
  happened, and both sit in the twelve-month ring long after the election that
  caused them. The predicate is `bucket.backfilled` against the metric's `scope`,
  applied at every reader of a bucket's values -- the per-bucket series and the
  range summary alike.
- **The page reaches a history through ONE door.** `IFleetHistoryView` is what the
  routes hold, and the two halves of a leader's record -- what it sampled while
  leading, and what the machines handed over for the windows it missed -- meet
  behind it. Handed a raw `FleetHistory const*` instead, the routes drew the raw
  series: the whole handover was filled, persisted, restored and never once drawn,
  while the merge function's own comment claimed a route could not reach past it.
  **Assert the wiring, through a route, not the merge.**
- **A retention cost is REPORTED, never estimated.** The rings are allocated in
  full at construction and do not grow, so `FleetHistoryBytes()` is the steady
  state rather than a ceiling -- 800 KiB a series, 1.6 MiB for a node's own two,
  and another 800 KiB on a leader for every machine that reports to it. The node
  logs it at startup, because a number an operator sizes a machine against has to
  come from the build they are running rather than from a document.
- **Coverage is how much of a window was observed, and it is said in words.** A
  bucket observed for one minute of five is drawn exactly like one observed for all
  five -- its value is a true reading either way, a gauge's last sample or a rate
  over the span actually seen -- so the difference belongs in the meta line and in
  `/fleet/series.json` (`covers` beside `coverage[]`), not in a different shape that
  would imply the number itself is suspect. The **newest** window is excluded from
  that count, always: it is still filling and is partly covered by definition, so
  counting it puts a number on every live page that means nothing.
- **A chart served as its own resource cannot see the page's custom properties.**
  An `<img>`-referenced SVG is a *separate document* and inherits nothing, so each
  one carries its own palette and its own `prefers-color-scheme` block -- and the
  theme is therefore part of its URL and part of its cache key. The sparkline is
  the exception that proves it: inlined into the page, it is part of that document,
  so it carries no palette and resolves the page's `var()` like anything else.
  - **The chart URL carries no cache-buster.** A generation in the query would make
    every closed bucket a new URL, and the conditional GET the whole arrangement
    exists for would never fire. Stable URL, `ETag` from the sampler's bucket
    counter (byte-exact and free, unlike a hash of the body), and a `Cache-Control`
    that runs only to the **end of the bucket being drawn** -- a fixed `max-age`
    leaves a viewer a whole bucket behind for the rest of it.
  - **A `304` carries its validators and no content at all.** RFC 9110 §15.4.5
    forbids content, and §8.6 forbids a `Content-Length` that is not what a `200`
    would have sent: a client that reads one and then finds the connection closed
    reports a truncated response rather than a cache hit. Whether a body is allowed
    is a property of the **status**, checked once in the writer, so a route
    answering `304` cannot get it wrong by forgetting.
  - **Every chart route is behind the same credential as the page.** An image URL
    that answered without one would leak the fleet's whole history while `/fleet`
    stayed locked. The gate is written once and the renderer is the parameter, so a
    route added later cannot be one that forgot to check.
- **An unknown `range` is refused; an unknown `theme` is not.** The asymmetry is the
  rule: a substituted range puts a reader on a different axis than the one they
  asked for with nothing on the page saying so, while `auto` renders correctly
  under either setting and costs them nothing. Refuse where a silent substitution
  would mislead, default where it cannot.
- **A stacked area is drawn top band first.** Each band is filled to the baseline
  and the band below paints over the part that is not its neighbour's. Drawing
  bottom-up leaves every band overlapping every one above it, and translucent fills
  then multiply into a colour that belongs to no series -- which is the "four
  reasons collapse into one" that chart exists to prevent, reintroduced by the
  renderer.
- **A `<circle>` is not path data.** One helper produced both the `d` value for a
  run of readings and the dot for a run of *one*, and returned them concatenated
  in a single string the caller put in `d="..."`. A browser parses an
  `<img>`-referenced SVG as XML and refuses the **whole document** over a `<` in an
  attribute value, so the chart arrived with a 200, a plausible byte count, and
  rendered as a broken image saying nothing. Markup of two different kinds comes
  back as two members, so the type says which is an attribute value and which is an
  element -- and the dots are then siblings carrying their own `fill`, because a
  document that inherits nothing renders an unfilled circle black.
- **A run of one reading is a dot, filled shape or not.** Closing a run of one
  gives `M x y L x bottom L x bottom Z`: a shape with no width, which draws
  nothing while leaving a non-empty `d` that says the series *was* observed, so the
  "never observed" guard beside it does not fire either. Every refusal series is a
  rate and a rate needs the bucket before it, so one sampled pair between two gaps
  is exactly one value -- a single refusal in an otherwise quiet hour, which is the
  reading somebody opened that chart for.
- **A renderer tested only on dense data is a renderer tested on data nobody has.**
  Both faults above were invisible for as long as every case rendered a full run of
  buckets: a run of one cannot occur there. Gaps are the ordinary case on a live
  dashboard -- a node samples only while it runs -- and the assertion that catches
  this class is a scan for `<` inside an attribute value, over gap-laden data.
- **A gridline's value label goes below its line.** The topmost gridline sits
  `PadTop` from the edge of the viewBox, so a label placed above *that* one has its
  ascenders outside it -- clipped silently, and only ever on the line carrying the
  largest number on the chart.
- **An `xmlns` is not a fetch.** `http://www.w3.org/2000/svg` is an XML namespace
  *name*; nothing resolves it. A "this page is self-contained" check written as "no
  absolute URL anywhere" fails on the inline sparkline and pushes it out of the
  page for a reason that was never true. Ask instead whether any attribute a
  browser resolves -- `src`, `href`, `@import` -- points off this origin.

- **A node's reported version is compiled in, never configurable.** The column
  exists to answer *which binary is actually running on that machine*, and it is
  read during a rolling upgrade -- so a version a node could be **told** to report
  is one that can be wrong at exactly the moment somebody is relying on it.
  - **It rides REGISTER's nested capacity record, not its top level.** That
    message's arity is exact and fixed forever; a sixth field there makes two
    builds of one fleet unable to speak at all. The nested record is read with the
    variable-arity split, so compatibility runs both ways: an older node reports
    nothing and is admitted, and a newer node's extra field is skipped by an older
    leader.
  - **It is refreshed on re-registration.** That path *is* a restart, and restarting
    on a new build is what an upgrade looks like. A version carried over from the
    first registration would leave the page naming the old binary for as long as the
    new process stayed up.
  - **It stays out of `NodeCapacity`.** That struct is a **literal type**, exercised
    by `constexpr` tests over `OfferableSlots` and the slot ceilings, and one
    `std::string` in it ends that property for the whole codebase. A version is also
    not a scheduling input -- nothing weighs it -- which is what `NodeCapacity`
    holds. It lives beside the other registration strings on `WorkerInfo`.
  - **A version whose SHAPE this build cannot parse is reported, not refused.** It
    is a string an operator reads rather than one any code branches on, so an
    unrecognised shape is a peer to report; refusing the registration over a
    diagnostic field's *format* would take a working machine out of the fleet. That
    is a different question from whether it is text at all -- see below, which does
    refuse. It reaches an HTML page, so it is escaped like every other value that
    came off a wire.

## Skipped, absent, unstarted and failed are four states, and tools collapse them

`Absent is not zero` above is the metrics instance of a rule that is not about
metrics. In one working session this project's own tooling collapsed at least two
of these four states **five separate times, in four different instruments** — none
of them a coding mistake, all of them a representation that could not tell two
things apart. The last three rows are from a later session and are counted
separately, so the table now stands at **twelve collapses across eight instruments**:

<!-- table-total: instruments=rows, collapses=weighted -->
| instrument | what it reported | what was true |
|---|---|---|
| the e2e wait classifier | `WORKING`, from CPU spent | that CPU was *cumulative*; none of it was recent |
| `build.yml`'s gated jobs | skip, from `code == 'true'` | the classifier had **failed**, so it answered nothing |
| the required-checks reading | a skipped required check passes | a skipped **matrix** job never reports at all |
| `pr-labels.yml`'s gate | skipped, therefore green | the labeller had **failed** |
| a CI watcher (twice) | "settled", then "25 of 26 green" | the list was still being built; the 26th was correctly skipped |
| a merge watcher (twice) | stop watching, on **any** red; then `RED`, from a `*)` | the red was an *unrequired* packaging leg, so ten required contexts went unwatched — and the repair's own default arm then swallowed `cancelled` |
| a merge-readiness checker (three times) | `fail` | `SKIPPED`, then `CANCELLED`, then *pending* — the last inventing a red that did not exist |
| a rule-vs-tripwire cross-check | `0`, read as *the two files disagree* | the phrase was present and **wrapped across a line** |

**An instrument answers the question it was given, and its author's confidence comes
from the question they meant.** The last row is the smallest instance and the clearest
statement of that: `grep -c` was asked whether a phrase occurs *on one line*, answered
`0` correctly, and was read as *the two files disagree* — by the author of the rule
about probes, minutes after writing it. No state was collapsed inside the tool at all;
the collapse was between the question asked and the question meant.

**Five of these were found in somebody else's tool; two were found by an author in their
own, within an hour of writing the rule against them.** That second group is the whole
argument for this being a check rather than better prose: the people who wrote it down
were the next to do it.

**Two rows are sharper still, because in them the collapsed distinction is the very one
the instrument existed to report.** A watcher built to tell required from unrequired
stopped at the first red without asking which it was; a checker built to keep `ABSENT`
distinct from pending folded `SKIPPED`, then `CANCELLED`, into `fail`. Neither author was
careless — a red is a red is the obvious reading, and it is wrong in exactly the domain
each tool was written for. So the sharper form of the observation that closes this
section: **a state-collapsing bug is likeliest in the tool whose job is that state
distinction**, because its author is thinking about the subject's states and not about the
instrument's. Every row above was written by somebody who had the distinction fully in
mind, and the last two by authors who had just been told about the others.

**The repair for one collapse is a prime site for the next, and the location is the
default arm.** A sixth followed within the hour: splitting `pending` out of `fail`, in
that same edit, left a `*)` that swallowed `cancelled` — so a superseded label run was
reported as `1 required context RED` on a pull request that had no red. Enumeration would
not have prevented it, which is what separates it from the row above: there `cancelled`
was **never enumerated**, an omission; here the arm was written **deliberately**, and the
author would have enumerated exactly the states they were thinking about and defaulted the
rest. The remedy is therefore not a longer list but **no default arm at all** — a `case`
with a `*)` is an unguarded table, and this is the `EnumTable` + `RowsInEnumeratorOrder`
argument the C++ side of this repository has made for years and has never once made for a
shell script.

**And a verdict computed from a summary, while the evidence sits in the same output, is a
separate defect from one computed from bad evidence.** That run printed
`PR labels  completed  cancelled` two lines above its own wrong conclusion; the tool had
gathered and displayed the disproof and then never consulted it. It is the more durable of
the two, because the output *looks* thorough.

What bounded the damage is worth naming, since every instance here is a tool that reported
*something* where it should have reported *nothing*: run from the wrong directory that same
script failed **closed** — `could not read the pull request head` — instead of reporting
zeros. That is *zero rows is not a verdict, it is the absence of one*, obeyed, and it is
the design decision that made the sixth the least bad of the six.

The fixed watcher shows the other half of a repair, usually left out: it reports an
unrequired red by name **and keeps watching**. One that had merely stopped exiting would be
indistinguishable from the broken one on any green run.

The four states, and why each pair is dangerous:

- **Deliberately not done** (a skipped job, an unsampled reading). Legitimate, and
  it must not be counted as an event or as a zero.
- **Not done yet** (a queued check, an unpopulated list). Says nothing at all, and
  reads as *finished* to anything that only counts what is pending.
- **Could not be observed** (an unreadable file, an unsampleable process, a failed
  classifier). This is the one that most often becomes a zero, and folding it into
  a sum is how "we could not tell" becomes "there was nothing there".
- **Observed and failed.** The only one anybody designs for.

Four consequences worth applying before writing the check rather than after:

**A count cannot carry this and neither can a `bool`.** `25 of 26 green` is
arithmetic that is true and useless. An outcome that can be *not attempted* is an
enum — the same conclusion the metrics rule reaches from the other end when it says
`NoUpstream`'s honest `false` was read as a failed store.

**Absence of the negative is not the positive.** "No pending checks" is not "all
checks reported"; "no failures found" is not "the tool ran". Every check that
concludes from a count of *bad* things needs a separate assertion that the good
things exist — which is why `tsan-gate.sh` proves the sanitizer is live before it
believes a clean run, and why `merge-queue-contexts` counts total references as
well as safe ones.

**And it applies to a probe somebody types, where the cheap half gets skipped.** The
must-find control is already carried to a typed search — `testing.md`'s *a census
that returns zero gets a positive control* — so what is missing is not that half but
the other: **read the probe's exit status too, and do not assume a non-match is the
only way to get nothing.** A content probe once returned empty for *every* string,
including ones visible in the output two lines earlier, and read exactly like dropped
hunks in a merge just declared clean. `grep -c -i -F` had **aborted: exit 134,
SIGABRT, no stdout** — and note it is not `2`, so a reader checking `status == 2` for
"error" misses precisely this case. Treat anything that is neither `0` nor `1` as the
instrument failing.

Which signal survives depends on the invocation, and getting that wrong argues for
throwing away the cheap one. A bare `grep` prints nothing whether it matched nothing
or died, so its stdout genuinely cannot separate them; `grep -c` prints `0` on a
non-match and printed nothing here. **Two independent signals were available and
neither was read.** The control is the stronger of the two — it survives a tool that
lies about its status as well as one that crashes — but it is not a substitute for
reading the number the tool already handed you. `build-and-toolchain.md` carries the
pipeline-corruption forms of the same family (`producer | grep -q`, `| tail`), which
are a different mechanism: there the query ran and its status was misread.

**Ask which state a silent tool is in, and make it say so.** Where the answer
genuinely cannot be determined, report that as its own outcome rather than picking
the nearest neighbour: `Get-WaitVerdict` reports `INCONCLUSIVE` and names which
reading was unreadable, because a verdict it cannot support is worth less than an
admission it cannot.

The pattern is easy to recognise in someone else's code and nearly invisible in
your own, because in your own the collapsed state is the one you were not thinking
about when you chose the representation.

## A refusal answered while nothing rises is a port that looks unused

Six refusals in `WorkerProtocol.cpp` answered the right wire code and incremented
nothing (#327). That was not six oversights: `Wire::EncodeErrorReply` takes a code and
knows nothing about a sink, so counting was something each author had to remember, and
a seventh would have joined them by omission. On `/metrics`, a port being probed then
looks exactly like a port nobody is talking to.

- **`Refuse(metrics, row, detail)` is the one way that surface refuses**, and it takes
  a ROW carrying the code AND the counter. There is no argument to pass a bare
  `ErrorCode` to, so the counter cannot be left out — the same guard shape as
  `NodeSurfaceTable`, where an opener takes a `NodeSurface` because there is nothing
  else to hand it.
- **The row is the REFUSAL, not the code**, and that distinction is load-bearing. Two
  of the six answer `MalformedFrame` and must not share a counter: a truncated frame
  is a framing fault or a hostile peer, an undecodable payload is a version or
  encoding mismatch between two ends that agree on the framing. One code, because a
  client acts on both identically; two counters, because an operator does not. **A
  table keyed on the code could not hold both** — which is why `EnumTable<ErrorCode,
  Counter>`, the obvious instrument, is the wrong one here.
- **`ErrorCode` could not carry an `EnumTable` anyway.** It is a WIRE enum with sparse
  values and no `Last`, and a sentinel would permanently claim a byte in the one enum
  whose retired-code rule exists because bytes are scarce and irreversible.
- **`RefusedVerb` cannot grow a counter column.** It lives in `CompileCacheWire.hpp`,
  which is header-only and dependency-free because `fastcache-cc` compiles it in
  without linking `FastCache`; `IMetricsSink::Counter` is not reachable from there. The
  pairing lives in the surface that owns the sink.
- **The refusals that ALREADY counted go through the same door**, converting their own
  tables' rows rather than restating the pair. Not tidiness: it is what makes the guard
  a structural fact — `EncodeErrorReply` appears exactly ONCE in that file — instead of
  "is there an `Increment` within N lines", which passes for the wrong reasons when an
  increment belongs to another branch or drifts out of range.
- **`worker-refusals-counted` closes the door the type system cannot**, because
  `EncodeErrorReply` is a free function every surface includes and stays callable. It
  fails in BOTH directions: a call site outside the primitive is named by line, and a
  scan that matches NOTHING is its own failure rather than a pass — the spellings are
  built on one each, so zero means the scan stopped seeing the file it thinks it is
  reading.
- **It GLOBS `src/`, and the thing it used to be is why.** For three years of this
  file's history it scanned a hand-maintained list of `.cpp` files, which grew by hand
  twice — `CompileResponder.cpp` after arriving with uncounted refusals in it, then
  `FrameEndpoint.cpp` after accumulating FIVE through an entire surface migration with
  the check green throughout — and could not reach a header at all, which is where
  #447 then had to put two security counters. **A list is exact about the files it
  knows and silent about the ones it does not, and silence reads identically to
  complete coverage** (#492). An over-broad scan fails CLOSED: a file that should not
  be scanned produces a visible failure somebody investigates, where a file that should
  be scanned and is not produces nothing. The only rows left are the two ends of one
  function — the header that DEFINES the encoder and `Protocol/SurfaceRefusal.hpp`,
  which wraps it — so adding a row is the only way to weaken the check, which makes
  weakening it an argued edit rather than an omission.
- **The primitive has a shared home for that reason**, not for tidiness.
  `SurfaceRefusal` and `Refuse` lived in `fastcache-cc`'s private `WorkerProtocol.hpp`,
  which the node's generic frame listener included for two symbols; with them in
  `Protocol/SurfaceRefusal.hpp` coverage is a property of the TYPE rather than of
  somebody's memory. Header-only, because `Refuse` reaches only `CompileCacheWire.hpp`
  and `IMetricsSink.hpp` — so it costs `_fc_cc_core` no row, and `CompileCacheWire.hpp`
  stays dependency-free.
- **"Deliberately uncounted" and "forgot to count" must not be the same text**, and
  they were. The considered decisions below — the byte budget, the framing arms, the
  unserved verb — were spelled as a bare `EncodeErrorReply`, exactly like the five
  defects, so no scan could separate them and any list admitting one admitted the
  other. **Three spellings, three different claims**: `Refuse` says a rise here means
  something; `RefuseWithoutCounter` says a rise would mean nothing and carries the
  reason; `RefuseUntriaged` says nobody has decided and carries the issue that will.
- **The third is safe only because the check TALLIES it.** It prints the outstanding
  total, split by issue, on every run — so the backlog is visible and monotonic, a new
  one cannot be added silently, and the issues it points at get a completion test that
  is measured rather than asserted. Marking a site untriaged buys nothing except
  honesty, which is what makes it safe to have. Spelling an undecided site
  `RefuseWithoutCounter` with a placeholder reason is WORSE than the bare encoder: it
  carries the appearance of a decision and a dead link to prove it.
- **And a tally at zero becomes an assertion, which is the only state it can.** #494
  emptied the backlog — the fleet scheduler's seven arms and the daemon's twelve were
  the last of it — so `worker-refusals-counted` now REFUSES a non-empty one instead of
  printing a number. A count can only be published and watched, and watching is exactly
  how five sites accumulated behind a green check (#447). The third spelling stays
  legitimate and stops being silent: file the issue and relax the assertion in the same
  change. The selftest's untriaged case had to be restated with it, and asserts three
  things rather than one — that the spelling is still RECOGNISED (not reported as a
  bare encoder call), that the site is still tallied and resolved to its issue, and
  that the refusal is the empty-backlog one by its own words. A case asserting only
  "it failed" would pass just as happily if the check stopped understanding
  `RefuseUntriaged` at all.
- **The reason is a forcing function, not a dead field.** Nothing sends it and nothing
  reads it at run time; what it does is make the author answer "would a rise here mean
  something happened" before the call compiles. `StartupPolicyRejection` carries a
  per-row reason for the same purpose. Measured, so the cost argument does not have to
  be taken on trust: at `-O2` a caller with the reason and one without emit
  **byte-identical** bodies and the literal is not emitted at all.
- **It is `rationale` and not `why`.** `CompileCacheWire::RefusedVerb::why` is the text
  a CLIENT is SENT; this one is never sent. The two meet in one expression in
  `CacheProxy.cpp` and `SchedulerProtocol.cpp`, and one word carrying two opposite
  contracts is how a reader comes to transmit the wrong one.
- **The SET of spellings is DERIVED from the header, never restated in the check.**
  This is the rule the first attempt got wrong, and the direction matters: a restated
  list notices a spelling going AWAY — a rename leaves the check hunting something
  nobody calls — and is blind to one ARRIVING. Add `RefuseDeferred` to the allowed
  header and every call site reaching it passes the scan, joins no backlog and asserts
  no claim at all, which is this rule's own failure re-entering through the instrument
  that closed it. `check-script-check-signals.cmake`'s idiom — *the set is read from
  the file, never restated here* — turned on the checker itself.
- **And the check is SEEN to fire.** `worker-refusals-selftest` drives it against seven
  synthetic source trees and asserts each verdict separately — a guard nobody has
  watched refuse is not a guard, and a glob is only worth more than the list it
  replaced if it bites on a file that was never named. Per case rather than
  `WILL_FAIL`, because a check that stopped refusing and one that refuses everything
  are opposite defects a bare inversion reports identically.
- **A default-set check is scanned for cheaply or it is a tax on every run.** Globbing
  `src/` and splitting every file cost 2.9 s on every platform to find matches in ten
  of 413 files. A whole-file `string(FIND)` for the three substrings before the
  line-by-line pass takes it to 208 ms, and is exact rather than heuristic because each
  needle is a strict prefix of the regex it guards.
- **Assert that no OTHER counter moved.** A test checking only the wire code passes
  with every refusal wired to one shared counter — and it passed, on the first run of
  the very test written to prove the two `MalformedFrame` refusals are separate: the
  fixture's "undecodable payload" frame had been shortened without rewriting the
  declared length, so it was refused as TRUNCATED, answered the same code, and moved
  the neighbouring counter. The confusion the split exists to end, inside the test
  written to prove the split.
- **A surface MERGING is how this rule gets undone without anybody writing a bug**,
  and it is the direction the check was blind to. #290 stage 3 retired the dedicated
  compile port; `FrameEndpoint` — the merged listener that replaced it — held five
  refusals encoded with a bare `Wire::EncodeErrorReply`, and two of those had counted
  on the port that went away. So #326's counter and
  `worker_jobs_refused_endpoint_busy_total` stopped moving at a *migration*, on the
  surface that had just become the only one, with no test failing and the operator
  documentation still naming both (#447). The scan could not see the file: it covered
  three, and the listener was not one of them.
- **The check is EXACT, so a file with one uncovered refusal cannot be covered at
  all.** That was a property of the instrument rather than a matter of thoroughness,
  and it decided scope whenever such a file was found: fixing the refusal the ticket
  names and deferring the rest left the scan permanently blind to the file that made
  the omission possible. It is also what made the list unfixable in halves — and #492
  is the exactness kept while the all-or-nothing goes away, since a site nobody has
  decided can now join as `RefuseUntriaged` instead of holding the whole file out.
- **"It cannot fire" is a reason not to count, and its STRENGTH is what decides.**
  #491 settled the cache surface's ten arms, and the interesting half was not the two
  it counted but the six it did not. Two surfaces answer this question oppositely and
  both are right: `CompileCapacity`'s credential rows are minted for arms `AUTH`
  routing makes unreachable *today*, where a shape that reversed it is plausible and
  the dead row buys a signal nobody would otherwise remember to add; the cache's
  equivalents are not, because `AuthRequired() == false` there is a standing decision
  (#287, #290) and its unknown-opcode arm is excluded by the DEFINITION of `FamilyOf`
  rather than by a routing choice. A row that cannot rise is not free — the table's
  whole value is that every row means something — so it is paid where it buys a future
  signal and not where it cannot. **Write the divergence down in both places**: two
  files taking opposite lines on one question, with the reasoning in neither, is an
  inconsistency the next session "fixes" in whichever direction it reads first. And
  prove it rather than asserting it — a claim about ROUTING stops being true without
  anybody editing the sentence that states it, so the sweep is over all 256 opcode
  values, in BOTH directions, since the one-directional half passes for a router that
  sends nothing to the surface at all.
- **The other two reasons not to count**, from the same pass: a rise that would be
  ORDINARY TRAFFIC (the cache's `AUTH` refusal is what a `FASTCACHE_TOKEN` launcher
  gets once per exchange for a whole build — `UnservedReply`'s argument, one surface
  over), and an event ALREADY COUNTED somewhere better placed to see it (a failed
  local write moves `NodeCacheStoreFailures` inside `LocalCache::Store`, where every
  caller is visible and not only the ones that arrived over the wire; a second row
  would count one write twice). Neither is "this refusal does not matter".
- **The endpoint owns WHEN a refusal is answered; the surface owns WHAT, counter
  included.** A listener serving several components cannot know which counter a
  refusal belongs to — a cache `STORE` over the byte budget counted against the
  scheduler names the wrong subsystem, and naming the subsystem is the whole reason
  these are read. So the endpoint asks the owner (`IFrameResponder::RefusalReply` for
  the wire-shared `PrePayloadDecision`, `EndpointRefusalReply` for the decisions only
  this process can make) and encodes nothing itself.
- **A refusal decided before a header exists belongs to the ENDPOINT, and gets its own
  row.** The at-capacity refusal is answered at accept, so it names no verb and the
  router has no input to route it by. Do not contort the router into answering it: a
  router asked a question it cannot have the input for grows a default arm that is
  wrong later. Two categories — verb-owned and endpoint-owned — which also makes it
  *impossible* for the byte-budget and at-capacity refusals to share a counter,
  although they share `EndpointBusy` on the wire and say opposite things to an
  operator. The types keep them apart rather than a comment asking somebody to.
- **Not every refusal is an EVENT, and one that ordinary traffic produces must not be
  counted.** The merged listener answers `UnimplementedVerb` to a verb this node runs
  no component for — and that is what a *healthy* deployment gets: a worker with no
  scheduler refuses every `AUTH` a `FASTCACHE_TOKEN` launcher sends, once per exchange
  for a whole build, and a node with no cache tier refuses every local `FETCH` the same
  way. Counted, the series is a build's traffic and a port scan is invisible inside it
  — which is this rule's own failure reached from the opposite side: a signal nothing
  can be read out of is no better than a counter that never moves. The test is not "is
  this a refusal" but "would a rise mean something happened". Splitting such an answer
  into its ordinary and its alarming halves is a real design question and never a
  by-the-way row.
- **A refusal counter can be a SECURITY signal, and the one that was missing is.**
  `SchedulerRequestsRefusedUnauthenticated` fires only at the pre-payload gate — a
  peer reaching a verb having never authenticated, which is a misconfigured member.
  A peer presenting a token and getting it WRONG was answered `unauthenticated` on
  the wire and moved nothing, so credential guessing against a token-configured
  scheduler was invisible to the exact series whose own documentation tells an
  operator to read zero there as "the port is not being reached". Three outcomes,
  three rows: never presented, presented and wrong, presented and undecodable.

## Text a peer sent is text, or the fleet refuses it

Every string a peer states about itself -- a toolchain fingerprint, an endpoint,
a version, a cluster member id -- is copied into the leader's view of the fleet
and read back out of it by an operator: `/fleet.json`, the page, the charts,
`--cluster-status`, the logs. One byte belonging to no valid UTF-8 sequence made
`/fleet.json` a document a strict parser may reject **for the whole fleet**, not
for the row that carried it (issue #141). RFC 8259 §8.1 requires UTF-8 of JSON
exchanged between systems, and an SVG is XML, which a strict parser refuses
outright rather than drawing with a gap.

- **It is refused where it enters, never repaired by a renderer.** A renderer
  that sanitises is a second place the value is decided, and every *other*
  surface still carries the original. `SchedulerService::Register` refuses the
  registration outright.
- **A cluster member's identity enters by three doors, and the shape of each
  refusal is decided by what that door can do about one** (#159). A peer's beacon
  repeats forever, so it is *filtered*, at `PeerDirectory::NoteBeacon` -- a peer
  this node cannot name is a peer it does not remember, which means no
  permanently-refusable proposal is ever generated. A cluster change is one-shot
  with somebody reading the answer, so it is *refused*, at
  `SchedulerService::Offer` and again in `Cluster::Validate`. And this node's own
  `--node-id` and `--raft-peer` are refused where they are **parsed**
  (`ParseUtf8Text` / `ParseRaftPeer`, #155) -- in front of whoever typed them,
  which is why no startup rule needs to say it again.
- **The wire door is not the flag door, and neither replaces the other.** The
  option table refuses what an operator typed into *this* binary;
  `SchedulerService::Offer` refuses what arrives as a request, from a client built
  before that check existed or from a peer. A cluster entry is applied after it is
  committed, with nobody left to refuse it, so the surface that accepts the request
  is the last place anybody can be told.
- **`Cluster::Validate` carries the rule per VERB, and `RemoveMember`'s row is
  empty.** One rule for every verb alike was written, verified and reverted once
  (#159), because `Validate` governs removal too and a removal's key *is* the
  offending id: a bad member already in replicated state could then never be
  forgotten and would count towards quorum forever, refused by the very check
  meant to keep it out. The empty row is load-bearing and named rather than
  omitted, and a test forgets an unnameable member so that tidying it away fails.
- **A refusal describes the command or the moment, and a proposer of a LIST has
  to know which.** `ConsensusTier::Reconcile` returned on the first refusal --
  right for a moment-shaped one, and the only kind that could arise when it was
  written. A command-shaped one is permanent, so that `return` skipped every
  later proposal *and* `ReconcileQuorum` on every pass forever, with one Info
  line per interval as the symptom: a cluster that silently stops admitting
  anybody. `RefusalSubject` is the distinction, as a table over
  `ConsensusErrorCode` so a code appended later cannot be classified by accident.
  What the beacon door filters, what it deliberately does not, and why nothing
  prints a claim a peer has not proved are `Cluster/` facts and live in
  [`consensus-and-cluster.md`](consensus-and-cluster.md).
- **Refused, not cleaned up, and the reason is per-field.** A fingerprint is
  matched byte for byte, so a worker admitted under a repaired name would match
  nothing and sit in the fleet never being picked. A member id is what every
  later `RemoveMember` has to name, so a repaired one could never be removed by
  the name its operator typed. A repair is the failure that is quiet.
- **All the strings, from a table.** A fourth string added to
  `WorkerRegistration` or to `Cluster::Command` that nobody remembered to check
  is the failure this guards against, and it stays invisible until a peer sends
  one.
- **And the encoders are total anyway.** That is not a second answer to the same
  question -- it is what an encoder owes its own format, exactly as escaping a
  quote is. Entry validation cannot be the whole story: a consensus entry is
  applied AFTER it is committed, so a peer built before the refusal existed can
  still replicate a member id past it with nobody left to refuse. Both escapers
  in `FleetText.hpp` walk one shared decoder so they cannot disagree.
- **Markup carries what XML's `Char` production admits, and that is a rule about
  CODE POINTS, not bytes.** JSON spells every control byte with a legal
  `\uXXXX` escape and puts no hole in its character range at all. XML admits
  only tab, LF and CR below `0x20` and forbids the rest **outright** -- there is
  no reference that makes a NUL legal -- and it stops at `U+FFFD`, so `U+FFFE`
  and `U+FFFF` are excluded while being perfectly good UTF-8. A byte-level check
  cannot see the second case, and a test asserting only "the output is UTF-8"
  passes on a document no XML parser will take. Both halves were missed exactly
  that way, one after the other.
- **The rule is about encoding, not about ASCII.** A fingerprint is opaque by
  design and a node id may be in the operator's own language. Narrowing either to
  ASCII would be a second restriction nobody announced.

## Open work

- **[#143](https://github.com/LASTRADA-Software/fastcached/issues/143)** —
  `/fleet` collects and renders a full snapshot per request and the page carries a
  meta refresh, so tabs left open are steady load on the node that also schedules
  every compile. Deliberately a *measurement* ticket: a TTL buys latency and pays
  in staleness on the one page that exists to be current, and the charts already
  avoid the cost with a validator rather than a lifetime.

- **[#592](https://github.com/LASTRADA-Software/fastcached/issues/592)** — whether the
  fleet scheduler should count a non-member caller in a series of its own. #494 left it
  `RefuseWithoutCounter`, deferring to `SchedulerService::UncountedRefusals`, and that
  service's reason argues against *mixing* the series rather than against counting at
  all — on the surface a credential-guessing client reaches first. Two things have to
  be settled together: the counter belongs in the service's own table beside the
  existing decision, and the refusal has **two** paths (the pre-payload gate and
  `Route` → `Gate`) which are byte-identical on the wire, so counting one alone
  under-reports by an amount that varies with frame size.
