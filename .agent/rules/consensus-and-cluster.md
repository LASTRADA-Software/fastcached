# Consensus and cluster membership

Rules for `src/FastCache/Consensus/` and `src/FastCache/Cluster/`: Raft itself,
the LAN discovery beacon and its pre-shared-key handshake, the Raft peer wire and the
handshake every connection on it proves each end's identity key with, the replicated
cluster configuration, and the admin verbs that change it.

Read this before touching `RaftNode`, `RaftLog`, `RaftDriver`, `RaftWire`,
`RaftPeerSession`, `RaftPeerTransport`/`RaftPeerServer`, `RaftPeerIdentity`, `RosterKeys`,
`RaftClusterHarness`, `DiscoveryService`, `PeerDirectory`,
`ClusterState`/`ClusterStateMachine` or `MembershipPolicy` — and before adding a
verb to the cluster-admin surface.

Every rule below has already been a bug.

## Discovery and the identity key

- **A discovery proof is a SIGNATURE by the node's OWN identity key, and discovery admits
  nobody (#178).** It was an HMAC under the pre-shared key until then, and a proof of
  possession of the fleet's key WAS membership, so any machine holding the file on the
  segment was desired and admitted. Now discovery broadcasts what a node *is* -- cluster,
  id, Raft endpoint -- and the challenge that follows is answered with an Ed25519
  signature over `(fastcache-discovery-proof-v2, cluster, nonce, id, endpoint, key)`, the
  key carried beside it. The consequences, each of which some plausible simpler design
  gets wrong:
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
  - **The proof signs a `(node, endpoint)` PAIR and the key, not the nonce alone.**
    Signing the nonce only would let anyone who observed one valid proof replay it
    with a *different* endpoint substituted -- pointing a known node id at an
    attacker's address. A member is assigned compile jobs and returns objects cached
    fleet-wide, so that is object injection into everybody's build. The key is inside
    the message so a signature cannot be re-attributed to another key.
  - **A proof is only ever an answer to a challenge THIS node issued**, and the
    nonce is spent whatever the outcome -- a forgery included, so a forger cannot race
    the honest answer for free. An unsolicited proof is refused *even when it is signed
    by a key the roster holds*: it answers a nonce nobody here chose, and accepting one
    would make the nonce -- and therefore the replay protection -- pointless.
  - **A peer that moves loses its authenticated bit.** The bit is a property of the
    node *at an endpoint*, not of the node, because that is what the proof covered.
    Carrying it across a change would admit an address nobody proved.
  - **The pending-challenge table is one entry per node, with a lifetime.** A
    beacon is unauthenticated by construction, so anything on the segment can
    provoke a challenge -- a table that grew per datagram would be a
    memory-exhaustion hole reachable without holding any key, which is the same
    shape as the pre-auth payload cap on the `0xFC` port.
  - **Discovery never changes membership.** It answers which known members proved
    their keys and where they answer; a caller proposes. A membership change is a
    Raft decision only a leader may make, and a layer that proposed directly would
    have every node on the segment proposing the same change at once.
  - **A peer this node cannot NAME is a peer it does not remember.** `NoteBeacon`
    refuses an id or endpoint that is empty or is not valid UTF-8, alongside the
    wrong-cluster and own-beacon filters, because what the directory holds is what
    is eventually proposed as a `ClusterMember` -- and every surface reads that back
    out as text (#159). Filtered here rather than at any later layer, which is what
    keeps a permanently-refusable proposal from ever being generated; it also keeps
    such a peer out of the challenge table, out of `Peers()`, and out of the line
    logged when a peer proves its key. The beacon's *cluster id* is deliberately
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
  pre-shared key MACs a lease token and a member's proof on the `0xFC` surface -- and
  MACed a discovery proof until #178, and every Raft peer connection from #1308 until #178,
  both moved to identity keys -- and the lease and the discovery proof each used to build
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
    goes through it** -- `AuthenticateLeaseToken` for the lease and `VerifyNodeProof` for
    the `0xFC` surface's, and `DiscoveryWire::VerifyProofTag` for the discovery proof
    until #178 retired it. That discovery one is the whole
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

- **The shape, because every other rule refers to it.** Connections are one-way: the
  transport only writes, the server only reads, and a reply travels on the other node's
  own outbound connection. So authentication is a short two-way prologue on an otherwise
  one-way stream (`Consensus/RaftPeerSession.hpp`):
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
  with each end. `RaftPeerSession_test` changes each of the proof's six transcript fields in
  transit and requires each change to be refused, so dropping any one of them from what is
  signed turns that field's section red. The verdict carries the proof's signature, so the
  dialler's fresh values reach it three ways at once; the field a single-field test can see
  there is the verdict byte, and a signed refusal flipped to `Accepted` is its case.

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
  `NonceBytes` (32, `static_assert`ed at least that) and `DrawNonce`, and discovery's
  challenge and the node proof's draw through the same helper -- a second draw site would be a
  second answer to how big a nonce is.

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
    proving, discovery withholds its challenge (`ChallengeWithheld`), the node-proof surface
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

- **A revoked key ends the sessions it proved, at their next frame -- PULLED, not pushed.**
  Both ends re-ask the roster (`IRaftPeerIdentity::StillProves`) for every frame: the acceptor
  after the tag verifies, the dialler before it seals. So an applied forget -- or a
  re-admission under another key -- closes the session at its next frame
  (`connections_ended_key_withdrawn`, `dials_ended_key_withdrawn`), and the redial is judged
  against the roster as it is then. A push from the state machine would be a close from the
  apply thread on a connection the reactor owns; a Raft peer is never quiet for longer than a
  heartbeat, so the pull costs no latency worth a thread hazard. Over TCP a dialler whose
  acceptor closed learns so from its next write's reset; the in-memory socket accepts writes
  nobody reads, so the link and node cases READDRESS to force the redial and say why.

- **The roster is the command line until the cluster says anything, and then the cluster.**
  `Cluster::RosterKeys` answers from `--raft-peer`'s `@<key>`, then from `ClusterState`, which
  WINS wherever it states a key: a member re-admitted under a new key proves itself with the
  new one whatever a command line typed a year ago. **A bootstrap key the state has revoked is
  revoked, whatever the command line says** -- a restart with the original command line must
  not bring it back, which would be removal failing open. A member the state records with no
  key falls back to its bootstrap key only when that key is not revoked. A principal is a
  stranger here: it never joins consensus. So a fresh cluster whose members were given no
  keys cannot FORM, and says so in `connections_refused_unknown_key` rather than trusting
  whoever answers first; `--print-identity` is how an operator gets every member's token
  before any of them starts, minting into the state directory the start then reads.

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
  configuration reaches that refusal, because a consensus node always has a state directory
  (`HoldsNodeKey`, asserted over every consensus shape). The key is resolved by the START and
  PASSED to the tier, never read twice, so the key a node announces and the key it proves
  itself with are one reading of one file. The server and transport take the identity and an
  `ISecureRandom` as REQUIRED constructor arguments -- no default and no null -- and their own
  id IS the identity's, never a parameter beside it that could name somebody the proofs do
  not. `ConsensusNeedsClusterKeyRefusal` stays in the startup table for reasons that MOVED:
  the tier no longer reads the cluster key, and neither do discovery nor enrollment since #178
  PR 4, but a consensus node still signs leases and proves itself on the node port with it,
  and #178 retires it surface by surface. "No key, so skip the check" is the shape the worker's lease rule (#282)
  refuses one surface over: the port open, every refusal counter at zero, and the fleet
  healthy-looking from both ends.

- **Layering decides where the seam sits.** `Cluster/` already includes `Consensus/`
  headers, so `Consensus/` cannot read `ClusterState`. It states what it needs instead:
  `Consensus::IRaftPeerKeys` -- this node's own key, which signs and never leaves it, and the
  roster's keys for a peer id, NOW -- and `Consensus::IRaftPeerIdentity` (`Self`, `Sign`,
  `Verify`, `StillProves`) over it, whose one implementation is `RaftPeerIdentity`.
  `Cluster::RosterKeys` implements the keys over `ClusterState` and the command line. The two
  signatures are labelled by `RaftPeerSignatureLabels` (`fastcache-raft-proof-v2`,
  `-verdict-v2`), `static_assert`ed present and distinct, so a proof reflected back as a
  verdict verifies as nothing; the session key's HKDF label is `fastcache-raft-session-v2`.
  The pre-shared key's three Raft rows left `SigningDomainTable`, and their `-v1` labels are
  retired rather than reused. Frame sealing is `Core/SessionSeal`, not Raft's, because the
  `0xFC` wire needs it next. The session code never compares a tag itself.

- **One refusal, one row -- at BOTH ends, because one misconfigured machine shows on two.**
  Every refusal is a row of `Consensus/RaftPeerRefusals.hpp` beside its log sentence, and the
  key refusals are apart by REMEDY: `unknown_key` (a key never given), `proof` (somebody
  signing for an id whose key it does not hold), `revoked_key` (the removed machine, still
  dialling), `ended_key_withdrawn` (a session the roster outlived), and at the dialler
  `acceptor_key_unknown`, `acceptor_key_revoked`, `own_key_revoked` and `ended_key_withdrawn`.
  A peer that sent NOTHING and closed asked nothing, so it is closed and NOT counted. Refusals
  before authentication are logged at most once per 60 s and name only the source address,
  except a revoked key, which names the WHOLE key, since that is what an operator matches
  against the revocation they made; refusals after authentication name the proven ids,
  unthrottled; a dialler's refusals are throttled per peer.

- **What became false was removed, not kept** (the project's position on superseded
  code). `EnrollmentConfigured`'s key clause could no longer decide anything, so the
  predicate is gone and `ServesEnrollment` asks `RunsConsensus`; `EnrollmentResponder`
  still reads the key before `ClusterAdmit`, because a key file readable at boot can break
  later. `NodeMembership`'s refusal to let a replicated `fleet-open` WIDEN a keyless node is
  gone too: cluster state reaches only a consensus node, and every consensus node holds the
  key its lease check verifies with. The RELOAD guard in `ValidateNodeReloadable` stays,
  because a node that runs no consensus can still be keyless and be opened by its operator.
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
  - **A recovered snapshot reaches the APPLICATION, and constructing the driver is
    what hands it over**
    ([#1542](https://github.com/LASTRADA-Software/fastcached/issues/1542)). A node
    recovered from storage comes back with `_lastApplied` AT the snapshot's boundary,
    so nothing the snapshot covers is ever applied again -- and `RestoreSnapshot` was
    called only on `output.restoreSnapshot`, which only `OnInstallSnapshot` sets. So a
    node that compacted (every 512 entries) and restarted ran without every cluster
    fact its snapshot held: members, settings, and the forget tombstones, which made
    REMOVAL fail OPEN -- a forgotten host admitted again after a restart, reported by
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
    current state* -- and the node then RAN with an empty `ClusterState`, tombstones
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
    elects itself and becomes a second cluster. A LEADER's snapshot the application
    cannot read is the install path's own question, deliberately left as it was; its
    log line now says *installed* and a recovered one says *recovered*.
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
  - **A forget outranks an observation**
    ([#1528](https://github.com/LASTRADA-Software/fastcached/issues/1528)). Everything
    the reconciler is handed is an OBSERVATION -- a peer proved the key, this node knows
    its own record -- and `--cluster-forget` leaves the machine running with the key, so
    discovery proves it again at its next beacon. `ConsensusTier::Desire` never prunes,
    `MembershipProposals` proposed every desired id the state lacked, and `AddMember`
    lifts the tombstone for the host it admits at: the leader re-recorded the forgotten
    member on the very next pass, tombstone gone, and the quorum flapped -- removed on
    one pass, re-added on the next. Inferred from reading, then REPRODUCED at the policy
    before the fix. So `MembershipProposals` refuses a desire at a forgotten host --
    by NAME, into `MembershipPlan::forgotten`, which the tier logs once per member,
    because a refused desire and one the state already matches both propose nothing.
    **Refused at the decision, not by pruning the desire**: discovery hands it back at
    the next proof for as long as the machine holds the key. The predicate is `Apply`'s
    own (`HasForgotten` over `HostOfEndpoint`, through `SameHost`), asked only of a desire
    that would propose something, and it covers this node's OWN record. Lifting a
    tombstone is the operator's: `--cluster-admit` commits `AddMember` directly. The
    tombstone is a HOST, so a loopback cluster -- which records none -- is not covered,
    and #178's per-node keys are what replace it with an identity.
  - **A forget means the same thing whoever currently leads**
    ([#1539](https://github.com/LASTRADA-Software/fastcached/issues/1539)). After #1528 a
    forgotten LEADER stopped recording itself and still led, counted, indefinitely:
    `NextQuorumChange` skipped `id == self`, and a node's own bootstrap set always names
    it. Now a leader that is FORGOTTEN proposes its own removal, LAST -- after every
    change it can still make as the leader -- and steps down once it commits (`RaftNode`,
    §4.2.2; #1449's demoted leader is the same rule). **Forgotten is the record gone
    AND a fact only `Forget` writes beside it** -- the host tombstoned, or (#1555) the
    key revoked under the id, which is what reaches a rig sharing one machine over
    loopback: record absence alone is every fresh leader's first pass, and a tombstone
    alone may be a client forget naming a member's machine. Neutered to absence alone, five cases go red,
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

[#178](https://github.com/LASTRADA-Software/fastcached/issues/178) PR 2: every node with a
state directory holds an Ed25519 identity key, and `ClusterState` records members' keys,
principals admitted by key, and keys revoked for good. PR 3 made the Raft peer wire VERIFY
against them (see *The Raft peer wire*), and PR 4 moved discovery and enrollment onto them
(see *Discovery and the identity key*, and the enrollment window in
`distributed-compilation.md`); the `0xFC` surface and leases still use the cluster key until
later PRs move them. Every rule below is about getting the record
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
- **A forget revokes, in the same entry, and there is no verb that only revokes (#1555).**
  `--cluster-forget=<id>` is `CommandKind::Forget` -- `RemoveMember`'s ordinal, widened: the id
  leaves whichever list records it, a member or a principal, and the key that record held is
  revoked. Two halves, one act, because each half alone is a state nobody asked for: the record
  gone with its key live is a machine every node whose `--raft-peer` types that key goes on
  accepting -- removal failing OPEN -- and a key revoked under a record that stays is a member
  the configuration goes on counting, so on the consensus wire the revocation never takes
  effect (next bullet but two). That second one is what the retired `RevokeKey` verb produced
  for a member, which is why it went rather than gaining an operator verb.
  - **The key is DERIVED at `Apply`**, from the record being removed, as the host tombstone is
    (#1309), so a key replaced between the proposal and the commit is the one revoked.
  - **Beside it, the key the proposing LEADER holds live for the id** (`PrepareForget`, from
    `RosterKeys::KeysOf`). The state cannot derive it: a member a `--raft-peer` line typed with
    its key is recorded without one or not at all, and its key lives on command lines. Revoked,
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
