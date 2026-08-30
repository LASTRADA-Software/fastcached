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

- **The worker's argument filter is an ALLOWLIST, not a shape-based denylist**
  (`IsAcceptableJobArgument`, `CompileJob.cpp`). The client's arguments are spliced
  into the compiler's command line verbatim, and the compile port carries no
  credential while loopback is admitted unconditionally — so any local process can
  reach it. Naming the compiler is refused (the fingerprint rule above), but several
  driver options *run a program or load code* without ever naming one: `-wrapper
  prog,args`, `-fplugin=`, `-Xclang -load`, `-specs=`, `-B`. None carries a path
  separator, so the old "could this name a file?" filter admitted every one — a
  proven RCE as the node's service account
  ([#240](https://github.com/LASTRADA-Software/fastcached/issues/240)). The flag
  space belongs to GCC, Clang and Microsoft and grows every release, so a denylist
  we audit against upstream forever **fails open** the day we miss one; the allowlist
  **fails safe** — an unrecognised flag costs one local compile, visible as
  `fastcache_worker_jobs_refused_rejected_argument_total`, where a missed denylist
  entry was code execution. The accepted set (`AllowedArgs`) is per driver family —
  the worker knows its own compiler's family, never the client's — and refusal names
  the offending flag in `JobError::detail`, which rides the reply message to the
  client's fallback log.
  - **The `-f` space is ENUMERATED, never prefixed, and that is the load-bearing
    half.** A blanket `-f` prefix with a `Deny` row for `-fplugin` reads as an
    allowlist and behaves as a denylist over the largest and most volatile flag family
    GCC and Clang have: a new code-loading `-f*` is admitted *by default* until
    somebody adds a row. That is not hypothetical. `-fmodule-mapper=|program args`
    makes GCC **spawn a subprocess** — this tree already declares it path-valued and
    lists it in `SideArtefacts` — and `-fpass-plugin=x.so` is Clang's pass-manager
    plugin loader. Neither begins with `-fplugin`, and neither carries a path
    separator, so neither a carve-out on that spelling nor the shape rule beneath it
    stops them. Enumeration is what makes the class fail closed.
  - **A prefix row is legitimate only where its non-listed members are a CLOSED set,
    named as `Deny` rows beside it.** Two qualify: `-W` (whose only non-warning
    members are the three sub-tool passers `-Wa,`/`-Wl,`/`-Wp,` — there is no fourth
    sub-tool a GNU driver forwards to) and `-m` (whose only pass-through is `-mllvm`,
    which takes its value as a *separate* argument that must itself survive the
    table). Every other prefix row's spelling ends at the option's own `=` or `:`, so
    it names exactly one option and only its value is open. `-X` is not a prefix at
    all — nothing under it is code generation, so `-Xclang -load` is refused by
    absence.
  - **A prefix on one family's spelling reaches the other family's flags.** `/w`
    needs a prefix for `/wd4996`; written as `DriverFamily::Any` it admitted GNU
    `-wrapper` — the exact flag this ticket is about, let back in by a one-letter
    prefix. Caught by the regression case, not by review. A prefix row is scoped to
    the family that actually spells it that way.
  - **Every row carries a value shape (`Bare` / `NoPathSeparator`), so the old shape
    rule composes INSIDE a prefix rather than being deleted with it.** A row-level
    constraint kills a class of value; a `Deny` row kills one member per incident. It
    is defence in depth beneath the allowlist and never a substitute — `-fpass-plugin=`
    carries no separator either.
  - **The maintained tables are asked, not restated.** `ProducesSideArtefact` answers
    "does this write something besides the object" (`-fmodule-mapper=`,
    `-fmodule-output=`, `/Yc`); `DriverSpec::preprocessedInput` spells the language
    tokens the client sends; `TargetPinPrefixFor` spells `--target=`. Copying any of
    the three into rows here means a spelling added upstream turns every such job into
    a silent `RejectedArgument`.
  - **An unclassifiable compiler refuses the JOB, not its arguments.** With
    `DriverFamily::None` the allowlist refuses everything, which reports a *worker*
    misconfiguration as `RejectedArgument` and sends an operator to look at the
    fleet's flags — and a job with an **empty** argument list has nothing to refuse,
    so it sails past the loop and spawns a driver whose dialect this worker does not
    know. Checked once where the family is derived, answered `SpawnFailed` ("this
    worker is broken, compile elsewhere"), which already owns the right wire code and
    counter.
  - **`JobError::detail` carries client bytes onto the wire, so it is built by
    `RejectedArgumentNaming` and never assigned directly.** It is encoded into the
    reply and lands in the client's fallback log, so the argument is capped and
    reduced to printable ASCII at the one producer — which also makes it valid UTF-8
    whatever arrived, as the fleet requires of text a peer sent.
  - **Operator extension of the allowlist is a follow-up, not this rule.** The table
    is built-in; there is deliberately no config hook.

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
  different fact and there is nobody to name. That "so a client redirects" was true of
  the design and false of every client for the whole life of the code — the launcher
  read it as an ordinary refusal until #237. Serving the endpoint is half a rule; the
  half that makes it work is below, under *a refusal that names somewhere else*.
  Three roles rather than a `bool`, for exactly that third state.
- **A node's cache tier serves THIS MACHINE, always — locality is a property of the
  VERB, never of the bind and never of a member list** (#287). `CacheResponder` used
  to admit `Membership::Member`, and a Member is any machine the operator listed, so
  a fleet peer could `FETCH` every object this machine had ever compiled. The docs
  promised exactly that ("its own machine and its cluster"), which is why the fix is
  a breaking change rather than a bug fix.
  - **The two lists were one list answering two questions.** `--fleet-member` names a
    machine that may spend this node's **CPU** and be leased its slots. The cache
    tier is this machine's entire **build output**. Contributing capacity does not
    make another host entitled to read it, and there is no configuration in which it
    should — so the responder does not take an `IMembershipOracle` at all. Its
    absence is the fix; a flag it consulted would be a flag somebody could set wrong.
  - **"It is only bound to loopback" is not a policy and stops being available at
    all.** The default bind closes this by accident, and the accident evaporates the
    moment `--listen-cache` is widened — or, once cache, scheduler and compile share
    one wildcard listener (#290), the moment there is no per-surface bind left to
    reason about. A rule on the verb survives that merge; a rule on the bind does not.
    This ticket is that merge's hard prerequisite for exactly that reason.
  - **The question is ambient, so it arrives through a seam with a clock**:
    `Platform/ILocalityOracle`, over `IHostAddressSource` (`getifaddrs` /
    `GetAdaptersAddresses`). Not a syscall in the responder — a security decision
    nothing can substitute for is one no test can present a non-local caller to.
  - **Fast by construction before it is fast by cache.** `IsLoopbackHost` is asked
    first and takes no lock, so essentially every real caller — the surface binds
    loopback — never touches the address set, and the cache bounds only the rare
    path. Loopback is NOT the whole answer, though: a local client dialling this node
    at its own routable address on a widened bind is still this machine.
  - **Refreshed on an INTERVAL, never on a miss.** A miss-triggered refresh hands a
    remote peer a free amplifier: one probe per request, just by asking, and
    `GetAdaptersAddresses` measured **2.04 ms** against `getifaddrs`' **0.0086 ms** —
    238×, so a design that is free on Linux dominates a request on Windows. Nothing
    about the *answers* distinguishes the two designs, so the test counts probes.
  - **Both staleness directions are named in the header, because one of them alone is
    how a longer interval gets talked into being fine.** An address just GAINED is
    refused for at most one interval — fails closed, self-heals, costs one local
    compile. An address just LOST stays admitted for at most one interval, which
    needs a DHCP reassignment racing the refresh to exploit. Neither is a wrong
    answer served confidently, which is the line the project's caching rule draws.
  - **Folded with `SameHost`, never compared raw.** A surface bound to `::` reports
    an IPv4 caller as `::ffff:10.0.0.7` while the interface list says `10.0.0.7`, so
    a raw compare refuses this machine's own clients on exactly the dual-stack bind
    the rule was written for — the failure #180 already paid for once on the member
    list. It also refuses an unnameable peer against an empty entry, which a raw
    compare admitted.
  - **`NotAMember`, not a code of its own.** `fastcache-cc` reads a FETCH outcome as
    "is this daemon worth a second command" and steps over this one, so a peer whose
    access was withdrawn compiles locally. A new code would be an unknown one to
    every launcher already deployed, and an unknown refusal is what cost this tree a
    permanent 0% hit rate before. `NodeCacheRequestsRefusedNotLocal` carries the
    operator's half: the tightening removed access somebody had, and a peer whose hit
    rate fell needs one number that says why.
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
  - **A registration's endpoint is where work gets SENT, and nothing ties it to the
    connection that carried it. What landed for #242 is a counter, and calling it a
    fix would be the error.** `SchedulerService::Register` runs `Gate()` and then
    checks only that the fields are UTF-8; `WorkerRegistry` stores the endpoint
    verbatim and `Lease` hands it to clients, which dial it and send a whole
    preprocessed translation unit. The obvious remedy — refuse an endpoint whose host
    is not the caller's — was priced and **rejected**, and both halves of that are
    the rule:
    - **It refuses the ordinary case.** A worker that advertises a DNS name
      mismatches and the scheduler cannot resolve it: the service is I/O-free by
      construction, and a resolver is not something a security decision may depend on
      — which `IsLoopbackHost` already records about `localhost`. So do multi-homed
      workers, NAT, a VPN, and the **first fleet the getting-started page builds**,
      where a node registers with its own scheduler over loopback while advertising a
      routable name. A control whose false-positive set is the documented setup is
      not deployable.
    - **And it buys less than it looks like.** The caller is already an admitted
      member, so it may receive the fleet's work by naming its **own** address; the
      comparison only stops it naming a *third* host. Membership already granted "may
      this machine receive work", so the check narrows the primitive without removing
      it — which is the same shape as the security theater it was meant to replace.
    - **The real mechanism is a credential, and this tree already has it one layer
      over.** Discovery proves a `(node, endpoint)` pair with a MAC over a nonce the
      challenger chose, *precisely* so a claimed endpoint cannot be substituted. A
      registration wants that, and it is the same mechanism as the planned signed
      lease tokens.
    - **So what shipped is instrumentation, and it says so.**
      `DispatchWorkerEndpointMismatch` counts an **accepted** registration whose
      endpoint host is not the caller's, beside a bounded `Info` line naming both
      addresses — `Info` and not a warning, because on a DNS-named fleet this is every
      registration and a permanent warning is one an operator learns to filter. No
      wire code was added: a code nothing returns would put a lie in the refusal
      table. The number exists because *nobody knows* how often endpoints legitimately
      differ on a real estate, and that is what decides whether a stricter rule is
      viable — both ends of the argument were guessing.
    - **`CallerContext::peerId` used to disclaim itself, and that comment is how the
      hole stayed open.** It said "who the peer says it is, for logs; never trusted
      for a decision" — every clause false. The transport fills it from
      `ISocket::PeerAddress()` and hands the same string to the membership oracle, so
      it is already the basis of the decision on that surface and is the one fact a
      caller cannot forge. A comment asserting a check is impossible is worth more
      than a missing check: it stops the next person looking.
    - **Hosts only, never ports, and unmapped on both sides.** A peer dials from an
      ephemeral source port, so `peerId` carries none — the reason `ClusterMembership`
      keys on hosts too. And `::ffff:10.0.0.1` versus `10.0.0.1` is a property of how
      the *listener was bound*, so `Core/HostPort::UnmappedHost` folds them or two
      identically configured nodes disagree about one machine.
  - **The oracle is a seam and not a call into `Cluster::PeerDirectory`.** The
    dependency would run the wrong way — `Distributed` is the policy, `Cluster` is
    one way of establishing the fact it needs — and the answer is *deployment*-shaped
    rather than universal, which is what an interface is for. `Publish` is a setter
    and one of the documented carve-outs to configuration-at-construction: membership
    is precisely what changes while the object lives, and rebuilding the oracle per
    join would mean handing a new one to a running server.
  - **Cluster membership is a SOURCE of admissions, never the whole policy — one
    list answering two questions revoked an operator's by agreeing anything.**
    `NodeMembership::Publish`, driven from `ConsensusTier`'s state observer, replaced
    the `--fleet-member` hosts with the cluster's member set. The two answer different
    questions: who may spend this node's CPU and read its cache tier — which is mostly
    **clients**, developer laptops and CI runners, machines that never join consensus
    and never should — and who is in the cluster, which is peers only. So on any node
    running consensus the first replicated membership commit discarded every host an
    operator listed, and agreeing something is *routine*: a node joining, a node being
    forgotten, a settings change. A client machine admitted by name worked right up
    until the fleet agreed anything at all and then stopped, with no configuration
    having changed on either machine and nothing in the log tying the two events
    together (#251). Four things about the shape that closes it:
    - **It is the admission-layer reading of a rule consensus already has.** *Absence
      from `ClusterState` is not removal* — a `--raft-peer` member is in the
      configuration and in no state, and is never proposed for removal. An operator's
      `--fleet-member` list is the same fact one layer up, and an empty agreed member
      set — which is what every clustered node sees before the first entry naming
      anybody commits — must therefore take nothing away.
    - **The union is composed at the SEAM, not inside one oracle.**
      `AnyOfMembership` holds participants and admits whoever any of them admits;
      `ClusterMembership` keeps its single list and its wholesale `Publish`, which is
      right for the one question its owner gave it. The obvious alternative — teaching
      that class to hold a table of lists keyed by where each came from — cannot hold
      the route that is coming: a signed lease token is a *credential check* with no
      host set to add a row to, and it **adds** to an address policy rather than
      replacing it, because that policy is what still gates the cache tier and what an
      operator sets on a worker taking no leases at all. An enumeration of host lists
      would have had to be taken apart again to admit it; a participant list does not.
    - **Which list is written is decided by the owner, not passed in.**
      `NodeMembership` holds one `ClusterMembership` per question and `Publish` writes
      the cluster's, so the observer consensus installs cannot name the wrong one —
      the same defence the host/endpoint collapse gets from being done in the
      constructor, and for the same reason: the failure is silent. The participants are
      borrowed, which is safe only because that type owns them, is declared before the
      composite and is neither copyable nor movable.
    - **`AdmissionSummary` had encoded the defect as advice.** It refused to name
      `--fleet-member` on a clustered node, on the reasoning that the flag was about to
      be overwritten — which was true and was the bug. A line that steers an operator
      around a defect is one more thing to correct when the defect is fixed.
- **An unbounded wait does not avoid an ending, it only chooses who picks it — and
  the supervisor picks `SIGKILL` with no diagnostic.** `~WorkerServer` drained on an
  unbounded condition variable, and the comment defending that was right about its
  premise and wrong about its conclusion. The premise: a job on the executor holds a
  pointer into the server — the counter, the protocol, the metrics sink, the logger,
  the byte budget — so *returning* from the drain while one is still running frees
  all of them underneath it. Bounding the wait does not make that safe. The
  conclusion it drew, that the wait therefore had to be unbounded, bought nothing: a
  single wedged compiler turned `systemctl stop` into a supervisor timeout and a
  kill, and on Windows into an SCM stop timeout an operator reads as "the service is
  hung" (#239). Observed, not theorised — `dist-compile-e2e` case 8 failed on a
  loaded macOS runner with the worker not exiting in 15s while cases 1–7 passed, so
  the fleet worked and only the *stop* did not.
  - **So on expiry it states what it is abandoning and ends the process.** That is
    the one exit which abandons those jobs without touching what they are still
    inside, and each one's client resolves its own lease on every path out of a
    compile (#212). `_Exit`, never `exit`: static destructors would run the very
    teardown being avoided.
  - **The decision is a pure function because the interesting arm cannot be
    tested.** A branch that calls `_Exit` is a side effect no in-process case
    survives, and a side effect no test can survive is one no test will check. So
    `NextDrainAction` is arithmetic over (outstanding, waited, timeout), exhaustively
    unit-tested, and the destructor is left with nothing but carrying it out.
    `Finished` outranks an expired bound, or a stop that had already finished would
    log a false abandonment at exactly the moment the thing worked.
  - **The bound is a flag, and zero still means forever.** How long a compile
    legitimately runs is a property of the site, not of this program, and a
    compile-time answer on a binary whose `--install-service` replays its command
    line forever is one nobody can move afterwards. Zero is what the node did before
    the bound existed and stays sayable, so a behaviour change is not one an operator
    is unable to turn off.
  - **A silent bounded wait is barely better than an unbounded one.** It reports the
    outstanding count on a cadence, because a stop that says nothing for thirty
    seconds is indistinguishable from one that has hung — which is the reading the
    whole change exists to prevent.
  - **It does not close the ticket, and the other half is where the real subtlety
    is.** Killing a wedged compile's *direct child* would not even unblock the drain:
    `posix_spawn` dup2s the pipe write ends onto the child's stdout/stderr, so every
    grandchild inherits them and EOF arrives only when the last holder exits. The
    drain blocks on the pipes, not on the wait. So the process-**group** kill (a job
    object on Windows) is load-bearing rather than a refinement, and the naive fix —
    kill the child, wait for EOF — looks correct and hangs forever.
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
- **A node's toolchain identity is derived once and the machine keeps moving, so it
  is re-derived.** The launcher recomputes its fingerprint per invocation; a worker
  computed one at startup and then ran for weeks. A compiler patched in place — a
  distribution upgrading `gcc`, a Windows SDK update — left the node advertising the
  pre-upgrade digest while spawning the post-upgrade compiler, so clients received
  objects built by a compiler they had not keyed against and stored them in the
  shared cache under the old key, where the whole fleet then read them (#238). A
  wrong-object path, and the one a staggered upgrade across an estate walks into
  deliberately.
  - **Re-register under the new fingerprint AND stop serving the old one; they are
    not alternatives.** Stopping is the load-bearing half — it *is* the wrong-object
    path — but stopping alone takes a machine out of the fleet on every routine
    upgrade, and a staggered rollout would empty the fleet one machine at a time with
    nothing saying so. The dropped fingerprint is refused `UnknownFingerprint`, which
    already exists with its own wire code and counter; nothing new is invented, and
    the client falls back to a local compile. The compile port is updated **before**
    the registration, or the window between them is the defect itself.
  - **The staleness check spawns nothing, and that is what makes it affordable.**
    `ComputeToolchainStamp` is pure filesystem stats — it is its *inputs* that cost
    driver spawns. So the banner, the resolved compiler path and the include roots
    are recorded at survey time and the stamp is recomputed from them: a stat of the
    binary plus one per root, on every heartbeat, with the expensive re-survey paid
    only when it says something moved. Re-stamping per job would be the cost #188 is
    separately removing from the launcher's hot path.
  - **A witness-driven recheck can only notice what it is already watching, so it
    needs an unconditional sweep beside it.** A toolchain that leaves the served set
    takes its evidence with it, and `IdentityDefect::UnrunProbe` is transient by
    construction -- so one unlucky spawn drops a healthy compiler permanently, and a
    node left serving nothing has no witnesses at all and could never recover without
    a restart. That directly contradicts the promise the rest of this makes, that a
    compiler may come back with the next package. A slow unconditional survey is the
    way back; it must answer unchanged when it finds the machine unchanged, or it
    re-registers the whole fleet on a timer.
  - **The re-survey is `ResolveToolchains` itself, never a cheaper second
    derivation.** A node whose identity was computed one way at startup and another
    way afterwards drifts from its own clients exactly when nobody is looking.
  - **A witness recorded from a probe that did not RUN is worse than no witness.**
    The identity was computed from what the FIRST include probe found, so a witness
    built on a failed SECOND probe watches a narrower root set than the fingerprint
    covers — and the node then stops noticing SDK-side changes for that toolchain,
    permanently and in silence. This is #225's rule reaching a second caller, by a
    door nobody had opened yet: **a probe that did not RUN is not a probe that
    answered nothing**, so `IncludeSearchRoots::answered` decides, never an empty
    `roots`. Not watching a toolchain is visible at the next sweep; watching the
    wrong evidence is not.
  - **An operator's pinned identity has no witness and is never reconsidered**, and
    an unstampable compiler yields an empty stamp that must read as "cannot be
    watched" rather than as "changed" — the latter is a re-survey loop with no exit.
  - **A set that stops being fixed at startup makes every remaining reader of it a
    race.** `toolchains` was `const` and read by the ready line on the main thread;
    making the heartbeat thread able to replace it turned that read into UB on a
    `std::map`. The count is captured before the thread starts -- which is also the
    honest number, since a ready line is a statement about starting.
  - **A map a job reads must not be held as an ITERATOR across the compile.**
    `CompileJobRunner::Run` looked its compiler up and then dereferenced that
    iterator twice far downstream — after the scratch directory was made and the
    whole preprocessed source written — on the two lines that decide which program
    executes and which driver family names the output flag. Making the map
    replaceable turned that into a dangling read. The compiler path is copied out
    under the lock at lookup, so a job already admitted finishes against the compiler
    its client was told it would get. A test that replaces the map before or after
    `Run` proves nothing; the replacement has to land while a job is provably inside.
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
- **A lease token is a CREDENTIAL, and its MAC covers the granted endpoint or it is
  a credential for the whole fleet.** For a long time it was a small decimal serial
  the scheduler minted for its own bookkeeping, and the worker's validator was
  literally `[](...){ return true; }` — so a worker compiled for anyone who could
  reach its port and present any string, and membership (which matches on a source
  address) was the entire boundary. A grant now carries an HMAC-SHA256 tag over the
  worker's **endpoint**, the fingerprint, the object key and an absolute expiry,
  under the same pre-shared key discovery uses. The endpoint is the load-bearing
  field: a MAC over "somebody may compile" is a token captured on the way to one
  machine and replayed against every machine that trusts the key, which is the
  identical failure `Cluster::DiscoveryWire` closes by covering the
  `(node, endpoint)` pair. The claim fields are **length-prefixed, never joined**,
  and here the separator argument is not hypothetical — an endpoint is `host:port`,
  so `{endpoint="a", key="b:1"}` and `{endpoint="a:b", key="1"}` would authenticate
  identically. Every message is prefixed with a **domain label**, because the same
  key already MACs discovery proofs and one key serving two constructions is how a
  tag made for one comes to pass for the other. The token wraps the `LeaseTable`
  serial rather than replacing it, which is what keeps that component pure — no key,
  no wall clock, no crypto in the thing whose whole job is a deterministic unit
  test. See `src/FastCache/Distributed/LeaseToken.hpp` (#281, #282).
- **The MAC covers WHICH FLEET and WHICH EPOCH, or one key file is one fleet.** The
  endpoint stops a grant being replayed against another *worker*; it does nothing
  about another *cluster*. Two fleets provisioned from one `--cluster-key-file` — the
  ordinary result of copying a working configuration to a second site, or cloning
  staging from production — authenticated each other's grants perfectly: the MAC
  verified, the endpoint matched, the fingerprint matched, the expiry was in the
  future, and nothing said which fleet issued it (#322). The same gap admitted a grant
  from before a leadership change, because no epoch travelled either.

  **The identity is MINTED, never configured, and that is the whole of why it works.**
  The act that causes the bug is copying a config file, so a `--cluster-id` in one is
  copied by the same `cp`; an identity derived from the key is identical by
  construction in exactly the case that matters; and one derived from the member set
  changes as members are added, which would silently re-identify a live fleet. What is
  left is a draw from `IRandomSource` on first start, **persisted** so a scheduler
  restart is ordinary, and **replicated** as a `SettingTable` row so every member of
  one cluster signs under one value. Without consensus — a node with no `--node-id`,
  which `SchedulerTier` calls "what most people run" — a lone scheduler owns a file.
  State the residual rather than papering over it: cloning a whole machine image,
  state directory included, still copies the identity. Copying a *configuration* is
  what this closes.

  **The epoch stays a separate field.** Identity and freshness answer different
  questions — *which fleet* and *how fresh* — and a value answering both answers
  neither the moment they disagree. It is the consensus term, which `ConsensusTier`
  had been dropping on the floor; `SetRole` only ever RAISES it, because a scheduler
  talked backwards into minting under an old term is indistinguishable from the attack
  the epoch exists to stop.

  **A worker learns both rather than being told either.** There is deliberately
  nowhere to configure a fleet identity on a worker, and no way for it to ask who
  leads without a dependency on cluster state, so it pins the first identity it
  authenticates and keeps a high-water mark of the freshest epoch — both learnt only
  from grants that passed *every* check, so a token that was authentic but wrong for
  this worker teaches it nothing. What that leaves open is a worker which has verified
  nothing yet, and the expiry is what bounds it. Closing it properly means the worker
  learning its fleet from its own registration, which is a wire change and is filed
  rather than assumed.

  A refusal is `LeaseForeignCluster` or `LeaseStaleEpoch` — never a generic MAC
  failure, and never `UnknownLease`, which is the scheduler's code. The foreign-cluster
  message names the shared `--cluster-key-file` explicitly: an operator who has just
  cloned a configuration and been handed a security-flavoured refusal will otherwise
  spend a day on the network, and that day is the real cost of this bug.
- **The MAC is verified BEFORE any other claim is reported on, and the expiry is not
  a capacity bound.** Checking the plaintext endpoint first would be cheaper and
  would turn a diagnostic into an oracle: `EndpointMismatch` and `Expired` are only
  ever reported for a token that provably came from the scheduler, so a forger
  learns exactly that their forgery failed, while an operator whose worker
  advertises a name clients do not resolve is told precisely that — which is the
  overwhelmingly common cause and is a NAT or a hostname, not an attack. The expiry
  carries **five minutes of slack** because a fleet's machines are not all
  NTP-managed and an unsynchronised clock is minutes out, not seconds; it bounds how
  long a *captured* token is worth replaying and nothing else. A worker will
  therefore accept an authentic lease for minutes after the scheduler stopped
  suppressing its key, and that is correct: what bounds what a worker runs is its
  own slot accounting and its in-flight byte budget, never the lease. Anything built
  on "an unexpired lease implies the scheduler still holds capacity" is built on a
  guarantee that does not exist.
- **A scheduler with no `--cluster-key-file` signs nothing, and says so.** Refusing
  to schedule without a key would break every single-machine install, which is what
  most people run; doing it quietly is the failure class this repository keeps
  rediscovering — a fleet that is green and is not doing the thing it claims. So the
  fallback is the bare serial exactly as before, plus one bounded warning line at the
  first grant. #303 is the open question of whether that should become a refusal;
  the worker's answer below is the shape it should take.
- **Whether a worker CHECKS a lease is a startup decision, never a per-request
  fallback.** The two are not the same rule written twice. "No key, so skip the
  check" taken per request is silent degradation of exactly the kind this file
  exists to name: the compile port is open, all three `worker_jobs_refused_lease_*`
  counters read zero, and the fleet looks healthy from both ends — indistinguishable
  from a fleet where nobody ever presented a bad lease. Taken once, before anything
  is served, it is a node that states what it cannot do. So a node another machine
  could dial and holding no key is refused **by name** at startup, and a node
  nothing else can dial runs unchecked and warns once, loudly.
- **The question is "can a machine that is not this one reach the surface", never
  "is a key configured".** Keying the refusal on the key alone breaks every
  single-machine install to prevent nothing — a process on this host already has
  this host's compiler, so a lease check there escalates nobody. "Could reach it"
  has **two halves and either one closes it**: the socket (a loopback `--bind`
  answers no other machine whatever the policy says — which is what keeps this
  repository's own `--fleet-open` e2e fleets working) and the policy
  (`--fleet-open`, a non-loopback `--fleet-member`, or consensus). **Consensus
  counts**, because a clustered node's admitted set GROWS at runtime: the agreed
  member list is published into the same oracle the compile port consults, so such a
  node admits machines nobody typed. `CompilePortFacesTheNetwork` and
  `AdmitsRemotePeers` in `NodeConfig.cpp` (#282).
- **A validator returns a REASON, not a `bool`, and it does not take the endpoint.**
  Three refusals are distinguishable on the wire and counted apart, so a boolean
  collapses the one distinction an operator needs — somebody probing the port, a
  worker whose advertised endpoint is not the one clients dial, and a machine whose
  clock has drifted are three different things to go and do. The endpoint checked
  against is the WORKER's own advertised address, captured at construction: a
  parameter would invite a caller to pass something the *request* supplied, which is
  the whole failure the endpoint is inside the MAC to prevent. And the refusal is
  never `UnknownLease` — that is the SCHEDULER's code, meaning "a lease I issued and
  have since forgotten", and a worker answering with it sent an operator to the
  scheduler to look for a fault that is local.
- **A fixture that carries the key and never dispatches proves only that a keyed node
  STARTS.** `cluster-e2e` and `fleet-dashboard-e2e` were given `--cluster-key-file`
  because they leave `--bind` at the wildcard, and neither compiles anything — so for
  a while the only end-to-end evidence for the lease check was construction. Meanwhile
  the fixtures that *do* dispatch bind loopback, slipped under the startup rule, and
  ran `UncheckedLeaseValidator` for every one of their hundreds of compiles. Both
  halves have to meet in one fixture, which is why `dist-compile-e2e` carries the key
  on every node: an in-process test mints and verifies inside one process and cannot
  show that the endpoint a worker ADVERTISED is the endpoint the scheduler signed.
- **A flag that describes nothing under socket activation cannot answer whether a
  port faces the network.** `--bind` is the obvious answer to "is this port local",
  and it is wrong for a reason nothing in this tree stated until now. Under
  activation the unit owns the address -- the shipped
  `fastcache-compile-node.socket` says `ListenStream=6676`, every interface -- and
  `--bind`/`--port` are read by nothing. The value is still *in* the configuration;
  it has simply stopped describing anything. So a keyless node carrying a stale
  `--bind=127.0.0.1` in its configuration, plus `--fleet-open` or a remote
  `--fleet-member`, passed the startup table, built `UncheckedLeaseValidator` and
  served an unauthenticated compile port to the network with all three
  `worker_jobs_refused_lease_*` counters reading zero. #282 recurring inside the fix
  for #282, and found by review rather than by CI.
  - **The table's rule is INSUFFICIENT, not wrong, and that distinction is the whole
    entry.** `--install-service` bakes `--bind` into a command line it replays at
    every boot, so a startup-time check on it is right for the configuration it was
    written about -- and it is the only check that can run before any tier exists.
    What it cannot cover is a listener this process did not open. Hence two rules:
    the table keeps `--bind`, and `MakeWorkerLeaseValidator` takes whether the
    listener was inherited and refuses the activated case by name. Delete either as
    redundant and one of the two shapes goes unguarded.
  - **So the test asserts the PAIRING, not either half.** One case drives the same
    `NodeConfig` through `StartupPolicyRejection`, checks it PASSES, and then checks
    `MakeWorkerLeaseValidator` refuses it. That regresses the gap. A case asserting
    only the guard regresses the rule, and leaves the next reader free to collapse
    the two.
  - **The family: a premise that is true and a conclusion that does not follow.** The
    premise -- `--bind` records what this process asked for -- is true. The
    conclusion -- therefore it says what the port faces -- does not follow, and only
    under activation. This file already carries the same shape in the
    `~WorkerServer` unbounded drain, whose defending comment was "right about its
    premise and wrong about its conclusion". That one was in a comment; this one is
    in a guard. Two instances make it a family rather than a coincidence: when a
    premise is doing load-bearing work, check it still holds on every path that reads
    it, not only on the path it was written for.
  - **And the sibling is already known.** `--listen-scheduler` describes nothing under
    activation for exactly the same reason, so the scheduler-side refusal (#303) will
    have this hole the day it is written. Recorded on that issue from the other
    direction.
- **The trust decision does not live in `main()`.** It lived there, as
  `[](...){ return true; }`, through a fully passing suite — the shape this file
  already names as *a reclaimer nothing constructs is the bug it was written to
  fix*. `main` is the one translation unit no test can reach, so the choice is
  `Node::MakeWorkerLeaseValidator`, exercised directly from a `NodeConfig` and a real
  key file, and `main` is left with a call. A key file that cannot be READ stops the
  node; falling back to checking nothing is the failure this whole rule is about.
- **A `--cluster-key-file` rule keyed on "does anything read it" has been wrong
  twice and is gone.** It began as *unless `--discovery`*; #281 made the scheduler a
  reader and the rule refused the correct scheduler. It was widened to name the
  scheduler; #282 made the worker a reader, and a plain worker runs neither of the
  other two surfaces — so the rule refused the configuration the lease rule above
  *requires*. There is no third narrowing: whether a worker tier exists depends on
  what `--toolchain` and discovery resolve to on the machine, which the config table
  cannot see. A refusal whose premise has become false is worse than no refusal, and
  one that cannot state its premise without guessing has no business firing.
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

**The cache and the fleet are two failure domains, and the client must not join
them.** `RunCached` returned on a `FETCH` that failed at the transport, and that
`return` sat above the call site that would dispatch — so a cache the launcher could
not reach turned distribution off with it. Every property this repository cares
about held while it did: the build succeeded, the object was correct, the exit code
was zero, and one `FASTCACHE_VERBOSE` line said the cache was unavailable, which is
true and is not the interesting half. On a forty-machine estate a mistyped
`FASTCACHE_ADDR` made every build local while the fleet sat idle and healthy
(issue #236). The rule is that **a fetch outcome decides one thing only: whether the
daemon is worth sending a second command to** (`Cc::CacheIsServing`). It does not
decide whether the invocation continues, because the docs put the shared cache on a
different machine from the scheduler and an answer about one is not an answer about
the other. Three parts of that are each load-bearing:

- **A refusal and a transport failure stay distinguishable, and both carry on.** The
  daemon's own code and words go in the fall-back reason for a refusal, `fetch
  exchange failed` for a transport failure, because an operator fixes those in
  different places. What they have in common is only that neither is an answer about
  the fleet.
- **The reason is still recorded, under `Outcome::Unavailable`.** Reaching the
  dispatch path must not make a broken cache invisible — so the MISS trace is
  *skipped* when the daemon did not answer, since tracing one clears the reason
  `--show-stats` ranks and reports a broken cache as a cold one.
- **The `STORE` is skipped when the fetch was not answered, and that is not an
  optimisation but a restoration.** Before the fix a failed fetch returned, so
  nothing was ever pushed at a daemon that had just failed to answer; carrying on
  had to leave that true, or every translation unit in a build against a wrong
  address pays another connect — a *timeout* rather than a refusal whenever the
  address is remote enough to be worth pointing at, and on a refusing daemon an
  object-sized transfer paid to be told the same thing again. It is **not** a claim
  that one connect is all a dead cache costs: direct mode is on by default and asks
  for a manifest before the object fetch runs at all, so the ceiling is two either
  way, and `TryDirectMode` is what would have to change for it to be one.

**A premise can be correct when it is written and false when it is read, and
nothing connects the two.** `CacheProxy`'s STORE handler ignored the roots the
client sent, with a comment saying why: *"Canonicalization is the SHARED cache's
job ... What this tier stores is what this machine will replay."* Both halves were
true. A node was a private tier in front of `fastcached`, which canonicalized every
store, and doing it twice would have been the redundancy the comment refused. Then
[#229](https://github.com/LASTRADA-Software/fastcached/issues/229) made a node the
shared cache in its own right, and neither half survived: there was no longer a
daemon behind it doing the job, and "this machine" is not one layout, because every
checkout on it is a different one. Nothing failed at the moment the premise did.

The result was that a value stored through a node kept the producing checkout's
absolute paths in its text regions
([#319](https://github.com/LASTRADA-Software/fastcached/issues/319)). Three things
follow, and each is a rule of its own:

- **A policy every server must apply belongs with the thing it applies to, not with
  one of the servers.** `CanonicalStoredValue` now lives beside `CompileValue`
  and both servers call it. It had been a file-local helper in
  `Protocol/CompileCacheHandler`, so when a second server appeared there was one
  copy and no way to notice the other end lacked it — `grep` for the function
  answered "one caller" and that read as normal.
- **A replayed region is not diagnostics; it is a dependency record.** `/showIncludes`
  notes are handed to the build system as the object's dependencies, so a region
  naming another checkout gives this build dependencies on files it will never
  edit — and no edit here can then invalidate that object. Wrongness that arrives
  once persists indefinitely.
- **A path with no `<SRCROOT>` sentinel is NOT evidence of corruption, so do not
  build a reader that treats it that way.** 92 of the 93 paths a trivial translation
  unit reports are toolchain headers, which correctly have no token; a consumer
  refusing sentinel-less paths would refuse essentially every hit. What distinguishes
  a poisoned region from a sound one is not visible in any single path, which is why
  the retirement is a schema-tag bump (`objkey-v6` / `manifest-v6`) rather than a
  heuristic.

**A manifest that revalidates nothing revalidates forever, and `all_of` is how it
gets written.** `BuildManifest` drops a reported dependency classified as toolchain,
because `toolchainStamp` covers those collectively. `IsToolchainHeader` reports every
path outside both roots as toolchain — so a header belonging to ANOTHER checkout is
classified exactly as an SDK header is, and dropped. Feed it the paths from an
uncanonicalized region and every one of them drops, leaving a manifest naming the
translation unit and not one header. `ValidateManifest` then re-hashes the TU, finds
it unchanged, and serves the object however the headers move — on a first build, in a
fresh directory, with no stale ninja graph anywhere in the story. Both sides now
refuse: `BuildManifest` answers `NoProjectDeps` when dependencies were reported and
none survived, and `ValidateManifest` refuses an empty entry set rather than passing
`all_of` vacuously. The refusal costs direct mode for that compile and the ordinary
preprocessed key still serves it, which is the same trade `Unanchored` already makes.

**A refusal that names somewhere else is an instruction, and reading it as an answer
takes the whole fleet out of distribution.** `SchedulerService` has always answered a
non-leader with the leader's endpoint. Only the interactive `--cluster-*` CLI ever read
it: the launcher dropped it into the same branch as `NoWorker` and `NoCapacity` and
compiled locally, so a single election removed every client from the fleet until forty
machines were re-pointed by hand
([#237](https://github.com/LASTRADA-Software/fastcached/issues/237)). Everything looked
healthy while it happened — the builds succeeded, the objects were correct, and the only
symptom was that they were slow.

- **The redirect is judged by PARSING the message, never by asking whether it is
  empty.** An empty message is replaced with the error table's default sentence by
  `EncodeErrorReply` before it reaches the wire, so "no leader is known" and "the leader
  is at `h:p`" arrive as the same shape. Only "does this parse as an address" separates
  them, and a client that skipped the test dials a sentence — which is a scheduler
  endpoint no operator ever typed. `Cc::RedirectTarget` is the one owner of that
  question, and `ClusterAdminCli` — which had the only previous copy — asks it rather
  than keeping a second.
- **Parsing is not splitting, and the difference is the whole rule above.**
  `SplitHostPort` takes the LAST colon and returns whatever follows it, so
  "no leader: try again" splits contentedly into a host and a port of `" try again"`. A
  redirect needs a host, a colon and a port that is a number. Not `ParseEndpoint`
  either, close as it looks: a bare port there takes the caller's default host, which
  for every caller in this tree is loopback -- so a scheduler answering `6675` would
  send the client back to itself.

  Those three refusals are `Core/HostPort.hpp`'s `ParseDialEndpoint`, and they are one
  function because `Cc::DialEndpoint` asks the identical question of the identical
  string a moment later. Two spellings of "is this an address" cost a hop whichever way
  they came to disagree: one the redirect accepts and the dial refuses is a connect
  that could never succeed, and one the dial would have taken is a leader the client
  can reach and declines to.

  This is the consumption test from the claim-record rule below, arriving at the
  opposite answer. `NotLeader`'s message was diagnostics for as long as
  `ClusterAdminCli` merely PRINTED it, and a loose test cost a confusing sentence.
  A launcher DIALS it, so it became a dependency record in that same moment — the
  bullet below says *"the moment something did, it would stop being diagnostics and
  this bullet would be wrong"*, and this is that moment, for a different field.
- **`NotLeader` is the one code whose message is DATA, and that is not an exception
  to the table rule.** `wire-and-protocol.md` says a refusal's wire code and its
  message are one fact and therefore one table row, because a ternary picking the code
  beside a separate call picking the text made them disagree. `NotLeader` obeys it from
  the other side: the table supplies its default sentence, and `Gate()` overrides that
  in the one place it refuses (`Refuse(NotLeader, LeaderEndpoint())`), so the code and
  the endpoint are still chosen together. The override is *why* the empty case has to be
  parsed rather than tested for empty -- the table's sentence is what fills the gap when
  there is no endpoint to name.

- **The chain is bounded.** Two nodes with a stale `_knownLeader`, or a partition
  healing, name each other indefinitely; without a ceiling a build spends one connect per
  translation unit per hop discovering it. Two hops is one more than a correct fleet
  needs, and running out is an ordinary local compile — which is what every other lease
  refusal already means.
- **This is the CLIENT half, and one half alone is not a fix.** A node used to
  register only with its own `--scheduler` (`main.cpp`'s heartbeat round, through
  `WorkerRegistrar`, which special-cased `UnknownLease` and nothing else), so a
  `NotLeader` there was logged and retried against the same address forever. A launcher
  that follows its redirect perfectly then reached a leader whose registry every worker
  had expired out of, and got `NoWorker` — the same outage one layer down, which is why
  fixing one half moves where it is observed rather than whether it happens. Closed by
  the worker-half section below; "a fleet survives an election" became sayable only
  when both halves redirected, and not before.
- **The RELEASE follows the lease, not the configuration.** A lease issued by the leader
  the client was redirected to must be resolved there: sent to the configured endpoint it
  resolves nothing, and the key stays marked in flight on the machine that actually holds
  it for the whole lease timeout. That is the `already-in-flight` outage #212 records,
  reached from the client's side.
- **A fixture with one scheduler cannot fail.** If the first endpoint contacted is
  already the leader, a build that follows no redirect at all passes every assertion. The
  case has to have two, and the assertion is that the SECOND is reached.
- **A scripted fleet proves the client, never the server.** `ScriptedFleet` hands the
  launcher a `NotLeader` the test wrote, so on its own it shows that a stub can redirect
  and nothing about whether a scheduler emits that shape. The two ends are pinned
  separately and deliberately: `SchedulerService_test` fixes what `Gate()` puts in the
  message (a follower names the leader, an undecided node names nobody), and
  `CacheProtocol_test` drives the empty case through the REAL `EncodeErrorReply` to fix
  what an empty one becomes on the wire. The premise the parse rests on is asserted
  there rather than asserted about here.

**A WORKER follows that redirect too, or the client's half of it arrives at an empty
fleet.** `SchedulerService::Gate()` refuses **every** verb off the leader, `Register`
included — leadership and membership are one gate and it runs for reads as well. So
while the heartbeat thread dialled the configured `--scheduler` unconditionally and
merely *logged* the refusal, an election did this: every worker went on announcing
itself to the demoted node, expired out of the new leader's `WorkerRegistry` inside
the 90 s heartbeat timeout, and the leader answered every lease `NoWorker`. A
launcher that now correctly follows the redirect then arrived at a leader that knew
of no workers and compiled locally — the same #237 outage, one layer down, behind a
green build and a fleet whose counters all read zero. Fixing one half without the
other moves where the outage is observed and not whether it happens.

- **One rule, one implementation.** The worker reads its redirect through the same
  `RedirectTarget` the launcher does, so what counts as one — a host, a colon, a
  numeric port, and never prose or a bare port — is decided in a single place for
  both. A second copy on the node would be a second thing to be wrong, and the two
  would drift in the direction the tests did not cover.
- **A refusal has to reach the caller as an ENDPOINT, not as a phrase.**
  `Register` returned `expected<void, std::string>` and `Heartbeat` a bare `bool`,
  so the redirect was already being thrown away one frame after it arrived, and no
  amount of loop in the caller could have followed it. `AnnounceRefusal` carries the
  words and the endpoint separately because they answer different questions: the
  words are for the operator and are always set, the endpoint is for the node and is
  set only when there is one.
- **A leader is committed only once a round has been ACCEPTED there.** An endpoint
  some scheduler *named* is a lead; an endpoint that took this node's registration is
  a leader. Remembering on the name alone lets one bad redirect become the endpoint
  every future round starts at, outlasting the election that caused it — and an
  endpoint that answered but refused every registrar for its own reasons (not a
  member, a fingerprint it will not take) is exactly that case.
- **A remembered leader that stops answering falls back inside the SAME round.** Not
  a heartbeat interval later: this machine is missing from the fleet for as long as
  it takes, and the configured `--scheduler` is both the endpoint an operator can
  actually fix and the one still standing after an election the remembered leader
  lost. Arriving back at the configured endpoint *forgets* the remembered one rather
  than storing it as a value equal to the default, or every diagnostic that says
  which endpoint this node is following becomes a lie.
- **`NotLeader` must not clear the worker id; `UnknownLease` must.** They are
  different sentences: one says *this scheduler is the wrong one to ask*, the other
  *the fleet has forgotten you*. The registry is replicated, so the leader a redirect
  names may well be holding the very registration that id belongs to — clearing it on
  a redirect turns every election into a fleet-wide re-registration storm.
- **The budget is per round, not per process.** A fleet that re-elects once an hour
  should spend one redirect an hour. A lifetime ceiling would follow redirects for a
  while and then silently stop, which is the same outage as never following one,
  arriving later and harder to see.
- **The policy is a testable object; `main.cpp` gets only the dialling.**
  `SchedulerLink` is pure — no socket, no clock, no logger — because the alternative
  home for it is `main.cpp`, which is in no test target. Same reasoning that moved
  `MakeWorkerLeaseValidator` out of `main`: a decision that lives there is a decision
  nothing can assert, and this list already records what that costs.

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

- **A port this node LISTENS on is a row of `NodeSurfaceTable()`, and an opener takes
  the `NodeSurface` rather than an address.** The port map lived in five places --
  `EndpointFlags`' install-time grammar (four surfaces), a `StartupPolicyRejection`
  row (raft), four `*ListenDefaultHost` constants, the six opener call sites, and the
  operator documentation -- and an operator's firewall list was the sixth
  ([#288](https://github.com/LASTRADA-Software/fastcached/issues/288)). They are one
  table now, and the guard is the **type system**: `FrameEndpoint::Start` and
  `AdminEndpoint::Start` take an enumerator, so there is no argument to pass a bare
  string to and a port cannot be opened without a row. A guard that fails the build
  beats one that fails a suite.

  What that removes is subtler than the duplication. **The default host used to be a
  caller's argument**, so the cache's loopback and the scheduler's wildcard -- which
  exist for a security reason, not a preference -- were mechanically whatever the call
  site passed. A test binding a bare port with an explicit `"127.0.0.1"` default
  against a wildcard-defaulting surface would have bound every interface and gone
  green.

  Three columns carry what the openers cannot: **protocol** (discovery is UDP and the
  other five are TCP, so a worksheet without it yields five correct firewall rules and
  one wrong one), **`defaultHost`** (the host a bare port falls *back* to -- empty for
  the compile port, whose host is `--bind`, a flag of its own), and a free-form
  **note** for what a column cannot say.

  There is deliberately **no `presence` column**: `resolve` already answers it, and a
  column restating it was wrong for raft, whose address an operator can name while
  `--node-id` is what actually binds it. No `explicitBit`: that is `OptionSpec`'s, and
  copying it here would create the fifth place while removing it. And **no
  `HostOrigin`** naming which mechanism supplied the host -- it documented rather than
  drove, and what it tried to record is enforced by code instead: discovery's sockets
  bind the wildcard whatever `--discovery` says because its *resolver* says so, never
  because a label described it.

  `--print-surfaces` renders the **resolved** configuration, never the defaults --
  a worksheet claiming `127.0.0.1` for a node started with `--listen-cache 0.0.0.0:6674`
  is a security misstatement rather than an untidy one -- so it parses the whole
  command line (`ParseFlow::Continue`, unlike `--help`) or it describes a machine
  nobody configured.

  **`--advertise` is not a surface.** It is what this node tells others to dial, not a
  socket it opens.

  **"The operator typed it" is `cfg.cacheListenExplicit`, never a comparison against
  the default** ([#286](https://github.com/LASTRADA-Software/fastcached/issues/286)).
  Why a comparison cannot answer it, and why the registration side of the same flag
  had it wrong too, is
  [`platform-service-and-config.md`](platform-service-and-config.md) under
  "provenance is not value" — read it before adding a flag whose default means
  something.
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
  - **And the declared length it checks is the COMPRESSED one, which is not what the
    request costs.** A COMPILE carries its preprocessed source in a codec envelope
    whose `rawLen` is what the decoder sizes its output buffer from, so a hundred-byte
    frame may declare a 256 MiB expansion and pass admission having reserved a hundred
    bytes -- `slots` of them are `slots` times the per-request ceiling all over again,
    now where the budget cannot see it. #241's ceiling is per REQUEST and closes only
    half of this; the budget charges `DeclaredRequestFootprint`, the larger of the
    frame length and the declared expansion, raised onto the same reservation once the
    payload has been read. It cannot be asked earlier -- the envelope is a field of the
    payload. The reasoning, including why the larger and not the sum, and why a
    footprint above the whole budget is left to the decoder rather than answered
    `EndpointBusy` on an idle worker, is in
    [`wire-and-protocol.md`](wire-and-protocol.md).
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

**The codec a reply travels in is chosen from what the OTHER end said it accepts,
and a hard-coded list is a fleet that never compresses.** Both directions negotiate
the same way — every exchange is client-initiated, so the request states what its
sender can decode and the answer picks from it, which is what buys the whole thing
without a handshake round trip. The worker's half of that was wrong three times over
and each defect hid the next (#265): `ChooseCodec` was called with the worker's own
list on **both** sides, so the client's was never read; the answer was then discarded
and the envelope hard-coded to `Identity`; and the node constructed both its
`WorkerProtocol` and its `WorkerRegistrar` with a literal `{ Identity }`, so even a
correct negotiation had nothing to pick from. The last of those governs the *request*
direction too — the scheduler files that list against the worker and the grant relays
it to the client — so a preprocessed translation unit, several megabytes on the hot
path of a parallel build, crossed the network uncompressed as well. Nothing anywhere
reported any of it: an uncompressed envelope is a *correct* envelope, so every object
arrived intact and every counter read normally.

So: a codec list is `AvailableCodecs()`, never a literal, and it is **one** function
in `CodecEnvelope.hpp` that both halves call rather than each deriving what it can
speak. `Envelope` sits beside it for the reason `Unenvelope` already does — the
encoding and decoding halves of one negotiation, in the module named for it. It takes
the peer's list and this end's, falls back to `Identity` for anything compression did
not actually shrink — the same shrink-check `Core/Compression` applies to stored
values — and always declares the **uncompressed** size as `rawLength`, because that
is the field `Unenvelope` bounds its allocation by before it decompresses a byte. A
list wider than the build can honour is not an error: `Compression::IsAvailable` is
asked at the point of use, so two peers compiled with different codec sets still
complete the exchange, and a build never loses distribution over configuration.

One consequence worth stating, because a comment in `CodecEnvelope.hpp` used to rest
on it: "the source arrives `Identity` — the only codec a node negotiates" was true
only *because* of this defect. `UnenvelopeText` exists to spare that path a copy, and
after the fix it is the compression-less build's path rather than every build's.

The property to assert is what the two ends **disagree** about, which is why a single
"the object round-trips" case is worth nothing here: it passes under every one of the
defects above. What separates them is a client that accepts a codec being answered in
one, and a client that accepts only `Identity` being answered in `Identity` — with an
object that actually compresses, or the shrink-check makes both cases `Identity` and
the pair is indistinguishable again.

- **A scratch root is CLAIMED, not merely named — and claiming is the liveness
  check.** A worker's root came from `temp_directory_path() /
  "fastcache-compile-node"` with jobs numbered from a counter starting at 1 in every
  process, so a second node on one host derived the identical `job-1` and everything
  beneath it. `create_directories` succeeds on a directory that already exists, so
  nothing was told anything (#279). Two outcomes, and the quiet one is worse: one
  node's `ScratchGuard` removes the shared directory under the other, whose compile
  fails and whose client falls back to compiling locally — the build stays correct
  and **distribution silently stops**, which is #232's shape again. Or the two share
  `tu.o` and one answers with the other's object, which its client caches under its
  own key.

  The fix is an **OS-level exclusive claim held for the process's lifetime**, and
  three properties follow that a pid-inspecting design would not have. There is no
  race, because acquiring the lock IS the answer rather than something checked before
  taking it. `_Exit` stops being special: `WorkerServer`'s abandoned drain (#239)
  bypasses every destructor, but the OS releases the lock however the process died,
  so a leaked root is one whose lock is FREE — which is exactly what "reclaimable"
  should mean, with no staleness to infer. And a recycled pid cannot confuse it,
  because nothing reads a pid.

  Consequences that are each load-bearing:

  - **`flock`, never `fcntl`.** The rulebook already says this for the CoW store
    because an fcntl lock is per process. Here it bites twice: two claimants inside
    ONE process would both take an fcntl lock and both succeed, which is the
    in-process form `a22e056` fixed — and it would **pass the two-runner model this
    defect was reproduced with**. A guard that its own test cannot fail is worse than
    no guard.
  - **The lock file lives BESIDE the root, never inside it.** Reclaiming empties the
    root, and so does ordinary success. On POSIX it is worse than untidy: deleting an
    `flock`'d file is legal, and a second process may then create a fresh file at that
    path and lock *that*, giving two live owners of one root.
  - **The lock decides ownership; the record never does.** The claim file carries a
    versioned record, and on a root whose lock we hold an unknown version is stale
    data to overwrite rather than a refusal — `FleetHistory`'s rule that no state of
    a file may keep a node from starting. Making the version load-bearing would mean
    a bump left every root unclaimable and no node able to start.

    Calling that record *diagnostics* is earned here rather than asserted, because
    the rule above says a replayed region is **not** diagnostics but a dependency
    record. The test is whether anything consumes it: a `/showIncludes` region is
    handed to a build system as the object's dependencies, while nothing reads this
    one but a person looking at the directory. Nothing branches on it, so it decides
    nothing — and the moment something did, it would stop being diagnostics and this
    bullet would be wrong.

    The `objkey-v6` / `manifest-v6` bump above is not a counter-example either. That
    version identifies stored VALUES that must not be reused, so it has to be
    load-bearing; this one labels the format of a note attached to a resource whose
    ownership the operating system decides. A version is load-bearing exactly when
    something downstream would otherwise trust bad data.
  - **No unclaimed fallback.** `FilePageStore` opens unguarded when a filesystem
    cannot lock, because refusing would stop a working deployment and a second opener
    is only a possibility. The reverse holds here: an unclaimed root IS the defect and
    two nodes collide by construction. `TEMP`/`TMPDIR` relocates the root, so the
    escape hatch exists without a flag — which is also what the end-to-end fixture
    uses as its control.
  - **The claim belongs to the WORKER tier.** A machine that schedules and compiles
    nothing has no use for a root and must not fail to start for want of one (#206),
    so the claim is conditional on serving compiles and says so.
  - **Counted where a counter can be read.** Reclamation gets
    `WorkerScratchRootsReclaimed`; the refusal to claim does not, because that path
    exits the process and nothing would ever scrape it — an operator learns of it by
    the node not starting, and the log names which refusal it was.

**A COMPILE reply is tied to the request that asked for it, or it is refused.**
Nothing did that before #280. The cache key covers the inputs, the fingerprint covers
the toolchain and the lease covers the authorization -- every one of them **upstream
of the reply** -- so a client sent a job and accepted whatever object came back on
that connection. Any defect crossing two jobs therefore produced a build that
succeeded with a **wrong object under a correct key**, and nothing downstream could
notice: it is silent, the client stores it, and every other machine that fetches that
key gets it.

**Where the digest is computed is the entire mechanism.** It is taken in
`CompileJobRunner::Run`, from the argument vector about to be spawned and the text
just written to scratch -- never folded in `WorkerProtocol` from the decoded request.
At the wire layer both of two crossed requests are still *pristine*: a digest taken
there would be computed from job A's fields, travel with job B's object, and agree
with whatever the client compared it against. That implementation satisfies the
ticket's own acceptance criterion -- swap two in-flight replies, watch the client
refuse -- and catches nothing. `WorkerProtocol` therefore takes `ICompileJobRunner`,
because a fake at the inner `IProcessRunner` seam sits *below* the point where the
runner records what it compiled and so can make the object wrong but never the
report.

**What it covers is generated by a constraint, not chosen.** A field belongs in the
correlation exactly when the CLIENT knows it before sending **and** the runner
observes it at execution: a value only the worker knows cannot be verified, and a
value only the client knows cannot be reported on. That admits the preprocessed text,
the client's argument slice, the fingerprint and the source name -- and excludes the
resolved compiler path, the scratch paths, `-c`, the accepted codecs, the object
itself (that would make it a checksum of the reply rather than a correlation of reply
to request) and the lease token (a credential authorizes a job, it does not describe
one, and folding it into a value that travels is how one reaches a log).

Two of the four get missed. The **fingerprint**, because two jobs identical in text,
flags and name but built for different toolchains have different correct objects and
crossing them is invisible in every other field. The **source name**, because a
compiler records the name of the file it was handed -- the COFF/ELF `.file` symbol,
seven bytes on clang-cl -- so two otherwise identical jobs really do differ. It is
covered **raw**, before `SafeSourceName`, so the client is not made a second author
of the sanitization rule; that makes the digest finer than strictly required, which
is safe, where coarser would be a hole.

**The base name is derived once on the client.** The worker digests the name it was
SENT, so a client that sent one spelling and verified another would refuse every
honest compile -- a fleet that silently stops distributing rather than one that
mis-serves, which is harder to notice and reads as "distribution just is not
helping". Two `BaseName` calls beside each other is exactly how that arises.

**The refusal is a refusal.** `DispatchStatus::Mismatched` is its own enumerator and
is checked before the object envelope is opened -- a correlation is a string
comparison, an envelope is an allocation the peer's declared length decides. There is
no best-effort match and no fallback to using the object anyway. The build still
succeeds, because the client holds the source and compiles locally as it does for
every other non-answer; what it must never do is *use* that object.

**And it is not counted, deliberately.** The issue asked for a `MetricsCatalog` row
and there is nowhere for one to live. Only the client can detect a crossed reply -- a
worker that knew its reply was crossed would not have sent it, so the server has
nothing to count -- and the client is `fastcache-cc`, one short-lived process per
translation unit with no metrics sink anywhere in its production path. A row
incremented only by tests would export a `/metrics` series reading a permanent zero
**while the defect fires**, which is worse than no series: it is the
reclaimer-nothing-constructs shape and the series-an-operator-was-told-to-scrape
shape at once, and it misinforms rather than merely failing to inform. So **no
fleet-wide aggregate of this exists and none can** until a client has a way to report
one. The two places the fact lives are an *unconditional* stderr line -- not behind
`FASTCACHE_VERBOSE`, because a correctness alarm and a "distribution did not help
today" note must not share a verbosity level -- and a fixed detail string that
`--show-stats` ranks by cause. The recorded outcome stays a MISS: the cache answered
honestly and still stores this object, so recording `Unavailable` would blame the
cache and file the source under "never cached", both untrue.

**Read the scope, because read as unconditional it makes something else look
redundant.** This catches a reply routed to the wrong waiter, and a runner that fed
the driver something other than what it was handed. It does **not** catch a runner
that fed the right bytes and read back the wrong object file -- there the metadata is
honest and only the object is foreign, and no input-side digest can see that. That is
#279's failure and #279's exclusive scratch claim is what closes it. #279 secures the
output side and this the input side; **neither alone is sufficient**, and removing
either because the other exists is how this codebase's worst regressions have
happened. It is also `MurmurHash3` with no key, so it is integrity against
**accident**: a worker that can return a wrong object can return a wrong digest just
as easily. Not a security control, and the header says so before somebody cites it as
one in a design review.

**What a test must separate here is the two ends disagreeing.** A case that asserts
the field is present proves nothing, and so does one that computes both sides with
the same helper -- they agree by construction. The cases that earn their place are a
crossed pair (both directions, with an uncrossed control, or a client that refused
*everything* would pass), a table over the four covered fields, and one that drives
`Dispatch` against a real `WorkerProtocol` over a real `CompileJobRunner` through the
real framing, with an empty argument and one containing a space. That last one is the
only thing that would catch an encoding that drops a field on the way.

## Open work

- **[#371](https://github.com/LASTRADA-Software/fastcached/issues/371)** — a scheduler
  that loses leadership between granting a lease and being handed it back refuses its
  own release, because `Gate()` puts leadership ahead of every verb. The client is
  right and the key stays pinned on the only machine that could free it, until it
  expires. Routing the release to the new leader instead is strictly worse and the
  fleet harness demonstrates why: two schedulers number their leases independently and
  both start at one, so that release matches another client's live lease for the same
  key and frees a job somebody is still running. So the fix is on the scheduler, and
  which shape it takes — exempting `Release` from the leadership check, sweeping on
  demotion, or accepting the flap window — is the open question.
- **[#303](https://github.com/LASTRADA-Software/fastcached/issues/303)** — a scheduler
  with no `--cluster-key-file` signs nothing and only warns, while the WORKER half of
  the same question is now a startup refusal (#282). The objection this issue was
  filed on — refusing would break every single-machine install, which is what most
  people run — is answered by the shape #282 landed on: ask whether a machine that is
  not this one can reach the surface, not whether a key is configured, and a
  single-machine install is out of scope by construction. What is left is applying
  that predicate to `--listen-scheduler` rather than to `--bind`.
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
