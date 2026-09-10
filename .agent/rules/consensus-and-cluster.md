# Consensus and cluster membership

Rules for `src/FastCache/Consensus/` and `src/FastCache/Cluster/`: Raft itself,
the LAN discovery beacon and its pre-shared-key handshake, the replicated cluster
configuration, and the admin verbs that change it.

Read this before touching `RaftNode`, `RaftLog`, `RaftDriver`, `RaftWire`,
`RaftPeerTransport`/`RaftPeerServer`, `DiscoveryService`, `PeerDirectory`,
`ClusterState`/`ClusterStateMachine` or `MembershipPolicy` — and before adding a
verb to the cluster-admin surface.

Every rule below has already been a bug.

## Discovery and the pre-shared key

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
  - **A peer this node cannot NAME is a peer it does not remember.** `NoteBeacon`
    refuses an id or endpoint that is empty or is not valid UTF-8, alongside the
    wrong-cluster and own-beacon filters, because what the directory holds is what
    is eventually proposed as a `ClusterMember` -- and every surface reads that back
    out as text (#159). Filtered here rather than at any later layer, which is what
    keeps a permanently-refusable proposal from ever being generated; it also keeps
    such a peer out of the challenge table, out of `Peers()`, and out of the line
    logged when a peer proves the key. The beacon's *cluster id* is deliberately
    exempt: it is compared and never recorded, and filtering it would take every
    peer away from a fleet named in some other encoding, silently.
  - **A claim a peer has not proved is printed only once it is TEXT, and never
    unthrottled.** The mismatch line for a proof does name the endpoint it claimed --
    that is a real diagnostic, because the ordinary cause is a peer that moved -- so
    what is refused first is a claim that is not text, before the line that would
    have carried it. An unnameable beacon has no such diagnostic to offer and is
    reported by the address it came *from* instead. Both are provokable by anything
    on the segment holding no key, which is why the beacon line is rate-limited and
    why neither may ever grow a table.
- **A node listens where the segment shouts and answers from an address of its
  own. Sharing a port buys hearing a broadcast and nothing else.** Every node binds
  the beacon port on the wildcard, shared, because a beacon is a broadcast and a
  node listening anywhere else would send perfectly and hear nothing. But two
  sockets on one UDP port both receive a broadcast and only **one** receives a
  unicast -- measured, Windows 11 hands it to the first-bound socket and Linux to
  the last -- and the challenge and the proof are both unicast to `received->from`.
  A node that answered out of the shared socket would be answering for its
  *machine*, so two nodes on one host saw each other's beacons and silently never
  finished proving the key (#126). It holds two sockets now: the shared listener,
  which never sends, and a private one, which sends everything and receives the
  answers. `Net/SharedPortDatagram` pairs them, so `DiscoveryService` still holds
  one socket -- which datagram left from where is a question about sockets, not
  about what a datagram means.
  - **Which socket takes which of the four options is `OpenSharedPortUdpSocket`'s
    to know, not a caller's.** Transposing the two ports yields a node answering
    where the segment shouts; putting the broadcast capability on the listener
    yields one that is reachable and never announces itself. Every wrong pairing
    still starts, and still passes a suite that only drives the in-memory bus.
  - **`PortSharing::Shared` sets `SO_REUSEPORT` too, and that is one intent in two
    spellings.** `SO_REUSEADDR` permits a duplicate bind on Linux and Windows; on
    BSD and Darwin it permits one only for a *multicast* address, so without this
    the second node on a macOS host cannot bind the beacon port at all.
  - **Answering happens on a kernel-chosen port, so a firewall scoped to the beacon
    port alone is no longer enough.** It passes the beacons and drops every
    challenge and proof, which presents as peers seen and never admitted --
    the same silence, moved. `--discovery-reply-port` pins it, one port per node on
    a machine, and naming the beacon port there is refused rather than left to fail
    at bind.
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
- **One key, one signing construction, and the domain label is a required
  parameter rather than a constant each signer remembers to fold in.** The
  pre-shared key MACs a discovery proof and a lease token, and each used to build
  its message inline out of `HmacSha256` and `WireFields::Encode` -- which are
  primitives, not a construction. So what a message is *made of* was written
  twice, and the requirement that every message carry a domain label was true of
  exactly one of them: the lease carried `fastcache-lease-v1`, the proof carried
  nothing. Nothing was reachable, and that is the point rather than the reprieve
  it reads as -- **the safety was a property of the PAIR, not of either
  construction.** A proof was a four-field encoding beginning with a cluster id
  and a lease tag a two-field one beginning with a literal, so no byte string was
  a valid message under both. That is a coincidence, and it survives exactly as
  long as nobody adds a field to discovery or adds a third signer whose shape
  collides with one of them. **Arity is not domain separation.**
  `Cluster/ClusterSigning.hpp` is now the only thing in `src/` that calls the
  primitive (#402), and five things about it are each what some plausible simpler
  design gets wrong:
  - **The domain is a `SigningDomain`, and it has no default.** There is no
    argument to pass a bare label to and nothing to omit, so a signer is a row of
    `SigningDomainTable` or it does not sign. The labels are `static_assert`ed
    present and distinct: an empty one encodes as a zero-length field and
    separates domains exactly as well as no label did, and a duplicated one is
    that same hole reached by copying the row above and changing only the
    enumerator, which is how a new signer actually gets written.
  - **The label is a FIELD, not a prefix glued onto the first one.** It goes
    through the same length-prefixed grammar as everything after it, so no choice
    of first field can shift bytes across the boundary and spell a different
    domain's label. It is the object key's rule and the proof's own, applied to
    the one part of the message a caller does not supply.
  - **`VerifyFields` is the only comparison the seam exposes, and every verifier
    goes through it** -- `AuthenticateLeaseToken` for the lease,
    `DiscoveryWire::VerifyProofTag` for the proof. That second one is the whole
    reason this bullet is worth reading: the seam landed with `DiscoveryService`
    still taking an expected tag and comparing it by hand, so the property was
    true of one of the two wires it named. It was constant-time, so nothing was
    exploitable -- but #402's subject is that an invariant stated and enforced
    nowhere is not an invariant, and a seam whose own claim holds on half its
    callers reproduces the ticket inside its fix. Signing entry points stay
    public, because minting is a separate act, so a future caller *could* still
    take a tag and compare it itself; that residual is smaller than it was rather
    than gone, and `psk-signing-seam` does not cover it -- a legitimately obtained
    tag compared with the wrong operator is not a call to the primitive.
  - **What it deliberately does NOT own**, because each is per protocol and
    getting it wrong here would be getting it wrong everywhere: which fields a
    message carries, whether a weak or empty key may sign (`ReadClusterKey`
    refuses a short one at load and `AuthenticateLeaseToken` an empty one at
    verify -- two layers, one policy, and neither belongs in a MAC), and WHEN the
    MAC is checked relative to everything else. The last is an ordering property
    of a verifier and no seam can hold it for one.
  - **A guard, because the rule was already written down and enforced nowhere.**
    A required parameter binds whoever goes *through* the seam and says nothing
    about a fourth signer calling `HmacSha256` itself, which is an ordinary call
    to a public function in `Core/` that no compiler will remark on. `ctest -R
    psk-signing-seam` is what refuses that, and it fails when its own scan matches
    nothing -- a table row vouching for a file that has stopped signing, or a
    primitive renamed out from under the scan, would otherwise leave it passing
    vacuously forever. `psk-signing-seam-selftest` drives all seven verdicts,
    including the two that must PASS.
- **Giving the proof a label changed the proof wire, and the datagram version did
  not move.** Every tag differs from a pre-#402 build's; `CurrentVersion` stays at
  1 because what changed is the MAC *input* and not the grammar, and bumping it
  would misdescribe the format. It is also the better failure of the two: an older
  node reaches the proof step and is logged failing to prove the key, where an
  unsupported version is dropped by `ClassifyDatagram` and presents as peers seen
  and never admitted -- the same silence this file already records
  `--discovery-reply-port` being needed for. Loud beats silent; a cluster that
  quietly will not form while every node looks healthy is the shape that costs a
  day. The lease token's message is byte-for-byte what it was, and a test pins
  that against the pre-seam construction written out as a literal rather than
  against the code that produces it -- "the tests still pass" cannot show it,
  because the tests moved with the code.
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

## A refusal code carries its own permanence, and there are THREE answers

`ConsensusErrorCode::InvalidConfiguration` had two producers that meant opposite
things. `Cluster::Validate` returns it for a command nothing could ever apply --
permanent. `RaftNode::ProposeMembership` returned it for *a membership change is
already in flight; wait for it to commit* and for *the proposed member set is the
current one*, both of which clear on their own.

`SubjectOf` is a `constexpr` table over that enum and therefore reads as a global
fact, so it was correct only on the entry-propose path and wrong on the membership
one. What guarded that was a sentence in its own doc plus `ConsensusTier::ReconcileQuorum`
declining to consult it -- the weakest kind of guard, and the obvious next tidy-up
would have wired it in and reported *wait for the change in flight to commit* as
**can never be recorded as it stands**, at Warn, every reconcile interval.

The fix is that the CODE carries the property (#196):
`ConfigurationChangeInFlight` and `MembershipUnchanged` are enumerators of their
own, and `SchedulerService`'s `WireCodeFor` -- an `EnumTable` over the same enum --
fails the build until each has decided what it says on the wire. Both new wire
codes are in `UncountedRefusals`: one is what a healthy cluster answers while a
change it accepted replicates, the other is an idempotent request arriving twice,
so a rise in either would measure how often somebody retried.

**`RefusalSubject` gained a THIRD value, and that is the part worth remembering.**
*The proposed member set is the one already in force* is a refusal only in the
sense that nothing was appended: the caller's goal is TRUE. Folded into `Command`
it is reported at Warn as a record somebody must go and correct; folded into
`Moment` it abandons a pass that had nothing left to do. Both are the misleading
symptom the classification exists to remove, so `Satisfied` is named -- the
four-states rule arriving in a consensus taxonomy.

It stays a REFUSAL rather than becoming a success, and the reason is not
squeamishness: a success would have to carry a `Proposal` naming an entry that
does not exist, and `Cluster::NextQuorumChange` never proposes an unchanged set --
so the state a success would force every caller to handle is one production does
not reach. The ticket asked whether it is a refusal at all; that is the answer,
and it is recorded here because the question will be asked again.


## The replicated cluster configuration

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

## Raft

- **A node IS its state directory, and its identity is MINTED there rather than derived
  from the machine**
  ([#1024](https://github.com/LASTRADA-Software/fastcached/issues/1024)). An id had to
  be invented per machine and typed twice -- once as `--node-id` and again inside
  `--raft-peer=<id>=<host>:<port>`, because a node that does not name itself is refused.
  It is now written into `--cluster-dir` on the first start and read back on every one
  after; `--node-id` remains as an override and is RECORDED, so it is typed once.
  - **Not the hostname**, which is the obvious default and fails on the property that
    matters: a node id is durable identity in a replicated log, and a
    `hostnamectl set-hostname` or a re-image would silently re-identify the machine --
    the cluster counting a member that no longer exists beside a stranger nobody
    admitted. It is also not unique per node, since one machine may run one node per
    toolchain.
  - **Not the OS machine-id either, and NOT AS A SEED.** The ticket proposed seeding
    from an application-specific derivation of `/etc/machine-id` / `MachineGuid` /
    `IOPlatformUUID`, and that is unsound in two directions this tree can show. It makes
    the ticket's own stated property FALSE -- two machines cloned from one image, each
    with no state yet, mint the SAME id, silently, which is the duplicate the derivation
    was chosen to avoid. And an identity that survives losing the state directory is
    worse than one that does not: that directory holds the Raft log and the vote record,
    so a node returning under its old id having forgotten which term it voted in is
    `--cluster-dir`'s own documented hazard -- two leaders in one term -- arriving
    automatically. **A wiped state directory MUST produce a new identity.** So the mint
    is 128 bits from `IRandomSource` and there is no platform seam for a machine-id at
    all: folded in beside fresh randomness it changes no outcome, and a three-platform
    reader whose value decides nothing is a claim with no reader.
  - **A derived id cannot be typed, so `--raft-self=<host>` is not separable
    ergonomics.** It states the address peers dial and takes the port from
    `--listen-raft`, because a bare `--listen-raft` binds the WILDCARD and what a node
    binds is routinely not what a peer can dial. `RaftSelfEndpoint` is the one author of
    the pair: the rule that refuses a `--raft-peer` CONTRADICTING it has to recognise the
    entry `ApplyNodeIdentity` synthesised, or a node accepted at startup has every reload
    refused by name -- the reload path judges a candidate the identity has already been
    applied to.
  - **The startup rules stay pure functions of the command line, so identity resolution
    runs AFTER them.** The self-peer rule therefore asks `ClusterSelfMember(c) == nullptr
    && c.raftSelf.empty()` rather than looking for the member afterwards; `--install-service`
    reaches it, and a rule that needed the filesystem could not be one of the table's.
  - **The resolved value is applied to EVERY configuration this process builds** -- the
    running one, the one a registration bakes in, and every reload candidate -- through
    one function. A candidate rebuilt without it holds an empty `--node-id`, which is an
    unreloadable field that has CHANGED, so every reload would be refused by name on a
    worker whose configuration was perfectly valid.
  - **`--node-id` is the one flag emitted on VALUE rather than on provenance**, and its
    `explicitBit` is deleted rather than left unread. `emitIfSet` was removed by #713
    precisely so nobody reaches for it; this row is written out at its call site instead,
    because the flag no longer has a DEFAULT for a value comparison to be wrong about --
    it has a value minted on this machine, and a registration omitting it would let a
    re-image answer to an identity the cluster never admitted.
  - **A recorded id that is empty or is not text is REFUSED, never re-minted.** Replacing
    an identity because a file was hard to read makes this node a member the cluster has
    never heard of while the one it counts is gone, and both machines are up throughout.
  - **A state directory copied to a second machine copies the node, and that is NOT
    refused.** It cannot be refused where it would be seen: `--cluster-admit` re-pointing
    an id at a new address is how an operator records a node that has MOVED, so the
    request is byte-identical to the honest one, and the fact that separates them --
    whether the old endpoint still answers -- is wrong in both directions at the moment
    it is asked. A moved node's old address never answers; a cloned one's answers only
    while both happen to be running. What IS done is that the case is documented where
    an operator meets the directory, and that resolution never mints for an invocation
    that only asks a question (`--print-surfaces`, a cluster verb, `--uninstall-service`)
    -- a flag whose own comment says it changes nothing must keep saying so.
- **The hostname is a LABEL on the fleet page and decides nothing.** It reaches the
  leader on REGISTER's nested capacity record, beside `version` and for the same arity
  reason, and renders as a `name` column. Nothing keys on it, routes by it, admits by it
  or dispatches by it -- which is precisely what makes it safe to carry a value that is
  mutable and not unique per node, the two properties the identity was deliberately not
  built on. It goes through the same UTF-8 gate as every other string a peer sends, and
  "it decides nothing" is not a reason to exempt it: one byte makes `/fleet.json`
  unparseable for the WHOLE fleet, and this is the field most likely to arrive in a
  legacy code page, because the machine chose it rather than this project.
  - **And no prefix matching on ids, however tempting `--cluster-forget=a3f5` looks.**
    An abbreviated identifier is a DISPLAY form; the full one is read, never padded,
    truncated or re-derived. Full id, or the display name.
  - The wire seam is where a display name is LOST invisibly: `SchedulerService_test`
    builds a `WorkerRegistration` in memory, so deleting the line that carries the name
    off the nested record leaves that whole file green -- measured. The case that fails
    is in `SchedulerProtocol_test`, driving a real encoded REGISTER.

- **A mode rides on the PORT, never on the absence of a NAME**
  ([#1022](https://github.com/LASTRADA-Software/fastcached/issues/1022)).
  `RunsConsensus` read `!cfg.nodeId.empty()`, so consensus was switched by an
  identity — and an identity whose absence carries a mode can never be given a
  default. Any default at all makes `nodeId.empty()` false forever, so
  `ClusterSelfMember` finds no member on a machine that names no `--raft-peer`,
  `ConsensusNamesNoSelfPeerRefusal` fires, and the one-machine deployment — the
  common one — refuses to start at every boot AND at `--install-service`, where the
  registration replays the same command line forever. That is not a tuning problem:
  the identity cannot be derived while the switch lives on it.
  - **`--listen-raft` rather than a new `--cluster` boolean.** A boolean is a second
    thing that can disagree with the port, and both disagreements are states nothing
    could describe: a node that opens a consensus port and runs no consensus, and one
    that runs consensus and opens none. The port *is* the fact, which also keeps the
    rule at one flag.
  - **Asked of the surface ROW, never of `cfg.raftListen`.** `RowFor(NodeSurface::Raft)
    .Resolve(cfg)` is where "is this surface served" is decided for every surface, and
    `--print-surfaces` prints from it. Reading the member directly would be a second
    author of that, so a worksheet could name a port the mode says is off. A value that
    is not an address resolves to nothing here and is refused by the grammar walk at the
    top of `StartupPolicyRejection`, which runs before every rule that consults the
    predicate and before any tier exists.
  - **One predicate, still.** #613 was `StartConsensusOrExplain` and `SchedulerTier`
    authoring this one rule apart, and the symptom was the scheduler answering `Lease`
    as a leader that had never been elected. A MOVED rule is exactly when a second
    author reappears — a tier that re-spelled `cfg.nodeId.empty()` would compile, pass
    every case that gives both flags, and be wrong the day the identity gains a
    default. `AdmissionSummary` was a third author of it and had to move too, or a
    clustered node's ready line tells an operator it admits this machine only.
  - **The five refusals did not all move the same way, and two of them INVERTED.**
    `--node-id` and `--raft-peer` were things a consensus node needed; they are now
    things that configure nothing on their own, so each is refused for naming a cluster
    with the switch off. `--raft-join` and `--discovery` re-point at the new switch and
    keep their meaning. `--listen-raft` with no `--node-id` becomes the ORDINARY case
    and is refused by nothing, which is the whole point. Assert the MESSAGE and not the
    refusal: under the old switch every one of these inputs was refused too, so a test
    counting refusals passes whichever flag the predicate reads.

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
  - **`RaftPeerTransport::Start()` was called by nobody.** The outbound side owned a
    thread per peer then, started on request, so every node came up, listened,
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
  disruption pre-vote exists to prevent. The residual it left — a **leader** never
  hears from a leader, so its own `_lastLeaderContact` ages out and it granted a
  challenger's pre-vote, exactly as it did before, since a leader arms no election
  timer either — is what the next entry closes.
- **A leader answers pre-vote from its OWN quorum, because a leader never hears
  from a leader (issue #103).** `OnPreVote` read `_lastLeaderContact`, and
  `NoteLeaderContact` sets that only where a leader spoke to *this* node — an
  accepted AppendEntries or InstallSnapshot. A leader executes neither for its own
  term, so its copy is absent or left over from a term it no longer holds, and past
  `electionTimeoutMin` an established leader **granted** every challenger it was
  asked about: the node best placed to refuse was the only one that never did.
  What makes it bite is not a dead leader — a challenger only campaigns after its
  own timeout has expired, so in the ordinary case the leader really is gone and
  granting is correct — but the case where the leader is alive and only *that one
  follower* lost contact: an asymmetric partition, a saturated link, a paused
  process. The cluster then re-elects for no reason at all. `HasLiveLeader` is now
  where the two roles' evidence lives: every other role still asks
  `_lastLeaderContact`, and a leader asks `HasQuorumContact` — CheckQuorum, decided
  from responses it already receives rather than from a lease, which would need a
  clock-drift bound nothing else here assumes. Consequences that are each
  load-bearing:
  - **Refusing *without* tracking the quorum would be worse than granting.** A
    leader that said no unconditionally is a partitioned leader vetoing its own
    replacement forever, so the two halves are one mechanism and neither ships
    alone. Both are tested — and the "a leader that has lost its quorum grants"
    case passes before the fix as well as after. That is not a weak test: it is not
    a regression test at all, but the guard against over-correcting the other one,
    and it is the half a later change is most likely to break.
  - **A rejected response counts as contact, which is why the record is not hung
    off `AdvanceFollowerProgress`.** That funnel is the obvious place and is
    reached only by the accepted branch, while a rejection says the follower's
    *log* disagrees, not that the follower is gone. A leader that counted only
    accepted responses would lose the quorum it is in the middle of repairing. The
    same argument puts the call ahead of `OnInstallSnapshotResponse`'s branch too:
    a follower far enough behind is caught up by snapshot and answers on that
    message and no other, so counting only AppendEntries loses a quorum during
    exactly the repair that needs it.
  - **Winning an election IS contact from a quorum, so `BecomeLeader` seeds the
    record from the votes** — from the voters, not from `_peers`, because only the
    voters actually spoke. Without the seed a new leader answers "I have no quorum"
    until its first heartbeat comes back, and grants pre-votes for that round trip:
    the moment a cluster is least able to afford another election. It is cleared
    first, so contact from a leadership this node has already lost and regained
    cannot pass for contact with this one. Every voter is stamped with the instant
    the election was *won* rather than the instant its own vote landed, which
    overstates liveness by the election's duration — deliberately, because what is
    true at that instant is that a majority endorsed this node for this term, and
    buying the difference back would mean making `_votesGranted` a map to correct a
    skew bounded by one round trip that the first heartbeat corrects anyway.
  - **The window is `electionTimeoutMin` and the comparison is strict.** It is the
    soonest any peer could start campaigning, so it is the shortest silence that
    could mean this leader is about to be challenged. This bullet used to end
    "matching the follower side exactly", on the reasoning that a leader answering
    on a window of its own is how two nodes disagree at the same instant — **and
    that reasoning was exactly backwards.** See the entry below.
  - **This WAS not leader step-down, and #437 made it one.** The paragraph that
    stood here said CheckQuorum elsewhere also *deposes* a leader that has lost its
    majority, that this was a separate mechanism, and that "`RaftCluster_test` and
    `RaftClusterHarness::Leader` still record that nothing here deposes a leader,
    which is what their assertions actually rest on". #437 then added exactly that
    deposition — `Tick` calls `RelinquishLeadership` when `HasQuorumContact` fails
    — and this file was not touched, so for the whole of that time the rulebook
    told every session the mechanism did not exist. That is the shape this file's
    own `## Open work` rule is about, reached the other way: an entry saying
    something *cannot* happen instructs the next session not to look for it, and
    #1061 is what it cost. The reason it is deposition rather than a nicety is
    #437's: Raft needs none of it for safety, since a partitioned leader commits
    nothing, and everything that READS from a leader needs all of it — two nodes
    answering `--cluster-status`, a `--cluster-set` reported `accepted` that can
    never commit, a fleet page served `200` by a node that no longer leads.
  - **No cluster case covers it, and why is worth recording rather than
    apologising for.** `RaftClusterHarness` is this module's oracle, so the first
    attempt was a one-way link cut — written for exactly this, and then removed,
    because the case it produced passed against the defect. A challenger's
    pre-vote only decides anything if the leader's **grant reaches it**, and that
    grant travels the same path as the heartbeats whose absence made the
    challenger campaign. A follower that stops hearing the leader therefore also
    stops receiving its answer — and stops answering it, so the leader loses that
    follower's contact in the same instant and correctly grants. Losing contact
    with a follower and losing that follower's responses are **one event**, which
    is why no persistent topology separates them: not a one-way cut, not a
    two-sided one, not any number of them. What separates them is transient
    trouble — a saturated link, a paused process, a partition healing just as the
    timer expires — and reproducing that needs the grant to win a race against the
    next heartbeat, which is arithmetic over the step size, the per-message delay
    and the heartbeat phase. A case resting on that reports a future regression as
    a flake, which this rulebook already records paying for once. The six
    `ManualClock` cases on `RaftNode` pin the rule instead, which is where it
    lives: the node reads no clock of its own, so each of them is exact. It is
    also the answer to why the defect survived being written down as a residual —
    the harness that found five other consensus defects could not have found this
    one.
- **Two nodes asked about the same leader can share a window and still disagree,
  because they measure from different instants (issue #117).** `NoteFollowerContact`
  stamps when a **response** arrives; `NoteLeaderContact` stamps when the
  **request** does. On one link the leader's stamp is therefore always the later of
  the two by half a round trip, so comparing both against one `electionTimeoutMin`
  leaves a band — half a round trip wide — in which a leader still counts a
  follower toward its quorum while that follower has already told a challenger
  there is nobody in charge. The entry above claimed a shared window bought a
  shared answer; it never could.
  - **In a three-node cluster that band is decisive, not marginal.** A challenger
    needs ONE grant, so the ignorant follower's grant is a quorum with the
    challenger's own vote and the leader's refusal buys nothing. The fix from
    issue #103 — teaching the leader to refuse — was necessary and, on its own,
    inert against this.
  - **So a non-leader answers from its own belief, not from a clock it shares with
    nobody**: `_knownLeader.has_value() && now < _electionDeadline`. A node
    authorizes a challenger exactly when it has given up on its own leader, which
    is the instant it would stand itself. That is coherence with its own behaviour
    rather than agreement with the leader's clock — and the latter is not
    available at any price, because the two sides measure one link from opposite
    ends.
  - **The conjunct is what keeps this from being the version #103 replaced.** That
    one read `_electionDeadline` ALONE, and the deadline is re-armed when a node
    begins its own pre-vote round — so a node that had just started campaigning
    refused every peer timing out alongside it, at a cost of some forty election
    rounds for a five-node cluster. `StartPreVote` also clears `_knownLeader`, so
    a campaigning node answers "no" and that livelock stays fixed. A candidate
    therefore *always* answers no, which is the point: standing for election is
    the act of declaring there is no live leader.
  - **It costs failover time, deliberately.** A follower used to grant after a flat
    `electionTimeoutMin`; it now grants after its own draw from [min, max], so a
    genuinely dead leader is replaced once the SECOND smallest draw among the peers
    expires. Bounded by `electionTimeoutMax`, which the design already accepts for
    one election round, and paid to stop a HEALTHY cluster re-electing — an
    unnecessary election costs a term and a leaderless window too, and happens far
    more often than a leader actually dies.
  - **`_lastLeaderContact` is gone, and its absence is the rule.** Nothing read it
    afterwards. A written-but-unread record of "when did a leader last speak" is
    worse than none: the next person needing that question answered reaches for it
    without noticing it no longer decides anything.
  - **No cluster case can pin this either**, for the reason the entry above gives:
    the harness delivers every heartbeat on time, so a follower whose leader is
    punctual never reaches the rule at all. What made the real cluster re-elect was
    a runner slow enough to age out a timestamp. Three `ManualClock` cases on
    `RaftNode` pin it exactly — including one asserting a follower that has
    genuinely given up **still grants**, which passes before the fix as well as
    after and is the guard against over-correcting into a leader that vetoes its
    own replacement forever.
- **A cluster that has ELECTED is not a cluster that has FORMED, and only the
  second one is stable (issue #117).** While a member's process is still
  connecting, the remaining two carry the whole quorum — so a leader's
  `HasQuorumContact` degenerates into "has that ONE follower answered inside
  `electionTimeoutMin`". The arithmetic then guarantees the protection is absent
  rather than merely unlucky: a follower campaigns only after its own draw from
  [`electionTimeoutMin`, `electionTimeoutMax`] has elapsed *with no contact*, so
  by the instant it asks, the leader's evidence about it is at least that old and
  therefore always stale. The leader grants, and any transient stall — a
  saturated link, a paused process, the `fsync` a proposal takes under the
  driver's mutex — re-elects a cluster with nothing wrong with it. Consequences:
  - **Nothing about this belongs in the algorithm.** A leader that refused anyway
    is the partitioned leader vetoing its own replacement forever, which the
    entry above rejects for exactly this reason. Pre-vote and CheckQuorum failing
    open here is them working; the cluster genuinely has no fault tolerance left
    until every member attaches, and re-electing is the correct response to
    losing the only link it has.
  - **So the property a fixture may assert is leadership stability of a FORMED
    cluster.** `cluster-e2e.sh` asserted "elects once and never moves" from the
    moment `find_leader` saw any node answer, which is the weaker fact, and it
    reported an algorithm behaving exactly as specified as a defect. It now waits
    until one node answers as leader *and* the other two redirect to that same
    endpoint — a follower can only name it once it has taken an `AppendEntries`
    from that leader and the leader's own record has committed, which also means
    the leader holds that follower's answer and its quorum has slack again.
  - **Asked, never scraped, and re-derived every pass.** A log line proves less
    than an answer and keeps passing once the answer stops matching it, and
    leadership may legitimately move *during* formation — so the endpoint is
    taken from the pass that succeeds rather than checked against whichever one
    `find_leader` happened to see first.
  - **A role line with no term cannot answer any of this, which is why the first
    change was diagnostic.** The artifact showed three nodes moving between roles
    and nothing else: no term, and no record of what deposed anybody. An
    intermittent election is diagnosable from its logs or not at all, so
    `RaftOutput` now carries a `TermAdoption` — the term and role being replaced
    and the peer that carried the higher one — and the driver's role report
    carries the term, with the term part of what counts as a *change*. That last
    detail is load-bearing: a node disturbed round after round by a campaigning
    peer is a follower knowing no leader before and after each one, so a report
    keyed on (role, leader) alone is silent during precisely the storm somebody
    is reading the dump for.
- **CheckQuorum measures SILENCE, and a member admitted a moment ago has not been
  silent — it has not been asked
  ([#1061](https://github.com/LASTRADA-Software/fastcached/issues/1061)).**
  `ProposeMembership` adopts the new member set the instant the entry is appended,
  which is the §4.3 rule that makes the change committable at all — so `Quorum()`
  grows before the member it adds has been asked anything, while `_followerContact`
  gains nothing, because nothing could have arrived. Read as silence by the entry
  above, the leader deposes itself at its very next heartbeat, for a lack of
  contact that could not have existed. So while a change is UNCOMMITTED, CheckQuorum
  asks about the **committed** configuration — the set that elected this leader and
  still answers it ([#1095](https://github.com/LASTRADA-Software/fastcached/issues/1095)).
  - **At two members it is unsatisfiable by construction, and there is no
    recovery.** Growing from one member to two doubles the quorum, and the second
    member is the one that has never answered — so the arithmetic can only fail.
    The deposed leader then needs a vote from a node holding no configuration,
    which grants none (`OnPreVote` refuses a non-member), and that node is excused
    from every deadline by `HasCluster()`, so it does not campaign either. Both
    machines up, both listening, every verb answering `not-leader`, in term 1,
    forever. At three members and above somebody else can campaign, so the SAME
    defect presents as an election storm that settles — four terms for one
    membership change — which is why it read as a slow formation for as long as it
    did, and why a case asserting only that *a leader exists eventually* passes
    under it. **The two-member arrangement is the discriminator; the larger one is
    the control.**
  - **The member it never received is why nothing repairs itself.** A leader
    replicates from its own last index, a joiner's empty log rejects, and the
    walk-back lives in `OnAppendEntriesResponse` behind `_role != Leader`. So the
    rejection arrives at a node that has already relinquished, the configuration
    entry never lands, and the member the change was about stays a joiner.
  - **NOT seeded into `_followerContact`.** That is the shorter fix and the field's
    own comment refuses it: absence there means *has not answered since this
    leadership began*, and `BecomeLeader` fills it from the votes actually cast for
    exactly that reason. A stamp for a response nobody sent is a false record, and
    pre-vote reads the same map through `HasLiveLeader` — so the lie would decide a
    second question nobody was thinking about. The two records are kept apart and
    `HasQuorumContact` prefers the real one.
  - **The first fix was a CONSTANT, and the constant was the next defect (#1095).**
    It granted the admitted member one `electionTimeoutMin`, argued as the right
    size rather than a generous one: a leader that cannot get an answer inside one
    is a leader its followers are already timing out on. **That reasoning was drawn
    from the wrong quantity.** A joiner's FIRST exchange carries the leader's log
    append, the joiner's term adoption and a `nextIndex` walk-back over an empty
    log — two fsyncs and a round of walk-back, none of which a steady-state
    heartbeat to an established follower carries. So the window for a joiner's
    first round trip was sized against established followers' silence, and under
    coverage instrumentation or on a loaded host it is not enough. The committed
    configuration has no constant, so there is nothing to outrun.
  - **`--cluster-admit` naming an address nothing listens on now leaves the leader
    LEADING, and that is the fix rather than an over-correction.** The old argument
    was that such a leader must give up or it is pinned while able to commit
    nothing. That holds only where a SUCCESSOR exists. At 1→2 none does: the new
    configuration's quorum is two, the joiner is dead, and the deposed leader
    cannot re-elect alone — so deposing converts *a leader that cannot commit* into
    *no leader that also cannot commit*, which is this defect by another route. The
    guard that replaces it is the one the new rule can actually fail: a leader
    still steps down the moment it loses the quorum of the **committed** set.
  - **One-member-at-a-time is load-bearing for READS, not only for commitment.**
    CheckQuorum is what everything reading from a leader rests on (`Tick`), so
    consulting the old configuration has to rule out a second leader. It does,
    because `ProposeMembership` refuses any change but a single member: any
    majority of the old and any majority of the new then share one, that shared
    member would have moved to a higher term to grant a competing vote, and it
    would have stopped confirming this leader. **A future ticket arguing for
    two-member deltas that reads only the commitment argument would be wrong for a
    reason nothing warned it about.**
  - **The harness could not see it as written, and that is the reportable part.**
    `RaftClusterHarness` delivers in one to three steps, so the admitted member's
    first answer beat the leader's own heartbeat and the case stepped straight over
    the defect — measured green against it. It partitions the leader for four steps
    after the proposal now: long enough that no contact can exist at the first
    heartbeat, short enough to land inside the window the fix grants. What produces
    that delay in the field is ordinary — a leader's durability write and a
    joiner's first reply, on a sanitizer build, are not reliably done inside one
    50 ms heartbeat. The exact `RaftNode` cases are still where the rule lives.
  - **`undecided` is not a Raft role, and reading it as one sends you to the wrong
    file.** It is `SchedulerRole::Undecided` — *not leader AND names no leader* —
    so it spells Follower-with-no-leader, PreCandidate and Candidate alike. The
    node that logs it here is a plain **Follower**: `RelinquishLeadership` leaves
    the term untouched and reports no `TermAdoption`, which is exactly why the log
    shows a leadership lost with nothing named as having taken it.

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
- **A member the cluster admits must be countable AND dialable, and doing one half
  is worse than doing neither.** Membership reached the replicated state and stopped
  there: `RaftNode`'s member set came from `--raft-peer` at startup and never moved,
  and `RaftPeerTransport`'s peer table was fixed at construction — so a node admitted
  at runtime was served by every surface, voted in none, and was dialled by nobody.
  Growing a cluster's *consensus* still meant restarting its members with a longer
  bootstrap list (issue #97). Closing it is three pieces, and each of the last two was
  found by running the thing rather than by reasoning about it:
  - **A node that bootstrapped itself can never be admitted, so a joining node must
    not bootstrap.** With `--raft-peer` naming it, a new machine elects itself, takes
    a term and a log, and afterwards refuses `AppendEntries` from every leader its own
    configuration does not name — and two clusters cannot be merged by any local rule.
    So `RaftConfig::members` may be **empty**, meaning "no cluster yet": such a node
    never stands (`NextDeadline` reports that nothing falls due, rather than naming a
    deadline `Tick` would have to decline to act on), grants no votes, and accepts
    `AppendEntries` and `InstallSnapshot` from **any** leader — because the membership
    test that guards those has nothing to test against and the only way to learn a
    member set is to be sent one. It gives nothing away: the node holds no committed
    state, has never voted, and is counted by nobody. The moment it adopts a
    configuration the guard applies again, permanently.
  - **Who a node DIALS is not who it COUNTS, and a joiner needs the first without the
    second.** The obvious spelling — `--raft-join` takes this node's own address and
    nothing else — deadlocks, and the end-to-end case is what said so. A leader
    admitting a member starts replicating at its own last index; the joiner's log is
    empty and refuses; and the leader only walks `nextIndex` back to the beginning
    when that refusal arrives. A joiner whose transport knew no peer could not send
    it, so it was admitted, dialled, and permanently silent — with nothing logged
    anywhere. `--raft-peer` under `--raft-join` therefore populates the transport and
    not the configuration, and `LearnMembers` also teaches the transport what
    *discovery* has proved, which is the only route to an address for a node that has
    not been replicated to yet.
  - **Absence in the replicated state does not mean removal.** `--raft-peer` puts a
    member in the configuration and nothing puts it in `ClusterState`, so on a cluster
    whose peers were typed rather than discovered the leader's own record is all the
    state holds. Read as "everybody else was forgotten", the reconciler proposed
    removing every peer, one commit at a time, until a healthy three-node cluster was
    one node counting only itself and refusing the other two as strangers — measured,
    once, on the first run of the end-to-end case. `NextQuorumChange` therefore takes
    the **bootstrap set** — the ids this node was started with, which `ConsensusTier`
    keeps for its whole life — and never proposes removing one of them: that is the
    fact which tells "the operator forgot it" from "nobody ever wrote it down".
    Deliberately the command line rather than a record of what this process has
    observed, and the difference is a restart: an observation is rebuilt from live
    members only, so a fleet restarted after a removal would count a forgotten member
    forever, with a correct-looking member set and nothing logged. A node given no
    bootstrap set at all — a `--raft-join` joiner — therefore proposes **no** removal
    whatsoever, which is the same rule read at its limit rather than an exception to
    it: it fails closed, counting a member too many rather than too few. Taking a
    typed member out of the quorum stays the operator's decision, made by editing that
    `--raft-peer` line.
  - **`--cluster-admit` is the counterpart `--cluster-forget` never had.** Nothing
    could put a member *into* the replicated state without `--discovery`, so a typed
    fleet could shrink and never grow. It carries `<id>=<host>:<port>` — the same
    token `--raft-peer` takes, because a second spelling of one thing is a second
    thing to get wrong — and no scheduler endpoint, because a member announces its own
    once elected and a value typed about somebody else would outrank what they say
    about themselves.
  - **The quorum follows the state, never the other way round, and one step at a
    time.** Additions come first: growing before shrinking keeps the quorum reachable
    through a replacement, where the other order passes through a configuration
    smaller than either endpoint. A member with no dialable address is never added —
    the quorum would grow and the votes to satisfy it could never arrive — while one
    already counted is never dropped for an unreadable one, since shrinking a quorum
    over a typo is how a cluster stops being able to elect. And the reconciler
    proposes nothing while its last change is uncommitted, reporting the wait once it
    becomes unreasonable: a configuration naming a member that will not accept this
    leader never commits, and from both ends that looks exactly like a cluster which
    is merely busy.
  - **`RaftPeerTransport::Learn` re-addresses in place and never forgets.** A peer
    that moved keeps its outbox and its queued messages; replacing it would destroy a
    coroutine frame the reactor may still point into. Its live socket is closed, so
    the next write fails and the sender redials — at the next *message*, because a
    sender parked on its outbox is not woken by a socket closing under it, which costs
    a member nothing and a silent peer less. A member removed from the configuration
    keeps an idle sender until the process restarts, which is a socket rather than a
    fault: ending it early needs a per-peer cancellation and a sweep for finished
    frames. The map is guarded by a **shared** mutex, for the reason
    `Distributed::MembershipOracle` gives, and nothing is held across
    `ISocket::Close` — which resumes a parked sender inline on epoll and kqueue, so a
    lock held across it is a lock held across arbitrary sender code with `Send`, and
    therefore the driver's mutex, waiting behind it.
  - **`OnInstallSnapshot` had no membership guard at all**, which was an asymmetry
    rather than a policy: it discards the log, adopts a member set out of the message
    and replaces the application's whole state, so a stranger who could reach the port
    could rewrite the cluster's configuration on a node, with a term above its own as
    the only thing to supply — while the identical attempt over `AppendEntries` was
    refused. A guard a second entry point does not apply is not a guard.

    **The same path was missing the role change too**, and for the same reason: a
    candidate that hears from a leader of its own term has lost the election, and
    `OnAppendEntries` demotes it while `OnInstallSnapshot` — which publishes
    `_knownLeader`, clears the tally and arms the election timer from the very same
    message — did not. It was survivable while `HasLiveLeader` read a timestamp, and
    stopped being survivable the moment a non-leader started answering pre-votes from
    `_knownLeader`: a node left a candidate there denies the pre-votes a candidate is
    documented to always grant, which is #103's livelock on one entry point. **Both
    halves of what a message means have to be applied wherever that message is
    handled** — the two handlers for "a leader spoke" are `OnAppendEntries` and
    `OnInstallSnapshot`, and a rule stated in only one of them is a rule the cluster
    does not have.

- **A shutdown drain here is `Core/BoundedDrain.hpp`'s `DrainWithin`, never a loop.**
  `RaftPeerServer::Shutdown` and `RaftPeerTransport::Stop` both wait for detached
  coroutines that borrow members of the object being torn down, and both are bounded
  so a stuck peer cannot turn a stop into a hang. `RaftPeerServer`'s wait was correct
  and was then copied by `RaftPeerTransport` and by the node's `FrameServer`, both of
  which accumulated the poll they *requested* instead of measuring what it cost, and
  so enforced 7.5 s and 15 s against a stated 5 (#452). Both copies named
  `RaftPeerServer::Shutdown` in a comment while reimplementing it. The ceiling and the
  cadence are `DrainBound`'s defaults, so neither site states one; the reasoning is in
  [`.agent/rules/distributed-compilation.md`](distributed-compilation.md).

## Open work

- **[#144](https://github.com/LASTRADA-Software/fastcached/issues/144)** — a
  follower answering `/fleet` names the leader but cannot link to it, because
  where a dashboard is served is local configuration and any URL it built would be
  a guess. A third recorded endpoint was priced and refused: `StateVersion` is
  checked on decode, so a field makes every existing snapshot and log entry
  undecodable. It stays open because the trigger is natural — whenever
  `ClusterState` next bumps for a reason that carries the migration on its own,
  this rides along.
