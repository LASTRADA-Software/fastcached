# fastcached - Fast Cache Daemon

## Project Architecture

A layered C++23 server. Each layer reaches its collaborators through a
narrow interface so the whole thing is testable end-to-end against an
in-memory transport.

```
src/FastCache/
  Core/         Errors taxonomy, Clock, Logger, BufferPool, Bytes, Endian,
                Crc32c, MurmurHash3 (128-bit key digest), StringHash, Owner,
                Profiling (Tracy wrappers)
  Async/        Task<T>, Cancellation, ResumeOn, IReactor + TestReactor and the
                platform reactors (EpollReactor / IocpReactor / KqueueReactor)
  Net/          ISocket, IListener, IoAwaitable, IAdmissionControl, SocketAddress,
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
                CpuAffinity, HostMemory, ServiceControl, Terminal,
                Environment (the one place the process environment is read),
                FileTrust (could only an administrator have put a file here?)
  Config/       Config, CliParser, ByteSize, YamlReader (yaml-cpp), ConfigReloader,
                EnvExpand ($VAR/${VAR} in path settings), DefaultConfigPath
                (per-platform config lookup + --seed-config, behind IConfigPathProbe)
  Metrics/      IMetricsSink + AtomicMetricsSink (counter-only by design; the
                dispatch counters separate no-worker from no-capacity because
                one says a fleet is misconfigured and the other that it is too
                small, and summing them hides the first when a fleet is busy)
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
  fastcache-compile-node/   the compile worker (FASTCACHED_BUILD_NODE, default
                            ON) — registers with a scheduler's `--listen-dispatch`
                            endpoint, then answers exactly one verb, `Compile`,
                            on its own port. It takes a *fingerprint* from a job
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

The first two break cross-checkout sharing while every unit test still passes,
the third breaks it the moment two machines differ, and the fourth breaks it the
moment a root is spelled unusually, so `scripts/compile-cache-e2e.sh` (POSIX) and
`run-launcher-e2e.ps1` (Windows) assert all of them end-to-end in CI on both
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
- **A worker must be told its input is already preprocessed.** Having added the
  markers back, `-pedantic` then rejects the markers themselves as a GNU extension
  (`-Wgnu-line-marker`) — the fix for the first defect creates the second. The
  answer is the one ccache and distcc already use: `-x c++-cpp-output`
  (`cpp-output` for C, and *nothing* for an MSVC driver, whose `/E` emits standard
  `#line`). It is a `DriverSpec` column, not a branch, so a fifth driver is a row.

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

`scripts/dist-compile-e2e.sh` asserts the consequence rather than the mechanism:
that a worker's object is **byte-identical** to a locally compiled one. That single
assertion is what fails if either rule is broken, and it is the whole soundness
claim of the feature — an object that differs is stored under a key other machines
then fetch.

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
- **`clang-format` after every change** — use the project `.clang-format`.
- **`clang-tidy` reports must be fixed at the source.** Never silence with `NOLINT` — address the underlying issue. The `clang-debug` preset enables `clang-tidy` automatically.
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
broken" when it means "something else was listening".

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

