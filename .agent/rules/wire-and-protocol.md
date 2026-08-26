# Wire formats, protocol handlers and sockets

Rules for `src/FastCache/Protocol/`, `src/FastCache/Net/` and the shared framing
in `src/FastCache/Core/`.

Read this before touching `CompileCacheWire`, `WireFrame`/`WireFields`,
`CompileCacheHandler`, `TcpClient`, `BlockingSocket`/`BlockingConnector`, or any
reactor socket implementation.

Every rule below has already been a bug.

## Framing

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
## Authentication on the compile-cache port

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
## The Net boundary

- **`Net/` is meant to be lifted out of this tree, so what it may include is a
  table and a test rather than an intention.** The constraint was already written
  down -- "`Net` must not depend on `Core`, so `ConnectTcp` takes host and port
  separately" -- and honoured for *new* code, while ten edges that predated it sat
  there untouched (issue #100). That is the shape the constraint will always fail
  in: an include graph drifts in silence. Nothing fails, nothing warns, no test
  goes red, and the edge is discovered by whoever finally attempts the lift.
  `ctest -R net-boundary` is the answer, and four things about it are
  load-bearing:
  - **`Async/` travels WITH `Net/`, and that decision had to come first.**
    `ISocket::Read`/`Write` return `Task<T>`, `IoAwaitable` is the reactor's
    completion hook, and `EpollSocket`/`IocpSocket`/`KqueueSocket` are the
    reactors' own I/O side -- there is no `Net` without the awaitable vocabulary.
    Moving that vocabulary into `Net/` instead is the alternative, and it is worse
    twice over: it leaves `Async/` -- a general coroutine and event-loop library --
    unusable without `Net/`, or it duplicates `Task`.
  - **Three `Core/` leaf headers travel too, each a row with a reason, and the
    check verifies they are still leaves.** `Core/Clock.hpp` (`IClock` and
    `TimePoint`, which every deadline in `Net/` and every timer in `Async/` is
    expressed in -- carrying a second clock interface would fragment the one seam
    the whole codebase injects), `Core/Ranges.hpp` (`FindOrNull`, a toolchain
    shim rather than a domain type) and `Core/Profiling.hpp` (the `FC_ZONE_*`
    macros, which expand to `(void) 0` and carry no code at all). The row is only
    safe while the header depends on nothing, so the check reads each one and
    fails if it has grown a `FastCache/` include: a leaf that quietly gained one
    would drag the whole of `Core/` back across the boundary while still passing.
  - **An edge is closed by moving the file to the layer that owns it, not by
    widening the table.** `Core/Errors/NetError.hpp` became `Net/NetError.hpp` --
    it is `Net`'s own taxonomy and sat in `Core/Errors/` only because that is
    where the taxonomies were shelved. `Net/Framing/LineReader` became
    `Protocol/Framing/LineReader`: it fails with `ProtocolError`, its caps are a
    session's caps, and its own doc lists the protocol handlers as its callers.
    `Net/InheritedListener` became `Platform/InheritedListener`: it reads the
    environment and checks a pid, and handing back an `IListener` does not make
    socket activation a network primitive. In all three the dependency was
    pointing the wrong way round, and moving the file makes `Protocol -> Net` and
    `Platform -> Net` the directions that were always intended.
  - **The check is a scan of the include graph, deliberately, and not a target
    that compiles the set.** Compiling it means a second full build of `Net/` +
    `Async/` in every configuration on every platform, and a staged include root
    copied at configure time goes stale exactly when a header changes -- which is
    the moment the answer matters. The scan reads the same graph the compiler
    would, from the sources, in milliseconds; combined with this project's
    separate rule that public headers are self-contained, a set closed under
    inclusion is a set that compiles standalone. It was verified by running it
    against the tree as it stood before this work, where it names all ten edges.
  - **Test sources are out of scope and that is a decision, not an oversight.**
    What gets lifted is the library. `Net/HealthProbe_test.cpp` drives the
    daemon's own `AdminHttpServer`, which is the entire point of that case;
    gating it would force either a second `AdminHttpServer` fake inside `Net/` or
    the loss of the one test that proves the probe works against the real thing.
    (This is also why the issue's own edge count was high: it counted `_test.cpp`
    files, so `Core/Bytes.hpp` and `Core/Logger.hpp` appeared on the list while
    never being reachable from production `Net/` code at all.)

## Sockets

- **Three implementations of one TCP client, and the rot was in the one nobody
  built.** `Net/BlockingConnector` dialled non-blocking through `getaddrinfo` and
  was coroutine-aware; `fastcache-cc` carried a synchronous `Cc::ITcpClient` with
  its own `SetIoTimeouts` copied from `Net/BlockingSocket` and a comment saying it
  could not be shared; and `compile-cache-testclient` carried a hand-written class
  that was `inet_pton(AF_INET)`-only (so a hostname could never work), unbounded,
  unprotected against SIGPIPE while STOREing whole object files, and **did not
  compile on POSIX at all** (issue #84). The third was invisible because
  `FASTCACHED_BUILD_TESTCLIENT` defaults OFF and no job turned it on, so nothing
  ever discovered that two of its methods named functions that did not exist
  there. `Net/TcpClient` is now the only one. Consequences that are each
  load-bearing:
  - **`Net` must not depend on `Core`, so `ConnectTcp` takes host and port
    separately.** `Net` is meant to be liftable out of this codebase, so it does
    not reach into `Core/HostPort` for a grammar its caller can apply first —
    which also keeps the one parser one parser, since `rfind(':')` picks the wrong
    colon in `[::1]:7000`. The join lives one layer up in `Cc::DialEndpoint`,
    because six call sites across the launcher and the node were otherwise about
    to write it out separately. It refuses a **bare port**, which
    `ParseEndpoint` would accept by supplying a default host: that is right for a
    bind address an operator types and wrong for a dial, where text with no host
    is a misconfiguration and quietly trying loopback turns a typo into a
    connection to whatever happens to be listening locally.
  - **The partial-transfer loops are coroutines because `ISocket::Read`/`Write`
    are awaitables, and synchronous callers drive them with `SyncRun`.** That is
    sound over a blocking socket and nowhere else: such a socket resolves every
    awaitable inline, so the task is never left suspended, which is the one thing
    `SyncRun` refuses to read from. `RaftPeerTransport` already relied on exactly
    this. Over a *reactor* socket the awaitable really does suspend and `SyncRun`
    throws — the defect `cluster-e2e` found the first time consensus was run.
  - **`IConnector::Connect` grew an `ioTimeout`, with no default argument.** A
    dial that succeeds says the peer accepted and nothing about whether it will
    ever answer, and a peer that accepts and then goes quiet parks the calling
    thread forever — which for the launcher turns an optional cache into a
    build-stopping dependency. Bounding it has to happen before the first read, so
    it belongs where the socket is minted rather than in a step every caller has to
    remember. No default, because a default argument on a virtual binds statically
    and would silently differ between a call through the base and one through the
    derived type.
  - **The launcher still does not LINK `FastCache`, and that rule survived
    intact.** The four `Net` rows added to `_fc_cc_core` reach only
    `Net/NetError.hpp`, `Async/Task.hpp` and `Core/Profiling.hpp`, all
    header-only and all std-only, so the launcher stays free of yaml-cpp, OpenSSL
    and the reactor and can still link the CRT statically.
  - **`std::array`'s iterator is a raw pointer on libstdc++ and libc++ and a class
    on MSVC**, so `readability-qualified-auto` asks for `auto const* const` while
    MSVC cannot deduce it — no spelling of `auto` satisfies both. The lookup
    returns the row by value instead of picking a side. Found by building Windows
    immediately after Linux rather than in CI a phase later, the same ordering that
    the `ParsePort` entry above exists to argue for.

- **A daemon that ignores SIGPIPE process-wide hands that decision to every
  program it launches.** `Detail::EnsureNetworkInitialised` did
  `::signal(SIGPIPE, SIG_IGN)` the first time anything touched the network, which
  keeps a broken-pipe write from killing a server and is wrong for any process that
  also spawns a child: an ignored disposition is **inherited across exec**.
  `fastcache-compile-node` links this library, listens on a socket and then runs a
  compiler per job, so it was handing every one of those compilers a disposition
  they never asked for — which is precisely what `fastcache-cc` is documented
  as having to avoid, for the same reason, reached by a different route. Nothing
  in the parent misbehaves, which is why it went unnoticed. Suppression is
  per socket now (`Detail::ArmNoSigPipe`: `SO_NOSIGPIPE` on macOS and the BSDs,
  `MSG_NOSIGNAL` per send elsewhere, process-wide only where neither exists),
  applied at each implementation's single construction funnel. Two things worth
  keeping:
  - **Removing a process-wide safety net exposes every raw sender that was
    leaning on it, and they had to be found by grep rather than by test.**
    `EpollSocket` already passed `MSG_NOSIGNAL` on all three of its sends;
    `KqueueSocket` passed `0` on all three and so needed its descriptor armed;
    `HealthProbe` owns a bare socket and needed the same. `Stats.cpp` writes a
    regular file and the reactors' self-pipes are internal, reachable only through
    a lifetime bug a stray write would already be.
  - **Both regression cases were verified by reintroducing the defect.** Removing
    the send flag terminates the test binary with **signal 13**, exactly as issue
    #68 records for the launcher; restoring the process-wide ignore fails the
    disposition assertion instead. A regression test for a fatal signal that
    cannot be seen to fail is worth nothing.

- **A listening socket claims its address, and the option that says so is spelled
  differently on each platform.** `Detail::BindAndListen` set `SO_REUSEADDR`
  unconditionally, commented "so restart-after-crash rebinds without TIME_WAIT
  delay" -- POSIX reasoning about an option that does not mean the same thing on
  Windows. On POSIX it only lets a bind step over a `TIME_WAIT` left by a **dead**
  socket, and a live listener still holds its address alone; on Windows it lets a
  second socket bind an address a **live** socket already holds, which is the
  documented reason `SO_EXCLUSIVEADDRUSE` exists. So on Windows any process on the
  box -- unprivileged -- could bind the port `fastcached`, a compile node, or
  either one's admin endpoint was already serving, with which of the two answered a
  given connection undefined: for a compile cache reached without a credential that
  is object injection into everybody's build, and for `/metrics` it is a scrape
  surface an attacker can answer (issue #85). It is `ExclusiveBindOption` now --
  one intent, each platform's own spelling. Four things worth keeping:
  - **The `TIME_WAIT` concern the old comment raised is not what was traded away.**
    Measured on Windows 11, across processes: a fresh process rebinds a listening
    port while a connection its crashed predecessor accepted is still in
    `TIME_WAIT`. A listening socket that never accepted does not enter `TIME_WAIT`
    itself, which is what the comment had actually been reasoning about.
  - **Sharing a port on purpose is still opt-in, and it is a different option.**
    `ReusePort::Yes` (`SO_REUSEPORT`, POSIX only) is what lets N reactor threads
    bind one port and have the kernel load-balance across them. Exclusivity is the
    default, not the only setting, and both halves are asserted -- the second bind
    refused, and two `ReusePort::Yes` listeners sharing.
  - **A `setsockopt` carrying a security property is not best-effort.** It fails
    the candidate rather than being ignored the way `TCP_NODELAY` and
    `IPV6_V6ONLY` are: a daemon that silently came up shareable is worse than one
    that visibly did not come up at all.
  - **The discovery beacon's UDP socket keeps `SO_REUSEADDR`, and it is a
    different question rather than the same one answered differently.**
    `OpenUdpSocket` binds the wildcard on the *shared* beacon port so every node
    on the segment hears the broadcast, and claiming that port exclusively would
    let the first process on a host lock every other one out of hearing beacons
    at all. What sharing it does **not** already buy is two nodes on one host
    completing the handshake: measured on Windows 11 and on Linux, two
    `SO_REUSEADDR` sockets on one port both receive a broadcast and only one
    receives a unicast -- and the challenge and the proof are both unicast to
    `received->from` (`DiscoveryService.cpp`). Co-hosted nodes therefore see each
    other's beacons and silently never finish proving the key. That is a defect
    in discovery, not in the bind option, and it is not this change's to fix.

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
## Dialing, and the reactor underneath it

- **A synchronous dial spends a thread the caller does not own, and the argument
  for it reasoned about the wrong thing.** `IConnector::Connect` blocked, and its
  header defended that at length: the caller has nothing to do until the
  connection exists, so a coroutine "would buy the ability to interleave work
  that does not exist", with one rule holding it up -- **a reactor thread never
  calls this**. Three things were wrong with it, and the third had already
  shipped:
  - **The caller has nothing to do; the THREAD has thousands of other
    connections.** The argument described one caller's own work and silently
    ignored whose thread it was spending. That is the same mistake, in the same
    direction, that `Net/PlatformListener.hpp` records for the accept side.
  - **The rule was not free, it was paid for.** `RaftPeerTransport` owned a
    thread per peer *because of this interface*, and it could not be made safe:
    thread-per-peer is only expressible over a blocking socket, because the write
    was driven by `SyncRun` -- which this list already records throwing the
    instant its task really suspends. So the decision pinned one component's
    outbound half to `BlockingSocket` for good while its inbound half was already
    on the reactor: two socket implementations in one class, chosen by direction.
  - **It hid a hang that killed nodes.** The transport passes no I/O timeout,
    deliberately, so over a blocking socket there is no `SO_SNDTIMEO` and a peer
    that accepts and then stops reading parks the sender inside `::send` once the
    buffer fills. `Stop()` cleared the outbox, notified a condition variable the
    sender was not waiting on, and joined it unconditionally -- and the socket was
    a LOCAL of the sender, so nothing else could reach it to close it.
    `~RaftPeerTransport` blocked forever and the node died to SIGKILL, which is
    the `systemctl stop` escalation this list already records once, reached from
    the outbound side.

  And the rule never covered the worst case anyway: `connectTimeout` bounds the
  dial, while **name resolution runs first and is bounded by nothing**, because
  `getaddrinfo` takes no timeout. For `fastcache-cc` that is every translation
  unit in a build waiting on a wedged resolver with no knob anywhere.
  Consequences that are each load-bearing:
  - **`Connect` takes `std::string` by value, not `string_view`.** A coroutine
    frame outlives the call expression, so a view names storage the caller may
    already have destroyed -- the hazard `Net/TcpClient.hpp` records for reference
    parameters, reached by another route. clang-tidy enforces the reference half
    (`cppcoreguidelines-avoid-reference-coroutine-parameters`) and cannot see the
    view half, which is why it is written down. The copy is one the threaded
    resolver needed anyway.
  - **`ioTimeout` left the interface.** It is `SO_RCVTIMEO`, which bounds a
    *blocking* syscall and is inert on a socket whose reads suspend -- so keeping
    it would hand every reactor caller a bound that does not exist, which is worse
    than having none. It survives as `BlockingConnectorOptions::ioTimeout`. A
    reactor caller arms a `DeadlineTimer` that closes the socket instead, which is
    strictly *more* than the option gave: the option bounds one call, so a peer
    dribbling a byte at a time could still take forever.
  - **The budget is divided across candidates, and both halves are needed.**
    Giving every candidate the full timeout means a caller asking for two seconds
    waits four -- a bound that multiplies by however many addresses a name happens
    to have is not a bound. Giving the FIRST candidate all of it defeats the
    fallback whenever that candidate black-holes rather than refuses, which is the
    AAAA-on-a-machine-with-no-IPv6 case trying every candidate exists for. Found
    on Windows, where a closed loopback port is silently *dropped* rather than
    reset: the dead candidate consumed the whole budget and the real one was never
    tried. The test had been passing only because each candidate previously got a
    fresh timeout.
  - **`DialEndpointBlocking` takes a `BlockingConnector&`, never an
    `IConnector&`.** Every remaining `SyncRun` is sound only because the socket
    underneath resolves inline, and the failure when it does not is a
    `std::logic_error` thrown from inside a heartbeat thread. A comment saying so
    is a rule somebody breaks; the type is the rule.
  - **A literal address never reaches a resolver thread.** Every internal dial
    here is to one -- Raft peers, `127.0.0.1:6674`, an endpoint discovery proved --
    and the launcher makes one per translation unit, so a thread hand-off on that
    path would be a real regression. It is also what lets the whole connect path be
    tested without a thread existing. The pool is fixed at two (never one per dial;
    never sized to cores, since this is I/O-bound) and its queue is bounded and
    *refused* rather than waited on, which is the same shape as the pre-auth
    payload cap.

- **Four defects sat between the reactor and a dial that could work, and three of
  them were already latent.**
  - **`EpollReactor` routed only `EPOLLIN` and `EPOLLOUT`, and dropped
    `EPOLLERR`/`EPOLLHUP`.** Those arrive whether or not they were requested, and a
    failed connect can be reported with neither direction set -- so the dial would
    never be told, and because the fd is level-triggered it would be re-reported on
    the very next iteration: a hang AND a loop spinning at 100% CPU, with nothing
    logged at either end. `EpollFdHandler::onError` is where an error goes now, and
    `SelectEpollCallback` is a pure function precisely so the rule is unit-testable
    without a socket or a way to provoke a kernel error.
  - **That same loop read `handler->onWritable` after `onReadable` may have freed
    the object the handler lives in.** It services at most one callback per fd per
    iteration now; level-triggering re-reports whatever was skipped, so the cost is
    one extra turn.
  - **`TestReactor::Submit`/`Schedule` touched bare containers** while `IReactor`
    documents both as callable from any thread. Nothing noticed while every
    producer was the test's own thread -- and every primitive added here crosses
    threads by definition, so a double that cannot be used the way its interface
    reads forces each of those cases onto a real reactor, where nothing is
    deterministic.
  - **`IListener` had no `BoundPort()`**, so only `BlockingListener` could answer
    "which port did I actually get" -- the question every caller binding port 0 has
    to ask, and the one every script-driven test here relies on.

- **The dial's own residuals are recorded rather than dressed up.**
  - **The handler detach in `SettleDial` guards the reactor's loop against a spin,
    NOT the socket built afterwards.** It looks as though it should guard both: the
    socket's constructor attaches the same fd, epoll refuses that with `EEXIST`,
    and the failure is ignored. But `UpdateInterest` uses `EPOLL_CTL_MOD` with a
    fresh `ev.data.ptr`, so the socket's first armed read overwrites the stale
    registration. Verified by removing the detach and watching the byte-transfer
    case still pass. Claiming otherwise would send the next reader looking for a
    bug that is not there.
  - **A loopback connect completes INLINE, so the readiness path is unreachable
    from an ordinary test.** `::connect` returns 0 and the whole
    attach/park/settle block is skipped, which means a dial test that stops at
    "connected" exercises none of it. Provoking it needs a filled accept queue.
  - **On IOCP an accept must be awaited while it is outstanding.**
    `IocpListener::Accept` issues `AcceptEx` immediately but records the awaitable
    only in its suspend callback, so a completion arriving before anyone awaits is
    dropped and the accept never resolves. Arming the accept before a dial and
    awaiting it after -- which reads naturally and works on epoll -- deadlocks.
  - **Both connector tests move BYTES, and arrange the read to park.** `Read` tries
    the syscall before suspending, so a read finding data or EOF already waiting
    would be answered perfectly well by a socket the reactor was never told about.
    Only a read with nothing to return proves the registration exists -- and on
    Windows only a real transfer proves `SO_UPDATE_CONNECT_CONTEXT` was applied,
    without which the socket is connected and every ordinary call on it fails.

- **`ConnectEx` needs two steps `AcceptEx` does not, and neither had precedent
  here.** The socket must be `bind`-ed to the wildcard of its family before the
  call, or it fails with `WSAEINVAL` and names nothing; and
  `SO_UPDATE_CONNECT_CONTEXT` must be applied afterwards, or the handle's context
  stays unset and `getpeername`, `shutdown` and the ordinary calls all fail on a
  socket that is genuinely connected. The extension pointer is cached in a two-row
  table keyed by family, because a connector -- unlike a listener, which has one
  family -- dials whichever the resolver hands it. `IocpSocket` therefore takes an
  `IocpAttachment`: `ConnectEx` requires the port association BEFORE the operation
  is issued, and a second `CreateIoCompletionPort` on an associated handle fails,
  so without it the constructor would report `IsAttached() == false` and tell the
  caller to abandon a connection that works. And **`overlapped.Internal` is an
  NTSTATUS, not a WSA code**: the reactor hands it over as-is, so `0xC0000236`
  (refused) falls through every `WSAE*` row onto `SystemError` -- useless to a
  connector whose job is to tell refused from unreachable. `WSAGetOverlappedResult`
  is the documented conversion. The same wart affects IOCP reads and writes today
  and is left alone deliberately: their `Dispatch` cannot reach the socket handle.

## Socket and coroutine lifetime

- **`Close()` can be the last thing that runs on a socket, so it must touch no member
  after it completes an awaitable.** `EpollSocket::Close` walked `{readOp, writeOp}`
  and completed each parked awaitable inside the loop. Completing one resumes the
  coroutine that was waiting on it -- and a coroutine that OWNS the socket
  (`ServePeer` holds it in a by-value `unique_ptr` parameter) then runs to its end and
  destroys it, so the loop's next iteration reads `_impl->writeOp` out of freed
  memory. Reported by ASan as a heap-use-after-free from `RaftPeerServer::Shutdown`,
  which is the one caller that closes accepted connections from outside their own
  coroutines. The awaitables are detached from the ops first and completed last now,
  with `_fd` cleared before them. Two things worth keeping: the parked awaitables live
  in their coroutines' own frames rather than in `_impl`, which is what makes it safe
  to complete the second after the first has taken the socket down; and the same edit
  went to `KqueueSocket::Close`, which was a copy of the same loop and had the same
  defect on a platform where nothing had ever run a sanitizer either.
- **A wait that cannot be cancelled is a frame that cannot be freed, so `Schedule`
  grew a counterpart.** `IReactor` could park a coroutine on a deadline and had no
  way to take it back, which is why `DeadlineTimer` and `InterruptibleSleepUntil`
  poll in bounded steps rather than parking once: a disarmed timer then retires at
  its next tick instead of never. What that still leaves is a frame parked *during*
  the tick, and a reactor destroyed in that window frees nothing --
  `IReactor::Run` returns with its timer heap exactly as it was, deliberately. One
  leaked coroutine frame **per dial and per cache exchange**, harmless in a daemon
  that outlives them and fatal for `fastcache-cc`, where an ASan build then exits
  non-zero and turns every cached compile into a failed one.
  `IReactor::CancelPending` closes it: `Disarm()` takes the timer's own handle back
  off the reactor and destroys it there and then. Five things are load-bearing:
  - **The return value is an ownership transfer, not a status.** `true` means THIS
    call removed the handle, so the caller is now the only one who may resume it;
    `false` means the reactor still has it -- running, or already queued -- and the
    caller must not touch it. That is what makes the race against a timer firing
    concurrently decidable instead of a guess, and it is decided under the same lock
    the timer heap is popped under.
  - **The frame is DESTROYED, not resumed, and that is the difference between
    fixing the leak and moving it.** Resuming only queues it, and the caller that
    disarms is typically about to stop the reactor on its very next line --
    `ReactorExchange` does exactly that -- so the queued handle would never run. The
    first version resumed, and ASan reported the same leak in the same place.
  - **`DeadlineTimer` had to stop awaiting a nested `Task`.** Awaiting one parks the
    INNER coroutine's handle, so the handle the timer recorded would not be the one
    `CancelPending` has to name. Its wait is written out against `SleepUntil`
    directly, and the handle is captured by an awaitable whose `await_suspend`
    returns `false` -- recorded on the way past, without suspending, so it is
    available even in the window before the first hop onto the reactor. It is also
    what makes destroying it sound: the handle is the whole chain, and a
    `DetachedTask` leaves no awaiter holding it.
  - **The first attempt was for the reactor to free what it still held at teardown,
    and it is unsound.** `Submit` and `Schedule` convey no ownership: a `Task` local
    in a test parks its handle and then destroys its own frame at scope exit, so the
    reactor -- destroyed afterwards -- destroyed it a second time. ASan named it
    immediately, which is the argument for the sanitizer fix below in one line.
  - **IOCP cancels timers and not submissions**, because a submission there is a
    completion packet already posted to the kernel and no call takes one back. It
    answers `false` for that case, which is the honest answer under the rule above.

- **A struct returned BY VALUE from a decoder must not borrow from what it decoded.**
  `DecodeCapacity(EncodeCapacity(x))` is the obvious spelling and was a
  use-after-free the moment `CapacityFields` grew one `string_view` member: the
  encoded buffer dies at the semicolon and the view outlives it. Nothing in the type
  warned anybody -- `RegisterView` says "View" precisely because it borrows, while
  `CapacityFields` is used for both directions and reads as a value, so a borrowing
  member turned every existing call site into a trap without a single name changing.

  The decode side of a value-returning record **owns** its bytes. A registration is
  once per node; the copy is free, and the alternative is a hazard in a header every
  binary compiles.
  - **It passed on libstdc++ and failed on libc++**, which is the whole difficulty:
    the read only misbehaves once something reuses the freed block, and which
    allocator does that promptly is a property of the platform. A local ASan run
    reproduces it only if the test *churns the heap* between the decode and the read
    -- without that, ASan reports nothing and the bug looks absent.
  - **The regression test decodes from a temporary on purpose**, and a second one
    scopes the buffer and reads every field after it is gone. A rule this shape
    cannot be left to a case that happens to exercise it.
