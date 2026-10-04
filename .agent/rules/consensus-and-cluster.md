# Consensus and cluster membership

Rules for `src/FastCache/Consensus/` and `src/FastCache/Cluster/`: Raft itself,
the LAN discovery beacon and the identity-key challenge after it, the Raft peer wire and the
handshake every connection on it proves each end's identity key with, the replicated
cluster configuration, and the admin verbs that change it.

Read this before touching `RaftNode`, `RaftLog`, `RaftDriver`, `RaftWire`,
`RaftPeerSession`, `RaftPeerTransport`/`RaftPeerServer`, `RaftPeerIdentity`, `RosterKeys`,
`RaftClusterHarness`, `DiscoveryService`, `PeerDirectory`,
`ClusterState`/`ClusterStateMachine` or `MembershipPolicy` — and before adding a
verb to the cluster-admin surface.

Every rule below has already been a bug.

## Discovery and the identity key

<!-- agent-tripwire: A discovery proof is a signature by the node's OWN identity key over a nonce -->

- **A discovery proof is a SIGNATURE by the node's OWN identity key, and discovery admits
  nobody (#178).** It was an HMAC under the pre-shared key until then, and a proof of
  possession of the fleet's key WAS membership, so any machine holding the file on the
  segment was desired and admitted. Now discovery broadcasts what a node *is* and the fleet
  it is in -- its fleet SUMMARY: cluster id, state, age, leader, node id, Raft endpoint, the
  ids of the machines it records (cut to `MaxFleetSummaryMembers` in a datagram, a measured
  bound, with the total beside them) and its own `0xFC` endpoint -- and the challenge that follows is answered with an Ed25519 signature over
  `(fastcache-discovery-proof-v3, challenger's cluster, nonce, summary, key)`, the key
  carried beside it. The consequences, each of which some plausible simpler design gets
  wrong:
  - **The signature is checked BEFORE the roster is asked.** Until it verifies, the
    carried key is as much a claim as the id, and naming it would be naming bytes
    anybody could have sent -- so a forgery is counted (`DiscoveryProofsRefusedForged`)
    and logged by the address it came from, never by what it claimed.
  - **Only the key the roster holds FOR THAT ID authenticates.** A verified key the
    roster does not hold is `PeerUnknownKey`: counted, and logged -- at most once per
    `UnacceptedKeyReportInterval`, with how many it stands for, because a fresh key
    costs its sender nothing -- with the id, the source, the key WHOLE (it verified, so
    naming it is no oracle) and the `--cluster-admit` that would admit it. A revoked one
    is `PeerRevokedKey`, whose remedy is the opposite, so it is counted and worded apart.
    The acceptance pair is a stranger that proves its own key and is NOT desired, beside
    the same stranger desired once the roster holds its key; a service accepting any
    verified key fails the first, one refusing everybody fails the second.
  - **Discovery states no KEY in what it desires.** It authenticates only the key the
    roster already holds, so it has nothing to add, and a desire outlives its moment
    (`ConsensusTier::Desire` replaces per id and never prunes) -- a key named there would
    be proposed straight back over an operator who re-keyed that member. And the
    authenticated set is re-asked of the roster at every publish
    (`DiscoveryTier::PublishAuthenticated`), because it is re-published whenever ANY peer
    proves itself and a proof taken before a revocation must not ride along after it.
  - **The proof signs the node's whole fleet SUMMARY (cluster id, state, age, leader, node
    id, Raft endpoint, member ids and total, and its own `0xFC` endpoint) AND the key, not the
    nonce alone.** Signing the nonce only would
    let anyone who observed one valid proof replay it with a *different* endpoint
    substituted -- pointing a known node id at an attacker's address. A member is assigned
    compile jobs and returns objects cached fleet-wide, so that is object injection into
    everybody's build. The key is inside the message so a signature cannot be
    re-attributed to another key. The summary is what formation decides on, so every field
    of it a node acts on is signed; a field outside the signature is one a relay can
    rewrite into a yield.
  - **A proof is only ever an answer to a challenge THIS node issued**, and the challenge
    is spent once a proof of it VERIFIES. An unsolicited proof is refused *even when it is
    signed by a key the roster holds*: it answers a nonce nobody here chose, and accepting
    one would make the nonce -- and therefore the replay protection -- pointless. A proof
    that does NOT verify spends nothing: it proves nothing about the challenge, and spending
    on it let anybody who saw a challenge go out destroy its honest answer.
  - **A peer that moves loses its authenticated bit.** The bit is a property of the
    node *at an endpoint*, not of the node, because that is what the proof covered.
    Carrying it across a change would admit an address nobody proved.
  - **No table holds an outstanding challenge: the nonce is a COOKIE** (`ChallengeCookies`).
    A beacon is unauthenticated, so a table of challenges is one anything on the segment grows
    by inventing ids, and every bound on such a table is spent by displacing somebody -- who
    is a real peer as soon as the flood is fast enough. It was: a bound per other cluster of
    four let four spoofed ids of a fleet's OWN cluster, one hop and no signature against the
    fleet's two hops and one, displace its challenge before it answered, every round (the
    reviewer's P5, 0 of 4 proven; kept as a case). So the nonce is
    `serial || HMAC(K_epoch; label, serial, challenger's cluster, target's cluster, node,
    endpoint, DESTINATION host)`, the proof ECHOES it, and this node recomputes it from what the
    proof claims and the host it ARRIVED from: a proof naming another cluster, node or endpoint
    than was challenged, or answering from another host than the challenge went to, answers
    nothing, refused at one HMAC before the budget and before any signature is checked.
    - **The host binds a cookie to an address its answerer OWNS** (the reviewer's V-1, P9 kept
      as a case). Without it a flood collected real cookies at its own address and answered
      them from as many sources as it could type, each a fresh per-source bucket: 128 forged
      proofs were checked, the lab's honest proof was refused unchecked, and the fleet went
      unproven. So the per-source budget bounded TYPED addresses. The assumption the binding
      rests on, stated rather than implied: **an honest peer answers from the address it was
      challenged at** -- discovery is LAN-local and answers from the socket it beaconed from.
      A peer behind anything that rewrites its source address is refused `NotIssued`, said
      once a minute with the host it came from.
    - **A node nobody configured beacons on every LINK, never on a guessed one**
      (`Cluster::DirectedBroadcastDestinations`). The limited broadcast `255.255.255.255`
      leaves by one interface the stack infers, and on a Windows host with a VPN-type adapter
      that was the adapter (measured: source `172.31.255.2`, not the LAN's `192.168.86.24/24`),
      so a default node beaconed where no LAN peer heard it. With no `--discovery` NAMED --
      provenance, `discoveryAddressExplicit`, never a comparison with the default -- beacons go
      to the directed broadcast of every up, non-loopback, non-link-local IPv4 interface that
      has one (`EligibilityOf`), re-enumerated on an INTERVAL and never per beacon; a named
      address, the limited broadcast included, is used exactly. Receiving stays on the one
      wildcard socket. A directed broadcast is ON-LINK, so it leaves with that link's address
      as its source -- which is why the honest-peer assumption above still holds. Link-local is
      passed over on purpose (an adapter with no real address), and no eligible interface at
      all is said by name with each interface's reason and the remedy, never a silent no-op.
    - **Keys are drawn per EPOCH from `ISecureRandom`**, half a challenge lifetime each, and
      the current and the previous are accepted, each until its start plus a lifetime -- so a
      challenge answers for at least half a lifetime and never more than one. The serial makes
      a cookie unique within a process and the key across processes: it never REPEATS, which
      is the property a nonce needs (`Core/Nonce.hpp`).
    - **Spent once VERIFIED, and the record has a place for EVERY cookie issued.** A record
      whose capacity was below what an epoch issued moved R-1's race rather than ending it: a
      burst of valid proofs, under a key of the attacker's own, answering cookies it collected,
      filled it before an honest proof arrived (the reviewer's P8; kept as a case, driven
      through the check budget). So an epoch issues at most `MaxEpochChallenges` and keeps two
      bits per serial -- unanswered, forged once, exhausted, spent -- and the next challenge
      draws a new key; there is no "full", and expiring an honest cookie early takes two forced
      rotations, 2^17 challenges, inside its round trip. A replay is `ProofReplayed` in the
      PREVIOUS epoch too, which a record kept only for the current one, or cleared at rotation,
      would miss. `Expired` and `Replayed` are neither counted nor logged -- an honest slow peer
      and a duplicated datagram look exactly like them.
    - **So signature CHECKS are budgeted too**, or not spending on a forgery reopens the hole
      from the other side: one beacon from a real address bought a live cookie that a stream of
      forged proofs could name, one datagram per signature check. The check budget
      (`DiscoveryWork::ProofCheck`) is asked by `ChallengeCookies::Verify` BETWEEN the cookie and
      the signature -- before it, garbage naming no challenge would spend what honest proofs
      need -- as a REQUIRED parameter, so no caller can skip it. Past it the proof is
      `ProofUnchecked`: counted (`DiscoveryProofChecksWithheld`), said once a minute by source
      address, and nothing spent, so the honest answer can still be checked.
    - **And each cookie buys at most `MaxForgeriesPerChallenge` failed checks**: past that it
      is `ProofExhausted`, refused unchecked, counted in the same series and said. Budgets bound
      work per SOURCE; this bounds it per COOKIE. Since the host binding, only the address a
      cookie was sent to can name it, so the two bounds meet at one owned address. A forger racing the honest
      answer can exhaust that one challenge -- as a spoofed proof arriving first could under any
      design -- and the next beacon round asks again.
  - **Discovery never changes membership, and answers a challenge from ANY cluster.** It
    answers which known members proved their keys and where they answer; a caller
    proposes. A membership change is a Raft decision only a leader may make, and a layer
    that proposed directly would have every node on the segment proposing the same
    change at once. A node answers every challenge, whichever cluster asked, because a
    solitary machine must see a fleet to yield to it and an established one must see a
    foreign fleet to raise `foreign-fleet-visible`. What it signs is its public summary,
    the bytes its beacon already shouts, so answering announces nothing a listener did
    not have. A proof from another cluster is judged by its signature alone (possession
    of the key it carries, over OUR nonce), handed to `IFleetObserver`, and never asked
    of the roster or desired -- this cluster's roster says nothing about another's keys.
    A beacon is unauthenticated, so EVERY table it can grow is BOUNDED, by one table of
    bounds (`Cluster/DiscoveryBounds.hpp`): the other fleets and this cluster's peers the
    roster holds no key for -- and the record of verified challenges, which only a
    signature grows. What the roster or an answered
    challenge vouches for is kept; at a bound the oldest entry nothing vouches for is
    displaced, and a new fleet is dropped only when every remembered one has proven itself.
    Every displacement and drop is COUNTED in one series (`DiscoveryBeaconsOverBound`),
    which says a flood is under way and deliberately not which table it reached.
    - **That drop is the RESIDUAL, and it sits inside trust-on-first-use.** Holding the table
      takes `MaxForeignFleets` fleets proven under fresh keys -- an answering host at a real
      address on the segment per slot, which is an on-segment attacker, the one TOFU says it
      cannot stop -- and while they keep beaconing no further fleet is seen (the reviewer's
      P7). So this drop alone of the bounds is also SAID, throttled
      (`ForeignTableFullReportInterval`), naming the source address and never the claim.
  - **No discovery reply is larger than the datagram that provoked it, and answers are
    rate-limited.** A reply goes to whatever address its request came FROM, which the
    sender typed, so a reply larger than its request is an amplifier aimed at a third
    party (`DiscoveryWire::AnswerFits`). A beacon is padded to exactly its sender's proof,
    a challenge to exactly the beacon it answers -- and not sent when it cannot fit -- and a
    proof larger than its challenge is withheld BEFORE anything is signed. Every answer is a
    signature, so answers draw on a work budget on the injected clock (`WorkBudget`,
    `DiscoveryWork::Answer`); a reply withheld for either reason is counted
    (`DiscoveryRepliesWithheld`) and never logged, since anything on the segment can provoke it. The padding is GRAMMAR -- a beacon is two fields
    and a challenge three -- so it rides the discovery wire's pending move to version 3 rather
    than taking its own.
    - **Every work budget is PER SOURCE HOST first, then shared, and the order is the fix.**
      One shared bucket let one host at a real address spend a round's burst before this
      cluster's own peer asked, every round (the reviewer's P6, 0 of 4; kept as a case). The
      host's own bucket refuses it BEFORE the shared one is asked -- asked the other way, each
      refused datagram would still take a shared token. The table is bounded
      (`MaxBudgetedSources`) and displaces the host heard from LONGEST ago, so a flood that keeps
      sending keeps its drained bucket, and a displacement only ever makes a node MORE willing to
      work. Keyed by host: a port is free to vary, and so is an ADDRESS to a spoofer, who meets
      the shared budget. Answers and checks are two rows of one table (`WorkBudgets`) over one
      class, and each case in `WorkBudget_test` walks the table.
    - **Each shared rate is sized from a MEASURED cost** -- a signing, a signature check --
      whose figures and conditions live beside `WorkBudgets` and are not restated here: each
      holds a flood to a few percent of one core.
  - **An id is BOUNDED where it ENTERS, by ONE constant** -- `MaxIdBytes`, the summary grammar's,
    named in `DiscoveryBounds.hpp` -- a cluster's and a node's alike: the summary's codec (its
    cluster, node and leader ids; a beacon, a proof, a seed's answer), a challenge's decoder, the
    formation record (by name, as damage, since no build writes one), and the option rows that
    take an id -- `--node-id`, the enrollment verbs' `<id>`, and the member grammar
    `ParseMemberSpec` that `--cluster-admit` and a leader re-reading its request share -- each by
    name. (A cluster's id is no longer typed: the formation record mints it.)
    Past it an id is no less valid to its own node: every PEER refuses the summary carrying it,
    silently. A challenge carries the challenger's id and is not sent when it cannot fit the
    beacon it answers, so an unbounded cluster id was a node that challenged NOBODY, in silence;
    `LargestUnpaddedChallenge() <= SmallestProofDatagram()` is asserted, and so are both minted
    ids inside the bound. `--cluster-forget` is deliberately NOT held to it (#159): its operand
    is the id to remove, whatever an older peer admitted.
  - **A seed's FLEET-SUMMARY answer is the same kind of signature, asked over TCP, and held
    to the same shape.** The responder answers anybody, pre-auth, ONE question per connection
    (`FrameReply::endsConnection`), signing under its own label as the FIRST field -- a
    stranger chose the nonce, so the label is all that separates it from every other
    construction under the key, and `NodeProof_test` asserts field 0 of every one. The
    prober verifies against an `IssuedNonce` only `ChallengeIssuer::IssueNonce` makes and
    `VerifyAnswer` consumes, so a recorded answer cannot meet a recorded question; and it
    reads under one deadline and the verb's reply ceiling (`wire-and-protocol.md`). A
    `NoCluster` refusal is UNSIGNED, so it is no answer, never the fact that a seed has no
    fleet.
  - **What `foreign-fleet-visible` calls foreign is `ClassifyEncounter`'s answer, asked
    afresh, never a test of two states.** Two established clusters that are one fleet split
    in two heal by the tiebreak (the owner's split-brain decision); a watch that raised on
    "both established" would call that split a foreign fleet forever. `ForeignFleetWatch`
    raises exactly while the encounter table says `ForeignFleet`, forgets a fleet it no
    longer classifies so at the next tick, and forwards EVERY proven fleet to formation
    whatever it decided. It asks the SAME evidence formation acts on
    (`ISplitEvidenceSource`), so a pair with split evidence that would otherwise be foreign is
    raised as `fleet-split-healing` instead -- naming who yields and why, or that an operator
    decides and on which machine's word -- and the two rows and the decision to heal cannot
    disagree.
  - **A split is recognised only on SPLIT EVIDENCE a key this fleet already held verifies,
    never on the other fleet's member list** (`SplitEvidenceFor`). The signature proves who
    SPOKE, not what it records, and ids ride every beacon: a fleet minted with
    `createdAt = 1` that lists this fleet's leader, believed, dissolves an established fleet
    into whoever minted it -- a one-beacon takeover. So a yield between established fleets
    needs (A) their speaker recorded in OUR state under that id WITH the key it proved, or
    (C) a machine of ours that asked THEIR fleet under the key proving it now, and that
    their list names; a member's announced memo counts only while the member is recorded.
    Evidence changes only what would otherwise be foreign, and only to the tiebreak
    (`EvidenceDecidesOnlyForeignPairs`), so a first join is decided as before. The attack
    case -- speaking as our leader, claiming a fleet id we asked, listing everybody, under a
    key nobody here holds for any of them -- is `SplitEvidence_test`'s, and neutering
    either key comparison reddens it and nothing else.
    - **A split heals BY ITSELF only on (A) spoken by a VOTER; (C), and (A) spoken by a
      learner, are an OPERATOR's decision.** A whole fleet follows a dissolve, so what moves
      one must rest on a key an operator chose to trust. (C)'s key is FIRST-USE trust: a
      solitary machine asks whichever fleet beaconed oldest, and a fleet that refused it,
      never answered, or admitted it and then dissolved itself away leaves the same memo a
      real split does -- so an outsider who mints a fleet, is asked once, and later lists the
      asker would otherwise move the established fleet the asker joined, voters included. A
      learner's key is first-use trust too, since auto-approval admits learners. Which kind
      heals by itself is the `healing` column of `SplitEvidenceNames`, read by
      `ClassifyEncounter`; the rest raise `fleet-split-healing` naming the machine the
      evidence rests on, and fail CLOSED as a foreign fleet does. The attack cases -- a fleet
      that refused the asker, one that never answered, one that admitted it and dissolved --
      are `FormationController_test`'s, and setting (C)'s row to heal automatically reddens
      them; the learner case is beside them.
  - **A split heals on ONE replicated order, never on each member's evidence.** Only the
    losing fleet's LEADER decides (`FormationController::TickMember`), and it proposes
    `CommandKind::DissolveInto` -- the survivor's id, its leader's `0xFC` endpoint, the key
    that proved it and its age -- recorded as `ClusterState::dissolveOrder`, in the snapshot
    as well as the log. Every voter and learner that APPLIES it leaves (`DissolvedInto` ->
    `LeaveForSurvivor`): one record naming the left cluster in `archivePending`, a NEWLY
    minted solitary cluster (a founder never reopens the left id over an empty directory),
    and the survivor as its join target with NO speaker, so its leader is proved before it
    is asked and the admission needs a member under the proven key. From there it is a first
    join; a member restarting after the order applied re-applies it and leaves the same.
    `RaftSplitHeal_test` drives it over the whole harness -- a voter's (A) on either side and
    the mutual case, with (C) and the attack case beside them moving nothing.
  - **A pending node is a POINTER, never a fleet to join.** Its cluster ends when the fleet
    it asked admits it, so a machine that yielded to it would be left in a cluster nobody
    runs -- and the tiebreak sends there every machine minted after it, which after a
    split's leave is every machine that starts. It announces `FleetState::Pending`, whose
    signed leader slots name the fleet it asked; a solitary node meeting one is told
    `Encounter::Follow`, asks that endpoint for its own proof and decides on THAT, one step
    and never on the pointer's word -- a key the pointer states is one the answer must be
    signed by, never one taken as proven. Neutering the row back to the tiebreak strands the
    fresh machine in the pointer cases of `FormationController_test`.
    - **No reader can take a pointer for a leader, because none is ever handed one**: the
      codec reads a `Pending` summary's leader slots into `FleetSummary::pointsAt`
      (`FleetStateTable`), and leaves `leaderId`, `leaderNodeEndpoint` and `leaderKey` EMPTY,
      which every reader already treats as no leader to ask. A rule every reader had to
      remember -- consult the state first -- would be the census this makes unnecessary.
  - **A fleet's proof binds a KEY, never the truth of its summary, so joining a fleet on a LAN is
    trust-on-first-use unless the node is PINNED -- and the pin is a KEY pin** (`--fleet-id=
    <cluster-id>@<voter-key>[,...]`, `Cluster::FleetPin`). A key costs nothing, so anybody on the
    segment can prove "established, created at 0": unpinned, every solitary node that hears it
    yields, enrolls, and on its approval adopts its roster -- launchers dispatching source to its
    workers, the node compiling its jobs. **A cluster id is a name every beacon carries, so a pin by
    name alone stops nobody who can hear the fleet it names** -- that pin shipped first and was
    replaced in the same task; an id with no key is REFUSED where typed, never read as a pin by name,
    because that would be a confident wrong signal of safety. The threat model is stated in
    `docs/getting-started/cluster-discovery.md`: a key pin stops a LAN impostor; an unpinned node
    trusts on first use.
    - **ONE predicate, `AdmitsFleet(pin, clusterId, signers)`**: the id compared WHOLE and one of the
      signers a pinned voter key. Asked of the summary yielded to (INSIDE `ClassifyEncounter`, a
      REQUIRED parameter, so beacon, seed, SRV and remembered routes are one decision --
      `Encounter::PinnedElsewhere`), of every `Enroll` answer and of the leader a redirect names
      (`ProvePollEndpoint`, whose refusal names the key to ADD -- a promoted voter is the honest
      cause), of the dissolve order (with NO signer: its keys are claimed fields of the replicated
      order, and the id decides, since a pinned node is in its pinned cluster and a survivor is
      another), and of the record (`ApplyFormation`, judged by ONE key this node PROVED: the asked
      fleet's proven key, the key that SIGNED the admission -- `FleetMembership::admittedBy`, never
      the voters the stored roster CLAIMS, which a forged approval lists as easily -- or its own).
    - **The pin anchors the JOIN; from then on the fleet's applied state is the authority.**
      After the join nothing asks the pin: Raft takes `AppendEntries` from whoever the
      configuration counts, the lease roster verifies grants against the APPLIED voters, and a
      voter promoted later is followed. Deliberate -- pinning every leader forever makes every
      promotion an outage for every pinned node -- and the limit of the protection: a capture of
      the fleet's quorum after the join gets everything, as with any member. So a promoted voter's
      key belongs in the pins of machines NOT YET joined (their join may be redirected to it) and
      in its OWN pin (or it serves no enrollment), and nowhere else is it needed.
    - **It restricts and never widens, with ONE turn the other way**: the pinned fleet, as a pinned
      voter signs it, wins a tiebreak the node is in. Without it, a pinned node whose own cluster is
      the OLDER stays, and an unpinned founder then yields to IT. For that second half a node that
      could not answer as a pinned voter serves no enrollment (`ServesEnrollment` asks the pin of its
      own cluster under its OWN key), and a joiner pointed at it is told WHICH reason
      (`EnrollmentAbsenceTable`), never *runs no consensus* from a node that runs it. Pin the
      founder to its own `fleet-id`, too.
    - **NODE-STATUS prints the pin to paste** (`fleet-id`, through the ONE formatter
      `ParsePinnedFleet` reads back), so nobody composes keys by hand. The unpinned control beside
      each pinned case -- `Encounter_test`, `FormationController_test`, `FormationFleet_test` -- is
      what shows the attack is real rather than assumed, and the impostor cases (the id copied, signed
      by another key) are what a name pin fails.
  - **A memo of a fleet that ADMITTED this node is never displaced by asks that went
    nowhere.** A fleet costs nothing to mint, so eight asks would push out the one memo an
    operator's split is told on; `RememberAsked` drops the oldest memo NOT admitted first
    (`AskedJoin::admitted`, set by the dissolve, which runs only on a signed admission). When
    eight real admissions displace one anyway the split reads as `ForeignFleet`, which is
    told to an operator too. Admitted is not TRUSTED: `CountedMemos` reads every memo, and
    none of them heals by itself.
  - **A node never announces an endpoint every peer resolves to itself.** `localhost`, a
    name under `.localhost` or a loopback address reach the DIALLER on every machine
    (`NamesOnlyThisMachine`), so a beacon naming one sends each peer to dial itself,
    confidently, with no error at either end. `AnnouncesOnlyThisMachine` is asked at the
    ONE door every announcement passes (`DiscoveryService::AnnounceableSummary`, the
    beacon and the answer to a challenge alike), which is the guarantee; the policy is
    above it -- a DEFAULTED discovery on a loopback consensus port stands down and
    answers the row not evaluated, and a TYPED one is refused by the startup table
    (`DiscoveryAnnouncesOnlyThisMachineRefusal`) and by the tier.
  - **A peer this node cannot NAME is a peer it does not remember.** `NoteBeacon`
    refuses an empty id, or an id or endpoint that is not valid UTF-8, alongside the
    own-beacon filter -- and asks it of another fleet's node too -- because what the
    directory holds is what
    is eventually proposed as a `ClusterMember` -- and every surface reads that back
    out as text (#159). Filtered here rather than at any later layer, which is what
    keeps a permanently-refusable proposal from ever being generated; it also keeps
    such a peer out of the challenge table, out of `Peers()`, and out of the line
    logged when a peer proves its key. An EMPTY endpoint is nameable: it is a learner,
    which dials in and answers nothing, and `DiscoveryTier::PublishAuthenticated` is
    what leaves it out of a desire, since there is nowhere to dial. The beacon's
    *cluster id* is deliberately exempt: it is compared, and another fleet's is kept as a
    key of the bounded foreign table -- filtering it would take every peer away from a
    fleet named in some other encoding, silently. **It IS rendered**, in
    `foreign-fleet-visible`'s detail, which reaches `--node-status`, the fleet page and the
    live-stats rows; it is text there because `NodeConditions` escapes every detail's
    bytes that are not UTF-8 as `\xNN` and clamps its length, which is what a watch case
    with a non-UTF-8 cluster id pins.
  - **A claim a peer has not proved is printed only once it is TEXT, and never
    unthrottled.** The mismatch line for a proof does name the endpoint it claimed --
    that is a real diagnostic, because the ordinary cause is a peer that moved -- so
    what is refused first is a claim that is not text, before the line that would
    have carried it. An unnameable beacon has no such diagnostic to offer and is
    reported by the address it came *from* instead. Both are provokable by anything
    on the segment holding no key, which is why the beacon line is rate-limited and
    why neither may ever grow a table. So is every REJECTED PROOF's line -- a forgery,
    an answer from another endpoint than the one challenged, a key the roster does not
    accept -- each through its own `ThrottledReport` at `RejectedProofReportInterval`,
    so one kind never hides another and each line says how many it stands for.
- **A node listens where the segment shouts and answers from an address of its
  own. Sharing a port buys hearing a broadcast and nothing else.** Every node binds
  the beacon port on the wildcard, shared, because a beacon is a broadcast and a
  node listening anywhere else would send perfectly and hear nothing. But two
  sockets on one UDP port both receive a broadcast and only **one** receives a
  unicast -- measured, Windows 11 hands it to the first-bound socket and Linux to
  the last -- and the challenge and the proof are both unicast to `received->from`.
  A node that answered out of the shared socket would be answering for its
  *machine*, so two nodes on one host saw each other's beacons and silently never
  finished the handshake (#126). It holds two sockets now: the shared listener,
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
  mistake. What it buys is that two fleets on one segment never become each other's
  PEERS: another fleet's node is kept apart, in a bounded table, its proof judged by
  signature alone and handed to formation rather than to the roster -- which holds
  even when somebody shares a key across fleets, which they should not.
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
- **Every signature carries a domain LABEL, signed as its first FIELD -- and the pre-shared
  key's one seam went with the key (#178 PR 6).** From #402 until then
  `Cluster/ClusterSigning.hpp` was the only caller of the HMAC primitive under the pre-shared
  key: a signer was a `SigningDomainTable` row or it did not sign, `VerifyFields` was the one
  comparison, and `ctest -R psk-signing-seam` refused a signer calling `HmacSha256` itself.
  Discovery and the Raft peer wire left it for identity keys at #178, the lease at PR 5 and
  the node proof at PR 6, and the seam, the key file, `ClusterKeySource` and the check were
  DELETED -- removed, never shimmed. What the seam existed to teach still holds of every
  Ed25519 construction that replaced it, and each is what some plausible simpler design gets
  wrong:
  - **Arity is not domain separation.** Before #402 the lease carried `fastcache-lease-v1`
    and the discovery proof carried nothing, and no byte string was valid under both only
    because a four-field encoding began with a cluster id and a two-field one with a literal
    -- a coincidence that lasts until somebody adds a field or a signer. **The safety was a
    property of the PAIR, not of either construction.** It matters MORE with identity keys,
    not less: one node key now signs its discovery proof, its Raft proof and verdict, its
    challenge replies and -- on a scheduler -- every lease, so a label
    is the only thing keeping one of those from verifying as another. So the labels are ONE
    table again, `Core/IdentityKeyLabel.hpp`'s `IdentityKeyLabels`, keyed by
    `IdentityKeyPurpose`: present, distinct and never retired is a `static_assert` over it
    (`IdentityKeyLabelsSeparate`), and `NodeProof_test` walks a builder row per purpose,
    keyed on the same enum so a construction added without one fails the BUILD.
  - **A label is first BY TYPE, not by each builder remembering to put it there.** Every
    identity-key signing seam takes a `LabelledMessage` -- `SignLabelled`,
    `ILeaseSigner::Sign`, `IRaftPeerKeys::SignAsSelf` -- and
    the only way to make one is `LabelledMessage::Of(purpose, fields)`, which writes the
    label itself. A protocol that signs twice keeps an enum of its own mapped onto a
    purpose (`RaftPeerSignatureLabels`, `NodeProofSignatureLabels`, each
    `EachRowItsOwnConstruction`), so its interface cannot be handed another protocol's.
    The one door the type cannot close is `Ed25519KeyPair::Sign`, which takes raw bytes:
    `IdentityKeyLabel_test` counts every `.Sign(` / `->Sign(` under `src/FastCache` and
    `src/apps` against a row per file, `SignLabelled`'s the only raw one, and FAILS OPEN
    for a signature spelled any other way (a member pointer, `std::invoke`).
  - **The label is a FIELD, not a prefix glued onto the first one.** It goes through the same
    length-prefixed grammar as everything after it, so no choice of first field can shift
    bytes across the boundary and spell a different domain's label.
  - **A retired label is never reused**, so a MAC input from the pre-shared era and a signed
    message can never be the same bytes (`RetiredIdentityKeyLabels`, one list for every
    construction, which the same `static_assert` holds the live table apart from).
  - **WHEN a signature is checked relative to every other claim belongs to each verifier**,
    and no seam could ever hold it: the signature before the roster is consulted, and before
    any claim is reported on.
- **Giving the proof a label changed the proof wire, and the datagram version did
  not move.** Every tag differed from a pre-#402 build's; `CurrentVersion` stayed at
  1 because what changed was the MAC *input* and not the grammar, and bumping it
  would have misdescribed the format. **#178 then moved it to 2**, and for the opposite
  reason: a key and a 64-byte signature where a 32-byte MAC was change the proof's
  arity and widths, which is grammar, and `MinimumVersion` moved with it because a
  version-1 proof is a MAC nothing here can verify. It is also the better failure of the two: an older
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

## The Raft peer wire

<!-- agent-tripwire: Every Raft peer connection proves each end's OWN identity key before a message is read -->

Until #1308 a Raft connection authenticated nothing. `RaftPeerServer` delivered whatever
decoded, and every message names its own sender (`candidateId`, `voterId`, `leaderId`,
`followerId`) in a field nothing tied to the connection it arrived on -- so anything that
could reach a `--listen-raft` port could vote, depose a leader or replicate a log under
any member's name, and discovery's proof bought admission to a cluster whose wire then
trusted every address. #1308 made every connection prove the pre-shared key before a
message is read. #178 (`RaftWire` 4) made it prove each end's OWN identity key instead,
because a pre-shared key proves "holds the key" and never WHICH holder: any holder could
claim any member's id, and removing one machine meant rotating the key on every other.
Every frame after the handshake stays bound to it. Each rule below is what some plausible
simpler design gets wrong.

- **The shape, because every other rule refers to it.** A voter's connections are one-way: the
  transport only writes, the server only reads, and a reply travels on the other node's
  own outbound connection. The one exception is a learner's, which the acceptor writes on too
  (*A leader reaches a learner over the session the learner dials*, under Learners -- not yet
  *never dials a learner*: the node still dials one whose record carries an address). So
  authentication is a short two-way prologue on an otherwise one-way stream
  (`Consensus/RaftPeerSession.hpp`):
  1. the ACCEPTOR sends a `Challenge` carrying its nonce and an ephemeral X25519 key,
     before it has read a byte;
  2. the DIALLER answers a `Proof`: its id, the id it believes it dialled, its own nonce and
     ephemeral key, and an Ed25519 signature under its OWN identity key over all of that and
     the challenge;
  3. the acceptor verifies the signature under the key its ROSTER holds for the claimed id,
     then the claims, and answers a `Verdict` signed with its own key over the whole
     transcript, the proof's signature, the verdict and its own id;
  4. the dialler verifies the verdict under the key its roster holds for the id that
     answered before it sends a single Raft frame. Both ends derive the session key --
     HKDF-SHA256 over the X25519 output, salted with both nonces and bound to both ephemeral
     keys and both ids -- and every frame after that carries a 32-byte HMAC tag under it,
     over an implicit sequence number and the frame (`Core/SessionSeal.hpp`).

- **Each signature covers the WHOLE transcript so far.** A field left out is a field whatever
  sits on the path can change while the signature still verifies -- and the ephemeral keys
  are the ones that matter most: swapped for its own, a man in the middle agrees a session key
  with each end. `RaftPeerSession_test` changes each of the proof's seven transcript fields in
  transit (the session direction among them) and requires each change to be refused, so
  dropping any one of them from what is signed turns that field's section red. The verdict
  carries the proof's signature, so the dialler's fresh values reach it three ways at once;
  the field a single-field test can see there is the verdict byte, and a signed refusal
  flipped to `Accepted` is its case.

- **A two-way session is the same proof, and then BOTH ends read.** The dialler states the
  session's DIRECTION in its proof and the signature covers it: a relay that flipped `OneWay` to
  `TwoWay` would make an acceptor write to a peer that never asked to be written to. A two-way
  session has a key per direction (`SessionKeys`, derived under two labels from one agreement) and
  one sequence number per direction, so a frame reflected back at its sender does not verify.
  `StillProves` is pulled on every frame in BOTH directions -- the acceptor's sender and reader, the
  dialler's writer and reader, the readers being one loop (`ReadProvenSession`) -- so a forget
  closes a learner's session at its next frame whichever end speaks, and a signed `OwnKeyRevoked`
  on the redial is handed to `RaftPeerTransport::ObserveOwnKeyRevoked`, which is how a learner
  offline through its forget learns of it.
  - **The dialler's handshake `ByteReader` IS the session's reader.** The acceptor's first frame
    can arrive in the same read as its verdict, and a reader dropped at the verdict takes that
    frame with it, silently; `RaftPeerTransport_test` asserts the ONE-read arrangement, not only
    the delivery, and building the handshake's reader locally again turns it red.
  - **The two halves share one socket and end together.** A reader that ends closes the socket and
    pushes a ZERO-LENGTH frame the writer reads as "ended" (no real frame is empty; one left
    behind by an earlier session is stepped over, since that session's reader is alive); a writer
    that ends closes the socket, which completes the parked read. The session returns only once
    both have (`whenAll`), so neither outlives the socket -- never a `co_await` on a started
    `Task`, which RESUMES a coroutine still parked in a read. The reader's ending is RAII
    (`ReaderEnding`), because a reader that threw and skipped it would leave `whenAll` waiting
    on a writer nothing wakes; and a writer asks whether the reader has ended BEFORE it judges a
    frame, so a `Send` queued in the turn a withdrawn key ended the session is dropped rather
    than counted as the same withdrawal a second time.
  - **A learner's redial grows; a voter's does not** (`DialBackoffTable`, keyed by direction): 1 s
    doubling to 30 s, back to 1 s after a session that carried a frame each way, because a laptop
    offline for hours should not dial every voter four times a second; a voter's row is flat at
    250 ms, because a partition heals on the next heartbeat.
  - **A two-way dialler's READER has an idle bound; a one-way dialler's has none**
    (`SessionIdleTable`, keyed by direction, `LearnerSessionSilentHeartbeats` leader heartbeats
    measured from the last frame read). The learner is the PASSIVE end of its session: a laptop
    that wakes on a session whose leader end was aborted during the sleep is never dialled and is
    sent nothing (`NoSession`), so an unbounded read parks forever and the roster, every forget
    and `shared-cache` stop arriving until a restart (W-3). The bound is a `DeadlineTimer` re-armed
    per frame, never a sleep beside the reader and never a power event; it ends the session as
    `SessionEnd::Silent`, counted `raft_peer_dials_ended_silent` at Debug, and the sender redials on
    the backoff above. A voter that is not leading writes a learner nothing, so those sessions end at
    the bound too: one handshake per voter per backoff cycle, the stated price.

- **The acceptor goes first because its port is the surface anybody can reach.** It signs
  nothing until the other end has proved an id, so a stranger who connects learns a nonce
  and an ephemeral public key and nothing else. The dialler signs only for an address it
  CHOSE to dial. Reversing the order makes every listener a signing oracle for whatever
  connects to it.

- **The signature is checked before any claim in the proof is reported on, and then `OwnId`
  before `WrongTarget`.** Discovery's and the lease's rule, applied a third time: a named
  refusal decided before the signature is an oracle that tells a stranger which ids exist
  here. **One claim is read first, and has to be**: the claimed id SELECTS the key to verify
  under. So an id the roster holds no key for is `UnknownKey` and a signature that does not
  verify under the id's key is `Proof`, and BOTH are answered with nothing and logged with the
  address alone -- an id nobody proved is not one worth printing. `OwnId` first because a
  dialler proving THIS node's id holds this node's private key, which is a copied
  `--cluster-dir`, and that is the finding whatever it dialled.

- **The verdict is SIGNED, and so are the refusals** (A1). A dialler refused `WrongTarget`
  or `OwnId` by a close would see exactly what an unknown key produces, and report a key
  problem to an operator whose keys are right: **a confident wrong signal is worse than a
  vague right one.** A verdict exists only once the proof's signature verified, so signing a
  refusal tells a stranger nothing. Four consequences:
  - `dials_refused_wrong_target` and `dials_refused_own_id` are the dialler's own rows,
    and the log line names the member that actually answered at that address;
  - an `Accepted` verdict naming an acceptor other than the one dialled is refused as
    `WrongTarget`, because a signature proves a key and not the member meant;
  - **a signature that verifies under a key the roster has REVOKED is answered with a signed
    `KeyRevoked`**, so the removed machine reports its OWN removal
    (`dials_refused_own_key_revoked`) rather than a key problem at the acceptor. No oracle:
    only the holder of that key can produce the signature, and all it learns is that its own
    key is revoked. The check runs against EVERY revoked key, never only the ones revoked under
    the claimed id (`RosterKeys::KeysOf`). A revocation's id is a LABEL (`RevokedKey`), so
    narrowing to it reports the removed machine as a key nobody gave, or as a forgery, whenever
    the label and the claim differ. The list decides a diagnosis and never an acceptance, and a
    stranger's proof costs one check per key an operator ever revoked;
  - `dials_ended_by_acceptor` now means ONLY a close after the proof with no signed verdict
    -- the acceptor holds no key for this node's id, or another one, or refused the proof's
    shape. Its description says so, and a change that sends a refusal unsigned moves a count
    into the wrong row.

- **Fresh values from BOTH ends in every signature, and a nonce must never repeat -- it need
  not be unpredictable.** The acceptor's makes a proof unreplayable; the dialler's makes a
  verdict fresh, so a recorded `Accepted` cannot answer a dialler talking to something else.
  The EPHEMERAL SECRETS must be unpredictable, and are: they are what makes the session key
  one only the two ends hold. **Size and source are one seam** (A2): `Core/Nonce.hpp` holds
  `NonceBytes` (32, `static_assert`ed at least that) and `DrawNonce`, and the node proof's and a
  fleet probe's nonces draw through the same helper -- a second draw site would be a second
  answer to how big a nonce is. Discovery's challenge is `NonceBytes` wide too, and draws no
  nonce: it is a cookie under an epoch key drawn from the same `ISecureRandom`.

- **Never-repeat needs an ENTROPY SOURCE, not a seeded engine** (#1527). This bullet used to
  conclude that `SystemRandomSource` was good enough, and the conclusion rested on a premise
  nobody wrote down: that its seed was random. It is seeded from `std::random_device`, which on
  the host #1507 was measured on answers zero for 57% of draws, so about a third of processes
  there (0.57 squared -- inferred, not reproduced) seed zero and draw ONE nonce stream: a
  restarted acceptor re-issues its previous run's challenges. So nonces, ephemeral secrets and
  minted node ids come from `Core/ISecureRandom.hpp` -- `getrandom` on Linux, `getentropy` on
  macOS, `BCryptGenRandom` on Windows -- and `SystemRandomSource` keeps jitter and
  tie-breaking.
  - **A draw that fails is a REFUSAL, never a fallback** to the device, a clock or an engine.
    The handshakes are FACTORIES (`AcceptorHandshake::Create`, `DiallerHandshake::Create`), and
    each draws TWICE -- the nonce, then the ephemeral secret -- so a handshake with either
    weak cannot be constructed: the acceptor closes unchallenged, the dialler abandons before
    proving, discovery withholds its challenge while it cannot draw an epoch key
    (`ChallengeWithheld`, keeping the epochs it holds), the node-proof surface
    refuses `NoCluster`, and a mint refuses to start the node by name. The second draw has a
    case of its own (`ScriptedSecureRandom::DenyAfter`), because a factory that checked only
    the first would pass every nonce case.
  - **Each of those is uncounted, and says so in an Error naming the primitive**: every refusal
    row on these surfaces describes a PEER, and this describes THIS host. The transport's
    precedent is its "cannot prove this node's id" line; the acceptor and discovery throttle
    theirs, because anything on the network can provoke them.
  - **Its test is CROSS-PROCESS, because the defect was.** An engine seeded once per process
    never repeats within it, so every in-process "two nonces differ" case passed on the broken
    build -- measured: with `DrawNonce` neutered back to a zero-seeded engine, that case stays
    green and `ctest -R secure-random-cross-process` goes red. That gate runs the pre-#1527
    construction first and requires its repeat detector to FIRE, before believing its silence.

- **The ids are bound; the endpoint deliberately is NOT**, although discovery's proof binds
  a `(node, endpoint)` pair. Discovery produces an ADDRESS somebody records, so the address
  is what must be proved. This produces "the frames on this connection come from `d`, for
  `a`", and neither end can state the endpoint identically -- an acceptor binds the
  wildcard, a dialler reaches it through `--raft-self` or NAT. A relay at another address
  can only forward frames it cannot seal, which a network path already can. Binding it
  would refuse the NAT'd member and stop nothing.

- **A signature proves WHICH member, which is what the pre-shared key could not -- and the
  claim that it could be swapped in "without a wire change" was WRONG.** Under #1308 any key
  holder could claim any member id. `IRaftPeerCredential.hpp` argued that a per-node
  credential could replace the shared one without the wire moving, because every field it
  MACed already named its ids, and this rulebook repeated it. A signature is 64 bytes where a
  MAC was 32, agreeing a session key needs both ends' ephemeral keys on the wire, and a
  verifier needs the SIGNER's public key rather than one shared secret -- every handshake
  frame changed shape. **A claim that a replacement needs no wire change is a claim about the
  replacement's construction, made before there was one.** Confidentiality is still a
  non-goal: log entries stay in cleartext.

- **Every session frame is tagged, the sequence number never travels, and a verified
  message naming another sender closes the connection.**
  - The tag is an HMAC under the SESSION key over `[seq, header, payload]`. `seq` counts the
    connection's frames from zero on both ends, so a frame replayed, reordered or dropped
    fails the NEXT tag; a frame spliced in from another connection fails because that
    connection agreed another key; an on-path injection fails outright. One key per
    direction, which a Raft connection is.
  - Header and payload are two FIELDS, because the server reads them apart and tags what it
    read without first copying them together. The opener advances only on success.
  - The tag sits OUTSIDE `payloadLength`, so `WireFrame` keeps its meaning and
    `CompileCacheWire` is untouched.
  - The proven dialler id is what `RaftNode` reads: `SenderOf(message)` naming anybody
    else is `frames_refused_sender`, and only the proven member can produce one, so a rise
    is a defect in a member rather than an attacker.
  - An unknown message type is still stepped over -- after its tag verifies, and it
    consumes a `seq`. Stepping over an UNVERIFIED frame would be a free injection channel.

- **A revoked key ends the sessions it proved, at their next frame -- or, for an ATTACHED session a
  peer dialled in on, at the next reconcile pass -- PULLED, not pushed.**
  Both ends re-ask the roster (`IRaftPeerIdentity::StillProves`) for every frame: the acceptor
  after the tag verifies, the dialler before it seals. So an applied forget -- or a
  re-admission under another key -- closes the session at its next frame
  (`connections_ended_key_withdrawn`, `dials_ended_key_withdrawn`), and the redial is judged
  against the roster as it is then. A push from the state machine would be a close from the
  apply thread on a connection the reactor owns; a Raft peer is never quiet for longer than a
  heartbeat, so the pull costs no latency worth a thread hazard. Over TCP a dialler whose
  acceptor closed learns so from its next write's reset; the in-memory socket accepts writes
  nobody reads, so the link and node cases READDRESS to force the redial and say why.
  - **Asked per frame AND per reconcile pass for ATTACHED inbound sessions, still PULLED; a dialler
    learns at its next write.** A session nothing is SENT on has no next frame, and that is exactly
    a forgotten LEARNER's: the configuration drops it, the leader sends it nothing, and a learner
    never campaigns, so it never dials again to hear the signed `OwnKeyRevoked`. So the pass calls `RaftPeerTransport::RecheckProofs` right after
    `AdoptConfiguration`, which pushes a zero-length WAKE onto the outbox of every ATTACHED session
    whose key no longer proves. Still a pull: the wake carries no verdict, and the reactor's own
    sender asks the roster again and decides.
  - **The wake is internal.** Both senders decide what they popped through ONE step
    (`PeerSenderAccess::StepFor`): the roster first, whatever was popped, and only a MESSAGE
    reaches the seal. A sealed wake would spend a sequence number the peer never sees, and the
    next real frame's tag would fail there -- an idle session closed by asking whether it may stay
    open. A wake is not a message, so a withdrawal it causes counts no drop.
  - **Sessions this node DIALLED are not woken, deliberately.** A forgotten member learns only
    from the verdict on a dial of ITS OWN, so closing one this node dialled teaches the acceptor
    nothing, and the sender redials after its backoff with or without a message: one idle socket
    would become a refused handshake every backoff for as long as the process runs. A forgotten
    VOTER's own one-way dial closes at its next frame, which its election timer sends because
    nobody heartbeats it once the configuration drops it; a voter that applied its own removal
    has no election timer and already knows. No idle TIMEOUT either: the wake asks the one
    question a timeout would only approximate. `ConsensusTier_test` ("A forgotten learner hears its
    key revoked...") through the real tier, `RaftPeerLink_test`'s recheck cases at the link.

- **The roster is the one the node started from until the cluster says anything, and then
  the cluster.** `Cluster::RosterKeys` answers from the bootstrap members' keys -- the roster is
  the one the node's own cluster applied (itself alone while solitary)
  or the approved enrollment roster (a learner), until the replicated state says otherwise -- then from
  `ClusterState`, which WINS wherever it states a key: a member re-admitted under a new key
  proves itself with the new one whatever the bootstrap roster named. **A bootstrap key the
  state has revoked is revoked, whatever the bootstrap roster says** -- a restart from the
  same record must not bring it back, which would be removal failing open. A member the state
  records with no key falls back to its bootstrap key only when that key is not revoked. A
  principal is a stranger here: it never joins consensus. So a member admitted with no key
  cannot be verified, and says so in `connections_refused_unknown_key` rather than trusting
  whoever answers first; `--print-identity` is how an operator gets a member's
  `--cluster-admit` line before it is admitted, minting into the state directory the start
  then reads. (Until the formation record, the bootstrap roster was `--raft-peer`'s
  `@<key>`, typed on every member's command line.)

- **The version is a property of the CONNECTION, and it moves when the GRAMMAR does** -- to 2
  for #1308's handshake and trailer, to 3 for #1449, whose `InstallSnapshot` carries a
  configuration of two sets where it carried one list (see *Learners* below), and to 4 for
  #178, whose challenge, proof and verdict carry ephemeral keys and signatures. The opposite of
  #402, where only discovery's MAC input changed and `DiscoveryWire::CurrentVersion` rightly
  stayed -- and the same as #178's discovery change, where a key and a signature replaced the
  MAC and that version moved to 2: the question is always WHICH changed. `MinSupportedVersion` moved with it every
  time: a version 1 peer authenticates nothing, a version 3 peer proves the pre-shared key,
  and accepting either is the per-connection fallback these tickets refuse -- so a fleet
  upgrades its consensus members together. A session frame whose version byte differs from
  the handshake's closes the connection, because stepping over it would mean guessing whether
  a trailer follows. `RaftWire.hpp`'s old "Why there is no handshake" section argued against a
  VERSION negotiation per reconnect, soundly; it is now "Why there is a handshake", because
  freshness is the one thing a frame cannot prove about itself.

- **Pre-authentication reachability is a COLUMN of `MessageTable`.** Each row's
  `FramePhase` is `Handshake(ceiling)` or `Session()`, and `FramePhase` has a deleted default
  constructor, so a new row cannot omit the answer. The acceptor reads the header, refuses
  a type other than `Proof` or a length over that row's ceiling BEFORE buffering the
  payload, and every handshake ceiling is `static_assert`ed within `MaxHandshakePayload`
  (4096). The `0xFC` wire's rule, one protocol over: an unbounded pre-auth read is a
  memory-exhaustion hole reachable by anybody.

- **One handshake bound, at both ends** (`RaftWire::HandshakeBound`, 5 s), armed through
  `ArmSocketDeadline`. Before it, a connection that sent nothing held one of the
  listener's 64 slots for as long as its socket lived, so a stranger could fill them all
  with no timeout at all; now it is closed and counted. The dialler abandons an acceptor
  that never challenges -- which is what a build from before the handshake looks like --
  and the ordinary backoff applies. A non-positive bound arms nothing, which is the seam
  tests over an unturned reactor use, not a production setting.

- **Whether the wire authenticates is a STARTUP decision, never a per-connection
  fallback.** `ConsensusTier::Start` refuses a start with no identity key
  (`ConsensusNeedsIdentityKeyRefusal`) before anything is bound or dialled -- and no
  configuration reaches that refusal, because EVERY node has a state directory: `--cluster-dir`,
  or the platform default the start resolves (`NodeStateDirectory`, a precondition that throws
  rather than answer a relative path). The key is resolved by the START and
  PASSED to the tier, never read twice, so the key a node announces and the key it proves
  itself with are one reading of one file. The server and transport take the identity and an
  `ISecureRandom` as REQUIRED constructor arguments -- no default and no null -- and their own
  id IS the identity's, never a parameter beside it that could name somebody the proofs do
  not. `ConsensusNeedsClusterKeyRefusal` went at #178 PR 6 with the key file it required,
  once the node port -- the last surface reading that file -- proved identity keys too. Its
  question moved one flag over and then went: `SchedulerNeedsIdentityRefusal` refused a node
  that named a scheduler and held no identity key, until the zero-config defaults gave every
  node a state directory and so a key. "No key, so skip the check" is the shape the worker's
  lease rule (#282) refuses one surface over: the port open, every refusal counter at zero,
  and the fleet healthy-looking from both ends.

- **Layering decides where the seam sits.** `Cluster/` already includes `Consensus/`
  headers, so `Consensus/` cannot read `ClusterState`. It states what it needs instead:
  `Consensus::IRaftPeerKeys` -- this node's own key, which signs and never leaves it, and the
  roster's keys for a peer id, NOW -- and `Consensus::IRaftPeerIdentity` (`Self`, `Sign`,
  `Verify`, `StillProves`) over it, whose one implementation is `RaftPeerIdentity`.
  `Cluster::RosterKeys` implements the keys over `ClusterState` and the command line. The two
  signatures are two constructions, mapped by `RaftPeerSignatureLabels` onto
  `IdentityKeyLabels`' `fastcache-raft-proof-v3` and `-verdict-v3` and `static_assert`ed
  distinct, so a proof reflected back as a verdict verifies as nothing; the session keys'
  HKDF label is `fastcache-raft-session-v3`.
  The pre-shared key's three Raft rows left `SigningDomainTable` (itself deleted at #178 PR 6),
  and their `-v1` labels are retired rather than reused; the `-v2` labels signed a transcript
  with no session direction and are retired too (`RetiredIdentityKeyLabels`, `static_assert`ed
  against every live label by `IdentityKeyLabelsSeparate`).
  - **The proof signs a SESSION DIRECTION** (`RaftWire::SessionDirection`, the proof's third
    field, `OneWay = 0`, `TwoWay = 1`), inside both transcripts, so a relay cannot make an
    acceptor write on a connection its dialler only writes on. A flipped byte is `Forged`.
  - **One key per direction** (`SessionKeys`): one HKDF call per way, each naming its way and
    the direction. Both ends count positions from zero, so under ONE key a frame reflected back
    at its own sender would open there; a one-way connection uses `diallerToAcceptor` only.
    Frame sealing is `Core/SessionSeal`, not Raft's, because the `0xFC` wire seals with it too
    (`SealedFrameSocket`, #178 PR 6). The session code never compares a tag itself.

- **One refusal, one row -- at BOTH ends, because one misconfigured machine shows on two.**
  Every refusal is a row of `Consensus/RaftPeerRefusals.hpp` beside its log sentence, and the
  key refusals are apart by REMEDY: `unknown_key` (a key never given), `proof` (somebody
  signing for an id whose key it does not hold), `revoked_key` (the removed machine, still
  dialling), `ended_key_withdrawn` (a session the roster outlived), and at the dialler
  `acceptor_key_unknown`, `acceptor_key_revoked`, `own_key_revoked` and `ended_key_withdrawn`.
  **Each end counts the direction it READS**, and both session-end tables count every ending
  but a close: an acceptor never reads what it writes to a learner, so a frame the dialler refuses
  on a two-way session -- `dials_ended_frame_tag`, `_sender`, `_unreadable`, `_over_cap`,
  `_bad_magic` -- is counted by the dialler or by nobody, and the acceptor's `frames_refused_*`
  rows are the same five for its own direction. A bad magic or an over-cap frame is refused
  before any tag verifies, so its row says what was OBSERVED, never who sent it.
  A peer that sent NOTHING and closed asked nothing, so it is closed and NOT counted. Refusals
  before authentication are logged at most once per 60 s and name only the source address,
  except a revoked key, which names the WHOLE key, since that is what an operator matches
  against the revocation they made; refusals after authentication name the proven ids,
  unthrottled; a dialler's refusals are throttled per peer.

- **What became false was removed, not kept** (the project's position on superseded
  code). `EnrollmentConfigured`'s key clause could no longer decide anything, so the
  predicate is gone and `ServesEnrollment` asks `RunsConsensus`; `EnrollmentResponder`
  read the key before `ClusterAdmit` until #178 PR 4 stopped handing one over.
  `NodeMembership`'s refusal to let a replicated `fleet-open` WIDEN a keyless node is gone
  too: cluster state reaches only a consensus node, and every consensus node holds the roster
  its lease check verifies against. The RELOAD guard in `ValidateNodeReloadable` stays,
  because a node that runs no consensus can still show no roster in its configuration and be
  opened by its operator.
  And at #178 `PskRaftPeerCredential` went with its rows, rather than staying as a second way
  to authenticate this wire.

- **`RaftClusterHarness` authenticates EVERY message**, through the same session objects
  the server and transport drive, and delivers what the receiver DECODED rather than what
  was sent. Five decisions, each with a way to get it wrong:
  - **One session per message**, not per link: the harness has no connections, and a
    session that outlived a partition or a restart would be a property of the harness, not
    of the wire. The loss, reorder and duplication adversary stays intact; `seq` is the
    unit cases' subject.
  - **Nonces and ephemeral keys come from a source of their own**, never `_network`, so every
    existing seed sees a byte-identical delay and loss schedule. That source is the operating
    system's generator (#1527), as in production, and a run stays a function of its seeds:
    a nonce's VALUE decides nothing, since every signature over it verifies under the signer's
    key alone.
  - **The identity factory is REQUIRED at construction.** A defaulted one would let a case
    forget who its members are and still read as a cluster that forms.
  - **A machine's NAME on the network and the id it proves are two things**: messages route by
    name, and the node inside runs as its identity's id. That is what lets `Intrude` put n3's
    twin on the network claiming n2 -- a network keyed by one of them could not express it --
    while messages addressed to n2 still reach n2.
  - **An intruder case is read beside the formation case, under the same neuter.** With
    `RaftPeerIdentity::Verify` answering `Verified`, the intruder sections -- a key the roster
    lacks, a member's id claimed with another member's key, a revoked key -- are the cases
    that must go red, and the formation cases must stay green; a harness where both go red is
    measuring the neuter, not the wire.

## A refusal code carries its own permanence, and there are THREE answers

<!-- agent-tripwire: A refusal code carries its own PERMANENCE, and there are THREE answers -->

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

<!-- agent-tripwire: A replicated setting must not decide where a node sends a CREDENTIAL -->

- **A cluster setting that nothing can change at runtime is a log entry pretending to
  be configuration.** The replicated log carried settings, applied them, snapshotted
  them and replicated them — and no surface anywhere said "set `upstream` to this", so
  the only way to configure a fleet was still `--upstream` on every machine, which is
  the file-editing the log exists to replace. `Op::ClusterStatus` / `ClusterSet` /
  `ClusterForget` on the scheduler's port close that, and four things about their
  shape are load-bearing. (**`upstream` is the wrong example now** and is left standing
  as the history it is: the verbs were built for a table that held it, and #1123 then
  took that row out for the reason in the next bullet. The argument is unchanged for
  the rows that remain.)
  - **They go through the same `Gate()` as the dispatch verbs, the READ included.** A
    follower's copy of the state is valid and merely older, so `ClusterStatus` could
    have been answered anywhere; one rule for the whole surface is what makes "a verb
    added without the gate" impossible, and it sends an operator to the node they
    would need anyway to change anything. The refusal for a non-member is not about
    capacity here: a stranger who could set `fleet-open` would admit every caller on
    the network to the fleet. That named `upstream` until #1123, which closed that one
    at the TABLE rather than at the gate — the gate is still what stands between a
    stranger and the rows that remain.
  - **`NoCluster` is distinct from `NotLeader`, because the operator does something
    different.** `NotLeader` names somewhere else to ask; `NoCluster` says the
    question does not apply here at all — a node started without `--listen-raft` runs
    no scheduler (a scheduler is a cluster of one since #178) and has no replicated
    state. Answering the second with the first
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

- **A replicated setting must not decide where a node sends a CREDENTIAL.** `upstream`
  was such a row and is [#1123](https://github.com/LASTRADA-Software/fastcached/issues/1123).
  It looked inert beside #1112's `fleet-open`, which decides ADMISSION: replicating an
  address reads as telling every member where the shared cache moved to. What that
  misses is that a node does not merely dial it — `CacheTier.cpp:227`/`:228` construct
  the `RemoteUpstream` from `cfg.upstream` AND this node's `ICredentialSource`, and
  `RemoteUpstream.cpp:135`/`:175` present `_credential.Current()` on every `CacheFetch`
  and every `CacheStore`. The address and the secret are then governed by different
  mechanisms — `--requirepass` is per machine and reloadable one node at a time, a
  setting is committed by a majority — so wiring the row would have let ONE committed
  entry redirect every member's `--requirepass` to an address of the committer's
  choosing, each node presenting it on its next fetch. Nothing read the row, exactly as
  nothing read `fleet-open` before #1112, so the two tickets are the two answers to one
  shape and **what the value would DECIDE is what picks between them** — never whether
  the key looks harmless. And it is refused **BY NAME**: a key this build deliberately
  stopped replicating and a key nobody ever heard of both come out of `FindSetting` as a
  null pointer, and *no such cluster setting* reads as a typo or as a node too old, which
  sends an operator to upgrade a machine over a decision. `RefusedSettingTable` is the
  row that lets the answer say why and name `--upstream`; a `static_assert` refuses a key
  that is in both tables, so the refusal cannot be shadowed by a row arriving later.

  **A replicated address is safe only under TWO conditions, and each one alone fails.**
  Nothing the node sends there may be replayable, AND the peer must prove, before anything is
  sent, an identity the roster holds in the role the leg is for. `upstream` fails the first:
  a password in a `CacheFetch` is the same bytes at whichever endpoint captured it. The first
  alone still hands every node's objects to whoever a committed entry names. The second alone
  presents a replayable secret to a machine that is a member today and forgotten tomorrow. So
  a node's `--requirepass` goes to ONE place, the `fastcached` its own `--upstream` names. No
  scheduler-facing round presents it, so no replicated, redirected or announced scheduler
  endpoint can collect it.

  **`shared-cache` complies with this rule rather than escaping it.** It names an ID, never an
  address. `RefuseSharedCache` refuses an address by its shape, and `ValidateAgainst` refuses
  an id the cluster holds no live key for.
  - The address a node dials is the `0xFC` endpoint that MEMBER records about itself in the
    replicated state (`ClusterMember::schedulerEndpoint`), proposed by that machine and
    nobody else.
  - Nothing is sent until the peer proves the key the roster records for that id.
    `NamedMachineTrust` checks BOTH halves: the id alone is a claim anybody can type, and the
    key alone accepts another fleet machine answering at a stale address.
  - The role proven is *recorded MEMBER, under this id*. `LiveKeyOf` reads `members` only, so
    a principal cannot be named, and there is no cache role of its own to prove.
  - Every frame after the proof is sealed under the session key, so nothing captured on that
    leg replays.
  - Nothing a node holds as a secret is presented there. `SharedCacheUpstream` holds no
    `ICredentialSource`, which is structural rather than a setting.
  - The leg speaks its own verb pair, `SharedFetch`/`SharedStore`, which `fastcached` refuses
    by name (`NotSharedCache`, never an unknown opcode).
  - `upstream` stays in `RefusedSettingTable`, and its refusal names `shared-cache` as the
    replicated alternative.
  - Pinned by `No credential reaches the shared cache` and `A node sends nothing to a machine
    that proves another key`.

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

<!-- agent-tripwire: A node IS its state directory: its identity is MINTED into `--cluster-dir` and read back forever -->

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
    is 128 bits from `ISecureRandom` (#1527 -- an engine seeded from `std::random_device`
    reproduces the cloned-image collision wherever that device answers a constant) and
    there is no platform seam for a machine-id at
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

- **A mode is the STATE held in the cluster dir, never a flag or a port**
  ([#1022](https://github.com/LASTRADA-Software/fastcached/issues/1022), then zero-config
  formation). `RunsConsensus` read `!cfg.nodeId.empty()`, so consensus was switched by an
  identity — and an identity whose absence carries a mode can never be given a
  default. Any default at all makes `nodeId.empty()` false forever, so
  `ClusterSelfMember` finds no member on a machine that names no `--raft-peer`,
  `ConsensusNamesNoSelfPeerRefusal` fires, and the one-machine deployment — the
  common one — refuses to start at every boot AND at `--install-service`, where the
  registration replays the same command line forever. That is not a tuning problem:
  the identity cannot be derived while the switch lives on it.
  #1022 moved the switch from the id to the port; zero-config formation moves it again,
  to the record, for the same reason one level up: a port that has a DEFAULT can no longer
  say whether this machine runs consensus, and a record written by the node's own
  transitions can. `RunsConsensus` stays the ONE reader (#613 was two tiers authoring one
  rule) and, since every mode runs consensus, answers whether a record shaped this
  configuration at all; the Raft surface's `resolve` reads the mode's `raftListener`
  column, so `--print-surfaces` prints what the node does.
  - **Every configuration is shaped before it is judged, including before the first mint.**
    `main` reads the record (`ReadKeptFormation`) before any verb or rule is asked and applies
    it, or, where none is kept yet, the solitary record the first start WILL mint, with no
    cluster id (`ProspectiveRecord`); the mint itself (`KeepFormation`) waits until the command
    line has been judged, as the identity's does, and a reload candidate is shaped by the record
    the start kept. A configuration NO record shaped runs no consensus -- a bare test
    configuration -- so a test about a node that runs one shapes it
    (`src/tests/NodeFormationFakes.hpp`), never assumes it.
  - **`--listen-raft` rather than a new `--cluster` boolean** (#1022's reasoning, and it
    carries to the record). A boolean is a second
    thing that can disagree with the port, and both disagreements are states nothing
    could describe: a node that opens a consensus port and runs no consensus, and one
    that runs consensus and opens none. The port *is* the fact, which also keeps the
    rule at one flag.
  - **Asked of the surface ROW, never of `cfg.raftListen`**, for every mode that opens the port;
    a mode that dials in (a learner) runs consensus with none. `RowFor(NodeSurface::Raft)
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
  - **The port is ON by default** (`DefaultRaftListen`): a node with no flags is a one-voter
    cluster of itself, and an EMPTY `--listen-raft=` closes it -- on a mode that opens the port,
    that is a node running no consensus, never a name deciding. Two of the refusals above moved again with it: `--node-id` alone is
    accepted, because every node holds an identity key now and the id travels with it; and
    discovery, on by default too, runs beside consensus only, so only a TYPED `--discovery` is
    refused on a node running none (`discoveryAddressExplicit`) and the defaulted one resolves
    no surface there. A fixture written about a node running no consensus NAMES `--listen-raft=`
    rather than relying on the absence of the flag, one running consensus names `--discovery=`,
    and every one names `--cluster-dir` -- `ctest -R node-fixture-starts`, a scan whose census
    of the scripts CTest hands the node fails CLOSED.
  - **And the port stands down on a name that reaches only this machine** (`localhost`,
    `ConsensusNameWithheld`): a DEFAULTED `--listen-raft` resolves no surface, while every choice
    that needs other machines to reach it is refused by name -- ONE table, `ConsensusPeerAsks()`:
    a typed `--listen-raft`, `--serve-scheduler`, `--raft-join`, `--raft-peer`, a typed
    `--discovery`, a recorded mode other machines dial (`ModeServesConsensusToPeers`: a voter).
    A choice left off it meets its DOWNSTREAM refusal instead ("needs --listen-raft"), which sends
    the operator to add a flag before naming the cause; the refusal's text names every row, and
    a test holds both to the table. Two orthogonal facts folded in `RunsConsensus` and the row, never a
    withheld column on every mode row -- the name is the MACHINE's, and every mode that opens the
    port is subject to it; a learner dials out and needs no name. `[formation][mode]` walks the
    mode TABLE on a `localhost` machine, so a new mode is judged with no edit.
  - **A worker's withheld name is offered to a scheduler on THIS machine at loopback**, and the
    refusal (`WorkerNameReachesOnlyThisMachineRefusal`) is only for a scheduler elsewhere
    (`SchedulerIsRemote`): a solitary node on a `localhost` machine registers its worker with
    its own scheduler and serves.

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

- **`RaftNode` reads no clock, opens no socket and draws no randomness of its own**,
  and all three clauses are load-bearing rather than one habit stated three ways. It
  is a pure state machine: every instant it decides from arrives as a `now` argument,
  every message it wants sent leaves through `RaftOutput`, and every random draw --
  election jitter -- comes from the `IRandomSource` it was handed. That is what lets
  `RaftClusterHarness` run a whole cluster in one process against a scripted partition
  and a `ManualClock`, and what makes the six `ManualClock` cases pinning pre-vote and
  CheckQuorum EXACT rather than approximate: a rule about a window cannot be pinned by
  a test whose subject reads a clock the test does not control.
  - **The driver is where all three re-enter**, which is why the split exists at all:
    `RaftDriver` owns the reactor, the sleep, the sockets and the mutex, and `RaftNode`
    owns the algorithm. A convenience that reached for `steady_clock::now()`,
    `ISocket` or a seeded engine inside the node would not fail any existing case --
    it would quietly make every future one inexact, which is the failure nothing
    reports. The same sentence, one layer out, is the distributed rules' *a heartbeat
    age is a duration on a report, never a `TimePoint` on `WorkerInfo`*.
  - **A seeded draw is not an exception to the randomness clause**; it is how the
    clause is satisfied. `SystemRandomSource`'s fixed-seed constructor and
    `UniformInRange` exist so a harness schedule is reproducible, and they are reached
    through the injected source like everything else -- see *A seeded draw must be the
    same on every platform* below, and #1527's `ISecureRandom` for the draws whose
    unpredictability is a security property rather than a reproducibility one.

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
  - **Durable means the DIRECTORY too.** Every file a node keeps in its state directory is
    replaced through ONE writer, `Consensus::ReplaceFileWith`: write, flush to the platter, a
    CHECKED close, rename, and then the parent directory flushed (`SyncDirectoryToDisk`). The
    rename changed the directory, so a replace reported before that flush is one a power loss
    can take back -- the old entry returns, and for the Raft state that is a forgotten vote and
    a node that votes twice in one term. POSIX `fsync`s the directory; Windows flushes a handle
    to it opened with `FILE_FLAG_BACKUP_SEMANTICS` and `FILE_WRITE_DATA`, measured to succeed on
    NTFS and ReFS where read access alone is refused. A failed directory sync is a failed
    replace, reported and never taken as durable; the identity key, created once rather than
    replaced, flushes its directory the same way. **Except a FILESYSTEM that cannot sync a
    directory at all** (`MeansDirectorySyncUnsupported`, a table of named answers per platform):
    a sync it does not offer never succeeds, so refusing it refused EVERY state write on such a
    volume. That is DEGRADED -- the replace lands, the answer rides `ReplacedBy::directoryUnsynced`,
    and the start probe says it once and counts it (`StateDirectorySyncsUnsupported`) -- while any
    OTHER refusal of the sync still fails the replace, since it may be a sync that would have
    succeeded.
    **Such a volume runs with a STATED durability gap**: a power loss there can resurrect an old
    vote or record -- PostgreSQL's `fsync_fname` precedent -- and the start probe's one warning and
    `fastcache_state_directory_syncs_unsupported_total` are what an operator sees of it.
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
  - **A recovered snapshot reaches the APPLICATION, and constructing the driver is
    what hands it over**
    ([#1542](https://github.com/LASTRADA-Software/fastcached/issues/1542)). A node
    recovered from storage comes back with `_lastApplied` AT the snapshot's boundary,
    so nothing the snapshot covers is ever applied again -- and `RestoreSnapshot` was
    called only on `output.restoreSnapshot`, which only `OnInstallSnapshot` sets. So a
    node that compacted (every 512 entries) and restarted ran without every cluster
    fact its snapshot held: members, settings, and the revoked keys, which made
    REMOVAL fail OPEN -- a forgotten machine admitted again after a restart, reported by
    nothing. It recovered only if a leader later sent it an `InstallSnapshot`, which a
    follower whose log is current never gets. `RaftDriver::Create` -- a factory over
    a private constructor, the one way a driver is built -- now restores a node's
    snapshot into the application before any step can apply an entry above it, so no
    caller has to remember, and `IRaftStateMachine::RestoreSnapshot` states both of its
    callers -- recovery and install -- and is told which (`SnapshotOrigin`). Asked of
    the boundary (`SnapshotIndex() != BeforeFirst()`), never of the bytes, because an
    empty state is a legitimate snapshot. `ConsensusTier` reads the recovered term
    BEFORE building the driver, since the restore's publication announces the role.
  - **And a node whose own state the application cannot read does not start.** The
    restore above met a change of the cluster state's encoding (#178's `StateVersion`
    5 -> 6, `CommandVersion` 2 -> 3): a node restarting over its OWN snapshot from the
    build before reached `RestoreSnapshot`, which logged *cannot decode ... keeping
    current state* -- and the node then RAN with an empty `ClusterState`, revocations
    included, while `Apply` skipped every retained command it could not decode.
    Removal failed open on the upgrade path, loudly but open. So `Create` is FALLIBLE:
    it asks `CanRead` -- const, no effects -- of every command the log holds above the
    snapshot, and only then restores, and `RestoreSnapshot` replaces wholesale or
    refuses changing NOTHING. The ORDER is the property: a refusal means the
    application was handed nothing, never a snapshot with no future or the entries
    without the snapshot under them. The refusal code is the storage rule's --
    `UnsupportedFormatVersion` for intact bytes another build laid out, `StorageFailure`
    only for bytes that are no state at all -- and `ConsensusTier::Start` names the
    directory, where in the state, both versions and `UnreadableStateRemedy`, which is
    ONE remedy with the store's own format refusal because the three files go aside
    together. That remedy moves `raft-state`, `raft-log` and `raft-snapshot` and KEEPS
    `node-id` and `node-key` -- moving the whole directory mints a new identity the
    cluster must admit while it still counts the old one -- and rejoins with
    `--raft-join`, because an empty node under a bootstrap set naming only itself
    elects itself and becomes a second cluster. A recovered snapshot's log line says
    *recovered*, an installed one's *installed*.
  - **And a LEADER's snapshot the application cannot read is refused before the node
    takes it on** ([#1552](https://github.com/LASTRADA-Software/fastcached/issues/1552)).
    Taken on, a snapshot resets the log, moves both indices past it, and is persisted
    and ACKNOWLEDGED inside one step; the application saw it only at the end, logged
    *keeping current state*, and every later entry was then applied on the state the
    snapshot was meant to replace while Raft reported the replica current. So the driver
    asks `IRaftStateMachine::CanRestore` -- const, and in agreement with `RestoreSnapshot`
    -- BEFORE the node sees the offer, and hands the node a `SnapshotReadability`. An
    unreadable one is answered `Rejected` after the covered check (a snapshot this node
    already holds needs no reading) and after the leader was HEARD (a refused offer still
    arms the election timer, so the node does not campaign against the leader offering it),
    and nothing moves. **The decision, recorded: refuse and stay behind, never stop the
    tier.** Both need the question asked first -- stopping after the node took the snapshot
    on leaves an acknowledgement nothing can retract -- and staying behind is what Raft
    already knows how to be: the leader counts the answer as contact and advances nothing,
    the node keeps its vote, its application stays a committed prefix rather than a mix,
    and it catches up BY ITSELF the moment it can read an offer, with nothing moved aside.
    Stopping would buy none of that and cost the vote. The refusal is held by the driver
    (`Progress::installRefusal`, reported once through `ObserveInstallRefusal` however often
    the leader offers again, ended when the node has applied past it) and raised as the
    `unreadable-leader-snapshot` condition, Live and Alert, with an Error line. A machine
    whose `CanRestore` accepted bytes its `RestoreSnapshot` then refuses stops the driver:
    by then the node has persisted and acknowledged the snapshot.
  - **The harness's state machine holds real state, and a restart empties it.** Its
    `RestoreSnapshot` was a no-op ("this machine records what it was told") and its
    `TakeSnapshot` returned nothing, so every restart case passed whether or not a
    snapshot reached the application -- the fake more permissive than the component
    it stands for, which is exactly how #1542 went unseen. `Member::application` is
    now the application's state (snapshotted, restored wholesale, cleared by
    `Restart` as a process's memory is) and `Member::applied` stays the HISTORY State
    Machine Safety is checked against. A restart case asserts the application, not
    only the log; the harness takes a `CompactionPolicy`, so a case can make every
    node compact at all. Its `Restart` goes through `RaftDriver::Create` too and
    returns the refusal: a node whose restart is refused is DOWN -- no driver, reached
    by no message, ticked by no step -- never still running the driver it had.
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
  - **A node must have DISSOLVED its solitary cluster before it adopts a roster.** Every
    node bootstraps itself now -- a solitary cluster at its first start -- so the rule is
    kept by ARCHIVING rather than by not bootstrapping: an approval writes the learner
    record with `archivePending` naming the solitary cluster and MOVES NO FILE -- the
    decision is taken while that cluster's tier still runs, holding its log open and
    rewriting its state into the root, and on POSIX a store moved under it is re-created
    in the root after the record said it was archived. The move asks for a reform, and
    only `ResumeFormation`, at that reform or at a start, before any tier opens the
    directory, moves `FileRaftStorage::StoreFileNames()` under `archive/<cluster-id>/` in the
    state directory (kept, never deleted: filled as `<id>.partial` and renamed into place,
    so a complete archive is never written again and a cluster left twice lands beside the
    first, in `<id>.1`); nothing else moves a store.
    A learner therefore never opens the log that elected it. The archive is a row of the
    state-file table like every other entry, and the start-time walk accepts its fixed
    layout and nothing else below the top level -- a directory the node writes and the
    walk refuses is a node that cannot start after its first join. The reason the rule
    exists is unchanged: a node that bootstrapped itself and KEPT that store can never be
    admitted. With `--raft-peer` naming it, a new machine elected itself, took
    a term and a log, and afterwards refused `AppendEntries` from every leader its own
    configuration did not name — and two clusters cannot be merged by any local rule.
    So `RaftConfig::members` may be **empty**, meaning "no cluster yet": such a node
    never stands (`NextDeadline` reports that nothing falls due, rather than naming a
    deadline `Tick` would have to decline to act on), grants no votes, and accepts
    `AppendEntries` and `InstallSnapshot` from **any** leader — because the membership
    test that guards those has nothing to test against and the only way to learn a
    member set is to be sent one. It gives nothing away: the node holds no committed
    state, has never voted, and is counted by nobody. The moment it adopts a
    configuration the guard applies again, permanently.
  - **A reform is a START of the body, and is judged and adopted as one.**
    `Node::RunNodeBodies` adopts the record again before every body but the first
    (`ReadoptFormation`), judges the reshaped configuration by `StartupPolicyRejection`
    before it serves, and publishes it -- otherwise a reform serves a shape the next restart
    refuses, and every reload between is declined. The body that ended was adopted from a
    record, so one that is GONE is lost state, refused by name (`FormationRecordGone`):
    only a first start mints, or the node leaves its fleet for a cluster of its own over
    the fleet's store. A reload shapes its candidate by the record the RUNNING body
    adopted (`RunningFormationOf`), never the file a move saves ahead of the body.
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
  - **Every member record says where its `0xFC` port answers.** `ClusterMember::schedulerEndpoint`
    is every member's `0xFC` endpoint, learners included, and it is what anything resolving a
    machine reads (a shared-cache resolver among them). It is set at join from the
    `Enroll` request, which carries the joiner's CURRENT advertised endpoint (read through
    `Cc::IAdvertisedEndpointSource` at every poll, so a reload between polls is what the leader
    records) and is SIGNED by the key it asks with, over every field it states -- verified before
    the window records or refreshes anything (`Cluster::VerifyEnrollRequest`, before any other claim
    is reported on, revocation included). The id and the key are PUBLIC, so an unsigned request let
    any host poll under a joiner's pair with its own endpoint, and the last poll before approval was
    what the record kept. It is moved afterwards only by a PROVEN NODE-ANNOUNCE: the leader compares the endpoint
    a member announced, under the id it proved on that connection, with the recorded one, and
    re-proposes that member's record (its own seat, `publicKey = nullopt` so its key is kept)
    through the ordinary desired-member reconcile (`Cluster::AnnouncedEndpointDesires`). Never from
    an unproven claim, never for an id the state does not record (an announcement admits nobody),
    and never more than one change in flight per member. A leader asserts its own on every pass,
    read from the same source; an operator's admit or promotion states none and KEEPS the recorded
    one, since `AddMember` applies wholesale and the same machine at the same port has not moved --
    but only under the SAME key (or none): a re-admit naming another key records a REPLACED
    machine, and keeping its predecessor's endpoint would send a resolver to the old host expecting
    the new key, so it clears until the new machine announces.
    - **What the record may hold is ONE rule, asked wherever an endpoint is produced and once more
      where the record is decided**: one ANOTHER machine can dial (`IsPeerDialableEndpoint`: a
      dial endpoint whose host is neither `NamesOnlyThisMachine` nor a wildcard), or none.
      `Cluster::Validate` refuses anything else on `AddMember`/`AddLearner`, whichever route
      proposed it, and each producer -- the joiner's `Enroll`, the responder judging it, a proven
      announcement, a leader's own word -- states none in its place. Three routes with three
      filters had recorded a loopback member the join route refused, and a resolver sent there
      reaches ITSELF with no error at either end.
  - **`--cluster-admit` is the counterpart `--cluster-forget` never had.** Nothing
    could put a member *into* the replicated state without `--discovery`, so a typed
    fleet could shrink and never grow. It carries `<id>=<host>:<port>` — the same
    token `--raft-peer` takes, because a second spelling of one thing is a second
    thing to get wrong — and no scheduler endpoint, because a member announces its own
    once elected and a value typed about somebody else would outrank what they say
    about themselves.
  - **A forget outranks an observation**
    ([#1528](https://github.com/LASTRADA-Software/fastcached/issues/1528)). Everything
    the reconciler is handed is an OBSERVATION -- a peer proved the key, this node knows
    its own record -- and `--cluster-forget` leaves the machine running with the key, so
    discovery proves it again at its next beacon. `ConsensusTier::Desire` never prunes,
    and `MembershipProposals` proposed every desired id the state lacked: the leader
    re-recorded the forgotten member on the very next pass and the quorum flapped --
    removed on one pass, re-added on the next. Inferred from reading, then REPRODUCED at
    the policy before the fix. So `MembershipProposals` refuses a desire for a forgotten
    id -- by NAME, into `MembershipPlan::forgotten`, which the tier logs once per member,
    because a refused desire and one the state already matches both propose nothing.
    **Refused at the decision, not by pruning the desire**: discovery hands it back at
    the next proof for as long as the machine holds the key. The predicate is
    `ForgottenById` -- a key `revokedKeys` holds under that id, the one forget fact that
    outlives the record it removed -- asked only of a desire that would propose something,
    and it covers this node's OWN record. It is asked by the id and the key, never by an
    address: a machine is forgotten wherever it now dials from, which is what answers on
    a rig sharing one machine over loopback and what survives a machine moving to a new
    address.

    **So only a NEW key brings a forgotten machine back.** `--cluster-admit` records the
    id again, but `KeyRevoked` is PERMANENT (*A revoked key is never admitted again*,
    below), so re-admitting the same machine under the key it still holds is refused by
    name -- an operator re-keys it with `--print-identity` after wiping `node-key`, or
    it stays out. That is the intended cost rather than an awkwardness: a forget is the
    only removal this cluster has, and one an operator could undo by retyping the
    original token would be removal failing open by the front door.
  - **A forget means the same thing whoever currently leads**
    ([#1539](https://github.com/LASTRADA-Software/fastcached/issues/1539)). After #1528 a
    forgotten LEADER stopped recording itself and still led, counted, indefinitely:
    `NextQuorumChange` skipped `id == self`, and a node's own bootstrap set always names
    it. Now a leader that is FORGOTTEN proposes its own removal, LAST -- after every
    change it can still make as the leader -- and steps down once it commits (`RaftNode`,
    §4.2.2; #1449's demoted leader is the same rule). **Forgotten is the record gone
    AND a key revoked under the id**, never the record alone (#1555): record absence
    alone is every fresh leader's first pass. Neutered to absence alone, five cases go red,
    including the pre-existing *this node never proposes its own removal*. **Its own
    bootstrap entry does not protect it, and nobody else's is touched**: a node names
    itself because it cannot start otherwise, so that one entry asserts nothing. A
    forgotten typed FOLLOWER stayed counted -- the control -- until #1555 made the forget
    revoke its key; see *A forget revokes*, below. Forgetting the ONLY voter is refused BY
    NAME before it is proposed (`PrepareForget`, from `ConsensusTier::Propose`,
    `InvalidConfiguration`): afterwards the record would say forgotten while no
    configuration could ever drop it. Pinned in `RaftClusterHarness`
    (`MembershipCluster_test`): the configuration commits without the leader, it steps
    down, a successor is elected and refuses it by name; neutering the self-removal keeps
    it in the configuration and leading.
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

## Learners: a member no quorum counts

<!-- agent-tripwire: Voting is a property of the CONFIGURATION, never of a role -->

[#1449](https://github.com/LASTRADA-Software/fastcached/issues/1449), for the machine
[#178](https://github.com/LASTRADA-Software/fastcached/issues/178) describes: an always-on
node and a laptop. With the laptop a voter, two voters are a quorum of two, so the laptop
leaving the VPN costs the always-on node its majority and CheckQuorum deposes it at the next
tick -- the scheduler answers `NotLeader` and the fleet page goes dark until it comes back.

- **Voting is a property of the CONFIGURATION, not of a role.** A learner still follows, so it
  cannot be a fifth state `Tick` moves between: it is a `Follower` whose STANDING forbids the
  two things a follower may otherwise do. `Consensus::Configuration` is two disjoint sets with
  at least one voter, and `Membership::StandingOf(configuration, id)` is the ONE author of
  *where does this node sit* -- `RaftNode::CurrentStanding`, `--node-status`'s
  `consensus-standing`, the member series' `seat` label and the node's own log line all ask it.
  What a standing permits is `StandingTable`'s `timer` and `votes` columns, never a switch: a
  learner's row is `TimerKind::None`, so it has no election deadline at all rather than one it
  ignores. A LEADER keeps its heartbeat whatever its standing -- one demoted by a change it has
  not committed is the only node that can commit it -- and steps down once that change commits.
- **A learner refuses a vote by ROW, never by silence.** `VoteRefusal` names why a pre-vote or
  a vote was denied and the row comes FIRST (`CastsNoVote`), before the term, the log or a live
  leader: a learner behind on its log would otherwise refuse "for being behind" and a test
  asserting *denied* would pass under a learner that votes whenever it is caught up. The enum
  is private and in `RaftOutput` only, so it is observable without being a wire contract; the
  wire answer is still `Denied`.
- **Every quorum read counts voters through `Membership::QuorumOf`, and replication reaches
  both sets.** Commitment (`AdvanceCommitIndex` walks the voters' match indices, self only when
  a voter), pre-vote and vote tallies (a grant from a non-voter is dropped on arrival, and a
  candidate counts its own vote only when it is a voter -- which also closed a pre-existing
  defect: a node removed from a one-voter configuration elected ITSELF), and CheckQuorum
  (`HasQuorumContact` walks the committed configuration's voters). `_peers` is everybody
  replicated to, `_voterPeers` everybody asked for a vote; a learner's answers land in
  `_followerContact` like anybody's and are simply not counted.
- **A leader named only as a LEARNER is still a leader to follow.** `OnAppendEntries` and
  `OnInstallSnapshot` test `IsMember`, not `IsVoter`, and deliberately: a follower that has not
  yet applied the entry promoting its new leader holds a configuration naming that leader as a
  learner, and refusing it would wedge exactly the node that needs catching up. Both handlers,
  for the "a leader spoke arrives at two handlers" rule.
- **One change at a time is restated for VOTERS.** `ChangeShape` is `AddedOne`, `RemovedOne`,
  `PromotedOne`, `DemotedOne` -- exactly one member moves, and a promotion counts as the voter
  addition it is. Two moves in one proposal (promote one and remove another, add two) is
  `Unsafe`, because a majority of the old voters and a majority of the new could then share no
  voter. A learner addition or removal changes no voter set, so it is always safe; the READ
  argument (CheckQuorum on the committed configuration while a change is in flight) needs the
  shared voter and is unaffected by learners.
- **The reconciler's order is additions, promotions, demotions, removals**
  (`Cluster::NextQuorumChange`): growing the voter set before shrinking it. A demotion or
  removal that would leave no voter is never proposed -- the record then runs ahead of
  consensus, which is the fail-closed direction.
- **A voter is COUNTED only once it has caught up** (#1537). Every member enters the
  configuration as a LEARNER, whatever its record names, and one recorded as a voter is
  promoted once it is dialable AND its match index has reached the commit index
  (`Replication::CaughtUp`, from `RaftDriver::Progress::matchIndex` read under the same lock as
  the commit index). REPRODUCED before the fix, over `RaftClusterHarness`: promote an absent
  learner in a one-voter cluster -- or admit a voter that is not up -- and a write proposed
  afterwards never commits while the leader goes on leading; the promotion entry itself needs
  both machines. It governs WHEN a promotion takes effect and never WHETHER to promote, which
  stays the operator's (#1535). The wait is NAMED (`QuorumPlan::catchingUp`), because from the
  outside it is `seat=voter` against a `learner` standing, which reads as a bug: the leader
  logs it once and again at Warn after `QuorumProposalPatience`. Neutering the gate reddens both
  harness cases; neutering the staging reddens the admission case and the four
  additions-enter-as-learners units, and the promotion case stays green.
- **A learner has no endpoint.** `StandingTable`'s `link` column says how a standing is
  reached: a voter is `Dialled`, a learner `DialsIn`, and nobody reaches the other two. A seat
  reads it through its standing (`MemberSeatRow::standing`, `Cluster::LinkOfSeat`), which is
  `static_assert`ed to agree with the seat's `counted` column, so the column is stated once.
  `Validate` requires an endpoint only for a seat whose link is `Dialled` (`SeatNeedsEndpoint`),
  so a learner is recorded with none and a PROMOTION of a learner that never had one is refused
  by name; the reconciler adds a learner without one (`ReachableInItsSeat`, at the ADDITION
  site only -- a promotion counts the member, and a counted member is dialled, so `Countable`
  keeps `Dialable`); and `MembershipProposals` drops a desire with no endpoint only where its
  seat is dialled, since a learner's empty endpoint is its whole record. A member recorded as a
  VOTER still enters the configuration as a learner and is promoted only once dialable and
  caught up, so one with no address is never added and never promoted. Neutering the learner
  row's `link` to `Dialled` reddens the table case, the learner half of the `Validate` case, the
  addition case and the desire case, and leaves the voter half green; a report renders a learner's
  missing endpoint ABSENT (`cluster-members`, `--cluster-status`), never as a blank.
- **A leader reaches a learner over the session the learner dials, and dials none it has no
  address for -- NOT YET "never dials a learner": the node still dials a learner whose record
  carries an address, until a learner node's own transport dials two-way, and restoring the plain
  headline is owed with that change.** A learner sits behind NAT, a VPN or a laptop lid; a leader that dialled
  it would spend its sender on an address that answers nothing. The learner dials every voter
  two-way (`SessionDirection::TwoWay`, signed into the proof), the acceptor attaches the session
  to its transport (`IRaftInboundLinks`, `RaftSessionLink`), and `Send` to a peer the transport
  does not dial rides that session or is DROPPED and counted, never dialled: a dialled peer, else
  an attached session, else a drop. A dialled peer wins, which is why the node's own dial of a
  learner's recorded address still reaches it today. The drop is counted on the row for what the
  transport OBSERVED (`SendDrop`): a peer it was told dials in and has no session attached is
  `RaftSendsDroppedNoSession` -- a learner offline or not yet dialled, which waiting fixes -- and
  a peer it can place nowhere is `RaftSendsDroppedUnknownPeer`, which waiting does not. Folding
  them back together reports a shut laptop as an id nobody gave this node, or the reverse:
  counting every drop as no-session reddens the two unknown-peer cases, and ignoring the placement
  reddens the three no-session cases and the tier's, which is the one that sees `LearnMembers`.
  The column is the only place that decides which peers dial in: the node's `LearnMembers` reads
  it and places exactly the peers whose link is `DialsIn` (`RaftPeerTransport::LearnDialsIn`, the
  whole set on every pass, since a promotion or a forget takes one out), so the dial-in path is
  expected only where the column says `DialsIn`. It reads the column TWICE (`DialInPeers`):
  through the record's seats (`Cluster::LinkOfSeat`) and through the configuration's standings
  (`Membership::StandingOf`), because a restarted node recovers the configuration at once and the
  commands only after it has led and committed again -- a learner admitted in the log tail is sent
  to before the applied state records it, and placed from the record alone every such drop landed
  on the row that says no wait fixes it. The direction travels from the SIGNED proof to the attach
  (`ProvenPeer::direction`, read through `SessionDirectionRows`' `acceptorWrites` column), because
  a two-way session the acceptor never attaches is a learner the leader never reaches, and nothing
  about that is visible. Each attached socket keeps ONE read and ONE write operation, which
  core-cpp ends the process to enforce: the server's loop only reads it once its verdict is
  written, and the transport's sender for that link is its only writer. The link is detached on
  every way the session ends (`AttachedLink`, RAII, before the socket closes); a newer session
  proving the same ID CLOSES the one it supersedes (`Attach` replaces by peer id), whose detach
  then leaves the newer one attached, and COUNTS it (`RaftInboundSessionsSuperseded`) -- one per
  reconnect is a roaming learner, a steady rate two machines sharing one key, and only the rate
  separates them, so no condition claims a clone from a supersede; and a transport that is
  stopping attaches nothing. A key withdrawn mid-session is counted once, on the ACCEPTOR's
  `KeyWithdrawn` row, by whichever of the reader and the sender notices first. The senders are
  detached coroutines holding a share of their entry, never `Task`s the map owns: an entry IS
  erased on a detach, and destroying a parked `Task` frees a frame the reactor still points into.
- **The SEAT is the operator's record, written only by the verb.** `ClusterMember::seat` is set
  by `AddMember` (voter) or `AddLearner` (learner), so promotion and demotion are re-admissions
  and `MemberSeatTable` is the one statement of which verb writes which seat and which
  configuration set it means. **A desire carries no seat at all** (#1535; #1449 had an optional
  one): a node desires ITSELF on every pass and discovery re-desires every peer at every proof,
  so a seat fixed when the desire was made would undo an operator's promotion or demotion one
  interval after it committed. `MembershipProposals` decides it against the state at every pass
  (`SeatFor`): the recorded seat, else the set the CONFIGURATION counts it in -- a `--raft-peer`
  member is counted and recorded nowhere, and read from the state alone it looks exactly like a
  newcomer, so recording it as one DEMOTES it -- else `NewcomerSeat`. The record outranks the
  configuration because a demotion in flight is the record running ahead. An enrollment
  approval, which recovery repeats, states no seat either (`RecordedSeatOf`).
- **A machine discovery proves joins as a LEARNER, and an operator promotes it** (#1535). A
  proof says a machine holds the key now; a vote says it will go on answering, and admitted
  straight as a voter a laptop made the always-on machine beside it unable to commit or re-elect
  alone -- #178's failure, reached through discovery. The shared key also names no holder, so a
  vote per proof is a vote any key holder can multiply. **Promotion is deliberately NOT
  automatic once caught up**: that was the alternative, and it answers the wrong question --
  caught up is *can answer now*, which the proof already said, and nothing records whether a
  learner is the operator's (the laptop, meant to stay one) or discovery's. #178's approved
  design ends discovery admission altogether, so votes as an operator act is where this is
  heading anyway. The cost, stated in the docs: a fleet formed by discovery alone has ONE voter
  until somebody promotes more. Pinned with the neuter *newcomers are voters* reddening the
  per-pass quorum assertion, and a typed-peer control the neuter *ignore the configuration*
  reddens.
- **A learner is never removed for being absent.** Nothing in `NextQuorumChange` asks whether a
  member answers, so this is the bootstrap rule applied to a second population: only a member
  the operator FORGOT, and admitted at runtime, is proposed for removal, whichever set it is in.
- **The formats moved, each refused by name.** The configuration payload is two id lists and
  carries no version of its own -- its containers do: the Raft store is format 2
  (`FileRaftStorage`, every log record now carrying its format so an old log can be told from a
  torn one), `RaftWire` was version 3 (4 since #178), `ClusterState`'s encoding is 5, and the live-stats
  grammar is `-5`. An old store is `UnsupportedFormatVersion` -- a new `ConsensusErrorCode`,
  `Moment`, mapped to `InvalidClusterChange` on the wire -- judged from the header before the
  CRC, so an intact store of another vintage never reads as damage. `CommandVersion` did NOT
  move: `AddLearner` is a verb, not a field, which is also why #144's trigger has not fired.
  - **A log's FIRST record decides the store's format, and a foreign LATER record is a TORN
    TAIL.** Per-record formats make both questions askable and make it possible to answer them
    oppositely, which is the mistake: a log is written by one build at a time, so a record whose
    format differs from the first record's cannot be *the store's* format arriving late -- it is
    an interrupted write, or bytes from a run that was replaced, and the log ends there. Read the
    other way round, a single torn record at the tail condemns a healthy store as
    `UnsupportedFormatVersion` and an operator moves a log that only needed truncating; read as
    damage, an entire store another build wrote reports `Corrupt` and somebody deletes it. The
    storage rules' `UnsupportedFormatVersion` / `Corrupt` split, stated about the one file where
    both answers are reachable from the same bytes.
- **What the neuters showed.** Counting learners in `QuorumOf` reddens five cases, not one,
  because `QuorumOf` is the single author of commitment, elections and CheckQuorum alike -- the
  absent-learner cluster case (at committing alone and re-leading alone: the learner
  configuration itself never commits under that neuter, so CheckQuorum keeps reading the
  voter-only one), its node-level twin, and the three cases asserting commitment, election and
  the quorum function with learners present. Treating a learner as a voter in CheckQuorum ALONE
  reddens exactly the two absent-learner leadership cases, the cluster one at the per-step
  leadership assertion. Letting a learner stand reddens the vote-refusal case and the four other
  cases asserting *a learner never stands* by another route (the table, a snapshot, a demotion,
  a promotion's before-state). An "only this case" prediction is a claim about how many tests
  assert one property, and here several do.
- **What a learner cannot do, stated so nobody expects it.** It cannot rescue a cluster whose
  voters are gone -- it was never part of the majority that decides -- so promote it while a
  voter can still commit the change. INFERRED, not tested: a node whose promotion has committed
  on the leader but not yet reached it still refuses votes by its row, so a leader lost in that
  window leaves the cluster waiting for it to return rather than electing around it.

## Identity keys and the roster

<!-- agent-tripwire: Only an ABSENT `node-key` mints (#178): a key file that is there and cannot be used is refused by name -->

[#178](https://github.com/LASTRADA-Software/fastcached/issues/178) PR 2: every node with a
state directory holds an Ed25519 identity key, and `ClusterState` records members' keys,
principals admitted by key, and keys revoked for good. PR 3 made the Raft peer wire VERIFY
against them (see *The Raft peer wire*), and PR 4 moved discovery and enrollment onto them
(see *Discovery and the identity key*, and the enrollment window in
`distributed-compilation.md`); PR 5 moved leases onto them and PR 6 the `0xFC` surface, which
is when the cluster key went. Every rule below is about getting the record
right, because the wire now trusts it.

- **Only an ABSENT key file mints.** `node-key` is read back on every start; a file that is
  there and cannot be used -- unreadable, truncated, not a key file, another build's format,
  or a seed and a public key that disagree -- is REFUSED by name and left untouched, the
  `node-id` rule arriving at the file that proves the id. Absent is what the OPEN says
  (`ENOENT`), never `exists()`, and the mint is an exclusive create with nothing in front of
  it -- the idiom `StoreClusterKey` had until enrollment stopped writing a key file (#178). The file stores the public key beside the seed on purpose: a
  flipped bit in a bare seed is another VALID key, adopted silently. `ctest -R` the
  `[identity][key]` cases; the truncation case is the one a "re-mint on unreadable" neuter
  turns red.
  - **The key file is its OWNER's alone whoever created the directory it is in.** A mint
    CREATES the file already protected (`CreateStateFile`, the key's row: a security descriptor
    with SYSTEM, Administrators and OWNER RIGHTS, and share mode 0; POSIX `O_EXCL` 0600),
    reads the protection back (`INodeKeyFileGuard::Protect`) and only then writes the secret.
    **Access is decided at OPEN, never per read**: a list applied after the create leaves
    readable every handle opened in between, and the later write reaches it -- measured. One
    whose protection does not read back (`Undetermined` included, at MINT only) is removed
    while still empty and refused `Unprotectable`. The ORDER is the property, so the `[secret]`
    cases assert the file's size AT the protect and that a racing open there FAILS.
  - **A key nobody else can read may still be one somebody else WROTE.** A state directory
    other accounts can add to or delete from is refused (`OpenDirectory`, with the remedy -- TWO
    commands on Windows, `/inheritance:d` THEN `/remove:g`, since one reports success on an
    INHERITED list and removes nothing, measured; and because that removal propagates and would
    erase an inherited key read, the refusal carries the key's `Exposed` question itself; on
    POSIX any group/other write counts, the sticky bit NOT excepted, since it stops removal and
    never the creation of a name not yet there) -- whoever may write there decides which
    key is found and whether one is, and a deleted key is a silent re-mint -- and one the node
    creates itself is created owner-only (`CreateOwnerOnlyDirectory`, the identity's mint
    included). The key is resolved BEFORE the id and the formation record, so nothing is
    written into an unjudged directory.
  - **And so is every other file of the state directory: ONE table, `NodeStateFiles()`**, a
    row per file naming what it holds, what trusting another account's copy would make the
    node do, the remedy, and an ANSWER column. Every entry at any depth is asked its owner
    (`RefuseForeignStateFiles`, from `ResolveNodeKey` and ahead of the two earlier READS --
    the start's formation record and enrollment's consensus history); `Another` is refused
    `ForeignOwner`, naming the file, what it holds and the owner, and left as it is. So is a file
    OTHER ACCOUNTS MAY WRITE (`OthersMayWrite`, with `chmod go-w` or the two `icacls` as its
    remedy): whoever owns it, its contents are theirs to choose. A temporary answers as its file;
    an entry no row names is refused WHOEVER owns it (`UnknownEntry`) -- the node writes nothing
    without a row, and its own account's stray entry is no less decisive; a LINK is refused
    before its owner is asked (`LinkEntry`): entries are judged as themselves (`lstat`, a reparse
    point opened as itself), never through what they point at, and the node writes none. The histories answer
    `StartWithout` -- no state of a history file may keep a node from starting -- and are set
    aside where they are read (`SetAsideForeignHistory`): not read, never written over, WARNED.
    **The table is the ONLY author of a state file's name**: `StateFile` (`Core/StateFiles.hpp`,
    header-only because four layers write these files) names every one, every writer's constant
    is a lookup into it, and the node's table is an `EnumTable` keyed by the same enum -- so a new
    file cannot be named without a row. A real-filesystem case runs the writers besides, and
    requires a row per entry AND an entry per row.
    **Who may read a state file is a COLUMN of that table too** (`StateFileAccess`, in
    `Core/StateFiles.hpp`), and every writer creates through ONE call that takes the file,
    never a mode (`CreateStateFile`). The protected owner-only list, POSIX 0600, is the KEY's
    alone. Every other state file -- temporaries included -- is created exclusively with the
    JUDGED directory's list on Windows, since a list of its own locks out the service account the
    directory grants when an elevated operator (`--print-identity`, an install) wrote it; and on
    POSIX with EXACTLY 0644, set on the descriptor after the create. **Never the umask's**: the
    create's 0666 less a umask of 000 left formation, the Raft store and the id world-writable,
    and the next start adopted them -- measured. 0644 rather than 0600 because a verb run as
    another account (`sudo --print-identity`) writes files the service's account must read. A
    Linux case runs every writer under umask 000 and asserts each file's mode is its row's.
    Store directories are `CreateOwnerOnlyDirectory`'s. A state file that is there and cannot
    be opened is REFUSED with `StateFileUnreadableHint`'s line -- `/setowner` on Windows,
    `chown` on POSIX -- never read as absent (the `node-id` did, and would have minted over it).
    **The directory is judged BEFORE the key is opened**, and the key is opened as a regular
    file only (`OpenRegularFile`: `O_NONBLOCK | O_NOFOLLOW` and `fstat`; a reparse point opened
    as itself on Windows): a FIFO planted under its name blocked an ordinary open forever, before
    the refusal the directory deserved (`NotARegularFile`).
  - **A key file others may read is refused `Exposed` and left untouched, never tightened
    in place**: whether the key was read while exposed is the operator's question, and the
    refusal names both answers and what each costs -- and BRANCHES on the only voter, which a
    node with no cluster flags is and which `--cluster-forget` refuses to remove. One table,
    two columns (`RefusesKeyExposure(exposure, KeyMoment)`), differing in `Undetermined` alone:
    refused at MINT, reported and read at READ. The secret-exposure row tells the key the SAME
    owner-only remedy (`SecretFileRow::hint`), never the services one.
- **A node holds a key when it has a state directory to hold it in** -- consensus, or a named
  `--cluster-dir` -- and holds NONE otherwise, said at startup. A key minted into a working
  directory is minted afresh at every boot under the packaged unit (a runtime directory) and
  lands in `System32` under the SCM: a new machine at every boot is worse than no key.
  `--install-service` mints no key; the service mints its own, as the account it runs as.
- **The key file is a secret-by-path row, and the row's path is DERIVED.** `--cluster-dir`
  moved from the public table to `NodeSecretFileTable()`, whose rows are now PROJECTIONS
  (`SecretFileRow::path` is a function of the configuration) because the flag names a
  directory and the secret is the file the node minted inside it. `NodeKeyPath` is the one
  author of that path, asked by the start and by the row.
- **A public key has ONE spelling**: 43 characters of unpadded base64url
  (`FormatEd25519PublicKey` / `ParseEd25519PublicKey`, canonical last character), shown whole
  everywhere. It travels as its 32 bytes -- in `ClusterState`, in a command, in
  `NodeRuntimeFields::identityPublicKey` -- and is rendered only at the edge.
- **`ValidateAgainst` is the courtesy and `Apply` is the guarantee.** The key rules are about
  what the state already holds, so `Validate(Command)` cannot ask them; every proposer asks
  `ValidateAgainst(state, command)` so the operator is told, and `Apply` enforces the same
  rules on commit because two proposals judged against one state can both be appended. The
  rules share one author (`StandingOf` in `ClusterState.cpp`) so the two cannot drift.
- **A revoked key is never admitted again, and the refusal says PERMANENT.** `KeyRevoked` is
  its own `ConsensusErrorCode`, `RefusalSubject::Command` in `RefusalSubjects`, and maps to the
  generic permanent wire code (`InvalidClusterChange`) -- a code of its own would claim a
  client acts differently, and none does. `revokedKeys` keeps the WHOLE key and is never
  shortened.
  - **It holds per key BYTES, not per HOLDER.** A forgotten machine presenting its old key plus
    a torsion point, A0 + T, presents different bytes: the roster reads it as UNKNOWN, exactly
    like a freshly minted key, and it reaches admission only the way any new key does -- through
    an operator's approval of a key they were shown. Under the COFACTORED verify only the holder
    of a0 can sign under A0 + T, so it proves the same machine; recognising it would take a
    multiplication by L, and refusing it would grant nothing that moving `--cluster-dir` aside to
    mint a fresh identity does not already (the mixed-order clause of the crypto seam,
    `distributed-compilation.md`). Do not read this rule as "that machine can never come back".
- **A forget revokes, in the same entry, and there is no verb that only revokes (#1555).**
  `--cluster-forget=<id>` is `CommandKind::Forget` -- `RemoveMember`'s ordinal, widened: the id
  leaves whichever list records it, a member or a principal, and the key that record held is
  revoked. Two halves, one act, because each half alone is a state nobody asked for: the record
  gone with its key live is a machine every node whose `--raft-peer` types that key goes on
  accepting -- removal failing OPEN -- and a key revoked under a record that stays is a member
  the configuration goes on counting, so on the consensus wire the revocation never takes
  effect (next bullet but two). That second one is what the retired `RevokeKey` verb produced
  for a member, which is why it went rather than gaining an operator verb.
  - **The key is DERIVED at `Apply`**, from the record being removed, so a key replaced between
    the proposal and the commit is the one revoked.
  - **Beside it, the key the proposing LEADER holds live for the id** (`PrepareForget`, from
    `RosterKeys::KeysOf`). The state cannot derive it: a member a `--raft-peer` line typed with
    its key is recorded nowhere, or under another key, and that key lives on command lines. Revoked,
    it outranks every one of them (`RosterKeys` drops a typed key the state revoked). Never
    another id's -- `ValidateAgainst` refuses it by name and `Apply` skips it, or a forget of one
    machine would revoke one nobody named.
  - **Never dropped once there is a key to revoke.** Every admitting verb is dropped at commit
    when its preconditions have stopped holding; a dropped revocation is removal failing OPEN.
  - **On the Raft wire it takes effect when the CONFIGURATION drops the member, not at the
    commit.** `RosterKeys` keeps a key revoked under an id live for that id alone while this
    node's configuration counts it (`AdoptConfiguration`, fed every reconcile pass). Cut off at
    the revocation, a forgotten voter still counted is a voter that cannot vote for a pass, and
    a cluster losing its leader inside it can WEDGE for good -- four voters, one forgotten, the
    leader lost: two of four, nobody elected, nobody to propose the removal. `cluster-e2e`
    found it (forget n3, stop the leader) and `MembershipCluster_test` pins it with production
    `RosterKeys` per node; neutered, three of four never elect. Raft's own rule for a removed
    server, stated about keys. Everywhere else the revocation is immediate: the same key under
    another id, the enrollment door, discovery, and a principal, which consensus never counts.
    The pass that drops the member is also the one that re-asks every idle attached session
    (`RecheckProofs`, *A revoked key ends the sessions it proved*), so the grace ends on the wire
    in that pass rather than at a frame nobody will send.
  - **A forgotten member leaves the quorum whoever typed it.** The rule that a typed member is
    never removed is about ABSENCE, and a revocation under the id is the forget's own record,
    which a fresh leader's empty state cannot contain. It is load-bearing BECAUSE of the grace
    above: a member still counted keeps its key for itself, so a forgotten member this rule did
    not remove would go on voting for as long as it runs -- the forget failing open on the one
    wire it most concerns. Neutered, `MembershipCluster_test`'s forgotten typed follower stays
    counted. That fleet runs production `RosterKeys` per node, adopting each node's own state and
    configuration: its first version shared one fake roster that never adopted a revocation,
    which was more permissive than any node and could not reach the wedge the grace prevents.
  - **The ordinal did not move and neither did `CommandVersion`.** Every entry a released build
    wrote names a keyless member, which `Forget` applies exactly as `RemoveMember` did, and no
    released build can join a cluster that has keys (`RaftWire::MinSupportedVersion`).
  - A revoked key is refused at the ENROLLMENT door too, counted
    (`EnrollmentRequestsRefusedRevokedKey`), rather than listed for an approval that could not
    admit it -- see the distributed-compilation rules.
- **Absent is no opinion, again.** A member admission with no key KEEPS the recorded one --
  unlike the scheduler endpoint, a machine that moves keeps its identity -- and discovery,
  which has no opinion about a peer's key, can therefore never clear one. A node asserts its
  OWN key on its self record, as it asserts its own scheduler endpoint.
- **A recorded member holds a key BY TYPE** (`ClusterMember::publicKey`, `RosterMember::publicKey`,
  neither an `optional`): a machine is admitted and forgotten by its key, so a member without one
  is a record no forget could revoke. The type rules out ABSENT and not the all-zero key an
  omitted initializer yields, so what holds is narrower and exact: **a member or principal
  `Apply` recorded or a decoder read never holds an absent or all-zero key**
  (`IsZeroEd25519PublicKey`). `KeyToRecord` decides it for all three admitting verbs:
  `ValidateAgainst` refuses when it returns nothing, and `Apply` records exactly the key it
  returns. `DecodeState` and `DecodeRoster` each refuse such a member or principal by name.
  What an operator TYPES may still state no key, so a token is a `MemberSpec` --
  `--cluster-admit`'s, and the bootstrap set consensus starts from (`BootstrapMembersOf`) --
  and never a member.
- **One key, one identity; one id, one list.** A key held by another id is refused, and an id
  is a member or a principal, never both. `DecodeState` refuses a snapshot breaking either rule
  or holding a revoked key live -- the combinations `Apply` never produces.
- **`@<key>` rides the member token** (`<id>=<host>:<port>@<key>`, split at the first `@`), so
  `--raft-peer` states a member's key and a service registration re-renders it through
  `FormatMemberSpec`. On this node's OWN entry a key other than the one it holds is a startup
  refusal (`SelfKeyContradiction`), never quietly overwritten.
- **An admission CARRIES the key it parsed, or it must refuse it -- never parse and drop.**
  A key accepted at the flag and lost on the wire is a member admitted keyless while its
  operator believes otherwise. So CLUSTER-ADMIT and CLUSTER-ADMIT-LEARNER carry it as a THIRD
  field (0xFC version 11, `MinSupportedVersion` too), in its TEXT form; absent is a zero-length
  field read back DISENGAGED by the decoder and nowhere else. The LEADER parses it again, before
  it proposes: a malformed key is its own counted refusal
  (`ClusterAdmissionsRefusedMalformedKey`, a row sharing `InvalidClusterChange` with refusals
  that count nothing -- never `MalformedFrame`, since the frame decoded), and a revoked one is
  `ValidateAgainst`'s. The receipt echoes the key the COMMAND carries, re-spelled through the
  one encoder, and an absent one is printed as *none stated*, which KEEPS a recorded key --
  never a dash, which reads as *holds none*.
- **The formats moved, both, each refused by name.** `CommandVersion` 2→3 because the layout
  gained two fields (a key and a role), and a v2 command is judged by its VERSION before its
  arity, or an intact entry from before the upgrade reads as damage. `StateVersion` 5→6. A
  state at the previous version is `UnsupportedVersion`, never `MalformedFrame`, and the test
  builds it in the previous LAYOUT rather than flipping a byte of the current one -- a decoder
  that judged the arity first passes the flipped-byte test and fails this one.

## Open work

<!-- agent-tripwire: none: deferred work, tracked as GitHub issues; AGENT.md tripwires rules, not residuals -->

- **[#144](https://github.com/LASTRADA-Software/fastcached/issues/144)** — a
  follower answering `/fleet` names the leader but cannot link to it, because
  where a dashboard is served is local configuration and any URL it built would be
  a guess. A third recorded endpoint was priced and refused: a node ANNOUNCES it, so
  it rides in `AddMember` and moves `CommandVersion` — every existing log entry
  becomes undecodable, and a committed entry that will not decode is skipped — as
  well as `StateVersion`, which every snapshot carries. It stays open because the
  trigger is natural — whenever the COMMAND format next bumps for a reason that
  carries the migration on its own, this rides along. `StateVersion` moving alone is
  not that trigger: #1340 moved it for a fact `Apply` derives, which no command carries.
