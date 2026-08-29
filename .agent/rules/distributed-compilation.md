# Distributed compilation

Rules for `src/FastCache/Distributed/`, `src/apps/fastcache-compile-node/` and the
dispatch half of `src/apps/fastcache-cc/`: preparing a translation unit for a
remote worker, running a worker as a service, the scheduler's gate, the node's
own cache tier, and what the end-to-end fixtures assert.

Read this before touching `SchedulerService`, `WorkerRegistry`, `LeaseTable`,
`Node::LocalCache`, `FrameEndpoint`, `DispatchPreprocessCommand`, or the worker's
`main`/`WorkerBody`.

One failure shape dominates this file and is worth naming up front: **distribution
appearing to work while never once helping** — every dispatched unit fails
remotely, falls back to a local compile, and the build goes green. Several rules
below exist only because that shape is invisible from both ends.

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
  - **A membership flag is the NODE's, not the scheduler's, and a startup rule that
    assumed otherwise closed every worker.** One `NodeMembership` serves all three
    surfaces — the scheduler, the cache tier and the compile port — and `WorkerServer`
    is constructed unconditionally, so `--fleet-member` / `--fleet-open` are consulted
    on every node there is. `StartupPolicyRejection` nonetheless refused them without
    `--listen-scheduler`, reasoning that "a policy nothing consults is a policy an
    operator believes is in force" — and the premise was simply false. What the row
    achieved was pinning every non-scheduler node's oracle to an *empty list*, which
    admits loopback and nothing else, so the worker the getting-started page called
    "the whole of it" refused every dispatched compile with `NotAMember` (#235). A
    rule stating what a flag is *for* is a rule that has to be re-derived when a
    second surface starts reading it — so check which tiers reach the value, not
    which flag it reads like.
  - **Admitting peers and advertising an address are two halves of one decision.**
    Peers are admitted so that they can *dial* this worker, and `--advertise` falls
    back to `{--bind}:{--port}` whose bind is the wildcard — which the scheduler
    hands to clients verbatim, so a client on another machine dials `0.0.0.0` and
    reaches itself. `NodeServiceRejection` had always refused that for an *install*;
    letting a worker carry a membership flag at all is what made it reachable from a
    hand-started one, so the startup table gained the row in the same change.
    Deliberately scoped: a node that registers **nowhere** and admits peers to its
    cache tier is reached at `--listen-cache` and needs no advertise, and a node with
    no membership flags is the one-machine deployment and is correct as it stands.
    Only the wildcard is refused, never an address that might not resolve — a host
    that is down today can be right at the next boot (#208), while the wildcard is
    wrong on every machine and forever.
  - **A refusal one hop past the lease is invisible from the side anybody watches.**
    The lease **was** granted, so every scheduler counter is correct and unmoved; the
    only signal is the *worker's* `WorkerJobsRefusedNotAMember`, on a machine whose
    operator has no reason to scrape it and which exports nothing at all without
    `--admin-listen`. "No counter moves" is the tempting summary and it is wrong in a
    way that matters — it sends a reader looking for a missing counter instead of at
    the one that exists. What was actually missing is a line at **startup**:
    `AdmissionSummary` is one phrase for the worker's ready line and the scheduler
    tier's, so a node that admits nobody says so while an operator is watching, and a
    node running both surfaces states one policy rather than two.
  - **The oracle is a seam and not a call into `Cluster::PeerDirectory`.** The
    dependency would run the wrong way — `Distributed` is the policy, `Cluster` is
    one way of establishing the fact it needs — and the answer is *deployment*-shaped
    rather than universal, which is what an interface is for. `Publish` is a setter
    and one of the documented carve-outs to configuration-at-construction: membership
    is precisely what changes while the object lives, and rebuilding the oracle per
    join would mean handing a new one to a running server.
- **A dispatched compile is bounded by how long a COMPILER runs; a cache exchange
  by a round trip. One number cannot be both.** The launcher armed a single deadline
  on every exchange it made and handed the dispatch dialler the cache's -- ten
  seconds, which is right for a `FETCH` answered out of memory and is the wrong
  question entirely for a `COMPILE`. A worker writes nothing until the compiler has
  finished, so the client sits in **one read** for the whole remote compile, and every
  translation unit slower than ten seconds was abandoned mid-compile and rebuilt
  locally — precisely the set distribution exists for. The worker meanwhile ran the
  job to completion and wrote back an object nobody read, so the CPU was spent twice,
  the network carried a wasted object, and `leases_granted` kept rising: this file's
  own headline failure, distribution appearing to work while never once helping.
  Measured at 23.5 s of worker CPU and 84 MB returned to a client that had already
  given up (#223). `DispatchBudgets` is the split — `control` for the scheduler's
  `LEASE`/`RELEASE`, `compile` for the worker — and the two knobs are
  `FASTCACHE_TIMEOUT_MS` and `FASTCACHE_DISPATCH_TIMEOUT_MS`. Four things about the
  shape are load-bearing:
  - **A per-call socket ceiling is not a bound, and the seam is where that becomes
    sayable.** `SO_RCVTIMEO` bounds one `recv`, so a peer dribbling a byte before each
    expiry holds a build forever without ever exceeding it — and a *dialler* seam can
    arm nothing else, because all it hands back is a socket. Bounding the exchange
    needs a `DeadlineTimer` that CLOSES the socket, which needs the reactor the
    exchange runs on. So dispatch asks for an **exchange** (`IEndpointExchange`) and
    hands over the budget it must finish inside, which is `ReactorExchange` — the one
    the cache path already used. A second mechanism here would have been a second
    thing to get wrong.
  - **A bigger constant only moves the cliff, so the default is anchored on something
    real.** Ten minutes, because that is `LeaseTable::DefaultLeaseTimeout`: above it a
    client is waiting on a lease the scheduler has already reclaimed and may have
    re-granted for the same key, and below it a compile the fleet is still holding
    capacity for is thrown away. The two cannot be `static_assert`ed together — the
    launcher deliberately does not link the library the scheduler lives in — so moving
    either means moving both.
  - **A flat deadline cannot tell "still working" from "gone", so raising it is a
    REGRESSION as well as a fix, and it is stated rather than absorbed.** The same
    number is how long a legitimate compile may take and how long a genuinely dead
    worker goes unnoticed, and it went from ten seconds to ten minutes -- sixty times
    slower to notice a machine that was powered off, unplugged or suspended
    mid-compile, which on a `-j16` build is a handful of slots and a build that looks
    hung. Sizing for the slow compile is nonetheless the only choice available while
    the worker sends nothing mid-compile: sizing for the dead worker is the defect
    above, reintroduced. Splitting the two needs a periodic progress frame from the
    worker so the IDLE bound can be seconds while the TOTAL stays long, which is a
    wire change (#245). A cheaper partial mitigation exists and is not in the
    launcher's reach: TCP keepalive with real intervals on the dispatch socket
    detects a dead HOST in under a minute, but the option has to be armed where the
    native handle is (`Net/`), not where the budget is chosen.
  - **Zero means UNBOUNDED, and the arithmetic alone says the opposite.** A zero total
    puts the deadline at `Now()`, so every exchange dies on the reactor's next turn —
    a knob documented as "turn the ceiling off" that turns the *cache* off instead,
    silently, because every caller answers a transport failure by compiling. The
    timer is not armed at all below one millisecond.
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
- **A lease has THREE transitions and expiry is only the third.** `Acquire` takes
  one, `Release` resolves it, and the lifetime running out is the safety net for a
  client that *died* — `Ctrl-C` on a build. For a long time only two of the three
  were built: there was no wire verb for resolving, so `LeaseTable::Release`,
  `ReleaseWorker` and `WorkerRegistry::JobFinished` had no production caller at all,
  and every key stayed suppressed for the full 600-second timeout. Recompiling one
  translation unit inside ten minutes of dispatching it was refused
  `AlreadyInFlight` and fell back to a local compile, while `inFlight` only ever
  climbed between heartbeat overwrites. **The client sends `Release`, not the
  worker**, and on *every* path out of the compile — an object built, a job refused,
  a worker that was not there — because the client is who the lease was issued to
  and the only party that sees all three. That is why the compile half of `Dispatch`
  is a function: four early returns are four places to forget it. The release rides
  a **fresh** connection; the scheduler sweeps one idle for five seconds and a
  compile is longer, so reusing the lease socket would fail exactly when there was
  something to release.
- **A resolve answers on liveness, never on presence, and an unknown token is
  refused.** Nothing visits an expired token but an `Acquire` for the same key, so
  an expired entry is still sitting in the table when its holder finally reports —
  and calling that a successful release would leave the one condition worth naming,
  a job that outlived its lease, with nowhere to be observed. The entry is dropped
  either way, through the one helper `Release` and `ReleaseWorker` share: the key
  index is erased only when it still points at *this* token, and two copies of that
  guard is how one of them comes to evict the client that replaced the lease.
- **A refusal is only ever as good as what the table it consults can SAY.**
  `RemoteCompileArgs` refused any command line naming the input language, on sound
  reasoning: the launcher appends "this text is preprocessed <language>" LAST so it
  wins, and silently overriding a language the build chose is a wrong object rather
  than a failed one. But `LanguageSelectors` recorded only *that* a language was
  named, never *which* -- so refusing was the best answer available to it, and the
  comment describing the language as coming from an explicit flag, then the driver
  default, then the extension described an order only its last two thirds
  implemented.
  The cost was total and silent: **CMake emits `/TP` on every C++ source it compiles
  with MSVC**, so no CMake + MSVC translation unit was ever dispatchable. The fleet
  cached normally and distributed nothing. Nothing anywhere said so, because no lease
  was ever *requested* -- `granted`, `no-worker` and `no-capacity` all read zero, and
  the dashboard showed registered workers with every slot free. A fleet that has
  never distributed anything is indistinguishable from an idle healthy one.
  The row carries the language now, so a selector that names one is folded into the
  language and **dropped** -- what the launcher appends says the same thing and more.
  Three shapes, and only the first changed: a spelling that names a language is
  dropped; `/Tc<file>` and `/Tp<file>` are still refused because they name a FILE,
  which is the build's own source path; and an `-x` value with no exact
  `SourceLanguage` is still refused, because guessing is how a worker comes to
  compile something other than what was asked for.
  The test is a **CMake-shaped** command line, not a minimal one. The hand-written
  argv the old test used is what let this through: the refusal reads as correct on
  `cl /c /TP a.c`, and the line CMake actually emits is the one nobody wrote down.
- **A worker being dropped has to be an EVENT, or nothing can release what was held
  against it.** Registry expiry used to be a filter — `IsLive` hid a dead worker
  from `Pick` while its entry stayed in the map forever — so `ReleaseWorker` had no
  moment to be called at, and a fleet losing a machine went on refusing every client
  that missed on one of its keys until each lease timed out. `ExpireStale()` erases
  and *names* what it erased; the registry does not touch the lease table itself,
  because that is its sibling rather than its dependency, so `SchedulerService` pairs
  them — from `Lease` and nowhere else, that being the one decision the leftovers
  corrupt. Reaping cannot see the second route to the same pin: a node that restarts
  inside the heartbeat window keeps its entry, so `Register` releases its leases too,
  on the same reasoning that already resets `inFlight` there.
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

Six more about what the tier IS and who gets to see it:

- **What a node holds back from compiles is what its tier BUILT, never what a flag
  asked for.** `--cache-memory` is a request, and three things grant it and one
  denies it: `--listen-cache=` builds no tier at all, `--cache-memory 0` builds no
  memory half, and a DEFAULT `--listen-cache` already held — by the `fastcached` on
  the same machine, which is where that port points — is a warning the node carries
  on past. None of the three touches `cacheMemoryBytes`, whose default is a quarter
  of RAM, so `NodeCapacityOf` reading the flag reserved 8 GiB on a 32 GiB box that
  cached nothing and offered the fleet 24 slots instead of 32. Under-utilisation
  rather than breakage, and therefore silent forever: nothing anywhere reports a
  reservation for a tier that does not exist. `NodeCapacityOf` takes the
  `NodeCacheCapacity` that `CacheCapacityOf` read off the tier, which is why
  `WorkerBody` derives capacity and `slots` BELOW the tier startup rather than above
  it — and why the record the leader renders and the number the worker enforces are
  now one call rather than a patched copy.
- **Which tiers cost the machine RAM is a column of `StorageTierTable`, not a check
  for `StorageTier::Memory`.** The taxonomy is open — the enum's own comment
  foreshadows a tier on a peer and a tier on a second filesystem — and enumerators
  are appended, so a resident tier added later would land past any hand-written
  check and be reserved as nothing. That direction over-commits the machine, which
  is the swapping-and-OOM failure `MemoryBudgetPerJobBytes` exists to prevent. And
  the reservation is total over the vocabulary: **absent** is a tier this node does
  not run, while **present and zero** is a tier with no ceiling, so a resident one
  is reserved as the whole machine rather than as nothing — the same two meanings of
  zero the bullet below already records.
- **`--cache-memory 0` means no in-memory tier, and zero is how
  `InMemoryLruStorage` spells UNBOUNDED.** `EvictToFit` returns immediately on a
  zero budget, so for as long as the flag existed the one number an operator could
  switch the cache off with was the number that removed its limit — a node told to
  hold nothing held everything, growing until the machine ran out of memory, with
  the cache appearing to work better and better on the way. Both halves off (no
  `--cache-dir` either) means no tier at all, said out loud rather than left to
  produce a cache port answering out of nothing.
- **A store that will not open is always fatal; a port that will not bind is fatal
  only when the operator typed the address.** The two leave `CacheTier` through one
  `std::string`, so `StartCacheTierOrExplain` opens the store itself rather than
  letting `Start` do it — otherwise a bad `--cache-dir` on a node using the DEFAULT
  cache port is logged as a warning and stepped over, and the error names
  `--listen-cache` while pointing at a directory.
- **The tier lives behind a single-shard `ShardedStorage`, and that wrapper is the
  lock rather than any kind of sharding.** The tier is mutated on the reactor thread
  while the heartbeat thread and the `/metrics` scrape read its statistics, and
  `Snapshot()` on these backends writes a `mutable` member. It is the same reason
  the daemon's `useShardingWrapper` includes `metricsEnabled`. A node's `--cache-dir`
  still does its I/O on the shared reactor thread (#136), which is stated where an
  operator meets it.
- **A store file belongs to exactly one open store, and the store enforces that
  rather than documenting it.** `FilePageStore::Open` claims the file — POSIX
  `flock(LOCK_EX | LOCK_NB)`, and on Windows the `FILE_SHARE_READ` share mode it
  already passed — and a second opener gets `StorageErrorCode::InUse`, which is a
  distinct code because the remedy is another process or another path, not a
  repair. Before this, two nodes on one `--cache-dir` (or two daemons on one
  `--storage`) silently lost half their writes on POSIX: 400 committed puts,
  200 readable, no error anywhere. It must stay `flock` and never `fcntl` — an
  fcntl lock is per PROCESS, so a second store inside one process would take it
  again and report success. Nothing is written to the file, so a store stays
  byte-compatible with builds that predate the claim; and a filesystem that
  cannot lock opens anyway and *says so*, because refusing there would break a
  working deployment over something that is not contention.
- **A cache is per NODE, and the registry is keyed per (fingerprint, endpoint).** A
  node with two `--toolchain` flags is two entries against one machine —
  deliberately — and both heartbeat the SAME cache figures, so summing a cache field
  across `LiveWorkers()` counts one node's objects once per toolchain it serves.
  `WorkerRegistry::NodeCaches()` is the deduped view, and it does not pick
  arbitrarily between siblings: `Register` clears a worker's load, so an entry that
  just re-registered holds nothing while its sibling holds last round's figures. One
  that has reported a cache wins over one that has not, most recently seen breaking
  the tie — otherwise a node reads as having no cache for one heartbeat interval,
  intermittently, depending on an `unordered_map`'s iteration order.

The cache facts themselves ride **nested**, inside the capacity and load records
rather than beside them, for the reason those records are nested inside REGISTER
and HEARTBEAT: `SplitFields` is exact by design, so a fact added at any fixed-arity
level makes two builds of a fleet unable to speak at all. Tiers travel
**positionally**, because `CompileCacheWire.hpp` is compiled into `fastcache-cc`
and cannot see `StorageTier` — which makes that enum's ORDER a wire contract, and
makes `CacheCapacityToWire`/`FromWire` the only pair that knows index 1 is the disk
tier. A transposition between them decodes perfectly and reports a node's RAM
budget as its disk budget, so both round trips give every tier a distinct value.
None of it is scheduling input: `AvailableSlots` does not read it and must not
start to, because how full a node's cache is says nothing about whether it can take
another compile.

`NoExpiry` is `TimePoint::max()` and **not** a default-constructed `TimePoint`: the
storage tests `entry.expiry <= now`, so zero means "expired before any clock reading"
rather than "no deadline" — every write landed and was unreadable, a cache that
silently stores nothing while reporting success on every write. `CacheEngine` already
spelled it the same way, which is what makes this a drift rather than a discovery.

- **`FASTCACHE_ADDR` defaults to LOOPBACK, and set-but-empty is the opt-out.** Unset
  resolves to `Cc::DefaultAddr` (`127.0.0.1:6674`), which is where `--listen-cache`
  answers, so the launcher caches against a node on this machine with no
  configuration at all. Three-valued on purpose, and `EnvOr` — which collapses
  set-but-empty into unset — must not be used to read it, or the documented opt-out
  `cmake/portable/CompileCache.cmake` exports becomes the default. A *remote*
  default would be indefensible: every TU on a machine with nothing listening would
  pay a connect timeout in silence, the exact cost `USE_COMPILER_CACHE` probes at
  configure time to avoid. Loopback is not that — a closed port refuses immediately,
  with no timeout and no round trip — which is the whole argument for this address
  and why it could not be another one.
- **A cache answer that dials must not be serialized behind the accept loop.**
  `FrameEndpoint` owned a thread and served its connections ONE AT A TIME, and
  `RemoteUpstream` dials the shared cache from inside a cache answer -- so a single
  upstream that took five seconds held every local `fastcache-cc` behind it, one
  after another. A node whose shared cache was unreachable had an unusable port of
  its own, and nothing anywhere said so: every client eventually got a correct
  answer, just far too late. `NodeIoLoop` is the reactor both framed surfaces now
  accept on, with a `DetachedTask` per connection. Six consequences, each
  load-bearing:
  - **The payload cap became a PER-CONNECTION cap the moment serialization went.**
    Serving one at a time bounded peak memory to a single `MaxRequestBytes()` frame
    by accident; serving them concurrently makes it N of them. So
    `MaxConcurrentRequests()` and `MaxInFlightBytes()` are columns on the responder,
    and they had to land in the same commit as the detach -- a fix that opens a
    memory-exhaustion hole is not a fix. The spread is the point: 256 concurrent at
    64 KiB for the scheduler, 8 for the cache with a 256 MiB total budget that is
    deliberately ONE request's worth, so eight ordinary objects run in parallel while
    a single 256 MiB monster still cannot be joined by seven more.
  - **The request deadline was otherwise silently lost.** It used to be
    `SO_RCVTIMEO`, applied to every accepted socket by `BlockingListener::SetTimeouts`.
    A reactor socket has no such option -- its reads suspend rather than block -- so
    without a replacement a client that connects and sends half a header holds a
    descriptor and a coroutine frame until the process dies. A free slow-loris on the
    node's cache port, created by the change that removed the stall. One sweeper per
    server on a bounded tick, NOT a timer per connection: `IReactor::Schedule` cannot
    be cancelled, so a per-connection timer would stay on the wheel for the full
    interval after its connection had finished.
  - **Every close is posted onto the reactor.** On epoll and kqueue `ISocket::Close`
    completes a parked awaitable by resuming its coroutine INLINE, so closing from
    the stopping thread would run the server's connection tasks there while the
    reactor thread is still driving them. IOCP routes cancellation back through the
    port and does not -- which is exactly what makes it a race that passes CI on
    Windows and corrupts state on the other two.
  - **`Shutdown()` waits for the LOOPS, not only the connections**, and the first cut
    did not. `~FrameServer` then freed state the accept loop and the sweeper still
    pointed into: SIGSEGV, plus a port still bound after the endpoint claimed to have
    stopped. The state is shared with the loops now. Found by the endpoint's own
    destruction test, which is the one that exists to catch exactly this.
  - **The reactor is stopped by its loops, when the last one ends** -- the sweeper
    counted among them, because a frame parked on the timer wheel is precisely what
    `IReactor::Run` must not return over. And `NodeIoLoop` is declared BEFORE the
    tiers in `WorkerBody`, so it is destroyed after them: a tier's destructor posts
    its closes onto this reactor, which therefore has to still be running.
  - **It is NOT consensus's reactor.** A cache answer now awaits a dial with a
    multi-second ceiling, and putting that in front of the Raft heartbeat timer would
    be this same defect moved one layer over -- the failure this list already names as
    nine role changes in twelve seconds. It is also the only node header that includes
    `PlatformReactor.hpp`, which is what keeps `<windows.h>` out of everything that
    includes `FrameEndpoint.hpp`; the endpoint holds an `IListener` rather than the
    platform type, which is what `IListener::BoundPort()` was added for.

- **A worker's compile port had the SAME defect, and the slot cap hid it.**
  `WorkerServer::Run` awaited each compile inline, so a node advertising 32 slots ran
  exactly one at a time: `_inFlight` could never exceed 1, the cap it enforces was
  unreachable, and the scheduler dispatched 32 jobs to a machine that would serve them
  in a queue. Nothing reported it -- every client got a correct object, and the fleet
  page showed a worker that was never busy because it never was. A cap that cannot be
  reached is indistinguishable from a cap that is never hit.
  - **A compile cannot go on a reactor.** It spawns a process and holds its thread for
    seconds, so `ResumeOn { someReactor }` would stall every other coroutine that
    reactor owns -- the same defect one layer over. It goes on a `ThreadPoolExecutor`,
    which is what `IExecutor` was split out of `IReactor` for: `ResumeOn` only ever
    needed `Submit`, and a pool is not a reactor.
  - **The hop is one-way only because the worker's listener BLOCKS.**
    `BlockingSocket::Read` does its `recv` eagerly and returns an already-ready
    awaitable, so nothing after `ResumeOn { _jobs }` suspends and the whole of
    `Serve` stays on the pool thread. Move that port onto a reactor and every read
    would resume the coroutine on the reactor thread -- putting the compile back
    there, invisibly, with no call site changed.
  - **The pool is sized to the cap, and the cap is what refuses.** The pool does not
    bound admission -- it runs what it is given -- so an unsized pool would queue
    silently and hide the overload from the scheduler trying to route around it.
    `WorkerServer` refuses over the cap and the pool has one thread per slot, which is
    what makes an admitted job always find a thread.
  - **The payload cap became a per-connection cap the moment serialization went**,
    exactly as it did for `FrameEndpoint`, and the byte budget had to land in the
    same commit rather than after it. Serving one at a time bounded peak memory to a
    single `MaxRequestBytes` by accident; `slots` jobs each declaring the maximum is
    `slots` times it -- 8 GiB on a 32-slot node, asked for by any cluster member.
    Checked on the DECLARED length before a payload byte is read, and refused with
    `EndpointBusy` and its own counter: slots were free and memory was not, so
    reporting it as `NoCapacity` would send an operator to buy machines that would
    not help.
  - **Concurrency turned a per-call scratch path into a per-thread one.**
    `CompileJobRunner` derived `job-N` from a plain `++`, and the source file and the
    hard-coded `tu.o` both live inside it. Two jobs reading the same counter compiled
    into the same file and one returned the OTHER's object -- which its client then
    cached under its own key. Silent wrong-object delivery is the worst thing a
    compile cache can do, and it was reachable in about one run in six once two jobs
    started together. Anything a worker derives per job is derived per THREAD now, and
    `Run` says in its docs that it may be called concurrently.
  - **A spawned child inherits what the PROCESS has, not what the call set up.**
    `CreateProcess` with `bInheritHandles = TRUE` hands over every inheritable handle
    in the process, and a POSIX fd without `FD_CLOEXEC` survives `exec`. So a
    sibling's compiler held another job's pipe write-end open, that job's drain never
    saw EOF, and it blocked until an unrelated compile finished. Windows names what
    may be inherited (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`) rather than chasing what
    may not -- which also closed a live leak of *accepted client sockets*, since
    `::socket()` returns an inheritable handle. POSIX marks both pipe ends
    close-on-exec, under a lock that also covers the spawn, because the window between
    creating a descriptor and marking it is a window a sibling can spawn in.
  - **A drain must not wake on an atomic it is about to destroy.** `~WorkerServer`
    waited on `_inFlight.wait()`; an atomic wait can return on observing the store
    alone, so the destructor could finish and free the object while the job that
    released the last slot was still inside `notify_all` on a member of it. It waits
    on a condition variable whose notifier holds the mutex, which is what proves the
    notifier is past its critical section before the waiter can run.
  - **A destructor that drains closes the door first.** Waiting without `Shutdown()`
    races the accept loop admitting one more job just as the count reaches zero, and
    the wait then returns while that job is starting. The loop re-checks the flag after
    `Accept()` returns too, since the check at the top of the loop was made before it
    parked.

- **`EndpointBusy` is not `NoCapacity`, and the split is for whoever reads it.**
  `NoCapacity` is a statement about the FLEET -- every matching worker full of this
  build's own work, which an operator answers by buying machines. `EndpointBusy` is
  one node's own front door: it has hit its concurrent-request cap or its in-flight
  byte budget, and the same client asking again shortly will very likely be served.
  Reporting either under the other's code sends an operator to fix something that was
  never wrong, which is the same argument `Withdrawn` already makes for splitting off
  its own case.

- **The launcher runs its cache exchange on a reactor of its own, and gets a bounded
  name lookup for it.** `main.cpp`'s private `TcpClient` is gone -- fifty lines in a
  file that is in no test target, existing only to give four sites an
  optional-shaped `Connect` and to `SyncRun` two loops. What it bought is not
  latency: the launcher is one-shot per translation unit and has nothing to
  interleave. It is that `getaddrinfo` takes no timeout, so `FASTCACHE_ADDR` pointing
  at a name whose resolver is wedged used to stall every translation unit in the
  build with no knob anywhere. Three things about `ReactorExchange`:
  - **One reactor per exchange, asserted rather than documented.** `IReactor::Run`
    returns only on `Stop()` and no reactor here clears that flag, so a reused
    instance performs the first exchange and silently skips every later one -- which
    the launcher answers by compiling locally, making every build slower with nothing
    anywhere saying why.
  - **The deadline CLOSES the socket rather than stopping the reactor.** Stopping
    would leave the coroutine parked on a read nobody completes: a leaked frame and a
    leaked descriptor, once per translation unit. Its test asserts the close, and
    fails without the timer.
  - **`Threads::Threads` is linked and named.** The resolver pool is the one thing
    this launcher genuinely needs a thread for. Not a departure from the rule this
    binary exists to protect -- that rule is about not linking `FastCache` and
    therefore not dragging in yaml-cpp, OpenSSL and the reactor's transitive world --
    and it is named rather than left to glibc folding pthread into libc since 2.34,
    which happens to work on the distributions CI uses.

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

**A dashboard route is a verb, and gets the same leadership answer as the rest of
the surface.** `/fleet` and `/fleet.json` are not registered at all on a node with
no `--listen-scheduler`, so they are a plain 404 rather than a route answering with
an empty fleet -- a process with no fleet view offers no fleet route. Where one
exists, a node that does not lead answers `503` naming the leader, which is
`Gate()`'s `NotLeader` rendered in HTTP. The reasoning, and why it is never a
redirect, is in
[`metrics-and-observability.md`](metrics-and-observability.md).

**The ceilings behind `AvailableSlots` are reported, not just the minimum.** That
function folds four numbers into one and the fold loses the only part an operator
can act on: a node offering 2 of its 16 slots is a different problem depending on
whether somebody is *using* the machine, its memory is gone, or its scratch disk
has filled, and the three have opposite fixes. `SlotCeilingsFor` is the same
arithmetic with each ceiling named and `AvailableSlots` is one line over it, so
there is one author for the subtraction. Two properties travel with it: the
ceilings are `optional`, so a machine that could not read its CPU has *no* CPU
ceiling rather than one of zero; and a tie names the earlier limit in enumerator
order, which is application order -- left unstated it would depend on how the
comparison happened to be written and change under somebody tidying it.

**A heartbeat age is a duration on a report, never a `TimePoint` on `WorkerInfo`.**
`WorkerInfo` is what `Pick` returns and what a lease is built from, and an age
frozen inside a lease stops meaning anything the moment it is stored. Worse,
handed a raw `TimePoint` the obvious thing for a consumer to do is subtract
`steady_clock::now()` -- right in production, wrong under every `ManualClock` test,
and silent, because the two clocks agree about nothing. Subtracting inside the
registry means the answer comes from the clock it was injected with; it clamps at
zero, because a manual clock can legitimately be set backwards and an unsigned
duration would otherwise read as several hundred million years.

**Nothing a receiver can RECOMPUTE travels on the wire.** A node hands its closed
buckets over on the heartbeat it already sends, and the record carries two instants
and the readings -- no fold and no coverage. The leader replays those readings at
those instants to fold them into its own rings, so it rebuilds both anyway; carrying
either would be a second answer to a question already answered, and the one a decoder
trusted would be the one nothing kept correct. A counter's peak is a RATE and cannot
be recovered from a single reading, which is the tempting reason to send it -- but it
*can* be recovered from the SEQUENCE, and the sequence is what travels.

**Handed-over history is filed under the MACHINE, never the worker id.** A host with
two `--toolchain` flags registers twice and heartbeats the same figures twice, so
keying per registry entry holds one machine's series once per toolchain and sums it
that many times -- the rule `WorkerRegistry::NodeCaches()` already exists to enforce
on the neighbouring number. `Heartbeat` therefore returns the endpoint rather than a
bool, which also removes the second lookup and the expiry gap in the middle of it.
A heartbeat from a worker the scheduler does not know is refused and routes nothing:
the refusal is what tells the node to register again, and the endpoint it would be
filed under comes from the very entry that is missing.

**The wire carries no acknowledgement, and needs none.** The leader keeps a
high-water mark per endpoint, so a batch redelivered after a reply the node never saw
is ignored there rather than counted twice. The node's own cursor is the other half:
it advances **only when the verb that carried the batch succeeded**. `accepted` also
counts a *registration*, which carries no history at all, so a round where every
heartbeat failed and one re-register succeeded stepped the cursor over a batch that
was never sent. The cursor lives on the sampler beside the series it indexes, not as
a bare integer in `main()` that only running the program could exercise.

**A leader persists what it was handed, and that is not an optimisation.** A node
advances its watermark once and never resends, so a leader that forgot what it had
been handed leaves those windows a gap for as long as the rings hold them -- the
exact failure the handover exists to remove, reintroduced by a restart. The store
nests one `FleetHistory` body per machine, written by the code that reads it back: a
second copy of the ring encoding drifts the first time a bucket grows a field, and
drifts *silently*, because both halves still round-trip against themselves.

**A worker discovers the machine's compilers; `--toolchain` narrows that, and does
not supply it.** `--toolchain` was required and had no default, which meant
`packaging/` shipped a worker that could not start: the Linux unit reads an env file
whose every line is commented out, so the service starts, refuses and exits, and the
Windows MSI registered no node at all. The reversal rests on a distinction that is
easy to lose — **"no default compiler" and "no discovery" are different claims**. A
default is how a job ends up running against something nobody chose; which compilers
a machine holds is a *fact*, and it is the half of the configuration that has to be
redone after every toolchain upgrade. Seven rules under it, each already a defect
caught in review:

- **Discovery adds CANDIDATES and must not add a second way to identify one.** The
  flavour comes from `Cc::ClassifyCompiler` and the fingerprint from
  `CachedToolchainFingerprint`, unchanged — a worker that derived either differently
  from its clients registers, heartbeats, and is never matched, with nothing anywhere
  reporting why.
- **A discovered path never goes back through the operator's grammar.**
  `SplitToolchain` reserves the first `=`, which is right for something a person
  typed and wrong for a path this process found itself: `/opt/gcc=13/bin/gcc` would
  register the fingerprint `/opt/gcc`, and a leading or trailing `=` would abort
  startup as "malformed --toolchain", naming a flag nobody passed.
- **The operator's list wins whole; the two are never merged.** A merged set would
  quietly re-add a compiler an operator had deliberately narrowed away, which is the
  entire reason a build farm names its toolchains.
- **A found compiler that cannot be spawned is not a toolchain.** That is the
  `SpawnFailed` refusal a client otherwise meets at job time, moved to startup where
  an operator can see it. An operator-NAMED one is not probed — the
  `<fingerprint>=<compiler>` override exists precisely for a compiler this process
  cannot execute.
- **An empty resolved set refuses startup, and the message names where it looked.**
  Left to run, such a worker registers nothing, heartbeats "0 of 0 toolchain(s)
  registered" as a complete success, and prints a ready line: a healthy unit, a green
  fleet, and every build compiling locally with no error at either end. The search
  list comes off `ToolchainLayouts()` rather than a list written by hand, which is
  maintained by the same person who forgot to add the row.
- **One location has one spelling, or one machine registers twice.** A root arrives
  spelled however its source spells it — the registry writes `C:\Program Files\LLVM`,
  an environment variable writes `C:\Program Files`, a table row writes `C:/msys64` —
  so the same `clang.exe` reached through two rows came back as two strings.
  `WorkerRegistry` keys on `(fingerprint, endpoint)`, so that is the double-counting a
  fleet view then has to render. Separators are collapsed on every host, case only on
  Windows; a scripted host cannot catch it, because it normalizes on the way in.
- **A fingerprint that names no compiler is refused, not registered**
  ([#140](https://github.com/LASTRADA-Software/fastcached/issues/140)). A driver this
  build cannot ask for a version falls back to a digest of its own basename, and
  discovery of its include tree is best-effort by construction; a toolchain that hits
  both at once digests to a value this repository could print with nothing installed. `ToolchainProbe.hpp` permits a
  banner-only fingerprint on the argument that it "can only cause two
  genuinely-identical toolchains to be treated as identical" — true, with an unstated
  precondition that the banner is a real version string. Check the precondition where
  both halves are known. An operator-pinned identity is never judged this way; it is
  the escape hatch for exactly the compiler this process cannot interrogate.
- **A probe that did not RUN is not a probe that answered nothing**
  ([#225](https://github.com/LASTRADA-Software/fastcached/issues/225)). Include-root
  discovery spawns the driver, and `DiscoverIncludePaths` returned a bare vector
  documented "empty when undiscoverable" — so a spawn that failed produced the same
  value as a driver that ran and listed nothing, which is a state this fingerprint
  legitimately serves. The digest was then taken over nothing and returned as the
  toolchain's identity. The refusal above does not fire, because it also requires the
  banner to be the fallback name, and a driver whose *include* probe failed usually
  answered `--version` perfectly well. `CompileRun::exitCode == NotSpawned` already
  told the two apart and nobody asked.
  - **The cache is not the self-correcting half it looks like.** The roots feed the
    stamp as well as the digest, so one failure stamps differently and the next good
    run recomputes — which is why the value moved and moved back. But a machine whose
    probe keeps failing stamps that failure *consistently*, hits its own entry, and
    settles on the wrong fingerprint permanently. A probe that did not run therefore
    neither reads nor writes the cache.
  - **A short WALK is worse, and is the case with no self-correcting run at all.**
    `ComputeToolchainStamp` folds each root's path and mtime, never its contents, so a
    walk that stopped inside a root that is there leaves every stamped input
    identical: the short digest validates against its own stamp forever and nothing
    walks again to notice. `ToolchainFileScan::complete` reports it.
  - **What is NOT a gap is half the rule**, and the test is DETERMINISM rather than
    "did something go wrong". A gap is worth refusing over when two ends running this
    same code would disagree about it. Three omissions are not: a root that is merely
    absent (drivers list paths they would search if they existed), an entry that is not
    a regular file, and a root whose bytes this process cannot decode into a path —
    that last one being a property of the bytes and of the UTF-8 narrow encoding every
    executable here pins, so both ends digest the same narrower tree and still match.
    Two of the three are easy to get wrong in the same direction: keying the
    absent-root test on `error_code` rather than on the resolved `file_type` calls
    every machine without `/usr/local/include` incomplete, because MSVC reports
    `no_such_file_or_directory` for a path that is simply not there; and treating a
    failed `is_regular_file` as a gap refuses every toolchain tree containing a
    dangling symlink. Refusing a healthy toolchain is a worse error than the one being
    caught.
  - **Why refuse rather than serve it anyway.** Both ends are silent otherwise: the
    worker registers, heartbeats and is matched by nobody, and the client sees
    `NoWorker` and compiles locally. Neither names a cause, and the failure is
    intermittent by nature. `IdentityDefect` carries which way an identity is
    unusable, and its reason and remedy are columns of a table because two surfaces
    report them — the node's refusal and `--print-toolchain-fingerprint`'s warning.
  - Related: [#148](https://github.com/LASTRADA-Software/fastcached/issues/148) is the
    same fact discarded one function away — `CompilerBanner` knows whether it spawned
    and reports only the banner, so `CanSpawn` runs the identical command again.
- **`cc` and `gcc` stay two candidates.** Usually one binary under two names, and they
  fingerprint *differently*, because a GNU driver prints its own `argv[0]` in the
  banner its clients hash. Collapsing them looks like tidiness and costs the fleet
  every `cc` build.
- **A layout describes a DIRECTORY LAYOUT, not a vendor.** Visual Studio ships
  clang-cl of its own, under `VC\Tools\Llvm` rather than beside `cl` — so the
  `visual-studio` row walked past it while all three LLVM rows wanted a *standalone*
  install, and a machine whose only clang-cl came with Visual Studio advertised none.
  The failure is the quiet half of the pair the whole feature exists to avoid: those
  builds were still **cached**, because a launcher needs no worker for that, and could
  never be **dispatched**, because nothing advertised the fingerprint. A green fleet,
  a working cache, and distribution simply off. One installation reached by two rows
  is normal; what must not double is the *process* — `vswhere`'s answer is memoized
  across rows, empty answers included, or a second row spawns it again at every start
  forever to be told the same thing.

**Only the native MSVC target variant is offered, and only what the filesystem
confirms.** This was once forced: every target variant of one toolset — x64, x86,
arm64 — shares an include tree, and they shared a banner too, so all of them
fingerprinted identically and offering them all registered one machine several times
under ONE identity. That is retired
([#195](https://github.com/LASTRADA-Software/fastcached/issues/195)) — the banner names
the target now, so they are distinct toolchains, and until then a fleet would happily
dispatch an x86 compile to an x64 worker. Offering them is therefore a capability
nobody has asked for rather than a duplicate; the restriction stands as scope, and
`ToolchainDiscovery_test` pins it so the day it changes it is a decision. Two neighbouring rules for the same reason: `vswhere` is
asked without `-requires`, because filtering on `...VC.Tools.x86.x64` is a false
negative on exactly the ARM64-only host the arm64 bin path exists for — an install
with no C++ toolset simply has nothing beneath its `versionRoot`; and nothing is
spawned to learn what the filesystem already said, so `vswhere` runs only when its
installer directory is present and the Xcode row (which asks once *per compiler
name*) only when `xcrun` is on the search path.

**A driver's include roots come from what it OWNS, never from the environment**
([#140](https://github.com/LASTRADA-Software/fastcached/issues/140),
[#145](https://github.com/LASTRADA-Software/fastcached/issues/145)). `INCLUDE` is set
per shell by `vcvarsall` and a Windows service inherits none, so any mechanism that
reads it gives a launcher in a developer prompt and a worker under the SCM two
different answers for one compiler — and a fingerprint disagreement is invisible from
both ends, presenting only as a scheduler that answers `NoWorker`. Both MSVC-family
drivers were on it and both had to come off, by different routes, because *owns*
means different things to them:

- **`cl` owns the VC toolset it lives inside**, so `MsvcToolsetIncludeRoots` walks up
  from the driver to `VC\Tools\MSVC\<version>`. It keeps an `INCLUDE` fallback
  because it must: a banner cannot see a patched header, so a wrapper outside that
  layout would be left with an identity carrying no toolchain content at all.
- **`clang-cl` owns only its resource directory** —
  `<prefix>/lib/clang/<version>/include` — and it is **asked** for it
  (`-print-resource-dir`) rather than having it derived. That is correctness, not
  taste: `/usr/bin/clang-cl-20` has `/usr` for a prefix, whose `lib/clang` holds `20`,
  `20.1.2`, `22` and `22.1.8` on an ordinary Debian, and no rule over those four names
  picks the right one — "the newest" hands a clang 20 driver the headers of clang 22.
  The layout is modelled for `cl` only because `cl` cannot be asked anything; this
  driver can, so it is. It reads `INCLUDE` **not at all**, not even as a fallback,
  which is what makes it symmetric *unconditionally* rather than merely wherever a
  layout is derivable. It can afford that because its banner is a real version string:
  a driver that does not answer degrades to a banner-only identity that still tells
  one clang from another.

The corollary is the part that looks like an omission and is not: **the VC toolset and
the Windows SDK stay out of `clang-cl`'s identity.** It borrows them rather than owning
them, a worker compiles text the client already preprocessed and so opens no header
from either, and the newest-kit-on-the-machine rule that would pick them splits two
boxes running one clang-cl with different SDKs installed — spending real matches to buy
no discrimination.

**A cold start fingerprints several toolchains at once, and reports them in table
order.** A cold walk is seconds per toolchain and a surveyed machine routinely holds
four or five; sequentially that is a node sitting silent for half a minute before it
reaches its scheduler, on the one start where an operator is watching. Bounded rather
than one thread each, because every one of them is a recursive directory walk hashing
what it finds. Results are collected positionally — the "serving" lines are where two
machines' digests get compared, so a log ordered by which thread finished first would
be a poor place to do it. The pool is joined **explicitly** before the return: locals
are destroyed after the return value is constructed, so leaving it to scope exit
copies the results while the workers are still writing into them.

**The workers share the ONE injected runner, and `IProcessRunner` requires that.**
Making a runner per thread reads like a courtesy to the seam and is the opposite of
one: a function whose caller passed an `IProcessRunner&` and that calls
`MakeProcessRunner()` anyway spawns real compilers no test can script, and every case
that believed it was driving a fake was quietly interrogating whatever the machine it
ran on happened to have installed — five of them passed for that reason and failed
the moment the hole was closed. If concurrency needs something of a seam, say so in
the seam.

## Open work

- **[#201](https://github.com/LASTRADA-Software/fastcached/issues/201)** — a node
  offers only the NATIVE MSVC target variant, on a reason that no longer holds: the
  variants shared a banner and so a fingerprint, and
  [#195](https://github.com/LASTRADA-Software/fastcached/issues/195) gave the banner the
  target it names. They are distinct toolchains now, so offering the cross-target
  drivers is a capability rather than a duplicate -- but it multiplies one machine into
  six registrations, and capacity is per NODE, so it is not a one-line change to the
  layout table.
- **[#175](https://github.com/LASTRADA-Software/fastcached/issues/175)** — a disk
  tier's in-memory key index is RAM nothing accounts for. `--cache-disk` and
  `tierBytesLimit[Disk]` both denominate bytes on the filesystem, while
  `CowTreeStorage` keeps every live key resident, so a `--cache-memory 0
  --cache-dir …` node reserves nothing and offers the whole machine. Marking the
  disk tier resident is NOT the fix — its budget is disk, so summing it into a
  memory total is wrong by the ratio between the two rather than by the index's
  size. The fix needs a figure that does not exist yet.
- **[#174](https://github.com/LASTRADA-Software/fastcached/issues/174)** — the two
  POSIX rows of `ToolchainLayouts()` are walked on Windows, where `/usr` is
  drive-relative, so an MSYS2 or Cygwin install rooted at `C:\` re-admits through
  `/usr/bin` the MSYS-runtime `gcc` the `msys2` row deliberately excludes -- and
  records it under a spelling `PathIdentity` will not collapse against the `C:/...`
  one, so a machine registers twice. Fixing it needs a platform column on the row,
  which has to be honoured by the walk WITHOUT being honoured by the tests, or the
  Windows rows stop being reachable from a Linux runner and the reason the table
  carries no `#if` is lost.
- **[#148](https://github.com/LASTRADA-Software/fastcached/issues/148)** — every
  discovered compiler is spawned twice at startup with the same argv, once to learn
  it can be spawned and once for its banner, and the first is in a serial loop in
  front of the pool built to hide exactly that. `CompilerBanner` knows both facts
  and reports neither, so two callers reconstruct what it discarded.
- **[#146](https://github.com/LASTRADA-Software/fastcached/issues/146)** — the MSVC
  bindir a layout row searches is chosen by `#if` on the architecture this binary was
  COMPILED for, so an x64 build on an ARM64 Windows host never offers that machine's
  native toolset. Moving the fact onto `IToolchainHost` costs the row its
  `constexpr` span, and it is only fully correct once a fingerprint can tell one
  toolset's target variants apart -- which today it cannot, since they share an
  include tree and a fallback banner and therefore digest identically.
