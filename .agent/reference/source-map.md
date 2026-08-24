# Annotated source map

The long-form version of the tree in `AGENT.md`, with each directory's rationale
kept. `AGENT.md` carries the same tree with one-line annotations; this is where
the "why does this directory exist" prose lives.

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
  Async/        Task<T>, Cancellation, ResumeOn, SleepUntil,
                InterruptibleSleepUntil (a bounded wait a stop can interrupt),
                DeadlineTimer (the same shape with a callback, for a timeout that
                must tear an operation down rather than merely stop waiting for
                it -- and which retires its own frame through
                IReactor::CancelPending rather than waiting out a poll),
                AsyncQueue (MPSC, bounded, closable: what replaces a condition
                variable once the consumer is a coroutine), IReactor
                (Run/Stop/Submit/Schedule/CancelPending -- the last being how a
                parked frame is ever reclaimed) + TestReactor and the platform
                reactors (EpollReactor / IocpReactor / KqueueReactor)
  Net/          ISocket, IListener, IConnector (the outbound counterpart to
                IListener, coroutine-shaped: BlockingConnector for threads that
                may block, PlatformConnector -> EpollConnector /
                KqueueConnector / IocpConnector for a reactor thread, with
                ConnectFlow the platform-free half all four share and
                ReactorDial one body for the two readiness backends),
                IAsyncAddressResolver + ThreadedAddressResolver (getaddrinfo has
                no async form and no timeout, so it gets a fixed pool; a literal
                address never reaches it), TcpClient (the ONE TCP client:
                ConnectTcp plus the coroutine SendAll/RecvExactly loops, which
                three separate copies of this code each used to carry),
                IoAwaitable, IAdmissionControl, SocketAddress,
                BlockingSocket (Winsock + POSIX),
                EpollSocket / IocpSocket / KqueueSocket (reactor-driven),
                InMemoryTransport (paired pipes + InMemoryListener),
                InheritedListener (systemd socket activation: LISTEN_FDS/
                LISTEN_PID parsing is pure and unit-tested; adoption applies
                close-on-exec and the shutdown timeouts, which are parameters
                rather than the caller's job)
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
                one coroutine per peer on the reactor), RaftPeerServer (inbound,
                also on the reactor)
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
  Protocol/     IProtocolHandler, ProtocolAutodetect,
                Framing/ByteReader (line and length-prefixed), MemcachedText,
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
                            work sits behind `IProcessRunner` / `Net/ISocket` /
                            `IPathResolver` (the last collapsing every spelling
                            of one location — 8.3, `subst`, junctions, symlinks
                            — to one, memoized per directory),
                            so main.cpp's flow logic is platform-free. Compiles
                            in `Cli/UsageDoc.cpp`, the four `Net/` rows that are
                            its TCP client, plus `Platform/Environment.cpp`
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
                            OFF — test infrastructure, never installed, but
                            built by the `linux` and `clang-tidy` CI jobs for the
                            reason `fastcache-bench` is: a target nothing
                            compiles is a target that rots, and this one had
                            rotted all the way to not building on POSIX). Drives
                            either driver family off a two-row table, and gets
                            its socket, its process spawning and its
                            dependency-path check from the same code the daemon
                            and launcher use — a tool that exists to prove
                            the canonicalization contract proves nothing if it
                            reimplements either side of it
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
