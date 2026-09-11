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
                SecureBytes (the one zeroing primitive, and the allocator credentials live in),
                Utf8 (the one strict decoder), Compression, WireFrame + WireFields
                (the shared framing), Profiling
  Async/        Task<T>, Cancellation, ResumeOn, SleepUntil,
                InterruptibleSleepUntil, DeadlineTimer, AsyncQueue (MPSC,
                bounded, closable), IExecutor (the one thing ResumeOn needs)
                with ThreadPoolExecutor for work that BLOCKS, IReactor
                (an IExecutor plus Run/Stop/Schedule/CancelPending)
                + TestReactor and the platform reactors
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
                capacity decisions, all pure with respect to I/O; FleetSample
                (the slot vocabulary and IFleetHistorySink, so the scheduler's
                header carries no file format), FleetHistory (three rings, eight
                views, one file envelope) and FleetNodeHistories (what every other
                machine handed over); plus FleetView and FleetChart, which render
                what the leader can see as a page, as SVG and as JSON
  Protocol/     IProtocolHandler, ProtocolAutodetect, Framing/ByteReader,
                MemcachedText, MemcachedMeta, MemcachedBinary, RedisResp,
                CompileCacheHandler (the 0xFC executor), CompileCacheWire
                (header-only and dependency-free, shared verbatim by every
                binary) and SurfaceRefusal (the three ways a 0xFC surface
                refuses: counted, decided-not-to, not-yet-decided)
  Server/       Connection (per-client coroutine), Server, ReactorServerLoop,
                AdminHttpServer (its routes are a table) + AdminCredential
  Platform/     IDaemonHost, ISignalSource, DaemonControls, CpuAffinity,
                HostMemory, HostInfo, ServiceControl (ServiceSpec), Terminal,
                InheritedListener (systemd socket activation),
                Environment (the one place the environment is read), FileTrust,
                LocalAddresses (which addresses are THIS machine's, behind an
                interval-refreshed oracle), NarrowText (what a `char` is on this
                host, and reading text something else wrote)
  Config/       Config, CliParser + CliOptions (the one flag table), ByteSize,
                YamlReader, FileOptions (an option table applied from a config
                FILE, through the same appliers argv reaches), ConfigReloader,
                EnvExpand, DefaultConfigPath
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
for its lifetime. The disk backend is always wrapped in a
`ShardedStorage`, whose per-shard mutex serialises access: `main.cpp` wraps
whenever more than one thread can reach the storage, and the persistent
backend is one of four conditions that say so — the others being an explicit
multi-shard layout, the reactor running on more than one thread, and the
metrics endpoint, whose `fc-admin` thread calls `engine.Snapshot()`
concurrently with the reactor. That third one is the DEFAULT rather than an
opt-in: `--threads` unset means `hardware_concurrency()`, so on any multi-core
host the wrapper is on without anybody asking for it, and reading it as
"`--threads` above one" describes a single-threaded default this daemon does
not have.

**No completion port is ever drained from several threads**, on Windows or
anywhere else. `IocpReactor.hpp` says that migrates a coroutine across threads
and is unsafe, and `RunMultiReactorWindows` runs one thread per reactor exactly
as the POSIX path does. This paragraph claimed the opposite until
[#896](https://github.com/LASTRADA-Software/fastcached/issues/896), and a wrong
sentence HERE is worse than a stale one twice over: this is the document a
session reads FIRST and is told to obey, so it licenses the defect, and it
changes how a reader grades a concurrency bug. The one
`ThreadPoolExecutor { 1 }` on each of the three serving paths runs the EXPIRY
SWEEP; it drains no completion port and overlaps no `fsync` with anything.

## The rulebook

`.agent/rules/` holds this project's load-bearing constraints. **Every rule there
has already been a bug**, most of them a silent one — a cache that stops sharing
while every test passes, a service that registers and then cannot work, a series
an operator was told to scrape that was never exported, a fleet that never
distributes anything and goes green anyway.

**Read the matching file before changing code in its area.** The bullets below are
tripwires, not summaries: they are there so a rule fires even in a session that
never opens the file, and none of them carries the reasoning that makes it stick.

**So a rule landed in `.agent/rules/` without a bullet here fires in no session that
does not open its file** — which is the population the rule was written for. It has
shipped that way (#355), caught by a review rather than by the author, and the reason
it is easy is that writing the rule feels like the work.

**And the converse governs this file's size: a bullet that has grown a derivation is
a rule nobody finishes reading.** Reasoning belongs in the matching rules file, where
it can be as long as it needs to be; what belongs here is the sentence that makes the
rule fire and the pointer to where the argument lives. A tripwire that has acquired
measurements, ticket archaeology or a counter-argument has stopped being one.

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
  driver is asked for one the way it answers: `cl` has no `--version`, and bare `cl` is
  its probe.
- A root and the paths a driver emits are reconciled on both sides, or neither.
- Bump `manifest-v*` whenever `objkey-v*` moves. The reverse is not required.
- A compile that writes a second artefact (a module BMI, a PCH) is refused, not cached.
- The compiler identity is the driver AND the target it generates for. The **key** folds
  the target, the **fingerprint** must not.
- The FINGERPRINT folds the driver's argument GRAMMAR (`DriverGrammarName`) — which is
  which spellings the driver can READ, a different question from the target. A NAME,
  never the enumerator's value, and the grammar is in the cache file NAME as well as in
  the digest.
- Read the `-cc1` line's `-triple`, never the unversioned `Target:` header. An empty
  triple means the identity is UNCHANGED, so a driver that states nothing keeps its keys.
- Discovering a target and stating one are different questions: `gcc` is keyed on its
  target and never handed a `--target=` it does not accept.
- `cc` and `c++` name a policy, not a product, so the banner corrects the name — and it
  must never reclassify `clang-cl`, whose banner is plain clang's.
- The banner and the target triple come from ONE `-###` spawn on a `ClangDriverLine`
  driver. **No fingerprint bump rides with that because `ctest -R banner-probe-identity`
  asserts the two spellings are byte-identical; deleting that check reopens the bump
  question in silence**, and a driver found disagreeing is a finding, not a check to adjust.
- A path a COMPILER wrote is not this process's text: `cl` writes `/showIncludes` in the
  console output code page. Decoded at `RootReconciler::Path`, or the compile is not cached.
- The `/showIncludes` MARKER is a canonical form exactly as `<SRCROOT>` is: the LAUNCHER
  normalizes its own prefix to `IncludeNoteMarker` before storing and restores this build's
  after localizing, so the stored bytes are locale-free and no server changed. The restore
  runs AFTER the stale-hit guard, and recognition is anchored — leading blanks only.
- Reading `/showIncludes` and WRITING it are different questions and must not be
  consolidated: `RenderShowIncludes` takes the marker as a REQUIRED, undefaulted parameter
  and spells no literal of its own.
- A compiler with debug info on records the WORKING DIRECTORY, which is on no command line,
  so no key can relativize it and a hit replays an object naming the producing checkout.
  `-fdebug-prefix-map` closes it on ELF and on NEITHER COFF driver; that residue is an
  accepted cost, not open work.
  - Never `-ffile-prefix-map`, and no table row for it or `-fmacro-prefix-map` either:
    both rewrite `__FILE__` INTO the text the key hashes.
  - The key relativizes the rule's HEAD (`PathValueRole::PrefixMap`, a table COLUMN rather
    than a branch) and leaves the replacement literal. The build-tree rule is mapped LAST.
    Relative is not checkout-independent, and a root with a SPACE is not mapped at all.
    `ctest -R debug-prefix-map-rules`.
  - A DISPATCHED compile carries the client's DIRECTORY and its REPLACEMENT, never the rule
    — a rule's left-hand side is a path on the WORKER. The worker maps BOTH candidates and
    DROPS its own rule; an empty directory means map nothing, and a worker that cannot spell
    the rules REFUSES. It is the node's WORKING directory, and the DIRECTORY is what says
    "map nothing", never the replacement. `ctest -R node-working-directory`.
  - **Each end predicts that directory from `$PWD`, not `getcwd(3)`** — `CompilerWorkingDirectory`,
    on both sides. Read `comp_dir`, never compare objects. **A model of a driver that is MORE
    PERMISSIVE than the driver produces WRONG AGREEMENT**, so err NARROW.
- `DW_AT_name` is `comp_dir`'s SIBLING: the client's spelling travels as a REPLACEMENT, never
  a path the worker opens; the WHOLE path is mapped, and the rule goes LAST. gcc needs a
  SECOND pair (`sourceRoot`/`sourceRootReplacement`) carrying the RAW spelling beside the
  mapped one, ordered AFTER the directory rules and BEFORE `DW_AT_name`'s. A half-filled pair
  is REFUSED; every other failure to spell a rule is NO RULE. The worker's half of that skip
  is reported at STARTUP (`ScratchRootMappingWarnings`), asserted TOGETHER with the rule builder.
- An object file is not a byte string. Every MSVC driver stamps the clock into the COFF
  `TimeDateStamp`, so `FASTCACHE_VERIFY` cannot `memcmp` there; ELF keeps the byte comparison.
  Parsing never grants an excuse, it only says WHERE: a FRESH object that will not lay out is
  `Unsupported`, a SERVED one that will not while the fresh one does is `Mismatched`. Never
  `/Brepro`.

**[`.agent/rules/distributed-compilation.md`](.agent/rules/distributed-compilation.md)**
— dispatch, workers, the scheduler, the node's tiers. Before `Distributed/`,
`apps/fastcache-compile-node/`.
- The text sent to a worker is **not** the text the key hashed; dispatch preprocesses a
  second time, with `#line` markers.
- A worker is told its input is preprocessed *and* what language it is in; the file
  extension is the last of three answers, never the first.
- That language is stated by the flags dispatch APPENDS, so a build that named one itself
  (`/TP`) is folded into the language and dropped — never refused. A selector naming a FILE,
  or an `-x` value with no exact language, is still refused.
- The arguments a worker will pass on are an **allowlist** keyed on the driver family, and
  the `-f` space is ENUMERATED rather than prefixed. **A refusal by ABSENCE and a refusal by
  ROW are the same answer only while nothing else is consulted**, and `--allow-compile-arg`
  is the something else — so the program-invoking class is `Deny` ROWS, and a `Deny` row takes
  `ArgValue::AnySuffix`. Operator entries are consulted LAST, matched whole and exactly, and
  ride into `MakeNodeServiceSpec`.
- A cache exchange is bounded by a round trip, a dispatched compile by how long a COMPILER
  runs; they must not share a deadline. A per-call `SO_RCVTIMEO` is not a bound at all.
- That total cannot also answer *how fast is a stopped worker noticed*: keepalive answers a
  dead HOST, and the worker's own `Status::Progress` pulse answers a stalled process, against
  a SLIDING idle deadline with its own `SocketDeadlineTarget` — so `Silent` is distinguishable
  from `Expired`.
- Leadership and membership are one `Gate()`, run for every verb, reads included. Only
  leadership stops applying at demotion, so a `GateScope` says which; membership is never
  relaxed; `Scheduling` is the default. A verb qualifies for a narrower scope only if it
  touches non-replicated state this node created AND can create nothing — both clauses — and
  the settlement still refuses a token it never issued.
- Duplicate suppression is asked **before** capacity.
- A lease has three transitions and expiry is the third: the **client** resolves it, on every
  path out of the compile, over a fresh connection.
- A listen flag answers "does this port face the network" only when this process bound the
  port. Under socket activation the flag describes nothing, so the config table's rule and the
  runtime guard both stay.
- Whether a worker **checks** a lease is a startup decision, never a per-request fallback. The
  question is "can a machine that is not this one reach the compile surface", not "is a key
  configured". A validator returns a REASON rather than a `bool`, captures the worker's own
  endpoint rather than taking one, and never answers `UnknownLease`.
- A resolve answers on liveness, not presence — an unknown token is refused.
- And it says WHICH nothing it resolved: one wire code, three rows, keyed on the OUTCOME. Only
  `Expired` is counted, and the enumerators name what was OBSERVED. Acceptance is a
  discrimination case; per-arm sections all pass when all three answer alike.
- A credential lives in `SecureByteBuffer`, and the wipe is an **allocator**, not a destructor.
  Container-agnostic is not SUFFICIENT — SSO keeps a short secret where no allocator is called,
  which makes **macOS the platform to write the failing test against**. Secret STRINGS need an
  inline wipe too. Holders are found by NAME, so a row that has stopped matching is a refusal.
- A lease token is a credential, and its MAC covers the granted **endpoint**. Fields
  length-prefixed, never joined.
- The PSK signs through ONE seam and the domain is a required PARAMETER, never a string a caller
  remembers: `Cluster/ClusterSigning.hpp`'s `SigningDomain` and its `SigningDomainTable`.
- That change moved the proof's MAC *input* and **`DiscoveryWire::CurrentVersion` deliberately
  did not move** — the datagram grammar is unchanged. "We changed the MAC, so bump the version"
  is the tempting correction, and it is wrong.
- The MAC is checked before any other claim is reported on, or a named refusal is an oracle. The
  expiry bounds how long a *captured* token is useful and is **not** a capacity bound.
- A grant is spendable **once**, at the worker it names. The spend runs LAST, so a grant refused
  on a reading of its claims is not consumed, and every refusal a client RETRIES is decided above
  the validator. Keyed by a DIGEST, since serials repeat across a restart while tokens do not.
- **Retention window and acceptance window are ONE window**, and one predicate — not one
  constant, because `expiresAt` is attacker-chosen, so it is a comparison BEFORE a subtraction.
- The learned scheduler term is a DIAGNOSTIC, never a gate: a lower term is ADOPTED, and
  `WorkerJobsRefusedLeaseStaleEpoch` is RETIRED rather than left reading zero. A ratchet is a
  permanent denial of service **with no attacker at all**.
- **A lower term is a REGRESSION, and calling it a reset is a claim the worker cannot make** — a
  grant delivered across a leadership change arrives as one too. Report what is OBSERVED, name
  both causes, and say that the RATE separates them. **A confident wrong signal is worse than a
  vague right one.**
- No key means the SCHEDULER signs nothing: unsigned grants and one bounded warning, never a
  silent fallback. Its startup refusal is still open (#303) and must take the worker's shape above.
- An OUTBOUND credential is read where it is PRESENTED, through one seam
  (`Node::ICredentialSource`), never captured at construction — which is what lets `--requirepass`
  on the worker be `Reloadable::Yes` at all. A rotation reaching two of three sites is WORSE than
  one reaching none. A site that never reaches for the seam is a SCAN, with a positive control on
  the pattern. The daemon's `SharedAuthSource` answers the opposite question and does not transfer.
- The worker's five key files are asked about at the START **and at every accepted reload**, from
  `main` and never from `WorkerBody`. A mode is in no configuration, so the re-ask is of the
  FILESYSTEM, not of the reloader's two snapshots; no configuration file means no second moment.
- A worker being dropped is an **event** (`ExpireStale`), or nothing releases what was held against
  it. A node restarting inside the heartbeat window is the second route to the same pin, closed by
  `Register`.
- A discovery layout describes a **directory layout, not a vendor**: one installation may match two
  rows, and `vswhere`'s answer is memoized across them, empty answers included.
- A port this node LISTENS on is a row of `NodeSurfaceTable()`, and an opener takes the
  `NodeSurface` — not a listen spec, a default host and a name. Protocol is a column, so is the
  host a bare port falls back to. `--print-surfaces` prints the RESOLVED configuration.
  `--advertise` is not a surface — it is told, not opened.
- `--cache-memory 0` means no tier. Zero is how `InMemoryLruStorage` spells *unbounded*.
- What a node holds back from compiles is what its tier **built**, never what a flag asked for —
  so capacity is derived *below* the tier startup. Which tiers cost RAM is a column of
  `StorageTierTable`, and a present zero is *unbounded*, not nothing. A disk tier's key index is
  RAM no budget covers, and it is **a working set rather than a store**, which rules out a
  reservation taken at startup.
- A node SERVES while it identifies its toolchains: the cheap half stays at startup, the walk moves
  to the heartbeat thread's first round, and it registers NOTHING until the fingerprint is real. An
  empty map is two opposite answers, so `ToolchainSurvey` travels beside it with a deleted default
  constructor. "Nothing to serve" stays fatal; `/healthz` stays green.
- A probe that did not RUN is not one that answered nothing, and an identity built on one is neither
  served nor cached. Empty roots are ordinary, so the guard is `exitCode == NotSpawned`, never the
  count. A short include-tree WALK is worse still. What counts as a gap is what two ends would
  DISAGREE about.
- A reply's codec is chosen from what the OTHER end said it accepts, never from this end's list
  against itself — and a codec list is `AvailableCodecs()`, never a literal. A test must separate
  the two ends **disagreeing**; "it round-trips" passes under the bug.
- A stored value's text regions are canonicalized by **every** server on this wire, through the one
  `CanonicalStoredValue` beside `CompileValue`. A path with no `<SRCROOT>` sentinel is ordinary, so
  retirement is a schema bump, never a sniff.
  - And by every **VERSION** of them, or the rule holds at no moment a fleet is actually in.
    `CompileValueVersion` names the canonicalization spec and not only the framing, pinned to the
    BEHAVIOUR by a conformance digest. A value from a generation this build does not implement is
    `ForeignGeneration` and is REFUSED, carrying no bytes to store.
- A manifest naming the TU and no header revalidates forever: `ClassifyAgainstRoots` calls every
  path outside both roots toolchain, and answers in THREE values with no bool beside it.
  `BuildManifest` refuses `NoProjectDeps` when deps were reported and none survived, and
  `DepsNotObserved` when no dependency record was observed at all — a fact the caller STATES through
  `ReportedDependencies`, never one inferred from an empty vector. `ValidateManifest` refuses an
  empty set rather than letting `all_of` pass vacuously.
- A WORKER follows `NotLeader` too, or the client half arrives at an empty fleet: `Gate()` refuses
  `Register` as well. A leader is remembered only once a round was ACCEPTED there, and a remembered
  one that stops answering falls back to `--scheduler` in the SAME round. `NotLeader` must not clear
  the worker id; `UnknownLease` must.
- `NotLeader` is an instruction, not an answer about the fleet: a client follows it to the endpoint
  it names (`RedirectTarget`), and the RELEASE goes to whoever ISSUED the lease. Judged by PARSING
  the message, never by testing it for empty — and splitting is not parsing, so one predicate,
  `ParseDialEndpoint`. Bounded, or two nodes with a stale `_knownLeader` name each other forever.
- A COMPILE reply is tied to its request or REFUSED (`Mismatched`), before the object envelope is
  opened. The digest is taken in `CompileJobRunner::Run` from what is about to be spawned, never
  folded in `WorkerProtocol` from the decoded request. A field is covered exactly when the client
  knows it before sending AND the runner observes it at execution. The base name is derived ONCE.
  It has no counter and can have none, so the alarm is an UNCONDITIONAL stderr line plus a
  `--show-stats` reason, and the outcome stays a MISS. Input side only.
- A cache is per node; the registry is keyed per `(fingerprint, endpoint)`. Summing a cache field
  across `LiveWorkers()` counts one machine once per toolchain.
- A `FETCH` outcome decides whether the daemon is worth a second command (`CacheIsServing`), never
  whether the invocation continues: an unreachable or refusing cache still dispatches. The reason
  is still recorded, the MISS trace is skipped, and the `STORE` is skipped.
- A bounded wait MEASURES its ceiling: `waited += poll` counts the sleep it ASKED for, not what the
  host's timer granularity charged. One `DrainWithin` (`Core/BoundedDrain.hpp`), whose `DrainBound`
  carries the ceiling and the cadence and whose blocking and clock are ONE injected seam. A comment
  naming what it duplicates vouches for the duplicate's bugs.
- An unbounded drain hands the ending to the supervisor, which answers `SIGKILL` with no diagnostic.
  `~WorkerServer` bounds it, says what it abandons and ends the process itself — returning would
  free members a running job is still inside.
- A REGISTER endpoint is **not** verified against the caller — `DispatchWorkerEndpointMismatch` only
  counts it. Comparing hosts refuses the documented setup and stops only a *third* host; the fix is
  a credential, as discovery's `(node, endpoint)` MAC is.
- `CallerContext::peerId` is the kernel's peer host and IS trusted — membership is decided from it.
  It carries no port; a peer dials from an ephemeral one.
- A node's cache tier serves **this machine**, always: locality is a property of the VERB, never of
  the bind and never of a member list. `CacheResponder` therefore takes no membership oracle — its
  absence IS the fix. The question is ambient, so it arrives through `Platform/ILocalityOracle`:
  `IsLoopbackHost` first and lock-free, then an address set refreshed on an INTERVAL — never on a
  miss, which any stranger can bill this machine for. Folded with `SameHost`, or a `::`-bound
  surface refuses its own clients.
- Cluster membership is one ROUTE to admission, never the whole policy: `--fleet-member` admits
  *clients*, which never join consensus, so what the cluster agrees is **added** and never
  substituted — composed at the `IMembershipOracle` seam (`AnyOfMembership`). Absence from
  `ClusterState` is not removal, a forget is a positive act, and revoking a host on both lists is a
  **reload**. Pinned by a test in the *worsen* direction; `NodeMembership::Adopt` is the SECOND
  publisher and writes only `--fleet-member`'s list.
- REMOVAL is the direction a live admission path has to get right, and the direction a test skips:
  adding a member fails CLOSED and self-heals, removing one fails **OPEN** and nothing reports it.
  So `NodeMembership` IS the oracle rather than handing one out — surfaces bind an
  `IMembershipOracle const&` once, and a test that re-asks `Oracle()` after a reload passes under
  exactly that defect. And a reload may not WIDEN admission on a node with no `--cluster-key-file`,
  which built an unchecked lease validator at startup. Asked as a TRANSITION, or it refuses the
  keyless nodes running happily today.
- A compile is awaited onto a `ThreadPoolExecutor` sized to the slot cap, never served inline and
  never on a reactor. On the merged `0xFC` surface that is TWO hops, a frame arriving on a reactor:
  off to the pool, and **back before the reply is returned**. The hop back is invisible at every
  call site, so a test asserts the THREAD IDENTITIES, not the reply. That door spends
  `WorkerServer::Capacity()`, never a second `CompileCapacity`; the worker is declared before the
  surface so the listener stops admitting before the drain counts. One `RefuseUnlessMember`, the
  lease still checked inside `WorkerProtocol`, and `AuthRequired` **false** for `Op::Compile`.
- Detaching the compiles made the per-request payload cap a per-connection one; the in-flight byte
  budget lands in the same change, refusing with `EndpointBusy` because a slot was free and memory
  was not.
- That budget charges what a request **costs**, not what its frame is long: a codec envelope's
  declared expansion is the larger number, and a ceiling on it is per request while the budget is
  per surface. A price above the whole budget is left to the decoder.
- Anything a worker derives per job is derived per THREAD: two compiles sharing a scratch number
  shared `tu.o`, and one answered with the other's object.
- And per PROCESS across machines: a scratch root is CLAIMED exclusively, never merely named
  uniquely. Claiming is the liveness check, so there is no race. `flock`, never `fcntl`. The lock
  file sits BESIDE the root, because emptying the root is what cleanup does. No unclaimed fallback:
  `TEMP` is the relocation mechanism and a refusal is named.
- A child inherits what the PROCESS has, not what the call set up. Windows names the handles it may
  inherit; POSIX marks both pipe ends close-on-exec, under the lock that covers the spawn.
- A drain waits on a condition variable, never `atomic::wait` — an atomic wait can return without
  the notify and free the object the notifier is still inside. And it calls `Shutdown()` first.
- `AvailableSlots` folds four ceilings into one; `SlotCeilingsFor` is the same arithmetic with each
  named, and a tie names the earlier limit in enumerator order.
- A heartbeat age is a duration on a report, never a `TimePoint` on `WorkerInfo` — a raw instant
  invites `steady_clock::now()` and breaks every `ManualClock` test.
- A CoW store file is claimed exclusively at `Open` and a second opener is refused by name (`InUse`).
  `flock`, never `fcntl` — an fcntl lock is per process and a second store inside one would take it
  again and succeed.

**[`.agent/rules/consensus-and-cluster.md`](.agent/rules/consensus-and-cluster.md)**
— Raft, discovery, membership. Before `Consensus/`, `Cluster/`.
- The pre-shared key never travels in a beacon. It appears only inside an HMAC over a nonce
  *this* node chose, and the MAC covers the `(node, endpoint)` pair.
- A proof only ever answers a challenge this node issued, and the nonce is spent whatever
  the outcome.
- Discovery never changes membership: it reports who proved the key and where.
- `RaftNode` reads no clock, opens no socket and draws no randomness of its own.
- A snapshot is durable before it is acknowledged, and the configuration travels inside it.
- A seeded draw must be identical on every standard library — `UniformInRange`, never
  `std::uniform_int_distribution`.
- A node being admitted must never have bootstrapped a cluster of itself, so
  `RaftConfig::members` may legally be **empty** and `--raft-join` is what starts a machine
  that way. Who a node dials is not who it counts.
- The quorum follows the replicated state, one change at a time, additions before removals —
  and a member is never counted before every node can dial it. **One at a time is
  load-bearing for READS as well as for commitment**: CheckQuorum consults the committed
  configuration while a change is in flight, and that is only safe because any majority of
  the old and any majority of the new share a member. Relaxing the restriction on the
  commitment argument alone breaks a rule nothing would warn you about.
- Absence from `ClusterState` is not removal: a member named in the bootstrap set is never
  proposed for removal, and a node given no bootstrap set proposes none at all.
- A cluster that has elected is not one that has formed. Until every member attaches,
  pre-vote refuses nothing and any stall re-elects — assert leadership stability only after
  formation, and log the term or nothing explains it.
- A leader and a follower stamp the same link half a round trip apart, so a shared window
  never bought a shared answer. A non-leader decides a pre-vote from its own `_knownLeader`
  and election deadline, never from a timestamp.
- CheckQuorum DEPOSES a leader here. It measures SILENCE, and a member admitted a moment ago
  has not been silent, it has not been ASKED — so while a change is UNCOMMITTED, CheckQuorum
  asks about the **committed** configuration, never by seeding `_followerContact`, whose
  absence means something else and which pre-vote reads too. **This rule has no constant**: a
  window closed by one is a window a slow first round trip outruns. `undecided` in a node log
  is `SchedulerRole::Undecided`, not a Raft role.
- A refusal code carries its own PERMANENCE, and there are THREE answers. `SubjectOf` reads
  as a global fact, so one enumerator may have only one meaning; `WireCodeFor` fails the
  BUILD until each says what it means on the wire. **`RefusalSubject` has `Satisfied`** —
  *already in force* is `Command` to go and correct, or `Moment` abandoning a pass with
  nothing left to do. It stays a refusal rather than a success because a success must name an
  entry that does not exist.
- "A leader spoke" arrives at two handlers, and every rule about it belongs in both:
  `OnInstallSnapshot` is `OnAppendEntries` speaking, membership guard and candidate demotion
  included.
- **A node IS its state directory: its identity is MINTED into `--cluster-dir` and read back
  forever, never derived from the machine.** Not the hostname, and not the OS machine-id even
  as a SEED — two clones of one image with no state yet mint the SAME id. A wiped state
  directory MUST get a new identity. `--node-id` stays as an override and is recorded; a
  derived id cannot be typed into `--raft-peer`, so `--raft-self=<host>` states the address
  and takes the port from `--listen-raft`. The resolved value reaches the running config, the
  service registration AND every reload candidate, or reloads are refused by name. A recorded
  id that is empty or not text is refused, never re-minted. A copied state directory copies
  the node and is NOT refused — `--cluster-admit` at a new address is how a MOVE is recorded.
- **The hostname is a fleet-page LABEL and decides nothing** — nothing keys, routes, admits or
  dispatches by it, which is what makes a mutable non-unique value safe to carry. It still
  passes the UTF-8 gate at `SchedulerService::Register`. **No prefix matching on ids.**
- **A replicated setting must not decide where a node sends a CREDENTIAL.** A node presents
  its `--requirepass` at whatever address it names, so one committed entry would redirect
  every member's secret while the secret itself stays per machine. Removed rather than wired,
  and refused BY NAME through `RefusedSettingTable`, since *no such cluster setting* reads as
  a typo or as a node too old.
- **A mode rides on the PORT, never on the absence of a NAME.** Consensus is on iff
  `--listen-raft` resolves — asked of the surface row, so `--print-surfaces` and the mode
  cannot disagree. A flag whose ABSENCE carries a mode can never be given a default. A boolean
  beside the port is the tempting alternative and is worse: two things that can disagree, and
  both disagreements are states nothing could describe. One predicate, `RunsConsensus`.

**Backwards compatibility is not owed yet, and designing around it has already cost
work.** Until this software is declared production ready, a wire format, an on-disk format,
a cache-key version, an enumerator order, a CLI flag or a config key may change outright;
`fastcache-compile-node` has ONE installation, so a format change costs one local rebuild
rather than a migration. **Two limits, and they are the whole of it:** it does not license
changing a format without bumping its VERSION — the version is how a mismatch is *detected*,
which is why `UnsupportedFormatVersion` and `Corrupt` are different answers
([`.agent/rules/storage.md`](.agent/rules/storage.md)) — and it EXPIRES at the
production-ready declaration, so anything written on the strength of it is due a re-read then
rather than inherited. Removing superseded code outright rather than keeping a compatibility
shim is the same position from the other side.

**[`.agent/rules/wire-and-protocol.md`](.agent/rules/wire-and-protocol.md)** —
framing, the auth gate, sockets, dialling and coroutine lifetime. Before
`Protocol/`, `Net/`, `Async/`.
- A frame declares its own length, so a rejection is a **reply** and a resynchronization —
  never a close.
- A reply carries a status byte and NO kind, so the step-over-what-you-do-not-know property
  is REQUEST-side only. `Status::Progress` therefore moved `MinSupportedVersion` with
  `CurrentVersion`. Which verbs admit it is a table column, `static_assert`ed to `Op::Compile`
  alone; the payload is empty and the encoder takes no argument; a reader loops to a TERMINAL
  status, and the exchange's total is what bounds how many arrive.
- Silence is only measurable against something that would otherwise be said, so the worker's
  cadence and the client's idle bound are ONE pair of numbers in `CompileCacheWire`, with the
  relation between them `static_assert`ed.
- A pulse is a SECOND writer for the length of the answer, so the endpoint settles it before
  it writes anything, and a pulse still parked past the bound ends the connection —
  `SettleWatch`'s rule on the write side. A test that reads one framed reply cannot see a
  frame emitted AFTER it, and "every frame but the last is a pulse" passes vacuously on a
  build that pulses none.
- Which verbs are reachable before authentication is a *column of the table*, and the gate
  runs before the payload is buffered.
- A pre-auth verb carries its own payload ceiling, `static_assert`ed so a new one cannot
  reopen the hole by omission.
- An unimplemented verb is refused `Wire::UnimplementedVerb`, never `DispatchNotPermitted` —
  the launcher steps over the first and proceeds unauthenticated, and treats the second as
  fatal. The choice is a `(op, code, why)` table row, not a `switch` special case, and the
  code is ONE named constant every surface and the client spell. *Unimplemented* is not
  *served elsewhere*: a verb another port answers stays `DispatchNotPermitted`. A row for a
  verb the surface does serve is dead — `static_assert` it cannot be added.
- A wire constant has TWO facts, its name and its value, and a symbol both ends spell can only
  test the first. Pin the **byte** as well, and keep one test on the raw enumerator — it is
  the anchor, not the code smell it looks like.
- `Net/` must not depend on `Core/`. `Async/` travels with it, plus three named
  dependency-free leaf headers; `ctest -R net-boundary` enforces the table.
- `CompileCacheWire.hpp` must stay header-only and dependency-free — the launcher does not
  link `FastCache`. It therefore carries cache tiers **positionally**, which makes
  `StorageTier`'s enumerator order a wire contract.
- **And an enum SAYS which kind it is at its declaration** — transmitted or persisted, or
  private — because *no comment* means both, in a tree holding both. A mid-enum insertion
  shifts every later ordinal: free in a private enum, and in a serialized one every record
  already written comes back with each field attributed to the NEXT enumerator, silently. The
  explicit `= N` is the enforcement, and on a PRIVATE enum it is harmful. **`RowsInEnumeratorOrder`
  is not that guard although it reads like one**: it fires on a row omitted or misplaced, and
  an insertion whose row goes in at the matching position leaves the table consistent while
  every record already written decodes shifted. The census pattern `static_cast<T>(byte)` is
  NARROWER than it reads — a table walk, a positional carrier and a cast to a TEMPLATE
  PARAMETER are all outside it — so nothing checks the declarations and this bullet is the guard.
- SIGPIPE is suppressed per socket, never process-wide: an ignored disposition is inherited
  across exec.
- So is keepalive, and for the mirror reason: `ApplyHotSocketOptions` is where every socket
  passes, so arming there would change when every idle client connection and every Raft link
  is dropped. It is a `DialOptions` field, and the flag without the intervals inherits a
  two-hour default that reads back as armed. A faster failure nobody can NAME is not an
  improvement — expiry and a lost peer are one broken socket, so the timer records which.
- A child inherits this process's sockets too, and neither platform stops it. Armed once, in
  `ApplyHotSocketOptions`. A spawn names what it hands over
  (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`) rather than marking what it does not.
- A listening socket claims its address exclusively — `SO_EXCLUSIVEADDRUSE` on Windows, where
  `SO_REUSEADDR` lets a second process take a port already being served. Sharing a port is
  `ReusePort::Yes`, and only that.
- A struct a decoder returns **by value** must not borrow from the bytes it decoded:
  `Decode(Encode(x))` is the obvious spelling and is a use-after-free the moment one member
  becomes a view. A `*View` type borrows and says so; anything else owns. Which shape to pick
  is per type, and the test is a CONJUNCTION: OWN when the result outlives the buffer in
  practice, stay a named `*View` when every consumer reads it in scope AND something depends
  on not copying. One clause false is not a tie. The encode side goes on borrowing either way.
- **What the borrowed field DECIDES outranks the arithmetic.** A wrong `CapacityFields` is a
  wrong number; a wrong `CallerContext::peerId` is a membership decision read from freed
  memory. Where the two mistakes are not commensurable — a trust boundary against nanoseconds
  — own it and do not bother weighing the copy.
- A regression test for the above needs a payload of REAL SIZE **and** the right ARRANGEMENT.
  Read inline (`Use(Make(x))`) nothing dangles at any size, so the obvious test passes under
  the bug: STORE the value, drop the source, churn the freed storage, then read. Size decides
  WHICH check fires rather than whether one does. Pick a size that is REAL rather than large,
  and `static_assert` it so nobody shortens it back into uselessness.
- There is exactly one TCP client, `Net/TcpClient`. Do not write a second.
- A synchronous dial spends a thread the caller does not own — a reactor thread dials through
  `PlatformConnector`, never `BlockingConnector`.
- EOF means "this peer has finished SENDING", not "this peer is gone": a server answers what
  is already determined and abandons what is still pending. `ISocket::ShutdownWrite` exists so
  the question is askable in PRODUCTION at all — a rule nothing can express is a rule nothing
  can be held to. The answer does not transfer between wires — the compile surface reads a
  mid-compile EOF as *gone* under the SAME rule — so state which surface any measurement
  covers. A watcher for it reads the COUNT: an ERROR is an abortive close and `0` is EOF,
  which is the ORDINARY way a client leaves. Both arms set one flag, so a green suite proves
  nothing — **DELETE an arm and see which case fails**, and keep a control that must survive
  an open write side. The signal is that the handler RETURNED: still parked and
  unwound-having-written-nothing are identical bytes.
- A peer that sent NOTHING asked nothing, so it is CLOSED, not refused — and "sent nothing" is
  TWO states, neither of which is "the request was bad". **`sawHeadEnd` decides THAT a head is
  refused; the cause only selects WHICH code**: over the byte cap `431`, cut off by the
  DEADLINE `408` (the reachable route, since the read timeout is per READ and the head has no
  total budget), ended by EOF mid-head `400`. The tempting reading of the EOF rule — *the peer
  finished sending, so serve what arrived* — is the bug: a head with no terminating blank line
  determines nothing. The control that belongs beside it is a COMPLETE head half-closed after,
  still served `200`. Neither silent outcome is counted, deliberately. A deadline expiry is TWO
  codes: `Net::IsDeadlineExpiry`, never one operand — except at a listener that arms NO poll
  timeout, where `Timeout` cannot arrive; that exception is REACHABILITY, never semantics, and
  **a reason that generalises further than the fact it was drawn from is worse than the narrow
  one.** The reported shape cannot be asserted on, because a late write draws an RST that
  destroys the response, so the probe never writes — and a probe reporting silence must be able
  to say it observed none. Closing is retriable where `400` is final, and one number still
  answers both *how long may a peer be silent* and *how long may a head take* (#828).
- And a TLS peer says it with a RECORD, so the raw socket answers the OPPOSITE: `close_notify`
  then FIN means bytes are on the wire and the raw peek reports `>0`. `TlsSocket::WaitReadable`
  decrypts with `SSL_peek`, which removes nothing — "consumes nothing" is about bytes the
  CALLER could have read, not the decorator's own buffering. A raw EOF before a full record is
  EOF too.
- And on the compile surface those two arms are COUNTED apart, so folding them is a regression
  even though both reach one outcome. A peer that RESET and one that said goodbye are different
  diagnoses and only the first is worth an alert. Each arm names a `PeerDeparture` a table maps
  to its counter. Proved by neutering, and the FIN case is the load-bearing one — counting
  every departure abortive PASSES the RESET case.
- An object a reactor OWNS is destroyed on that reactor's worker thread, or with that reactor
  stopped — `IReactor::TeardownIsSerialisedWithDispatch()`, asked of EVERY reactor. **Match the
  ASSERTION, never the case that happened to be running.** The defect is PORTABLE and only the
  predicate was Windows-only. A drain that waits for the loops does not mean the reactor stopped
  — the last loop decrements before `NoteLoopFinished()`, and `Stop()` only posts a wakeup.
  Posting the teardown HANGS the ordinary single-surface case; DEFER instead (`NodeIoLoop::Retire`,
  where member ORDER is the mechanism). A test removes the race rather than waiting for it: a
  loop that never finishes, and a fake listener that RECORDS the predicate instead of asserting.
- `Close()` can be the last thing that runs on a socket, so it must touch no member after it
  completes an awaitable.
- An awaitable's address is taken in `await_suspend`, never in the factory that returns it —
  that one is a LOCAL returned by value, so the caller suspends on a different object.
  `SetSuspendCallback` is what makes the right answer the only reachable one.
- And it is the ONLY thing that retrieves a parked read, so the shared FAKE owes that too:
  detach first, complete last with `Cancelled`. Prove the leak instrument before believing a
  green ASan run — a parked frame is a live unreachable allocation, which LSan reports exactly.
- `Read`'s buffer must be NON-EMPTY, because `0` is taken and taken by the opposite fact: every
  transport's receive primitive answers `0` for a zero-length request, so an empty span would be
  answered *the peer has finished sending*. `Detail::RequireReadBuffer`, called as `Read`'s first
  statement by all six transports; a PROGRAMMER ERROR, so an assert and not an error code.
  Release still answers EOF, and that is a stated trade rather than an omission.
  `empty-read-buffer-canary` watches ONE site — a canary aborts at the FIRST violation — so
  `read-buffer-guard` DERIVES the set and requires the call before the body's first `return`.
- **A guard folded INTO the operation is self-enforcing; a guard called ALONGSIDE one needs a
  scan.** Ride the guard on something the site must do; where you cannot, the scan is not
  optional. **The author of that rule broke it one commit later, in the same branch** — whoever
  reads this and concludes *I would have noticed* is the next instance. And the fix is NOT pure
  virtual: **reach for the type system when the obligation is DO SOMETHING, reach for a scan
  when it is SAY WHY** — a pure virtual compels seven fakes to write `{}` with no reason beside
  it, which is *forgot* in the vocabulary of *decided*.
- **A `/simplify` finding is a change like any other and is not exempt from the review its
  subject just had.** A late cleanup arrives wearing the authority of a review rather than the
  suspicion of a change. And **a regression test can fail to reproduce its regression** — six
  self-test cases could not see the defect the cleanup introduced.
- A socket has ONE read operation and `Read` and `WaitReadable` share it, so arming either while
  the other is parked drops the parked coroutine — never resumed, never freed, no signal. The
  rule lives on `ISocket`, not in one consumer's comment; `Detail::ClaimReadSlot` folds the claim
  and the `assert` into one expression, and `read-slot-guard-canary` double-arms a REAL socket
  and must die. Not a refusal, which breaks a live caller — and **the second half of that
  sentence has been RETRACTED**: the hazard is the **SITE**, not ownership. At the ARM site a
  socket cannot tell a stale parked wait from a live one, and cancelling a live one is a false
  disconnect that drops a healthy client. The CALLER can tell.
- The WRITE slot is the same rule: one write op per direction. `Detail::ClaimWriteSlot`
  (`Net/WriteSlot.hpp`) folds the claim in, Debug-only, and it reaches `FrameEndpoint`'s
  one-writer property because `WriteAll` sends a whole frame in ONE `Write` — so a parked write
  is a HALF-SENT frame and a second writer splices into it. `write-slot-guard-canary` watches it
  BOTH ways, requiring the acceptance marker first: **a guard nobody has watched ACCEPT is not
  known to work either.** What is still unenforced is a new helper naming `Loop`.
- So a parked read is retrieved by `ISocket::CancelRead()` — the only spelling of *abandon* that
  is not `Close()`, virtual with a default no-op like `ShutdownWrite`. Keep ONE watch,
  re-TARGETED per pass and re-armed only once the previous has RESOLVED (arming once and never
  again makes the pipelined case pass vacuously), retired by RAII AND explicitly before the reply
  write. Retiring is not disconnecting — the cancel arrives as an ERROR and any error reads as a
  departure, so what silences it must be the RETIREMENT, never the code. **Synchronous on every
  transport that parks a read**, IOCP included — the kernel owns a retracted op's `OVERLAPPED`
  until a LATER turn, so the retired operation node is stood down and the next read gets a fresh
  one. `InMemorySocket` never parks a `WaitReadable`, so
  `Testing::ParkingReadableSocket` parks and COUNTS orphaned watches instead of aborting.
- A wait nothing can cancel is a coroutine frame nobody frees: park through
  `Schedule`/`CancelPending`, and bound any sleep a peer can move the deadline of.
- A reactor resumes what it parks or FREES it, and it may free only what nothing else owns. A
  blanket destroy is a `heap-use-after-free`, because `Schedule` BORROWS; so ownership travels
  with the park (`ParkedWork::abandon`, derived from the promise type, non-empty exactly for a
  chain rooted in a `DetachedTask`). What is freed is the chain ROOT, because a `Task` chain's
  ownership runs downward. Resuming is a hang, not an alternative: a bounded wait re-parks.
  `Resume()` disowns and resumes in ONE expression; IOCP's posted submissions have no entry to
  fold into and keep a side table instead, which is stated rather than left silent.
- Re-declaring ONE overload in a derived interface HIDES the base's others, and here it hid the
  one carrying ownership. Nothing diagnoses it — every call still compiles and binds to the
  borrowing overload. The detection is the compiler: convert the sites to pass the owning type,
  and the `no viable conversion` errors enumerate the defect's reach.
- A missing keyspace event has two ends — the tier that never named the victim and the observer
  that never published it. Check both before changing either.
- A reclaim is reported **before** the call that caused it: `ADD` on a lapsed TTL names the same
  key twice, and the wrong order tells a subscriber a live key is gone.
- In a layered cache no single tier's eviction is total, so none is reported. An expiry is,
  because both tiers hold the same TTL.
- A reclaimer nothing constructs is the bug it was written to fix: `PurgeExpired` was correct
  and tested, and had no production caller at all. Assert the wiring.
- The expiry cycle sweeps `engine.Storage()` — the notifying decorator. One layer down it frees
  the bytes and publishes nothing, and every tier test still passes.
- One cycle per daemon, on reactor 0; its reclaim ceiling sits below `ReclaimLog::DefaultCapacity`
  or the sweep drops the events it runs to produce.
- A bounded sweep resumes from a cursor and `ShardedStorage` rotates its starting shard, or
  everything past the first budget never expires. That cursor outlives the call, so a tier gets
  exactly one erase point.
- `--expiry-interval=0` disables the cycle (a coroutine that *ends*); `--expiry-scan=0` is
  `PurgeBudget`'s spelling of *no ceiling* and is refused.
- On disk a read may not reclaim and a write must: `Get`/`Peek` can hold a shared lock, every
  write verb holds the exclusive one. Reporting without erasing is worse than neither — the
  record stays and fires `expired` again.

**[`.agent/rules/platform-service-and-config.md`](.agent/rules/platform-service-and-config.md)**
— service registration, config lookup, the CLI table. Before `Platform/`, `Config/`,
`packaging/`.
- A service to register is a `ServiceSpec`; what it runs as is part of it, and an empty
  `serviceAccount` means **root**.
- `--install-service` registers the *command-line* config, never the merged one, and carries
  the config PATH rather than the file's values or a resolved default — either pins the
  service to one reading of a file the operator then edits with no effect.
- An install is judged by the **startup** rules as well as the install-time ones: a
  registration replays its command line forever, so refuse it while somebody is watching.
- A refusal that depends on nothing but the parsed configuration belongs in a table — the
  option row for a grammar, `StartupPolicyRejection` for a cross-flag rule — never in the tier
  that happens to need it. An install returns before any tier exists.
- **The addresses a node OPENS and the ones it DIALS are two tables, and one predicate cannot
  serve both.** Opened surfaces are `NodeSurfaceTable()`'s rows; dialled ones carry a predicate
  EACH, because their grammars differ. `--advertise`, `--scheduler` and `--upstream` take
  `ParseDialEndpoint`, where a bare port names no machine. `--fleet-member` must NOT: it is
  matched against a peer's source address through `HostOfEndpoint`, so a bare host is legal
  there. What IS refusable there is an EMPTY element. Shape and PRESENCE stay separate rules.
  `--bind` is in neither — a host is only checkable by binding it.
- Whatever reaches a supervisor must survive this project's own parser round trip — including
  the flags the *installer itself* adds, which are the daemon's only when the spec names an
  application.
- Whether the operator **named** a setting is provenance, recorded by the parse in
  `OptionSpec::explicitBit` — never recovered by comparing the value to the default, which
  cannot see the operator who typed the default. Both the startup decision AND the service
  registration ask it, which is `emitIfExplicit`, never `emitIfSet`. A flag whose default is
  empty needs no bit.
- A config the operator named is strict; one the daemon found is skipped when absent,
  unreadable or untrusted.
- A machine-wide config is obeyed only when only an administrator could have written it
  (`Platform/FileTrust`).
- That is INTEGRITY. Secrecy is a second question and the same access list cannot answer it:
  a directory readable by `BUILTIN\Users` hands that read to the one file operators are told
  to move `requirepass:` into, so the documented remedy relocates the secret.
  `--seed-config` gives the FILE a protected list of its own, not the MSI, which cannot reach
  a file that is not payload. An existing file is repaired only when it is *currently* broadly
  readable, never by content.
- A secret reached BY PATH is not provenance-gated — the path is not the secret and the file
  is — while `--requirepass` out of a config file still is. Which `=<path>` rows are which is
  a TABLE per binary and classification is MANDATORY (`--tls-cert` is named PUBLIC, not left
  off), because an opt-in list reads identically to complete coverage. One subject list per
  binary, so a row reaches every moment that reads it. BOTH binaries ask at the start AND at
  every accepted reload, through one `SecretExposureWatcher` that re-asks the FILESYSTEM: a
  mode is in no configuration, so an implementation diffing the reloader's two snapshots
  covers half the rule and looks right.
- Every flag is one row of `CliOptions()`, which drives parsing **and** help.
- Which flags carry text *other machines* will read is a column of that table (`ParseUtf8Text`).
  `--cluster-forget` is deliberately out of it, or a bad member becomes unremovable; so is
  every path-valued flag, and the compiler half of `--toolchain`.
- A value parser cannot know which flag it was reached through, so it names none and
  `ApplyOneOption` stamps the row's own spelling.
- A configuration FILE reaches the same fields through the SAME appliers, in that order, so
  "the command line wins" is which loop runs second — never a per-field merge with a per-field
  explicit bit and a per-field presence bit. And a RELOAD rebuilds the candidate the way the
  START built it — one `AssembleEffectiveConfig` (file, then argv, then the environment), which
  `ConfigReloader` takes as a REQUIRED argument; re-reading the file alone makes an immutable
  setting refuse every reload by name and a RELOADABLE one PUBLISH a wrong value. A setting a
  FILE can carry and argv cannot is that same defect standing still, so closing one spans
  `Config/` and `Platform/` in ONE change. Which key a row answers to is a COLUMN (`yamlKey`),
  because the mapping is not derivable. A key naming no row is REFUSED. A row a file may not
  carry is on a named list with a per-row reason, and the compile-time guard READS that list.
- A flag whose meaning is its presence is a boolean in the file and `apply` runs on `true`
  alone — the key spells the FLAG. A repeatable row APPENDS, so the command line EMPTIES the
  list first (driven off the `clear` column), and that reset walks argv through the parser's
  own `TakeValue`, or a flag's VALUE spelled like a list flag empties the list. A file that
  failed halfway is DECLINED, never half-applied.
- A missing file is `FileNotFound`, not `ParseError`: `YAML::BadFile` derives from
  `YAML::Exception`, and the general catch sent a mistyped `--config` hunting for a syntax
  error in a file that is not there.
- The shipped reference configuration is checked against the table
  (`ctest -R node-config-reference`) — nothing else connects them, and that check fails when
  either scan matches nothing, because two empty lists agree perfectly.

**[`.agent/rules/storage.md`](.agent/rules/storage.md)** — the on-disk format and
converting a store. Before `Cache/CowTreeStorage`, `CowTree/`.
- An old store is `UnsupportedFormatVersion`, never `Corrupt` — the code is what monitoring
  sees, and `Corrupt` is what makes somebody delete a healthy cache.
- `Corrupt` means the BYTES ARE DAMAGED, and nothing a client sends may reach it. A set or a
  stream is a value blob tagged by a `flags` word a client chooses, so `SetCodec`/`StreamCodec`
  return `MalformedValue` themselves and no caller picks. `CacheMalformedValues` keeps it
  visible, because removing a wrong signal without adding a right one is the other way to get
  this wrong.
- One `Corrupt`, two events, and WHERE decides: damage within `Open`'s reach refuses the
  process to start, damage anywhere else is found per key while it serves. Making `Open` touch
  more of the store converts the second into the first, which is a decision rather than an
  optimisation.
- A format is convertible exactly as long as its reader is in `RecordFormats()`. Bumping the
  version without adding a row is the decision to discard every store.
- "No marker" is an INFERENCE. Validate every record before writing any of them: a store this
  build cannot read must come back unmodified.
- The conversion commits in slices — one transaction inflates the file by a page per record
  per level, permanently — and each slice must `Flush()`, or the freed pages are not
  reclaimable and the slicing buys nothing.
- Each slice records its resume point in its own transaction, so an interrupted run is refused
  by name and finished by re-running it.
- A tree walk is bounded by `PageCount()`, and must not overlap a commit.
- A tier's `bytesUsed` is denominated differently per tier: memory counts STORED (compressed)
  bytes, disk counts `originalLen`. So a compression test asserted through the DISK tier's
  `bytesUsed` compares a number with itself and cannot fail — measure the store FILE there.
- The LRU mirror holds what this SESSION touched — `TouchOrInsert` is its only writer and no
  `Open` path calls it — so eviction reaches the COLD set first, and that is LRU rather than a
  workaround. A test asserting only the total sees neither the wrong victim nor the bound still
  violated. Fourth of a family where state describing the STORE was populated only by touch;
  this one could not be closed by finding a durable SOURCE for a number, because eviction needs
  a VICTIM rather than a figure.

**[`.agent/rules/metrics-and-observability.md`](.agent/rules/metrics-and-observability.md)**
— counters and scrape surfaces. Before `Metrics/`, `/metrics`, `/healthz`.
- A counter is a row in `MetricsCatalog`, `static_assert`ed to cover every enumerator; the
  renderer walks the table rather than a hand-picked list.
- A refusal's wire code and its counter are one row — one fact, two audiences. `Refuse` takes
  the row, so there is no argument to pass a bare `ErrorCode` to, and every refusal on the
  surface goes through it, including the ones that already counted. The row is the REFUSAL,
  not the code: two refusals may share a code and must not share a counter.
- A surface MERGING undoes that without anybody writing a bug, so the scan is EXACT: a file
  with one uncovered site cannot be covered at all. The endpoint owns WHEN, the surface owns
  WHAT including the counter (`RefusalReply` / `EndpointRefusalReply`); a refusal decided
  before a header exists names no verb, so it is the ENDPOINT's own row rather than a default
  arm on the router. `SchedulerRequestsRefusedUnauthenticated` fires only pre-payload, so a
  WRONG token is a third row. But not every refusal is an EVENT — a verb this node runs no
  component for is what a HEALTHY build gets, so counting it buries the scan it would be read
  for.
- That scan is a GLOB over `src/`, never a file list. **A list is exact about the files it
  knows and silent about the ones it does not, and silence reads identically to complete
  coverage.** An over-broad scan fails CLOSED. Header-only, so `_fc_cc_core` gains no row.
- And "deliberately uncounted" must not be spelled like "forgot". THREE spellings, three
  claims — `Refuse` (a rise means something), `RefuseWithoutCounter` (a rise would mean
  nothing, and why), `RefuseUntriaged` (nobody has decided, and which issue will). The third
  is safe only because the check TALLIES it and prints the total per issue on every run: a
  placeholder reason would spell *forgot* in the vocabulary of *decided*. The reason is a
  forcing function, and it is `rationale`, never `why`, which on `RefusedVerb` is text a CLIENT
  is SENT — one word cannot carry both contracts.
- The SET of spellings is derived from that header, never restated in the check. A restated
  list catches one going away and is blind to one ARRIVING. `worker-refusals-selftest` drives
  synthetic trees including that one, because a guard nobody has watched refuse is not a guard.
- The scan filters whole-file before splitting; each needle is a strict prefix of the regex
  that would have matched it.
- Text a peer sent is text, or the fleet refuses it: one byte that is not UTF-8 makes
  `/fleet.json` unparseable for the **whole** fleet. Refused where it enters
  (`SchedulerService::Register`) and never repaired by a renderer — the encoders are total
  anyway, because a consensus entry is applied after it is committed, with nobody left to
  refuse it. Markup's rule is XML's `Char` production over **code points**, not bytes.
- **Skipped, absent, unstarted and failed are FOUR states**, and tooling collapses them — five
  times in four instruments in one session, twelve across eight once a later session's are
  counted, none of them a coding mistake, all of them a representation that could not tell two
  things apart. A count cannot carry this and neither can a `bool`. **Absence of the negative
  is not the positive** — "no pending checks" is not "all checks reported" — so a check
  concluding from a count of BAD things needs a separate assertion that the good things exist.
  **That reaches any probe you TYPE — a `grep`, a `find`, a `gh` query, a throwaway script —
  which is where it is skipped**: ask it for something it must find before believing what it
  did not find — **and the mirror, which is the half that gets acted on: a positive finding
  settles nothing when it is true under BOTH readings of the claim** — and read its exit
  status, where anything neither `0` nor `1` is the instrument failing. Where the answer cannot
  be determined, report that as its own outcome rather than the nearest neighbour.
- **A state-collapsing bug is likeliest in the tool whose JOB is that state distinction**, and
  its author is thinking about the subject's states rather than the instrument's. The repair is
  not "stop exiting": it must report the unrequired failure by name AND KEEP GOING, or it looks
  identical to the broken one on every clean run. **The repair for one collapse is the prime
  site for the next, and the location is the `*)` arm** — enumeration does not save you there,
  because you enumerate the states you are thinking about. **A `case` with a `*)` is an
  unguarded table.** And a verdict computed from a SUMMARY while the evidence sits in the same
  output is its own defect, more durable because the output looks thorough.
- Absent is not zero: a process with no cache reports no cache, and *names* the field to do it.
- Its converse: an absence must not be counted as an event. `NoUpstream`'s honest `false` read
  as a failed store made a machine with no shared cache report a 100% upstream failure rate. An
  outcome that can be *not attempted* is an enum, not a `bool` — and it is fixed at the seam,
  never at the one call site that noticed.
- A counter is a tally, so zero is the truth about events that never happened; absence is
  modelled in the **snapshot**, never by dropping a counter row.
- A duration is a `_sum`/`_count` pair, never a gauge.
- A merged snapshot is one tier's answer standing in for all of them: `SnapshotTiers()` reports
  the split, the `tier` label comes from a table, and a tier the cache does not have renders no
  line at all.
- The fleet page is served by the leader; anyone else answers `503` **naming** the leader, never
  a redirect and never a link to an address it guessed.
- Its columns are a table every renderer walks, one spelling serving as header cell, JSON key
  and text column. Absent renders at the **cell** — `null`, `–` or `-`, never a blank and never
  a zero — and a tier no member runs gets no column.
- A fleet total is computed over `NodeReports()`, never over registry entries.
- Nothing a receiver can **recompute** travels: a handed-over bucket carries two instants and
  the readings, and the leader rebuilds the fold and the coverage by replaying them. History is
  filed under the **machine**, never the worker id.
- A handover cursor advances only on the verb that carried the batch — `accepted` also counts a
  registration, which carries no history at all.
- A node's version is compiled in, rides REGISTER's *nested* capacity record (whose arity is
  variable) rather than its top level (whose arity is exact), and is refreshed on
  re-registration — a restart is what an upgrade looks like.
- A history stores a counter **raw**; a rate is the delta at render, taken only between adjacent
  *present* buckets. A restart is then a gap, not a spike.
- A node records **itself** always; only the fleet-wide slots are leader-only, and which is which
  is `FleetMetricTable`'s `scope` column. Every node samples whatever surfaces it serves — a
  sampler owned by the admin surface left a pure worker recording nothing.
- A **backfilled** window answers for a machine, never for the scheduler: its fleet-scoped zeroes
  are not readings, and drawn as such they are a rate running backwards and then a spike.
- The routes reach a history through **one** door (`IFleetHistoryView`), or the backfill is
  filled, persisted, restored and never drawn. Assert the wiring.
- No state of a history file may keep a node from starting — and a file a **later** build wrote
  is kept and never written over, which is a property of the shared envelope rather than of each
  store that remembers to copy it.
- A chart served as its own resource inherits nothing from the page, so it carries its own
  palette and theme is part of its URL — and that URL carries no cache-buster, or the `304` never
  fires.
- A `304` carries its validators and no content; whether a body is allowed is a property of the
  **status**, not a flag each route sets.
- An unknown `range` is refused, an unknown `theme` is not: refuse where a silent substitution
  would mislead, default where it cannot.
- A stacked area is drawn **top band first**, or translucent fills multiply into a colour
  belonging to no series.
- A `<circle>` is not path data. One `<` in an attribute value makes a browser refuse the whole
  SVG, and the chart is then a broken image behind a 200. A run of one reading is a dot whether
  the shape is filled or not — closing it gives a shape with no width that draws nothing and
  still claims the series was observed. Neither is visible in a test that renders dense data:
  gaps are what a live dashboard has.
- The dashboard credential is its own file, never `--requirepass`; a non-loopback bind without
  one is a startup refusal, and TLS does not substitute for it.
- Plain HTTP is a supported way to serve the admin surface. TLS is on by naming material or by
  asking for material to be made (`--tls-self-signed`) — never a bare boolean, and the two
  spellings are refused together.
- A generated certificate encrypts but does not identify: its fingerprint is logged because that
  is all an operator can compare, and its subject names decide whether any client accepts it at
  all.

**[`.agent/rules/packaging-and-release.md`](.agent/rules/packaging-and-release.md)**
— packaging, versioning, cutting a release. Before `packaging/`, `cmake/Packaging.cmake`,
`cmake/Version.cmake`, the release job.
- The git tag is the only version source. `version.txt` must never come back —
  `ctest -R repository-hygiene` fails at `git add` time if it does.
- The resolved version stays a bare numeric `X.Y.Z`; suffixes live on the string.
- The payload is rooted at `/`, not `/usr`; third-party `install()` rules are excluded.
- Neither a `.pkg` nor an MSI has a conffile mechanism — only a `.default` ships.
- Every `build.yml` checkout that could configure passes `fetch-depth: 0`, and the release
  job's asset list stays the **last** key of its `with:` mapping.
- **A new INSTALLED binary is not one CMake row.** CPack installs the whole Runtime component
  while the three `Package (...)` jobs name their build targets by hand, so a target with an
  `install()` rule that no packaging job builds fails at INSTALL time on all three platforms
  at once — and none of those contexts is required, so it lands on whoever cuts the release.
  Three `--target` lines in `build.yml`, the macOS redistributable loop (a different list:
  payload, not symlinks) and `FASTCACHED_MACOS_LINKED_TOOLS`. Guard: #1202.

**[`.agent/rules/build-and-toolchain.md`](.agent/rules/build-and-toolchain.md)** —
what differs between compilers, standard libraries, hosts and tool versions.
- Run `bash scripts/local-gate.sh` before pushing — **`bash <path>`, never the bare path**.
  A call that fails to START fails for reasons a `chmod` does not cover, and inside a
  `want-fail` assertion any of them is indistinguishable from the rule firing. One
  configuration is not the gate. A RED run stops at the first failing leg and NAMES the legs
  it skipped, so read the per-leg block, not just the reason. A run printing NEITHER terminal
  line did not CONCLUDE — a fifth state beside skipped/absent/unstarted/failed, and never a
  red gate; `--classify=<log>` reads the rule back with an exit status per outcome. A trap
  cannot do this job. **A GREEN run is silent in the OTHER direction**: a platform's leg
  answers a different question, not a weaker version of the same one.
- **A retry makes an instrument's own failures disappear without fixing them.** The ways the
  gate has reported on a tree other than the one under test are enumerated in the rules file
  and deliberately NOT tallied — a number no check can derive from the thing it counts is a
  second source of truth, so the list is the claim. None announces itself, each looks like a
  flake, a re-run clears every one. **Neither the wrapper's exit status NOR the presence of a
  log settles whether the gate ran**, so the wrapper writes an artefact BEFORE the gate starts
  naming the commit it is about, and the verdict is read from the tool's own terminal text.
  Serialise the gate across lanes with `flock` — **a poll-for-zero is not a lock**: it starves
  and it stampedes, and it behaves correctly only while there is a single waiter, which is the
  condition it is invariably tested under. A run that was KILLED mid-build is discarded rather
  than read.
- **A red gate reads as "my branch is bad", never as "the gate is broken" — so a gate that
  fails CLOSED and UNCONDITIONALLY can sit for days with nobody filing it.** Not another
  wrong-tree entry: that list is the gate reporting on the wrong tree, this is the gate
  refusing EVERY tree, at its first leg, with a confidently worded false cause. **A guard
  nobody has watched ACCEPT is not known to work**, so assert the passing direction, not only
  the refusing one.
- **Verifying the FACT a check is about is not verifying the CHECK**, and that is the version
  that feels like diligence. **Run both directions: which one you skipped decides which way it
  lies** — skip the negative case and a predicate fails toward *present*, skip the positive and
  it fails toward *refuted*, which is the direction that gets believed because refuting looks
  like rigour. And measure an idiom at REAL size.
- **A count that OVERSTATES what is wrong is the same defect as one that understates it**, and
  the tell is an arm reporting a number nobody can explain. The directions are not symmetric:
  an under-report is silent and found late, an over-report is **loud and misattributed**,
  sending somebody to a defect that is not there and discrediting the instrument. A `macro()`
  substitutes its arguments TEXTUALLY, so CMake re-parses them, a backslash is eaten twice, and
  `if(param ...)` inside a macro compares the literal string `param`, silently dead.
- **A claim about a tool is checked against the tool.** A pattern is broader than its author
  reads it as (`pgrep -f "scripts/local.gate"` is a REGEX), a process is attributed by its
  ancestor chain and never by a cmdline match or a leaf `cwd`, and a bound nobody has watched
  fire is untested rather than proven. The general claim and the specific one can point
  opposite ways with nothing to warn you which you hold. And **the tree you measured is not
  necessarily the tree in question** — the tell is that the answer was too convenient.
- A hygiene script `ctest` runs is constrained to **bash 3.2** — macOS ships a 2007
  `/bin/bash`. No `mapfile`/`readarray`, `declare -A`, `${var^^}`, `local -n`; keep the process
  substitution when replacing `mapfile`, or the `pipefail` trap comes back.
- **`std::from_chars` has no FLOATING-POINT overload in libc++ before macOS 26.0** — the
  `macos-14` runners — so it compiles on libstdc++ and on MSVC and fails to BUILD on the one
  leg CI runs it on. The standing remedy for a missing libc++ facility (*grep for it and see
  that macOS already compiles it*) answers YES and is WRONG here: a grep tests a NAME while the
  hazard is a SIGNATURE. `Core/NumericText.hpp`'s `ParseFiniteDouble` is the tree's one answer,
  and it pins the C locale as well.
- A `char` is UTF-8 here, at run time and at compile time: every Windows executable declares the
  UTF-8 process code page and MSVC gets `/utf-8`. Converting one boundary instead would leave
  `path`, `CreateProcessA` and `getenv` on the legacy page.
- `std::filesystem::path`'s narrow constructor THROWS on such a host for bytes that are not
  UTF-8 — before any `error_code` overload runs. `PathFromNarrowText` is the one `catch` in this
  tree.
- Where `clang-debug` will not build, get ASan from GCC (`-fsanitize=address` alone — UBSan
  breaks the option tables' constexpr checks) and run the **whole** suite: a freed block nothing
  disturbs reports nothing.
- Run clang-format and clang-tidy **at the version CI pins**, in a build directory of its own;
  `PATH` resolving to an older binary reports clean in the way that means nothing.
- **And `CLANG_TOOLS_VERSION` pins a MAJOR, not a BUILD.** apt.llvm.org ships rolling snapshots
  under one version number, so two binaries a month apart print the same `--version` and only
  `dpkg-query -W` tells them apart. What makes it expensive is that the remedy reads as already
  applied — you have a binary with the right name, so the rule above looks satisfied while the
  analyser is silent about a check it does not carry — and it invalidates every earlier verdict
  of that session, not just the one file. `clang-format` rides the same stream, so upgrade the
  pair; read `apt-cache policy`, never the `--version` banner.
- A script that NAMES a tool version must name it **everywhere that version matters**. And a
  cached `find_program` result outlives every reason it was chosen, so check the pin against the
  cache, not only pass it. `ctest -R local-gate-selftest`.
- A **reference build passes `-DUSE_COMPILER_CACHE=OFF`, and the gate is a reference build.**
  Pinning the other two tools argues for REMOVING this one, not versioning it: a cache is
  supposed to be verdict-neutral, and requiring "the launcher built from this tree" is unsound
  twice. Passing the flag is not the fact — `CompileCache.cmake` leaves an externally-set
  launcher untouched, so the refusal reads `build.ninja`, never `CMakeCache.txt`.
- **sccache is never selected AUTOMATICALLY** (`ALLOW_SCCACHE_FALLBACK`, default OFF). Being the
  unasked-for answer to `fastcache-cc` being unusable is how this project built through sccache
  for weeks while believing it dogfooded its own launcher. The three Windows jobs opt in — miss
  one and it compiles cold, which their `compile_requests -eq 0` assertion catches. Gate the
  ROW, never have CI set `CMAKE_CXX_COMPILER_LAUNCHER`. **And dropping sccache RELOCATES the
  silence to ccache rather than ending it** — so a rejection matching its row's `predicts` column
  is a WARNING whatever replaced it, *including nothing*, which is the loudest case. A WARNING,
  never `SEND_ERROR`: this module may not fail a configure, and a stale daemon is the normal
  state during a rollout.
- A `cmake -P` check is judged by its OUTPUT, never by its exit code (`FAIL_REGULAR_EXPRESSION`,
  one spelling defined once): `message(WARNING)` exits **0** on every CMake while printing
  `CMake Warning`, and a check that shells out without reading `RESULT_VARIABLE` exits 0 with
  its child's error on the output. `ctest -R fatal-error-exit` ASKS on every platform rather
  than restating a sentence.
  - A failure signal that is a *property* needs both `script-check-canary` (a script that must
    be seen to fail, `WILL_FAIL`) and `script-check-signals` (no registration omits it). And
    **`WILL_FAIL` inverts the WHOLE verdict**, so a canary carried by its exit code is vacuous:
    it therefore exits **0** and prints a real `CMake Error` from a nested `cmake -P`, which
    leaves the pattern as the only thing that can fail it. Fixing a wrong *reason* without
    re-deriving what it justified is how a guard survives as decoration.
- **A guard's REMEDY TEXT is part of the guard. Nothing tests it, and it is the only part of a
  check most people ever read.** It survives every test a check normally gets, because a
  self-test asserts THAT the check objected and never what it advised. The window is a change of
  MECHANISM rather than of verdict: nobody forgets the message when a check starts refusing
  something new; it rots when the check refuses the same SHAPE for the opposite reason. Read it
  as somebody who has never seen it: not *is this accurate* but **if I did exactly what this
  says, where do I end up.** And state what the rule does NOT cover in the refusal itself, or the
  next reader over-applies it and deletes a property that is load-bearing elsewhere.
- Never silence clang-tidy with `NOLINT` — fix the source.
- A return type is not part of a function's mangled name on Linux, so two functions differing
  only in return type silently collide.
- `cmake/portable/CompileCache.cmake` stays stock-CMake-only and must never fail a configure.
  `check_<lang>_compiler_flag` is a hard error for a language the project has not ENABLED, and a
  bad flag in `CMAKE_<LANG>_FLAGS` fails the ABI check — so ask `ENABLED_LANGUAGES` first, and
  CHECK a flag rather than gating on a compiler-ID string. What it computes and appends is a
  function, checked as a computation (`ctest -R debug-prefix-map-rules`).
- A sanitizer that is on in the cache is not one that is on in the build — a tool that silently
  does nothing is worse than one that is visibly off.
- A Windows **Debug** leg is run for `_ITERATOR_DEBUG_LEVEL=2`, not for the compiler, so it runs
  `ctest` rather than only building. Nothing states that level, so `iterator-debug-canary` is a
  program that must die and `scripts/iterator-debug-gate.ps1` refuses a build where it survives.
  Guarded to MSVC Debug, so its absence elsewhere is normal rather than a lost registration.
- So a sanitizer job proves nothing until something proves the sanitizer. `scripts/tsan-gate.sh`
  refuses to report clean until every artefact's OWN OBJECT FILES carry an **undefined**
  `__tsan_init` reference **and** a deliberate race (`src/tests/TsanCanary.cpp`) has gone red —
  run **with** `.tsan-suppressions` active, so no pattern broad enough to swallow an obvious race
  can disarm it. Do not repair that file. A known race lives in `.tsan-suppressions` with its
  issue number; deleting the entry is part of closing the issue, never part of going green.
- That proof is asked of the OBJECTS because it cannot be answered by a binary: `__tsan_init` is
  DEFINED by the sanitizer runtime, which the link pulls in whole, so a canary whose TU was
  compiled with no sanitizer flag still produced a binary carrying it and PASSED. An object
  cannot borrow the symbol. The canary was never the exposed half — an uninstrumented one fails
  closed — the SUITES were. Not `__tsan_func_entry`: that is per-FUNCTION, so an object whose TU
  has no functions carries none.
- The canary's job is to be caught EVERY time, so a change to it is judged by a RATE and never by
  a green run: `scripts/tsan-canary-rate.sh`, a few hundred runs, the number recorded. A gate that
  is red a few percent of the time teaches people to re-run it and is then disarmed as thoroughly
  as if it had been deleted. It races across an array rather than one `int`, because distinct
  LOCATIONS are what the measurements move on — and the file claims no mechanism, since the
  obvious one predicts the opposite of what the data says.
- An edit script asserts its anchor **matched** — `assert count == 1`, count rather than presence,
  so "missing" and "not unique" both fire — and a generator that produced nothing fails instead of
  printing success. Report what changed, not that the script finished.
- `producer | grep -q` is a false **negative** under `set -o pipefail`, and it fails on the
  SUCCESS path: `grep -q` exits at the first match, the producer dies of SIGPIPE, and `pipefail`
  reports the producer's status. **It came back in eighteen sites across twelve scripts**,
  including the two checks that decide which contexts are REQUIRED, so it is now a SCAN in
  `check-e2e-helpers.sh`. A rule stated in the five files that obey it reaches no file that does
  not. **And the hand census that opened the ticket missed five of the eighteen**, because the
  real spelling was `grep -Fxq` — a pattern is narrower than its author reads it as, the mirror
  of `pkill -f`. The remedy is a HERESTRING, which is not a pipe.
- The TSan scope is one Catch2 tag expression, in `tsan-gate.sh`'s `TARGETS` table, READ from
  there and never restated; `ctest -R tsan-scope-hygiene` is in the **default** set. A row is a
  directory OR a FILE, and the file row IS the exemption mechanism. It proves a CASE, not a FILE:
  a case's tag string is the **LAST** string literal of its header, and every way of losing the
  question is a REFUSAL. **A scope selecting NOTHING is a refusal.** The BINARY half is checked
  separately and is not a proxy (`ctest -R tsan-binaries`) — a tag expression says nothing about
  which binaries the gate builds, and a binary the launcher owns is one no tag could reach.
  Adding a row is TWO edits, and **run the binary under TSan before adding its row.**
- **A workaround with an expiry date is WIRED to fire, never written down where only its own
  file's reader will meet it.** The scope is a tag expression rather than `ctest -L` because
  `catch_discover_tests` exports tags as labels only from a Catch2 later than this tree pins, so
  `check-tsan-scope.cmake` READS the declared version and refuses past a WATERMARK. The watermark
  is a pinned CONDITION and must not be made to track `CMakeLists.txt` — point it at its own
  subject and the comparison is `x > x`, false forever, the tripwire silently gone.
- A `paths-ignore` filter on a workflow whose checks are **required** makes a pull request
  unmergeable, not fast: the workflow never triggers, so no check run is created and the required
  context never reports. Master is guarded by a *ruleset*, so the protection API answers `404` and
  tells you nothing. Gate at the **job** level instead — a skipped job still reports — and
  `scripts/ci-scope.sh` is what decides, escalating every way of not knowing to "build everything".
- A **merge queue** is the third door to that same never-arrives failure: it dispatches
  `merge_group`, and a workflow not listening for it produces no check run, so a queued PR *sits
  there*. `pull_request_target` does not fire on `merge_group` at all. Check the concurrency key
  too, state `merge_group` in the scope classifier, and add no JOB to `build.yml`.
  `ctest -R merge-queue-contexts` asserts every required context can report. **Neither the SET nor
  its COUNT is written in prose** — a stale LIST is worse than a stale count, because it reads as
  complete. Nothing GUARDS this, so the rule is all there is.
- A **CONFLICTING** pull request is the fourth door, and the only one the workflow files cannot
  explain: a `pull_request` workflow runs off the MERGE REF, GitHub computes none for a conflicting
  pull request, and it dispatches NOTHING — required contexts ABSENT rather than pending. The tell
  points the wrong way, since `pull_request_target` jobs report normally throughout, so it reads as
  CI being slow. **Order matters more than the fact**: ask `mergeable` BEFORE reading a workflow,
  and on push ask `git merge-tree --write-tree`, which needs no pull request and no API. Resolving
  it, assert the ORDERING of diff3's four markers rather than counting three, and prove the
  resolution with `git diff origin/master HEAD -- <file>` showing no deletion lines.
- A skipped job REPORTS, and a skipped REQUIRED context reads as PASSING. A skipped **matrix** job
  is the opposite: it never expands, so its per-leg contexts never exist and nothing reports at all.
  One passes, one hangs; the difference is the matrix. So never let a dependency's failure skip a
  required gate. `if: ${{ !cancelled() }}`, and check for real — not `always()`, which runs even
  while the run is being cancelled.
- A workflow must not invert its own script's principle one level up: `ci-scope.sh` escalates every
  way of not knowing to build-everything, and the workflow read it as `== 'true'`, so a FAILED
  `changes` published no output and sixteen jobs skipped green. `!= 'false'` everywhere, plus
  `!cancelled()` on every job that consults it. The matrix trap is no longer load-bearing, so do
  not reintroduce a job-level `if:` believing it will catch you. `ctest -R gated-jobs-fail-safe`.
- A gate that does not REPORT reads as a gate that passed, and the *required* clause is not what
  makes it so: five of six failing queue runs failed a job that is not a required context, and all
  five pull requests merged with nobody told. The unit is the CONTEXT, never the job key — two legs
  of one matrix job can differ. Not a JOB in `build.yml` and not a STEP per job, so `workflow_run`,
  after the queue has concluded, gating nothing. Its trigger carries **no `branches:` filter** on
  purpose. `merge_group` reports unrequired failures, `push` to master reports ALL of them,
  `pull_request` reports NONE and says why. A push report is a TRANSITION, opened once, or a context
  failing on every push comments forever; and a push with no branch is REFUSED, never assumed master.
- A step's `env:` is its OWN, and a whole-file grep cannot tell a line that runs from one that
  cannot: a variable defined on one step and read by another died on `unbound variable` and opened no
  report in its entire life. Its own guard passed, correctly — a whole-file grep satisfied by a line
  inside the step that cannot run is a rule satisfied by prose. So the rule is per STEP, and it
  covers EVERY `run:` and not only the `set -u` ones: without `-u` the name expands to EMPTY and the
  branch is taken the wrong way silently, which is worse. The runner vocabulary is an ALLOWLIST, the
  model of bash is deliberately narrower than bash, and the refusal names the STEP.
- Every check whose SUBJECT is documentation was skipped on exactly the change it exists to catch,
  because `code=false` is right for a compiler and backwards for prose. Prose drifts by being
  EDITED. The set is the `docs-subject` ctest LABEL, read out of `src/tests/CMakeLists.txt` and
  never restated, and every way of not being able to run one is a REFUSAL, never a skip. It runs
  from an UNGATED step of the job whose `name:` is the required context `Check C++ style`, because
  reporting without gating is the previous bullet's defect. That `name:` is a wire constant.
- **A flag combination that cannot express the question still returns an answer.** Same species as
  `pkill -f` and `grep -q` under `pipefail`: the shell obliges regardless and **none of them
  errors**. Cleanest specimen `grep -Lq`, where the contradiction is internal. Worst of the family
  for three reasons, and the middle one generalises past shells: it was a **positive control**, and
  *a control is the one instrument nobody checks, because checking it is what it was for*; its error
  pointed at **refuting** a claim, the direction that gets acted on because refuting feels like
  diligence; and nothing about the output looked wrong. Re-derive a control by a different
  construction before trusting it.
- **An intermediate reading is BIASED, not noisy.** A tree sampled mid-build can only be MISSING
  artefacts, never carrying extra ones, so the error is one-sided by construction — always toward
  failure. So the natural response to a mid-build failure, investigating it, is the wasted motion,
  and no care in *interpreting* the reading helps. **Do not take the reading** — wait for the
  completion signal.
- **CMake WRAPS its diagnostic messages**, so a phrase you grep for can exist in the output and in
  no single LINE of it. The `STATUS` row is the trap inside the trap — it does NOT wrap, so a
  negative test written with it reproduces nothing and reads as a refutation. It is the DIAGNOSTIC
  types that wrap, and that is a property of every verdict this repository reads: a check's verdict
  travels through the DIAGNOSTIC channel because `FAIL_REGULAR_EXPRESSION` must also hear one that
  merely WARNS. Flatten (`tr '\n' ' ' | tr -s ' '`) before matching, or match a phrase that cannot
  straddle 74 columns. The tell: three arms agreeing perfectly is what a broken instrument looks
  like as well as what a real pattern looks like.
- Five ways an instrument reported on something other than its subject, all in one branch, all
  written by someone who had just read the rulebook. A **COMMENT is not a call site** — so strip
  full-line comments, and self-test both directions. **`bash <path>`, never a bare path**, in a
  self-test AND in a workflow. **`IFS=$'\t' read` does not read TSV**: tab is IFS whitespace, so an
  empty field collapses and shifts every field after it. **A fixture built on `message(FATAL_ERROR)`
  cannot test that verdicts are read from OUTPUT**, since the status alone is then sufficient. And
  **a self-test that stops early must not look like one that judged something**, so they print how
  many cases they ran.
- A bracket-vulnerable `cmake -P` reader is judged per **(reader, file, surviving lines)** and never
  per script, because the verdict flips on the corpus alone: two of the six remaining readers refuse
  LOUDLY, two pass SILENTLY over a real violation, one is unchanged **by coincidence**, and only one
  is safe for a reason. A reader left alone because today's files happen to be safe is a defect
  scheduled for later.
- **And the usual remedy is wrong where the brackets are the DATA.** Blanking `[`/`]` before
  splitting made the TSan scope check report its table as naming no Catch2 tags at all — a tag IS
  `[async]` — and refuse on a good tree. That reader is a list-free `FIND`/`SUBSTRING` walk instead,
  immune by construction. Consolidating the copies of the splitting idiom is **#495**, not the
  ticket in front of you: absorbing it closes one ticket by swallowing another.
- **`if(VAR STREQUAL "")` does not fire when VAR is UNSET**, and the one place that matters is a glob
  that came back empty: `set(x ${empty})` unsets `x`, and CMake then reads the left operand as the
  literal string `x`. Quote the variable. Siblings assigned by `set(x "")` are fine, which is why
  only one instance broke.
- **A clean-tree injection understates a check that only reports violations**, so plant the
  violation. Three arms, and the third is not decoration: violation alone, violation behind a
  bracket, bracket alone — without the last, a check that refused every bracket would pass the
  middle one for the wrong reason.
- **A census cannot falsify a premise — it can only produce a number consistent with it.** Re-derive
  an inherited claim BEFORE the census, never after: afterwards it has already told you what you
  expected to hear. **Agreement between a fresh measurement and a source you have not read is worth
  nothing**, and it *feels* like corroboration.
- **A listing that came back AT its `--limit` is an answer about a set that is not the whole set**,
  and nothing about the rows says so — the cap reads as the total, wrong in the unsuspicious
  direction. Raising the limit moves the cliff and hides that there is one. `ctest -R
  gh-listing-seam` requires every `gh` listing to go through a seam that warns at the cap, or to
  carry a stated reason. Ask a question truncation cannot reach where one exists.
- **A census states its PATTERN, not only its number.** Two independent audits of one file set
  differed by exactly one and neither had miscounted, the whole difference being a filename that
  contains the pattern. **A pattern is broader than its author reads it as** — and the second
  instance was a manager quoting a corrected number back, wrong for the same reason as the thing it
  corrected. Nobody is outside this: the remedy is that the pattern travels with the figure. And
  **a ticket cannot be closed against a count that no longer describes the tree.**
- **A total stated beside a table is DERIVED from it, or it is a second claim** — a hand-maintained
  number describing a hand-maintained list is two sources of truth wearing one hat, and it drifted
  three commits running, in one file, in one day, each commit fixing the last count and introducing
  the next. `ctest -R table-totals`. The multipliers are PARSED, not banned, so a checker that counts
  ROWS is wrong for exactly the table that motivated it. The marker is MANDATORY with `none` as the
  opt-out, because opt-in is silent about a table that never opted in. And the figure that describes
  a table is the one NEAREST it.
- `clang-format -i` at any version but the pinned one silently reformats code the pinned one already
  accepted; run an older binary as `--dry-run` only. Both pinned tools ship on PyPI, so "the distro
  only has an older one" is not a reason to use it. An older clang-tidy is worse than a laxer one: it
  is *silent* about checks that do not exist in it yet.
- A clang-tidy sweep that cannot prove the tool ran is worth nothing and reads like success —
  `scripts/tidy-sweep.sh` canaries it first and treats a failure to execute as fatal, never as "no
  findings".
- A ccache hit does NOT skip clang-tidy: the launcher and the analyser are two independent commands
  under `cmake -E __run_co_compile`. On a pull request CI therefore tidies the diff plus every
  translation unit that includes a changed header, and anything that changes how EVERY unit is read
  gets a row in `SweepEverythingWhen` — a missing row is a sweep that checks the wrong set and prints
  a confident count.
- A compile database generated for clang-tidy needs `CMAKE_CXX_SCAN_FOR_MODULES=OFF` named
  explicitly. Without it every unit fails to parse and the sweep reports clean.
- And it must be configured with the same TARGET SET CI builds. A sweep whose scope comes from a
  database is only as complete as that database's targets, and a target gated off by default is
  invisible to it rather than absent from CI. The script cannot catch it — a changed file with no
  compile command is dropped silently, and must be, since that is also what a platform-specific TU
  looks like. Account for every file in the diff the sweep did not reach, before trusting its count.
- **Running the launcher is not testing it.** The synthetic fixtures prove it RUNS and produces AN
  object; only building a REAL target through it and running that target's tests catches a WRONG
  one. `scripts/launcher-replay-e2e.sh` builds three times — cache-off control, cold (stores), warm
  (REPLAYS) — and runs the replayed binary. Compare cold-against-warm, never control-against-warm: a
  launcher-active configure disables PCH and module scanning. The cold build must be seen USING the
  launcher, the warm one seen HITTING. And staging a wrong object needs the LINK command, never
  `touch`: ninja records output mtimes, so a replaced object reads as dirty and is rebuilt, undoing
  the injection silently.
- A compiler cache that reads like success is worse than none: the Windows sccache was running into a
  directory the runner deletes, so the jobs are asserted to be backed by the Actions cache — before
  `ctest`, which restarts the sccache server and zeroes its counters.
- Every `bool` and byte-wide enum in a config struct lives in one run: one between two 8-aligned
  members costs seven bytes, and clang-tidy's padding budget fails the build.
- A table indexed by an enumerator is `EnumTable<Enum, Row>` + `RowsInEnumeratorOrder`. A length
  anchored on an enumerator by name is a guard that fires only when nothing is wrong.
- Coverage is Clang source-based, never gcov: ~2000 Catch2 cases are ~2000 processes, and gcov's
  shared `.gcda` races them. `%8m`, not `%p`. `*_test.cpp` sits next to the implementation, so a
  report that counts it measures the tests testing themselves. A compiler cache and coverage cannot
  be combined — a replayed object's embedded mapping names the tree it was built in.
- A rulebook `## Open work` entry names an OPEN issue, or it is a rule that has gone false — the
  expensive shape being an entry saying something *cannot* be done, which instructs the next session
  not to try. `ctest -R rulebook-open-work`. The ENTRY is a bullet's LEADING reference; a citation in
  its prose names the landed change that produced the residual and is correctly closed. `gh` falls
  back to PULL REQUESTS asymmetrically, so the KIND is asserted too. FOUR outcomes, not two, and of
  the three sharing exit 1 with an empty result **two are the checker's own fault** — read as *stale*
  they invent a finding somebody then edits a correct entry to satisfy — so the verdict is the HTTP
  status behind a liveness anchor.
- **A configure's OUTPUT is the module's CLAIM; the generated buildsystem is the artefact.** A module
  printing the right status line at the right severity while wiring NO launcher passes every row, and
  the cache cannot answer it — the launcher is a NORMAL variable, unset in `CMakeCache.txt`. Read
  Ninja's per-rule `LAUNCHER =` or the Makefile compile line. A fixture needs a TARGET, two stand-in
  launchers must be two PROGRAMS, and a generator the reader cannot parse is a THIRD state, named —
  while a generator it CLAIMS to handle reading nothing is a violation. **Its first version was green
  on Linux and red on all three Windows legs for one and the same file**, because a comparison
  written against the spelling ONE generator on ONE platform emits is a comparison nobody has tested:
  normalise a path as a PATH, on BOTH sides, and re-run the MUTATION on the platform that failed.
  Its sibling: **the DECISION a guard makes is what gets tested, not the acquisition around it.**
- A branch BEHIND master is unverified, and only a build says otherwise: its green checks are a true
  statement about the tree it was branched from. **How far behind is not a measure of the risk** — it
  measures elapsed time, while the defect is a collision that either exists or does not. **File
  overlap is evidence only in the direction that says there IS a hazard**; its ABSENCE is evidence of
  nothing. The hazard is a NAME shared between two changes, not a line shared between two diffs, and
  a diff cannot show a name it does not mention. NEITHER branch has ever gone red, which is the point
  rather than a contradiction: the failing tree is the combination. So a branch is rebased and
  rebuilt before it merges, never inspected.
  - **And the diff that VERIFIES such a rebase is three-dot.** `A..B` folds in the commits `B`
    carries and `A` does not — the ordinary state right after a rebase — so it answers a question
    nobody asked, and produces a wrong NUMBER rather than an error inside the step that exists to
    verify. The two forms disagreeing is itself the signal that the branch is behind its base.
- **After a revert, test for the REVERT, never for the defect.** A revert leaves the reverted commit
  in the ancestry forever, so `--is-ancestor <fix>` answers YES for every branch and discriminates
  nothing; the only useful question is `--is-ancestor <revert>` coming back NO. The instinct is to
  check for the thing that broke you, and it is the one test that cannot work.
- **A subject line is an abbreviated identifier with no prefix to disagree about.** Two commits here
  carried the identical subject and opposite revert status, and two people measured correctly and
  reported contradictory answers. A truncated SHA at least *looks* like an identifier and invites
  comparison; a subject line looks like a description. Same family as a ctest index, two worktrees
  one token apart, and a leg name without its compiler — **a leg travels with its compiler and its
  machine.**
- **A reason that generalises further than the fact it was drawn from is worse than the narrow one**,
  because it reads as licence somewhere it was never measured — and it is introduced while TIDYING,
  which is when it is least likely to be re-checked. Measured: a true reachability reason was
  "improved" into a false semantic one during a `/simplify`, a later review faithfully propagated it
  into two rulebook files, and the header then argued both sides.
- A reference-build refusal is a REFUSAL and not a warning, because a verdict about a tree that was
  not built cannot be read in EITHER direction: a substituted object can equally HIDE a real failure,
  and nothing would say so. That sentence lives in the refusal's own text, or whoever meets it argues
  for a warning. And the count is LAUNCHER **BINDINGS**, not edges — ninja emits one per RULE — so
  **a unit error in a refusal message is how two numbers get compared that should not be.**
- **A diagnostic that never RAN and one that ran and found nothing are the same green.**
  `continue-on-error` is RIGHT for a probe and is also exactly what hides a probe that could not
  start. Keep the flag; add the missing half — **assert the classifier was REACHED** (a verdict as
  `::notice`, its absence as `::warning`) — and give it **three** outcomes, since a probe that can
  only answer the two you expect will answer one of them whatever it sees. And **a green test on the
  wrong object is worse than no test, because it RETIRES THE SUSPICION**: ask not *is this tested*
  but *is the thing tested the thing that ships*, and where they differ as TEXT, extract the step's
  own `run:` block from the workflow and execute THAT.
- `PEDANTIC_COMPILER_WERROR` decides **fatality, not which warnings exist**, so a flag and the
  suppressions it makes necessary are governed by ONE condition — split, a build directory reused
  across presets holds `PEDANTIC_COMPILER` ON with `WERROR` OFF, which is a database no correction to
  the configure line explains. And **a symptom with two mechanisms reads as unreproducible the moment
  either one alone is ruled out.**
- **When the SUBJECT under test is the build environment, a green local gate is not weak evidence —
  it is none.** One change, one afternoon, four platform defects, none visible locally: a `/_deps/`
  DENYLIST that CI's in-repo CPM cache walked past (an exclusion list bets on the world's layout; an
  inclusion list states your own), a POSIX shell stub spawned by Python on Windows (**ENOEXEC**), a
  heredoc inside `$( )` that bash 3.2 cannot PARSE, and Git Bash rewriting a `/`-led MSVC flag into a
  path — `MSYS2_ARG_CONV_EXCL='*'` **and** `MSYS_NO_PATHCONV=1`, both spellings, always. The mangling
  was the LUCKY half: the same GNU-only strip left `/c` and `/Fo<obj>` standing, which SUCCEEDS, and
  a coverage check then reported CLEAN over the six files it exists to read. Drop flags from a TABLE
  keyed on the driver NAME, never by sniffing a leading `/`. And a mode that NAMES its set may not
  report clean over a member it could not cover. Knowing a rule and having just applied it is not
  protection: the ENOEXEC was the same author's own fix from three hours earlier.

**[`.agent/rules/testing.md`](.agent/rules/testing.md)** — how tests are registered
and what they may assume.
- `ctest --repeat until-fail:N` reports the LAST iteration, so a 1% flake reads as
  `100% tests passed`. `scripts/flake-rate.sh` keeps a tally, keeps the early stop, and states
  its N on every line, because a stability claim without one invites the inference it cannot
  support.
- **A wall clock is not a duration.** `SECONDS`, `TIMEFORMAT='%3R'` and `date` all read
  CLOCK_REALTIME, which a VM host's time sync steps BOTH ways. Assert what a helper DECIDED —
  the pause it REQUESTED is exact and host-independent — or time it against something monotonic.
  **The variable is the ENVIRONMENT, not the load**, so a clean run cited against a clock-step
  ticket names its environment or is unreadable. **A shape guard is not enough**: a well-formed
  reading can be an order of magnitude short of the work it describes. And it is
  **BIDIRECTIONAL** — a forward step SHORTENS a bound, so a healthy wait gives up early and reads
  as a slow runner, which is the direction that gets "fixed" by raising a budget. **A derivation
  is only as sound as the premise it does not state**: the algebra here rested on *the clock does
  not move*, which nobody wrote down and a census cannot falsify.
- **Assert what DISTINGUISHES, not what both sides produce.** Four lanes in one evening found
  five tests that could not fail for the reason they existed, three of them acceptance criteria
  written by whoever understood the defect best. Each asserted something the healthy AND the
  broken state produce. A refusal test asserts WHICH refusal. **Prove the test can fail** —
  neuter the fix and check the failures are the ones you expect AND ONLY THOSE; the asymmetry is
  the evidence. "What would prove this fixed" and "what would fail if it were not" are different
  questions, and only the second one tests anything.
- **Three fixtures that could not fail came out of one batch, none found by reviewing the
  assertion** — one watched an object no consumer holds, one used a candidate an earlier guard
  refuses so it never reached the code under test, one asserted a string BOTH refusals contain.
  One instance reads as bad luck; three read as the default outcome. What found them was
  neutering the fix, building the neighbouring case, and asking how production ACQUIRES what the
  fixture acquires. Reading harder finds none of them.
- A fixture that RE-ACQUIRES a collaborator the production code binds ONCE is testing a different
  object, and its assertion can be exactly right while the case is green over a live defect. Bind
  it the way the call sites do, before the mutation, and read back through that binding. Covers
  any cached seam — a reference, an iterator, a `shared_ptr` snapshot, a resolved endpoint. The
  tell is that the fixture is MORE CONVENIENT than the production code, which is when nobody
  re-reads it; it is the mirror of *a fake more permissive than the thing it stands for*, with
  the CALL PATTERN as the fake, so reading the fake never finds it.
- Every wait is bounded and says what it waited for — and, when it times out, which KIND of
  failure it was. A slow machine and a wedged process are fixed in different places, so a wait
  records what tells them apart: the cost on success, whether the process is still alive, whether
  the log grew, and how much CPU it burned. The last one is not optional — an include-tree walk
  logs nothing while it runs. Where the signals disagree, say INCONCLUSIVE. **And an INCONCLUSIVE
  verdict is a place to ask somebody ELSE**: the next move is a different instrument, not a better
  reading of the same one — asked on the FAILURE PATH, every command `|| true`, because a
  diagnostic on an already-failed case must explain the verdict and never change it.
- A **cumulative** figure cannot answer a question about **now**, and a duty cycle over the same
  window is the same number divided by the same constant. Draw the verdict from a RECENT window
  and print the totals as evidence only. No magnitude bar calibrates, but **zero does not vary**,
  so test for presence, measure the process TREE, and report the band between idle and clearly
  working as neither.
- An `-or` is right for two independent CONFIRMATIONS and wrong for two competing READINGS — the
  disjunction lets the weaker win unopposed. A signal that cannot be false in the failing case is
  not evidence. And an instrument that prints a **remedy** cannot know when the remedy is under
  dispute. State the finding and stop.
- A classifier that cannot be made to say BLOCKED cannot report a hang, so each verdict is driven
  against a synthesised readings record, in the default set.
- A stand-in built to exhibit a MAGNITUDE must not be measured through an instrument whose own
  overhead is comparable to it — no arrangement fixes that, because the noise IS the interpreter.
  Split the DECISION out as a pure function over a record; leave acquisition alone.
- A fixture waits on what a line MEANS, not on its wording: a marker whose sentence stayed the
  same while the fact behind it changed silently un-serialised three concurrent walks. The budget
  was the symptom; raising it would have bought a fixture three times slower with the cause
  buried. Wait on the STAGE, and keep the phases separate so a stall says which.
- **A fixture that has never completed has told you nothing**, however carefully it was read: one
  was reviewed and merged into a CI job and its first run anywhere died on the first line that
  starts a process, behind which sat three more defects. A flag spelling is checked against
  `CliOptions()`, never remembered. `exit` inside a `( ... )` ends the subshell only, so a `fail`
  helper signals the top-level shell unconditionally.
- **A guard written to prove a fixture bites can itself fail to bite**, and then announces a true
  observation carrying a false claim — which names the subject as broken when the instrument is.
  An injection is ASSERTED, in the artefact and again in the thing that consumes it.
- **Attribute by asking the process, never by adjacency in interleaved output.** Reading the
  progress line above each outcome split one unit differently across two runs of the same build.
  A wrapper that labels the unit on the same stream, from the same process, inside the same edge
  cannot be separated from what it labels; an unlabelled outcome is counted as NEITHER and refused
  by name. And an unattributed total hides the only column that means anything.
- A `$<TARGET_FILE:x>` naming a target that was NOT built is a hard error at **generate** time,
  not a skipped test, so the whole configure fails and the message names CMake rather than the
  option the operator set. Guard the block on `TARGET x` as well as on whatever feature makes the
  test interesting — the two are independent. `ctest -R target-file-guards`, in the default set,
  reads the optional targets from `src/apps/CMakeLists.txt` rather than restating them.
- A fixture whose client is always LOCAL cannot test who is admitted: `Classify` returns `Member`
  for the whole of `127.0.0.0/8` before it reads the member list. A second loopback address does
  not reach that list, and changing to one looks exactly like a fix; the host's OWN non-loopback
  address does, with no second machine. Assert BOTH directions, with the refusal COUNTER moving
  and the scheduler admitting the client in both so the refusal is the worker's. The fixture's own
  liveness probe is a caller too, so the refusing leg's baseline is one rather than zero. A host
  with no such address reports SKIPPED, loudly and as its own ctest test — a quiet fall back to
  loopback is a pass for a case that never ran, which is the defect itself.
- A script-driven test naming more than one executable is registered in `src/tests`, not beside a
  binary.
- An abbreviated identifier is a DISPLAY form: the full one is read, never padded, truncated or
  re-derived. A fabricated SHA shares its prefix with the real one, so every human-readable trace
  reads correctly and only the raw error disagrees. **Zero rows is not a verdict, it is the
  absence of one**, so a response's SHAPE is checked before any conclusion is drawn and an
  unparseable answer is a hard failure, never a quiet retry. Before concluding "nothing there",
  state what was searched and whether that search could have found it. A census returning zero
  gets a POSITIVE CONTROL: this repository sets `grep.lineNumber`, so `git grep -h` emits a
  `<lineno>:` prefix and an anchored pattern matched nothing while the true answer was in the
  hundreds. Nothing errored, nothing was empty, and zero was the expected answer.
- Tests allocate their ports per run rather than fixing them — from **below** the kernel's
  ephemeral range, and remembered, because a connect probe cannot see a port already held as an
  outbound connection's local endpoint. **A fixed one needs a reaper**, or a run that misses its
  cleanup leaves a daemon up for the life of the machine. A holder is **refused, never adopted** —
  a leftover is of unknown vintage and may hold another build's store — except one whose image
  path is byte-for-byte this run's daemon, which is reaped; a holder whose path cannot be READ is
  refused, or the reap arm kills a process nothing knows. And the DECISION is what gets tested,
  over staged records and real listeners, REGISTERED-and-skipped where the interpreter is absent.
- `Unwrap(x)` after `REQUIRE(x.has_value())` for `std::optional`; a bare `*x` is a build failure.
- A Catch2 case name is an ARGUMENT, and that one fact has three consequences. It may not begin
  with `-`. A **COMMA** splits the spec, so a name containing one selects NOTHING and the run
  answers `No tests ran`, which exits **non-zero** — so the miss presents as a *deterministic red*
  rather than an empty result. That is not a reason to rename: the sentence-shaped convention is
  right, so select by TAG, and **anything selecting a subset asserts how many cases RAN**, not only
  how many failed. Write it as *the name is an argument*, never as *commas are bad*, or the next
  separator character is a new ticket.
- A DUPLICATED case name registers two ctest entries that each run BOTH cases. Nothing is skipped —
  what breaks is ATTRIBUTION, so it reads as harmless: one defect surfaces as two reds naming
  neither case, and `ctest -R "^<name>$"` cannot select one of the pair. `test-name-hygiene` refuses
  duplicates, its exemption rows carry a REASON, and a row that has stopped describing a duplicate
  is refused as STALE.
- **A failing `REQUIRE` above an explicit `Stop()` turns a RED into a HANG.** Catch2 unwinds, so the
  failure skips the stop and `~jthread` joins a loop nobody stopped: the timeout does not name the
  assertion, and a timeout and a wedge look identical. It concentrates in TEARDOWN tests, whose
  author is thinking about the subject's ordering rather than the harness's. `CHECK` does not unwind
  and is not this. Fix by stopping BEFORE asserting, or by RAII — where **declaration order is half
  of it**, the fake outliving the thread that touches it, or the hang becomes a use-after-free.
  **Not soundly mechanizable**, so no check is written, deliberately: a textual scan is approximate
  in both directions and would refuse legitimate raw threads on arrival, and a check that fails on
  arrival gets disabled.
- A fixture states which PATH it exercised. A synthetic tree is not a git repository, so a check
  deriving its file set from `git ls-files` with a directory-walk fallback had six green self-test
  cases exercising the FALLBACK while CI exercised GIT. **The mode under test was not the mode in
  use**, which is a guard passing because it is testing something else. So the mode is part of the
  OUTPUT and asserted on both sides.
- A Catch2 `SKIP(...)` exits **4**, and ctest must be told what that means or it scores a skip as a
  FAILURE — a false RED, and whoever meets it deletes the SKIP rather than suspecting the
  registration. **And the mechanism that fixes that RED is itself broken**: Catch2's exit code **is**
  its failed-assertion count clamped at 255, so `SKIP_RETURN_CODE 4` scores a case failing exactly
  four assertions as *skipped*. Genuinely failing tests, silent, inside `100% tests passed`. **The
  repair for one state collapse was the site of the next, inside the mechanism chosen to prevent
  state collapse.** It is still what the tree carries, because **all three alternative channels were
  measured and every one is worse** — the exit status is fully occupied, the OUTPUT is a substring
  search over text the SUBJECT controls and cannot be anchored through `cmd.exe`, and
  `FAIL_REGULAR_EXPRESSION` does not outrank `SKIP_RETURN_CODE`. A `catch_discover_tests` property
  value is a **wire format with three parsers in it**, and **no Linux gate can reproduce any of
  that.** The fix is a change of MECHANISM, open as #1152; `catch-skip-exit-collision` asserts the
  premises and `src/tests/CatchSkipCanary.cpp` carries the shapes a replacement must survive.
  **Adding shapes cannot make a pattern safe.** Not `WILL_FAIL` anywhere either — a skipped test
  carrying it is not scored a failure, so such a canary is green under the very defect it guards.
  And the list is written at BUILD time, so a reconfigure alone leaves a stale one that reads
  exactly like a current one.
- The converse, and the direction nobody investigates: `SUCCEED` is right when a case RAN and had
  nothing to assert, and wrong when the case could not run — where it stands in for a skip it
  reports a PASS for a property nothing established. The twenty-one sites were all
  environment-conditional, so the green arrived exactly on the runs where coverage is thinnest.
  "Covered by another test" is a reason to SKIP, never to pass. It spread by IMITATION under a
  comment that was already correct, so the guard is a check on two signals — a bail-out `return`
  after the `SUCCEED`, and a table of skip vocabulary in its message. A `SUCCEED` it cannot read is
  refused, not cleared.
- A scratch directory comes from `src/tests/ScratchPath.hpp`. A per-process counter is not unique —
  `catch_discover_tests` gives every case its own process, and the suite runs in parallel.
- A test FAKE is a shared helper too: `src/tests/ScriptedSocket.hpp`. A fake nothing exercises does
  not report its own bugs.
- And a fake that resolves SYNCHRONOUSLY what production SUSPENDS on cannot exercise a suspension
  protocol, however correct its assertions — nothing is wrong with the fake, which is what makes
  this the harder half of the rule above. Every property defined by parking is vacuous over
  `InMemorySocket`. The survey of which such properties have a real-socket case, and which have
  none, is in the rules file.
- So is a BUILDER, and it hides better: `src/tests/ForeignGenerationValue.hpp`. Hand-rolled copies
  fail SILENTLY, because every one asserts a REFUSAL and a value damaged another way is refused too
  — so a field moving leaves each copy stamping something different while every case goes on passing
  under a name for what it no longer builds. An assertion review finds none of them. Two facts live
  in the helper alone: WHICH byte carries the generation, and WHICH generation is foreign — DERIVED,
  never `CompileValueVersion + 1`, which stops being right at the top of the reserved range. A
  hand-built FRAME this build could never have encoded is a different subject and must not be routed
  through it.
- The POSIX shell fixtures share `scripts/lib/e2e-common.sh`, tested by `ctest -R
  e2e-helpers-selftest`. A wait's bound is read from a **clock** and its timeout reports the
  **measured** elapsed — a duration nobody ever observed, derived from assuming the machine was
  fast, is the one reading that says whether it was slow. A bespoke condition is a predicate passed
  to `wait_until`, never a new loop. And `fail` signals the top-level shell unconditionally rather
  than testing `BASHPID`, which is bash 4.0+ and silently inert on macOS's 3.2 — across **every**
  script under `scripts/`, since a scan that read only the library missed a script arguing in a
  comment that its banned guard was correct. The file set is WALKED, refuses when it matches
  nothing and when the token table empties, and a tracked `*.sh` outside `scripts/` is refused by
  name rather than quietly excluded. A file that matches its own scan by construction exempts a
  REGION, never itself.
- **A background helper in these scripts runs the fixture's CLEANUP until you have watched it not.**
  Anything forked into the background inherits the shell's traps, and in a fixture the EXIT trap IS
  the cleanup — so a helper signalled catchably deletes the workdir of the run still using it, and
  nothing in the failure names a trap, a timer or a cleanup. `trap - EXIT TERM INT HUP` first in the
  subshell is the fix everybody reaches for and is **insufficient**: it covers a subshell that has
  STARTED, and the window that fires is the one BEFORE that. Measured, `trap -` alone is
  indistinguishable from unfixed while `kill -KILL` is decisive. BOTH guards stay, each naming the
  window it closes, or the redundant-looking one is deleted. It is also VERSION-DEPENDENT and does
  not reproduce on bash 5.2, so a green Linux run is not evidence. Two standalone probes reproduced
  none of it — the first having sent the subshell's output to `/dev/null`, which is where its
  evidence went.
- A fleet property that spans two machines needs `src/tests/FleetHarness.hpp`, whose `OnCompile`
  places the interleaving rather than waiting for one. It is in `src/tests/` and not beside
  `RaftClusterHarness`, because a fleet spans the library AND the apps, and `src/FastCache/` must
  not include an app header. A harness earns its place only by a property shown RED when its rule is
  removed — and prove it with a case that stays GREEN under the same break, or the suite is
  measuring nothing.
- A leader-pinned **mutating** command is put to whoever leads NOW, re-derived, never to an endpoint
  an earlier section recorded. The retry keys on the answer the caller ASSERTS, never on a
  recognised refusal wording: that refusal has TWO spellings, so a fixture matching them stops
  retrying the day either is reworded. Which is also what lets one helper assert a REFUSAL. **A
  helper implementing a rule does not spread it, only a call site using it does** — this exact rule
  sat correct in both halves, in the same file, above two call sites that lacked it.
- A first failure MASKS its identical siblings: one site was never observed failing, not because it
  was sound but because a byte-identical one earlier failed first and the run never reached it.
  Fixing only the OBSERVED site relocates the flake rather than removing it, and the relocated one
  then presents as a regression introduced by the fix — no failure history, first seen in the run
  after the change. Which is the argument for the audit: nothing about the sibling looked
  suspicious, and instinct would not have found it.

## Issues and pull requests

Labels here have teeth: a pull request carrying no `type/` label **fails a check**,
because that label decides which section of the generated release notes the change
lands in (`.github/release.yml`), and nothing downstream can recover it afterwards.
CI derives `area/` and `os/` from the changed paths and reads `type/` from a
conventional-commit title when there is one — a prose title, which most of this
repository's are, means setting the label by hand.
[`CONTRIBUTING.md`](CONTRIBUTING.md) carries the taxonomy and the reasoning.

A label applied by hand can be **destroyed by CI seconds later**, and the gate then
fails for a pull request that was labelled correctly. `actions/labeler` finishes with
`setLabels(...)` — a full replacement computed from the labels it read when its run
started — *whatever* `sync-labels` is set to. `pr-labels.yml` brackets the action with
a remember/restore pair and warns when it fires; if a `type/` label vanishes, re-apply
it and read that warning rather than assuming the gate is flaky (#347).

Deferred work is a GitHub issue linked from the matching rulebook file's
`## Open work` section, never a residual recorded only in prose.

**A ticket is a claim about a tree, taken once, and every merge since is an unrecorded
condition change — so a premise is CHECKED against the tree before it is built on, never
read.** The rulebook carries this for performance figures (*a quantity UNDER CONDITIONS,
and the citation is where the conditions get lost*); a ticket is the same object with a
longer half-life and no units, and the difference is that a figure at least LOOKS like it
might be stale while a sentence does not. **Not one instance has been visible from reading
the ticket** — every one was found by checking, and the check is cheap: a `grep` for a
cited symbol, `gh issue view` on a cited number, `git log -S` on a cited claim. **The cost
is asymmetric**: a stale figure makes somebody re-measure, a stale premise sends a whole
lane to build against a tree that does not exist.

Three consequences, none of which follows from the headline:

- **The ticket may not be OPEN.** Work has been dispatched against issues closed days
  earlier, one of them escalated and decided at owner level. Asking `gh issue view` is one
  call and it is not the same question as *is the body accurate*.
- **"Does not reproduce" is a first-class outcome with a DELIVERABLE**, not a close and not
  a shrug: the falsified claims with `file:line`, plus **a test at the production seam that
  stays green**, committed `Refs #N` rather than `Closes #N`. A test that records why a
  ticket does not reproduce is worth keeping, and it must not silently close a design
  question underneath it.
- **A ticket whose acceptance clause cannot be EXECUTED is a different failure from one
  that is merely stale, and it can be closed by nobody, ever** — a clause naming a removal
  from a reply that carries no such field could not have been shown red even on a tree
  where the defect existed. The other variant is a ticket dispatched as buildable that is a
  signature record by design, with no acceptance clause at all. Separate these from
  stale bodies rather than filing them together. This generalises the counts rule in
  [`.agent/rules/build-and-toolchain.md`](.agent/rules/build-and-toolchain.md) — *a ticket
  cannot be closed against a count that no longer describes the tree* — from counts to
  claims.

When several sessions work this repository in parallel — a manager plus two or three
developers — the lane ownership, rebase and merge protocol, review gates and the
type-label check's cancelled-versus-failed distinction are in
[`.agent/guides/team-run.md`](.agent/guides/team-run.md). It carries no board state by
design; what is done and what is left lives in the issues.

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

### Caching an expensive repeated answer
**An answer that costs a spawn, a syscall or a walk, and is asked for more than
once, is cached — but only where staleness degrades safely.** That second clause is
the whole rule; without it this principle is a licence to serve wrong answers
quickly.

**Decide safety first, because it decides whether to cache at all.**

- **Staleness that costs a refusal, a miss or a retry is safe to cache.** The
  local-address set behind the node's cache gate is this shape: an address added is
  refused until the next refresh — it fails **closed** and self-heals.
- **Staleness that produces a wrong answer which looks right is NOT cacheable**,
  however expensive the probe. `DiscoverTargetTriple` costs ~40 ms per translation unit
  and is deliberately *not* memoized, because the triple goes into `compilerId`: a stale
  one is **a wrong hit, not a miss**
  ([#188](https://github.com/LASTRADA-Software/fastcached/issues/188)). Expense is not
  the criterion; what a stale answer *does* is.

**Then, if it is safe:**

- **A performance figure is a quantity UNDER CONDITIONS, and the two halves get lost
  separately.** Attaching conditions is necessary and **not sufficient: the citation is
  where they get lost**, so a figure others will refer to lives in ONE place they point
  at, never restated. A figure can be current, correctly measured and still **the wrong
  quantity** — quoting a warm cost as the price of MISSING a cache is circular. Record a
  table of conditions, not a number.
  - **The same rule governs a claim handed between PEOPLE, and a handoff IS a citation.**
    Worse than the comment case: a reader who doubts a comment can re-measure, while one
    who is HANDED a claim usually cannot, and a sentence looks identical whether it was
    measured, inferred, remembered or guessed. The remedy is on the SENDING end because
    only it can be: **say what was MEASURED and what was INFERRED, separately, every
    time.** The receiver cannot recover the distinction at any price.
  - **And the receiver owes one thing back: state what would FALSIFY a claim BEFORE
    opening the file to check it.** A handed-over shape arrives already sounding checked,
    so reading for CONFIRMATION stops at the first line matching it — reading the source
    is not enough on its own, and has produced the wrong answer twice in an hour. **A
    relayed diagnosis is relayed code.** Derive the falsifier from the claim itself, first.
  - **And that never-restate sentence does NOT reach a measurement's CONDITIONS — pin
    those, do not point at them.** The test is whether the two copies are supposed to stay
    EQUAL: a live figure has two copies meant to agree, so one copy and everyone points at
    it; a measurement's conditions are the world at one INSTANT and must NOT track their
    source, or an unrelated edit silently re-attributes a real measurement to conditions it
    was never taken under. **The corrected comment LOOKS like the defect**, so it says why
    it is pinned or the next cleanup reverses it.
- **Measure before choosing, on every platform.** `GetAdaptersAddresses` costs ~2.09 ms on
  Windows against ~0.0088 ms for `getifaddrs` on Linux — **238×** apart. A design that
  looks free on the platform you develop on can be the dominant cost on the one you ship
  to. "It is only a syscall" is how a hot path gets slow.
- **Prefer being fast by construction to being fast by cache.** The cache gate answers
  `IsLoopbackHost(peer)` first and never consults the seam for essentially all real
  traffic, so the cache bounds only the rare path — a far weaker thing to get right.
- **Refresh on an interval, never on a miss.** A miss-triggered refresh hands a remote peer
  a free amplifier: it can force the expensive probe once per request simply by asking.
- **Name both failure directions in the header** — what a too-old answer costs in each
  direction — because that asymmetry is what makes a longer interval defensible.
- **Reach it through an injected seam with an injected clock.** A cache with a hidden clock
  is untestable by construction.

**And prefer not needing the cache.** Computing a value once and returning what you already
have beats caching it: a fingerprint helper that derived the resolved path, the include
roots and the stamp and returned none of them cost its caller an extra driver spawn per
toolchain per survey, on warm starts too, because a cache HIT said nothing about the roots
it covered. A wider return value is also the STRONGER answer, not merely the cheaper one:
two derivations a few milliseconds apart can disagree. **Evidence a caller may not have is
a disengaged `optional`, never an empty field** — empty roots and an empty stamp are both
ordinary answers, so neither can carry "there was no probe".

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
- **Run the local gate before pushing** — `scripts/local-gate.sh`: clang-format **and
  clang-tidy** at the pinned version, then `clang-debug` and `gcc-release`. The default
  agent preset is one compiler at `-O0`; CI is four more, and defects invisible below a
  release build or a second standard library are why the script exists. See
  [`.agent/rules/build-and-toolchain.md`](.agent/rules/build-and-toolchain.md).
  **It builds in `out/build/gate-clang-debug` and `out/build/gate-gcc-release`, which it
  OWNS** — never the `out/build/clang-debug` and `out/build/gcc-release` this file tells
  you to build in. Sharing a directory leaves every ordinary build in the tree uncached
  for good, because a reference build turns the compiler cache off with a `-D` and
  `option()` never overrides a cache entry (#487).
- **`clang-format` and `clang-tidy` after every change — at the version CI pins**
  (`$CLANG_TOOLS_VERSION` in `.github/workflows/build.yml`). Successive LLVM
  releases disagree with each other, so a tree clean under whichever binary is on
  `PATH` can still be rejected. Name the version explicitly and use a build
  directory of its own; the `clang-debug` preset is **not** that sweep.
- **`clang-tidy` reports must be fixed at the source.** Never silence with `NOLINT`. The `clang-debug` preset enables `clang-tidy` at whatever version `PATH` resolves to, which is why **`scripts/local-gate.sh` passes `-DCLANG_TIDY_EXE=clang-tidy-$V` and refuses to start when that binary is missing** rather than letting the preset pick — so running the preset by hand is not the sweep CI enforces.
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

Dependency sources are shared across build trees per machine, so a fresh worktree does
not re-clone Catch2, yaml-cpp, zstd and lz4 — which matters here because the work
happens in throwaway worktrees, making that a per-BRANCH cost rather than a
per-machine one. `CPM_SOURCE_CACHE` defaults to `%LOCALAPPDATA%\fastcached\cpm` or
`~/.cache/fastcached/cpm`; anything that already set it wins, including the environment
variable CI uses to point it inside the workspace for its own cache action. Override
with `FASTCACHED_CPM_CACHE`, and a machine where no home directory can be found simply
fetches into the build tree as before. Measured (native NTFS, Git Bash, Windows 11,
cold build tree): **45 s and 118 MB fetched without it, 24 s and 1.1 MB with** — and
over DrvFs, where #545 found it, the same fetch is slow enough to read as a hang.

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

# ThreadSanitizer, the way CI runs it: the concurrency-bearing tests only
# (Async / Consensus / Distributed / the node), behind a gate that refuses to
# report clean unless the sanitizer is proven live. See scripts/tsan-gate.sh.
cmake --build --preset clang-tsan --target FastCacheTest fastcache-compile-node-tests tsan-canary
scripts/tsan-gate.sh out/build/clang-tsan

# Linux/macOS — RelWithDebInfo + Tracy profiler (.agent/guides/profiling-tracy.md)
cmake --preset clang-tracy
cmake --build --preset clang-tracy

# Windows — MSVC CL Debug (no VCPKG_ROOT needed; run from a VS dev shell)
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
failed connect in silence. Falls through to `ccache`; `sccache` sits above it in
preference order but is never selected automatically (`ALLOW_SCCACHE_FALLBACK`,
default OFF, #815). With either launcher active the
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
