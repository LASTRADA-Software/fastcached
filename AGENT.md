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
- A compiler with debug info on records the WORKING DIRECTORY, which is on no command
  line, so no key can relativize it and a hit replays an object naming the producing
  checkout. Measured: `g++` 143 B, `clang++` 6 B, `clang-cl` 23 B, and `cl` **11 B with
  no debug flag at all** — `.debug$S`'s `S_OBJNAME` holds the absolute object path, so
  `/Z7` widens this and does not open it. `-fdebug-prefix-map` closes it on ELF and on
  NEITHER COFF driver; that residue is an accepted cost, not open work.
  - Never `-ffile-prefix-map`, and no table row for it or `-fmacro-prefix-map` either:
    both rewrite `__FILE__` INTO the text the key hashes, and a row would make the key
    hash text the compile never produced.
  - The flag names the producing root BY CONSTRUCTION, so the key relativizes its head
    (`PathValueRole::PrefixMap`, a table COLUMN rather than a branch) and leaves the
    replacement literal — two machines mapping differently must MISS.
  - The build-tree rule is mapped LAST: both drivers honour the LAST match, measured off
    `DW_AT_comp_dir`. No object comparison can see this, since both orders are
    checkout-independent.
  - Relative is not checkout-independent — `file(RELATIVE_PATH)` answers
    `../../mnt/d/.../checkout` out-of-tree, and with a trailing separator — and a root
    with a SPACE is not mapped at all, the rules being spliced into a space-separated
    flags string. `ctest -R debug-prefix-map-rules`.
  - A DISPATCHED compile is not covered ([#506](https://github.com/LASTRADA-Software/fastcached/issues/506)):
    `RemoteCompileArgs` drops every path-valued flag.
- An object file is not a byte string. `FASTCACHE_VERIFY` compared one with `memcmp`,
  and every MSVC driver stamps the CLOCK into the COFF header — a cached object is
  older than the fresh one BY CONSTRUCTION, so every Windows hit reported a wrong
  object on the platform where #368 was observed. Measured: the 4-byte `TimeDateStamp`
  and nothing else at 2 s and at 300 s; ELF is identical, `-g` included, so it keeps
  the byte comparison. `.debug$S`/`.chks64` are volatile in the PATH, not in time, and
  the verifier holds the path fixed — so they are NOT excused, or #489 (a hit from
  another checkout) goes silent, which is #493 cured by no longer looking. Parsing
  never grants an excuse; it only says WHERE. A FRESH object that will not lay out is
  `Unsupported`, refused by name; a SERVED one that will not while the fresh one does
  is `Mismatched`, because that is a truncated transfer. Never `/Brepro`.

**[`.agent/rules/distributed-compilation.md`](.agent/rules/distributed-compilation.md)**
— dispatch, workers, the scheduler, the node's tiers. Before `Distributed/`,
`apps/fastcache-compile-node/`.
- The text sent to a worker is **not** the text the key hashed; dispatch
  preprocesses a second time, with `#line` markers.
- A worker is told its input is preprocessed *and* what language it is in; the
  file extension is the last of three answers, never the first.
- A cache exchange is bounded by a round trip, a dispatched compile by how long a
  COMPILER runs; sharing one deadline abandoned every TU worth distributing while the
  worker finished the job anyway. And a per-call `SO_RCVTIMEO` is not a bound at all.
- That language is stated by the flags dispatch APPENDS, so a build that named one
  itself (`/TP`, which CMake emits for every MSVC C++ source) is folded into the
  language and dropped — never refused, which made the whole fleet cache and
  distribute nothing while every scheduler counter read zero. A selector naming a
  FILE, or an `-x` value with no exact language, is still refused.
- Leadership and membership are one `Gate()`, and it runs for every verb — reads
  included. But they answer different questions and only leadership stops applying at
  demotion: a RELEASE settles an obligation this node itself created, in a per-node
  `LeaseTable` nobody else holds, so gating it on leadership pinned the key on the one
  machine that could free it (#371). A `GateScope`, never a verb that skips the gate;
  membership is never relaxed; `Scheduling` is the default. A verb qualifies only if it
  touches non-replicated state this node created AND can create nothing — both clauses,
  since `Lease` passes the first. And the settlement still refuses a token it never
  issued, or the fix is a hole.
- Duplicate suppression is asked **before** capacity, or a busy fleet reports
  `NoCapacity` for a key it is already building.
- A lease has three transitions and expiry is the third: the **client** resolves it,
  on every path out of the compile, over a fresh connection. Expiry is the safety net
  for a client that died.
- A listen flag answers "does this port face the network" only when this process
  bound the port. Under socket activation the unit owns the address and the flag
  still holds a value that describes nothing — a stale loopback value passed the
  startup table and served an unauthenticated compile port. The config table's rule
  is insufficient there, not wrong, so both it and the runtime guard stay. Recorded
  against `--bind`, which #290 stage 3 deleted; `--listen-node` inherits the hole and
  is now the only flag, so it is wider.
- Whether a worker **checks** a lease is a startup decision, never a per-request
  fallback — skipped per request, the port is open and every refusal counter reads
  zero. The question is "can a machine that is not this one reach the compile
  surface", not "is a key configured": a loopback bind and a loopback-only policy
  each close it on their own. A validator returns a REASON rather than a `bool`,
  captures the worker's own endpoint rather than taking one, and never answers
  `UnknownLease` — that is the scheduler's code.
- A resolve answers on liveness, not presence — an unknown token is refused, because
  that is the only place "this job outlived its lease" can be observed.
- A lease token is a credential, and its MAC covers the granted **endpoint** or it is
  a credential for every worker that trusts the key. Fields length-prefixed, never
  joined — an endpoint is `host:port`. Own domain label: the same PSK MACs discovery
  proofs.
- The MAC is checked before any other claim is reported on, or a named refusal is an
  oracle. The expiry bounds how long a *captured* token is useful and is **not** a
  capacity bound — slots are.
- No key means the SCHEDULER signs nothing: unsigned grants and one bounded warning,
  never a silent fallback. Its startup refusal is still open (#303) and must take the
  worker's shape above, or it breaks every single-machine install.
- A worker being dropped is an **event** (`ExpireStale`), or nothing releases what
  was held against it. A node that restarts inside the heartbeat window is the second
  route to the same pin, and `Register` closes it.
- A discovery layout describes a **directory layout, not a vendor**: Visual Studio
  ships its own clang-cl under `VC\Tools\Llvm`, which the `visual-studio` row walked
  past and the three standalone-LLVM rows never reached — so those builds were cached
  and could never be dispatched. One installation, two rows; `vswhere`'s answer is
  memoized across them, empty answers included.
- A port this node LISTENS on is a row of `NodeSurfaceTable()`, and an opener takes
  the `NodeSurface` — not a listen spec, a default host and a name. The port map lived
  in five places plus the docs; the guard is the type system, since there is no
  argument to pass a bare string to. Protocol is a column (discovery is the only UDP
  surface), so is the host a bare port falls back to. `--print-surfaces` prints the
  RESOLVED configuration. `--advertise` is not a surface — it is told, not opened.
- `--cache-memory 0` means no tier. Zero is how `InMemoryLruStorage` spells
  *unbounded*, so the flag that turns a cache off once turned its limit off.
- What a node holds back from compiles is what its tier **built**, never what a flag
  asked for — so capacity is derived *below* the tier startup. Which tiers cost RAM
  is a column of `StorageTierTable`, and a present zero is *unbounded*, not nothing.
- A node SERVES while it identifies its toolchains. The cheap half (WHICH compilers)
  stays at startup; the walk (over 300 s cold, at 2.7% CPU duty — a filesystem wall,
  not a thread shortage) moves to the heartbeat thread's first round. It registers
  NOTHING until the fingerprint is real, which is #225 and falls out of a
  `ServedToolchain` needing one. An empty map is two opposite answers, so
  `ToolchainSurvey` travels beside it with a deleted default constructor. "Nothing to
  serve" stays fatal; only its timing moves. `/healthz` stays green, or every restart
  reads as an outage.
- A probe that did not RUN is not one that answered nothing, and an identity built on
  one is neither served nor cached. Empty roots are ordinary — several mechanisms
  legitimately have none — so the guard is `exitCode == NotSpawned`, never the count.
  A short include-tree WALK is worse still: it moves nothing the stamp covers, so the
  short digest validates forever. What counts as a gap is what two ends would DISAGREE
  about, so an absent root, a dangling symlink and an undecodable root name are none —
  and keying the first on `error_code` rather than on the resolved `file_type` refuses
  every machine without `/usr/local/include`.
- A reply's codec is chosen from what the OTHER end said it accepts, never from this
  end's list against itself — and a codec list is `AvailableCodecs()`, never a literal.
  A hard-coded `{ Identity }` on the node made every dispatched object *and* every
  preprocessed TU cross the network uncompressed while every object arrived intact and
  every counter read normally. What a test must separate is the two ends **disagreeing**;
  "it round-trips" passes under the bug.
- A stored value's text regions are canonicalized by **every** server on this wire,
  through the one `CanonicalStoredValue` beside `CompileValue` — a node serves it
  too since #229, and the copy it lacked left every value it stored carrying the
  producing checkout's absolute paths. A replayed region becomes the object's
  dependency record, so those never invalidate. A path with no `<SRCROOT>` sentinel is
  ordinary (92 of 93 in a trivial TU are toolchain headers), so retirement is a schema
  bump, never a sniff.
- A manifest naming the TU and no header revalidates forever: `IsToolchainHeader` calls
  every path outside both roots toolchain, so ANOTHER checkout's headers are dropped
  exactly as an SDK's are. `BuildManifest` refuses (`NoProjectDeps`) when deps were
  reported and none survived; `ValidateManifest` refuses an empty set rather than
  letting `all_of` pass vacuously.
- A WORKER follows `NotLeader` too, or the client half arrives at an empty fleet:
  `Gate()` refuses `Register` as well, so a heartbeat that only LOGGED the redirect kept
  announcing to the demoted node, expired out of the new leader's registry, and every
  lease answered `NoWorker` behind a green build. A leader is remembered only once a
  round was ACCEPTED there — an endpoint that merely named one is a lead, not a leader —
  and a remembered one that stops answering falls back to `--scheduler` in the SAME
  round. `NotLeader` must not clear the worker id; `UnknownLease` must.
- `NotLeader` is an instruction, not an answer about the fleet: a client follows it to
  the endpoint it names (`RedirectTarget`, which `ClusterAdminCli` asks too), and the
  RELEASE goes to whoever ISSUED the lease, never to the configured address. Judged by
  PARSING the message, never by testing it for empty — an empty one is replaced by the
  error table's default sentence, so "no leader known" and "the leader is at h:p" arrive
  the same shape. Splitting is not parsing: `SplitHostPort` takes the LAST colon, so
  "no leader: try again" splits into a host and a port of " try again", and a launcher
  DIALS what the admin CLI only printed — one predicate, `ParseDialEndpoint`, because
  `DialEndpoint` asks the same of the same string a moment later. Bounded, because two
  nodes with a stale `_knownLeader` name each other forever. That is the CLIENT half;
  the worker half is the bullet above, and neither works alone.
- A COMPILE reply is tied to its request or REFUSED (`Mismatched`), before the object
  envelope is opened — a crossed reply served is a wrong object under a correct key,
  which is silent, stored and shared. The digest is taken in `CompileJobRunner::Run`
  from what is about to be spawned, never folded in `WorkerProtocol` from the decoded
  request: at that layer both crossed requests are pristine, so such a digest agrees
  with whatever it is compared against and passes the ticket's own acceptance test
  while catching nothing. A field is covered exactly when the client knows it before
  sending AND the runner observes it at execution. The base name is derived ONCE, or
  the fleet refuses every honest compile instead. It has no counter and can have none
  — only the client can see it, and the client is a per-TU process with no sink — so
  the alarm is an UNCONDITIONAL stderr line plus a `--show-stats` reason, and the
  outcome stays a MISS. Input side only: #279's scratch claim is the output side and
  neither alone is sufficient.
- A cache is per node; the registry is keyed per `(fingerprint, endpoint)`. Summing
  a cache field across `LiveWorkers()` counts one machine once per toolchain.
- A `FETCH` outcome decides whether the daemon is worth a second command
  (`CacheIsServing`), never whether the invocation continues: an unreachable or
  refusing cache still dispatches, because the two live on different machines. The
  reason is still recorded, the MISS trace is skipped so it is not overwritten, and
  the `STORE` is skipped so a dead cache is asked no more often than before.
- A bounded wait MEASURES its ceiling: `waited += poll` counts the sleep it ASKED for,
  and a sleep costs what the host's timer granularity says — so a stated 5 s enforced
  15 s and 7.5 s at two sites, silently, with every test green. One `DrainWithin`
  (`Core/BoundedDrain.hpp`), whose `DrainBound` carries the ceiling and the cadence and
  whose blocking and clock are ONE injected seam — a test whose sleeps cost what they
  requested cannot tell the two implementations apart on any platform. Both copies cited
  the correct implementation in a comment and had reimplemented it; a comment naming what
  it duplicates vouches for the duplicate's bugs.
- An unbounded drain does not avoid an ending, it hands the choice to the supervisor,
  which answers `SIGKILL` with no diagnostic. `~WorkerServer` bounds it, says what it
  abandons and ends the process itself — returning would free members a running job is
  still inside. Killing a wedged compile's direct child would not even unblock it: the
  grandchildren hold the pipe write ends, and the drain blocks on the pipes (#239).
- A REGISTER endpoint is **not** verified against the caller — `DispatchWorkerEndpointMismatch`
  only counts it (#242). Comparing hosts refuses the documented setup (DNS names, a node
  dialling itself, NAT, VPN, multi-homing) and stops only a *third* host, since membership
  already let this one in. The fix is a credential, as discovery's `(node, endpoint)` MAC is.
- `CallerContext::peerId` is the kernel's peer host and IS trusted — membership is decided
  from it. It carries no port; a peer dials from an ephemeral one.
- A node's cache tier serves **this machine**, always: locality is a property of the
  VERB, never of the bind and never of a member list. `--fleet-member` names who may
  spend this machine's CPU; the tier is its entire build output, and a peer read every
  object it had ever compiled. `CacheResponder` therefore takes no membership oracle —
  its absence IS the fix. The question is ambient, so it arrives through
  `Platform/ILocalityOracle`: `IsLoopbackHost` first and lock-free, then an address set
  refreshed on an INTERVAL — a miss-triggered refresh is one 2 ms `GetAdaptersAddresses`
  per request that any stranger can bill this machine for. Folded with `SameHost`, or a
  `::`-bound surface refuses its own clients.
- Cluster membership is one ROUTE to admission, never the whole policy. `--fleet-member`
  admits *clients* — laptops, CI runners — which never join consensus, so what the
  cluster agrees is **added** and never substituted. Composed at the `IMembershipOracle`
  seam (`AnyOfMembership`), because the next route is a credential and not a host list.
  The admission-layer reading of *absence from `ClusterState` is not removal*.
- A compile is awaited onto a `ThreadPoolExecutor` sized to the slot cap, never served
  inline and never on a reactor — served inline, a 32-slot worker ran one at a time and
  the cap it advertises was unreachable while every client still got a correct object.
  On the merged `0xFC` surface that is TWO hops, because a frame arrives on a reactor:
  off to the pool, and **back before the reply is returned**, since `FrameEndpoint`
  writes it to a reactor socket. The hop back is invisible at every call site — correct
  object, successful write — so a test asserts the THREAD IDENTITIES, not the reply.
  That door spends `WorkerServer::Capacity()`, never a second `CompileCapacity`, so one
  drain covers both and the advertised slot figure describes both; the worker is
  declared before the surface so the listener stops admitting before the drain counts.
  It admits nobody the dedicated port refuses: one `RefuseUnlessMember`, the lease still
  checked inside `WorkerProtocol`, and `AuthRequired` **false** for `Op::Compile`
  because the lease is already a per-job credential.
- Detaching the compiles made the per-request payload cap a per-connection one; the
  in-flight byte budget lands in the same change, refusing with `EndpointBusy` because
  a slot was free and memory was not.
- That budget charges what a request **costs**, not what its frame is long: a codec
  envelope's declared expansion is the larger number, and a ceiling on it is per
  request while the budget is per surface. A price above the whole budget is left to
  the decoder, because `EndpointBusy` on an idle worker is a retry loop.
- Anything a worker derives per job is derived per THREAD: two compiles sharing a
  scratch number shared `tu.o`, and one answered with the other's object.
- And per PROCESS across machines: a scratch root is CLAIMED exclusively, never
  merely named uniquely. Claiming is the liveness check, so there is no race and a
  root whose lock is free is one whose owner is gone — `_Exit` included. `flock`,
  never `fcntl`, which is per process and would pass the two-runner test written to
  catch it. The lock file sits BESIDE the root, because emptying the root is what
  both reclamation and ordinary cleanup do. No unclaimed fallback: `TEMP` is the
  relocation mechanism and a refusal is named.
- A child inherits what the PROCESS has, not what the call set up. Windows names the
  handles it may inherit; POSIX marks both pipe ends close-on-exec, under the lock that
  covers the spawn.
- A drain waits on a condition variable, never `atomic::wait` — an atomic wait can
  return without the notify and free the object the notifier is still inside. And it
  calls `Shutdown()` first, or the accept loop admits one more job behind it.
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
- An unimplemented verb is refused `Wire::UnimplementedVerb`, never
  `DispatchNotPermitted`. The launcher steps over the first and proceeds
  unauthenticated; the second it treats as fatal, so a `FASTCACHE_TOKEN` client got a
  permanent 0% hit rate that presented as a cold cache, every `LEASE` declined behind
  a green build, and a credentialled worker that never joined. The choice is a
  `(op, code, why)` table row, not a `switch` special case — and the code is ONE named
  constant every surface and the client spell, because three surfaces naming the
  enumerator separately is exactly how they drifted (#283, #340). *Unimplemented* is
  not *served elsewhere*: a verb another port answers stays `DispatchNotPermitted`,
  because `UnknownOpcode` there tells a client this daemon is too OLD when it is in
  fact too new. A row for a verb the surface does serve is dead — `static_assert` it
  cannot be added.
- A wire constant has TWO facts, its name and its value, and a symbol both ends spell
  can only test the first. Pin the **byte** as well: change the alias consistently and
  every in-tree test still agrees while every deployed launcher breaks, because they
  tolerate `0x02` and nobody here can recompile them. Keep one test on the raw
  enumerator — it is the anchor, not the code smell it looks like.
- `Net/` must not depend on `Core/`. `Async/` travels with it, plus three named
  dependency-free leaf headers; `ctest -R net-boundary` enforces the table.
- `CompileCacheWire.hpp` must stay header-only and dependency-free — the launcher
  does not link `FastCache`. It therefore carries cache tiers **positionally**,
  which makes `StorageTier`'s enumerator order a wire contract.
- SIGPIPE is suppressed per socket, never process-wide: an ignored disposition is
  inherited across exec.
- So is keepalive, and for the mirror reason: `ApplyHotSocketOptions` is where every
  socket passes, so arming there would change when every idle client connection and
  every Raft link is dropped. It is a `DialOptions` field, and the flag without the
  intervals inherits a two-hour default that reads back as armed. A faster failure
  nobody can NAME is not an improvement — expiry and a lost peer are one broken socket,
  so the timer records which.
- A child inherits this process's sockets too, and neither platform stops it: a
  Windows handle arrives inheritable and `BlockingListener::Accept` is a plain
  `::accept()`. Armed once, in `ApplyHotSocketOptions`. A spawn names what it hands
  over (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`) rather than marking what it does not.
- A listening socket claims its address exclusively — `SO_EXCLUSIVEADDRUSE` on
  Windows, where `SO_REUSEADDR` lets a second process take a port already being
  served. Sharing a port is `ReusePort::Yes`, and only that.
- A struct a decoder returns **by value** must not borrow from the bytes it decoded:
  `Decode(Encode(x))` is the obvious spelling and is a use-after-free the moment one
  member becomes a view. A `*View` type borrows and says so; anything else owns. It
  has happened twice (`CapacityFields`, then `CompileResult`/`CodecEnvelope`). Which
  shape to pick is per type: OWN when the result outlives the buffer in practice
  (`CompileResultFields`, held across statements by `Dispatch`), stay a named `*View`
  when every consumer reads it in scope and something depends on not copying
  (`CodecEnvelopeView`, whose `Identity` path must not gain a second copy of a
  preprocessed TU). The encode side goes on borrowing its inputs either way. A
  regression test for this needs a payload of REAL SIZE: at four bytes the freed
  block reads back correctly and ASan reports nothing, so the sizes are load-bearing
  and must say so.
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
- Whether the operator **named** a setting is provenance, recorded by the parse in
  `OptionSpec::explicitBit` — never recovered by comparing the value to the default,
  which cannot see the operator who typed the default. Both sides of such a flag ask
  it: the startup decision AND what the service registration emits, which is
  `emitIfExplicit`, never `emitIfSet`. A flag whose default is empty needs no bit;
  there is nothing to arrive at without asking.
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
- A configuration FILE reaches the same fields through the SAME appliers, in that
  order, so "the command line wins" is which loop runs second — never a per-field
  merge with a per-field explicit bit and a per-field presence bit, which is the
  daemon's shape and has shipped a flag that parsed and never merged four times.
  Which key a row answers to is a COLUMN (`yamlKey`), because the mapping is not
  derivable: 34 keys for 44 daemon flags, diverging four ways. A key naming no row
  is REFUSED — a file is read at every start, so a key nothing reads is a setting an
  operator believes is in force forever. A row a file may not carry is on a named
  list with a per-row reason, and the compile-time guard READS that list.
- A flag whose meaning is its presence is a boolean in the file and `apply` runs on
  `true` alone — the key spells the FLAG, so `no_toolchain_discovery: false` passes
  nothing. A repeatable row APPENDS, so the command line EMPTIES the list first
  (driven off the `clear` column) or `--toolchain` extends the file's set instead of
  replacing it — and that reset walks argv through the parser's own `TakeValue`, or a
  flag's VALUE spelled like a list flag empties the list. A file that failed halfway
  is DECLINED, never half-applied.
- `--install-service` registers the command-line-only parse and carries the config
  PATH, never the file's values and never a resolved default — either pins the
  service to one reading of a file the operator then edits with no effect.
- A missing file is `FileNotFound`, not `ParseError`: `YAML::BadFile` derives from
  `YAML::Exception`, and the general catch sent a mistyped `--config` hunting for a
  syntax error in a file that is not there.
- The shipped reference configuration is checked against the table
  (`ctest -R node-config-reference`) — nothing else connects them, and that check
  fails when either scan matches nothing, because two empty lists agree perfectly.

**[`.agent/rules/storage.md`](.agent/rules/storage.md)** — the on-disk format and
converting a store. Before `Cache/CowTreeStorage`, `CowTree/`.
- An old store is `UnsupportedFormatVersion`, never `Corrupt` — the code is what
  monitoring sees, and `Corrupt` is what makes somebody delete a healthy cache.
- `Corrupt` means the BYTES ARE DAMAGED, and nothing a client sends may reach it. A set
  or a stream is a value blob tagged by a `flags` word the memcached verbs let a client
  choose, so a planted blob reported disk corruption against a healthy store — the rule
  above, reachable on demand rather than at a migration. `SetCodec`/`StreamCodec` return
  `MalformedValue` themselves, so no caller picks; it is not a persistence failure; and
  `CacheMalformedValues` keeps it visible, because removing a wrong signal without adding
  a right one is the other way to get this wrong.
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
- A refusal's wire code and its counter are one row — one fact, two audiences. And a
  refusal answered while NOTHING rises is a probed port that looks unused: `Refuse`
  takes the row, so there is no argument to pass a bare `ErrorCode` to, and every
  refusal on the surface goes through it — including the ones that already counted,
  which is what makes `worker-refusals-counted` exact rather than a proximity
  heuristic. The row is the REFUSAL, not the code: two share `MalformedFrame` and must
  not share a counter, so a table keyed on the code cannot hold them.
- A surface MERGING undoes that without anybody writing a bug. `FrameEndpoint` encoded
  five refusals itself, two of which had counted on the compile port #290 retired, so
  #326's counter went flat at a *migration* while the docs still named it and nothing
  failed (#447). The scan covered three files and not the listener. It is EXACT, so a
  file with one uncovered site cannot be covered at all — all of them or the door stays
  open, and a partial fix leaves the instrument blind to the file that allowed the
  omission. The endpoint owns WHEN, the surface owns WHAT including the counter
  (`RefusalReply` / `EndpointRefusalReply`); a refusal decided before a header exists
  names no verb, so it is the ENDPOINT's own row rather than a default arm on the
  router — which is also what keeps the two `EndpointBusy` refusals from ever summing.
  And `SchedulerRequestsRefusedUnauthenticated` fires only pre-payload, so a WRONG
  token counted nothing: three outcomes, three rows. But not every refusal is an EVENT
  — a verb this node runs no component for is what a HEALTHY build gets, once per
  exchange, so counting it buries the scan it would be read for.
- That scan is a GLOB over `src/`, never a file list. It was a hand-kept list of `.cpp`
  files, grown by hand twice and unable to reach a header at all — which is where #447
  then put two security counters. **A list is exact about the files it knows and silent
  about the ones it does not, and silence reads identically to complete coverage**
  (#492). An over-broad scan fails CLOSED, so the only rows left are the two ends of one
  function: the header defining the encoder and `Protocol/SurfaceRefusal.hpp`, the
  primitive's shared home — out of `fastcache-cc`'s private header so coverage is a
  property of the TYPE. Header-only, so `_fc_cc_core` gains no row.
- And "deliberately uncounted" must not be spelled like "forgot": both were a bare
  `EncodeErrorReply`, so no scan could tell a considered decision from a defect --
  and two of #490's five written rationales turned out to be neither, asserting
  positions #491 exists to settle, one of them the opposite of what #491 argues. THREE spellings, three claims — `Refuse` (a rise means something),
  `RefuseWithoutCounter` (a rise would mean nothing, and why), `RefuseUntriaged`
  (nobody has decided, and which issue will). The third is safe only because the check
  TALLIES it and prints the total per issue on every run: a placeholder reason would
  spell *forgot* in the vocabulary of *decided*, which is worse than the bare encoder.
  The reason is a forcing function, not a dead field — and it is `rationale`, never
  `why`, which on `RefusedVerb` is text a CLIENT is SENT; the two meet in one
  expression and one word cannot carry both contracts.
- The SET of spellings is derived from that header, never restated in the check. A
  restated list catches one going away and is blind to one ARRIVING — add a fourth and
  every call site reaching it passes the scan, joins no backlog and asserts nothing,
  which is #492's own defect one level up. `worker-refusals-selftest` drives seven
  synthetic trees, including that one, because a guard nobody has watched refuse is
  not a guard.
- The scan filters whole-file before splitting: 403 of 413 files contain none of the
  three substrings, and scanning them all cost a default-set entry 2.9 s on every
  platform to find matches in ten files. 208 ms after. Exact rather than approximate —
  each needle is a strict prefix of the regex that would have matched it.
- Text a peer sent is text, or the fleet refuses it: one byte that is not UTF-8
  makes `/fleet.json` unparseable for the **whole** fleet. Refused where it enters
  (`SchedulerService::Register`) and never repaired by a renderer — and the
  encoders are total anyway, because a consensus entry is applied after it is
  committed, with nobody left to refuse it. Markup's rule is XML's `Char`
  production over **code points**, not bytes: `U+FFFF` is valid UTF-8 and
  illegal in an SVG.
- **Skipped, absent, unstarted and failed are FOUR states**, and tooling collapses them — five times in four
  instruments in one session, none of them a coding mistake, all of them a representation that could not tell
  two things apart. A count cannot carry this and neither can a `bool`: "25 of 26 green" is arithmetic that is
  true and useless. **Absence of the negative is not the positive** — "no pending checks" is not "all checks
  reported", "no failures found" is not "the tool ran" — so a check concluding from a count of BAD things needs
  a separate assertion that the good things exist. Where the answer cannot be determined, report that as its own
  outcome rather than the nearest neighbour.
- Absent is not zero: a process with no cache reports no cache, and *names* the
  field to do it.
- Its converse: an absence must not be counted as an event. `NoUpstream`'s honest
  `false` was read as a failed store, so a machine with no shared cache reported a
  100% upstream failure rate. An outcome that can be *not attempted* is an enum, not
  a `bool` — and it is fixed at the seam, never at the one call site that noticed.
- A counter is a tally, so zero is the truth about events that never happened;
  absence is modelled in the **snapshot**, never by dropping a counter row.
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
- Nothing a receiver can **recompute** travels: a handed-over bucket carries two
  instants and the readings, and the leader rebuilds the fold and the coverage by
  replaying them. History is filed under the **machine**, never the worker id.
- A handover cursor advances only on the verb that carried the batch — `accepted`
  also counts a registration, which carries no history at all.
- A node's version is compiled in, rides REGISTER's *nested* capacity record (whose
  arity is variable) rather than its top level (whose arity is exact), and is
  refreshed on re-registration — a restart is what an upgrade looks like.
- A history stores a counter **raw**; a rate is the delta at render, taken only
  between adjacent *present* buckets. A restart is then a gap, not a spike.
- A node records **itself** always; only the fleet-wide slots are leader-only, and
  which is which is `FleetMetricTable`'s `scope` column. Every node samples whatever
  surfaces it serves — a sampler owned by the admin surface left a pure worker, the
  machine doing the compiles, recording nothing.
- A **backfilled** window answers for a machine, never for the scheduler: its
  fleet-scoped zeroes are not readings, and drawn as such they are a rate running
  backwards and then a spike, neither of which happened.
- The routes reach a history through **one** door (`IFleetHistoryView`), or the
  backfill is filled, persisted, restored and never drawn. Assert the wiring.
- No state of a history file may keep a node from starting — and a file a **later**
  build wrote is kept and never written over, which is a property of the shared
  envelope rather than of each store that remembers to copy it.
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
- Run `scripts/local-gate.sh` before pushing. One configuration is not the gate — and a
  RED run is silent about every leg after the failure, so "GATE FAILED: clang-debug tests"
  does not mean the rest passed (#501). Two `-Werror` defects in one change hid behind
  five red runs that never reached `gcc-release`. A GREEN run is silent in the OTHER
  direction: four defects in four tickets were reachable only by the analyser or a
  sanitizer, so a fully green MSVC run of ~2997 tests could not have reported any of
  them. A platform's leg answers a different question, not a weaker version of the same one.
- **A retry makes an instrument's own failures disappear without fixing them.** Ten ways
  the gate reported on something other than the tree under test have turned up across four
  tickets — `| tail` reporting the pipe's status, quoting collapsing through three parsers
  so the run never happened, two gates in one build directory, a dirty tree, the log on
  `/tmp` where a WSL idle-out erases it, the wrapper edited WHILE bash was executing it, a
  `/mnt` path mangled by Git Bash so the launcher exited **0** having run nothing, and a
  DrvFs log redirect that failed while leaving the gate child ALIVE, and the gate's own
  `clang-format -i` REWRITING the commit and then measuring what it had rewritten, and a
  `ctest` total that counts only the TARGET SET it was configured with — `-DFASTCACHED_BUILD_TESTCLIENT`
  and `-DFASTCACHED_BUILD_BENCHMARKS` default OFF, so one commit on one platform gives two
  different totals and the tree just looks smaller. A total is comparable only against the
  same tree, the same platform AND the same target set. None
  announces itself; each looks like a flake; a re-run clears all ten. A dirty-tree guard
  that samples once AT THE START cannot see an instrument that dirties the tree itself —
  sample at both ends, and note that implementing half of a two-clause rule looks exactly
  like compliance. Presence is not usability, and a
  finding fixed at the line rather than at the rule comes back.
- **A claim about a tool is checked against the tool.** A pattern is broader than its author
  reads it as (`pgrep -f "scripts/local.gate"` is a REGEX; the `.` matches the `-`), a
  process is attributed by its ancestor chain and never by a cmdline match or a leaf `cwd`,
  and a bound nobody has watched fire is untested rather than proven. The general form: "a
  SIGTERM does not take 55 seconds to arrive" is true of `local-gate.sh` only because it
  traps `EXIT` and not `TERM` — bash defers a TRAPPED signal, so the general claim and the
  specific one point opposite ways and nothing warns you which you hold. And **the tree
  you measured is not necessarily the tree in question** — a grep in a checkout parked on
  a merged branch produced a CONFIDENT CORRECTION, which propagates where a missing log
  merely stalls. The tell is that the answer was too convenient.
- A hygiene script `ctest` runs is constrained to **bash 3.2** — macOS ships a 2007 `/bin/bash`, and a default-set
  script runs on every platform CI builds. No `mapfile`/`readarray`, `declare -A`, `${var^^}`, `local -n`; keep the
  process substitution when replacing `mapfile`, or the `pipefail` trap comes back. The constraint was already in
  `coverage.sh`'s comments, where nobody looking at a new script would find it.
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
- A script that NAMES a tool version must name it **everywhere that version matters**.
  `local-gate.sh` pinned the formatter and handed the analyser to `PATH`, with the
  paragraph explaining why that is wrong in its own header four lines above — an
  argument carried one call short. And a cached `find_program` result outlives every
  reason it was chosen: the gate configured only when `CMakeCache.txt` was ABSENT, so a
  tree kept the first clang-tidy it ever found and re-running the gate could not repair
  it. Check the pin against the cache, not only pass it. `ctest -R local-gate-selftest`.
- A **reference build passes `-DUSE_COMPILER_CACHE=OFF`, and the gate is a reference
  build** — it did not, and both its configurations were fronted by whatever launcher was
  installed (148 and 618 `LAUNCHER = ` edges, measured). The rule was already standing in
  `launcher-replay-e2e.sh`, which names it and cites #319; *stating a rule in the file that
  obeys it is how the file that does not obey it never learns about it*. Pinning the other
  two tools argues for REMOVING this one, not versioning it: their version changes the
  verdict and has a canonical value, a cache is supposed to be verdict-neutral, and
  requiring "the launcher built from this tree" is unsound twice — `-dirty` is not an
  identity, and it routes the gate's objects through the change being gated. Passing the
  flag is not the fact: `CompileCache.cmake` leaves an externally-set launcher untouched,
  so the refusal reads `build.ninja`, never `CMakeCache.txt`, which never holds it.
- A `cmake -P` check **cannot fail its own test**: `message(FATAL_ERROR)` prints
  `CMake Error` and exits **0** on 3.28, this project's declared minimum. Thirteen
  hygiene checks reported PASSED whatever they found. The verdict is read from the
  output (`FAIL_REGULAR_EXPRESSION`), one spelling defined once — and a failure
  signal that is a *property* needs both `script-check-canary` (a script that must
  be seen to fail, `WILL_FAIL`) and `script-check-signals` (no registration omits
  it). The fact was already written down for the SKIP direction and never carried
  to the FAIL one.
- Never silence clang-tidy with `NOLINT` — fix the source.
- A return type is not part of a function's mangled name on Linux, so two
  functions differing only in return type silently collide.
- `cmake/portable/CompileCache.cmake` stays stock-CMake-only and must never fail
  a configure. `check_<lang>_compiler_flag` is a hard error for a language the project
  has not ENABLED, and a bad flag in `CMAKE_<LANG>_FLAGS` fails the ABI check — so ask
  `ENABLED_LANGUAGES` first, and CHECK a flag rather than gating on a compiler-ID
  string. What it computes and appends is a function, checked as a computation
  (`ctest -R debug-prefix-map-rules`): the layouts that break such a rule are the ones
  nobody has locally.
- A sanitizer that is on in the cache is not one that is on in the build — a tool
  that silently does nothing is worse than one that is visibly off.
- A Windows **Debug** leg is run for `_ITERATOR_DEBUG_LEVEL=2`, not for the compiler,
  so it runs `ctest` rather than only building. Nothing states that level — it follows
  from `_DEBUG`, from the runtime library, from `CMAKE_BUILD_TYPE` — so
  `iterator-debug-canary` is a program that must die and
  `scripts/iterator-debug-gate.ps1` refuses a build where it survives. It is guarded
  to MSVC Debug, so its absence on other platforms is normal rather than a lost
  registration.
- So a sanitizer job proves nothing until something proves the sanitizer.
  `scripts/tsan-gate.sh` refuses to report clean until every artefact's OWN OBJECT
  FILES carry an **undefined** `__tsan_init` reference **and** a deliberate race
  (`src/tests/TsanCanary.cpp`, built by the same `add_compile_options`) has gone red
  — run **with** `.tsan-suppressions` active, so no pattern broad enough to swallow
  an obvious race can disarm it. Do not repair that file. A known race lives in
  `.tsan-suppressions` with its issue number; deleting the entry is part of closing
  the issue, never part of going green.
- That proof is asked of the OBJECTS because it cannot be answered by a binary:
  `__tsan_init` is DEFINED by the sanitizer runtime, which the link pulls in whole,
  so a canary whose TU was compiled with no sanitizer flag at all still produced a
  binary carrying it and PASSED (#472). An object cannot borrow the symbol, so its
  undefined reference is the one that means something. The canary was never the
  exposed half — an uninstrumented one fails closed — the SUITES were, since
  `RunTarget` checks the timeout, the tag filter and the assertion count and none of
  those tells an instrumented run from an uninstrumented one. Not
  `__tsan_func_entry`: that is per-FUNCTION, so an object whose TU has no functions
  carries none, and six of `FastCacheTest`'s 135 are exactly that. `ctest -R
  tsan-gate-selftest` drives every verdict against staged object trees.
- The canary's job is to be caught EVERY time, so a change to it is judged by a RATE
  and never by a green run: `scripts/tsan-canary-rate.sh`, a few hundred runs, the
  number recorded. It used to be silent in a few runs per thousand — the race
  happened and nothing was reported — which is worse than it sounds, because a gate
  that is red a few percent of the time teaches people to re-run it and is then
  disarmed as thoroughly as if it had been deleted (#473). Pinning to two CPUs made
  the old shape WORSE (0.700% against 0.220%), which is the wrong direction for a
  two-core runner. It now races across an array rather than one `int`, because
  distinct LOCATIONS are what the measurements move on — and the file claims no
  mechanism, since the obvious one (shadow-cell eviction) predicts that more
  accesses are worse and the data says the opposite.
- An edit script asserts its anchor **matched** — `assert count == 1`, count rather
  than presence, so "missing" and "not unique" both fire — and a generator that
  produced nothing fails instead of printing success. Three separate tools reported
  success for work they did not do in one night: an edit that matched no anchor, a
  test fixture whose writer wrote nothing, and a YAML `if:` silently discarded as a
  duplicate key. Report what changed, not that the script finished.
- `producer | grep -q` is a false **negative** under `set -o pipefail`, and it
  fails on the SUCCESS path: `grep -q` exits at the first match, the producer dies
  of SIGPIPE, and `pipefail` reports the producer's status. `nm "$b" | grep -q
  __tsan_init` therefore says "absent" precisely *because* the symbol is there.
  Capture into a variable and match afterwards.
- The TSan scope is one Catch2 tag expression, in `tsan-gate.sh`'s `TARGETS`
  table. `scripts/check-tsan-scope.cmake` **reads** it from there rather than
  restating it — a second copy is not a cross-check, it is a second thing to be
  wrong — and `ctest -R tsan-scope-hygiene`, in the **default** set, fails when a
  test file in `Async`, `Consensus` or `Distributed` carries no tag that
  expression selects.
- A `paths-ignore` filter on a workflow whose checks are **required** makes a pull
  request unmergeable, not fast: the workflow never triggers, so no check run is
  created and the required context never reports. Master is guarded by a *ruleset*,
  so `/branches/master/protection` answers `404` and tells you nothing. Gate at the
  **job** level instead — a skipped job still reports, and `scripts/ci-scope.sh`
  (tested by `ctest -R ci-scope`) is what decides, escalating every way of not
  knowing to "build everything".
- A **merge queue** is the third door to that same never-arrives failure: it dispatches `merge_group`, and a workflow
  not listening for it produces no check run, so a queued PR *sits there*. `pr-labels.yml` is the sharp case —
  `pull_request_target` does not fire on `merge_group` at all, so its gate needs a queue leg that STATES what it
  checked, or it is a stub that reads like a working gate. Check the concurrency key too (a PR-number key collapses to
  a constant and each entry cancels the last), state `merge_group` in the scope classifier rather than falling through,
  and add no JOB to `build.yml` — `check-release-gate` would drag the release behind it.
  `ctest -R merge-queue-contexts` asserts all eleven required contexts can report.
- A skipped job REPORTS, and a skipped REQUIRED context reads as PASSING — measured: three required contexts came
  back `skipped` on `b4777aa`, which merged. A skipped **matrix** job is the opposite: it never expands, so its
  per-leg contexts never exist and nothing reports at all. One passes, one hangs; the difference is the matrix.
  So never let a dependency's failure skip a required gate — the skip reads green. `if: ${{ !cancelled() }}`, and
  check for real. Not `always()`, which runs even while the run is being cancelled.
- A workflow must not invert its own script's principle one level up: `ci-scope.sh` escalates every way of not
  knowing to build-everything, and the workflow read it as `== 'true'` — so a FAILED `changes` published no
  output, sixteen jobs skipped, and the skips read green. `!= 'false'` everywhere, plus `!cancelled()` on every
  job that consults it. The matrix trap had been the only thing saving this (a skipped matrix job hangs rather
  than passing); it is no longer load-bearing, so do not reintroduce a job-level `if:` on `linux`/`windows`
  believing it will catch you. `ctest -R gated-jobs-fail-safe` asserts both rules, derived not tabulated.
- `clang-format -i` at any version but the pinned one silently reformats code the
  pinned one already accepted; run an older binary as `--dry-run` only. Both pinned
  tools ship on PyPI (`pip download clang-format==<v>` / `clang-tidy==<v>`), so "the
  distro only has an older one" is not a reason to use it. An older clang-tidy is
  worse than a laxer one: it is *silent* about checks that do not exist in it yet.
- A clang-tidy sweep that cannot prove the tool ran is worth nothing and reads like
  success — `scripts/tidy-sweep.sh` canaries it first and treats a failure to
  execute as fatal, never as "no findings".
- A ccache hit does NOT skip clang-tidy: the launcher and the analyser are two
  independent commands under `cmake -E __run_co_compile`. On a pull request CI
  therefore tidies the diff plus every translation unit that includes a changed
  header, and anything that changes how EVERY unit is read gets a row in
  `SweepEverythingWhen` — a missing row is a sweep that checks the wrong set and
  prints a confident count.
- A compile database generated for clang-tidy needs `CMAKE_CXX_SCAN_FOR_MODULES=OFF`
  named explicitly. `CompileCache.cmake` sets it only when it picks a launcher, and
  without it every unit fails to parse and the sweep reports clean.
- And it must be configured with the same TARGET SET CI builds. A sweep whose scope
  comes from a database is only as complete as that database's targets, and a target
  gated off by default is invisible to it rather than absent from CI — 15 units where
  CI tidies 20, every one of the five missing ones inside the change. The script
  cannot catch it: a changed file with no compile command is dropped silently, and
  must be, since that is also what a platform-specific TU looks like. Account for
  every file in the diff the sweep did not reach, before trusting its count.
- **Running the launcher is not testing it.** The synthetic fixtures prove it RUNS and produces AN object; only
  building a REAL target through it and running that target's tests catches a WRONG one (#319, #320).
  `scripts/launcher-replay-e2e.sh` builds three times — cache-off control, cold (stores), warm (REPLAYS) — and
  runs the replayed binary. Compare cold-against-warm, never control-against-warm: a launcher-active configure
  disables PCH and module scanning, so those objects legitimately differ. Guards: the cold build must be seen
  USING the launcher, the warm one must be seen HITTING — a warm build that missed replayed nothing and passes
  everything. And staging a wrong object needs the LINK command (`ninja -t commands`), never `touch`: ninja
  records output mtimes, so a replaced object reads as dirty and is rebuilt, undoing the injection silently.
- A compiler cache that reads like success is worse than none: the Windows sccache
  was running into a directory the runner deletes, so the jobs are asserted to be
  backed by the Actions cache — before `ctest`, which restarts the sccache server
  and zeroes its counters.
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
- Every wait is bounded and says what it waited for — and, when it times out, which KIND of failure it was.
  A slow machine and a wedged process are fixed in different places, so a wait records what tells them apart: the cost on success, whether the process is still
  alive, whether the log grew, and how much CPU it burned. The last one is not optional — an include-tree walk logs nothing while it runs, so log growth alone
  diagnoses that case confidently and wrongly. Where the signals disagree, say INCONCLUSIVE.
- A **cumulative** figure cannot answer a question about **now**, and a duty cycle over the same window is the same
  number divided by the same 300: 3.4s spread over five minutes and 3.4s burned in the first ten before a wedge are
  opposite diagnoses. Draw the verdict from a RECENT window and print the totals as evidence only. No magnitude bar
  calibrates — the same include walk runs at 88% duty warm and a fraction of that cold — but **zero does not vary**, so
  test for presence, measure the process TREE (a spawned `cl` charges its own CPU), and report the band between idle
  and clearly-working as neither.
- An `-or` is right for two independent CONFIRMATIONS and wrong for two competing READINGS: `logGrew` was False and
  `busy` was True, and the disjunction let the weaker win unopposed. A signal that cannot be false in the failing case
  is not evidence. And an instrument that prints a **remedy** cannot know when the remedy is under dispute — "raise the
  budget" is what #354 refuses. State the finding and stop.
- A classifier that cannot be made to say BLOCKED cannot report a hang. `ctest -R node-scratch-isolation-e2e-selftest`
  drives each verdict against a synthesised **readings record**, in the default set.
- A stand-in built to exhibit a MAGNITUDE must not be measured through an instrument whose own overhead is comparable
  to it: the verdict band is 0.35s wide and a PowerShell process's startup costs 0.2–0.5s, so a stand-in that burned
  exactly 250ms still read 0.52s on CI and 0.16s when moved. No arrangement fixes that — the noise IS the interpreter.
  Split the DECISION out as a pure function over a record; leave acquisition alone. Branches that could not be staged
  become one line, every bound gets pinned on BOTH sides, and 53s + RUN_SERIAL becomes 0.3s.
- A fixture waits on what a line MEANS. `node-scratch-isolation-e2e` serialised its three include-tree walks by waiting for
  `compile node ready`, which meant *surveyed* until #365 made it mean *bound* — same wording, different fact, and the walks
  then ran concurrently at 2–5 file/s against ~30 single. The budget was the symptom; raising it would have bought a fixture
  three times slower with the cause buried. Wait on the STAGE, keep bind and survey separate so a stall says which, and note
  that the paragraph explaining the serialisation was correct and three lines above the wait that had stopped implementing it.
- **A fixture that has never completed has told you nothing**, however carefully it was read.
  `launcher-replay-e2e` was reviewed and merged into a CI job, and its first run anywhere died on the
  first line that starts a process, behind which sat three more defects — one of them #390, a
  repository bug the fixture merely reached first. A flag spelling is checked against `CliOptions()`,
  never remembered — `--memory-limit` does not exist, and the daemon answered usage while the fixture
  reported "never accepted a connection". `exit` inside a `( ... )` ends the subshell only, so a
  `fail` helper signals the top-level shell and tests `BASHPID`, since bash keeps `$$` at the parent's
  value there and the guard would silently never fire.
- **A guard written to prove a fixture bites can itself fail to bite.** The replay canary compiled a
  wrong object over a unit of ANOTHER target, the binary under test never linked it, and the fixture
  announced "the suite PASSED with a wrong object linked in" — a true observation carrying a false
  claim, which names the subject as broken when the instrument is. An injection is ASSERTED, in the
  artefact and again in the thing that consumes it.
- **Attribute by asking the process, never by adjacency in interleaved output.** Reading the
  `[n/m] Building CXX object ...` line above each launcher outcome split one unit differently across
  two runs of the same build (74/41 against 75/40) — a guard that fails a build for a scheduling
  accident. A wrapper that labels the unit on the same stream, from the same process, inside the same
  edge cannot be separated from what it labels; an unlabelled outcome is counted as NEITHER and
  refused by name. And an unattributed total hides the only column that means anything: 106 of 221
  units missing was alarming and was entirely third-party, while one project unit missing is the
  actual regression shape and is invisible inside the same number.
- A `$<TARGET_FILE:x>` naming a target that was NOT built is a hard error at **generate** time, not a skipped
  test, so the whole configure fails and the message names CMake rather than the option the operator set. Guard
  the block on `TARGET x` as well as on whatever feature makes the test interesting — the two are independent.
  `-DFASTCACHED_BUILD_DAEMON=OFF` could not be configured at all (#390), and only on machines that HAVE sccache,
  since the sccache rows are gated on finding the binary. The rule was already written out four lines above the
  block that obeys it in `fastcache-cc/CMakeLists.txt` while two blocks one directory away did not, which is a
  rule that needs a check rather than a better comment: `ctest -R target-file-guards`, in the default set,
  reading the optional targets from `src/apps/CMakeLists.txt` rather than restating them.
- A fixture whose client is always LOCAL cannot test who is admitted. `Classify` returns
  `Member` for the whole of `127.0.0.0/8` before it reads the member list, so twelve
  loopback end-to-end cases passed a worker that admitted only its own machine and refused
  every dispatched compile (#235). A second loopback address does not reach that list —
  the check is `127.`, not `127.0.0.1` — and changing to one looks exactly like a fix. The
  host's OWN non-loopback address does, with no second machine. Assert BOTH directions:
  listed and dispatched, unlisted and refused with the refusal COUNTER moving, the
  scheduler admitting the client in both so the refusal is the worker's. The fixture's own
  liveness probe is a caller too, so the refusing leg's baseline is one rather than zero
  and the compile is a delta from it. A host with no such address reports SKIPPED, loudly
  and as its own ctest test — a quiet fall back to loopback is a pass for a case that
  never ran, which is the defect itself.
- A script-driven test naming more than one executable is registered in
  `src/tests`, not beside a binary.
- Tests allocate their ports per run rather than fixing them — from **below** the
  kernel's ephemeral range, and remembered, because a connect probe cannot see a
  port already held as an outbound connection's local endpoint.
- `Unwrap(x)` after `REQUIRE(x.has_value())` for `std::optional`; a bare `*x` is a
  build failure.
- A Catch2 case name may not begin with `-`. CTest passes it as an argument, so
  `--help ...` printed usage and reported a pass for a case that never ran.
- A fixture states which PATH it exercised. `check-catch-skip-return-code` derives its
  file set from `git ls-files` and falls back to a directory walk; its selftest had six
  green cases and CI still failed, with no contradiction between them — a synthetic
  tree is not a git repository, so every case exercised the FALLBACK while CI exercised
  GIT. **The mode under test was not the mode in use**, which is a guard passing
  because it is testing something else. So the mode is part of the OUTPUT and asserted
  on both sides, or the cheap-to-construct path silently becomes the only one tested.
- A Catch2 `SKIP(...)` exits **4**, so every `catch_discover_tests` registration
  carries `PROPERTIES SKIP_RETURN_CODE 4` — without it ctest scores a skip as a
  FAILURE, and the binary prints `1 skipped` while ctest prints `***Failed` for the
  same run (#499). A false RED: all seven skip sites are environment-conditional, so
  they fire on a constrained runner and report a regression that is not there, and
  whoever meets it deletes the SKIP rather than suspecting the registration. The value
  is part of the rule — `77` is the script-driven convention and does nothing here.
  The mechanism was already applied eleven times one directory away, so the guard is a
  check (`catch-skip-return-code`, shown failing by `catch-skip-selftest`), because a
  sixth test binary reopens it by omission. And the list is written at BUILD time, so
  a reconfigure alone leaves a stale one that reads exactly like a current one.
- A scratch directory comes from `src/tests/ScratchPath.hpp`. A per-process
  counter is not unique — `catch_discover_tests` gives every case its own
  process, and the suite runs in parallel.
- A test FAKE is a shared helper too: `src/tests/ScriptedSocket.hpp`. Three private
  copies of one scripted `ISocket` carried the same `WriteVectored` defect in two of
  them, found a day apart — a fake nothing exercises does not report its own bugs.
- The POSIX shell fixtures share `scripts/lib/e2e-common.sh`, tested by
  `ctest -R e2e-helpers-selftest`. It was seven copies that had already diverged three
  ways, one of them a `wait_for_port` with no liveness check at all. A wait's bound is
  read from a **clock** and its timeout reports the **measured** elapsed: `100 x 0.2s`
  is not 20s, and `timed out after $((WAIT_TICKS / 10))s` was a duration nobody ever
  observed — the one reading that says whether the machine was slow, derived from
  assuming it was fast. A bespoke condition is a predicate passed to `wait_until`,
  never a new loop. And `fail` signals the top-level shell unconditionally rather than
  testing `BASHPID`, which is bash 4.0+ and silently inert on macOS's 3.2.
- A fleet property that spans two machines needs `src/tests/FleetHarness.hpp`, whose
  `OnCompile` places the interleaving rather than waiting for one. It is in
  `src/tests/` and not beside `RaftClusterHarness`, because a fleet spans the library
  AND the apps, and `src/FastCache/` must not include an app header. A harness earns
  its place only by a property shown RED when its rule is removed — and prove it with
  a case that stays GREEN under the same break, or the suite is measuring nothing.

## Issues and pull requests

Labels here have teeth: a pull request carrying no `type/` label **fails a
check**, because that label decides which section of the generated release notes
the change lands in (`.github/release.yml`), and nothing downstream can recover
it afterwards. CI derives `area/` and `os/` from the changed paths and reads
`type/` from a conventional-commit title when there is one — a prose title, which
most of this repository's are, means setting the label by hand.
[`CONTRIBUTING.md`](CONTRIBUTING.md) carries the taxonomy and the reasoning.

A label applied by hand can be **destroyed by CI seconds later**, and the gate
then fails for a pull request that was labelled correctly. `actions/labeler`
finishes with `setLabels(...)` — a full replacement computed from the labels it
read when its run started — *whatever* `sync-labels` is set to, so a label added
between that read and that write is lost. `pr-labels.yml` brackets the action with
a remember/restore pair and warns when it fires; if a `type/` label vanishes,
re-apply it and read that warning rather than assuming the gate is flaky (#347).

Deferred work is a GitHub issue linked from the matching rulebook file's
`## Open work` section, never a residual recorded only in prose.

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
  refused until the next refresh — it fails **closed** and self-heals; an address
  removed is accepted a little longer, and exploiting that needs DHCP to reassign it
  inside the window.
- **Staleness that produces a wrong answer which looks right is NOT cacheable**,
  however expensive the probe. `DiscoverTargetTriple` costs ~40 ms per translation
  unit and is deliberately *not* memoized, because the triple goes into `compilerId`:
  a stale one is **a wrong hit, not a miss** — an object built by a different code
  generator, served under a key claiming otherwise
  ([#188](https://github.com/LASTRADA-Software/fastcached/issues/188)). Expense is
  not the criterion; what a stale answer *does* is.

**Then, if it is safe:**

- **A performance figure is a quantity UNDER CONDITIONS, and the two halves get lost separately.**
  `ProbeToolchainFiles` recorded "about 2 s warm" — honestly measured, condition attached — and two other
  sites then cited it as "the 2-second full walk" and "about 2 seconds over 288 MB". Both dropped the `warm`,
  and the design was reasoned from the citations, so an operation observed exceeding **300 s** cold was
  treated as costing two seconds. Attaching conditions is necessary and **not sufficient: the citation is
  where they get lost**, so a figure others will refer to lives in ONE place they point at, never restated.
  And a figure can be current, correctly measured and still **the wrong quantity** — quoting the warm cost as
  the price of MISSING a cache is circular, since the cache is what makes the warm case warm. Record a table
  of conditions, not a number; a spread states its own uncertainty and forces a citer to pick a row.
- **Measure before choosing, on every platform.** `GetAdaptersAddresses` costs
  ~2.09 ms on Windows against ~0.0088 ms for `getifaddrs` on Linux — **238×** apart.
  A design that looks free on the platform you develop on can be the dominant cost
  on the one you ship to. "It is only a syscall" is how a hot path gets slow.
- **Prefer being fast by construction to being fast by cache.** The cache gate
  answers `IsLoopbackHost(peer)` first and never consults the seam for
  essentially all real traffic. The cache then bounds only the rare path, which is
  a far weaker thing to have to get right.
- **Refresh on an interval, never on a miss.** A miss-triggered refresh hands a
  remote peer a free amplifier: it can force the expensive probe once per request
  simply by asking. Interval-guarded, an attacker gets one per interval regardless.
- **Name both failure directions in the header** — what a too-old answer costs in
  each direction — because that asymmetry is what makes a longer interval
  defensible.
- **Reach it through an injected seam with an injected clock**, like every other
  ambient dependency. A cache with a hidden clock is untestable by construction.

**And prefer not needing the cache.** Computing a value once and returning what you
already have beats caching it: `CachedToolchainFingerprint` derived the resolved
path, the include roots and the stamp, returned none of them, and its caller
re-derived all three at an extra driver spawn per toolchain per survey — on warm
starts too, because a cache HIT said nothing about the roots it covered. That was
not a caching problem and a cache would have been the wrong fix for it; the fix was
`ToolchainIdentity` handing back the `ToolchainEvidence` it had already folded
([#259](https://github.com/LASTRADA-Software/fastcached/issues/259)). A wider return
value is also the STRONGER answer, not merely the cheaper one: two derivations a few
milliseconds apart can disagree, and the caller was recording evidence a compiler
upgrade in between would have made describe a different include tree than the digest
covers. **Evidence a caller may not have is a disengaged `optional`, never an empty
field** — empty roots and an empty stamp are both ordinary answers, so neither can
carry "there was no probe", and that is the guard from
[`.agent/rules/distributed-compilation.md`](.agent/rules/distributed-compilation.md)
moved out of the call site and into the type.

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
- **`clang-format` and `clang-tidy` after every change — at the version CI pins**
  (`$CLANG_TOOLS_VERSION` in `.github/workflows/build.yml`). Successive LLVM
  releases disagree with each other, so a tree clean under whichever binary is on
  `PATH` can still be rejected. Name the version explicitly and use a build
  directory of its own; the `clang-debug` preset is **not** that sweep.
- **`clang-tidy` reports must be fixed at the source.** Never silence with `NOLINT` — address the underlying issue. The `clang-debug` preset enables `clang-tidy` automatically at whatever version `PATH` resolves to — which is why **`scripts/local-gate.sh` passes it `-DCLANG_TIDY_EXE=clang-tidy-$V` and refuses to start when that binary is missing**, rather than letting the preset pick. Running the preset by hand still takes whatever `PATH` offers; see the bullet above for why that is not the same as the one CI enforces.
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
