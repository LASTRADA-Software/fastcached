# fastcached - Fast Cache Daemon

A layered C++23 server: a memcached- and Redis-compatible cache daemon, plus a
portable compile cache (`fastcache-cc`) and a distributed compile fleet
(`fastcache-compile-node`). Each layer reaches its collaborators through a narrow
interface, so the whole thing is testable end-to-end against an in-memory
transport, a manual clock and a scripted reactor.

## Project Architecture

```
src/FastCache/
  Core/         Error taxonomy, Clock, HostPort, IRandomSource, Logger, BufferPool,
                Base64, Bytes, Endian, Crc32c, MurmurHash3, Sha256/HMAC, StringHash, Owner,
                Utf8 (the one strict decoder), Compression, WireFrame + WireFields
                (the shared framing), Profiling
  Async/        Task<T>, Cancellation, ResumeOn, SleepUntil,
                InterruptibleSleepUntil, DeadlineTimer, AsyncQueue (MPSC,
                bounded, closable), IReactor (Run/Stop/Submit/Schedule/
                CancelPending) + TestReactor and the platform reactors
                (EpollReactor / IocpReactor / KqueueReactor)
  Net/          ISocket, IListener, IConnector (BlockingConnector for threads
                that may block; PlatformConnector -> Epoll/Kqueue/IocpConnector
                for a reactor thread, sharing ConnectFlow and ReactorDial),
                IAsyncAddressResolver + ThreadedAddressResolver,
                TcpClient (the ONE TCP client), SocketAddress, BlockingSocket,
                the reactor sockets, TLS, InMemoryTransport, HealthProbe,
                IAdmissionControl, IDatagramSocket + UdpSocket/InMemoryDatagram
                and SharedPortDatagram (listen shared, answer private)
  Cli/          UsageDoc (usage text as data) and Options (the one parse loop).
                Dependency-free by design, so fastcache-cc compiles it in rather
                than linking the library
  Cache/        IStorage atomic primitives, CacheEntry, CacheEngine,
                InMemoryLruStorage, CowTreeStorage (CoW B+tree), LayeredStorage
                (L1 LRU over L2 disk), ShardedStorage, TracingStorage
  CompileCache/ PathCanon (absolute<->canonical-token rewriting + the depfile /
                showIncludes grammars), CompileValue, PrefetchGroupManifest
  Consensus/    Raft, split into a pure state machine (RaftNode) and a coroutine
                driver, behind IRaftStorage / IRaftTransport / IRaftStateMachine;
                plus RaftLog, RaftWire, RaftPeerTransport/RaftPeerServer,
                RaftMembership, and RaftClusterHarness (a whole cluster in one
                process, against scripted partitions, loss and restarts)
  Cluster/      DiscoveryService + DiscoveryWire (the LAN beacon and its PSK
                challenge), PeerDirectory, ClusterState + ClusterStateMachine,
                MembershipPolicy — who is a member, WHERE they answer, and the
                settings every member must agree on
  Distributed/  WorkerRegistry, LeaseTable and SchedulerService — the fleet's
                capacity decisions, all pure with respect to I/O; plus FleetView,
                which renders what the leader can see as a page and as JSON
  Protocol/     IProtocolHandler, ProtocolAutodetect, Framing/ByteReader,
                MemcachedText, MemcachedMeta, MemcachedBinary, RedisResp,
                CompileCacheHandler (the 0xFC executor) and CompileCacheWire
                (header-only and dependency-free, shared verbatim by every
                binary)
  Server/       Connection (per-client coroutine), Server, ReactorServerLoop,
                AdminHttpServer (its routes are a table) + AdminCredential
  Platform/     IDaemonHost, ISignalSource, DaemonControls, CpuAffinity,
                HostMemory, HostInfo, ServiceControl (ServiceSpec), Terminal,
                InheritedListener (systemd socket activation),
                Environment (the one place the environment is read), FileTrust,
                NarrowText (what a `char` is on this host, and reading text
                something else wrote)
  Config/       Config, CliParser + CliOptions (the one flag table), ByteSize,
                YamlReader, ConfigReloader, EnvExpand, DefaultConfigPath
  Metrics/      IMetricsSink + AtomicMetricsSink, MetricsCatalog (the counter
                table) and PrometheusFormatter, which renders that table
```

Every executable lives under `src/apps/<name>/` and declares its own target and
install rule there; `src/apps/CMakeLists.txt` holds the app table that gates each
one, so adding an executable is adding a row:

```
src/apps/
  fastcached/               the daemon (FASTCACHED_BUILD_DAEMON, default ON)
  fastcache-cc/             the compiler launcher (FASTCACHED_BUILD_LAUNCHER,
                            default ON) — an sccache-style launcher keying on
                            preprocess + relativized args. Does NOT link the
                            FastCache library; it compiles in a few dependency-free
                            rows (see `_fc_cc_core`)
  fastcache-compile-node/   the compile worker AND the peer service
                            (FASTCACHED_BUILD_NODE, default ON). May also be the
                            scheduler, hold a cache tier, and run consensus — four
                            surfaces, each off unless asked for except the cache
  compile-cache-testclient/ low-level 0xFC protocol probe + cross-depth validation
                            (FASTCACHED_BUILD_TESTCLIENT, default OFF, never
                            installed, but built by the linux and clang-tidy jobs)
  fastcache-bench/          in-process storage micro-benchmarks
                            (FASTCACHED_BUILD_BENCHMARKS, default OFF, never
                            installed, but built by the linux and clang-tidy jobs)
```

Platform service integration and OS packaging live under `packaging/`, which
follows the same table idiom — one descriptor row per installed asset:

```
packaging/
  CMakeLists.txt      the asset install table (source|destination|kind|name|
                      component); exports the config-file list reused by the
                      dpkg conffiles and rpm %config filelists
  linux/              system + user systemd units, sysusers.d/tmpfiles.d, the
                      commented /etc/fastcached/fastcached.yaml, and the DEB/RPM
                      maintainer-script templates (*.in)
  macos/              /etc/paths.d entry, the per-component postinstall templates,
                      the uninstaller, and the installer panes
  windows/            WiX fragment driving --install-service / --uninstall-service
```

`cmake/Packaging.cmake` turns that into `.deb`/`.rpm`/`.pkg`/`.msi` via CPack.

`.agent/reference/source-map.md` carries the same tree with each directory's
rationale kept in full.

Production flow: `main()` -> CLI -> optional YAML -> `ConfigReloader` ->
`CacheEngine` over `InMemoryLruStorage` (or, when `--storage` is set, a
`ShardedStorage` of `LayeredStorage(InMemoryLruStorage, CowTreeStorage)` —
an in-memory L1 over the on-disk B+tree L2) -> `RunReactorServer`. The
reactor (IOCP / epoll / kqueue) multiplexes every connection on its event
loop, so the number of concurrent clients is bounded by memory, not by a
worker count. `--threads` runs that many independent single-threaded
reactors, each pinned to a core, with every connection pinned to one reactor
for its lifetime. On Windows the persistent backend additionally drains the
IOCP reactor from several threads so a blocking page-store `fsync` overlaps
with serving other connections; the disk backend is therefore always wrapped
in a `ShardedStorage` for thread safety.

## The rulebook

`.agent/rules/` holds this project's load-bearing constraints. **Every rule there
has already been a bug**, most of them a silent one — a cache that stops sharing
while every test passes, a service that registers and then cannot work, a series
an operator was told to scrape that was never exported, a fleet that never
distributes anything and goes green anyway.

**Read the matching file before changing code in its area.** The bullets below are
tripwires, not summaries: they are there so a rule fires even in a session that
never opens the file, and none of them carries the reasoning that makes it stick.

> Link these as plain markdown, never as an `@`-prefixed path. Claude Code resolves
> `@` imports recursively out of `CLAUDE.md`, so `@`-importing a rule file would
> pull every one of them back into every session and put the context cost straight
> back.

**[`.agent/rules/compile-cache.md`](.agent/rules/compile-cache.md)** — what the
launcher's cache key is made of. Before `apps/fastcache-cc/`, `CompileCache/`.
- Preprocess with line markers suppressed — `/EP` **alone** on MSVC, never `/EP /P`.
- `/` introduces an option only on a Windows layout; on POSIX it starts a path.
- Only machine-independent dependency paths are hashed, and a path is classified
  by what it *resolves to*, never by its spelling.
- The toolchain headers the key drops are covered by the compiler's *banner*, so every
  driver is asked for one the way it answers. `cl` has no `--version`; bare `cl` is its
  probe, and until it was asked that way every MSVC toolset keyed as the string `cl`.
- A root and the paths a driver emits are reconciled on both sides, or neither.
- Bump `manifest-v*` whenever `objkey-v*` moves. The reverse is not required.
- A compile that writes a second artefact (a module BMI, a PCH) is refused, not cached.
- The compiler identity is the driver AND the target it generates for: `clang-cl`
  takes `-fms-compatibility-version` from whatever MSVC it finds, and that is code
  generation. The **key** folds the target, the **fingerprint** must not.
- Read the `-cc1` line's `-triple`; the `Target:` header three lines above it is
  unversioned, and pinning it changes nothing while looking like a fix.
- An empty triple means the identity is UNCHANGED, so a driver that states nothing
  keeps its keys.
- Discovering a target and stating one are different questions: `gcc` is keyed on
  its target and never handed a `--target=` it does not accept.
- `cc` and `c++` name a policy, not a product — on macOS that is Apple clang — so
  the banner corrects the name. It must never reclassify `clang-cl`, whose banner
  is plain clang's.
- A path a COMPILER wrote is not this process's text: `cl` writes `/showIncludes` in
  the console output code page. Decoded at `RootReconciler::Path`, or the compile is
  not cached.

**[`.agent/rules/distributed-compilation.md`](.agent/rules/distributed-compilation.md)**
— dispatch, workers, the scheduler, the node's tiers. Before `Distributed/`,
`apps/fastcache-compile-node/`.
- The text sent to a worker is **not** the text the key hashed; dispatch
  preprocesses a second time, with `#line` markers.
- A worker is told its input is preprocessed *and* what language it is in; the
  file extension is the last of three answers, never the first.
- Leadership and membership are one `Gate()`, and it runs for every verb — reads
  included.
- Duplicate suppression is asked **before** capacity, or a busy fleet reports
  `NoCapacity` for a key it is already building.
- A lease has three transitions and expiry is the third: the **client** resolves it,
  on every path out of the compile, over a fresh connection. Expiry is the safety net
  for a client that died.
- A resolve answers on liveness, not presence — an unknown token is refused, because
  that is the only place "this job outlived its lease" can be observed.
- A worker being dropped is an **event** (`ExpireStale`), or nothing releases what
  was held against it. A node that restarts inside the heartbeat window is the second
  route to the same pin, and `Register` closes it.
- A discovery layout describes a **directory layout, not a vendor**: Visual Studio
  ships its own clang-cl under `VC\Tools\Llvm`, which the `visual-studio` row walked
  past and the three standalone-LLVM rows never reached — so those builds were cached
  and could never be dispatched. One installation, two rows; `vswhere`'s answer is
  memoized across them, empty answers included.
- `--cache-memory 0` means no tier. Zero is how `InMemoryLruStorage` spells
  *unbounded*, so the flag that turns a cache off once turned its limit off.
- What a node holds back from compiles is what its tier **built**, never what a flag
  asked for — so capacity is derived *below* the tier startup. Which tiers cost RAM
  is a column of `StorageTierTable`, and a present zero is *unbounded*, not nothing.
- A probe that did not RUN is not one that answered nothing, and an identity built on
  one is neither served nor cached. Empty roots are ordinary — several mechanisms
  legitimately have none — so the guard is `exitCode == NotSpawned`, never the count.
  A short include-tree WALK is worse still: it moves nothing the stamp covers, so the
  short digest validates forever. What counts as a gap is what two ends would DISAGREE
  about, so an absent root, a dangling symlink and an undecodable root name are none —
  and keying the first on `error_code` rather than on the resolved `file_type` refuses
  every machine without `/usr/local/include`.
- A cache is per node; the registry is keyed per `(fingerprint, endpoint)`. Summing
  a cache field across `LiveWorkers()` counts one machine once per toolchain.
- `AvailableSlots` folds four ceilings into one; `SlotCeilingsFor` is the same
  arithmetic with each named, and a tie names the earlier limit in enumerator order.
- A heartbeat age is a duration on a report, never a `TimePoint` on `WorkerInfo` —
  a raw instant invites `steady_clock::now()` and breaks every `ManualClock` test.
- A CoW store file is claimed exclusively at `Open` and a second opener is refused
  by name (`InUse`), never left to interleave meta-page writes. `flock`, never
  `fcntl` — an fcntl lock is per process and a second store inside one would take
  it again and succeed.

**[`.agent/rules/consensus-and-cluster.md`](.agent/rules/consensus-and-cluster.md)**
— Raft, discovery, membership. Before `Consensus/`, `Cluster/`.
- The pre-shared key never travels in a beacon. It appears only inside an HMAC
  over a nonce *this* node chose, and the MAC covers the `(node, endpoint)` pair.
- A proof only ever answers a challenge this node issued, and the nonce is spent
  whatever the outcome.
- Discovery never changes membership: it reports who proved the key and where.
- `RaftNode` reads no clock, opens no socket and draws no randomness of its own.
- A snapshot is durable before it is acknowledged, and the configuration travels
  inside it.
- A seeded draw must be identical on every standard library — `UniformInRange`,
  never `std::uniform_int_distribution`.
- A node being admitted must never have bootstrapped a cluster of itself, so
  `RaftConfig::members` may legally be **empty** and `--raft-join` is what starts a
  machine that way. Who a node dials is not who it counts.
- The quorum follows the replicated state, one change at a time, additions before
  removals — and a member is never counted before every node can dial it.
- Absence from `ClusterState` is not removal: a `--raft-peer` member is in the
  configuration and in no state, so a member named in the bootstrap set is never
  proposed for removal, and a node given no bootstrap set proposes none at all.
- A cluster that has elected is not one that has formed. Until every member
  attaches, pre-vote refuses nothing and any stall re-elects — assert leadership
  stability only after formation, and log the term or nothing explains it.
- A leader and a follower stamp the same link half a round trip apart, so a shared
  window never bought a shared answer. A non-leader decides a pre-vote from its own
  `_knownLeader` and election deadline, never from a timestamp.
- "A leader spoke" arrives at two handlers, and every rule about it belongs in
  both: `OnInstallSnapshot` is `OnAppendEntries` speaking, membership guard and
  candidate demotion included.

**[`.agent/rules/wire-and-protocol.md`](.agent/rules/wire-and-protocol.md)** —
framing, the auth gate, sockets, dialling and coroutine lifetime. Before
`Protocol/`, `Net/`, `Async/`.
- A frame declares its own length, so a rejection is a **reply** and a
  resynchronization — never a close.
- Which verbs are reachable before authentication is a *column of the table*, and
  the gate runs before the payload is buffered.
- A pre-auth verb carries its own payload ceiling, `static_assert`ed so a new one
  cannot reopen the hole by omission.
- `Net/` must not depend on `Core/`. `Async/` travels with it, plus three named
  dependency-free leaf headers; `ctest -R net-boundary` enforces the table.
- `CompileCacheWire.hpp` must stay header-only and dependency-free — the launcher
  does not link `FastCache`. It therefore carries cache tiers **positionally**,
  which makes `StorageTier`'s enumerator order a wire contract.
- SIGPIPE is suppressed per socket, never process-wide: an ignored disposition is
  inherited across exec.
- A listening socket claims its address exclusively — `SO_EXCLUSIVEADDRUSE` on
  Windows, where `SO_REUSEADDR` lets a second process take a port already being
  served. Sharing a port is `ReusePort::Yes`, and only that.
- A struct a decoder returns **by value** must not borrow from the bytes it decoded:
  `Decode(Encode(x))` is the obvious spelling and is a use-after-free the moment one
  member becomes a view. A `*View` type borrows and says so; anything else owns.
- There is exactly one TCP client, `Net/TcpClient`. Do not write a second.
- A synchronous dial spends a thread the caller does not own — a reactor thread
  dials through `PlatformConnector`, never `BlockingConnector`.
- `Close()` can be the last thing that runs on a socket, so it must touch no
  member after it completes an awaitable.
- A wait nothing can cancel is a coroutine frame nobody frees: park through
  `Schedule`/`CancelPending`, and bound any sleep a peer can move the deadline of.
- A missing keyspace event has two ends — the tier that never named the victim and
  the observer that never published it. Check both before changing either.
- A reclaim is reported **before** the call that caused it: `ADD` on a lapsed TTL
  names the same key twice, and the wrong order tells a subscriber a live key is gone.
- In a layered cache no single tier's eviction is total, so none is reported. An
  expiry is, because both tiers hold the same TTL.
- A reclaimer nothing constructs is the bug it was written to fix: `PurgeExpired`
  was correct and tested, and had no production caller at all. Assert the wiring.
- The expiry cycle sweeps `engine.Storage()` — the notifying decorator. One layer
  down it frees the bytes and publishes nothing, and every tier test still passes.
- One cycle per daemon, on reactor 0; its reclaim ceiling sits below
  `ReclaimLog::DefaultCapacity` or the sweep drops the events it runs to produce.
- A bounded sweep resumes from a cursor and `ShardedStorage` rotates its starting
  shard, or everything past the first budget never expires. That cursor outlives
  the call, so a tier gets exactly one erase point.
- `--expiry-interval=0` disables the cycle (a coroutine that *ends*);
  `--expiry-scan=0` is `PurgeBudget`'s spelling of *no ceiling* and is refused.
- On disk a read may not reclaim and a write must: `Get`/`Peek` can hold a shared
  lock, every write verb holds the exclusive one. Reporting without erasing is
  worse than neither — the record stays and fires `expired` again.

**[`.agent/rules/platform-service-and-config.md`](.agent/rules/platform-service-and-config.md)**
— service registration, config lookup, the CLI table. Before `Platform/`, `Config/`,
`packaging/`.
- A service to register is a `ServiceSpec`; what it runs as is part of it, and an
  empty `serviceAccount` means **root**.
- `--install-service` registers the *command-line* config, never the merged one.
- An install is judged by the **startup** rules as well as the install-time ones:
  a registration replays its command line forever, so refuse it while somebody is
  watching.
- A refusal that depends on nothing but the parsed configuration belongs in a table
  — the option row for a grammar, `StartupPolicyRejection` for a cross-flag rule —
  never in the tier that happens to need it. An install returns before any tier
  exists.
- Whatever reaches a supervisor must survive this project's own parser round trip
  — including the flags the *installer itself* adds, which are the daemon's only
  when the spec names an application.
- A config the operator named is strict; one the daemon found is skipped when
  absent, unreadable or untrusted.
- A machine-wide config is obeyed only when only an administrator could have
  written it (`Platform/FileTrust`).
- Every flag is one row of `CliOptions()`, which drives parsing **and** help.
- Which flags carry text *other machines* will read is a column of that table
  (`ParseUtf8Text`). `--cluster-forget` is deliberately out of it, or a bad member
  becomes unremovable; so is every path-valued flag, and the compiler half of
  `--toolchain`.
- A value parser cannot know which flag it was reached through, so it names none and
  `ApplyOneOption` stamps the row's own spelling.

**[`.agent/rules/storage.md`](.agent/rules/storage.md)** — the on-disk format and
converting a store. Before `Cache/CowTreeStorage`, `CowTree/`.
- An old store is `UnsupportedFormatVersion`, never `Corrupt` — the code is what
  monitoring sees, and `Corrupt` is what makes somebody delete a healthy cache.
- A format is convertible exactly as long as its reader is in `RecordFormats()`.
  Bumping the version without adding a row is the decision to discard every store.
- "No marker" is an INFERENCE. Validate every record before writing any of them:
  a store this build cannot read must come back unmodified.
- The conversion commits in slices — one transaction inflates the file by a page
  per record per level, permanently — and each slice must `Flush()`, or the freed
  pages are not reclaimable and the slicing buys nothing.
- Each slice records its resume point in its own transaction, so an interrupted
  run is refused by name and finished by re-running it.
- A tree walk is bounded by `PageCount()`, and must not overlap a commit.

**[`.agent/rules/metrics-and-observability.md`](.agent/rules/metrics-and-observability.md)**
— counters and scrape surfaces. Before `Metrics/`, `/metrics`, `/healthz`.
- A counter is a row in `MetricsCatalog`, `static_assert`ed to cover every
  enumerator; the renderer walks the table rather than a hand-picked list.
- A refusal's wire code and its counter are one row — one fact, two audiences.
- Text a peer sent is text, or the fleet refuses it: one byte that is not UTF-8
  makes `/fleet.json` unparseable for the **whole** fleet. Refused where it enters
  (`SchedulerService::Register`) and never repaired by a renderer — and the
  encoders are total anyway, because a consensus entry is applied after it is
  committed, with nobody left to refuse it. Markup's rule is XML's `Char`
  production over **code points**, not bytes: `U+FFFF` is valid UTF-8 and
  illegal in an SVG.
- Absent is not zero: a process with no cache reports no cache, and *names* the
  field to do it.
- A duration is a `_sum`/`_count` pair, never a gauge.
- A merged snapshot is one tier's answer standing in for all of them:
  `SnapshotTiers()` reports the split, the `tier` label comes from a table, and a
  tier the cache does not have renders no line at all.
- The fleet page is served by the leader; anyone else answers `503` **naming** the
  leader, never a redirect and never a link to an address it guessed.
- Its columns are a table both renderers walk, one spelling serving as header and
  JSON key. Absent renders `null`/`–` at the **cell**, and a tier no member runs
  gets no column.
- A fleet total is computed over `NodeReports()`, never over registry entries.
- A node's version is compiled in, rides REGISTER's *nested* capacity record (whose
  arity is variable) rather than its top level (whose arity is exact), and is
  refreshed on re-registration — a restart is what an upgrade looks like.
- A history stores a counter **raw**; a rate is the delta at render, taken only
  between adjacent *present* buckets. A restart is then a gap, not a spike.
- Sampling runs only while this node **leads**, and no state of the history file
  may keep a node from starting.
- A chart served as its own resource inherits nothing from the page, so it carries
  its own palette and theme is part of its URL — and that URL carries no
  cache-buster, or the `304` never fires.
- A `304` carries its validators and no content; whether a body is allowed is a
  property of the **status**, not a flag each route sets.
- An unknown `range` is refused, an unknown `theme` is not: refuse where a silent
  substitution would mislead, default where it cannot.
- A stacked area is drawn **top band first**, or translucent fills multiply into a
  colour belonging to no series.
- A `<circle>` is not path data. One `<` in an attribute value makes a browser refuse
  the whole SVG, and the chart is then a broken image behind a 200. A run of one
  reading is a dot whether the shape is filled or not — closing it gives a shape with
  no width that draws nothing and still claims the series was observed. Neither is
  visible in a test that renders dense data: gaps are what a live dashboard has.
- The dashboard credential is its own file, never `--requirepass`; a non-loopback
  bind without one is a startup refusal, and TLS does not substitute for it.
- Plain HTTP is a supported way to serve the admin surface. TLS is on by naming
  material or by asking for material to be made (`--tls-self-signed`) — never a
  bare boolean, and the two spellings are refused together.
- A generated certificate encrypts but does not identify: its fingerprint is
  logged because that is all an operator can compare, and its subject names decide
  whether any client accepts it at all.

**[`.agent/rules/packaging-and-release.md`](.agent/rules/packaging-and-release.md)**
— packaging, versioning, cutting a release. Before `packaging/`, `cmake/Packaging.cmake`,
`cmake/Version.cmake`, the release job.
- The git tag is the only version source. `version.txt` must never come back —
  `ctest -R repository-hygiene` fails at `git add` time if it does.
- The resolved version stays a bare numeric `X.Y.Z`; suffixes live on the string.
- The payload is rooted at `/`, not `/usr`; third-party `install()` rules are excluded.
- Neither a `.pkg` nor an MSI has a conffile mechanism — only a `.default` ships.
- Every `build.yml` checkout that could configure passes `fetch-depth: 0`, and the
  release job's asset list stays the **last** key of its `with:` mapping.

**[`.agent/rules/build-and-toolchain.md`](.agent/rules/build-and-toolchain.md)** —
what differs between compilers, standard libraries, hosts and tool versions.
- Run `scripts/local-gate.sh` before pushing. One configuration is not the gate.
- A `char` is UTF-8 here, at run time and at compile time: every Windows executable
  declares the UTF-8 process code page and MSVC gets `/utf-8`. Converting one
  boundary instead would leave `path`, `CreateProcessA` and `getenv` on the legacy
  page.
- `std::filesystem::path`'s narrow constructor THROWS on such a host for bytes that
  are not UTF-8 — before any `error_code` overload runs. `PathFromNarrowText` is the
  one `catch` in this tree.
- Where `clang-debug` will not build, get ASan from GCC (`-fsanitize=address` alone —
  UBSan breaks the option tables' constexpr checks) and run the **whole** suite: a
  freed block nothing disturbs reports nothing.
- Run clang-format and clang-tidy **at the version CI pins**, in a build directory
  of its own; `PATH` resolving to an older binary reports clean in the way that
  means nothing.
- Never silence clang-tidy with `NOLINT` — fix the source.
- A return type is not part of a function's mangled name on Linux, so two
  functions differing only in return type silently collide.
- `cmake/portable/CompileCache.cmake` stays stock-CMake-only and must never fail
  a configure.
- A sanitizer that is on in the cache is not one that is on in the build — a tool
  that silently does nothing is worse than one that is visibly off.
- `clang-format -i` at any version but the pinned one silently reformats code the
  pinned one already accepted; run an older binary as `--dry-run` only. Both pinned
  tools ship on PyPI (`pip download clang-format==<v>` / `clang-tidy==<v>`), so "the
  distro only has an older one" is not a reason to use it. An older clang-tidy is
  worse than a laxer one: it is *silent* about checks that do not exist in it yet.
- A clang-tidy sweep that cannot prove the tool ran is worth nothing and reads like
  success — `scripts/tidy-sweep.sh` canaries it first and treats a failure to
  execute as fatal, never as "no findings".
- Every `bool` and byte-wide enum in a config struct lives in one run: one between two
  8-aligned members costs seven bytes, and clang-tidy's padding budget fails the build.
- A table indexed by an enumerator is `EnumTable<Enum, Row>` + `RowsInEnumeratorOrder`.
  A length anchored on an enumerator by name is a guard that fires only when
  nothing is wrong.
- Coverage is Clang source-based, never gcov: ~2000 Catch2 cases are ~2000
  processes, and gcov's shared `.gcda` races them. `%8m`, not `%p`. `*_test.cpp`
  sits next to the implementation, so a report that counts it measures the tests
  testing themselves. A compiler cache and coverage cannot be combined — a
  replayed object's embedded mapping names the tree it was built in.

**[`.agent/rules/testing.md`](.agent/rules/testing.md)** — how tests are registered
and what they may assume.
- Every wait is bounded and says what it waited for.
- A script-driven test naming more than one executable is registered in
  `src/tests`, not beside a binary.
- Tests allocate their ports per run rather than fixing them — from **below** the
  kernel's ephemeral range, and remembered, because a connect probe cannot see a
  port already held as an outbound connection's local endpoint.
- `Unwrap(x)` after `REQUIRE(x.has_value())` for `std::optional`; a bare `*x` is a
  build failure.
- A Catch2 case name may not begin with `-`. CTest passes it as an argument, so
  `--help ...` printed usage and reported a pass for a case that never ran.
- A scratch directory comes from `src/tests/ScratchPath.hpp`. A per-process
  counter is not unique — `catch_discover_tests` gives every case its own
  process, and the suite runs in parallel.

## Issues and pull requests

Labels here have teeth: a pull request carrying no `type/` label **fails a
check**, because that label decides which section of the generated release notes
the change lands in (`.github/release.yml`), and nothing downstream can recover
it afterwards. CI derives `area/` and `os/` from the changed paths and reads
`type/` from a conventional-commit title when there is one — a prose title, which
most of this repository's are, means setting the label by hand.
[`CONTRIBUTING.md`](CONTRIBUTING.md) carries the taxonomy and the reasoning.

Deferred work is a GitHub issue linked from the matching rulebook file's
`## Open work` section, never a residual recorded only in prose.

## Design Patterns & Principles

### Error handling: `std::expected<T, E>`
Prefer `std::expected<T, E>` for fallible API surface. The error taxonomy
is split: `NetError`, `ProtocolError`, `StorageError`, `ConfigError`.
Chain monadically with `and_then`, `or_else`, `transform`,
`transform_error` rather than nested `if`s. Reserve exceptions for
programmer errors (precondition violation, contract misuse).

### Dependency injection
**This is a load-bearing principle, not a nice-to-have.** Anything that
touches I/O, time, randomness, the filesystem, the network, or any other
ambient/global resource is reached through an interface — never through a
concrete type, a singleton, or a free function with hidden state. The
existing seams are `IClock`, `IReactor`, `ISocket`/`IListener`,
`IStorage`, `ILogger`, `IDaemonHost`, `ISignalSource`,
`IAdmissionControl`, `IMetricsSink`. Collaborators are passed in (usually
by reference or `unique_ptr` at construction), so every layer can be
exercised in isolation: tests substitute deterministic fakes
(`ManualClock`, `TestReactor`, `InMemoryTransport`, `NullLogger`,
`CapturingLogger`, `ScriptedSignalSource`) and the whole server runs
end-to-end without a real socket or a real clock.

When you add a component that does I/O or depends on the environment,
**define the interface first and inject it** — do not reach for the
concrete type directly. If you find yourself wanting a global, a `static`
mutable, or a direct `::time()`/`::read()`/`new ConcreteThing` call in
business logic, that is the signal to introduce (or reuse) a seam instead.
Deviate from this only with a *strong, explicitly stated* reason (e.g. a
genuinely pure leaf computation with no environment coupling); the default
answer is "inject it".

### Data-driven design
**Behaviour is described by data; code interprets that data.** This is
equally load-bearing and goes well beyond "no magic numbers". The aim is
that adding a flag, a protocol verb, a storage backend, or an error code
is a matter of *adding a row to a table*, not editing logic scattered
across the codebase. Concretely:

- **One source of truth per concept.** The CLI flag table is data; the
  storage-record layout is documented and derived in one place; the
  per-DBMS / per-protocol dispatch lives in a single switch each. There is
  exactly one place to change when the concept changes.
- **No naive, hand-rolled repetition.** If two branches differ only by a
  value, lift the value into a descriptor/table and write the logic once.
  Copy-pasted blocks that diverge only in constants, names, or types are a
  defect — replace them with a data table the code iterates over, or a
  small generic helper.
- **Built for extension.** Prefer designs where the next case
  (flag, verb, backend, metric, signal) is a new table entry or a new
  interface implementation, not a new `if`/`else` arm threaded through
  existing functions. Open for extension, closed for invasive modification.
- **Tables over conditionals.** A `switch`/`if` ladder that mirrors a fixed
  set of named things is usually a table in disguise; express it as data
  (a descriptor array, a lookup map, a dispatch table) and drive it with a
  range-based loop or `std::ranges` pipeline.
- **A table indexed by an enumerator is `EnumTable<Enum, Row>`, guarded by
  `RowsInEnumeratorOrder`** (`Core/EnumTable.hpp`). The enum states its own count
  with a trailing `Last`, the table takes its extent from that, and one
  `static_assert` checks the extent and every row's position. Never anchor the
  length on an enumerator by name — that guard fires only when nothing is wrong.
  See [`.agent/rules/build-and-toolchain.md`](.agent/rules/build-and-toolchain.md).

As with DI, **adhere to this unless there is a very strong, explicitly
justified reason not to.** When in doubt, ask: "if a sixth case showed up
tomorrow, how many places would I edit?" If the answer is more than one,
the design is not data-driven enough yet.

### RAII for resource handles
Sockets, listeners, log files, coroutine handles — every resource is
owned by an RAII wrapper. `PooledBuffer` returns to its `BufferPool` on
destruction; `Task<T>`'s `Awaiter` takes ownership of the coroutine
handle on construction so the temporary `Task` cannot tear the coroutine
down across a suspend point.

## C++ Coding Guidelines

### Baseline (general C++23)
- **Data-driven design (non-negotiable)** — describe behaviour as data and let code interpret it. No hard-coded magic values; no copy-pasted branches that differ only by a constant/name/type; new cases should be a new table row or descriptor, not a new hand-written `if`. Prefer tables/descriptors and `std::ranges` over conditional ladders. See the "Data-driven design" principle above; deviate only with a strong, stated reason.
- **Dependency injection (non-negotiable)** — reach every I/O / time / randomness / filesystem / environment dependency through an injected interface, never a singleton, global, or direct concrete call. Define the seam first, then inject it. See the "Dependency injection" principle above; deviate only with a strong, stated reason.
- **Doxygen** on every new public function (params, return), class, struct, and member:
  ```cpp
  /// Short description.
  /// @param name Description.
  /// @return Description.
  ```
- **`const` correctness** throughout (refs, pointers, member functions).
- **C++23 features** — `constexpr`, `std::ranges`, `std::format`, `std::expected` and its monadic methods (`and_then`, `or_else`, `transform`, `transform_error`).
- **C-style loops are forbidden.** Use range-based `for`, `std::views::iota`, and other range views for generation/transformation.
- **`std::span`** for arrays and contiguous sequences.
- **`auto` type deduction** for readability; **structured bindings** for tuple-like returns.
- **Run the local gate before pushing** — `scripts/local-gate.sh`: clang-format at
  the pinned version, then `clang-debug` and `gcc-release`. The default agent
  preset is one compiler at `-O0`; CI is four more, and defects invisible below a
  release build or a second standard library are why the script exists. See
  [`.agent/rules/build-and-toolchain.md`](.agent/rules/build-and-toolchain.md).
- **`clang-format` and `clang-tidy` after every change — at the version CI pins**
  (`$CLANG_TOOLS_VERSION` in `.github/workflows/build.yml`). Successive LLVM
  releases disagree with each other, so a tree clean under whichever binary is on
  `PATH` can still be rejected. Name the version explicitly and use a build
  directory of its own; the `clang-debug` preset is **not** that sweep.
- **`clang-tidy` reports must be fixed at the source.** Never silence with `NOLINT` — address the underlying issue. The `clang-debug` preset enables `clang-tidy` automatically, at whatever version `PATH` resolves to — see the bullet above for why that is not the same as the one CI enforces.
- **No `g_`-prefix on globals either — and the rule lives in `.clang-tidy`, not only here.** A file-scope or `thread_local` name is spelled like any other name of its kind: `CamelCase` if it is a constant, `camelBack` if it is mutable. There is no "forbid this prefix" option in `readability-identifier-naming` (its `...Prefix` keys only ever *require* one), so the `GlobalVariableCase`/`GlobalConstantCase`/`StaticVariableCase` rows are what reject `g_foo` — and with `WarningsAsErrors: "*"` that is a build failure rather than a review comment. A function-local `static` is `camelBack` whether or not it is `const`: `StaticConstantCase` is left unset precisely so a local constant falls back to that, which keeps `g_` rejected there without demanding PascalCase for locals that are `static` only for their lifetime. The prefix is a substitute for a naming convention rather than one, and it makes ambient state read as normal; if a bare name looks wrong at the call site, that is the "inject it" rule above telling you something.
- **No `k`-prefix on identifiers.** Do not use the Google-style `kFoo` prefix for constants, enumerators, or any other symbol — it violates the project `.clang-tidy` naming convention. Use `Foo` (PascalCase) for constants/enumerators and `foo`/`fooBar` for locals and members instead.
- **All changes covered by unit tests.** Aim to **increase** coverage with every PR.
- **No raw owning pointers.** Use `std::unique_ptr` / `std::shared_ptr` for ownership; RAII for resources.
- **No new third-party dependencies** without strong justification.

### Project-specific additions
- Public headers must be **self-contained** (compile standalone, no PCH dependency).
- Public symbols live in the `FastCache` namespace.
- Mark `std::expected`-returning APIs `[[nodiscard]]`.
- Prefer `std::expected<T, SomeError>` over throwing on the public API surface.

## Building

Line endings are LF everywhere, enforced by `.gitattributes` (`* text=auto eol=lf`)
rather than by each developer's `core.autocrlf`. A CRLF `*.sh` does not misbehave —
it fails to start at all.

CMake presets live in `CMakePresets.json`. Common entry points:

```sh
# Clang Debug with PEDANTIC + ASan + UBSan + clang-tidy (the default agent preset; Linux + macOS)
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug

# Linux — GCC Debug
cmake --preset gcc-debug && cmake --build --preset gcc-debug

# Linux — Coverage. Needs llvm-profdata/llvm-cov at the SAME major version as
# clang; the `coverage` target runs the whole suite under instrumentation and
# writes out/build/clang-coverage/coverage/{html,coverage.lcov,percent.txt}
cmake --preset clang-coverage
cmake --build --preset clang-coverage
cmake --build --preset clang-coverage --target coverage

# Linux — sanitizer-only presets
cmake --preset clang-asan-ubsan
cmake --preset clang-tsan

# Linux/macOS — RelWithDebInfo + Tracy profiler (.agent/guides/profiling-tracy.md)
cmake --preset clang-tracy
cmake --build --preset clang-tracy

# Windows — MSVC CL Debug (requires VCPKG_ROOT in env)
cmake --preset cl-debug
cmake --build --preset cl-debug

# Windows — clang-cl Debug
cmake --preset clangcl-debug
cmake --build --preset clangcl-debug
```

`PEDANTIC_COMPILER_WERROR=ON` is the default for Windows presets — warnings break the build, fix them at the source.

`USE_COMPILER_CACHE` (default ON, `cmake/portable/CompileCache.cmake`) fronts the
compiler with `fastcache-cc` when it is on `PATH` and a daemon answers — at
`127.0.0.1:6674`, or wherever `FASTCACHE_ADDR=host:port` points. Configure *proves*
the cache works by compiling one tiny file through it, because a launcher that
cannot reach its daemon still compiles fine and would otherwise cost every TU a
failed connect in silence. Falls back to `sccache`. With either launcher active the
module scan and PCH are turned off and MSVC debug info is forced to `/Z7`, because
a hit reproduces only the object file. Full behaviour, including
`FASTCACHE_AUTO_INSTALL`, is in
[`.agent/rules/build-and-toolchain.md`](.agent/rules/build-and-toolchain.md).

## Testing

Catch2 tests live next to the implementation files, so `Foo.cpp` has a `Foo_test.cpp`. A `test_main.cpp` serves as the entry point.

Not every test is a Catch2 case: script-driven tests are registered in
`src/tests/CMakeLists.txt`, the `smoke`-labelled ones reporting a missing
prerequisite as skipped. `ctest -R repository-hygiene`, `ctest -R net-boundary` and
`ctest -R test-name-hygiene` need no daemon, socket or compiler and run in the
default set.

The rules that have each already cost a debugging session — bounded waits, where a
script-driven test must be registered, per-run port allocation, and the shared
`Unwrap` helper — are in [`.agent/rules/testing.md`](.agent/rules/testing.md).

## Releasing

Cutting a release is pushing a tag; CI runs the entire suite against the tagged
tree and drafts a GitHub release, and a human publishes it with `/publish-release`.
The steps and the constraints are in
[`.agent/rules/packaging-and-release.md`](.agent/rules/packaging-and-release.md).

## Profiling

Tracy instrumentation is opt-in (`TRACY_ENABLE`, default OFF) and collapses to
`(void) 0` when off. Instrument through the `FC_*` macros in
`FastCache/Core/Profiling.hpp`, never Tracy directly — and never let
`FC_ZONE_SCOPED*` straddle a `co_await`. Building the profiling daemon, adding
zones and analysing a capture:
[`.agent/guides/profiling-tracy.md`](.agent/guides/profiling-tracy.md).
