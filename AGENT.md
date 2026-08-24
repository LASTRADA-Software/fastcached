# fastcached - Fast Cache Daemon

## Project Architecture

A layered C++23 server. Each layer reaches its collaborators through a
narrow interface so the whole thing is testable end-to-end against an
in-memory transport.

```
src/FastCache/
  Core/         Errors taxonomy, Clock, HostPort (one parser for
                `port` / `host:port` / `[v6]:port`), IRandomSource (the randomness seam,
                beside Clock and for the same reason), Logger, BufferPool,
                Bytes, Endian, Crc32c, MurmurHash3 (128-bit key digest),
                StringHash, Owner, Profiling (Tracy wrappers)
  Async/        Task<T>, Cancellation, ResumeOn, IReactor + TestReactor and the
                platform reactors (EpollReactor / IocpReactor / KqueueReactor)
  Net/          ISocket, IListener, IConnector (the outbound counterpart to
                IListener; BlockingConnector dials non-blocking so its timeout
                means something), IoAwaitable, IAdmissionControl, SocketAddress,
                BlockingSocket (Winsock + POSIX),
                EpollSocket / IocpSocket / KqueueSocket (reactor-driven),
                InMemoryTransport (paired pipes + InMemoryListener),
                InheritedListener (systemd socket activation: LISTEN_FDS/
                LISTEN_PID parsing is pure and unit-tested; adoption applies
                close-on-exec and the shutdown timeouts, which are parameters
                rather than the caller's job),
                Framing/ByteReader (line and length-prefixed)
  Cli/          UsageDoc (usage text as data: sections of aligned rows and
                prose, rendered with an ANSI palette) and Options (OptionSpec
                row type, the matching rules, the one parse loop). Dependency-
                free by design — std plus the header-only ConfigError — so
                fastcache-cc can compile it in without linking the library
  Cache/        IStorage atomic primitives (incl. Prefetch — warm a tier with
                no read side effect), CacheEntry, CacheEngine,
                InMemoryLruStorage, CowTreeStorage (CoW B+tree, src/CowTree),
                LayeredStorage (L1 LRU over L2 disk), ShardedStorage
                (key-hash fan-out), TracingStorage (Tracy zones)
  CompileCache/ PathCanon (absolute<->canonical-token path rewriting +
                showIncludes/depfile region grammar), CompileValue (object-blob
                + tagged-text-region framing), PrefetchGroupManifest
                (prefetch-group id -> key-set + reverse index) — the
                compile-cache executor's domain logic
  Consensus/    RaftTypes, RaftLog, RaftNode and RaftDriver behind the
                IRaftStorage (state, log and snapshot, the last being what makes
                RaftLog::Compact's precondition satisfiable at all) /
                IRaftTransport / IRaftStateMachine / IRaftMessageSink seams, plus
                RaftWire (the 0xFA peer frame), RaftPeerTransport (outbound,
                a thread per peer), RaftPeerServer (inbound, on the reactor)
                and RaftMembership (the member set as a log entry) — Raft,
                split into a pure state machine and a coroutine driver that
                carries out what it asks for. RaftNode reads no clock, opens no
                socket and draws no randomness of its own: time arrives as a
                parameter, entropy through Core/IRandomSource, and everything
                the node wants done leaves as a RaftOutput. That split is what
                makes persist-before-send expressible at all — a callback sink
                cannot await a durability write, and a node whose vote reaches
                the wire before stable storage votes twice in one term after a
                restart, which is two leaders in one term. It is also what lets
                RaftClusterHarness run a whole cluster in one process against
                scripted partitions, loss, reordering and restarts while
                asserting the paper's safety properties after every step: a
                hand-written consensus implementation has no published
                verification vector to check against the way MurmurHash3 has
                SMHasher's, so that harness is the closest available oracle.
  Cluster/      DiscoveryService + DiscoveryWire (the LAN beacon and its PSK
                challenge), PeerDirectory (who proved the key, and where),
                ClusterState + ClusterStateMachine — the cluster's replicated
                configuration: who is a member, WHERE they answer, and the settings
                every member must agree on — and MembershipPolicy, the pure decision
                a leader makes about what to propose. The endpoint is the point:
                consensus counts ids, which is all a quorum needs and which leaves a
                node the cluster agreed to admit unreachable. A member records TWO
                addresses, and that pairing closed a defect rather than generalising
                one: `NotLeader` carries a redirect, and with only the consensus
                endpoint recorded a follower answered "ask the leader, at its Raft
                peer port". `Apply` is total because it runs after commitment, when
                refusing is no longer an option; `Validate` is where a change can be
                refused, and it runs on the proposer.
  Distributed/  WorkerRegistry (the worker set: exact-fingerprint grouping,
                least-outstanding pick, heartbeat expiry over IClock) and
                LeaseTable (lease issue/expiry/release plus the in-flight key
                map that suppresses duplicate work). Both pure with respect to
                I/O, which is what lets every capacity and expiry rule be a
                ManualClock unit test rather than a sleep. Named Distributed
                and not Dispatch because RedisResp.cpp already has a Dispatch()
                that collides under unqualified lookup inside namespace FastCache.
  Protocol/     IProtocolHandler, ProtocolAutodetect, MemcachedText,
                MemcachedMeta (1.6 mg/ms/md/ma/me/mn), MemcachedBinary,
                RedisResp (RESP2), CompileCacheHandler (the executor: custom
                0xFC binary protocol, canonicalize-on-STORE / serve-canonical-
                on-FETCH, leading-key group prefetch), CompileCacheWire
                (header-only, dependency-free: the 0xFC magic/version/opcode/
                status/error tables and their encoders, shared verbatim by the
                daemon, fastcache-cc and the test client)
  Server/       Connection (per-client coroutine), Server,
                ReactorServerLoop (the server driver)
  Platform/     IDaemonHost (ForegroundHost / PosixDaemonHost / WindowsServiceHost),
                ISignalSource, DaemonControls (process-wide stop/reload flags),
                CpuAffinity, HostMemory, HostInfo (what a machine IS: OS, version,
                architecture, disk space -- the facts a scheduler weighs),
                ServiceControl (ServiceSpec: what to launch, with which
                arguments, under which name and account -- the seam that lets one
                implementation of "install this as a service" serve more than one
                binary), Terminal,
                Environment (the one place the process environment is read),
                FileTrust (could only an administrator have put a file here?)
  Config/       Config, CliParser, ByteSize, YamlReader (yaml-cpp), ConfigReloader,
                EnvExpand ($VAR/${VAR} in path settings), DefaultConfigPath
                (per-platform config lookup + --seed-config, behind IConfigPathProbe)
  Metrics/      IMetricsSink + AtomicMetricsSink (counter-only by design; the
                dispatch counters separate no-worker from no-capacity because
                one says a fleet is misconfigured and the other that it is too
                small, and summing them hides the first when a fleet is busy),
                MetricsCatalog (the counter table: enumerator -> exported name,
                help and type, `static_assert`ed to cover every enumerator) and
                PrometheusFormatter, which renders that table rather than a
                hand-picked subset of it
```

Every executable lives under `src/apps/<name>/` and declares its own target and
install rule there; `src/apps/CMakeLists.txt` holds the app table that gates
each one, so adding an executable is adding a row:

```
src/apps/
  fastcached/               the daemon (FASTCACHED_BUILD_DAEMON, default ON)
  fastcache-cc/             the compiler launcher (FASTCACHED_BUILD_LAUNCHER,
                            default ON) — a drop-in sccache-style launcher that
                            keys on preprocess+relativized-args, FETCHes and
                            hit-replays with include paths localized,
                            misses→compile→STORE, and falls back safely on any
                            cache error. Config via `FASTCACHE_*` env, wired
                            through `CMAKE_<LANG>_COMPILER_LAUNCHER`. Platform
                            work sits behind `IProcessRunner` / `ITcpClient` /
                            `IPathResolver` (the last collapsing every spelling
                            of one location — 8.3, `subst`, junctions, symlinks
                            — to one, memoized per directory),
                            so main.cpp's flow logic is platform-free. Compiles
                            in `Cli/UsageDoc.cpp` plus `Platform/Environment.cpp`
                            and `Platform/Terminal.cpp` (see `_fc_cc_core`), so
                            its help renders and colorizes exactly like the
                            daemon's without linking the library. `Cli/Options`
                            is header-only, so including it costs no build row.
  fastcache-compile-node/   the compile worker AND the peer service (
                            FASTCACHED_BUILD_NODE, default ON) — registers with a
                            scheduler's `--listen-scheduler` endpoint (another node's;
                            `--listen-dispatch` on the daemon is gone) and answers
                            `Compile` on its own port. It may also BE the scheduler,
                            hold a cache tier for this machine's clients, and run
                            consensus — four surfaces, each off unless asked for
                            except the cache, and all four admitting this machine and
                            `--fleet-member` peers only. Carries its own daemon shell:
                            `NodeConfig` and its option table live in their own
                            translation unit (main.cpp is in no test target) so
                            `MakeNodeServiceSpec` and the install-time
                            `NodeServiceRejection` can be tested. It takes a *fingerprint* from a job
                            and never a program: the compiler comes from this
                            node's own `--toolchain` table, which is what keeps a
                            build accelerator from being a remote shell. Links
                            `FastCache` (unlike the launcher), because it needs
                            the reactor and the wire, and holds no cache stack of
                            its own — `AdminHttpServer`, not `Server`, is the
                            shape it follows.
  compile-cache-testclient/ low-level `0xFC` protocol probe + cross-depth
                            validation (FASTCACHED_BUILD_TESTCLIENT, default
                            OFF — test infrastructure, never installed)
  fastcache-bench/          in-process storage micro-benchmarks
                            (FASTCACHED_BUILD_BENCHMARKS, default OFF — test
                            infrastructure, never installed). Catch2 benchmarks
                            decomposing a lookup layer by layer, plus a
                            thread-scaling tier; driven by `bench/inproc_bench.py`,
                            which compares them against jitbit/FastCache's own
                            suite run on the same machine. Default OFF but built
                            by the `linux` and `clang-tidy` CI jobs, because a
                            target nothing compiles is a target that rots.
```

Platform service integration and OS packaging live under `packaging/`, which
follows the same table idiom — one descriptor row per installed asset, so a
new man page or logrotate snippet is a new row rather than a new
`install()` call:

```
packaging/
  CMakeLists.txt      the asset install table (source|destination|kind|name|
                      component); exports the config-file list reused by the
                      dpkg conffiles and rpm %config filelists
  linux/              system + user systemd units, sysusers.d/tmpfiles.d,
                      the commented /etc/fastcached/fastcached.yaml, and the
                      DEB/RPM maintainer-script templates (*.in)
  macos/              /etc/paths.d entry, the per-component postinstall
                      templates, the uninstaller, and the installer panes
  windows/            WiX fragment driving --install-service / --uninstall-service
```

`cmake/Packaging.cmake` turns that into `.deb`/`.rpm`/`.pkg`/`.msi` via CPack.
These constraints are load-bearing and have each already been a bug:

- **A pre-shared key authenticates a handshake; it never travels in a beacon.**
  Discovery broadcasts what a node *is* -- cluster, id, Raft endpoint -- and nothing
  derived from the key, because a broadcast reaches every listener on the segment
  and anything key-derived in one hands them what they need to join. The key only
  ever appears inside an HMAC over a nonce the challenger chose. Five consequences,
  each of which some plausible simpler design gets wrong:
  - **The proof authenticates a `(node, endpoint)` PAIR, not the nonce alone.**
    Both are inside the MAC. Signing the nonce only would let anyone who observed
    one valid proof replay its tag with a *different* endpoint substituted --
    admitting a legitimate node id at an attacker's address. An admitted node is
    assigned compile jobs and returns objects cached fleet-wide, so that is object
    injection into everybody's build. Verified by removing the endpoint from the
    MAC and watching exactly the one case that asserts it fail.
  - **A proof is only ever an answer to a challenge THIS node issued**, and the
    nonce is spent whatever the outcome. An unsolicited proof is refused *even when
    it carries the real key*: it answers a nonce nobody here chose, and accepting
    one would make the nonce -- and therefore the replay protection -- pointless.
  - **A peer that moves loses its authenticated bit.** The bit is a property of the
    node *at an endpoint*, not of the node, because that is what the proof covered.
    Carrying it across a change would admit an address nobody proved.
  - **The pending-challenge table is one entry per node, with a lifetime.** A
    beacon is unauthenticated by construction, so anything on the segment can
    provoke a challenge -- a table that grew per datagram would be a
    memory-exhaustion hole reachable without holding the key, which is the same
    shape as the pre-auth payload cap on the `0xFC` port.
  - **Discovery never changes membership.** It answers who proved the key and where
    they answer; a caller proposes. Admitting a node is a Raft decision only a
    leader may make, and a layer that proposed directly would have every node on
    the segment proposing the same change at once.
- **A cluster id is routing, not authentication, and saying so keeps it honest.**
  It is plain text in every beacon, so treating it as a credential would be the
  mistake. What it buys is that two unrelated fleets on one segment ignore each
  other -- before a challenge is issued, and again before one is answered -- which
  holds even when somebody shares a key across fleets, which they should not.
- **SHA-256 is implemented here because OpenSSL is optional and authentication is
  not.** `FASTCACHED_ENABLE_TLS` is off by default, so a cluster that could only
  authenticate its members when TLS happened to be compiled in would silently
  accept anybody in the common configuration. `Core/Compression` reaches for a
  library because a codec is large and its output need only round-trip; a MAC is
  small and its output has to be *identical* on every machine that checks it. An
  existing published algorithm, for the reason `MurmurHash3` records -- conformance
  is checkable against FIPS 180-4 and RFC 4231, and a local construction would have
  nothing to check against. Three things it carries:
  - **`ConstantTimeEquals` for every MAC compared against an untrusted value.**
    `memcmp` stops at the first difference, so its timing reveals how many leading
    bytes a guess got right and lets an attacker who can retry recover a tag byte at
    a time instead of guessing all 32.
  - **The padding sweep, not just `"abc"`.** The rule has two branches -- the length
    fits this block or forces another -- and the boundary is where a hand-written
    implementation goes wrong; a single vector never reaches it.
  - **A miscounted test vector accuses the wrong code.** Both long-key RFC 4231
    cases were hand-typed as hex runs and both were wrong (49 bytes for 50, 120 for
    131), which reads exactly like an implementation failure. They are constructed
    programmatically now.
- **A datagram double delivers a broadcast to the sender too, and loses only what
  it is told to.** `DatagramBus` mirrors what a real broadcast does, which is
  precisely the case `PeerDirectory` must ignore -- a double that spared the sender
  would hide the bug where a lone node records its own beacon and proposes a
  membership change to admit itself. Loss is scripted per destination rather than
  random: a random rate fails occasionally for reasons nobody can reproduce, and
  what discovery must survive is a specific peer going quiet. And the seam has a
  **real** implementation exercised by a smoke case, because an interface with only
  a fake behind it is an interface nobody has checked -- `OpenUdpSocket` returned
  null on Windows until it called `Detail::EnsureNetworkInitialised`, and no fake
  would ever have shown that.

- **Consensus had never been RUN, and five defects were waiting where no unit test
  could reach them.** `RaftNode`, `RaftLog`, `RaftDriver` and `RaftClusterHarness`
  are exhaustively tested against a simulated cluster in one process — which is the
  right place for the algorithm's rules, since a scripted partition is not something
  three real processes can be made to reproduce. What that cannot reach is the wire,
  the transport, the timers and the operator's own command line all having their
  first say at once, and `scripts/cluster-e2e.sh` is what does. It found, in order:
  - **`SyncRun` cannot drive a reactor.** It resumes a coroutine exactly once and
    throws when the coroutine is still suspended, so `SyncRun(driver->Run(&reactor))`
    aborted the process the moment `RaftDriver::Run` awaited `SleepUntil`. The
    correct spelling is the one `ReactorServerLoop` already uses: submit the loop as
    a `DetachedTask` and call `reactor.Run()`.
  - **A blocking listener serves one peer and never accepts another.** With
    `BlockingSocket`, every `co_await` inside `RaftPeerServer` completes
    synchronously, so its per-connection `DetachedTask` — written detached precisely
    so several peers can be read at once — runs to completion inline and the accept
    loop never reaches its next iteration. In a three-node cluster each node reads
    from exactly one of its two peers. Nothing crashes and nothing logs a fault; the
    fleet simply never becomes ready. Hence `Net/PlatformListener.hpp`, and hence the
    two loops sharing one reactor rather than owning a thread each.
  - **`RaftPeerTransport::Start()` was called by nobody.** The outbound side owns a
    thread per peer and starts them on request, so every node came up, listened,
    ticked its own timers and sent *nothing*. Three nodes sat at `undecided` forever
    with no error anywhere — the exact shape of failure this list keeps recording,
    and invisible to a single-node cluster, which elects itself with no messages at
    all.
  - **A leader's ADDRESS arrives after its role does.** A node announces its own
    record once elected, so the entry carrying its scheduler endpoint commits
    strictly after the role change that provoked it. Publishing only on a role change
    left every follower answering `NotLeader` with nothing for the rest of the term —
    which a client cannot tell from an election in progress and answers by compiling
    locally, every time. `ConsensusTier::Republish` therefore runs on a state change
    as well, and suppresses only an answer that has genuinely not moved.
  - **A loop that parks on a deadline it read cannot be told the deadline moved.**
    `RaftDriver::Run` reads `NextDeadline()` and hands the coroutine to the
    reactor's timer wheel, and `SleepUntil` has no cancellation -- while `Receive`,
    arriving from a peer-reader coroutine on that same reactor, can move that
    deadline *earlier*. The case that matters is a candidate winning: its deadline
    goes from an election deadline up to `electionTimeoutMax` away to a heartbeat
    deadline one interval away. The new leader's first heartbeat still goes out
    immediately (`BecomeLeader` sends it), so the cluster looks elected; its
    **second** is then late by most of an election timeout, the follower that drew
    the shortest randomized timeout elects itself,
    and the next leader repeats it. Measured on three real nodes: **nine role
    changes in twelve seconds** with nothing else wrong, against two after the fix
    -- and a `Linux-clang-release` CI failure in which `cluster-e2e` found exactly
    one leader, counted exactly one, and then found a *second* one two assertions
    later. The sleep is bounded by the heartbeat interval now, which is the same
    answer `BlockingListener::SetTimeouts` gives to the same shape of problem: a
    wait nothing can interrupt is bounded rather than left to be woken. It costs a
    leader nothing (it already wakes at that cadence) and a follower a few empty
    wake-ups per election timeout. Three things worth keeping:
    - **No single-threaded test can see it, which is why it is on this list.**
      `RaftDriver_test` and `RaftClusterHarness` both advance a node by calling
      `Tick` directly, so the deadline the loop is *sleeping on* does not exist in
      either. The regression case therefore drives `Run` on a `TestReactor` and
      delivers the winning vote through `Receive` while the loop is parked --
      verified by removing the bound and watching that one case, and only it, fail.
    - **A single poll passes against leadership that never settles.** A cluster
      re-electing on a timer has exactly one leader at almost every instant; two
      only in the window where a deposed leader has not yet heard from its
      successor. `cluster-e2e` asked once, which is how this reached CI as an
      unrelated-looking failure. It now holds the question open for three seconds
      and requires the same node to answer throughout.
    - **The fixture dumps every node's log on any failure**, not only when no
      leader appears. A consensus defect that reproduces once in five runs is
      diagnosable from the logs or not at all, and cleanup deletes them.

  Two further consequences are worth stating on their own. **`RaftDriver` holds a
  mutex now**, because the node is advanced from three routes by construction — the
  timer loop, a peer reader, and whatever proposes a configuration change — and
  `RaftNode` has no synchronization of its own, which is exactly what makes it
  testable. And **the reactor is stopped by the loops themselves, when the second of
  them finishes**, because `IReactor::Run` returns with its timer heap and its parked
  work exactly where they were: a loop still suspended at that moment is a coroutine
  frame nobody ever resumes and nobody ever frees. For the same reason
  `RaftPeerServer::Shutdown` closes the connections it accepted and not only its
  listener.

- **A cluster setting that nothing can change at runtime is a log entry pretending to
  be configuration.** The replicated log carried settings, applied them, snapshotted
  them and replicated them — and no surface anywhere said "set `upstream` to this", so
  the only way to configure a fleet was still `--upstream` on every machine, which is
  the file-editing the log exists to replace. `Op::ClusterStatus` / `ClusterSet` /
  `ClusterForget` on the scheduler's port close that, and four things about their
  shape are load-bearing:
  - **They go through the same `Gate()` as the dispatch verbs, the READ included.** A
    follower's copy of the state is valid and merely older, so `ClusterStatus` could
    have been answered anywhere; one rule for the whole surface is what makes "a verb
    added without the gate" impossible, and it sends an operator to the node they
    would need anyway to change anything. The refusal for a non-member is not about
    capacity here: a stranger who could set `upstream` would point the whole fleet's
    cache at a host of their choosing.
  - **`NoCluster` is distinct from `NotLeader`, because the operator does something
    different.** `NotLeader` names somewhere else to ask; `NoCluster` says the
    question does not apply here at all — a single node started without `--node-id`
    leads itself and has no replicated state. Answering the second with the first
    sends somebody looking for a node that does not exist.
  - **The consensus-to-wire refusal mapping is a table with one row per
    `ConsensusErrorCode`, `static_assert`ed on its length.** A `switch` here and a
    `switch` somewhere else drift, and a refusal reported under the wrong code sends
    an operator to fix something that was never wrong. The three peer-wire decode
    codes cannot arise from a *local* proposal — no bytes are involved — so they map
    to the generic refusal rather than to a claim about what happened.
  - **The reply says "accepted", never "committed".** A leader appends the entry and
    answers; whether a majority has taken it is not something it knows yet, and a
    tool that said otherwise would be the one kind of report that must not be wrong.
    `IClusterAdmin` is the seam the scheduler reaches all of this through, which is
    what lets the whole verb surface be tested against a fake that records what it was
    asked to propose, with no log, no threads and no cluster.

- **Absent is not empty, and a membership proposal is where that pays.**
  `Cluster::DesiredMember` carries `std::optional<std::string> schedulerEndpoint`
  while `ClusterMember` carries a plain string, and the difference is load-bearing in
  one direction only. `AddMember` applies **wholesale** — a re-proposal that omitted a
  field would clear it — which is right when the proposer knows the member has no
  scheduler surface, and destructive when it simply never knew. Discovery is always
  the second case: it proves where a peer answers *consensus*, because that is what
  the MAC covered, and learns nothing about the port clients speak to since nobody
  dials it. So a node says `""` about itself and `nullopt` about a peer, and only the
  first is an assertion. Collapsing the two would have every follower's discovery loop
  clear the leader's redirect address the moment it noticed the leader — a fleet whose
  redirects break on a timer, self-healing at the next election and therefore
  intermittent.

- **A service to register is a `ServiceSpec`, and what it runs as is part of it.**
  Every function in `Platform/ServiceControl` took `Config const&` -- the *daemon's*
  configuration type -- so a second binary could reach none of it without either
  depending on the daemon's configuration or growing a parallel copy of a 1273-line
  file. That is the whole reason `fastcache-compile-node` had no service integration
  on macOS or Windows. Four things about the seam's shape are load-bearing:
  - **`daemonFlag` is a field rather than an argument**, because the two supervisors
    disagree and one spec has to answer both: the Windows SCM needs it (it is the
    hook that hands control to `WindowsServiceHost`) and launchd needs its absence,
    since like systemd it supervises the process it started and reaps a job that
    forks as "exited".
  - **`serviceAccount` and `ownedDirectories` replaced a constant and a reach into
    `cfg.storagePath`.** Which account a service runs as, and which directories it
    must own before its first write, are properties of the *service*. The rule they
    carry is unchanged and still the point: only directories the operator actually
    named, never a parent -- `--storage=/var/db/fc` must not hand `/var/db`, shared
    with other system services, to an unprivileged cache account.
  - **An empty `serviceAccount` means root, so a worker names one.** A system-scope
    launchd job with no `UserName` runs as root, and `fastcache-compile-node`
    compiles input that arrived over the network. Naming `fastcache-node` -- the
    account the Linux unit already uses -- puts the existing "that account does not
    exist" guard in the way, so a macOS system-scope install **refuses** until the
    package creates it rather than silently succeeding with root privileges
    (issue #87). It is deliberately not the daemon's `_fastcached`: one account
    would let a compromised compile rewrite every cached object.
  - **`BuildServiceArgv` stays hand-written per binary.** An `OptionSpec` says how
    to *parse* a flag and carries no way to read a value back out, so "emit every
    field that differs from its default" cannot be written once generically. Each
    binary's version is guarded by a test that walks its own option table and
    requires every non-excluded row to be emitted -- the daemon's table once
    stopped after nine fields, so `--install-service --tls --metrics` reported
    success and registered a plaintext, unmonitored daemon.
  - **The refactor was checked to change nothing rather than argued to.** The
    registered command line is byte-identical to the one the previous version
    produced, which is why `--daemon` is inserted at the *front*: nothing downstream
    cares about flag order, and a refactor that can be shown to be identity is worth
    more than one that merely ought to be.
- **An install-time refusal is the only place an operator is watching.** Every rule
  in `NodeServiceRejection` describes a registration that would **succeed** and then
  produce a service which cannot do its job -- silent from both ends, since the
  operator is told it was installed and nothing later says otherwise. `--advertise`
  is the one worth naming: left empty it bakes in `{--bind}:{--port}`, and `--bind`
  defaults to `0.0.0.0`, which is not an address a client can dial. Such a worker
  registers, heartbeats happily, is leased out by the scheduler, and is never
  reached. `--scheduler` and `--toolchain` get the same treatment because each
  would start and exit at every boot. Two related choices: an **uninstall** is not
  gated the same way -- refusing to remove a registration because it was
  misconfigured is how a bad one becomes permanent -- and a bare compiler path is
  **not** resolved to a fingerprint at install time, because the worker derives that
  at startup through the identical code its clients use and a digest computed once
  would pin the registration to a toolchain an update then changes underneath it.
- **A candidate config location names an application, not `fastcached`.** Every row
  of `DefaultConfigCandidates()` hardcoded the daemon's file name, so a second
  binary had nothing to generalize onto. The rows carry `{app}` now, and two things
  about that are deliberate: the table stays `constexpr` so its four `static_assert`s
  keep stopping the *build* rather than waiting for `ctest`, and the substitution is
  a literal replace rather than `std::format` -- a display form already contains
  `%VAR%` on Windows and `$VAR` on POSIX, so a third meta-syntax whose braces are
  also `std::format`'s own grammar is how a path containing a brace becomes a thrown
  exception at startup.
- **A daemon host wraps the body, so what must reach a terminal has to happen
  first.** `WorkerBody` is separate from `main` because `IDaemonHost` double-forks on
  POSIX or hands control to the SCM, and neither can wrap a `main` that has already
  parsed, validated and registered. The cheap, fallible checks stay in `main`
  deliberately: run inside the body they would print their diagnosis to a stdout the
  POSIX host has already redirected to `/dev/null`, so a misconfigured worker would
  exit in silence.

- **A return type is not part of a function's name on Linux, and MSVC's mangling
  hides that.** `Core/HostPort.hpp` added an `inline FastCache::ParsePort(
  std::string_view)` returning `std::optional<std::uint16_t>` while
  `Config/CliParser` already had a `FastCache::ParsePort(std::string_view)`
  returning `std::expected<std::uint16_t, ConfigError>`. That is **not an
  overload**, and no compiler can say so: each translation unit sees exactly one
  of the two declarations, so both compile, and the Itanium ABI does not encode a
  return type in a free function's mangled name -- so both definitions claim the
  identical symbol, the linker keeps `CliParser`'s strong one over the header's
  weak inline, and every caller of the header version silently reaches the other.
  It reads an `expected` as an `optional`: a SIGSEGV on the first call, from code
  that is correct in isolation. Renamed to `ParseTcpPort`, with the reason at the
  declaration. Three things worth keeping:
  - **Windows cannot find this and will report the tree as green.** MSVC's
    mangling *does* include the return type, so the two are distinct symbols
    there and both link. This branch had 1730 passing MSVC tests at the moment
    Linux was segfaulting, which is the whole argument for running the Linux
    gate locally rather than discovering it in CI a phase later.
  - **A standalone reproducer will not reproduce it**, because the bug is in the
    *link*, not the code: the same calls compiled against the header alone are
    correct and pass under ASan. What identified it was `nm -C` on the library
    object, showing a strong `T FastCache::ParsePort(...)` that the test binary
    had no business resolving to.
  - **The two implementations were not merged**, deliberately. The CLI's version
    distinguishes "not a number" from "out of range" because an operator needs
    to be told which; an `optional` cannot carry that. Collapsing them to share
    one body would trade a real diagnostic for a de-duplication nobody asked for
    -- the same reasoning that keeps the dispatch counters split.

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
  - **"Bind a port twice" is not a portable way to test a bind failure.**
    `SocketAddress.cpp` sets `SO_REUSEADDR` unconditionally, which on POSIX only
    skips `TIME_WAIT` but on **Windows lets a second socket bind an address already
    in use** -- so the obvious test passes on Linux and macOS and fails on Windows.
    The case provokes the refusal with an address no host holds (RFC 5737
    `192.0.2.1`) instead. That the option means two different things on the two
    platforms is a real defect in its own right and is filed as issue #85, not fixed
    here.
  - **The stop test is bounded rather than allowed to hang.** A case that deadlocks
    when the destruction order is reversed reports the defect as a suite timeout
    naming nothing, which this repository has already paid for once
    (`dist-compile-e2e ***Timeout 900.10 sec`). It destroys the endpoint on another
    thread and fails in 15s saying what it waited for.
- **`main` is not exempt from cognitive complexity, and the fix is extraction rather
  than a raised threshold.** The node's `main` reached 70 against clang-tidy's limit
  of 60 as the admin endpoint was wired in. Both blocks that came out --
  `AdminEndpoint::Start` and `AdoptActivatedListener` -- are coherent decisions with
  one answer each, which is why the number was a symptom worth listening to rather
  than a rule to argue with. The six bare `return 2`s it left behind became
  `ExitUsage` for the same reason: seven copies of a magic exit code is the
  table-shaped defect this list keeps recording.
- **An architecture is what the compiler built for, not what the kernel is running.**
  `QueryHostFacts` reads the architecture from the compiler's own macros rather than
  from `uname`/`GetNativeSystemInfo`, because an x86-64 process under Rosetta or
  WOW64 executes x86-64 code on a machine that truthfully reports `arm64`. A
  scheduler weighing this node has to be told what the binary that will run a compile
  is, not what silicon is under it. Windows's version comes from `RtlGetVersion` and
  not `GetVersionEx`, which reports 6.2 for every release since Windows 8 unless the
  caller ships a compatibility manifest. And free space is `space.available`, not
  `space.free`: the difference is the root-reserved portion, which an unprivileged
  worker cannot write and must not offer to a scheduler as room it has.

- **A snapshot is durable before it is acknowledged, and its configuration travels
  with it.** `IRaftStorage` had `SaveState` and `SaveLog` and nothing else, so
  `RaftLog::Compact`'s stated precondition — the caller has made a snapshot through
  `through` durable first — could not be satisfied by any API in the tree. Two
  consequences, and neither fails loudly. A follower that answers `InstallSnapshot`
  at index N without persisting it **retracts that acknowledgement on restart**,
  after a leader may already have counted it towards commitment: Leader
  Completeness, lost to a write nobody made. And a leader that compacted and
  restarted came back with an *empty* `_snapshotState`, so the next follower far
  enough behind was shipped an empty snapshot **as though it were state**. Hence
  `IRaftStorage::SaveSnapshot`, the snapshot on `RecoveredState`, and
  `RaftOutput::saveSnapshot` — carried through the output channel rather than
  written by whoever asked for the compaction, for exactly the reason `persist`
  and `persistLog` are: it is a durability write that has to be ordered against
  the messages, and only the driver can order it. Consequences that are each
  load-bearing:
  - **The write order is snapshot-then-trim, and the crash window it leaves is
    the reason that order is right.** A crash between them leaves a durable
    snapshot beside a log that still holds the entries it covers, which
    `RaftNode`'s constructor reconciles by compacting to the boundary. The
    opposite order leaves a log missing committed entries and no snapshot to
    replace them, which nothing can repair. That reconciliation is also what lets
    a store which never trims its log be merely wasteful rather than wrong.
  - **A trimmed log cannot be positional, so each record carries its own index.**
    `FileRaftStorage` derived an entry's index from its position in the file,
    which has no answer once a prefix is gone — and in the crash window above it
    has a *wrong* answer, silently: entry 8 recovers as entry 1, and every index
    in the store is off by the length of the discarded prefix. The degenerate case
    needs the snapshot as well: a log trimmed to nothing has no record left to
    state where it resumes, and without that the next append is refused as a gap
    forever.
  - **The configuration is part of the snapshot, because compaction is precisely
    what leaves nothing to re-derive it from.** `RefreshConfiguration` scans the
    log for the newest `Configuration` entry and falls back when it finds none —
    and the fall-back was the **bootstrap** member set. So a node that took part in
    a membership change and then compacted past it forgot that change, silently,
    and only after a restart: `Quorum()` then counts a majority of the wrong set.
    The scan also ran to index 1 rather than stopping at the log's own first index,
    which is what made "no entry" the answer for a log that merely no longer holds
    one.
  - **`HasUncommittedConfiguration()` is `LatestConfigurationIndex() > _commitIndex`,
    derived rather than scanned for separately.** They are the same question, and
    two backward scans answering it independently are two places for the rule to
    drift.
- **A seeded draw must be the same on every platform, or a seeded harness is not
  reproducible.** `std::mt19937_64` is specified bit-for-bit by the standard;
  `std::uniform_int_distribution` is **not** — how it reduces the engine's output
  to a range is the implementation's business, and libstdc++ and libc++ do it
  differently. `SystemRandomSource`'s fixed-seed constructor exists so a failure
  can be replayed, and `RaftClusterHarness` seeds one per node so a whole cluster's
  adversarial schedule is reproducible; both promises held only within one standard
  library. The harness therefore ran a **different** schedule on macOS than on
  Linux and Windows, and three cluster cases failed there and nowhere else — in CI,
  at `-O3`, where nothing local reproduces it. `UniformInRange` now does the range
  reduction itself, and a golden vector pins it. The same argument `Core/MurmurHash3`
  makes about its digest and `PathCanon::AsciiLower` about locale: a value this
  codebase relies on being identical everywhere cannot come from something allowed
  to vary. Two consequences:
  - **It takes the HIGH bits of the engine draw, and that is not a detail.**
    Masking the low bits is the shorter spelling and was the first version. Mersenne
    Twister seeded with *adjacent* values produces correlated low-order output for
    its first draws, and the harness seeds its nodes `base + 0`, `base + 1`, … — so
    five nodes drew near-identical first election timeouts, campaigned together and
    split the vote, round after round. Election jitter exists precisely to
    decorrelate those draws; sourcing it from the one part of the output that is
    correlated across neighbouring seeds defeats the mechanism it feeds.
  - **The three tests it was masking were a real defect, not bad luck.** See the
    next entry — which is the reason a harness like this is worth its cost at all.
- **Pre-vote asks whether a LEADER is live, so it must not be answered from this
  node's own election timer.** `OnPreVote` refused when `now < _electionDeadline`,
  and that deadline is re-armed when the node *starts its own pre-vote round*. So a
  node that had just begun campaigning answered "yes, I heard from a leader
  recently" for a full timeout and refused every peer that timed out alongside it —
  which is the ordinary case in a cluster whose nodes are meant to race. Nothing
  fails and nothing is unsafe: a five-node cluster simply took some **forty**
  election rounds to elect anybody where one should do, which reads as a livelock
  in a cluster test and is invisible to a unit test that only ever has one
  candidate. Measured at 994 harness steps before and 20–23 after. `_lastLeaderContact`
  is now its own field, set only where a leader actually spoke — an accepted
  AppendEntries or InstallSnapshot — through `NoteLeaderContact`, while standing for
  election and granting a vote still arm the timer alone. The window is
  `electionTimeoutMin` rather than the node's own randomized deadline, because "is
  there a live leader" is a fact about the cluster that every node should answer
  the same way at the same instant. Both halves are tested: a campaigning node
  still grants, and a node that has just heard from a leader still refuses —
  losing the second while fixing the first would trade a slow election for the
  disruption pre-vote exists to prevent. The residual, unchanged by this and
  recorded deliberately: a **leader** never hears from a leader, so its own
  `_lastLeaderContact` ages out and it grants a challenger's pre-vote — exactly as
  it did before, since a leader arms no election timer either. Refusing there is
  CheckQuorum or a leader lease, which `RaftCluster_test` already names as a
  separate mechanism and which nothing here needs yet.
- **A round-trip test that omits a message type omits the arm most likely to be
  wrong.** Five of `RaftWire`'s eight encoder arms are near-copies of another —
  PreVote of RequestVote, `InstallSnapshotResponse` of `AppendEntriesResponse` —
  and the mistake copying invites is a transposed field index. The four types added
  with pre-vote and snapshots had **no positive round trip at all**, so an arm
  writing `lastIncludedTerm` where `lastIncludedIndex` belongs passed the entire
  suite; verified by making that transposition and watching only the new cases
  fail. The replacement is one exemplar per `MessageTable` row with **every field a
  different value** — two fields sharing a value would let the transposition
  through — compared whole through `operator==` rather than field by field, since
  field-by-field checks are what the copied arms already survived. A row without an
  exemplar fails the case rather than going quietly untested, and the enum sweep is
  kept separate precisely so the exemplars' values can stay distinct.
- **The residuals here are recorded rather than closed**, because each belongs to a
  later phase and closing it now would be guessing at that phase's shape:
  `RaftDriver` has no compaction *policy*, so `CompactThroughApplied` has no
  production caller yet — the log grows until something decides when to trim, which
  is the daemon's decision and lands with the daemon shell. And a member added by
  `ProposeMembership` carries an id but no **endpoint**, so a node the cluster has
  agreed to admit is still unreachable until discovery supplies one; membership and
  addressing are deliberately separate, and joining the two is what PR 5 is for.
- **A flag is one row, and every binary's row table drives both parsing and
  help.** The daemon used to declare flags four ways — hand-written `if (arg ==
  …)`, a descriptor array, two inline `initializer_list<tuple<…>>` tables, and
  seventeen copy-pasted blocks in `HandleTypedFlag` — and then spell every name
  again in a separate help table, so a flag could be accepted but undocumented
  or documented but rejected. `CliOptions()` is now the single source of truth,
  and the help column is *derived* from `primary`/`alias`/`operand` rather than
  restated. Rendering lives in `Cli/UsageDoc`, which must stay dependency-free:
  `fastcache-cc` compiles it in rather than linking `FastCache`, so an include
  of anything from `Config/` there breaks the launcher's link, not just its
  build. Two remaining hand-written spellings are guarded by tests rather than
  generated: `BuildServiceArgv` (a `ServiceControl_test` case walks
  `CliOptions()` and requires each non-excluded flag to be emitted — the
  exclusions, `--requirepass` above all, are listed with their reasons) and the
  launcher's `FASTCACHE_*` oracle list in `LauncherCli_test`.
- **Every protocol checks the configured credential, and the compile cache was the
  one that did not.** `session.CurrentAuth()` was consulted by `MemcachedText`,
  `MemcachedBinary` and `RedisResp` — and by nothing in `CompileCacheHandler`. So a
  daemon started with `--requirepass` gated three protocols and served the `0xFC` port
  to anyone who could open a socket, with no flag, log line or doc saying so. On its own
  that is a cache-poisoning surface; it becomes remote code execution the moment that
  port carries anything that *runs* a compiler, which is why it is closed before any
  distribution work rather than after. Consequences that are each load-bearing:
  **which verbs are reachable before a credential is a column of `OpTable`**
  (`OpDescriptor::preAuth`), not a predicate with its own `switch` — it is the
  security-relevant property of the whole verb set, so a reviewer must read it off the
  table, a verb added without a thought about it defaults to closed, and an opcode the
  table does not know is refused rather than waved through. The **gate runs before the
  payload is buffered** and drains with `Skip`, exactly as `MemcachedBinary`'s does:
  checking afterwards would let an unauthenticated peer pipeline frames each declaring
  `maxPayloadBytes` (256 MiB by default) and force that allocation per frame — a
  memory-exhaustion hole opened by the check meant to close a hole. And the
  per-connection state records **only what was verified**, never "is this connection
  allowed through": seeding a flag from the policy at connect time is the obvious
  spelling and is wrong in both directions — a connection opened while auth was off
  stays exempt for life across a `SIGHUP` that turns auth *on*, and nothing then
  distinguishes "auth is off" from "this peer proved something", so enabling auth later
  silently blesses every open connection. Rotation is the deliberate exception the other
  way: a peer that proved the credential current when it connected keeps access when the
  secret changes under it, as redis does, because re-gating on rotation fails every
  in-flight build at the moment an operator rotates.
  - **The gate has exactly one door held open, and that door needs its own lock.**
    `Op::Auth` is `preAuth` by construction, so its payload is read while the peer has
    proved nothing — bounded only by `session.maxPayloadBytes`, i.e. the whole 256 MiB
    the gate exists to deny, reached through the gate. `OpDescriptor::maxPayload` is
    therefore a second column (`MaxAuthPayload`, 4 KiB, for AUTH; `0` = "the session
    cap" for STORE and FETCH, which carry object files and are read only after
    authentication), and `PreAuthVerbsAreBounded()` is `static_assert`ed so a future
    pre-auth verb cannot reopen the hole by omission rather than by decision. The
    refusal names the verb whose ceiling it hit, because "exceeds cap 268435456" tells
    an operator nothing about a 4 KiB limit.
  - **Adding a verb must not break the fleet that does not have it, and that is a
    property of the CLIENT.** `Op::Auth` deliberately did not bump `CurrentVersion` —
    the framing exists so a receiver steps over a verb it does not know — so a daemon
    predating this change answers AUTH `unknown-opcode`, skips it, and serves the
    pipelined command correctly. Returning that refusal as the exchange's outcome, which
    is what a plain "any error is the answer" client does, gives a token-configured
    launcher a permanent **0% hit rate** against every not-yet-upgraded daemon, reported
    as `rejected (unknown-opcode)`: a plausible-looking message with no obvious cause,
    and the exact mixed-fleet case the wire's extensibility was built for. So
    `unknown-opcode` **on AUTH specifically** falls through to the command's own reply;
    every other refusal is about the credential and is still reported. It is not
    silent, though — `CacheOutcome::credentialIgnored` surfaces one note per build,
    because the operator asked for authentication and did not get it, and "the cache
    quietly did less than you told it to" is the failure mode this list exists for.
  - **It costs no round trip, and that is a property of how the client sends rather
    than of the wire.** Authentication is per-connection state and the launcher opens a
    fresh connection per *operation*, so AUTH-then-await-then-command would double the
    round trips of every translation unit — the exact cost the "no handshake" decision
    below exists to avoid. Replies are strictly ordered and one-per-request, so the
    launcher **pipelines**: both frames go out before either reply is read. They are two
    `SendAll` calls, not one concatenated buffer — equally pipelined, since neither waits
    for a reply, but concatenating means copying a STORE frame that carries a whole
    object file, raising peak footprint from about twice the object to three times it on
    the hot path of a parallel build, to buy nothing. The test therefore asserts the
    *write/read interleaving* (`"SSR"`, never `"SRSR"`) rather than a write count: a
    count of one would state the copy instead of the property, and the bytes are
    identical either way so the outcome alone cannot tell the two apart. The client must
    still consume the AUTH reply even when it intends to ignore it; skipping it strands a
    frame and the next command reads the previous one's answer.
- **A compile-cache frame declares its own length, so a rejection can be a reply
  instead of a close.** The pre-1 header was `[magic][op]` with no length, and
  that is what made every refusal — bad magic, unknown opcode, oversize field —
  a silent `co_return`: with no declared length the server could not find where
  the frame it was refusing ended, so it could not answer and resynchronize. A
  client cannot tell that apart from a dead connection, so a mismatched install
  presented as a flaky network and a cache that never warmed. The header is now
  `[magic][version][op][u32 payloadLength]` and every reply is
  `[status][u32 payloadLength][payload]` — uniformly, including a miss, which is
  a zero-length payload rather than no payload. `MemcachedBinary` already proved
  the pattern: it can refuse-and-continue precisely because its header declares
  `totalBodyLen`. Consequences that are each load-bearing: `Miss` is distinct
  from `Error` (both were `0x00`, so a rejected client saw an endlessly cold
  cache); an `UnsupportedVersion` message names the supported *range*, since a
  rejection that cannot say what would have worked cannot be acted on; and there
  is deliberately **no handshake**, because the launcher opens a fresh connection
  per *operation* and a HELLO would cost 2–4 round trips per translation unit on
  the exact path this list already records regressions on.
- **A key determines two artefacts, so it must be a function of both.** Preprocessing
  suppresses line markers (`-E -P`, `/EP`) so a checkout path never reaches the key —
  which is what makes a key portable, and equally what made it *invariant under a header
  move*. Move a header without changing a byte of it and the token stream is identical:
  the object is still correct and was served, while the depfile, which is nothing but
  paths, named a file that is gone. That is worse than a miss, because Ninja records the
  dependency, cannot stat it, rebuilds, hits the same value, and never converges — with a
  successful exit code every time. The dependency path set is therefore part of the key
  (`objkey-v4`, `KeyInputs::dependencyPaths`), captured on the preprocess run the launcher
  already makes rather than in a second probe: measured at **+1.5% on a 45 ms preprocess**,
  because the compiler has already opened every one of those files. A move is a different
  key by construction, so the *pre-move* entry survives the move rather than being
  overwritten — which is the property `check_header_move` asserts by moving the header
  back and requiring a HIT. Anchored as `fastcache-cc: HIT`: the launcher prints
  `STALE HIT (...); recompiling` on its way to a MISS, so a bare `grep HIT` is satisfied by
  exactly the collapse the case exists to reject. `ComputeManifestKey`'s `manifest-v4` tag is
  bumped in lock-step with `objkey-v4` for a related reason — a manifest stores the object key
  *by value* and its own key never sees the object-key schema, so a manifest written by an
  older launcher keeps resolving to an older object; direct mode is on by default and
  short-circuits before the preprocessed path,
  so without the second bump the re-key never happens where it matters most.
  - **Which paths are hashed is the whole subtlety, and the exclusion cuts the opposite
    way from the inclusion.** `KeyDependencySet` normalizes each path through
    `DirectManifest`'s `NormalizePath` **first** — a driver echoes a path as *resolved*, so
    `build/../inc/a.hpp` and `./inc/a.hpp` arrive verbatim, and unnormalized they are two
    key entries for one header and two different keys on two machines whose generators
    spell an include directory differently. Then it keeps a path that canonicalizes to a
    `<SRCROOT>`/`<BUILDTREE>` token and **drops** toolchain content, judged by
    `DirectManifest`'s own `IsToolchainHeader` so that this filter, the manifest's and the
    replay guard's cannot disagree: a path under neither root, *and* a vcpkg tree nested
    under the build tree, which canonicalizes but is still the producing machine's. That is
    content already covered collectively by the compiler identity in the key, and hashing it
    would mean two machines with the same compiler at different install prefixes share
    *nothing at all* — 476 of a real TU's 635 headers are toolchain, and a manifest naming
    them would be machine-specific. The set is sorted and deduplicated because
    `/showIncludes` repeats a header once per inclusion site and emission order is a
    property of the driver.
    - **A relative path is classified by what it resolves to, never by its spelling** —
      which is why `KeyDependencySet` takes a working directory at all, threaded from
      `RunCached` through `CompileWorkingDirectory()`, the one place `main.cpp` answers
      that question for both this filter and the replay guard. Both rules above ask what a
      path *names*; "is it absolute" answers a different question, and the two invert for a
      vendored or relocatable toolchain reached through a relative include path (issue
      #64). `-isystem ../toolchain/include` makes the driver report
      `../toolchain/include/foo.h`, which lies under no root — so a spelling test *kept and
      hashed* the very file it drops when the driver spells the same header
      `/home/dev/toolchain/include/foo.h`, and a build tree at a different depth then keyed
      every TU that touches it differently. Resolving first collapses the two branches into
      one: a relative project header becomes the token its absolute spelling would produce
      (so one header reached two ways is one entry), and a relative toolchain header is
      dropped exactly as an absolute one is. What the key gives up — a *vendored* header
      move no longer re-keys — is what `Cc::MissingReplayedDependency` still covers, since
      it probes a relative replayed path for existence before writing anything; the same
      trade and the same backstop the absolute exclusion has always had. Two mechanics are
      load-bearing: the resolution is **lexical**, because `weakly_canonical` would rewrite
      an 8.3 short component to its long form and break the prefix tests two bullets down;
      and it runs on `/`-folded text, because `std::filesystem` treats `\` as a separator
      only on a Windows *host*, so a Windows path normalized on POSIX keeps its `..`
      segments and canonicalizes to `<BUILDTREE>/../../inc/a.hpp` — a token naming a file
      the path does not name. Two further host couplings live in that same lexical pass and
      were each *measured* on libc++ rather than assumed. `lexically_normal` collapses a
      leading `//` on POSIX and keeps it on Windows, where it is a UNC root name, so the
      root is carried across the pass by hand — without that, a `\\server\share` layout
      stops prefix-matching its own root on one host, classifies every project header as
      toolchain, and hands back an empty set. And the absoluteness test is asked **before**
      the collapse rather than after, because `C:/../x` normalizes to a bare `x` on POSIX
      (`C:` is an ordinary filename there for `..` to eat) while Windows cannot ascend past
      a drive root and keeps `C:/x` — so collapsing first sends an absolute path down the
      resolve-against-the-working-directory branch on one host only. Asking first is also
      what makes the relative branch normalize once, after the join, instead of on each
      side of it. The tags moved to `objkey-v4`/`manifest-v4` for this change, in the
      lock-step the manifest bullet above describes — but note which half forced it. The
      *dependency-set* re-key would not have needed a bump on its own: it changes a key only
      for a translation unit that reported a relative path, and those keys differ by
      construction, so a stale entry becomes unreachable rather than servable under rules it
      was not written by. The manifest half is what required it, and `objkey` follows the
      manifest here rather than the other way round. A working directory that is empty or
      itself relative drops
      every relative path rather than guessing, and the `dependency set: N of M` note is
      what keeps that from being silent. The *residual*, recorded deliberately: a relative
      include-dir argument (`-I../../vendor/sdk/include`) still reaches the key verbatim
      through `RelativizeArgs`, which canonicalizes only paths it can resolve — absolute
      ones — so two machines whose build directories sit at different depths still key
      apart on the arguments even though the dependency set now agrees. Closing that means
      resolving arguments against the same working directory, which is a separate change to
      a separate function.
  - **"Absolute or relative" is the wrong question on Windows, because there are three
    answers.** `C:foo` carries a drive specifier and is still not rooted: it resolves against
    *that drive's* current directory, per-process state on the producing machine that no
    cache entry records. `PathCanon::AnchorForLayout` therefore returns an `Anchor` —
    `WorkingDirectory`, `Absolute`, `DriveRelative` — where it used to return a `bool
    IsAbsoluteForLayout` whose drive test stopped at the colon and so reported all three
    Windows shapes as absolute (issue #65). Every caller switches on it with no `default:`, so
    a fourth state is a compile error at each rather than a silent fall-through. What none of
    them may do is treat it as `WorkingDirectory`, and since issue #64 that branch does
    something stronger than keep a spelling — it *resolves* the path against the compile's
    working directory, which for `C:foo` names a file that was never read. Hashing the
    spelling instead, as it used to, would let two machines whose `C:` cwd differs key
    **identically for different headers**, the same silent cross-TU mis-serve #63 closed by a
    different route. Either way the answer is the same: a drive-relative path is neither.
    **Past that the callers part, and
    the asymmetry is the substance of the fix.** The key filter needs a portable *spelling*,
    so it leaves the path to the root tests — root membership is the stronger question, and
    under a drive-relative *root* (`C:src\proj`, a Windows root by its separators) the path
    canonicalizes to a token that is portable precisely because the consumer substitutes its
    own root. Dropping on the anchor alone would have silently un-keyed that whole layout,
    and it is what the first cut of this change did. The replay guard needs something to
    *stat*, which a drive-relative path is not under any working directory it could be handed
    — `std::filesystem::operator/` reaches a drive's current directory by no route: on POSIX
    the join names nothing that exists, on Windows it resolves against the *process* cwd — so
    it skips, under the existing rule that a path which cannot be examined counts as present.
    For a drive-relative root that arm is a behaviour *change*: such a path used to be probed
    against the wrong anchor and discarded every hit carrying it. The **manifest** is the
    third caller and sides with the key filter, on the same reasoning read through its own
    failure mode: it *opens* a path rather than probing one, and an unreadable entry refuses
    the manifest while recording (`HashFileContents` yields nothing) and fails to validate
    while reading — safe in both directions — so the stronger root question is worth asking,
    and dropping on the anchor alone would silently un-cover a project header, which is the
    defect its own bullet below exists to close. The residual, recorded
    deliberately: a drive-relative path under no root is then neither keyed nor guarded, so a
    moved one would replay a stale depfile — reachable only when a build passes a
    drive-relative `-I` *and* the driver echoes it unresolved (`cl` resolves through the
    filesystem; clang-cl echoes what it was handed), and closing it would mean recording the
    producing machine's per-drive cwd in the value, which is exactly the machine-specific
    state the key exists to keep out. One diagnostic consequence: such a path dropping out of
    the key makes the launcher's `dependency set: 0 of M reported path(s) keyed` line
    reachable for a second reason, so that fingerprint no longer identifies the #66 short-name
    mismatch on its own — the two are told apart by whether the *root* is short-name spelled,
    and separating them in the counter itself is left as the follow-up it is.
    - **The ASCII rules are one rule each, and the drive-letter one had drifted into four.**
      Two of the four spellings tested it with `std::isalpha`, which is **locale-dependent** — in a codebase
      whose whole premise is that two machines derive the same key from the same content, a
      classification that varies with the running process's locale is a classification they
      can disagree about. `PathCanon::IsDriveLetter` is now the single definition and every
      site takes the letter test from it. What each site adds on top deliberately differs and
      that is not drift: `AnchorForLayout` and `IsWindowsRoot` also ask what *follows* the
      colon, while the two depfile rule-splitters do not — they are deciding where a rule
      ends, and `C:foo` is one token there however it is anchored. The one place the two
      root-shaped tests genuinely part is a bare `C:`: as a layout **root** it is the
      degenerate spelling of the drive root and `IsWindowsRoot` accepts it, while as a
      **path** the same bytes name the drive's current directory and `AnchorForLayout` calls
      it `DriveRelative`. Both halves are pinned against the *same layout* in one test, so
      the shared helpers cannot quietly merge them in either direction. The same reasoning
      moved `PathCanon::AsciiLower` into the header beside it: `IsToolchainHeader`'s
      comparison form was folding case through `std::tolower`, and under a Turkish locale
      `std::tolower('I')` is not `i` — so a root spelled `D:\PROJECT\Inc` folds one way on
      one machine and another way on the next, and the two derive different manifests and
      different dependency sets from byte-identical content. A locale-sensitive
      classification is the same defect as a host-sensitive one, and this layer exists to
      have neither.
  - **A stream driver's notes must not reach the hashed text, and which stream carries them
    is not the driver table's answer to give.** `DriverSpec::includeStream` describes the
    *compile* run; the probe is a different command line and clang moves the notes off
    whichever stream the preprocessed text is using — measured, `clang-cl /c /showIncludes`
    reports on **stdout** while `clang-cl /EP /showIncludes` reports on **stderr** (LLVM
    D46394). Routing the probe by that table therefore read an empty set on clang-cl and
    made this whole key input a silent no-op there. So `Preprocess` guesses at nothing: it
    splits stdout unconditionally (a byte-exact no-op on a stream with no notes) and unions
    the notes from both — which is the treatment `RecordManifest` already gives the same
    question, "rather than guessing which compiler produced this value". A note left in the
    text would be keyed as if it were source, and it carries an absolute path, which is
    precisely what suppressing line markers exists to prevent.
  - **The note grammar is one rule, not one string, and it is anchored.** `SplitIncludeNotes`
    and `ParseIncludePaths` both call `IncludeNotePath`, which matches after leading blanks
    (`cl` indents by nesting depth) and **nowhere else**. Matching the marker anywhere in the
    line is safe on a pure note stream and a mis-serve on the one that also carries
    preprocessed *source*: it deletes an ordinary line that merely quotes the marker out of
    the hashed bytes, so two revisions differing only in that string literal key identically
    and the second is served the first's object. This repository's own sources contain the
    literal, so it was reachable while building `fastcached` itself.
  - **A manifest that names no dependency is refused, not recorded.** A direct hit
    revalidates exactly what its manifest lists, so a manifest built from the source alone
    replays an object whose headers nobody re-checked — edit a header, leave the `.cpp`
    untouched, and the stale object is served forever with a zero exit code. That is what a
    build passing neither `-MD`/`-MF` nor `/showIncludes` produces: no stream carries notes
    and there is no depfile to read. `RecordManifest` returns instead, which costs that build
    direct mode (a permanent manifest miss, resolved by the ordinary preprocessed key) and is
    the one shape where recording nothing is strictly better than recording something. The
    launcher never injects those flags itself — the compile runs the build system's own argv,
    so what it can revalidate is bounded by what the build asked the compiler to report.
  - **A manifest classifies a path's anchor before asking whether it is toolchain content,
    and the working directory is what makes that answerable.** `IsToolchainHeader` reports
    every path outside both roots as toolchain, and a *relative* path lies under no root, so
    asking it first reported every relative path as toolchain and dropped it. A GNU build
    whose depfile carried relative header paths — a relative `-I`, or a compile run from the
    source directory, which is also how the CMake Ninja generator spells its sources —
    therefore recorded a manifest of its absolute entries alone, and an empty manifest
    validates against anything: edit a dropped header, leave the `.cpp` untouched, and the
    direct hit serves the previous object under a zero exit code, *permanently*, because a
    direct hit never reaches `RecordManifest` to repair the manifest that let it through.
    `Cc::IsCheckable` and `Cc::PortableForm` are the other two consumers of that classifier
    and both already ordered it this way, in as many words; this was the third and did not
    (issue #57 is the same defect reaching the TU source rather than a header). The
    classification is now three-valued — `Project` / `Toolchain` / `Unanchored` — for the same
    reason `PathCanon::Anchor` is, and built on it: "outside both roots" and "not placeable at
    all" are different facts with opposite consequences, and collapsing them to two is
    precisely what went wrong. `PathRole` is not a second spelling of `Anchor` — `Anchor`
    describes the path, `PathRole` decides what the manifest does about it — and the mapping
    between them is the switch, which carries no `default:` for the reason the other two
    callers' do not.
    - **A resolved relative path is recorded as a canonical token, not kept relative** —
      the opposite of what `KeyDependencySet` does with the same input, deliberately. A key
      input is only ever *digested*; a manifest entry has to be *localized back to a file*
      on the validating machine, which a relative entry can only do against that machine's
      working directory. `cc -c ../src/t.cpp` run from `build/` and from `build/sub/`
      relativizes to the same argument list and so shares a manifest key while naming
      different files — resolving before canonicalizing is what removes that, and it is why
      `CanonicalSourceToken` is one function both `TryDirectMode` and `RecordManifest` call
      rather than two spellings of `PathCanon::Canonicalize(cmd.source, …)`.
    - **An unanchored path refuses the manifest rather than being dropped.** Reachable only
      when the working directory itself is unavailable, so it costs essentially nothing —
      and dropping is the silent stale serve this whole classification exists to prevent,
      while refusing merely costs one compile direct mode.
    - **`current_path()` must be re-spelled in the layout's vocabulary before anything
      compares it against a root.** `getcwd(3)` answers with the kernel's *resolved* path,
      so a build under a symlinked prefix (macOS `/tmp` → `/private/tmp`, any symlinked
      `/home` or `/mnt`) reports a working directory sharing no string prefix with the root
      it is actually inside — and every root test here and in `PathCanon` is a string prefix
      comparison. `AnchorWorkingDirectory` matches by filesystem *identity*
      (`weakly_canonical` on both sides, longest root first as `CanonicalizeOne` does) and
      hands back the **root's own spelling** with the tail appended, so the one value that
      comes from the environment speaks the same language as the ones that come from
      configuration. Found by `compile-cache-e2e.sh` on macOS, where `mktemp -d` returns a
      `/var/…` path and `getcwd` reports `/private/var/…`; it silently cost direct mode
      rather than failing, which is why it is asserted end-to-end. This is *not* a fix for
      issue #66 — that is about the paths a **driver emits**, where both sides would have to
      move together; here only the cwd is re-spelled, and against roots that are already the
      layout's own.
    - **The "normalize, then put the layout's separators back" rule is spelled once,
      in `NormalizeForLayout`.** `std::filesystem` answers with the **host's** preferred
      separator while every root and absoluteness test is the **layout's**, so on a
      Windows host `/w/src/a.hpp` comes back backslash-separated and
      `IsAbsoluteForLayout` — which for a POSIX layout asks only about a leading `/` —
      reads an absolute path as relative. `PortableForm` had the only copy, inline, and
      the manifest side turned out not to have it; Windows CI is what said so, on this
      very change. The correction runs **one way only, deliberately**: a Windows layout
      keeps whatever separators it arrived with, because `PathCanon` spells a Windows
      root either way (`C:/src/proj` is a Windows layout) and every prefix test unifies
      separators before comparing — only the POSIX direction can mislead, since there a
      backslash is an ordinary filename character rather than a separator spelled
      differently. `ResolveAgainst`'s *join* stays the host's path arithmetic on
      purpose, and its contract says so: it resolves against a directory on this machine
      and its result is handed straight to `HashFileContents`, so it only ever runs
      where the layout and the host agree.
    - **`manifest-v4` moved alone, and the lock-step with `objkey-v*` is one-way.** An
      `objkey` bump must drag the manifest tag with it (a manifest points at an object key by
      value); the reverse costs nothing, since an unreachable manifest is re-recorded next
      compile and points at the same still-valid objects. The bump is *required* here because
      the defect is invisible to the key: a build with an absolute TU source but relative
      header paths keeps the same `canonicalSource` and args across this fix, so its
      under-recorded manifest keeps the same key, keeps being found, and keeps validating.
      Re-keying is the only thing that retires those entries.
    - **The emptiness guard did not move to a count, and that was the decision.**
      `RecordManifest` still refuses on `includes.empty()` — "did the compile report a
      dependency record at all?", which is the right question and `includes` is the right
      thing to ask it of. A count over `manifest.entries` cannot replace it: after the fix a
      one-entry manifest is *legitimate* (a TU whose every header is toolchain content is
      completely covered by that entry plus the stamp), so a threshold would refuse correct
      manifests while still not catching a misconfigured root. The zero-entry case it was
      standing in for is closed a layer down instead — `BuildManifest` takes the TU as its
      own `sourcePath` field and refuses when it cannot record it, which turns the invariant
      its doc-comment used to *state* into one it *enforces*. What the caller adds is a
      diagnostic, not a veto: `manifest: N entries from M reported dependency path(s) plus
      the source`, for the same reason the key's `dependency set: N of M` line exists — the
      recorded manifest cannot report this itself, because it still validates.
  - **`Cc::MissingReplayedDependency` stays as the backstop**, and still runs before a hit
    writes anything; a stale hit falls through to the real compile, whose STORE repairs the
    entry. Its filter is load-bearing in both directions: probing a depfile's rule target
    would make every hit a miss, because the target is the object file and it does not exist
    yet (hence `ParseDepFilePaths`, which excludes it, rather than a whitespace split);
    probing an absolute path outside both roots would make two machines with different system
    include prefixes miss on *every* compile forever, each re-storing the other's record.
    `/showIncludes` is covered alongside the depfile because Ninja reads it as `deps = msvc`;
    `MsvcDiagnostics` is not, because a diagnostic quotes a path rather than declaring a
    dependency on it.
  - **The residual, recorded deliberately:** two machines whose compilers print the *same*
    `--version` banner from *different* install prefixes still share a key and can still
    replay each other's toolchain paths. Closing that would mean hashing those absolute
    paths, i.e. giving up cross-machine sharing wholesale for the population it affects, so
    the guard above is what covers it and the trade is left where `DirectManifest` already
    put it.
  - **A root must be spelled the way the driver spells what it emits, and on Windows the
    drivers disagree.** Every root test is a string prefix comparison
    (`IsToolchainHeader`, `PathCanon::CanonicalizeOne`), so a root carrying an 8.3 short
    component matches nothing `cl` reports: `cl` resolves an include through the filesystem
    and prints `C:\Users\runneradmin\...`, while clang-cl echoes the spelling it was handed
    and prints `C:\Users\RUNNER~1\...`. Measured on a GitHub runner, where `%TEMP%` is the
    short form. Two failures follow from the one mismatch and they hide each other: every
    path is classified as outside both roots, so the keyed dependency set is **empty** (the
    two layouts of a moved header key together) *and* `ReplayGuard` skips every path it
    would have checked (so nothing reports it) — and the stored `/showIncludes` region is
    never canonicalized either, so the value carries the producing machine's absolute paths.
    Two symptoms to recognise it by, since neither the key nor the guard will say a word:
    a replayed note that kept the driver's mixed separators (`...\src\inc/h1.h`) was never
    tokenized, where a localized one is uniformly native (`...\src\inc\h1.h`); and the
    launcher's `dependency set: N of M reported path(s) keyed` line reads `0 of M` with M
    non-zero — the probe reported paths and every one was filtered out, which is a
    different fault from `0 of 0` (a driver that reports nothing on the preprocess line).
    `run-launcher-e2e.ps1` therefore puts its scratch trees beside the **build tree**
    rather than under `%TEMP%`; it does not try to expand a short name, because nothing
    dependably does — `Resolve-Path`, `Get-Item` and `[IO.Path]::GetFullPath` all preserve
    it, and `Scripting.FileSystemObject` was tried and echoed it back unchanged.
  - **The reconciliation translates the emitted paths INTO the build's spelling; it does
    not respell the roots (issue #66).** There are two spellings of every root and the
    launcher needs both, for opposite reasons. **Matching** must use the spelling the
    filesystem reports, because that is what a driver reports. **Emitting** must use the
    spelling the build system uses, because a replayed depfile's rule target has to be
    byte-identical to the `-o` path the build passed. Resolving the roots and using that
    form everywhere satisfies the first and breaks the second — measured: a build tree
    reached through a symlink stored `.../link/build/a.o:` and replayed
    `.../real/build/a.o:`, which Ninja rejects outright (`expected depfile ... to mention
    ...`) while make matches no rule at all and silently drops every header dependency.
    `RootReconciler` (`apps/fastcache-cc/RootReconciler.cpp`) holds both layouts and is the only
    thing that sees the resolved one: it canonicalizes an emitted path against the
    resolved roots and localizes the token into the as-given roots. Everything downstream
    — the roots on the wire, the key, the manifest, the replay guard, the localized
    regions — keeps speaking the build system's own spelling exactly as before, so no
    protocol version moves and nothing else had to learn about this. Consequences that
    are each load-bearing:
    - **It is its own translation unit, for the reason `CacheProtocol.cpp` already
      records.** `main.cpp` is in no test target, so logic that lives there has no unit
      coverage at all — the mistake this list notes having been made once with the wire
      framing. `RootReconciler.cpp` is compiled into both the launcher and
      `fastcache-cc-tests`, and its tests drive it through a table-backed fake
      `IPathResolver`: the conditions it exists for (an 8.3 short component, a `subst`
      drive, a junction) cannot be created on the host running the tests, and two of the
      three cannot be created on any host that is not Windows, so a fake stating the
      aliasing directly is what makes every case reproducible everywhere.
    - **The translation is PathCanon's own two operations, not a third prefix test.**
      `Canonicalize` against one layout, `Localize` into the other. A rule written out
      again here is a rule that can come to disagree with the one everything else
      applies, which is the whole failure mode this entry documents.
    - **Only paths the COMPILER authored are reconciled; one the BUILD SYSTEM authored is
      already the spelling this build wants, and it is named BY VALUE.** A depfile is the
      single grammar carrying both, so `Cc::RootReconciler::Region` takes the object path and
      returns that span verbatim wherever it appears. Respelling it hands the build system
      back an output it never asked for, and a build whose `-o` does not share a spelling
      with `FASTCACHE_BINARY_DIR` then gets a depfile Ninja rejects outright and make
      matches against no rule at all. **By value and not by position**, because position
      does not say what a path is: `-MP` emits a phony rule per header whose TARGET is a
      path the compiler reported, and exempting every target would leave those unreconciled
      and so uncanonicalized, sending a consumer a depfile that points `-MP`'s
      deleted-header protection at files it cannot stat. Canonicalization on the daemon
      still rewrites every span, target included — a consumer needs the target pointing
      into ITS build tree.
    - **Symmetry is the property, and it is why this is a seam rather than a call at
      each comparison.** Expanding only the emitted paths breaks clang-cl exactly as
      spelling only the root long breaks `cl`. `Cc::IPathResolver`
      (`apps/fastcache-cc/PathResolve.hpp`) is where the filesystem lives, and it lives
      in the launcher because `PathCanon` also runs on the **daemon**, over a producing
      machine's roots that do not exist there (`CompileCacheHandler::HandleStore`), so it
      may never touch a filesystem. Hence `PathCanon::RewritePaths` taking the transform
      as a parameter: the grammar that finds path spans stays in the library.
    - **A path already spelled the way this build spells things is returned UNTOUCHED, and
      that is the correctness case rather than an optimization.** Resolution rewrites a
      symlink anywhere in a path, not only in the root prefix, so round-tripping an
      in-tree one (`src/inc -> src/real-inc`) would key it under this machine's real
      subpath while a machine holding the same content without that symlink keys under the
      plain one. Two byte-identical checkouts would stop sharing every entry — the property
      the launcher exists to provide, traded away to repair a spelling that was never wrong
      here. So `Translate` asks the as-given layout FIRST and only falls through to the
      resolved round trip when that fails. Measured: two such checkouts key identically,
      and at the same key a launcher built before this change produces.
    - **That fast path is also why it costs nothing, and the memo is what bounds the rest.**
      The resolver memoizes per parent DIRECTORY. Measured on this repository's own
      `CompileCacheHandler.cpp`: 1099 reported paths, 60 filesystem calls (all of them
      toolchain headers, which lie under no root by either spelling), and 271-276 ms per
      cache hit against 271-276 ms before — inside the noise. Per-path resolution with
      neither fast path nor memo is the version the issue worried about: roughly 1099 calls,
      ~25 ms, which on a 45 ms preprocess is not a rounding error.
    - **`Resolve` leaves the leaf as spelled, so an argument naming a directory must use
      `ResolveDirectory`.** The memo works per parent, so the final component of whatever
      is handed to `Resolve` is never resolved — fine for an include note (the name comes
      from an `#include` directive and is already long) and wrong for an `-I` pointing at
      a symlinked include directory, whose *own* last component is the aliased one. The
      argument list and the translation unit therefore go through `ResolveDirectory`,
      which also makes an `-I` resolve the same way the headers reported from under it
      do; there are few enough arguments that resolving each completely costs nothing.
    - **A relative path is returned verbatim.** It resolves against the compile's working
      directory and is therefore already machine-independent; absolutizing it would either
      re-key it for nothing or, when the working directory lies under neither root, push it
      outside both and have `KeyDependencySet` drop it. `RootReconciler::IsInTree` has to
      agree, and it asks its question of the RECONCILED path against the AS-GIVEN layout —
      the same two values that decide whether the path reaches the key. Asking the resolved
      layout instead answers a question nothing else asks, and a compile could then key
      nothing while the diagnostic reported it in-tree.
    - **A trailing separator is trimmed off each root, and leaving it is worse than a
      no-op.** `PathCanon::Layout` takes roots without one — `IsSegmentPrefix` requires a
      separator AFTER the root, so `/x/build/` matches nothing under `/x/build` — and a
      build system exporting one is doing nothing wrong. Untrimmed, nothing under that root
      canonicalizes (so the stored value keeps this machine's absolute paths) AND the path
      then gets a second chance through the resolved root, which `weakly_canonical` returns
      without the separator, after which `JoinLocalized` adds one of its own and the
      replayed rule target reads `/x/build//a.o`. A bare root (`/`, `C:\`) is left alone:
      it IS its trailing separator, and trimming `C:\` to `C:` would also flip the
      separator style `JoinLocalized` derives from it.
    - **The "never throws" contract is guarded at the entry points, not around the
      filesystem calls.** Every `std::filesystem` call here takes an `error_code`, so
      guarding only those looks sufficient — but on Windows `std::filesystem::path`
      *stores* a `wstring`, so constructing one from a narrow string converts through the
      active code page and `path::string()` converts back, either of which throws on a
      character the code page cannot represent. Those conversions happen before any step
      runs. `main()` has no catch of its own, so an escape breaks the build over a path
      the launcher merely failed to tidy up.
    - **No schema tag moved, and the reconciliation is why it did not have to.** Because it
      is identity wherever the spellings already agreed, `objkey-v3`/`manifest-v3` and
      `CompileValueVersion` all stay: verified, a launcher built from the previous commit
      stores entries this one HITs, and a manifest it recorded still direct-hits. Where the
      spellings did NOT agree the cache was not working, so there is nothing to invalidate.
    - **The diagnostic is verbose-gated, which reverses the original call.** It started
      ungated, on the reasoning that this defect is invisible and a diagnostic nobody
      enables is as silent as none. Two rounds of narrowing failed to find a condition
      that means "broken" reliably enough to justify four unsilenceable lines on the
      compiler's stderr for every translation unit: a source outside both roots is an
      ordinary CMake layout (`add_subdirectory(../shared shared)`, a superbuild,
      ExternalProject), and a message that cries wolf on a healthy build is the one that
      gets ignored when it is right. What tipped it is that `RootReconciler` now REPAIRS
      the mismatch rather than merely detecting it, so this is a backstop and not the
      mechanism.
      **The condition stayed narrow even so**, and reports the roots not containing the
      SOURCE rather than "nothing was keyed", because the broader one has an innocent
      reading: `/showIncludes` never names the primary source, so on MSVC a translation
      unit including only third-party headers outside the roots — Qt, a vendored SDK,
      anything the four-entry `ToolchainMarkers` table does not know about — reports paths
      and keys none of them while being perfectly healthy. `0 of 0` stays quiet too: that
      is a driver reporting nothing on the preprocess line, a different fault this message
      would misdescribe.
    - **The e2e cases create the second spelling deliberately** rather than relying on one:
      `compile-cache-e2e.sh` symlinks a root, `run-launcher-e2e.ps1` substitutes a drive
      (`subst`, not an 8.3 name — 8.3 creation is off on many volumes). Both compile one
      tree through both spellings and require the second to HIT the first's entry.
      **Which paths get which spelling is the whole design of the POSIX case**, and getting
      it wrong makes the case vacuous: the roots and the OUTPUT paths are the aliased
      spelling while the source and include paths are the real one, which is what `cl`
      actually does — a build system spells everything one way and the compiler reports its
      dependencies resolved the other way. Spelling the outputs the same way as the roots is
      what a real build does (`-o` and `FASTCACHE_BINARY_DIR` come from one generator) and
      it is what makes the second property testable at all: the replayed depfile's rule
      target must be byte-identical to the one the compiler wrote. A third leg repeats it
      with roots carrying a trailing separator. Both assertions were verified by
      reintroducing the bug they catch and watching them fail, and the whole case is checked
      against a launcher built from the previous commit, where it must fail with
      `dependency set: 0 of 2`. The Windows case names its object by the real path in both
      legs. That was load-bearing when it was written, because a fused `/Fo<path>` reached
      the key verbatim and would have keyed the legs apart for an unrelated reason; the
      bullet below has since made every path-valued flag relativize in its fused spelling
      too, so the precaution is now belt-and-braces rather than the thing holding the case
      up. Every other case keeps its roots unambiguous,
      so a regression in the reconciliation cannot present as a failure of something else.
- **A flag's value is relativized off one table, or it is relativized in one spelling
  only.** A path-valued flag can be written two ways — `/Fo <path>` and `/Fo<path>` —
  and the separated form needs no table at all: the value is a bare argument, so it
  reaches the source-path branch on its own. Only the *fused* form has to be split, and
  the table that split it listed the include-dir prefixes and nothing else. So the object
  output was relativized in the spelling nobody uses and left absolute in the spelling
  **every** build system driving MSVC writes, which put the producing machine's object
  path into every Windows key: two checkouts at different roots could never share an
  entry, and the launcher's whole reason for existing was off on that platform. It passed
  unnoticed because the unit test asserting `/`-still-introduces-an-option happened to use
  the separated form, and because the Windows cross-depth e2e case was hitting an entry an
  earlier case had stored from the same directory with the same `/Fo` path — a spurious
  pass that would have survived cross-depth sharing being broken outright (fixed by giving
  each case its own string literal, the device `check_header_move` already used). The three
  tables are now one, `CmdLine`'s `PathValueFlags()`: it answers whether a bare occurrence
  consumes the next argument, which flag names the object output, which flags the preprocess
  line must drop, and whose fused value the key relativizes. Consequences that are each
  load-bearing: a row carries a **driver family**, because the family is *not* derivable
  from the introducer — MSVC drivers accept `-` for every option, and `-MT` names a
  dependency target for a GNU driver while selecting the static multithreaded runtime for
  an MSVC one, so a row matched on `-` alone would make `cl -MT` swallow the source file.
  Which *introducers* may match is still decided by the **layout**, not the host and not
  the driver, for the reason recorded above: on POSIX a leading `/` starts an absolute
  path, and matching `/I` there splits a checkout rooted at `/Infra`. And the drop list no
  longer spells `/Fo` or `-MF` itself — it drops every row whose role is not `IncludeDir`,
  so a spelling added to the table is dropped by construction rather than by someone
  remembering the fourth place.
  - **This did not bump `objkey-*` / `manifest-*`, and that is the deliberate half.**
    (The tags now read `v4`, moved by issue #64's manifest half — a different change, for
    a reason this one does not reach; the re-key here rides along in that invalidation
    event rather than costing a second. The reasoning below is kept because it is the case
    for *not* bumping, and it stands on its own.)
    The tag versions the key *construction* and the rules the stored value is written
    under; both are unmoved by this change, and `ComputeKey`'s golden vector did not move
    for it either. What changed is one *input*, for exactly the builds whose command line
    carried a machine-specific string it should never have carried. Old entries stay
    correct in their own terms and simply stop being addressed — they miss and are
    rewritten. The mis-serve a tag exists
    to prevent is unreachable here: an old key could only become a new key if a build
    literally passed the text `<BUILDTREE>`, and a `/Fo` path that was already relative
    canonicalizes to itself and does not move at all. A bump would meanwhile invalidate
    every POSIX entry, where nothing changed. Direct mode needs no bump either, and not by
    luck: `ComputeManifestKey` takes the relativized args too, so a manifest key moves
    exactly where an object key does, in lock-step, for exactly the affected builds — the
    property whose *absence* is what forced the `manifest-v2`/`v3` bumps, and whose
    presence is why `manifest-v4` had to be argued from the manifest side rather than
    from this one.
- **A key that is 128 bits wide is not a key with 128 bits of strength, and four
  lanes of one polynomial are one lane.** The object key was four CRC32C digests of
  the same blob, distinguished only by a leading salt byte. CRC is affine over
  GF(2), so with `A` the per-byte state-update operator and `S_i` the state after
  salt `i`, `quarter_i XOR quarter_j` is `A^len(blob) * (S_i XOR S_j)` — a value
  that depends on the blob's *length* and on nothing else about it. Matching one
  quarter therefore forced all four and the key carried **32 bits**. Measured
  before the fix: one distinct XOR value across 2000 random equal-length 512-byte
  blobs, and a full 32-hex-char collision after 86,125 equal-length inputs — the
  birthday bound for 32 bits, against the ~10^5 entries a shared team cache
  reaches. Equal length is not an exotic condition, it is the ordinary shape of a
  source edit (`return 1;` → `return 2;`) and the blob is dominated by preprocessed
  text; and the consequence is not a miss but an unrelated translation unit's object
  file served under a **zero exit code**. Two dead ends worth not re-walking:
  varying the salt *length* per lane does not help (still one distinct XOR value —
  the salt only changes `S_i`), and four CRC lanes with *distinct* polynomials do
  work but cap out below 128: three of the four best-studied CRC-32 polynomials
  (Castagnoli, Koopman, Koopman-K/2) carry the factor `(x+1)` and only IEEE 802.3
  does not, so the least common multiple of the four has degree 126, not 128. The
  digest is now MurmurHash3 x64_128 in `Core/MurmurHash3.hpp` — an existing,
  published algorithm rather than a bespoke construction, which is the whole point: its conformance is checkable
  against SMHasher's verification value `0x6384BA69`, and a construction assembled
  here would have nothing to be checked against. Consequences that are each
  load-bearing: `objkey-v3` and `manifest-v3` moved together (to v3) because that was one
  invalidation event, and `HashFileContents` moved with them — it paired one CRC32C
  with the byte count, which is 32 bits against exactly the same-length case, and it
  is what a *direct hit* revalidates against, so a collision there does not miss, it
  decides an edited header is unchanged. `header-state-v1` deliberately did **not**
  move, because nothing is stored under it and a version with no work to do is the
  mistake `PathCanon::CanonError` already records. Domain separation between the
  three key spaces is now the leading schema tag rather than the salts, and each piece
  of a key is **length-prefixed** (`kind`, big-endian `u64` length, bytes) rather than
  terminated by a separator byte. That second half is not cosmetic, and it was found
  in review of this very change: terminating a value with a byte that can occur
  *inside* a value is not a framing at all, so `{compilerId="cc\0d", preprocessed="x"}`
  and `{compilerId="cc", preprocessed="d\0x"}` digested identically — the same silent
  cross-TU mis-serve, reached by a different route, and reachable rather than
  theoretical because preprocessed text can carry a raw NUL and a build system can
  pass an argument containing `0x01`. Fixed here rather than filed for the reason
  `HashFileContents` was: `v3` re-keys the whole cache once, so finding it later would
  have cost a `v4` invalidation for a defect of the class this change exists to close.
  The length must be big-endian for the same reason the digest's block loads must be
  little-endian — a *host*-order length makes the key differ between machines.
  The residual, recorded deliberately: MurmurHash3 is not collision-resistant against
  an **adversary**, and that is accepted because the key is not a security boundary —
  anyone who can STORE can already write a wrong object under a correct key. Closing
  it would mean a keyed or cryptographic hash *and* a trust model for STORE, which is
  a different change from this one.
  - **The digest must be bit-identical on every machine that shares the cache, and
    the `x64` in its name is a variant, not a target.** MurmurHash3 defines `x86_128`
    (four 32-bit lanes) and `x64_128` (two 64-bit lanes); they produce *different*
    digests, so the variant is part of the format. Nothing in the implementation is
    architecture-specific, and four hazards that would have made it so are closed by
    construction: `char` is signed on x86-64 Linux and **unsigned on aarch64**, so a
    byte widened through a plain `char` sign-extends differently and would have split
    Apple Silicon from everything else — everything is `std::byte`/`std::uint8_t`;
    block loads go through `ReadLittleEndian` rather than a native-order read; that
    same read would be unaligned and therefore UB whatever the hardware tolerates,
    which is also what `-fsanitize=alignment` traps under `clang-debug`; and rotation
    uses `std::rotl` rather than a shift pair that is UB at zero. A divergence here
    would not fail, it would silently split the cache in two with every machine
    missing on every entry the others wrote — so the SMHasher vector is checked on
    the arm64 `macos-14` job as well as x86-64 Linux and Windows, and it sweeps all
    256 tail lengths, which is where such a slip surfaces first.

- **A cache that can fail a build is not optional, and a refusal nobody can read
  is not a refusal.** The launcher STOREs an object by streaming one frame, and the
  daemon refused an over-cap frame by replying and then closing — while the sender
  was still writing. Two independent defects met there. Nothing in `fastcache-cc`
  suppressed SIGPIPE (the daemon does, in `Net/BlockingSocket`, but the launcher
  deliberately does not link the library), so the write that met the closed peer
  **killed the launcher with signal 13**; the build system saw a compile die of a
  signal even though the object file it asked for was already complete and correct
  on disk, and no retry could ever converge because the outcome was deterministic.
  Measured on a 356 MB object (C++23 templates plus `-g`) against the 256 MiB
  default cap, and reproduced here at 80 MB against the 64 MiB floor
  `SessionContext` keeps regardless of `--storage-max-value`. The launcher had
  correct fall-back logic for every store failure and reached none of it: a signal
  is not a return value. Three consequences, each load-bearing and each at a
  different layer, because any one alone leaves a hole the others do not cover:
  - **Suppression is per socket, not process-wide.** `SO_NOSIGPIPE` on macOS/BSD,
    `MSG_NOSIGNAL` on Linux, with `::signal(SIGPIPE, SIG_IGN)` only as a last
    resort for a platform with neither. The daemon can take the process-wide
    ignore; the launcher cannot, because an ignored disposition is **inherited
    across exec** and the launcher spawns the preprocessor and the real compiler —
    a process-wide ignore here silently changes how every compiler it fronts
    behaves. On macOS the socket option is also preferred over the newer
    `MSG_NOSIGNAL` *even though the SDK declares it*: the macro comes from the SDK
    while `CMAKE_OSX_DEPLOYMENT_TARGET` lets the binary run on an older kernel,
    and a flag the kernel does not know fails the send rather than the signal.
  - **The daemon drains an over-cap frame before refusing it**, exactly as it
    already does for an unknown opcode, so the sender's write completes and it
    can read the typed `payload-too-large` naming *both* numbers — the one message
    that tells an operator which way to move `--storage-max-value`. This is the
    "a rejection can be a reply instead of a close" rule two bullets up, applied to
    the one path that was still closing. `ByteReader::Skip` discards in chunks, so
    the memory the cap protects is still never taken. Bounded at a small multiple
    of the cap rather than by a knob of its own, because the cap is already the
    operator's statement of the largest thing this server will handle.
  - **The launcher declines the store before connecting**, at
    `FASTCACHE_MAX_STORE_BYTES` (256 MiB by default, `0` disables). Surviving the
    refusal is not enough: without a ceiling every rebuild of that translation
    unit pays the full transfer to be told no, and one 356 MB entry would dominate
    a cache sized for thousands of ordinary objects. The default matching the
    daemon's `--storage-max-value` default is a *chosen coincidence*, not a
    negotiation — there is deliberately no handshake, so raising one and not the
    other leaves the other refusing.

  The residual, recorded deliberately: the 64 MiB floor in `SessionContext` means
  a cap *below* it cannot be tested end-to-end without a genuinely large fixture,
  so the e2e drives the client ceiling and `TcpClient_test` pins the socket half
  (its `HangUpPeer` case terminated the test binary with signal 13 before the fix —
  verified by re-neutering it, since a regression test for a fatal signal that
  cannot be seen to fail is worth nothing). Separately, `--storage-max-value`'s
  help text still said `default 16m` long after the default became 256 MiB: the
  one knob this failure sends an operator to, misreporting itself.

- **The wire's two grammars are shared, and both live in `Core/` for the same
  reason.** `Core/WireFields` is the payload — a run of `[u32 length][bytes]` —
  and `Core/WireFrame` is the seven bytes in front of it:
  `[magic][version][kind][u32 payloadLength]`. `CompileCacheWire` and `RaftWire`
  had each spelled the second one out, the encoder character-identical and
  `IsSupported` identical outright, which is the drift `CompileCacheWire`'s own
  documentation warns about applied to the half a reader reaches first. What
  `WireFrame` deliberately does **not** decide is what any of it means: the magic
  is the caller's, so two protocols on two ports stay distinguishable; the kind
  byte comes back raw and is validated against the caller's own table, because a
  receiver has to be able to *step over* a frame whose kind it does not know; and
  the supported version range is a parameter, because the wires version
  independently and always will. `RaftWire::FrameHeader` is an alias of
  `WireFrame::Header` while `CompileCacheWire::RequestHeader` is rebuilt from it —
  not an inconsistency but the cost of a rename: that struct's field is spelled
  `opRaw` at some seventy call sites across the daemon, the launcher and the test
  client, and the thing that had to stop being duplicated was the *layout*.
- **`Protocol/CompileCacheWire.hpp` must stay header-only and dependency-free.**
  Same constraint as `Cli/UsageDoc`, same reason: `fastcache-cc` does not link
  the `FastCache` library, so an include of anything from `Net/`, `Cache/`,
  `Async/` or `Config/` there breaks the launcher's **link**, not merely its
  build. Being header-only is also what keeps it free — it costs no row in
  `_fc_cc_core`. The dependency runs *out* of `ProtocolAutodetect.hpp` (which
  pulls in `Task`, `CacheEngine` and `ISocket`, and so can never be included by a
  client) into the wire header, never the other way. The launcher's own framing
  lives in `apps/fastcache-cc/CacheProtocol.cpp` rather than `main.cpp` for a
  related reason: `main.cpp` is in no test target, so while the framing sat there
  it had *no* unit coverage at all.
- **A platform socket error is classified in one place.** `Detail::TranslateError`
  in `BlockingSocket.cpp` mapped ten conditions onto `NetErrorCode`;
  `BlockingConnector` then grew a three-condition copy of it, so `EACCES` — a
  firewall or a privileged port, the two most likely reasons an outbound
  connection is refused *administratively* — came back as an unclassified
  `SystemError`, and a caller matching on `PermissionDenied` never saw it. It is
  now `Detail::TranslateSocketError`, published from `BlockingSocket.hpp` and used
  by both; the connector's own contribution, `ENETUNREACH`/`WSAENETUNREACH`, moved
  into the table rather than being lost, beside `EHOSTUNREACH` because no route
  and no answer are the same fact to a caller: this endpoint is unreachable from
  here, and neither is retryable at this layer.
- **A failure is reported with its reason, and the reason has to be captured where
  it is still in scope.** `ReadWholeFile` in `FileRaftStorage` returned a `bool`,
  so every caller could say no more than `cannot read <path>` — the one thing the
  operator already knew. Each step there fails for a different and actionable
  cause (the path is a directory, the permissions are wrong, the file shrank
  under the read), `std::filesystem` reports through `error_code` and `fopen`
  through `errno`, and a caller handed a `bool` cannot recover either. It returns
  `std::expected<void, ConsensusError>` and translates both at the point of
  failure. A missing file stays *success with nothing read*: a store starting for
  the first time is the ordinary case, not a fault.
- **The supervisor's launch arguments must not pass `--daemon`.** The POSIX
  daemonize path double-forks and sends stdout/stderr to `/dev/null`, which
  silences journald; its pidfile is also written after both parents exit, racing
  `Type=forking`. launchd has the identical problem — it reaps the forked job
  instantly as "exited" — which is why `BuildServiceArgv` takes an
  `EmitDaemonFlag` rather than always emitting it.
- **A config the operator named is strict; one the daemon found is not.**
  Without `--config`, `DefaultConfigPath` walks a per-platform candidate table
  (user location before machine-wide) and takes the first entry that exists *and
  opens for reading* — `Config/DefaultConfigPath.cpp` is the single source of
  truth for that order, for what `--help` lists, and for where `--seed-config`
  writes. Readability, not mere existence, is the test: the macOS system config
  is mode `0640 root:_fastcached`, so a per-user agent has to fall through it
  rather than fail to start. A discovered file that is absent or unreadable is
  skipped silently; one that parses badly is still fatal, as is a missing file
  named with `--config`. The resolved path goes into a *local* in `main.cpp` and
  never back into `parsed->config` — that object is what `InstallService`
  registers, and a discovered path baked into `ProgramArguments` would outrank
  the file itself forever (see the next bullet) and make
  `InlineCredentialRejection` name a path nobody typed.
- **The lookup's two rules both turn on one question: is this process the
  machine-wide daemon, or somebody's own?** `probe.IsPrivilegedProcess()` (root;
  elevated administrator or LocalSystem on Windows, via `CheckTokenMembership`,
  which is false for the unelevated half of a split token) decides both halves,
  and neither is driven by the platform or by the row alone:
  - **Unprivileged runs skip every `ConfigScope::System` row.** The machine-wide
    file describes the system service, whose cache only the service account can
    write, so a `systemctl --user` instance that adopted its `storage_path:`
    would fail to open that directory and be respawned until the start limit
    tripped — the out-of-the-box restart loop the user unit's header says it
    exists to prevent. Readability is not enough of a filter here: the packaged
    `/etc/fastcached/fastcached.yaml` is `0644 root:root` and readable by all.
  - **Privileged runs trust-check *every* row, per-user ones included.** `$HOME`
    and `$XDG_CONFIG_HOME` are inputs an unprivileged account often controls and
    sudo does not always reset, so checking only `System` rows would leave
    `sudo -E fastcached` taking root's `storage_path:` from a file that account
    wrote. A path named with `--config` is never checked — that is the operator's
    assertion to make, and it is the escape hatch when a location is refused.
- **A machine-wide config is only obeyed when only an administrator could have
  written it.** `C:\ProgramData` grants `BUILTIN\Users` create-file on every
  subdirectory it hands down to, so moving the Windows config there made the
  configuration of a *LocalSystem* service plantable by any standard account —
  `storage_path:` alone turns that into arbitrary directory creation and file
  writes as SYSTEM. Two halves, and both are needed: the MSI owns
  `%ProgramData%\fastcached` and gives it a protected access list of its own
  (`PermissionEx`, not `Permission` or `util:PermissionEx` — those take an
  account *name* and are wrong on a non-English Windows), and `DefaultConfigPath`
  refuses a candidate whose directory fails `Platform/FileTrust`. The test there
  is the containing directory's owner and entries, never the file's owner: on
  Windows a new object belongs to its *creator*, so a config seeded by hand from
  an elevated shell is owned by that administrator's own account, while a file
  planted by a standard account is granted to that account's own SID through the
  inherited `CREATOR OWNER` entry — an owner whitelist rejects the first and an
  entry scan misses the second. A rejection goes to stderr *and* through the
  logger once there is one: a service started by the SCM has no console, so
  stderr alone would put the message nowhere in the one deployment where a
  machine-wide config is the norm, and a file that is present, readable and
  ignored anyway is the silent no-op this list exists to prevent.
- **`--seed-config` secures the directory before it looks for the file, not
  after.** The whole point of the repair is the case where something is *already*
  there: any standard account can create a `%ProgramData%` subdirectory and drop
  a config into it long before the installer runs, and the MSI cannot undo that
  on its own — `PermissionEx` replaces the access list but not the owner, who
  keeps `WRITE_DAC` regardless. So seeding secures the parent first (repairing a
  squat), then decides what to do about the file: seed-once keeps an operator's
  edits, but a file found in a directory that until that moment anybody could
  write is not established to be an operator's, and is reported rather than
  blessed by silence or destroyed by overwriting. Seeding refuses outright when
  it has the rights for none of this, and deletes a directory it created but
  could not secure — which would otherwise be the very shape being defended
  against, authored by the defence.
- **`ExecStart` still passes `--config` on Linux and macOS — by choice, not
  necessity.** It predates the lookup, where its absence made `ConfigReloader`
  have nothing to re-read and `systemctl reload` a silent no-op; the lookup now
  closes that hole for every daemon started without the flag. The packaged units
  keep it because the path is unambiguous there and CI asserts it. Windows goes
  the other way: its custom action registers *no* `--config`, so a seed that did
  not happen degrades to built-in defaults instead of a service that fails at
  every start. The per-user launchd agent and the systemd user unit pass none —
  the packaged config describes the system daemon, whose cache only the service
  account can write.
- **`--install-service` registers the *command-line* config, not the merged
  one.** A flag in `ProgramArguments` outranks the same key in YAML for the life
  of the registration, so baking merged values in froze every configured key at
  install time and made later edits to that same file silent no-ops — and copied
  `requirepass:` out of a mode-0640 file into a world-readable plist. Hence
  `main.cpp` hands `parsed->config` to `InstallService`, and
  `InlineCredentialRejection` refuses a `--requirepass` typed on the install
  command line rather than dropping or publishing it — *including* alongside
  `--config`, since nothing there can tell whether the named file carries the
  secret, and accepting it was the silent drop under another name.
- **What reaches a supervisor must survive its own parser.** Every registration
  flag is re-read by the daemon at the next start, so a value that cannot be
  spelled back is a service that registers cleanly and then fails forever:
  `--listen=[::]:11211` came back as `--listen=:::11211`, which the CLI rejects,
  and a Windows path ending in `\` escaped its own closing quote and swallowed
  the flags after it. `FormatListenHost` and `MaybeQuote` are where that round
  trip is kept honest. `ServiceNameRejection` covers the other direction: the
  name is concatenated into the directory launchd scans, so a separator writes a
  root-owned plist nothing knows how to remove.
- **Teardown must address every domain, not re-probe for one.** Which launchd
  domain a user agent lives in is decided at install time — `gui/<uid>` needs an
  Aqua session, so an SSH install lands in `user/<uid>`. Re-probing at uninstall
  booted out a job that was never there and reported success while the real one
  kept the port. `BootOutEverywhere` walks the whole `ScopeTraits::domains` row,
  and `fastcached-uninstall` mirrors it.
- **The package payload is rooted at `/`, not `/usr`.** `/etc` cannot sit under
  a `/usr` prefix, so `FASTCACHED_INSTALL_BINDIR`/`DOCDIR` spell their own
  `usr/` (and `opt/fastcached/` on macOS). A relative destination for the units
  would put them where systemd never looks — and on macOS an *absolute*
  `install(DESTINATION)` escapes CPack's staging tree and writes to the build
  host's real filesystem.
- **Neither a macOS `.pkg` nor an MSI has a conffile mechanism.** Both overwrite
  their payload on every install, so on both the live `fastcached.yaml` is
  deliberately not payload: only a `.default` template ships, and it is copied to
  the real location exactly once, when nothing is there. macOS does this in the
  Runtime postinstall (`seed-config.sh.inc`), Windows in a custom action running
  the daemon's own `--seed-config` — which takes its destination from
  `SystemConfigPath`, so the seeded file and the startup lookup cannot disagree.
  Only the DEB conffile and the RPM `%config(noreplace)` can ship the live file
  directly. Uninstalling leaves the config behind on every platform.
- **An HTML installer pane must begin with its doctype.** Installer.app decides
  HTML from plain text by sniffing the first bytes of the resource, so the
  `<!-- SPDX-License-Identifier -->` header that opens every other file in the
  tree made the welcome and read-me panes render with every tag visible — the
  `.txt` license pane looking right is what disguised it. `mime-type="text/html"`
  on the Distribution XML element does *not* override the sniff (tried, and the
  panes stayed raw), so the doctype goes first and the licence comment after it.
  The same files need an explicit `<meta charset="utf-8">`: without it the em
  dashes arrive as mojibake, a defect the raw markup was hiding.
- **Third-party `install()` rules must be excluded.** A CPM-fetched zstd brings
  its own, and with the payload rooted at `/` they put `zstd.h` and `libzstd.a`
  into `/include` and `/lib` on the user's machine. Hence `EXCLUDE_FROM_ALL`.
- **macOS binaries must link nothing outside `/usr/lib`.** `CPM_USE_LOCAL_PACKAGES`
  defaults ON and makes CPM prefer Homebrew's shared yaml-cpp, so the package job
  passes `-DCPM_USE_LOCAL_PACKAGES=OFF -DOPENSSL_USE_STATIC_LIBS=ON` and CI
  asserts the result with `otool -L`. `CMAKE_OSX_DEPLOYMENT_TARGET` is pinned to
  13.3 (the floor at which the system libc++ has floating-point `std::to_chars`,
  which `std::format` needs) and must be set *before* `project()`.
- **The git tag is the only version source, and `version.txt` must never come
  back.** There used to be a committed `version.txt`, and because
  `cmake/Version.cmake` read it *first* it was the real source of truth: a second
  version carrier that each release had to remember to bump in lock-step with the
  tag, and that pinned every build, every wire banner and every package to `0.0.1`
  for as long as it existed. `ctest -R repository-hygiene` now fails if it — or any
  other row in `scripts/check-repository-hygiene.cmake`'s table — is ever *tracked*
  again. The test asks the git **index**, not the filesystem, so it fails at
  `git add` time rather than after a commit, and an untracked local `version.txt`
  stays legitimate (it is in `.gitignore` and is read by nothing). A build that
  cannot reach a tag states its version with `-DFASTCACHED_VERSION=1.2.3`.
- **The resolved version triple must stay a bare numeric `X.Y.Z`, and the fallback
  with it.** `CMakeLists.txt` feeds it to `project(VERSION ...)`, which rejects
  anything else, and CPack carries it into the MSI `ProductVersion` (major/minor
  < 256, patch < 65536), the RPM `Version:` field (where `-` is illegal) and the
  Debian version (where `-` starts the package revision) — so `Version.cmake`
  validates the fields against a table and the *string*, never the triple, carries
  the `-12-gdeadbee`/`-dirty` suffixes. This is why the no-tag fallback is `0.0.0`
  and not `0.0.0-unknown`: the `docker` job takes that path on **every** ref
  regardless of clone depth, because `.dockerignore` excludes `.git/`, so a
  non-numeric fallback would turn every push red. It is also why the release
  trigger matches `v[0-9]+.[0-9]+.[0-9]+` rather than `v*` — a `v0.1.0-rc1` tag
  cannot configure, and failing to *start* costs nothing where fifteen red jobs and
  a burnt notarization slot would.
- **`cmake/portable/CompileCache.cmake` must stay stock-CMake-only, and must never fail a
  configure.** Same constraint as `Cli/UsageDoc` and `Protocol/CompileCacheWire`,
  for the same reason: the file is *meant* to be copied verbatim into other
  projects, so a dependency on anything else here breaks it where nobody in this
  repository would notice. It is also included at `CMakeLists.txt:164`, before CPM
  is bootstrapped at `:183`, so `CPMAddPackage`/`FetchContent` are not available to
  it even locally — the `FASTCACHE_AUTO_INSTALL` fetch therefore uses bare
  `file(DOWNLOAD)`. Not with `EXPECTED_HASH`, which aborts the configure on a
  mismatch *even when `STATUS` is captured* (measured); the SHA-256 the release
  publishes is compared by hand instead, which is the same guarantee without the
  abort. Every other way the fetch can fail — unpublished platform, no network, a
  binary that will not run here — ends the same way, in one `message(STATUS)` and a
  fall-through, because a project that vendored this file to get a *faster* build
  must not lose the ability to build at all when GitHub is unreachable.
  `ctest -R compile-cache` covers both halves offline, the decline paths through a
  sandbox and the install path through a `file://` mirror.
- **Every checkout in `build.yml` that could configure the project passes
  `fetch-depth: 0`, and the release job's asset list must stay the last key of its
  `with:` mapping.** The default depth-1
  checkout fetches no tags, so `git describe` finds nothing and the build silently
  falls back — which in a packaging job means artifacts named after a release
  nobody cut. Full history, not `fetch-tags: true`: fetching a tag into a depth-1
  clone leaves the tagged commit as an unrelated shallow root `describe` cannot
  reach. The two jobs that only *read* the workflow file — `check-release-gate`
  and `release` — are the stated exception and pass no `fetch-depth`, because
  history buys them nothing and a release must not be able to fail on a clone
  parameter that cannot affect it. Separately, `/publish-release` learns what a release should contain by
  parsing that literal asset list, and its extractor stops only at a line that does
  not look like a filename — `draft: true` looks exactly like one, so a key moved
  below the list is collected as a glob that can never match and publication blocks
  forever on a phantom asset. The same reason forbids writing `files:` followed by
  `|` anywhere else in that file, comments included. A step in the release job
  re-reads the list with that same extractor and fails on any entry containing
  whitespace or a colon, so the rule is enforced rather than merely documented.

`fastcached` and `fastcache-cc` are both installed. Three things the launcher's
cache key depends on, each of which has already caused a silent hit-rate
collapse or a mis-serve and is now covered by regression tests:

- **Preprocessing must suppress line markers** (`/EP` on MSVC, `-E -P` on GNU).
  A `# 1 "/abs/path.cpp"` marker embeds the checkout path in the hashed text.
  On MSVC that is `/EP` **alone**: `/EP` and `/P` are alternatives, not modifiers
  — `/EP` preprocesses to stdout, `/P` to a `<base>.i` file, and MSVC documents
  the pair as "to the file, without `#line`". Passing both left the launcher
  hashing an empty stdout, so a Windows key carried nothing from the source and
  an edited file was answered with the object built from the previous revision.
  That is a wrong build rather than a cold cache, and direct mode hid it, since
  its manifest hashes the source's own bytes — hence the e2e case that edits a
  source with `FASTCACHE_NO_DIRECT=1` and requires a MISS.
- **`/` introduces an option only on Windows.** On POSIX it starts an absolute
  path, and treating it as an option leaves absolute paths unrelativized.
- **Only machine-independent dependency paths may be hashed.** The dependency set
  is part of the key so a moved header re-keys; hashing a toolchain path along
  with it would re-key on the *machine* instead, and two boxes with the same
  compiler at different prefixes would stop sharing every entry they have.
- **A root and the paths a driver emits must be reconciled on both sides or
  neither.** Every root test is a string prefix comparison, so a root spelled
  differently from what the compiler echoes back matches nothing it emits — and
  that empties the keyed dependency set, silences the replay guard, and leaves the
  producing machine's paths in the stored value, all at once and all silently.
  `Cc::IPathResolver` resolves the roots and every emitted path through the same
  function; resolving only one side breaks the driver that previously worked.

- **A compile that writes a second artefact is not cached at all.** What a hit
  reproduces is the object and the dependency record; a **C++ module interface
  unit** also writes a BMI (`.ifc`, `.pcm`), and `/Yc` a precompiled header. Replay
  one and the second artefact is either missing afterwards — which fails loudly —
  or left over from a previous build, which does not. Both halves are one rule and
  neither may be dropped: the module **extensions** (`.ixx`, `.cppm`, `.ccm`,
  `.cxxm`, `.c++m`, `.mxx`) are classified as their own language and refused by
  name, and an ordinary source **promoted by a flag** (`cl /interface`,
  `-fmodule-output`, `--precompile`) is refused off a table. Until this was written
  down the first half held by accident — those extensions simply were not in
  `IsSourceSuffix`, so such a line fell through as "no source file found" and was
  passed through *in silence*, which reads exactly like a broken cache; and the
  obvious "add .ixx so modules are supported" change would have turned that
  accident into a silent wrong build. The refusal now says so under
  `FASTCACHE_VERBOSE`.

The first two break cross-checkout sharing while every unit test still passes,
the third breaks it the moment two machines differ, the fourth breaks it the
moment a root is spelled unusually, and the fifth is a wrong *build* rather than a
cold cache, so `scripts/compile-cache-e2e.sh` (POSIX) and
`run-launcher-e2e.ps1` (Windows) assert them end-to-end in CI on both
platforms.

Distributed compilation adds two more, and both were found by running the feature
under **this repository's own build flags** rather than a toy command line — which
is the reproducible lesson, since no unit test can reach either:

- **The text sent to a worker is NOT the text the key hashed, and the difference
  is `#line`.** The key's probe suppresses markers so no checkout path reaches it
  (the rule directly above). Those same markers are the only thing telling the
  compiler which lines came from a *system header*. Feed a worker the key's text
  and every warning inside libc++ or the CRT is re-reported as if it came from
  the user's own file — under `-pedantic -Werror`, which this project builds with,
  that is a failed compile. Every dispatched TU would fail and be retried locally,
  so **distribution would appear to work while never once helping**: a silent
  100 % fallback with a green build. So `DispatchPreprocessCommand` preprocesses
  a *second* time, with markers, at ~45 ms on a path already committed to seconds
  of remote compilation. Reusing the key's text is the free-looking option and it
  is wrong.
- **A worker must be told its input is already preprocessed, AND what language it
  is in.** Having added the markers back, `-pedantic` then rejects the markers
  themselves as a GNU extension (`-Wgnu-line-marker`) — the fix for the first defect
  creates the second. The answer is the one ccache and distcc already use:
  `-x c++-cpp-output`. It is a `DriverSpec` column, not a branch, so a fifth driver
  is a row.
  - **MSVC's column was empty, and that was the whole of a second defect.** The
    reasoning was that `/E` emits standard `#line` which `cl` accepts in an ordinary
    source file, so there is nothing to tell it — true about the *markers*, and it
    left nothing stating the *language*. The worker writes its own scratch file and
    MSVC reads the language off that file's extension, so a dispatched `.c` came
    back compiled as **C++**: a failed remote compile wherever C is not valid C++
    (so C silently never distributed — the 100 %-fallback-with-a-green-build shape
    this list already records twice), and an object with C++ mangling stored under
    the C key wherever it is. `/TC` and `/TP` are the `-x` spelling MSVC does have,
    and `/TP` is a **byte-for-byte no-op** on a C++ translation unit, so nothing
    that worked before moves. Verified end to end on both Windows drivers.
  - **The extension does not decide the language; it is the last of three
    answers.** A `++` driver compiles *everything* as C++ — "g++ treats .c, .h and
    .i files as C++ source files", in as many words — so taking the language off the
    extension tells a worker to compile as C what the client compiles as C++. That
    is a wrong object, not a failed one, and `gcc` and `g++` are one `Flavor`, so
    the answer is a second column on the *name* table rather than a new question.
    Above both, a build may state the language itself (`/TP`, `-x c++`, which is
    what CMake emits for `set_source_files_properties(... LANGUAGE CXX)`); the
    launcher appends its own spelling **last** so it wins, which is right when
    nothing else spoke and silently overriding when something did — so such a
    command line is **refused** instead. `/Tc`/`/Tp` are refused by the same row for
    a second reason: they name a file, and with a bare file name they carry no
    separator, so `CouldNameAFile` lets them past.
  - **An extension whose language depends on the driver is never guessed at.** `.C`
    is C++ to a GNU driver and C to an MSVC one, and `.M` likewise — so the
    extension does not answer the question and a guess would hand a worker the wrong
    language. Both are excluded from the table by a case-*sensitive* row, ahead of
    the case-insensitive lookup that exists so a `.CPP` on Windows still dispatches.
  - **A module interface unit is refused by both gates, and that is now a rule
    rather than an accident.** `.ixx`, `.cppm` and their kin write a **BMI** beside
    the object, and what a hit reproduces is the object and the dependency record —
    so replaying one leaves the BMI missing (which fails loudly) or left over from a
    previous build (which does not). They were previously not in `IsSourceSuffix`,
    so nothing recognised them and every such line fell through as "no source file"
    and was passed through **in silence**, indistinguishable from the cache being
    broken; adding one there — the obvious "support modules" change — would have
    started replaying objects whose BMI nobody reproduced. They are now recognised,
    refused by name with the reason said out loud, and the same reason refuses an
    ordinary source promoted by a flag (`cl /interface`, `-fmodule-output`,
    `--precompile`, and `/Yc` for the precompiled-header case).

Three more come from running the worker as a *service* rather than in a
terminal, and each has already been a bug:

- **A listener that cannot be woken cannot be shut down, and that is a property
  of the SOCKET rather than of the loop.** POSIX does not unblock a parked
  `accept()` when another thread closes the listening socket, so the only
  portable wake-up is `SO_RCVTIMEO` making `accept()` return periodically —
  which is exactly what `BlockingListener::SetTimeouts` exists for, and what the
  daemon's admin listener already does. `WorkerServer::Run` *documents* that poll
  timeout as the mechanism it relies on, and nothing supplied one: installing a
  SIGTERM handler then made the signal non-fatal without making the loop
  reachable, so `systemctl stop` hung until the supervisor escalated to SIGKILL.
  **macOS hides it** — there `close()` does wake the accept — which is why it
  passed locally and on `macOS-clang-release` and failed only on Linux, as
  `dist-compile-e2e ***Timeout 900.10 sec` in three jobs. It then arrived a
  *second* time through socket activation, where the caller was expected to apply
  the timeouts after adopting; `AdoptInheritedListeners` now takes them as
  parameters, so there is no way to obtain a listener that cannot be stopped.
- **`LISTEN_PID` is not a formality.** The activation variables survive fork and
  exec, so every grandchild of an activated service sees them. Adopting on their
  strength alone means treating whatever the parent left on descriptor 3 as a
  listening socket — a log file, a database connection, the read end of a pipe —
  and then accepting on it forever. The check is what makes "is fd 3 a listener?"
  answerable at all, which is why `ParseSocketActivation` is pure and every rule
  around it is a unit test. Two consequences: the variables are cleared **even
  when nothing was adopted**, since a process that decided they were not addressed
  to it must not pass them to a child that would decide differently on a reused
  pid; and the adopted descriptors are marked close-on-exec, because systemd
  deliberately omits that so a service can re-exec itself, while this worker
  spawns a compiler per job and a compiler holding the listening socket keeps the
  port alive after the worker exits.
- **Under socket activation `--advertise` is required, because the fallback
  becomes a guess the process cannot make.** The socket unit owns the port and
  never tells the service which one, so `{--bind}:{--port}` describes nothing —
  and `0.0.0.0` is not an address a remote client can dial regardless. The failure
  is the worst shape this system has: registration *succeeds*, the worker
  heartbeats happily, the scheduler leases that endpoint out, and every client
  fails to connect and compiles locally, with no error anywhere and a fleet that
  looks healthy from both ends. Refused at startup instead — and refused **before**
  the toolchain walk, which is the same cheap-and-fallible-first ordering the
  adoption check follows: a fingerprint takes seconds, a misconfiguration is
  decided in microseconds, and doing the expensive thing first means an operator
  watching a worker start sees nothing during the part where something can still
  go wrong.

**The scheduler lives where leadership lives, and that is not the cache daemon.**
`WorkerRegistry` and `LeaseTable` used to be reached through a `Dispatch` role on one
of `fastcached`'s listeners, which made the cache daemon a scheduler as well as a
store. The two have opposite deployment shapes: a cache is shared infrastructure
somebody operates, while handing out capacity is a decision only **one** node may
make at a time — and nothing in `fastcached` can establish which node that is.
`Distributed::SchedulerService` is that logic with leadership as a first-class
input, and it is pure with respect to I/O for the reason the two tables under it
are: every rule below is a `ManualClock` unit test rather than a socket and a sleep.
Consequences that are each load-bearing:

- **Leadership and membership are one `Gate()`, not a check per handler.** These are
  the security- and policy-relevant decisions of the whole surface, so a verb added
  without them would be a verb that quietly skips both — and the test asserts the
  refusal *per verb* precisely because "I added a handler that forgot the gate" is
  the regression the arrangement exists to make impossible.
- **Leadership is asked first, and the reason is the diagnostic rather than the
  cost.** A follower cannot know the cluster's membership any better than it knows
  the fleet, so answering `NotAMember` there sends an operator to inspect a policy
  that was never consulted. `NotLeader` carries the leader's endpoint **in the
  message**, so a client redirects instead of giving up; `SchedulerRole::Undecided`
  is the same code with an empty message, because an election in progress is a
  different fact and there is nobody to name. Three roles rather than a `bool`, for
  exactly that third state.
- **Anti-leeching refuses the fleet, never the cache.** A non-member reads and
  writes objects exactly as before — the cache is a separate service this class
  cannot reach — and is refused only the fleet's CPU time, which is the thing
  membership pays for. Hence `NotAMember` rather than `Unauthenticated`: one is
  about a credential an endpoint requires, the other about contribution.
  - **An empty member set refuses everybody, and "admit everybody" is a named type
    somebody constructs.** `ClusterMembership` with nothing in it is the state of a
    node whose discovery has not run, has the wrong key, or is misconfigured — and a
    scheduler that answered "member" there would silently become an open one, which
    is invisible from both ends because the fleet keeps working and merely serves
    strangers too. `OpenMembership` is the right answer for one machine or a fleet
    whose reachability is its boundary, but it is never a *default*: "no policy" and
    "a policy that admits everybody" have to be the same explicit decision, which is
    also why `Membership::Outsider` is the zero value. Matching is whole-string, not
    by prefix — `10.0.0.1` must not admit `10.0.0.10` — and the port is part of the
    identity, because the proof a peer gave covered the `(node, endpoint)` pair.
  - **The identity is a HOST, and the two vocabularies are collapsed in the
    constructor rather than left to each caller.** Discovery admits a peer at a
    `(node, endpoint)` pair, so the obvious member set is endpoints — and matching a
    caller against it would never succeed even once: a peer *connecting* to the
    scheduler comes from an **ephemeral source port**, which is not its Raft endpoint,
    so `ISocket::PeerAddress()` reports a bare host and there is nothing to compare a
    port against. An endpoint-keyed set refuses every legitimate member while looking
    entirely correct, and the fleet silently never distributes anything. So
    `ClusterMembership` takes endpoints and stores their host parts, and `Classify`
    takes a host; there is no way to publish one vocabulary and query the other. It
    splits through `Core/HostPort` rather than locally, because `rfind(':')` on
    `[::1]:7000` splits at the wrong colon — the defect that header exists to hold in
    one place. An endpoint with no port is kept whole rather than dropped: a member the
    set cannot represent must not silently stop being one. What this gives up is
    recorded rather than hidden — two nodes behind one NAT are indistinguishable here,
    which is acceptable because this refuses *strangers* rather than co-located peers,
    and separating them needs a credential in the frame.
  - **The oracle is a seam and not a call into `Cluster::PeerDirectory`.** The
    dependency would run the wrong way — `Distributed` is the policy, `Cluster` is
    one way of establishing the fact it needs — and the answer is *deployment*-shaped
    rather than universal, which is what an interface is for. `Publish` is a setter
    and one of the documented carve-outs to configuration-at-construction: membership
    is precisely what changes while the object lives, and rebuilding the oracle per
    join would mean handing a new one to a running server.
- **Duplicate suppression is asked BEFORE capacity, and the order is the whole
  point.** `LeaseTable::Acquire` needs a worker id, so the code this was lifted from
  had to `Pick` first — which meant a second client missing the same key at a busy
  fleet was told `NoCapacity`. Both conditions genuinely hold; the operator reads
  "buy more machines" where the truth is "this build asked for the same object
  twice", and it lands hardest exactly where duplicate suppression does the most
  good: a wide parallel build where many translation units miss one key at once.
  That is the same defect the no-worker/no-capacity split already exists to prevent,
  reached by a third route. `LeaseTable::IsInFlight` is what makes the question
  answerable without a worker; it is **advisory**, and `Acquire`'s own refusal stays
  as the backstop for the race, because that one decides atomically. It reports
  *liveness*, not presence — an expired entry is left behind until the next
  `Acquire` for that key sweeps it, so a check on the map alone would refuse one key
  forever after a single client abandoned it, with nothing saying so.
- **A refusal that moves a counter says so in a table.** `RefusalTable` pairs each
  code with the counter it moves, and `std::nullopt` is a legitimate row: a
  malformed frame is a *client* defect, and counting it beside the capacity
  refusals would put one broken build's noise into the numbers a fleet is sized
  from. Four hand-written `Count(...)` calls beside four `Refuse(...)` calls are
  four chances to forget one, and a refusal with no counter is invisible in exactly
  the situation an operator is trying to diagnose.

**The scheduler's port is reachable before membership is established, so its payload
cap is the small one.** `SchedulerServer` refuses a frame declaring more than 64 KiB,
against the cache port's 256 MiB, and the asymmetry is the whole point: the gate runs
*inside* `SchedulerService`, i.e. after the frame has been read, so an unauthenticated
peer can make this endpoint buffer whatever it declares. That is the same hole
`OpDescriptor::maxPayload` closes for `AUTH`, reached by a different route, and it is
closed the same way — a scheduler verb carries a fingerprint, an endpoint and a key,
none of which is large. Three consequences:

- **The check is on the DECLARED length, before the read**, so an over-cap frame costs
  no allocation at all. Checking after would be a memory-exhaustion hole opened by the
  check meant to close one.
- **The refusal names both numbers and is a reply, not a close** — `payload-too-large`
  with the ceiling in it, because "too large" alone tells an operator nothing about a
  64 KiB limit, and a close is indistinguishable from a dead host.
- **A wrong magic is still the one condition that closes.** With no declared length
  there is nowhere to resynchronize to, so there is nothing an answer could mean.

**An endpoint reports the port it bound, not the one it was asked for.** `Start` formats
`BoundEndpoint()` from `listener->BoundPort()`, which matters because `0` means "pick a
free one" — an endpoint echoing `:0` back cannot tell an operator, a log line or a test
where it ended up. `ParseTcpPort` still refuses `0` as a *CLI* value, correctly: as
something an operator types it names no port anyone could dial. The two rules are not in
tension, and the node's tests find a free port by binding a probe and releasing it, the
idiom `AdminEndpoint_test` already uses.

**A node caches for itself, and what that saves is the round trip rather than the
compile.** The shared `fastcached` holds every object, so a second copy on the node
looks redundant — it is not, because a developer who rebuilds one tree twenty times a
day pays the network twenty times for objects that never left their machine, and on a
slow or lossy link that is the difference between a cache that helps and one that
hurts. `Node::LocalCache` is pure over an injected `IStorage`, an `ICacheUpstream` and
a clock, so every rule below is a unit test rather than a socket and a sleep. Four
rules, none of them the obvious one:

- **A local hit does not consult the upstream at all**, and the test asserts *zero*
  calls rather than few, because "few" is not the property. Revalidating would have
  moved the round trip rather than removed it, and it is unnecessary by construction:
  an object key is a digest over the preprocessed text, the arguments, the compiler
  identity and the dependency set, so a key that matches names the same object.
- **A local miss populates the tier**, or this is a proxy and the second build is
  exactly as slow as the first. A failure to populate is *not* a failure to serve —
  the value is in hand and the caller is owed it.
- **A store writes local FIRST, then offers upstream.** The local write must not fail
  for a reason the network chose; the upstream offer is best-effort by contract, so
  its answer is **counted rather than returned** — a client retrying on it would be
  retrying something already durable where it matters.
- **An unreachable upstream is a MISS, not an error.** Every caller compiles either
  way, so distinguishing them would give a build a failure mode it does not need. The
  distinction lives in a counter, where it is actionable. `NoUpstream` is a named type
  rather than a null pointer, so "this node has no shared cache" is a decision
  somebody made rather than a pointer nobody set.

`NoExpiry` is `TimePoint::max()` and **not** a default-constructed `TimePoint`: the
storage tests `entry.expiry <= now`, so zero means "expired before any clock reading"
rather than "no deadline" — every write landed and was unreadable, a cache that
silently stores nothing while reporting success on every write. `CacheEngine` already
spelled it the same way, which is what makes this a drift rather than a discovery.

- **`FASTCACHE_ADDR` is deliberately NOT defaulted to the local node.** Unset means the
  cache is off, and it stays that way: defaulting to localhost would make every build
  on a machine without a node pay a failed connect per translation unit, in silence —
  the exact cost `USE_COMPILER_CACHE` probes at configure time to avoid. Pointing it at
  the node is one line of configuration, and it is better written by an operator than
  assumed by a tool.
- **One accept loop serves all three of the node's framed surfaces.** `FrameEndpoint`
  over an `IFrameResponder`; a second and third accept loop would be near-copies of a
  listener, a shutdown order and a poll timeout. An interface rather than a
  `std::function`, because a responder outlives the endpoint and a closure would keep
  its whole enclosing scope alive with it. Three things the seam keeps per-surface that
  one constant would have flattened: the **payload cap** (64 KiB for scheduler verbs,
  256 MiB for a cache STORE carrying an object file — sizing both large hands an
  unauthenticated peer a way to make the *scheduler* allocate megabytes, sizing both
  small silently stops caching this machine's largest translation units); the **default
  bind**, which is inverted between them (wildcard for the scheduler, because one no
  peer can dial does nothing; loopback for the cache, because a node's private cache
  reachable from the network is a decision); and **whether the peer's identity is
  consulted at all**, which only the scheduler does.
- **Each surface is one owned object, and the reason is not tidiness.**
  `SchedulerTier` and `CacheTier` each hold a *reference chain* — endpoint → responder
  → protocol → service, and endpoint → responder → proxy → cache → storage + upstream
  — so as locals in a function body their declaration order is load-bearing and
  silently so: getting it wrong is a dangling reference rather than a compile error. As
  members it is checked by the language. `WorkerBody` also reached a cognitive
  complexity of 67 against clang-tidy's 60 with both inline, and extracting only one of
  them left 62 — the number was pointing at two coherent decisions, not one.

`scripts/dist-compile-e2e.sh` asserts the consequence rather than the mechanism:
that a worker's object is **byte-identical** to a locally compiled one. That single
assertion is what fails if either rule is broken, and it is the whole soundness
claim of the feature — an object that differs is stored under a key other machines
then fetch.

**On Windows that assertion has to be spelled differently, and it is measured
rather than relaxed.** Both MSVC drivers write the **clock** into the COFF header
(two compiles of one file to one path two seconds apart differ in exactly byte 4;
only `/Brepro` suppresses it, and a fixture that passed `/Brepro` would be asserting
something about a command line no build uses). `cl` additionally records the
**absolute path of the object file** in `.debug$S` and hashes the file it opened
into `.chks64`, with no debug flag asked for — and a worker compiles its own scratch
file to its own scratch path, so neither can ever match. Everything carrying code or
data does: `.text$mn`, `.rdata`, `.xdata`, `.pdata`, `.drectve`, `.data$r` and
`.bss` are byte-identical between a reference compile of the original source and a
worker-shaped compile of `/E` text elsewhere. `dist-compile-e2e.ps1` therefore
compares **section by section** against a per-driver table of what may differ, and
`clang-cl`'s row is *empty* — it records only the source's base name, which the
worker is now told, so its objects differ by the clock alone. Three lessons are
worth keeping, and each was a hole first:

- The COFF **header** is compared field by field rather than skipped, because an
  object built for another architecture differs *there and nowhere a section walk
  would look*. Its two excused fields are excused by name — the clock, and the
  symbol-table pointer, which moves whenever an excused section changes size.
- **A section walk alone accepted a truncated object.** MSVC writes the symbol
  table last, so cutting a tenth off a file leaves every section intact and
  comparing equal — and a truncated transfer is one of the few faults distribution
  can actually introduce. COFF states its own end (the string table opens with a
  size that includes itself and is the last thing in the file), and that check runs
  on both sides under every rule: "the file is as large as it claims" is not a
  property a driver gets to opt out of. Whether the tail's *content* may differ is
  a second per-driver column, measured the same way — clang-cl's is identical, and
  `cl`'s is not.
- The comparison has a **`-SelfTest`** of its own, because the fixture's own logic
  is the one thing nothing else tests. Two defects in it were found that way and
  one by asking it to reject deliberately wrong objects: the earlier control
  compared two objects with different **names**, which `cl` records inside the
  object, and so reported a perfectly reproducible driver as non-reproducible in a
  CI log — as the answer to the question the fixture had been asking for three
  commits.

**Every wait in that fixture is bounded, and that is a rule rather than a
detail.** Its first version killed a worker and then `wait`ed, which HANGS when a
signal is handled but the stop never completes — and a 900-second ctest timeout
naming nothing is the least useful way CI can report a defect. `stop_and_require_exit`
fails in 15s saying what it waited for, and the cleanup trap escalates to SIGKILL
rather than blocking, because cleanup runs on every exit path including the
failing ones: an unbounded wait there turns any single failed assertion into a
silent suite timeout. The same lesson applies to unit tests — a helper thread
spinning on a counter a regression never advances hangs instead of failing, which
is what the `IdleListener` hook exists to avoid. Relatedly, CI's live-systemd step
waits for the worker's **own** readiness line rather than `systemctl is-active`:
`Type=simple` is reported active the moment systemd forks, while the worker still
has seconds of include-tree walking to do.

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

## C++ Coding Guidelines (self-contained — no external `cpp.md` required)

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
- **A local gate cannot see a configuration it does not build, and advice nobody
  runs is not a gate.** `scripts/local-gate.sh` is that advice as a script:
  clang-format at the pinned version, then `clang-debug` and `gcc-release`, refusing
  to run `ctest` against a build that did not complete. It exists because this
  paragraph was already here and was skipped twice in one branch -- once for a GCC
  `-Wnull-dereference` through an inlined `memcpy` that clang emits at no level, and
  once for a clang-tidy check the binary on `PATH` had never heard of. Both cost a
  full CI cycle for a configuration the developer already had.

  The default agent
  preset is `clang-debug`: one compiler, one standard library, `-O0`, sanitizers on.
  CI is four more — GCC at `-O3`, clang-cl, MSVC, and clang against **libc++** on
  macOS — and each of the three defects that reached CI on the Raft branch was
  invisible to every configuration below it. GCC 14 at `-O3` reports
  `-Wnull-dereference` inside `std::optional::value_or` where clang does not;
  clang-tidy 22 knows checks clang-tidy 20 has never heard of; and libc++'s
  `uniform_int_distribution` is a different function from libstdc++'s. Before
  pushing a change that touches a header everything includes, a randomness or
  timing seam, or anything a test harness's determinism rests on, build **at least
  one release configuration and one non-clang compiler** locally —
  `cmake --preset gcc-release` and `clang-release` both exist and both run in WSL.
- **`clang-format` and `clang-tidy` after every change — at the version CI pins.** Both jobs
  run the `$CLANG_TOOLS_VERSION` binary (`.github/workflows/build.yml`), and successive LLVM
  releases do not agree with each other: the style job compares against a *newer formatter*,
  and the clang-tidy job enables *checks that did not exist* in an older one. So a tree that is
  clean under whichever binary happens to be on `PATH` can still be rejected — a red build for
  code nobody mis-wrote, and one no local run catches unless it uses the same version. Name the
  version explicitly rather than relying on `PATH`:
  `git ls-files '*.h' '*.hpp' '*.cpp' | xargs clang-format-$V --dry-run --Werror --style=file`,
  and `-DCMAKE_CXX_CLANG_TIDY=clang-tidy-$V` **in a build directory of its own**. Found three
  times in one branch: four files reformatted by 22 after 20 had passed them, four
  `find(...) != npos` tests that only 22 reports as `readability-container-contains`, and five
  `std::lock_guard`s that only 22 reports as `modernize-use-scoped-lock`. **The preset alone is
  not that sweep**, and that is the trap: `clang-debug` sets `CMAKE_CXX_CLANG_TIDY=clang-tidy`,
  which on a machine carrying both resolves to whichever `PATH` finds first — 20 in this
  project's WSL image, where 22 sits right beside it as `clang-tidy-22`. So a `clang-debug`
  build reports "clang-tidy clean" in exactly the way that means nothing, and the version it
  used is printed nowhere. Configure a second build directory naming the version, and run that.
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

Line endings are LF everywhere, and that is a `.gitattributes` rule
(`* text=auto eol=lf`) rather than an instruction to set `core.autocrlf`. The
config is per-clone and per-developer, so without the rule two people editing one
file disagree about what a line ending is — and the disagreement is invisible
until a diff comes back as *every line changed* for a two-line edit, which is how
it was found. Stored content was already LF, so the rule changed nothing that is
committed, only what lands on disk at checkout. `*.sh` keeps a row of its own even
though the general rule covers it, because the consequence there is specific: a
CRLF shebang makes the kernel look for an interpreter whose name ends in a
carriage return, so such a script does not misbehave — it fails to start at all.

CMake presets live in `CMakePresets.json`. Common entry points:

```sh
# Clang Debug with PEDANTIC + ASan + UBSan + clang-tidy (the default agent preset; Linux + macOS)
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug

# Linux — GCC Debug
cmake --preset gcc-debug && cmake --build --preset gcc-debug

# Linux — Coverage (HTML in out/build/clang-coverage/)
cmake --preset clang-coverage
cmake --build --preset clang-coverage

# Linux — sanitizer-only presets
cmake --preset clang-asan-ubsan
cmake --preset clang-tsan

# Linux/macOS — RelWithDebInfo + Tracy profiler (see "Profiling with Tracy")
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

`USE_COMPILER_CACHE` (default ON, `cmake/portable/CompileCache.cmake`) fronts the compiler
with our own `fastcache-cc` when it is on `PATH` and a daemon answers — at
`127.0.0.1:6674` by default, or wherever `FASTCACHE_ADDR=host:port` points;
`FASTCACHE_SOURCE_DIR`/`FASTCACHE_BINARY_DIR` are injected from the source and build
trees. Configure proves the cache works by compiling one tiny file through the
launcher (~0.1 s) and requiring a `HIT`/`MISS`, because a launcher that cannot
reach its daemon still compiles fine and would otherwise cost every TU a failed
connect in silence. When nothing answers it falls back to `sccache`. When
*nothing* is installed, `-DFASTCACHE_AUTO_INSTALL=ON` (default OFF) fetches a
prebuilt `fastcache-cc` for the host from the latest stable release instead,
staged per user so a machine downloads it once; `cmake/README.md` is the note
for projects vendoring the module. A cache hit reproduces only the object file,
so with either launcher active the module scan and precompiled headers are
turned off and MSVC debug info is forced to `/Z7`
(a modmap flag makes the launcher's preprocess step fail, and a PCH or shared
PDB is a second artefact no hit can reproduce).

## Testing

Catch2 tests live next to the implementation files, so `Foo.cpp` has a `Foo_test.cpp`. A `test_main.cpp` serves as the entry point.

`src/tests/Unwrap.hpp` holds the one helper every test target shares.
clang-tidy's `bugprone-unchecked-optional-access` cannot see a `has_value()`
guard through Catch2's `REQUIRE`, so a plain `*x` after one is a **build failure**
under `WarningsAsErrors` — `Unwrap(x)` goes through `value_or`, which is provably
safe, and the preceding `REQUIRE` still fails first when the optional is empty.
It replaced eleven copies that were *near*-identical rather than identical: each
carried its own abbreviation of that reasoning, so why the idiom exists was
reconstructible from some and not from others, and an author who found a terse
copy had nothing telling them a plain dereference was not simply better. It is
header-only and includes only `<optional>`, so the launcher's and worker's test
binaries can use it without linking `FastCache`. `std::expected` is **not**
covered by that check, so a `*result` after `REQUIRE(result.has_value())` stays as
it is — routing it through `Unwrap` does not even compile.

Not every test is a Catch2 case. Script-driven tests are registered in
`src/tests/CMakeLists.txt`: the `smoke`-labelled ones start a real daemon or
invoke a real compiler and report a missing prerequisite as skipped (exit 77 with
`SKIP_RETURN_CODE`), while `repository-hygiene` runs
`scripts/check-repository-hygiene.cmake` through `cmake -P` and is deliberately
*not* labelled `smoke`, since it needs no daemon, socket or compiler and so belongs
in the default `ctest` set. It reports "not a git work tree" by printing `SKIP: `
and exiting 0, matched by `SKIP_REGULAR_EXPRESSION` — a `cmake -P` script cannot
choose its own exit code before CMake 3.29 (`cmake_language(EXIT)`) and this
project supports 3.28, so a `SKIP_RETURN_CODE` it could never return would be dead
configuration.

**Which CMakeLists registers a script-driven test is load-bearing, not filing.**
`src/apps` walks its app table *in order*, so a test registered beside one binary
cannot name a binary that comes later in that table: at the point
`src/apps/fastcache-cc` is configured, `fastcache-compile-node` is not a target
yet, and a `$<TARGET_FILE:>` guard on it does not fail — it silently skips the
test, forever, with one `message(STATUS)` in a configure log nobody reads.
`src/tests` is added *after* `src/apps`, so every target exists by the time it
runs. That is why `dist-compile-e2e` lives there, and the general rule is that a
script-driven test naming more than one executable belongs in `src/tests`
regardless of which binary it feels closest to. (`compile-cache-e2e` predates the
rule and names only `fastcached`, which the table happens to reach first.)

`dist-compile-e2e` additionally allocates its ports per run rather than fixing
them. It needs four, and four more fixed ports is four more ways to collide with
whatever else a CI runner is doing — a failure that reads as "distribution is
broken" when it means "something else was listening". `cluster-e2e` does the same
with the six it needs.

`cluster-e2e` is the consensus counterpart, and what it covers is deliberately
disjoint from the unit tests rather than a slower repeat of them: three real
processes elect a leader and *keep* that leader for three seconds of polling, a
follower's refusal names an endpoint that a client then successfully dials, a
setting replicates, a member is removed, and the cluster re-forms after its leader
is killed. The stability half is not padding: a cluster that re-elects on a timer
has exactly one leader at almost every instant, so a single poll passes against
leadership that never settles. Every one of those is a property of the wire,
the transport, the timers and the command line meeting at once — which
`RaftClusterHarness` cannot reach precisely because it replaces all four. It is
POSIX-only for now: the properties are platform-independent and the fixture is not,
so a Windows counterpart would be a translation rather than new coverage.

## Releasing

The version is the git tag, so cutting a release is pushing one:

```sh
git tag -a v0.1.0 -m "fastcached 0.1.0"
git push origin v0.1.0
```

That tag matches the trigger in `.github/workflows/build.yml`, which runs the
**entire** suite against the tagged tree — nothing about a release path is
exercised only at release time — and then the tag-gated `release` job collects the
three packaging jobs' artifacts, asserts that the set is complete and that every
filename carries the tag's version, and creates a **draft** GitHub release with
them attached. Nothing publishes automatically; a human does that with
`/publish-release` once the assets have been verified. `/draft-release` drives the
tagging half.

There is no changelog file: release notes are GitHub's generated commit summary
(`generate_release_notes: true`), so commit subjects are what a reader of the
release page sees.

## Profiling with Tracy

[Tracy](https://github.com/wolfpld/tracy) instrumentation is **opt-in and
zero-cost when off**: it is gated behind the `TRACY_ENABLE` CMake option
(default `OFF`). When off, no Tracy header is included, nothing is linked, and
every profiling macro in `FastCache/Core/Profiling.hpp` collapses to
`(void) 0` — the default `clang-debug`/`clang-release` binaries are unchanged
and link zero Tracy symbols.

### Building the profiling daemon

```sh
cmake --preset clang-tracy        # RelWithDebInfo, TRACY_ENABLE=ON, TRACY_ON_DEMAND=ON
cmake --build --preset clang-tracy
# -> out/build/clang-tracy/target/fastcached
```

`TRACY_ON_DEMAND=ON` means the daemon buffers nothing until a profiler
connects, so it is safe to leave running; you only pay the cost while
capturing.

### Adding zones

Instrument code through the wrapper macros, never Tracy directly:

```cpp
#include <FastCache/Core/Profiling.hpp>

FC_ZONE_SCOPED;                         // zone named by source location
FC_ZONE_SCOPED_N("CacheEngine::Get");   // zone with a compile-time literal name
FC_FRAME_MARK;                          // one logical request/frame boundary
FC_THREAD_NAME("fc-worker-0");          // name the calling OS thread
FC_PLOT("lru.bytesUsed", value);        // scalar timeline (value is numeric)
```

**Coroutine constraint (must be observed):** `FC_ZONE_SCOPED*` declares a
thread-local stack-RAII guard and **must not straddle a `co_await`** — under the
reactor model the await resumes on a later frame and the guard's destructor
would corrupt Tracy's per-thread zone stack. Place zones only in synchronous
leaf functions or in `{ }` blocks containing no `co_await`. `FC_FRAME_MARK` is a
stackless timeline event and is safe anywhere, including inside coroutine loops.
Macro arguments must be free of side-effects the program relies on (when Tracy
is off they are discarded unevaluated). `FC_ZONE_SCOPED_N` requires a
compile-time string literal; for a runtime label, annotate the current zone with
`FC_ZONE_NAME(ptr, len)` / `FC_ZONE_TEXT(ptr, len)` instead.

### Analyzing a capture

Build a Tracy viewer once (sources are fetched into the build tree under
`out/build/clang-tracy/_deps/tracy-src/`), or grab a prebuilt **v0.11.x** viewer
from the Tracy releases — the client and viewer protocol is version-locked.

```sh
# Interactive GUI (needs glfw/freetype/capstone/gtk3/dbus dev packages):
cmake -S out/build/clang-tracy/_deps/tracy-src/profiler -B /tmp/tracy-gui -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/tracy-gui -j        # -> /tmp/tracy-gui/tracy-profiler

# Headless capture (no GUI deps; writes a .tracy file to open later):
cmake -S out/build/clang-tracy/_deps/tracy-src/capture -B /tmp/tracy-cap -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/tracy-cap -j        # -> /tmp/tracy-cap/tracy-capture
```

Workflow — the client listens on TCP **8086**:

1. Start the daemon: `./out/build/clang-tracy/target/fastcached --bind 127.0.0.1`.
2. Connect the profiler (GUI **Connect**, or `tracy-capture -o out.tracy -a 127.0.0.1`).
   On-demand mode records only from the moment of connection, so connect **before**
   driving load.
3. Drive traffic through the hot path, e.g.
   `memtier_benchmark -s 127.0.0.1 -p 6674 -P memcache_text --ratio=1:4 -n 50000`,
   `redis-benchmark -p 6674 -t set,get -n 100000`, or a quick
   `printf 'set foo 0 0 3\r\nbar\r\nget foo\r\nquit\r\n' | nc 127.0.0.1 6674`.

What the instrumentation surfaces: thread rows named `fastcached-main` /
`fc-worker-N` / `fc-reactor`; one frame per request; the nested zone breakdown
`socket.read → LineReader.TryExtractLine → memcached.Handle*.dispatch →
CacheEngine::* → ShardedStorage::* → LruStorage::* / EvictToFit → socket.write`;
and the `lru.bytesUsed` plot for memory pressure. Use the **Statistics** window
sorted by self-time to find hotspots; a gap between a `ShardedStorage::*` zone
and its inner `LruStorage::*` zone is shard-mutex wait time.

