# fastcached - Fast Cache Daemon

A layered C++23 server: a memcached- and Redis-compatible cache daemon, plus a
portable compile cache (`fastcache-cc`) and a distributed compile fleet
(`fastcache-compile-node`). Each layer reaches its collaborators through a narrow
interface, so the whole thing is testable end-to-end against an in-memory
transport, a manual clock and a scripted reactor.

## Project Architecture

The trees below are the at-a-glance map;
[`.agent/reference/source-map.md`](.agent/reference/source-map.md) carries the same
tree with each directory's rationale kept in full.

```
src/FastCache/
  Core/         Error taxonomy, Clock, HostPort, IRandomSource, ISecureRandom, Logger,
                BufferPool, Base64, Bytes, Endian, Crc32c, MurmurHash3, Sha256/HMAC,
                StringHash, Owner, SecureBytes, SessionSeal, Utf8, Markup, Compression,
                WireFrame + WireFields (the shared framing), Profiling, and the crypto
                seam — Ed25519, X25519, Hkdf, the ONLY way into vendored Monocypher
  Async/        Task<T>, Cancellation, ResumeOn, SleepUntil, InterruptibleSleepUntil,
                DeadlineTimer, AsyncQueue, IExecutor with ThreadPoolExecutor for work
                that BLOCKS, IReactor (an IExecutor plus Run/Stop/Schedule/CancelPending)
                + TestReactor and the platform reactors (EpollReactor / IocpReactor /
                KqueueReactor)
  Net/          ISocket, IListener, IConnector (BlockingConnector; PlatformConnector ->
                Epoll/Kqueue/IocpConnector, over ConnectFlow and ReactorDial),
                IAsyncAddressResolver + ThreadedAddressResolver, TcpClient,
                SocketAddress, BlockingSocket, the reactor sockets, TLS,
                InMemoryTransport, HealthProbe, LingeringClose, IAdmissionControl,
                IDatagramSocket + UdpSocket/InMemoryDatagram and SharedPortDatagram
  Cli/          UsageDoc (usage text as data) and Options (the one parse loop),
                dependency-free so fastcache-cc compiles them in rather than linking
  Cache/        IStorage atomic primitives, CacheEntry, CacheEngine, InMemoryLruStorage,
                CowTreeStorage (CoW B+tree), LayeredStorage (L1 LRU over L2 disk),
                ShardedStorage, TracingStorage
  CompileCache/ PathCanon (canonical-token rewriting + the depfile / showIncludes
                grammars), CompileValue, PrefetchGroupManifest
  Consensus/    Raft as a pure state machine (RaftNode) plus a coroutine driver, behind
                IRaftStorage / IRaftTransport / IRaftStateMachine; RaftLog, RaftWire,
                RaftPeerSession (behind IRaftPeerIdentity over IRaftPeerKeys),
                RaftPeerTransport/RaftPeerServer, RaftMembership, RaftClusterHarness
                (a whole cluster in one process)
  Cluster/      DiscoveryService + DiscoveryWire (the LAN beacon and its challenge),
                RosterKeys, PeerDirectory, ClusterState + ClusterStateMachine,
                MembershipPolicy — who is a member, WHERE they answer, and the settings
                every member must agree on; Roster + RosterCertificate
  Distributed/  WorkerRegistry, LeaseTable, SchedulerService — the fleet's capacity
                decisions, pure with respect to I/O; FleetSample + IFleetHistorySink,
                FleetHistory, FleetNodeHistories; FleetView and FleetChart (page, SVG,
                JSON); RosterTrust + RosterStore; NodeProof (the join handshake and the
                session keys that seal the connection after it)
  Protocol/     IProtocolHandler, ProtocolAutodetect, Framing/ByteReader, MemcachedText,
                MemcachedMeta, MemcachedBinary, RedisResp, CompileCacheHandler (the 0xFC
                executor), CompileCacheWire, SurfaceRefusal, SealedFrameSocket +
                ProvenIdentity
  Server/       Connection (per-client coroutine), Server, ReactorServerLoop,
                AdminHttpServer (its routes are a table) + AdminCredential
  Platform/     IDaemonHost, ISignalSource, DaemonControls, CpuAffinity, HostMemory,
                HostInfo, ServiceControl (ServiceSpec), Terminal, InheritedListener
                (systemd socket activation), Environment, FileTrust, LocalAddresses,
                NarrowText
  Config/       Config, CliParser + CliOptions, ByteSize, YamlReader, FileOptions,
                ConfigReloader, EnvExpand, DefaultConfigPath
  Metrics/      IMetricsSink + AtomicMetricsSink, MetricsCatalog (the counter table) and
                PrometheusFormatter, which renders that table
```

Every executable lives under `src/apps/<name>/` and declares its own target and
install rule there; `src/apps/CMakeLists.txt` holds the app table that gates each one,
so adding an executable is adding a row:

```
src/apps/
  fastcached/               the daemon (FASTCACHED_BUILD_DAEMON, default ON)
  fastcache-cc/             the compiler launcher (FASTCACHED_BUILD_LAUNCHER, ON) — an
                            sccache-style launcher keying on preprocess + relativized
                            args; does NOT link the FastCache library (`_fc_cc_core`)
  fastcache-compile-node/   the compile worker AND the peer service
                            (FASTCACHED_BUILD_NODE, ON); may also be the scheduler, hold
                            a cache tier and run consensus — four surfaces, each off
                            unless asked for except the cache
  fastcache-cli/            the operator's client (FASTCACHED_BUILD_CLI, ON), INSTALLED
                            and documented on the Tools page
  compile-cache-testclient/ low-level 0xFC probe + cross-depth validation
                            (FASTCACHED_BUILD_TESTCLIENT, OFF, never installed, built by
                            the linux and clang-tidy jobs)
  fastcache-bench/          in-process storage micro-benchmarks
                            (FASTCACHED_BUILD_BENCHMARKS, OFF, never installed, built by
                            the linux and clang-tidy jobs)
```

Platform service integration and OS packaging live under `packaging/`, which follows
the same table idiom — one descriptor row per installed asset:

```
packaging/
  CMakeLists.txt      the asset install table (source|destination|kind|name|component);
                      exports the config-file list the dpkg conffiles and rpm %config
                      filelists reuse
  linux/              system + user systemd units, sysusers.d/tmpfiles.d, the commented
                      /etc/fastcached/fastcached.yaml, the DEB/RPM maintainer scripts
  macos/              /etc/paths.d entry, the per-component postinstall templates, the
                      uninstaller, the installer panes
  windows/            WiX fragment driving --install-service / --uninstall-service
```

`cmake/Packaging.cmake` turns that into `.deb`/`.rpm`/`.pkg`/`.msi` via CPack.

Production flow: `main()` -> CLI -> optional YAML -> `ConfigReloader` -> `CacheEngine`
over `InMemoryLruStorage` (or, when `--storage` is set, a `ShardedStorage` of
`LayeredStorage(InMemoryLruStorage, CowTreeStorage)`) -> `RunReactorServer`. The
reactor (IOCP / epoll / kqueue) multiplexes every connection on its event loop, so
concurrent clients are bounded by memory rather than by a worker count, and `--threads`
runs that many independent single-threaded reactors, each pinned to a core, with every
connection pinned to one reactor for its lifetime.

`main.cpp` wraps the storage in a `ShardedStorage`, whose per-shard mutex serialises
access, **whenever more than one thread can reach the storage** — four conditions say
so: a persistent backend (so the disk backend is always wrapped), an explicit
multi-shard layout, the reactor running on more than one thread, and the metrics
endpoint, whose `fc-admin` thread calls `engine.Snapshot()` concurrently with the
reactor. **That third one is the DEFAULT rather than an opt-in**: `--threads` unset
means `hardware_concurrency()`, so on any multi-core host the wrapper is on without
anybody asking for it, and reading it as "`--threads` above one" describes a
single-threaded default this daemon does not have.

**No completion port is ever drained from several threads**, on Windows or anywhere
else. `IocpReactor.hpp` says that migrates a coroutine across threads and is unsafe,
and `RunMultiReactorWindows` runs one thread per reactor exactly as the POSIX path
does. The one `ThreadPoolExecutor { 1 }` on each of the three serving paths runs the
EXPIRY SWEEP; it drains no completion port and overlaps no `fsync` with anything. This
paragraph claimed the opposite until
[#896](https://github.com/LASTRADA-Software/fastcached/issues/896), and why a wrong
sentence *here* is worse than a stale one is in
[`.agent/rules/wire-and-protocol.md`](.agent/rules/wire-and-protocol.md).

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

Both halves are checked. Every `##` section over there says whether this file
tripwires it and names the phrase, and `ctest -R rulebook-tripwires` verifies the
phrase is still here; `ctest -R agent-md-budget` holds this file to
`scripts/agent-md-budget.txt`, which is where its size lives and the only place it
is written down.

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
- The toolchain headers the key drops are covered by the compiler's *banner*, so every driver
  is asked for one the way it answers: `cl` has no `--version`, and bare `cl` is its probe.
- A root and the paths a driver emits are reconciled on both sides, or neither.
- Bump `manifest-v*` whenever `objkey-v*` moves. The reverse is not required.
- A compile that writes a second artefact (a module BMI, a PCH) is refused, not cached.
- The compiler identity is the driver AND the target it generates for. The **key** folds
  the target, the **fingerprint** must not.
- The FINGERPRINT folds the driver's argument GRAMMAR (`DriverGrammarName`) — a NAME, never
  the enumerator's value, and in the cache file NAME as well as in the digest.
- Read the `-cc1` line's `-triple`, never the unversioned `Target:` header; an empty triple
  means the identity is UNCHANGED.
- Discovering a target and stating one are different questions: `gcc` is keyed on its target
  and never handed a `--target=` it does not accept.
- `cc` and `c++` name a policy, not a product, so the banner corrects the name — and it must
  never reclassify `clang-cl`, whose banner is plain clang's.
- The banner and the target triple come from ONE `-###` spawn on a `ClangDriverLine` driver,
  and **no fingerprint bump rides with that** — `ctest -R banner-probe-identity` pays for it.
- A path a COMPILER wrote is not this process's text: `cl` writes `/showIncludes` in the
  console output code page. Decoded at `RootReconciler::Path`, or the compile is not cached.
- **A hit reproduces BOTH artefacts or is not a hit** — `Cc::ReproducesDepFile`, #1531. And
  clang-cl under CMake asks for a GNU depfile through `-clang:-MF<dep>` (`deps = gcc`), not
  for `/showIncludes`: the pass-through spellings are `PathValueFlags()` rows.
- The `/showIncludes` MARKER is canonical as `<SRCROOT>` is — normalized to `IncludeNoteMarker`
  before storing, restored AFTER the stale-hit guard, anchored on leading blanks only.
- Reading `/showIncludes` and WRITING it are different questions and must not be consolidated:
  `RenderShowIncludes` takes the marker as a REQUIRED, undefaulted parameter and spells no literal.
- A compiler with debug info on records the WORKING DIRECTORY, which is on no command line,
  so no key relativizes it; `-fdebug-prefix-map` closes it on ELF and on NEITHER COFF driver,
  an accepted cost rather than open work.
  - Never `-ffile-prefix-map`, and no table row for it or `-fmacro-prefix-map` either: both
    rewrite `__FILE__` INTO the text the key hashes.
  - The key relativizes the rule's HEAD (`PathValueRole::PrefixMap`, a table COLUMN) and leaves
    the replacement literal; the build-tree rule is mapped LAST, and a root with a SPACE is not
    mapped at all. `ctest -R debug-prefix-map-rules`.
  - A DISPATCHED compile carries the client's DIRECTORY and its REPLACEMENT, never the rule; the
    worker maps BOTH candidates and DROPS its own, an empty DIRECTORY (never the replacement)
    means map nothing, and one that cannot spell the rules REFUSES. `ctest -R node-working-directory`.
  - **Each end predicts that directory from `$PWD`, not `getcwd(3)`** — `CompilerWorkingDirectory`,
    on both sides. Read `comp_dir`, never compare objects. **A model of a driver that is MORE
    PERMISSIVE than the driver produces WRONG AGREEMENT**, so err NARROW.
- `DW_AT_name` is `comp_dir`'s SIBLING: the client's spelling is a REPLACEMENT, never a path the
  worker opens; the WHOLE path is mapped and the rule goes LAST. gcc needs a SECOND pair
  (`sourceRoot`/`sourceRootReplacement`) after the directory rules; a half-filled pair is REFUSED,
  every other failure is NO RULE, and the worker's skip is `ScratchRootMappingWarnings` at STARTUP.
- An object file is not a byte string. Every MSVC driver stamps the clock into the COFF
  `TimeDateStamp`, so `FASTCACHE_VERIFY` cannot `memcmp` there; ELF keeps the byte comparison.
  Parsing never grants an excuse, it only says WHERE: a FRESH object that will not lay out is
  `Unsupported`, a SERVED one `Mismatched`. Never `/Brepro`.

**[`.agent/rules/distributed-compilation.md`](.agent/rules/distributed-compilation.md)**
— dispatch, workers, the scheduler, the node's tiers. Before `Distributed/`,
`apps/fastcache-compile-node/`.
- The text sent to a worker is **not** the text the key hashed; dispatch preprocesses a
  second time, with `#line` markers.
- A worker is told its input is preprocessed *and* what language it is in; the extension is the
  last of three answers, never the first.
- That language is stated by the flags dispatch APPENDS, so a build that named one itself (`/TP`)
  is folded in and dropped, never refused.
- A MACRO SWITCH (`-D`, `-U`, `/D`, `/U`) leaves the DISPATCHED line only, in a table of its own
  rather than a `PathValues` row, PREFIX-matched and case-SENSITIVE (#1481).
- The arguments a worker passes on are an **allowlist** keyed on the driver family. **A refusal by
  ABSENCE and a refusal by ROW are the same answer only while nothing else is consulted** —
  `--allow-compile-arg` is, so program-invoking options are `Deny` ROWS consulted LAST.
- A cache exchange is bounded by a round trip, a dispatched compile by how long a COMPILER runs;
  they must not share a deadline, and a per-call `SO_RCVTIMEO` is not a bound at all.
- That total cannot also answer *how fast is a stopped worker noticed*: keepalive answers a dead
  HOST and the `Status::Progress` pulse a stalled one, against a SLIDING idle deadline — so
  `Silent` is distinguishable from `Expired`.
- Leadership and membership are one `Gate()`, run for every verb, reads included; only leadership
  stops applying at demotion, so a `GateScope` says which and `Scheduling` is the default.
- Duplicate suppression is asked **before** capacity.
- A lease has three transitions and expiry is the third: the **client** resolves it, on every
  path out of the compile, over a fresh connection.
- A listen flag answers "does this port face the network" only when this process bound the port;
  under socket activation it describes nothing, so both guards stay.
- Whether a worker **checks** a lease is a startup decision, never a per-request fallback — the
  question is whether a machine that is not this one can reach the compile surface. A validator
  returns a REASON rather than a `bool`, and never answers `UnknownLease`.
- A resolve answers on liveness, not presence — an unknown token is refused.
- And it says WHICH nothing it resolved: one wire code, three rows keyed on the OUTCOME, only
  `Expired` counted; acceptance is a discrimination case.
- Asymmetric crypto has ONE seam: `Core/Ed25519`, `Core/X25519`, `Core/Hkdf` over the vendored
  Monocypher (`ctest -R crypto-seam`), RFC 8032 Ed25519 and never Monocypher's default EdDSA over
  BLAKE2b; a missing primitive is added THERE.
- A credential lives in `SecureByteBuffer` and the wipe is an **allocator**, not a destructor.
  Container-agnostic is not SUFFICIENT — SSO keeps a short secret where no allocator is called, so
  a secret is never a `std::basic_string`: `SecureString` holds its characters in a `std::vector`,
  with no inline footprint to wipe and no INLINE-TO-HEAP transition to miss. Holders are found by
  NAME, so a row that has stopped matching is a refusal.
- A lease token is a credential, and its signature covers the granted **endpoint**. Fields
  length-prefixed, never joined.
- **There is no pre-shared key** (#178): `--cluster-key-file` and `Cluster/ClusterSigning.hpp`
  were DELETED, never shimmed, and every proof is an Ed25519 signature under a machine's OWN
  identity key, each construction carrying a versioned LABEL signed as its first FIELD.
- #402 moved the discovery proof's MAC *input* and **`DiscoveryWire::CurrentVersion` deliberately
  did not move**; it DID move at #178, as `RaftWire`'s did for #1308. The question is always which
  of the two changed, the MAC or the GRAMMAR.
- The signature is checked before any other claim is reported on. The expiry bounds how long a
  *captured* token is useful and is **not** a capacity bound.
- A grant is spendable **once**, at the worker it names, and the spend runs LAST; keyed by a
  DIGEST, since serials repeat across a restart while tokens do not.
- **Retention window and acceptance window are ONE window**, and one predicate rather than one
  constant: `expiresAt` is attacker-chosen, so it is a comparison BEFORE a subtraction.
- The learned scheduler term is a DIAGNOSTIC, never a gate: a lower term is ADOPTED, and
  `WorkerJobsRefusedLeaseStaleEpoch` is RETIRED rather than left reading zero.
- **A lower term is a REGRESSION, and calling it a reset is a claim the worker cannot make**:
  report what is OBSERVED, name both causes, and say that the RATE separates them. **A confident
  wrong signal is worse than a vague right one.**
- A scheduler IS a consensus member, alone or not, so every grant is signed by the ISSUING voter's
  own identity key; one without `--listen-raft` is refused.
- A worker checks that signature against a ROSTER, adopting a newer one only on a STRICT MAJORITY
  of the voters in the roster it HOLDS, never of the offer's own; past `notAfter` plus slack every
  grant is `RosterExpired`.
- An OUTBOUND credential is read where it is PRESENTED, through one seam
  (`Node::ICredentialSource`), never captured at construction — which is what lets `--requirepass`
  be `Reloadable::Yes`; a site that never reaches for the seam is a SCAN.
- So is the advertised ENDPOINT (`Cc::IAdvertisedEndpointSource`), which lets `--advertise` be
  `Reloadable::Yes`; the registration and the lease check read ONE value changed at ONE moment,
  never a snapshot each, and a move WITHDRAWS the old entry.
- The worker's key files — every row of `NodeSecretFileTable()`, the identity key among them — are
  asked about at the START **and at every accepted reload**, from `main` and never from
  `WorkerBody`, of the FILESYSTEM rather than of the reloader's two snapshots.
- A worker being dropped is an **event** (`ExpireStale`); a node restarting inside the heartbeat
  window is the second route to the same pin, closed by `Register`.
- A discovery layout describes a **directory layout, not a vendor**: one installation may match
  two rows, and `vswhere`'s answer is memoized across them, empty answers included.
- A port this node LISTENS on is a row of `NodeSurfaceTable()` and an opener takes the
  `NodeSurface`, never a listen spec plus a default host and a name; `--print-surfaces` prints the
  RESOLVED configuration, and `--advertise` is not a surface.
- `--cache-memory 0` means no tier. Zero is how `InMemoryLruStorage` spells *unbounded*.
- `--slots=0` means no WORKER, and `OfferableSlots` DERIVES from an absent count, so the flag
  reaches it only through `WorkerSlotsOf`; such a node registers nothing (#1440). A worker-only
  setting is `NodeOptions()`'s `component` column and a worker rule its `scope` column, never a
  pasted `RunsWorker(c) &&`; the merged listener reads `FamilyRoutes`.
- What a node holds back from compiles is what its tier **built**, never what a flag asked for, so
  capacity is derived *below* the tier startup. Which tiers cost RAM is a `StorageTierTable`
  column, a present zero is *unbounded*, and a disk tier's key index is **a working set rather
  than a store** (`storage.md`).
- A node SERVES while it identifies its toolchains: the cheap half at startup, the walk on the
  heartbeat thread's first round, registering NOTHING until the fingerprint is real;
  `ToolchainSurvey` travels beside the map with a deleted default constructor.
- A probe that did not RUN is not one that answered nothing, and an identity built on one is
  neither served nor cached: the guard is `exitCode == NotSpawned`, never the count.
- A reply's codec is chosen from what the OTHER end said it accepts, never from this end's list
  against itself — `AvailableCodecs()`, never a literal — and a test must separate the two ends
  **disagreeing**.
- A stored value's text regions are canonicalized by **every** server on this wire, through the
  one `CanonicalStoredValue` beside `CompileValue`; retirement is a schema bump, never a sniff.
  - And by every **VERSION** of them, or the rule holds at no moment a fleet is actually in.
    `CompileValueVersion` names the canonicalization spec and not only the framing, pinned to the
    BEHAVIOUR by a conformance digest; an unimplemented generation is `ForeignGeneration`, REFUSED.
- A manifest naming the TU and no header revalidates forever: `ClassifyAgainstRoots` answers in
  THREE values with no bool beside it, `BuildManifest` refuses `NoProjectDeps` and
  `DepsNotObserved` (STATED through `ReportedDependencies`), and `ValidateManifest` refuses an
  empty set rather than letting `all_of` pass vacuously.
- A WORKER follows `NotLeader` too, or the client half arrives at an empty fleet: `Gate()` refuses
  `Register` as well, a leader is remembered only once a round was ACCEPTED there, and `NotLeader`
  must not clear the worker id where `UnknownLease` must.
- `--scheduler` is a LIST for reaching the fleet, never for choosing a leader: each value is
  dialled at most once per round, `NotLeader` never consults it, and a one-shot verb falls back
  only where nothing was SENT. Assert WHICH endpoint took the request.
- `NotLeader` is an instruction, not an answer about the fleet: a client follows it to the
  endpoint it names (`RedirectTarget`), the RELEASE goes to whoever ISSUED the lease, and it is
  judged by PARSING — one predicate, `ParseDialEndpoint` — and bounded.
- A COMPILE reply is tied to its request or REFUSED (`Mismatched`), the digest taken in
  `CompileJobRunner::Run` from what is about to be spawned and never folded in `WorkerProtocol`.
  It has no counter and can have none, so the alarm is an UNCONDITIONAL stderr line and the
  outcome stays a MISS. Input side only.
- A cache is per node; the registry is keyed per `(fingerprint, endpoint)`, so summing a cache
  field across `LiveWorkers()` counts one machine once per toolchain.
- A `FETCH` outcome decides whether the daemon is worth a second command (`CacheIsServing`), never
  whether the invocation continues: an unreachable or refusing cache still dispatches.
- A bounded wait MEASURES its ceiling: `waited += poll` counts the sleep it ASKED for, not what
  the host's timer granularity charged. One `DrainWithin` (`Core/BoundedDrain.hpp`), whose
  blocking and clock are ONE injected seam.
- An unbounded drain hands the ending to the supervisor, which answers `SIGKILL` with no
  diagnostic; `~WorkerServer` bounds it, says what it abandons and ends the process itself.
- A REGISTER endpoint is **not** verified against the caller — `DispatchWorkerEndpointMismatch`
  only counts it; the fix is a credential, as discovery's `(node, endpoint)` signature is.
- `CallerContext::peerId` is the kernel's peer host and IS trusted — membership is decided from
  it. It carries no port; a peer dials from an ephemeral one.
- An address is a stand-in for *one of our nodes* and stops being one when it is not stable, so a
  caller that PROVES a live identity key is a member wherever it dialled from — and since #178 an
  address admits CLIENTS only: the verbs a machine JOINS the fleet with are
  `IdentityRequirement::ProvenNodeOnly`, loopback included. One fold,
  `Distributed::ExplainConnection`, reached only through `Node::RefuseUnlessMember`; a REVOKED key
  is `Forgotten` from every address.
- **A proof is worth nothing unless every frame after it is SEALED**: the answer to `ProveNode` is
  the first sealed frame WHATEVER it says, a bad tag closes the connection unanswered, and a
  connection proves ONCE. The test is a relay injecting a `Register`, neutered by a seal that
  accepts any tag.
- A node's cache tier serves **this machine**, always: locality is a property of the VERB, never
  of the bind and never of a member list, so `CacheResponder` takes no membership oracle — its
  absence IS the fix. It arrives through `Platform/ILocalityOracle`, refreshed on an INTERVAL and
  never on a miss, and is folded with `SameHost`.
- Cluster membership is one ROUTE to admission, never the whole policy: `--fleet-member` admits
  *clients*, so what the cluster agrees is **added** and never substituted, composed at the
  `IMembershipOracle` seam (`AnyOfMembership`). Absence from `ClusterState` is not removal, and
  `NodeMembership::Adopt` writes only `--fleet-member`'s list.
- A CLIENT is revoked by a replicated TOMBSTONE, `--cluster-forget-client`, outranking every
  admission route including `--fleet-open`: `Membership::Forgotten`, `PrecedenceOf`, and a fold
  rather than `any_of`; published from `PublishCluster`, never from `Adopt`, and loopback is never
  forgotten.
- And the fold REPORTS: `Op::ExplainAdmission` answers a verdict AND the SET of routes that
  produced it, through the same oracle the surfaces enforce; an unknown VERDICT is refused while
  an unknown ROUTE bit is KEPT, and there is one mapping, `Distributed/MembershipWire.hpp`.
- REMOVAL is the direction a live admission path has to get right, and the direction a test skips:
  adding a member fails CLOSED and self-heals, removing one fails **OPEN**. So `NodeMembership` IS
  the oracle rather than handing one out; the forget warns where the admit is SILENT; and a reload
  may not WIDEN admission on a worker that runs no consensus and names no `--voter-key`.
- An enrollment window is served by a node that runs consensus (`ServesEnrollment`). **No secret
  crosses it** (#178): the joiner sends its role and PUBLIC key, the approved reply is the roster,
  and no key file is read at approval or written by the joiner.
- A node that runs no consensus refuses the enrollment family `NoCluster`, never
  `UnimplementedVerb`, which a client reads as *this seed's build is too old*; asserted as NOT
  `UnimplementedVerb`, since both refuse.
- Rejecting an already-APPROVED joiner does NOT un-admit it; the remedy is `--cluster-forget` for
  either role (#1555), which the Warn names.
- **The key an operator compared is the key an approval admits**: a pending row keeps the FIRST
  key its id asked with, and a later poll under that id with another key is another machine —
  counted in `claimsChanged`, answered `Pending`, never recorded.
- `--node-status`'s `enrollment` field is ABSENT on a node with no window and `closed` on one
  whose window is shut — the distinction the two enrollment counters' zero cannot carry.
- A compile is awaited onto a `ThreadPoolExecutor` sized to the slot cap, never served inline and
  never on a reactor; on the merged `0xFC` surface that is TWO hops, and **back before the reply
  is returned**, so a test asserts the THREAD IDENTITIES. It spends `WorkerServer::Capacity()`,
  never a second `CompileCapacity`, and `AuthRequired` is **false** for `Op::Compile`.
- Detaching the compiles made the per-request payload cap a per-connection one; the in-flight byte
  budget lands in the same change, refusing with `EndpointBusy`.
- That budget charges what a request **costs**, not what its frame is long: a codec envelope's
  declared expansion is the larger number, and a ceiling on it is per request while the budget is
  per surface.
- Anything a worker derives per job is derived per THREAD: two compiles sharing a scratch number
  shared `tu.o`, and one answered with the other's object.
- And per PROCESS across machines: a scratch root is CLAIMED exclusively, never merely named
  uniquely, and claiming is the liveness check. `flock`, never `fcntl`; the lock file sits BESIDE
  the root; no unclaimed fallback, and `TEMP` is the relocation mechanism.
- A child inherits what the PROCESS has, not what the call set up. Windows names the handles it
  may inherit; POSIX marks both pipe ends close-on-exec, under the lock that covers the spawn.
- A drain waits on a condition variable, never `atomic::wait` — an atomic wait can return without
  the notify and free the object the notifier is still inside. And it calls `Shutdown()` first.
- `AvailableSlots` folds four ceilings into one, `SlotCeilingsFor` names each, and a tie names the
  earlier limit in enumerator order — except a cordon, applied last and named on every tie.
- A cordon is the worker PROCESS's state, never replicated and never persisted, so a restart lifts
  it. Stop, cordon and full are ONE locked decision with the slot take (`SlotAdmission`), and a
  compile's slot rides its reply (`IReplyHold`) until the endpoint has WRITTEN it.
- A heartbeat age is a duration on a report, never a `TimePoint` on `WorkerInfo` — a raw instant
  breaks every `ManualClock` test.
- A CoW store file is claimed exclusively at `Open` and a second opener is refused by name
  (`InUse`). `flock`, never `fcntl` — an fcntl lock is per process.

**[`.agent/rules/consensus-and-cluster.md`](.agent/rules/consensus-and-cluster.md)**
— Raft, discovery, membership. Before `Consensus/`, `Cluster/`.
- A discovery proof is a signature by the node's OWN identity key over a nonce *this* node
  chose, covering the `(node, endpoint)` pair AND the key (#178), checked BEFORE the roster.
- A proof only ever answers a challenge this node issued, and the nonce is spent either way.
- **Nonces and minted node ids come from `ISecureRandom`, never a seeded engine**, and a failed
  draw is a REFUSAL (#1527). Its test is CROSS-PROCESS: `ctest -R secure-random-cross-process`.
- Discovery never changes membership, and since #178 it admits NOBODY: it reports who proved a
  key the roster already holds, with no KEY opinion, re-asked of the roster at every publish.
- **Every Raft peer connection proves each end's OWN identity key before a message is read**
  (#178) — `Consensus::IRaftPeerIdentity` over `IRaftPeerKeys`, **each signature covering the
  WHOLE transcript**. Without one, a STARTUP refusal, never a per-connection fallback.
- The ACCEPTOR challenges first and checks the SIGNATURE before any claim, then `OwnId` before
  `WrongTarget`; every verdict but an unknown key or a forgery is SIGNED, refusals included.
  **An unsigned refusal of a member is a confident wrong signal.**
- Every session frame carries an HMAC under the HKDF session key (`Core/SessionSeal.hpp`) over an
  implicit sequence number, and a message naming another sender closes the connection: the ids
  are bound and the ENDPOINT deliberately is not.
- **An applied forget closes the sessions its revoked key proved, at their next frame once the
  configuration no longer counts the member** (#1555) — `StillProves`, asked per frame and
  PULLED; the roster is `--raft-peer`'s `@<key>` until the replicated state says otherwise.
- `RaftWire::CurrentVersion` and `MinSupportedVersion` are both 4, each bump a GRAMMAR change —
  the opposite of discovery's #402, where only the MAC input moved.
- `RaftClusterHarness` authenticates EVERY message through the real session objects, with a
  REQUIRED identity factory; an intruder case says nothing without the formation case beside it.
- `RaftNode` reads no clock, opens no socket and draws no randomness of its own.
- A snapshot is durable before it is acknowledged, and the configuration travels inside it.
- A snapshot reaches the APPLICATION on recovery AND on install (`RaftDriver::Create`, #1542), so
  a restart case asserts `Member::application`, never only the log.
- **A node whose OWN snapshot, or a command its own log holds, is one this build cannot read
  REFUSES TO START** — `CanRead` FIRST, so a refusal hands the application NOTHING.
- **A leader's snapshot the application cannot read is REFUSED before the node takes it on**
  (#1552): `CanRestore` first, answered `Rejected`, staying BEHIND rather than stopping the tier.
- A seeded draw must be identical on every standard library — `UniformInRange`, never
  `std::uniform_int_distribution`.
- A node being admitted must never have bootstrapped a cluster of itself, so `RaftConfig::voters`
  and `learners` may legally both be **empty**. Who a node dials is not who it counts.
- **Voting is a property of the CONFIGURATION, never of a role (#1449)** — a `StandingTable` row
  through `Membership::StandingOf`, never a fifth `Role`, and a learner refuses a vote by ROW.
  Every quorum read counts VOTERS through `Membership::QuorumOf`; replication reaches both sets.
- The quorum follows the replicated state, one change at a time, and a member is never COUNTED
  before it is dialable AND has CAUGHT UP (#1537). **One at a time is load-bearing for READS as
  well as for commitment**, because CheckQuorum reads the committed configuration.
- Absence from `ClusterState` is not removal, and **a learner is never removed for being absent**.
  **A FORGET is not absence**: it leaves the quorum whoever typed the member (#1555), THIS node
  once FORGOTTEN — record gone AND (host tombstoned OR key revoked), never the record alone —
  proposes its own removal last and steps down (#1539), and forgetting the only voter is refused
  by name (`PrepareForget`).
- **A forget outranks an observation (#1528)**: `MembershipProposals` refuses a forgotten desire
  BY NAME (`MembershipPlan::forgotten`), at the decision rather than by pruning it.
- **`--cluster-forget=<id>` is ONE act (#1555)** — the record goes and its key is revoked in the
  same entry (`PrepareForget`), because there is no verb for either half alone.
- The SEAT an operator chose is the record (`ClusterMember::seat`, `MemberSeatTable`); a desire
  carries NONE (#1535), and a discovered machine is a LEARNER until an OPERATOR promotes it.
- The Raft store is FORMAT 2 and every log record carries its format: a store another build wrote
  is `UnsupportedFormatVersion`, judged before the CRC. A log's FIRST record decides; a foreign
  LATER record is a torn tail.
- A cluster that has elected is not one that has formed: until every member attaches, pre-vote
  refuses nothing and any stall re-elects — assert leadership stability only after formation, and
  log the term or nothing explains it.
- A leader and a follower stamp the same link half a round trip apart, so a shared window buys no
  shared answer: a non-leader reads its own `_knownLeader` and deadline, never a timestamp.
- CheckQuorum DEPOSES a leader here and measures SILENCE, so while a change is UNCOMMITTED it
  asks the **committed** configuration, never by seeding `_followerContact`. **This rule has no
  constant.** `undecided` in a node log is `SchedulerRole::Undecided`, not a Raft role.
- A refusal code carries its own PERMANENCE, and there are THREE answers: `SubjectOf` reads as a
  global fact, `WireCodeFor` fails the BUILD, and **`RefusalSubject` has `Satisfied`**, which
  stays a refusal rather than a success.
- "A leader spoke" arrives at two handlers: `OnInstallSnapshot` is `OnAppendEntries` speaking,
  membership guard and candidate demotion included.
- **A node IS its state directory: its identity is MINTED into `--cluster-dir` and read back
  forever, never derived from the machine** — not the hostname, not the OS machine-id even as a
  SEED. `--node-id` overrides and is recorded, `--raft-self=<host>` states the address, and the
  resolved value reaches every configuration this process builds.
- **The hostname is a fleet-page LABEL and decides nothing**, and it still passes the UTF-8 gate
  at `SchedulerService::Register`. **No prefix matching on ids.**
- **A replicated setting must not decide where a node sends a CREDENTIAL** — removed rather than
  wired, and refused BY NAME through `RefusedSettingTable`.
- **A mode rides on the PORT, never on the absence of a NAME**: one predicate, `RunsConsensus`,
  asked of the surface row so `--print-surfaces` and the mode cannot disagree.
- **Only an ABSENT `node-key` mints** (#178): a key file that is there and cannot be used is
  refused by name and left untouched, never re-minted; its path is DERIVED (`NodeKeyPath`).
- **`ValidateAgainst` is the courtesy, `Apply` the guarantee.** A revoked key is never admitted
  again (`KeyRevoked`, permanent), and a forget's revocation is what `Apply` never drops.
- **A flag that parses a key CARRIES it, never parses and drops it**: `--cluster-admit@<key>`
  rides CLUSTER-ADMIT's third field (0xFC 11), and an absent key keeps the recorded one.

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
- A frame declares its own length, so a rejection is a **reply** and a resynchronization — never a
  close.
- A reply carries a status byte and NO kind, so step-over-what-you-do-not-know is REQUEST-side
  only: `Status::Progress` moved `MinSupportedVersion` with `CurrentVersion`. Its `static_assert`ed
  verb column, empty payload and terminal-status loop are in the rules file.
- Silence is only measurable against something that would otherwise be said, so the worker's
  cadence and the client's idle bound are ONE `static_assert`ed pair of numbers in
  `CompileCacheWire`.
- A pulse is a SECOND writer for the length of the answer, so the endpoint settles it before it
  writes anything and one still parked past the bound ends the connection — `SettleWatch`'s rule on
  the write side, with the vacuous "every frame but the last is a pulse" test beside it.
- `Status::Push` is the SECOND exception, `static_assert`ed to `Op::Subscribe` alone: a stream is
  PULLED, re-gated EVERY tick, and a client leaves by HALF-closing. The tick grid, the `Gap`, the
  claimed capture and the write-START hold are in the rules file.
- A stream frame a reactor frees after its owner touches only what it owns: the active count is a
  `shared_ptr` the frame holds, never a member of the component it outlives.
- Which verbs are reachable before authentication is a *column of the table*, and the gate runs
  before the payload is buffered.
- A pre-auth verb carries its own payload ceiling, `static_assert`ed so a new one cannot reopen the
  hole by omission.
- An unimplemented verb is refused `Wire::UnimplementedVerb`, never `DispatchNotPermitted` — a
  `(op, code, why)` table row rather than a `switch` case, spelling ONE named constant every
  surface and the client share. *Unimplemented* is not *served elsewhere*, and a row for a verb the
  surface DOES serve is dead: `static_assert` that one cannot be added.
- A wire constant has TWO facts, its name and its value, and a symbol both ends spell tests only
  the first. Pin the **byte** as well, and keep one test on the raw enumerator — the anchor, not a
  smell.
- **A new verb is not one dispatch, and none of the other places fails the BUILD**: a `switch` arm
  in the daemon (a missing one DROPS the frame), a `RelocatedVerbs` row, a documentation row for
  any counter it adds, and the live-stats layout pin.
- `Net/` must not depend on `Core/`. `Async/` travels with it, plus three named dependency-free
  leaf headers; `ctest -R net-boundary` enforces the table.
- `CompileCacheWire.hpp` must stay header-only and dependency-free — the launcher does not link
  `FastCache` — so it carries cache tiers **positionally**, making `StorageTier`'s enumerator order
  a wire contract.
- **And an enum SAYS which kind it is at its declaration** — transmitted or persisted, or private —
  because *no comment* means both. The explicit `= N` is the enforcement and is harmful on a
  PRIVATE enum; **`RowsInEnumeratorOrder` is not that guard although it reads like one**, and the
  `static_cast<T>(byte)` census is NARROWER than it reads, so this bullet is the guard.
- SIGPIPE is suppressed per socket, never process-wide: an ignored disposition is inherited across
  exec.
- So is keepalive, and for the mirror reason: not in `ApplyHotSocketOptions`, where every socket
  passes, but a `DialOptions` field — and the flag without the intervals inherits a two-hour
  default that reads back as armed. A faster failure nobody can NAME is not an improvement: expiry
  and a lost peer are one broken socket, so the timer records which.
- A child inherits this process's sockets too, and neither platform stops it. Armed once, in
  `ApplyHotSocketOptions`. A spawn names what it hands over
  (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`) rather than marking what it does not.
- A listening socket claims its address exclusively — `SO_EXCLUSIVEADDRUSE` on Windows, where
  `SO_REUSEADDR` lets a second process take a port already being served. Sharing a port is
  `ReusePort::Yes`, and only that.
- A struct a decoder returns **by value** must not borrow from the bytes it decoded:
  `Decode(Encode(x))` is a use-after-free the moment one member becomes a view. A `*View` type
  borrows and says so, anything else owns, and the choice is per type by a CONJUNCTION.
- **What the borrowed field DECIDES outranks the arithmetic**: a wrong `CapacityFields` is a wrong
  number, a wrong `CallerContext::peerId` is a membership decision read from freed memory. Own it.
- A regression test for the above needs a payload of REAL SIZE **and** the right ARRANGEMENT: read
  inline nothing dangles at any size, so STORE, drop the source, churn the freed storage, then
  read. Pick a size that is REAL rather than large, and `static_assert` it.
- There is exactly one TCP client, `Net/TcpClient`. Do not write a second.
- A synchronous dial spends a thread the caller does not own — a reactor thread dials through
  `PlatformConnector`, never `BlockingConnector`.
- EOF means "this peer has finished SENDING", not "this peer is gone": answer what is determined,
  abandon what is pending. `ISocket::ShutdownWrite` makes the question askable in PRODUCTION, the
  answer does not transfer between wires, and a watcher reads the COUNT. **DELETE an arm and see
  which case fails**, keeping a control that must survive an open write side.
- A peer that sent NOTHING asked nothing, so it is CLOSED, not refused: **`sawHeadEnd` decides THAT
  a head is refused; the cause only selects WHICH code.** The codes, the half-closed-but-complete
  control, `Net::IsDeadlineExpiry` and its one REACHABILITY exception are in the rules file (#828).
- And a TLS peer says it with a RECORD, so the raw socket answers the OPPOSITE: `close_notify` then
  FIN means the raw peek reports `>0`. `TlsSocket::WaitReadable` decrypts with `SSL_peek`, which
  removes nothing, and a raw EOF before a full record is EOF too.
- And on the compile surface those two arms are COUNTED apart, so folding them is a regression even
  though both reach one outcome: each arm names a `PeerDeparture` a table maps to its counter,
  proved by neutering, the FIN case being the load-bearing one.
- An AcceptEx socket takes `SO_UPDATE_ACCEPT_CONTEXT`, or `shutdown` fails on it and
  `ShutdownWrite` is a silent no-op on every socket the listener accepted.
- A socket DECORATOR forwards `SetReceiveDeadline`, or the deadline lands on a layer that never
  reads — `TlsSocket` inherited the base's no-op, and #828's bounds never applied under TLS.
- A close over unread input is a RESET, so a server answering a request it did NOT finish reading
  closes through `CloseLingering`: ONE helper, a row of bounds per surface, a lingering connection
  still holding its admission slot, and a fixture that closes the way production does.
- An object a reactor OWNS is destroyed on that reactor's worker thread, or with that reactor
  stopped — `IReactor::TeardownIsSerialisedWithDispatch()`, asked of EVERY reactor. **Match the
  ASSERTION, never the case that happened to be running.** Posting the teardown HANGS, so DEFER
  instead (`NodeIoLoop::Retire`), and a test removes the race rather than waiting for it.
- `Close()` can be the last thing that runs on a socket, so it must touch no member after it
  completes an awaitable.
- An awaitable's address is taken in `await_suspend`, never in the factory that returns it — that
  one is a LOCAL returned by value, so the caller suspends on a different object.
  `SetSuspendCallback` is what makes the right answer the only reachable one.
- And it is the ONLY thing that retrieves a parked read, so the shared FAKE owes that too: detach
  first, complete last with `Cancelled`. Prove the leak instrument before believing a green ASan
  run — a parked frame is a live unreachable allocation, which LSan reports exactly.
- `Read`'s buffer must be NON-EMPTY, because `0` is taken by the opposite fact — every transport's
  receive primitive answers `0` for a zero-length request. `Detail::RequireReadBuffer` is `Read`'s
  first statement in all six, an assert rather than an error code; `empty-read-buffer-canary`
  watches ONE site, so `read-buffer-guard` DERIVES the set.
- **A guard folded INTO the operation is self-enforcing; a guard called ALONGSIDE one needs a
  scan.** Where the guard cannot ride on something the site must do, the scan is not optional.
  **The author of that rule broke it one commit later, in the same branch.** And the fix is NOT
  pure virtual: **type system when the obligation is DO SOMETHING, scan when it is SAY WHY.**
- **A `/simplify` finding is a change like any other and is not exempt from the review its subject
  just had.** And **a regression test can fail to reproduce its regression**.
- A socket has ONE read operation and `Read` and `WaitReadable` share it, so arming either while
  the other is parked drops the parked coroutine with no signal. The rule lives on `ISocket`;
  `Detail::ClaimReadSlot` folds the claim and the `assert` into one expression and
  `read-slot-guard-canary` must die. Not a refusal — and **the hazard is the SITE, not ownership**,
  which is a RETRACTION: at the ARM site a socket cannot tell a stale parked wait from a live one.
- The WRITE slot is the same rule: one write op per direction, `Detail::ClaimWriteSlot`
  (`Net/WriteSlot.hpp`), reaching `FrameEndpoint`'s one-writer property because `WriteAll` sends a
  whole frame in ONE `Write`. `write-slot-guard-canary` watches it BOTH ways — **a guard nobody has
  watched ACCEPT is not known to work either** — and a new helper naming `Loop` stays unenforced.
- So a parked read is retrieved by `ISocket::CancelRead()` — the only spelling of *abandon* that is
  not `Close()`. ONE watch, re-TARGETED per pass, re-armed only once the previous has RESOLVED,
  retired by RAII AND explicitly before the reply write; what silences it is the RETIREMENT, never
  the code. Synchronous in the SLOT on every transport, IOCP included — the WAITER resolves inline
  only for a zero-byte `WaitReadable` probe; a real read settles to the next turn, and promising
  otherwise drops bytes. `Testing::ParkingReadableSocket` COUNTS orphans.
- A wait nothing can cancel is a coroutine frame nobody frees: park through
  `Schedule`/`CancelPending`, and bound any sleep a peer can move the deadline of.
- A reactor resumes what it parks or FREES it, and may free only what nothing else owns —
  `Schedule` BORROWS, so ownership travels with the park (`ParkedWork::abandon`) and what is freed
  is the chain ROOT. Resuming is a hang, not an alternative; `Resume()` disowns and resumes in ONE
  expression.
- Re-declaring ONE overload in a derived interface HIDES the base's others, and here it hid the one
  carrying ownership. Nothing diagnoses it; the detection is the compiler, and the `no viable
  conversion` errors from converting the sites enumerate the defect's reach.
- A missing keyspace event has two ends — the tier that never named the victim and the observer
  that never published it. Check both before changing either.
- A reclaim is reported **before** the call that caused it: `ADD` on a lapsed TTL names the same
  key twice, and the wrong order tells a subscriber a live key is gone.
- In a layered cache no single tier's eviction is total, so none is reported. An expiry is, because
  both tiers hold the same TTL.
- A reclaimer nothing constructs is the bug it was written to fix: `PurgeExpired` was correct and
  tested, and had no production caller at all. Assert the wiring.
- The expiry cycle sweeps `engine.Storage()` — the notifying decorator. One layer down it frees the
  bytes and publishes nothing, and every tier test still passes.
- One cycle per daemon, on reactor 0; its reclaim ceiling sits below `ReclaimLog::DefaultCapacity`
  or the sweep drops the events it runs to produce.
- A bounded sweep resumes from a cursor and `ShardedStorage` rotates its starting shard, or
  everything past the first budget never expires. That cursor outlives the call, so a tier gets
  exactly one erase point.
- `--expiry-interval=0s` disables the cycle (a coroutine that *ends*); `--expiry-scan=0` is
  `PurgeBudget`'s spelling of *no ceiling* and is refused.
- On disk a read may not reclaim and a write must: `Get`/`Peek` can hold a shared lock, every write
  verb holds the exclusive one. Reporting without erasing is worse than neither — the record stays
  and fires `expired` again.

**[`.agent/rules/platform-service-and-config.md`](.agent/rules/platform-service-and-config.md)**
— service registration, config lookup, the CLI table. Before `Platform/`, `Config/`,
`packaging/`.
- A service to register is a `ServiceSpec`; what it runs as is part of it, and an empty
  `serviceAccount` means **root**.
- `--install-service` registers the *command-line* config, never the merged one, and carries the
  config PATH rather than the file's values or a resolved default.
- An install is judged by the **startup** rules as well as the install-time ones: a registration
  replays its command line forever, so refuse it while somebody is watching.
- A refusal that depends on nothing but the parsed configuration belongs in a table — the option
  row for a grammar, `StartupPolicyRejection` for a cross-flag rule, never the tier that happens
  to need it. An install returns before any tier exists.
- **The addresses a node OPENS and the ones it DIALS are two tables, and one predicate cannot
  serve both.** Opened surfaces are `NodeSurfaceTable()`'s rows; `--advertise`, `--scheduler` and
  `--upstream` take `ParseDialEndpoint`, where a bare port names no machine, while
  `--fleet-member` is matched through `HostOfEndpoint` and must NOT — only an EMPTY element is
  refusable there. Shape and PRESENCE stay separate rules; `--bind` is in neither.
- Whatever reaches a supervisor must survive this project's own parser round trip — including the
  flags the *installer itself* adds, the daemon's only when the spec names an application.
- Whether the operator **named** a setting is provenance, recorded by the parse in
  `OptionSpec::explicitBit`, never recovered by comparing the value to the default. The startup
  decision AND the service registration ask it: `emitIfExplicit`, never `emitIfSet`.
- A config the operator named is strict; one the daemon found is skipped when absent,
  unreadable or untrusted.
- A machine-wide config is obeyed only when only an administrator could have written it
  (`Platform/FileTrust`).
- That is INTEGRITY; secrecy is a second question the same access list cannot answer.
  `--seed-config` gives the FILE a protected list of its own, not the MSI, and an existing file
  is repaired only when it is *currently* broadly readable, never by content.
- A secret reached BY PATH is not provenance-gated — the path is not the secret and the file is —
  while `--requirepass` out of a config file still is. Which `=<path>` rows are which is a TABLE
  per binary, classification is MANDATORY (`--tls-cert` is named PUBLIC, not left off), and BOTH
  binaries ask at the START and at every accepted reload through one `SecretExposureWatcher` that
  re-asks the FILESYSTEM.
- Every flag is one row of `CliOptions()`, which drives parsing **and** help.
- Which flags carry text *other machines* will read is a column of that table (`ParseUtf8Text`);
  `--cluster-forget`, every path-valued flag and the compiler half of `--toolchain` are out.
- A value parser cannot know which flag it was reached through, so it names none and
  `ApplyOneOption` stamps the row's own spelling.
- A configuration FILE reaches the same fields through the SAME appliers, in that order. A RELOAD
  rebuilds the candidate the way the START built it, through one `AssembleEffectiveConfig` (file,
  then argv, then the environment) that `ConfigReloader` takes as a REQUIRED argument. Which key
  a row answers to is a COLUMN (`yamlKey`); a key naming no row, or written twice, is REFUSED;
  and a row a file may not carry is on a named list with a per-row reason the guard READS.
- A flag whose meaning is its presence is a boolean in the file and `apply` runs on `true` alone
  — the key spells the FLAG. A repeatable row APPENDS, so the command line EMPTIES the list
  first, off the `clear` column and through the parser's own `TakeValue`. A file that failed
  halfway is DECLINED, never half-applied.
- A missing file is `FileNotFound`, not `ParseError`: `YAML::BadFile` derives from
  `YAML::Exception`, and the general catch sent a mistyped `--config` hunting for a syntax error.
- The shipped reference configuration is checked against the table
  (`ctest -R node-config-reference`), which also fails when either scan matches nothing.

**[`.agent/rules/storage.md`](.agent/rules/storage.md)** — the on-disk format and
converting a store. Before `Cache/CowTreeStorage`, `CowTree/`.
- An old store is `UnsupportedFormatVersion`, never `Corrupt` — the code is what monitoring sees.
- `Corrupt` means the BYTES ARE DAMAGED, and nothing a client sends may reach it: `SetCodec` and
  `StreamCodec` return `MalformedValue` themselves and no caller picks, with
  `CacheMalformedValues` keeping it visible.
- One `Corrupt`, two events, and WHERE decides: damage within `Open`'s reach refuses the process
  to start, damage anywhere else is found per key while it serves — so making `Open` touch more
  of the store is a decision rather than an optimisation.
- A format is convertible exactly as long as its reader is in `RecordFormats()`; bumping the
  version without adding a row is the decision to discard every store.
- "No marker" is an INFERENCE. Validate every record before writing any of them: a store this
  build cannot read must come back unmodified.
- The conversion commits in slices, and each slice must `Flush()`, or the freed pages are not
  reclaimable and the slicing buys nothing.
- Each slice records its resume point in its own transaction, so an interrupted run is refused
  by name and finished by re-running it.
- A tree walk is bounded by `PageCount()`, and must not overlap a commit.
- A tier's `bytesUsed` is denominated differently per tier: memory counts STORED (compressed)
  bytes, disk counts `originalLen` — so measure the store FILE for a compression test.
- The LRU mirror holds what this SESSION touched — `TouchOrInsert` is its only writer and no
  `Open` path calls it — so eviction reaches the COLD set first, and that is LRU rather than a
  workaround. A figure describing the store reads the STORE at the store's own denomination;
  only the victim reads the mirror, and `indexBytes` deliberately still reports the present.
- A refused `Open` carries the **errno it classified** (`OpenRefusal`) — `InUse` alone asserts
  *somebody holds this file*, and absent is a DISENGAGED optional, never `0`.

**[`.agent/rules/metrics-and-observability.md`](.agent/rules/metrics-and-observability.md)**
— counters and scrape surfaces. Before `Metrics/`, `/metrics`, `/healthz`.
- A counter is a row in `MetricsCatalog`, `static_assert`ed to cover every enumerator; the
  renderer walks the table rather than a hand-picked list.
- A live stream's ending is a row named for what was OBSERVED, a stall being the surface's row
  and never a sweep's; its body is the `StatsReading` `/metrics` renders, one provider per process.
- A refusal's wire code and its counter are one row, so `Refuse` takes the ROW and there is no
  argument for a bare `ErrorCode`. The row is the REFUSAL: two refusals may share a code.
- A surface MERGING undoes that without anybody writing a bug, so the scan is EXACT: one uncovered
  site and a file is not covered at all. The endpoint owns WHEN, the surface owns WHAT including
  the counter (`RefusalReply` / `EndpointRefusalReply`); a pre-header refusal is the ENDPOINT's
  own row; `SchedulerRequestsRefusedUnauthenticated` is pre-payload only, so a WRONG token is a
  third row; and not every refusal is an EVENT.
- That scan is a GLOB over `src/`, never a file list: **a list is exact about the files it knows
  and silent about the ones it does not.** An over-broad scan fails CLOSED; header-only.
- And "deliberately uncounted" must not be spelled like "forgot": THREE spellings, three claims
  — `Refuse`, `RefuseWithoutCounter` (and why), `RefuseUntriaged` (and which issue will decide),
  the third safe only because the check TALLIES it. The reason is `rationale`, never `why`.
- The SET of spellings is derived from that header, never restated in the check — a restated
  list catches one going away and is blind to one ARRIVING. `worker-refusals-selftest`.
- The scan filters whole-file before splitting; each needle is a strict prefix of its regex.
- Text a peer sent is text, or the fleet refuses it: one byte that is not UTF-8 makes
  `/fleet.json` unparseable for the **whole** fleet, so it is refused where it enters
  (`SchedulerService::Register`). Markup's rule is XML's `Char` production over CODE POINTS.
- **Skipped, absent, unstarted and failed are FOUR states**, and neither a count nor a `bool` can
  carry them. **Absence of the negative is not the positive**, so a check concluding from a count
  of BAD things needs a separate assertion that the good things exist. **That reaches any probe
  you TYPE**: ask it for something it must find before believing what it did not find — **and the
  mirror, which is the half that gets acted on: a positive finding settles nothing when it is true
  under BOTH readings of the claim** — and read its exit status, where anything neither `0` nor
  `1` is the instrument failing. Where the answer cannot be determined, report that as its own
  outcome rather than the nearest neighbour.
- **A state-collapsing bug is likeliest in the tool whose JOB is that state distinction**, and its
  repair must report the unrequired failure by name AND KEEP GOING. **The repair for one collapse
  is the prime site for the next, and the location is the `*)` arm** — **a `case` with a `*)` is
  an unguarded table** — and a verdict computed from a SUMMARY beside its own disproof is its
  own defect.
- Absent is not zero: a process with no cache reports no cache, and *names* the field to do it.
- Its converse: an absence must not be counted as an event, so an outcome that can be *not
  attempted* is an enum and not a `bool` (`NoUpstream`), fixed at the SEAM.
- A counter is a tally, so zero is the truth about events that never happened, and absence is
  modelled in the **snapshot**. **One carve-out**: a row this BUILD cannot represent is omitted
  and counted by `fastcached_metrics_catalogue_skew`, never rendered as a plausible zero.
- A SECOND carve-out: a row no writer in this PROCESS could move is absent, never zero;
  `CounterSoleWriterTable` says which surface writes each (**the count is not repeated here**;
  `size()` counts PAIRS, `AttributedCounterCount()` counts counters). **Err unattributed**, and
  narrow a BINARY's set only from what CONSTRUCTS a component — `ServedSurfaces{}` is *not
  narrowed*. `ctest -R counter-attribution`.
- A duration is a `_sum`/`_count` pair, never a gauge.
- A condition an operator must act on is a row of `NodeConditionTable`, never a log line alone.
  **Latched and live are drawn apart on every surface**; `none raised` is SAID and a node that
  sent no list is ABSENT; `undecided` is *nobody evaluated it* and must not read as `clear`;
  rows travel as WORDS, the remedy included. The `[conditions]` Catch2 tag, in three binaries.
- A merged snapshot is one tier's answer standing in for all of them: `SnapshotTiers()` reports
  the split, the `tier` label comes from a table, and a tier the cache lacks renders no line.
- The fleet page is served by the leader; anyone else answers `503` **naming** the leader, never
  a redirect and never a link to an address it guessed.
- Its columns are a table every renderer walks, one spelling serving as header cell, JSON key
  and text column. Absent renders at the **cell** — `null`, `–` or `-`, never a blank and never
  a zero — and a tier no member runs gets no column.
- A fleet total is computed over `NodeReports()`, never over registry entries.
- Nothing a receiver can **recompute** travels: a handed-over bucket carries two instants and
  the readings. History is filed under the **machine**, never the worker id.
- A handover cursor advances only on the verb that carried the batch, and exactly ONE verb
  carries it — NODE-ANNOUNCE: two callers of `NextHistoryBatch` would split one cursor.
- A node's version is compiled in, rides REGISTER's *nested* capacity record rather than its
  top level, and is refreshed on re-registration — a restart is what an upgrade looks like.
- A history stores a counter **raw**; a rate is the delta at render, taken only between adjacent
  *present* buckets, so a restart is a gap and not a spike.
- A node records **itself** always; only the fleet-wide slots are leader-only, and which is which
  is `FleetMetricTable`'s `scope` column. Every node samples whatever surfaces it serves.
- A **backfilled** window answers for a machine, never for the scheduler: its fleet-scoped zeroes
  are not readings, and drawn as such they run a rate backwards and then spike.
- The routes reach a history through **one** door (`IFleetHistoryView`). Assert the wiring.
- No state of a history file may keep a node from starting — and a file a **later** build wrote
  is kept and never written over, a property of the shared envelope rather than of each store.
- A chart served as its own resource inherits nothing from the page, so it carries its own
  palette and theme is part of its URL — which carries no cache-buster, or the `304` never fires.
- A `304` carries its validators and no content; whether a body is allowed is a property of the
  **status**, not a flag each route sets.
- An unknown `range` is refused, an unknown `theme` is not: refuse where a silent substitution
  would mislead, default where it cannot.
- A stacked area is drawn **top band first**, or translucent fills make a colour of no series.
- A `<circle>` is not path data: one `<` in an attribute value makes a browser refuse the whole
  SVG behind a 200, and a run of one reading is a DOT, since closing it draws nothing while
  still claiming the series was observed. Neither is visible on dense data — render GAPS.
- The dashboard credential is its own file, never `--requirepass`; a non-loopback bind without
  one is a startup refusal, and TLS does not substitute for it.
- Plain HTTP is a supported way to serve the admin surface; TLS is on by naming material or by
  asking for material to be made (`--tls-self-signed`), never by a bare boolean.
- A generated certificate encrypts but does not identify: its fingerprint is logged because that
  is all an operator can compare, and its subject names decide whether a client accepts it.

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
- **A new INSTALLED binary is not one CMake row**: CPack installs the whole Runtime component
  while the three `Package (...)` jobs name their build targets by hand, and none of those
  contexts is required. Three `--target` lines in `build.yml`, the macOS redistributable loop
  (a different list: payload, not symlinks) and `FASTCACHED_MACOS_LINKED_TOOLS`. Guard: #1202.

**[`.agent/rules/build-and-toolchain.md`](.agent/rules/build-and-toolchain.md)** —
what differs between compilers, standard libraries, hosts and tool versions.
- Run `bash scripts/local-gate.sh` before pushing — **`bash <path>`, never the bare path**. One
  configuration is not the gate; a run printing NEITHER terminal line did not CONCLUDE
  (`--classify=<log>`), and **a GREEN run is silent in the OTHER direction**.
- **A retry makes an instrument's own failures disappear without fixing them**, and **neither the
  wrapper's exit status NOR the presence of a log settles whether the gate ran**. **The gate
  serialises ITSELF** on one `flock` path, so run it bare (#1379).
- **A red gate reads as "my branch is bad", never as "the gate is broken" — so a gate that fails
  CLOSED and UNCONDITIONALLY can sit for days with nobody filing it.** **A guard nobody has
  watched ACCEPT is not known to work**: assert the passing direction too.
- **Verifying the FACT a check is about is not verifying the CHECK.** **Run both directions: which
  one you skipped decides which way it lies.** And measure an idiom at REAL size.
- **A count that OVERSTATES what is wrong is the same defect as one that understates it.** A
  `macro()` substitutes its arguments TEXTUALLY, so `if(param ...)` inside one is dead.
- **A claim about a tool is checked against the tool.** A pattern is broader than its author reads
  it as, a process is attributed by its ancestor chain, and **the tree you measured is not
  necessarily the tree in question**.
- A hygiene script `ctest` runs is constrained to **bash 3.2** — no `mapfile`/`readarray`,
  `declare -A`, `${var^^}`, `local -n`; keep the process substitution replacing `mapfile`.
- **`std::from_chars` has no FLOATING-POINT overload in libc++ before macOS 26.0**, and a grep
  tests a NAME while the hazard is a SIGNATURE. `Core/NumericText.hpp`'s `ParseFiniteDouble` is
  the tree's one answer, and it pins the C locale too.
- **`std::ranges::iota` and `std::ranges::fold_left` are written `Ranges::Iota` and
  `Ranges::FoldLeft`** (`Core/Ranges.hpp`), by FEATURE-TEST MACRO and never by compiler ID.
  `ctest -R ranges-seam` refuses a direct call — never an `#if` at a call site.
- A `char` is UTF-8 here, at run time and at compile time: every Windows executable declares the
  UTF-8 process code page and MSVC gets `/utf-8`.
- `std::filesystem::path`'s narrow constructor THROWS on such a host for bytes that are not UTF-8,
  before any `error_code` overload runs. `PathFromNarrowText` is the one `catch` here.
- Where `clang-debug` will not build, get ASan from GCC (`-fsanitize=address` alone — UBSan breaks
  the option tables' constexpr checks) and run the **whole** suite.
- Run clang-format and clang-tidy **at the version CI pins**, in a build directory of its own;
  `PATH` resolving to an older binary reports clean in the way that means nothing.
- **And `CLANG_TOOLS_VERSION` pins a MAJOR, not a BUILD** — two apt.llvm.org snapshots print the
  same `--version`, and only `dpkg-query -W` tells them apart.
- **So the analyser is a BUILD: `.clang-tidy-version`, an exact PyPI `clang-tidy` release, and a
  binary counts only when its WHEEL identifies it** (#1404) — no fallback, resolved by
  `check-clang-tidy-version.sh --resolve`; known-crash shapes are rows of
  `check-clang-tidy-known-defects.sh`, and the check stays on.
- **The formatter is a BUILD too: `.clang-format-version` declares ONE PyPI clang-format release
  by its banner, and a clang-format that is not that build does not WRITE** — banner compared
  WHOLE, one resolver `check-clang-format-version.sh --resolve` and never a `clang-format-$V` name
  (#1349, #1407). `ctest -R clang-format-version`.
- A script that NAMES a tool version must name it **everywhere that version matters**, and a
  cached `find_program` result outlives every reason it was chosen — check the pin against the
  cache, not only pass it. `ctest -R local-gate-selftest`.
- A **reference build passes `-DUSE_COMPILER_CACHE=OFF`, and the gate is a reference build.**
  Pinning the other two tools argues for REMOVING this one; the refusal reads `build.ninja`, never
  `CMakeCache.txt`.
- **sccache is never selected AUTOMATICALLY** (`ALLOW_SCCACHE_FALLBACK`, default OFF): gate the
  ROW, never have CI set `CMAKE_CXX_COMPILER_LAUNCHER`. **And dropping sccache RELOCATES the
  silence to ccache rather than ending it**, so a rejection matching its `predicts` column is a
  WARNING — never `SEND_ERROR`, since this module may not fail a configure.
- A `cmake -P` check is judged by its OUTPUT, never by its exit code (`FAIL_REGULAR_EXPRESSION`,
  one spelling defined once): `message(WARNING)` exits **0**. `ctest -R fatal-error-exit` ASKS on
  every platform.
  - A failure signal that is a *property* needs both `script-check-canary` (`WILL_FAIL`) and
    `script-check-signals`; **`WILL_FAIL` inverts the WHOLE verdict**, so the canary exits **0**
    and prints a real `CMake Error` from a nested `cmake -P`.
- **A guard's own stated BLIND SPOT is nobody's to re-derive, so it must name the DIRECTION it
  fails in** (#1494), and a stripper's self-test asserts what SURVIVES, never what it removes.
- **A guard's REMEDY TEXT is part of the guard. Nothing tests it, and it is the only part of a
  check most people ever read.** Read it as **if I did exactly what this says, where do I end
  up**, and state what the rule does NOT cover in the refusal itself.
- Never silence clang-tidy with `NOLINT` — fix the source.
- **And `readability-qualified-auto`'s own suggested fix does not COMPILE on MSVC**, trading a
  Linux-only lint for a Windows-only build failure. Use `Core/Ranges.hpp`'s `FindOrNull` /
  `FindIfOrNull`, or scan for a VALUE and name no iterator.
- A return type is not part of a function's mangled name on Linux, so two functions differing only
  in return type silently collide.
- **MSVC 19.44 targeting ARM64 miscompiled two coroutine shapes** (#1545, #1546), claimed nowhere
  wider: compute a value held across a `co_await` AFTER it, keep `await_ready` trivial, and treat
  the stack-slot audit as a probe, never a check.
- An instruction-set extension is used only inside a function that asks for it
  (`__attribute__((target(...)))`), never a global `-m` flag, and runs only once
  `Core/CpuFeatures` says so. `ctest -R instruction-set-flags` reads the FLAGS; what no flag
  spells is `src/tests/InstructionSetBaseline.cpp`'s (#1447). **And every SHIPPING configure runs
  that check, before its build** — `ctest -R shipping-configure-guard`.
- `cmake/portable/CompileCache.cmake` stays stock-CMake-only and must never fail a configure: ask
  `ENABLED_LANGUAGES` first, and CHECK a flag rather than gating on a compiler-ID string. What it
  computes is checked as a computation (`ctest -R debug-prefix-map-rules`).
- A sanitizer that is on in the cache is not one that is on in the build — a tool that silently
  does nothing is worse than one that is visibly off.
- **`NDEBUG` is not optimisation, and `CMAKE_BUILD_TYPE` is a LABEL that decides nothing.** A
  benchmark states its build on **stderr** before any case runs and marks every figure, from a
  macro the COMPILER defines in the asking TU. `ctest -R bench-build-banner`.
- A Windows **Debug** leg is run for `_ITERATOR_DEBUG_LEVEL=2`, not for the compiler, so it runs
  `ctest` rather than only building: `iterator-debug-canary` must die and
  `scripts/iterator-debug-gate.ps1` refuses a build where it survives.
- **No executable raises a modal error dialog, and the BUILD installs that, never each `main`** —
  `cmake/ErrorPopups.cmake` attaches one TU to every executable and
  `ctest -R error-popup-coverage` reads the LINK LINES. A new `catch_discover_tests` names
  `FASTCACHED_ERROR_DIALOG_ENVIRONMENT` or the check refuses its cases (#1389).
- So a sanitizer job proves nothing until something proves the sanitizer. `scripts/tsan-gate.sh`
  will not report clean until every artefact's OWN OBJECT FILES carry an **undefined**
  `__tsan_init` and `src/tests/TsanCanary.cpp` has gone red with `.tsan-suppressions` active. A
  known race lives there with its issue number; deleting the entry is part of closing the issue,
  never of going green.
- That proof is asked of the OBJECTS because a binary cannot answer it — the link pulls the
  runtime in whole. Not `__tsan_func_entry`, which is per-FUNCTION.
- The canary's job is to be caught EVERY time, so a change to it is judged by a RATE and never by
  a green run: `scripts/tsan-canary-rate.sh`, the number recorded.
- An edit script asserts its anchor **matched** — `assert count == 1`, so "missing" and "not
  unique" both fire — and a generator that produced nothing fails rather than reporting success.
  Report what changed, not that the script finished.
- `producer | grep -q` is a false **negative** under `set -o pipefail`, and it fails on the
  SUCCESS path; it is a SCAN in `check-e2e-helpers.sh`, since a rule stated in the files that obey
  it reaches no file that does not. The remedy is a HERESTRING, which is not a pipe.
- The TSan scope is one Catch2 tag expression, in `tsan-gate.sh`'s `TARGETS` table, READ from
  there and never restated; `ctest -R tsan-scope-hygiene` is in the **default** set. A row is a
  directory OR a FILE, it proves a CASE and not a FILE, and **a scope selecting NOTHING is a
  refusal.** The BINARY half is `ctest -R tsan-binaries` and is not a proxy; adding a row is TWO
  edits, and **run the binary under TSan before adding its row.**
- **A workaround with an expiry date is WIRED to fire, never written down where only its own
  file's reader will meet it**: `check-tsan-scope.cmake` refuses past a WATERMARK, a pinned
  CONDITION that must not track `CMakeLists.txt`, or the comparison is `x > x`.
- A `paths-ignore` filter on a workflow whose checks are **required** makes a pull request
  unmergeable, not fast: no check run is created, so the context never reports. Master is a
  *ruleset*, so the protection API tells you nothing. Gate at the **job** level;
  `scripts/ci-scope.sh` decides.
- A **merge queue** is the third door to that same never-arrives failure: it dispatches
  `merge_group`, which `pull_request_target` does not fire on. Check the concurrency key, state
  `merge_group` in the scope classifier, add no JOB to `build.yml`;
  `ctest -R merge-queue-contexts`. **Neither the SET nor its COUNT is written in prose**, and
  nothing GUARDS this.
- A **CONFLICTING** pull request is the fourth door: GitHub computes no merge ref, so a
  `pull_request` workflow dispatches NOTHING and ITS contexts are ABSENT rather than pending —
  while `pull_request_target` ones report normally throughout, which is the tell pointing the wrong
  way. **Order matters more than the fact** — ask `mergeable` BEFORE reading a workflow, on push
  `git merge-tree --write-tree`. **And it is TWO states**, since one that BECAME conflicting keeps
  its old verdicts; `ci-pr-required.sh` says the merge state on its own line (#1352).
- A skipped job REPORTS, and a skipped REQUIRED context reads as PASSING. A skipped **matrix** job
  never expands, so nothing reports at all. Never let a dependency's failure skip a required gate:
  `if: ${{ !cancelled() }}`, not `always()`.
- A workflow must not invert its own script's principle one level up: `ci-scope.sh` escalates
  every way of not knowing to build-everything, so read it as `!= 'false'` everywhere, plus
  `!cancelled()` on every job that consults it. `ctest -R gated-jobs-fail-safe`.
- A gate that does not REPORT reads as a gate that passed, and the *required* clause is not what
  makes it so. The unit is the CONTEXT, never the job key, so `workflow_run` after the queue
  concludes, with **no `branches:` filter** on purpose. A push report is a TRANSITION, opened
  once, and a push with no branch is REFUSED, never assumed master.
- A step's `env:` is its OWN, and a whole-file grep cannot tell a line that runs from one that
  cannot. The rule is per STEP and covers EVERY `run:`: an ALLOWLIST of runner vocabulary, a shell
  model per step, a GLOB of files, every `run:` COUNTED, and a read judged at its OWN line —
  loosened only inside a LOOP or FUNCTION body. `${{ env.NAME }}` is read too, and a step's own
  `env:` does not satisfy another row of that block. `ctest -R workflow-step-env`.
- **There is ONE model of workflow YAML, `scripts/lib/workflow-walk.awk`, and it calls exactly one
  hook.** A kind with no arm returns, so every consumer spells one for every kind.
  `ctest -R workflow-walk-sole` refuses a new private walk, `ctest -R workflow-walk-selftest`
  drives every kind, and it reads a job MATRIX too (#1432) — ask per COMBINATION, never per axis.
- Every check whose SUBJECT is documentation was skipped on exactly the change it exists to catch,
  because `code=false` is right for a compiler and backwards for prose. The set is the
  `docs-subject` ctest LABEL, read out of `src/tests/CMakeLists.txt`, every way of not being able
  to run one is a REFUSAL, and it runs from an UNGATED step of the job whose `name:` is the
  required context `Check C++ style` — a wire constant.
- **A flag combination that cannot express the question still returns an answer**, and none of
  them errors — cleanest specimen `grep -Lq`. *A control is the one instrument nobody checks,
  because checking it is what it was for*: re-derive one by a different construction.
- **An intermediate reading is BIASED, not noisy** — a tree sampled mid-build can only be MISSING
  artefacts. **Do not take the reading**; wait for the completion signal.
- **CMake WRAPS its diagnostic messages**, so a phrase you grep for can exist in the output and in
  no single LINE of it; the `STATUS` row does NOT wrap, which is the trap inside the trap. Flatten
  (`tr '\n' ' ' | tr -s ' '`), or match a phrase that cannot straddle 74 columns.
- Five ways an instrument reported on something other than its subject. A **COMMENT is not a call
  site**. **`bash <path>`, never a bare path**, in a self-test AND in a workflow.
  **`IFS=$'\t' read` does not read TSV.** **A fixture built on `message(FATAL_ERROR)` cannot test
  that verdicts are read from OUTPUT.** And **a self-test that stops early must not look like one
  that judged something**, so they print how many cases they ran.
- A bracket-vulnerable `cmake -P` reader is judged per **(reader, file, surviving lines)** and
  never per script: one left alone because today's files happen to be safe is a defect scheduled
  for later.
- **And the usual remedy is wrong where the brackets are the DATA** — a Catch2 tag IS `[async]`,
  so blanking `[`/`]` makes that reader refuse a good tree; it is a list-free `FIND`/`SUBSTRING`
  walk instead. Consolidating the splitting idiom is **#495**, not the ticket in front of you.
- **`if(VAR STREQUAL "")` does not fire when VAR is UNSET**, and the one place that matters is a
  glob that came back empty: `set(x ${empty})` unsets `x`. Quote the variable.
- **A clean-tree injection understates a check that only reports violations**, so plant the
  violation. Three arms, and the third is not decoration: violation alone, violation behind a
  bracket, bracket alone.
- **A census cannot falsify a premise — it can only produce a number consistent with it.**
  Re-derive an inherited claim BEFORE the census, never after: **agreement between a fresh
  measurement and a source you have not read is worth nothing**.
- **A listing that came back AT its `--limit` is an answer about a set that is not the whole
  set**, and raising the limit only moves the cliff. `ctest -R gh-listing-seam` requires every
  `gh` listing to go through a seam that warns at the cap, or to state why it does not.
- **A census states its PATTERN, not only its number**, because **a pattern is broader than its
  author reads it as** — so the pattern travels with the figure. And **a ticket cannot be closed
  against a count that no longer describes the tree.**
- **Which files are this project's own is ONE answer: `scripts/lib/third-party-roots.txt`.** A
  script listing the repository's files asks it (`first_party_paths`,
  `fastcached_decline_third_party`) and NAMES what it declined, or carries an exemption row with a
  reason. `ctest -R third-party-roots`; a root is the upstream COPY (`vendor/endo`).
- **And HOW a file set is found is one answer too: `fastcached_tracked_files`.** A `check-*.cmake`
  keeps its QUESTION and never its own work-tree probe, `ls-files` call, walk fallback or copy of
  the excluded names; `fastcached_first_party_cxx` is a CALLER, and the probe is
  `fastcached_work_tree_state`, THREE answers. `ctest -R tracked-files-selftest`.
- **A total stated beside a table is DERIVED from it, or it is a second claim.**
  `ctest -R table-totals`. The multipliers are PARSED, not banned; the marker is MANDATORY with
  `none` as the opt-out; and the figure that describes a table is the one NEAREST it.
- `clang-format -i` at any build but the pinned one silently reformats code the pinned one
  accepted; run another binary as `--dry-run` only. An older clang-tidy is worse than a laxer one:
  it is *silent* about checks that do not exist in it yet.
- A clang-tidy sweep that cannot prove the tool ran is worth nothing and reads like success —
  `scripts/tidy-sweep.sh` canaries it first and treats a failure to execute as fatal.
- A ccache hit does NOT skip clang-tidy: the launcher and the analyser are two independent
  commands under `cmake -E __run_co_compile`. Anything that changes how EVERY unit is read gets a
  row in `SweepEverythingWhen`.
- A compile database generated for clang-tidy needs `CMAKE_CXX_SCAN_FOR_MODULES=OFF` named
  explicitly. Without it every unit fails to parse and the sweep reports clean.
- And it must be configured with the same TARGET SET CI builds: a changed file with no compile
  command is dropped silently, and must be. Account for every file in the diff the sweep did not
  reach, before trusting its count.
- **Running the launcher is not testing it.** The synthetic fixtures prove it RUNS and produces AN
  object; `scripts/launcher-replay-e2e.sh` builds three times and runs the replayed binary.
  Compare cold-against-warm, never control-against-warm, and staging a wrong object needs the LINK
  command, never `touch`.
- A compiler cache that reads like success is worse than none: the jobs are asserted to be backed
  by the Actions cache — before `ctest`, which zeroes the sccache counters.
- Every `bool` and byte-wide enum in a config struct lives in one run: one between two 8-aligned
  members costs seven bytes, and clang-tidy's padding budget fails the build.
- A table indexed by an enumerator is `EnumTable<Enum, Row>` + `RowsInEnumeratorOrder`. A length
  anchored on an enumerator by name is a guard that fires only when nothing is wrong.
- An enum's ENUMERATORS are `Enumerators<Enum>()` / `Enumerators(from)`, never `views::iota` to
  `EnumeratorCount<Enum>` with a `static_cast` in the body. `ctest -R enumerator-walks` refuses
  it, but not a `static_cast` beside an `EnumeratorCount`, nor the C-style form, which is #1452's.
- **A C-style loop is classified by its BODY; the head is not a classifier** (#1452, closed with
  its backlog EMPTY) — a head-shaped verdict is a CANDIDATE, never a clearance. **Five shapes are
  not a RANGE-FOR, and "not a range-for" is not "stays a `for`"**, each with its own target:
  - the body ADVANCES the variable → a `while` with ONE advance at its foot;
  - a CALLEE advances it through `std::size_t&` (`ApplyOneOption`, `TakeValue`) → that `while`, or
    an exemption;
  - a COMPOUND bound → the range, plus a `break` asked where the head asked it;
  - the induction variable OUTLIVES the loop (`RedisResp.cpp`) → `while`s over one cursor;
  - erase-while-walking → an ALGORITHM: `std::erase_if`, or `ranges::find` and one `erase`.

  An INCLUSIVE bound is `iota(first, N + 1)` and a DESCENDING one that range through
  `views::reverse`, each with its premise stated; **what stays a `for` is a sequence the standard
  library has no range for**, with an exemption row stating why. **Overstating what is wrong is
  the same defect as understating it**, and **a text scan fails toward "nothing unusual here"**,
  so such a classifier fails CLOSED. Target `std::views::iota`, never the `Ranges::` seam; for
  argv, `std::span{argv, argc}.subspan(1)`.
- Coverage is Clang source-based, never gcov: ~2000 Catch2 cases are ~2000 processes, and gcov's
  shared `.gcda` races them. `%8m`, not `%p`. A report counting `*_test.cpp` measures the tests
  testing themselves, and a compiler cache and coverage cannot be combined.
- A rulebook `## Open work` entry names an OPEN issue, or it is a rule that has gone false — the
  expensive shape being one saying something *cannot* be done. `ctest -R rulebook-open-work`. The
  ENTRY is a bullet's LEADING reference, `gh` falls back to PULL REQUESTS so the KIND is asserted,
  and the verdict is the HTTP status behind a liveness anchor.
- **A configure's OUTPUT is the module's CLAIM; the generated buildsystem is the artefact** — read
  Ninja's per-rule `LAUNCHER =` or the Makefile compile line, never `CMakeCache.txt`. A generator
  the reader cannot parse is a THIRD state, named; normalise a path as a PATH, on BOTH sides. Its
  sibling: **the DECISION a guard makes is what gets tested, not the acquisition around it.**
- A branch BEHIND master is unverified, and only a build says otherwise. **How far behind is not a
  measure of the risk**, and **file overlap is evidence only in the direction that says there IS a
  hazard** — the hazard is a NAME shared between two changes, which a diff cannot show. So a
  branch is rebased and rebuilt before it merges, never inspected.
  - **And the diff that VERIFIES such a rebase is three-dot.** `A..B` folds in the commits `B`
    carries and `A` does not, producing a wrong NUMBER rather than an error; the two forms
    disagreeing is itself the signal that the branch is behind its base.
- **After a revert, test for the REVERT, never for the defect.** A revert leaves the reverted
  commit in the ancestry forever, so `--is-ancestor <fix>` answers YES for every branch; the only
  useful question is `--is-ancestor <revert>` coming back NO.
- **A subject line is an abbreviated identifier with no prefix to disagree about.** Same family as
  a ctest index and two worktrees one token apart — **a leg travels with its compiler and its
  machine.**
- **A reason that generalises further than the fact it was drawn from is worse than the narrow
  one**, because it reads as licence somewhere it was never measured — and it is introduced while
  TIDYING, which is when it is least likely to be re-checked.
- A reference-build refusal is a REFUSAL and not a warning, because a verdict about a tree that
  was not built cannot be read in EITHER direction; that sentence lives in the refusal's own text.
  The count is LAUNCHER **BINDINGS**, not edges — **a unit error in a refusal message is how two
  numbers get compared that should not be.**
- **A diagnostic that never RAN and one that ran and found nothing are the same green.** Keep
  `continue-on-error`, **assert the classifier was REACHED**, and give it **three** outcomes. And
  **a green test on the wrong object is worse than no test, because it RETIRES THE SUSPICION** —
  where tested and shipped differ as TEXT, extract the step's own `run:` block from the workflow
  and execute THAT.
- **A probe of one capability cannot clear a runner that degraded in another, and a ctest TIMEOUT
  is not a verdict about the tree** (#1515). The verdict is `scripts/ci-ctest-red-reading.sh`'s,
  from `--output-log`. Never raise the `TIMEOUT` instead.
- `PEDANTIC_COMPILER_WERROR` decides **fatality, not which warnings exist**, so a flag and the
  suppressions it makes necessary are governed by ONE condition. And **a symptom with two
  mechanisms reads as unreproducible the moment either one alone is ruled out.**
- **A build under WSL is I/O-bound on `/mnt/<drive>`, not CPU-bound.** Keep the BUILD TREE on the
  Linux filesystem (`cmake -S /mnt/... -B "$HOME/bld/<name>"`); sources may stay. Derive `-j` from
  the host and set `CTEST_PARALLEL_LEVEL`, or the suite runs SERIAL in silence.
- **When the SUBJECT under test is the build environment, a green local gate is not weak evidence
  — it is none.** An exclusion list bets on the world's layout; an inclusion list states your own.
  Git Bash rewrites a `/`-led MSVC flag into a path — `MSYS2_ARG_CONV_EXCL='*'` **and**
  `MSYS_NO_PATHCONV=1`, both spellings, always — and flags are dropped from a TABLE keyed on the
  driver NAME, never by sniffing a leading `/`. **And that switch belongs to the SPAWN, never to
  an environment `ctest` inherits**: no test in `src/tests` inherits it on Windows, Windows script
  tests run Git's `bin/bash.exe`, and a check in a work tree the git on PATH cannot read is
  REFUSED (#1355).

**[`.agent/rules/testing.md`](.agent/rules/testing.md)** — how tests are registered
and what they may assume.
- `ctest --repeat until-fail:N` reports the LAST iteration, so a 1% flake reads as
  `100% tests passed`. `scripts/flake-rate.sh` tallies, keeps the early stop, and states its N.
- **A wall clock is not a duration**: `SECONDS`, `TIMEFORMAT='%3R'` and `date` read
  CLOCK_REALTIME, stepped BOTH ways. Assert what a helper DECIDED or time against something
  monotonic; a negative reads as the FASTEST band, so the repair is a third OUTCOME recorded
  where the SUBTRACTION happens, never a clamp. The variable is the ENVIRONMENT, not the load.
- **Adding an OUTCOME changes what an old condition does to every case that tolerated the wrong
  answer**: enumerate what was asserted about the output it replaces, and pin the input.
- **Assert what DISTINGUISHES, not what both sides produce.** A refusal test asserts WHICH
  refusal. **Prove the test can fail** — neuter the fix and check the failures are the ones you
  expect AND ONLY THOSE; the asymmetry is the evidence.
- **Three fixtures that could not fail came out of one batch, none found by reviewing the
  assertion** — what finds them is neutering the fix, building the neighbouring case, and asking
  how production ACQUIRES what the fixture acquires.
- A fixture that RE-ACQUIRES a collaborator the production code binds ONCE is testing a different
  object, so its assertion can be exactly right over a live defect. Bind it the way the call sites
  do, before the mutation; the tell is that the fixture is MORE CONVENIENT than production.
- Every wait is bounded and says what it waited for — and, when it times out, which KIND of failure
  it was: cost on success, process alive, log growth, CPU burned. Disagreeing signals are
  INCONCLUSIVE, **a place to ask somebody ELSE** — on the FAILURE PATH, every command `|| true`.
- **A Catch2 assertion runs on its case's thread, never a helper's** — `OffThreadAssertionGuard`
  ends the process at one, in every test binary (#1211). Return the observation and assert after
  `.get()`/`.join()`; it cannot see `INFO`/`CAPTURE`/`SECTION`.
- A C++ test waits through `src/tests/BoundedWait.hpp` — `WaitUntil` on the case's thread,
  `OffThreadWaits` on a helper thread, `AwaitUntil` in a coroutine, which STOPS when its wait runs
  out — never a counted or unbounded poll. `ctest -R test-loops` refuses an atomic-polling `while`
  and a coroutine `while` that OPENS by parking **in tests** — it fails open on a park spelled any
  other way — and a C-style `for` **anywhere under `src/`**; `scripts/check-test-loops-backlog.txt`
  is a RATCHET. **The count is not repeated here on purpose.** **Neuter a new wait and watch the
  teardown.**
- A **cumulative** figure cannot answer a question about **now**, and a duty cycle over the same
  window is the same number divided by the same constant. Draw the verdict from a RECENT window;
  no magnitude bar calibrates but **zero does not vary**, so test presence over the process TREE.
- An `-or` is right for two independent CONFIRMATIONS and wrong for two competing READINGS, and a
  signal that cannot be false in the failing case is not evidence. An instrument printing a
  **remedy** cannot know when the remedy is under dispute — state the finding and stop.
- A classifier that cannot be made to say BLOCKED cannot report a hang, so each verdict is driven
  against a synthesised readings record, in the default set.
- A stand-in built to exhibit a MAGNITUDE must not be measured through an instrument whose own
  overhead is comparable to it — the noise IS the interpreter. Split the DECISION out as a pure
  function over a record; leave acquisition alone.
- A fixture waits on what a line MEANS, not on its wording. Wait on the STAGE, and keep the phases
  separate so a stall says which; a blown budget is the symptom, never the cause.
- **A fixture that has never completed has told you nothing**, however carefully it was read. A flag
  spelling is checked against `CliOptions()`, never remembered; `exit` inside a `( ... )` ends the
  subshell only, so a `fail` helper signals the top-level shell unconditionally.
- **A guard written to prove a fixture bites can itself fail to bite**, naming the subject as broken
  when the instrument is. An injection is ASSERTED, in the artefact and again in what consumes it.
- **Attribute by asking the process, never by adjacency in interleaved output.** A wrapper labels the
  unit from the same process inside the same edge; an unlabelled outcome is counted as NEITHER and
  refused by name, and an unattributed total hides the only column that means anything.
- A `$<TARGET_FILE:x>` naming a target that was NOT built is a hard error at **generate** time, not a
  skipped test: guard the block on `TARGET x` as well as on the feature that makes the test
  interesting. `ctest -R target-file-guards` reads TWO sources; never merge their counts.
- A fixture whose client is always LOCAL cannot test who is admitted: `Classify` returns `Member` for
  the whole of `127.0.0.0/8` before the member list, so only the host's OWN non-loopback address
  reaches it. Assert BOTH directions with the refusal COUNTER moving; no such address is a loud SKIP.
- A script-driven test naming more than one executable is registered in `src/tests`, not beside a
  binary.
- An abbreviated identifier is a DISPLAY form: the full one is read, never padded, truncated or
  re-derived. **Zero rows is not a verdict, it is the absence of one** — check the SHAPE first,
  fail hard on what will not parse, say what was searched, and give a zero census a POSITIVE CONTROL.
- Tests allocate their ports per run rather than fixing them — from **below** the kernel's ephemeral
  range, and remembered, since a connect probe cannot see a port held as an outbound connection's
  local endpoint. **A fixed one needs a reaper**; a holder is **refused, never adopted** — except
  one whose image path is byte-for-byte this run's daemon, which is REAPED, and one whose path
  cannot be READ, which is refused or the reap arm kills a process nothing knows. The DECISION is
  what gets tested.
- `Unwrap(x)` after `REQUIRE(x.has_value())` for `std::optional`; a bare `*x` is a build failure.
- A Catch2 case name is an ARGUMENT, and that one fact has three consequences. It may not begin with
  `-`. A **COMMA** splits the spec, so such a name selects NOTHING and `No tests ran` exits
  **non-zero** — a deterministic red, not an empty result. Select by TAG, and **anything selecting a
  subset asserts how many cases RAN**; write it as *the name is an argument*.
- A DUPLICATED case name registers two ctest entries that each run BOTH cases — nothing is skipped,
  ATTRIBUTION breaks, so it reads as harmless. `test-name-hygiene` refuses duplicates; an exemption
  row carries a REASON and is refused STALE once it stops describing one.
- **A failing `REQUIRE` above an explicit `Stop()` turns a RED into a HANG.** Catch2 unwinds, so the
  failure skips the stop and `~jthread` joins a loop nobody stopped; it concentrates in TEARDOWN
  tests, and `CHECK` does not unwind. Stop BEFORE asserting, or use RAII — where **declaration order
  is half of it**. **Not soundly mechanizable**, so no check is written, deliberately.
- A fixture states which PATH it exercised. A synthetic tree is not a git repository, so a check
  with a `git ls-files` primary and a walk fallback self-tested only the FALLBACK while CI took
  GIT. The mode is part of the OUTPUT and asserted on both sides.
- A Catch2 `SKIP(...)` exits **4**, and ctest must be told what that means or it scores a skip as a
  FAILURE. **And the mechanism that fixes that RED is itself broken**: Catch2's exit code **is** its
  failed-assertion count clamped at 255, so `SKIP_RETURN_CODE 4` scores a case failing exactly four
  assertions as *skipped*. It stays because all three alternative channels are worse and **no Linux
  gate can reproduce any of that**; the fix is a change of MECHANISM, open as #1152.
  `catch-skip-exit-collision` asserts the premises, `src/tests/CatchSkipCanary.cpp` the shapes, and
  **adding shapes cannot make a pattern safe.**
- The converse, and the direction nobody investigates: `SUCCEED` is right when a case RAN and had
  nothing to assert, wrong when it could not run — a PASS for a property nothing established,
  exactly where coverage is thinnest. "Covered by another test" is a reason to SKIP, never to pass.
  `ctest -R succeed-not-skip` reads two signals; a `SUCCEED` it cannot read is refused, not cleared.
- A scratch directory comes from `src/tests/ScratchPath.hpp`. A per-process counter is not unique —
  `catch_discover_tests` gives every case its own process, and the suite runs in parallel.
- **And neither is `std::random_device`**, which on one host here answers zero often enough that every
  process picks the identical "random" name and the second is refused `InUse` by a lock doing its job
  (#1507). `ctest -R unique-temp-paths` is the scan, because no type forces a test to reach the seam.
- A test FAKE is a shared helper too: `src/tests/ScriptedSocket.hpp`, and the membership oracles are
  `src/tests/MembershipFakes.hpp` (`ctest -R membership-fakes`). A WRONG fake makes its cases pass,
  so no failure ever finds it, and a copy's cost GROWS — the shared fake takes the participant
  REQUIRED and undefaulted.
- And a fake that resolves SYNCHRONOUSLY what production SUSPENDS on cannot exercise a suspension
  protocol, however correct its assertions: every property defined by parking is vacuous over
  `InMemorySocket`. The survey of which have a real-socket case is in the rules file.
- `InMemorySocket`'s CLOSED states are one table pinned against a real loopback pair
  (`Net/SocketClosedStates_test.cpp`): where the platforms disagree it answers the way Windows does,
  and on Windows every row is exact. A fake MORE permissive than a real socket is a red there (#1553).
- So is a BUILDER, and it hides better: `src/tests/ForeignGenerationValue.hpp`. Hand-rolled copies
  fail SILENTLY, since every one asserts a REFUSAL that other damage also produces. Two facts live
  there alone: WHICH byte carries the generation, and WHICH generation is foreign — DERIVED, never
  `CompileValueVersion + 1`. A hand-built FRAME is a different subject and is not routed through it.
- The POSIX shell fixtures share `scripts/lib/e2e-common.sh`, tested by `ctest -R
  e2e-helpers-selftest`. A wait's bound is read from a **clock** and its timeout reports the
  **measured** elapsed; a bespoke condition is a predicate for `wait_until`, never a new loop. And
  `fail` signals the top-level shell unconditionally rather than testing `BASHPID` — across **every**
  WALKED script under `scripts/`, where a file matching its own scan exempts a REGION, never itself.
- **A background helper in these scripts runs the fixture's CLEANUP until you have watched it not.**
  Background work inherits the shell's traps, and in a fixture the EXIT trap IS the cleanup.
  `trap - EXIT TERM INT HUP` inside the subshell is **insufficient** — it misses the window before
  the subshell starts — while `kill -KILL` is decisive; BOTH stay, each naming the window it closes.
- A fleet property that spans two machines needs `src/tests/FleetHarness.hpp`, whose `OnCompile`
  places the interleaving rather than waiting for one. It lives in `src/tests/` because
  `src/FastCache/` must not include an app header. A harness earns its place by a property shown RED
  when its rule is removed — proved beside a case that stays GREEN under the same break.
- A leader-pinned **mutating** command is put to whoever leads NOW, re-derived, never to an endpoint
  an earlier section recorded, and the retry keys on the answer the caller ASSERTS rather than on a
  recognised refusal wording — which is also what lets one helper assert a REFUSAL. **A helper
  implementing a rule does not spread it, only a call site using it does.**
- A first failure MASKS its identical siblings, so fixing only the OBSERVED site relocates the flake
  — and the relocated one then presents as a regression introduced by the fix. Audit the shape
  mechanically; nothing about a sibling ever looks suspicious.

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
Prefer `std::expected<T, E>` for fallible API surface. The error taxonomy is split:
`NetError`, `ProtocolError`, `StorageError`, `ConfigError`. Chain monadically with
`and_then`, `or_else`, `transform`, `transform_error` rather than nested `if`s. Reserve
exceptions for programmer errors (precondition violation, contract misuse).

### Dependency injection
**This is a load-bearing principle, not a nice-to-have.** Anything that touches I/O,
time, randomness, the filesystem, the network, or any other ambient/global resource is
reached through an interface — never a concrete type, a singleton, or a free function
with hidden state. The existing seams are `IClock`, `IReactor`, `ISocket`/`IListener`,
`IStorage`, `ILogger`, `IDaemonHost`, `ISignalSource`, `IAdmissionControl`,
`IMetricsSink`. Collaborators are passed in (usually by reference or `unique_ptr` at
construction), so every layer can be exercised in isolation: tests substitute
deterministic fakes — `ManualClock`, `TestReactor`, `InMemoryTransport`, `NullLogger`,
`CapturingLogger`, `ScriptedSignalSource` — and the whole server runs end-to-end
without a real socket or a real clock.

**Define the interface first and inject it.** Wanting a global, a `static` mutable, or
a direct `::time()`/`::read()`/`new ConcreteThing` call in business logic is the signal
to introduce or reuse a seam. Deviate only with a *strong, explicitly stated* reason
(e.g. a genuinely pure leaf computation with no environment coupling); the default
answer is "inject it".

### Data-driven design
**Behaviour is described by data; code interprets that data.** Equally load-bearing and
well beyond "no magic numbers": adding a flag, a protocol verb, a storage backend or an
error code is *adding a row to a table*, not editing logic scattered across the tree.

- **One source of truth per concept** — the CLI flag table is data, the storage-record
  layout is derived in one place, each per-DBMS / per-protocol dispatch is a single
  switch. Exactly one place to change when the concept changes.
- **No naive, hand-rolled repetition.** Two branches differing only by a value — a
  constant, a name, a type — are a defect: lift the value into a descriptor/table and
  write the logic once, or use a small generic helper.
- **Built for extension.** The next case (flag, verb, backend, metric, signal) is a new
  table entry or a new interface implementation, never a new `if`/`else` arm threaded
  through existing functions. Open for extension, closed for invasive modification.
- **Tables over conditionals.** A `switch`/`if` ladder mirroring a fixed set of named
  things is a table in disguise; express it as data (a descriptor array, a lookup map,
  a dispatch table) and drive it with a range-based loop or a `std::ranges` pipeline.
- **A table indexed by an enumerator is `EnumTable<Enum, Row>`, guarded by
  `RowsInEnumeratorOrder`** (`Core/EnumTable.hpp`): the enum states its own count with a
  trailing `Last`, the table takes its extent from that, and one `static_assert` checks
  the extent and every row's position. Never anchor the length on an enumerator by name
  — that guard fires only when nothing is wrong.
  [`.agent/rules/build-and-toolchain.md`](.agent/rules/build-and-toolchain.md).

As with DI, **adhere unless there is a very strong, explicitly justified reason not
to.** When in doubt: "if a sixth case showed up tomorrow, how many places would I edit?"
More than one, and the design is not data-driven enough yet.

### RAII for resource handles
Sockets, listeners, log files, coroutine handles — every resource is owned by an RAII
wrapper. `PooledBuffer` returns to its `BufferPool` on destruction; `Task<T>`'s
`Awaiter` takes ownership of the coroutine handle on construction so the temporary
`Task` cannot tear the coroutine down across a suspend point.

### Caching an expensive repeated answer
**An answer that costs a spawn, a syscall or a walk, and is asked for more than once,
is cached — but only where staleness degrades safely.** That second clause is the whole
rule, so **decide safety FIRST, because it decides whether to cache at all**: staleness
that costs a refusal, a miss or a retry is safe (it fails **closed** and self-heals),
while staleness that produces a wrong answer which LOOKS right is not, however
expensive the probe. **Expense is not the criterion; what a stale answer *does* is** —
`DiscoverTargetTriple` is deliberately not memoized because a stale triple is a wrong
hit rather than a miss
([#188](https://github.com/LASTRADA-Software/fastcached/issues/188)).

Where it IS safe, a cache still owes five things: **measure before choosing, on every
platform** (`GetAdaptersAddresses` against `getifaddrs` — two orders of magnitude, so a
design that is free where you develop can dominate a request where you ship); **fast by
construction before fast by cache** (`IsLoopbackHost` first and lock-free), so the cache
bounds only the rare path; **refresh on an INTERVAL, never on a miss**, or a remote peer
has a free amplifier — one expensive probe per request, just by asking;
**name both staleness directions in the header**, which is what makes a longer interval
defensible; and **an injected seam with an injected clock**, because a cache with a
hidden clock is untestable by construction. Each is derived with its measurement in
[`.agent/rules/compile-cache.md`](.agent/rules/compile-cache.md) and
[`.agent/rules/distributed-compilation.md`](.agent/rules/distributed-compilation.md).

**A performance figure is a quantity UNDER CONDITIONS, and the two halves get lost
separately.** Attaching the conditions is necessary and **not sufficient: the citation
is where they get lost**, so a figure others will refer to lives in ONE place they point
at, never restated — and a figure can be current, correctly measured and still **the
wrong quantity**. Record a table of conditions, not a number.
- **The same rule governs a claim handed between PEOPLE, and a handoff IS a citation.**
  The remedy is on the SENDING end because only it can be: **say what was MEASURED and
  what was INFERRED, separately, every time.**
- **The receiver owes one thing back: state what would FALSIFY a claim BEFORE opening
  the file to check it**, or reading for confirmation stops at the first line that
  matches. **A relayed diagnosis is relayed code.**
- **It does NOT reach a measurement's CONDITIONS — pin those, do not point at them**,
  and say *why* they are pinned, or the next cleanup reverses it: the corrected comment
  looks like the defect.

All three, with the measurements that produced them, are in
[`.agent/rules/compile-cache.md`](.agent/rules/compile-cache.md).

**And prefer not needing the cache. Computing a value once and returning what you
already have beats caching it**: a wider return value is also the STRONGER answer, not
merely the cheaper one, since two derivations a few milliseconds apart can disagree.
**Evidence a caller may not have is a disengaged `optional`, never an empty field** —
empty roots and an empty stamp are both ordinary answers, so neither can carry "there
was no probe".

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
  (`.clang-format-version` and `.clang-tidy-version`, each an exact PyPI release). Successive LLVM
  releases disagree with each other, so a
  tree clean under whichever binary is on `PATH` can still be rejected. Format through
  `scripts/check-clang-format-version.sh --resolve`, resolve clang-tidy through
  `scripts/check-clang-tidy-version.sh --resolve`
  and use a build directory of its own; the `clang-debug` preset is **not** that sweep.
- **`clang-tidy` reports must be fixed at the source.** Never silence with `NOLINT`. The `clang-debug` preset enables `clang-tidy` at whatever version `PATH` resolves to, which is why **`scripts/local-gate.sh` passes `-DCLANG_TIDY_EXE=` the declared build `check-clang-tidy-version.sh --resolve` identifies, and refuses to start when there is none** rather than letting the preset pick — so running the preset by hand is not the sweep CI enforces.
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
fetches into the build tree as before. Measured (Git Bash, Windows 11, cold build tree,
native Windows volumes rather than DrvFs): **45 s and 118 MB fetched without it, 24 s
and 1.1 MB with** — and over DrvFs, where #545 found it, the same fetch is slow enough
to read as a hang. *Volumes* rather than a filesystem NAME, because the two halves of
this measurement do not share one: the cache sits under `%LOCALAPPDATA%` and the build
tree does not, so on the machine this was taken on they are different filesystems. The
contrast the figure exists to draw is native-against-DrvFs, and that holds either way.

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
# The targets are the gate's TARGETS rows plus the canary. Nothing checks this list --
# it had lost two rows -- but the gate refuses to run over a row whose binary was never
# built, so a short list fails loudly rather than sanitizing less.
cmake --build --preset clang-tsan --target FastCacheTest fastcache-compile-node-tests fastcache-cc-tests fastcache-cli-tests tsan-canary
bash scripts/tsan-gate.sh out/build/clang-tsan

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
