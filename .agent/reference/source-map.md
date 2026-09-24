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
                beside Clock and for the same reason), ISecureRandom (the OS CSPRNG,
                for bytes that must never repeat: nonces and minted ids), Logger, BufferPool,
                Bytes, Endian, Crc32c, MurmurHash3 (128-bit key digest),
                StringHash, Owner, SecureBytes (the one zeroing primitive, and
                `SecureByteBuffer` -- the allocator every credential lives behind, so
                the wipe happens at each RELEASE and not only at the holder's death),
                Utf8 (one strict RFC 3629 decoder -- overlongs,
                surrogates and anything above U+10FFFF refused, so what it accepts
                a strict parser on the far end accepts too),
                Markup (the one markup escaper, over Utf8: it tests decoded CODE
                POINTS against XML 1.0's `Char` production rather than bytes, because
                what that production excludes includes perfectly good UTF-8 -- and it
                is here rather than beside its first consumer because three targets
                spell the convention, two of which have no business reaching into
                Distributed/), Profiling (Tracy wrappers), and the crypto seam
                -- Ed25519 (RFC 8032 signatures), X25519 (RFC 7748 key agreement,
                refusing the all-zero secret) and Hkdf (RFC 5869, over Sha256's
                HMAC) -- which is the ONLY first-party code that reaches the
                vendored Monocypher, so each primitive's traps are decided once,
                beside its RFC vectors (#178) -- and SessionSeal, the one construction
                that tags a session's frames under the key its handshake agreed, at an
                implicit position: here rather than beside the Raft peer wire it was
                written for, because the `0xFC` wire needs the same thing next
  Transport/    What this project keeps of its own beside core-cpp's `core::net` (#1596),
                which is where the coroutines (`core::async`), the event loop, every
                socket, TLS, the connectors and the resolvers live now. NativeListen:
                BindAndListen (exclusive by default, SO_REUSEPORT only when asked),
                ListenOnSharedPort (a port several loops share, handed to
                `core::net::adoptListener`), AdoptInheritedListener (socket activation,
                closing a descriptor it could not adopt) and BlockingListener (a
                listener a thread BLOCKS on, for the admin endpoints) -- each a
                candidate for graduation into core-cpp. LingeringClose (how a
                server closes after answering: half-close, listen until the
                peer closes or a bound says stop, then close -- a bare close
                over unread input is a reset that destroys the answer)
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
                RaftWire (the 0xFA peer frame), RaftPeerSession (the handshake
                every peer connection proves each end's OWN identity key with, and
                the session key both ends agree for the per-frame tag -- pure, so the
                server, the transport and RaftClusterHarness drive the same objects --
                reached through IRaftPeerIdentity over IRaftPeerKeys, because
                Cluster/ includes Consensus/ and the roster the keys come from is
                Cluster/'s ClusterState), RaftPeerTransport (outbound, one coroutine per
                peer on the reactor), RaftPeerServer (inbound, also on the reactor)
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
  Cluster/      DiscoveryService + DiscoveryWire (the LAN beacon and the
                identity-key challenge after it, driven over the socket pair Net/SharedPortDatagram
                gives a node: it listens where the segment shouts, on a port every
                node shares, and answers from one only it holds, because just one
                of the sockets sharing a port is handed a unicast and the
                challenge and the proof are both unicast),
                PeerDirectory (who proved which key, and where),
                RosterKeys (the keys the Raft peer wire judges members by: the
                command line's `@<key>` until the replicated state says otherwise,
                with a revoked key staying revoked whatever the command line says),
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
                Roster + RosterCertificate (#178): the roster a lease is checked
                against -- voters, principals and revoked keys, projected from the
                state at a version `Apply` derives -- and a voter's endorsement of
                it, `[cluster, version, SHA-256(roster), notAfter]` signed with that
                voter's identity key. `CertifyRoster` is the one majority rule: a
                roster is adopted only if a strict majority of the voters the
                ADOPTER already trusts endorse it, never of the voters it names.
  Distributed/  WorkerRegistry (the worker set: exact-fingerprint grouping,
                most-free-slots pick tie-broken by utilization, heartbeat
                expiry over IClock) and
                LeaseTable (lease issue/expiry/release plus the in-flight key
                map that suppresses duplicate work). Both pure with respect to
                I/O, which is what lets every capacity and expiry rule be a
                ManualClock unit test rather than a sleep. Named Distributed
                and not Dispatch because RedisResp.cpp already has a Dispatch()
                that collides under unqualified lookup inside namespace FastCache.
                FleetView renders what the leader can see -- members, machines,
                workers, per-tier caches and the lease split -- as one page and as
                JSON, off column tables both renderers walk. Pure with respect to
                I/O for the reason the two above it are: collecting reads the
                scheduler, rendering reads only the snapshot, so every "absent is
                not zero" rule is a unit test over a literal.
                FleetSample carries the slot vocabulary -- FleetMetric and its
                table, FleetFold, FleetBucket -- plus IFleetHistorySink, so
                SchedulerService's header reaches the seam without compiling
                against a ring buffer, a std::map and <filesystem>.
                FleetHistory keeps three rings (minute, hour, day) serving eight
                views, behind one file envelope that carries magic, version,
                CRC32C, temp-then-rename and the refusal to overwrite a file a
                LATER build wrote. FleetNodeHistories is the leader's half of the
                handover: one history per machine endpoint, a high-water mark per
                endpoint so a redelivered heartbeat cannot count twice, and the
                backfill that fills the windows this leader was not elected for.
                FleetChart derives every series from those buckets -- a rate is a
                delta between adjacent buckets that can both ANSWER for it -- and
                renders them as standalone SVG.
                RosterTrust is what a worker that runs no consensus checks a lease
                against (#178): the certified roster it holds, rooted in its
                `--voter-key` anchors until the first adoption and in the roster
                itself after, with RosterStore keeping it in `--cluster-dir` over
                the durable-file seam the Raft store uses. A consensus member uses
                StateLeaseRoster instead -- its applied state IS the roster.
                NodeProof is the handshake a machine joining the fleet proves its
                identity key with on the 0xFC surface (#178): the server signs its
                challenge first, the caller signs the whole transcript, and both
                derive one session key per direction from an ephemeral X25519
                exchange -- pure functions over the transcript, so every field a
                signature covers is a unit test that changes it.
  Protocol/     IProtocolHandler, ProtocolAutodetect,
                Framing/ByteReader (line and length-prefixed), MemcachedText,
                MemcachedMeta (1.6 mg/ms/md/ma/me/mn), MemcachedBinary,
                RedisResp (RESP2), CompileCacheHandler (the executor: custom
                0xFC binary protocol, canonicalize-on-STORE / serve-canonical-
                on-FETCH, leading-key group prefetch), CompileCacheWire
                (header-only, dependency-free: the 0xFC magic/version/opcode/
                status/error tables and their encoders, shared verbatim by the
                daemon, fastcache-cc and the test client) and SurfaceRefusal
                (the ONE way any 0xFC surface answers a refusal: Refuse with a
                row carrying the counter, RefuseWithoutCounter with the reason
                nothing rises, RefuseUntriaged with the issue that will decide
                -- so a scan can tell a decision from an omission, which it
                could not while both were a bare EncodeErrorReply).
                SealedFrameSocket seals every 0xFC frame after a node proof: an
                ISocket decorator wrapped around the connection from the start and
                ENGAGED at the proof, because the loop, the sweeper, the peer watch
                and the progress pulse all hold the connection's socket and a swap
                could not reach them at once. ProvenIdentity is what the proof
                established -- the id claimed and the key it verified under.
  Server/       Connection (per-client coroutine), Server,
                ReactorServerLoop (the server driver), AdminHttpServer (the
                read-only HTTP surface; its routes are a table a caller
                contributes rows to, so the fleet page can be served without
                Server/ ever learning about Distributed/) and AdminCredential
                (the Basic/Bearer scheme table, over ConstantTimeEquals)
  Platform/     IDaemonHost (ForegroundHost / PosixDaemonHost / WindowsServiceHost),
                ISignalSource, DaemonControls (process-wide stop/reload flags),
                CpuAffinity, HostMemory, HostInfo (what a machine IS: OS, version,
                architecture, disk space -- the facts a scheduler weighs),
                ServiceControl (ServiceSpec: what to launch, with which
                arguments, under which name and account -- the seam that lets one
                implementation of "install this as a service" serve more than one
                binary), Terminal,
                InheritedListener (systemd socket activation: LISTEN_FDS/
                LISTEN_PID parsing is pure and unit-tested; adoption applies
                close-on-exec and the shutdown timeouts, which are parameters
                rather than the caller's job),
                Environment (the one place the process environment is read),
                FileTrust (could only an administrator have put a file here?),
                LocalAddresses (is this caller on THIS machine? -- the probe
                behind an ILocalityOracle whose set is refreshed on an interval,
                because a refresh a stranger can provoke is a syscall a stranger
                can bill this machine for, and on Windows it costs ~2 ms),
                NarrowText (what code page this process transcodes narrow text
                through, whether a path built from bytes decodes as UTF-8, and
                the two total conversions for text something ELSE wrote)
  Config/       Config, CliParser, ByteSize, YamlReader (yaml-cpp; ReadYamlConfig
                for the daemon's own shape, ReadYamlSettings for any table -- keys,
                values and line numbers, with nothing typed), FileOptions (applies
                those settings through an OptionSpec table, so a file and a command
                line reach a field by the same applier and precedence is the order
                the two run in), ConfigReloader,
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
                            its TCP client, plus `Platform/Environment.cpp`,
                            `Platform/NarrowText.cpp` and `Platform/Terminal.cpp`
                            (see `_fc_cc_core`), so
                            its help renders and colorizes exactly like the
                            daemon's without linking the library. `Cli/Options`
                            is header-only, so including it costs no build row.
  fastcache-compile-node/   the compile worker AND the peer service (
                            FASTCACHED_BUILD_NODE, default ON) — registers with a
                            scheduler's `--serve-scheduler` endpoint (another node's;
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
                            the reactor and the wire. It runs no `Server`: its
                            framed surfaces are `FrameEndpoint`s over one shared
                            `NodeIoLoop`, and its HTTP surface is
                            `AdminHttpServer`.
  fastcache-cli/            the operator's client (FASTCACHED_BUILD_CLI, default
                            ON, INSTALLED). Reads, writes and measures a running
                            cache from a terminal; `docs/tools/fastcache-cli.md`
                            is its reference. It was absent from this tree and
                            from `AGENT.md`'s app table until #1439 noticed,
                            having shipped and been documented on the Tools page
                            throughout — which is what a missing row of a table
                            called the spec looks like
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
                            `BuildBanner` + `BuildBannerListener` are the one
                            place any of that says which BUILD produced a figure:
                            printed to stderr before any case runs, every verdict
                            drawn from a macro the compiler defines rather than
                            from `CMAKE_BUILD_TYPE`, and each figure marked with
                            what that build makes of it (#1439)
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
