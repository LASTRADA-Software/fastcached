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
  - **The window is `electionTimeoutMin` and the comparison is strict, matching the
    follower side exactly.** "Is there a live leader" is a fact about the cluster,
    and a leader answering it on a window of its own is how two nodes come to
    disagree about it at the same instant.
  - **This is not leader step-down, deliberately.** CheckQuorum elsewhere also
    *deposes* a leader that has lost its majority. That is a separate mechanism
    with its own safety argument, and it is not needed for the hole above: an
    isolated leader's contact ages out, so it grants and blocks nothing.
    `RaftCluster_test` and `RaftClusterHarness::Leader` still record that nothing
    here deposes a leader, which is what their assertions actually rest on.
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

## Open work

- **[#126](https://github.com/LASTRADA-Software/fastcached/issues/126)** — the
  beacon is a broadcast but the challenge and the proof are unicast to
  `received->from`, and only one of two sockets sharing a UDP port receives a
  unicast (measured on Windows 11 and Linux; which one differs between them). Two
  nodes on one host therefore see each other's beacons and silently never finish
  proving the key.
- **[#144](https://github.com/LASTRADA-Software/fastcached/issues/144)** — a
  follower answering `/fleet` names the leader but cannot link to it, because
  where a dashboard is served is local configuration and any URL it built would be
  a guess. A third recorded endpoint was priced and refused: `StateVersion` is
  checked on decode, so a field makes every existing snapshot and log entry
  undecodable. It stays open because the trigger is natural — whenever
  `ClusterState` next bumps for a reason that carries the migration on its own,
  this rides along.
