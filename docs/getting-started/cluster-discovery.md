# Cluster discovery

How `fastcache-compile-node` machines find one another and become one fleet with no
configuration, and how one proves which machine it is before anything it claims is
believed.

It is on by default, beside consensus: every node beacons on port `6681` (UDP), and an
empty `--discovery=` turns it off. A discovery proof is a signature by the node's OWN
identity key, the same key every consensus connection proves — see
[Raft peer authentication](../operations/cluster-communication.md#raft-peer-authentication).
The flags are under
[finding peers instead of typing them](../tools/fastcache-compile-node.md#finding-peers-instead-of-typing-them);
where this exchange sits among everything else a fleet says to itself — and which ports
it needs open — is [Cluster communication](../operations/cluster-communication.md).

## Zero-config formation, start to finish

**Every machine starts as a fleet of its own.** Installed with no configuration, a node
founds a solitary cluster at its first start — its own voter, its own scheduler, its own
worker — and serves at once. Nobody waits for a fleet to exist. The cluster id, the node
id and the identity key are minted into its state directory and read back forever.

**Then it looks for a fleet to join**, from four sources, and the order matters:

| Source | What it is | Preference |
|---|---|---|
| `--fleet-seed` | `<host>` or `<host>:<port>`, typed at install (on Windows the installer's `FASTCACHE_FLEET_SEED`) — what reaches across a VPN, where no beacon does | first: asked before anything is decided, whatever a beacon says |
| remembered | the fleet's voters' endpoints, kept in the state directory after a join | second |
| DNS SRV | `_fastcache._tcp.<this machine's DNS suffix>`, when the machine has one | third |
| the LAN beacon | every node broadcasts its fleet summary every 15 seconds on every eligible interface's directed broadcast | last: anything on the segment can beacon |

The three seed sources are dialled in `SeedSourceTable`'s order — remembered, then
`--fleet-seed`, then DNS SRV — and each is asked over TCP for a signed **fleet summary**
(`FLEET-SUMMARY`); a typed seed is asked first of all, one per beat, before any fleet is
chosen. Which fleet a node then asks is ranked by where it was found first
(`--fleet-seed`, remembered, DNS SRV, beacon), then by age.

**Who yields is decided by one table, the same on every machine** (`ClassifyEncounter`):

| This node | The node it found | Outcome |
|---|---|---|
| solitary | an established fleet | this node yields: it asks to join |
| solitary | another solitary node | the **younger** yields — the older creation time wins, then the lower cluster id — so exactly one of two machines started apart asks the other |
| established | an established fleet | **neither yields**: `foreign-fleet-visible` is raised on both, naming the other fleet, and an operator decides |
| any | a pending node | it is a pointer, never a fleet: this node asks the fleet the pending one asked |
| pinned by `--fleet-id` | any fleet the pin does not admit | it stays, and `foreign-fleet-visible` names the fleet it refused |

An **established fleet never yields**, even to a larger or older one: a machine that has
joined a fleet stays in it until an operator forgets it. The one exception is a fleet
healing a split of *itself*, decided on evidence a voter's key this fleet already held
verifies — see the rulebook's *A split heals BY ITSELF only on a VOTER's key*.

**The yielding node asks, and keeps serving while it waits.** It goes *pending*: it asks
the fleet's leader to admit it (`Enroll`, signed by its own identity key, following a
`not-leader` redirect to wherever the leader is now), and polls. Its own cluster of one
goes on serving its builds meanwhile, and its beacon now points at the fleet it asked, so
a third machine that finds it asks that fleet too. A join nobody answers for ten minutes is
given up and asked again later; a rejection keeps it from asking that fleet for an hour.

**A person approves it** — on the fleet's leader, or with `--scheduler` naming it:

```sh
fastcache-compile-node --enroll-list
fastcache-compile-node --enroll-approve=<id>@<key>
```

`--enroll-list` prints each waiting machine with the key it asked with and the paste-ready
approval line; compare the key with what the joiner's `--print-identity` printed. On a day
when many machines join, an operator can delegate that comparison for a bounded time
instead:

```sh
fastcache-compile-node --enroll-auto-approve=15min
fastcache-compile-node --enroll-auto-approve=off
```

The deadline lives in the leader's memory alone — at most `24h`, refused by name above
that — so a restart or a change of leader ends it. `enrollment-window-open` is raised while
it is armed, `fastcache-cli node` reads `auto-approve (N min left)`, `--enroll-list` marks every
row it admitted with when it was armed, and
`fastcache_enrollment_approvals_auto_total` counts them apart from a person's approvals.
A machine that asks again under the same id with a different key is never auto-approved.
Details are under
[enrolling a machine instead of typing it](../tools/fastcache-compile-node.md#enrolling-a-machine-instead-of-typing-it).

**Approved, the joiner dissolves its own cluster and becomes a learner.** It records the
fleet it joined, archives its solitary cluster's consensus store under
`archive/<cluster-id>/` in its state directory — never deletes it — adopts the approved
roster and restarts its serving body as a **learner** of the fleet: it applies the fleet's
replicated state, verifies every lease against it, registers its worker with the fleet's
scheduler, and counts towards no quorum. A learner opens no consensus port; it dials every
voter, and the voter writes to it over that connection. Its identity key and node id are
kept, so it is the same machine to the fleet.

**Voting stays an operator's decision.** A fleet formed this way has ONE voter — the
machine that founded it — until somebody promotes another, with the id and address
`--cluster-status` shows:

```sh
fastcache-compile-node --cluster-admit=<id>=<host>:6680
```

A promotion takes effect once the learner has caught up. Promote machines that stay on —
two more for three voters, which survive losing one — and leave laptops as learners; see
[a machine that is usually away](../tools/fastcache-compile-node.md#a-machine-that-is-usually-away-admitting-a-learner).

## What problem the beacon solves

A fleet has to be found before it can be joined, and an address list on every machine is
exactly the configuration a zero-config fleet does without. Raft membership names a member
by **id** and carries no address — deliberately, so that agreeing *who* is in the cluster
stays independent of *where* they are — so the beacon also supplies the address a known
member now answers at.

**It supplies addresses, never admissions**
([#178](https://github.com/LASTRADA-Software/fastcached/issues/178)). A machine whose key
the roster does not hold proves it perfectly well and is still only *reported*: counted,
and logged with its id, the address it came from, its key and the `--cluster-admit` that
would admit it. Joining is the enrollment above. Under the retired shared key it was not: a
proof then showed possession of the fleet's key, possession was membership, and any machine
holding the file on the right segment was admitted. That is what ended.

## The exchange

```
     ┌───────┐                                        ┌───────┐
     │ node  │                                        │ peer  │
     └───┬───┘                                        └───┬───┘
         │  ── beacon (broadcast) ─────────────────────►  │  its fleet summary
         │                                                │  nothing that proves anything
         │  ◄──────────────── challenge (unicast) ──────  │  a 32-byte cookie only the
         │                                                │  challenger can recompute
         │  ── proof (unicast) ────────────────────────►  │  the summary, the cookie, its public key,
         │                                                │  and an Ed25519 signature by that key over
         │                                                │  (label, challenger's cluster, cookie,
         │                                                │   summary, key)
         │                 [signature verifies]
         │                  same cluster -> [roster holds that key for that id?]
         │                                    yes: may be desired   no: reported
         │                  another cluster -> handed to formation, never desired
```

Five properties are worth reading off that diagram, because each is what some plausible
simpler design gets wrong.

**A beacon is an invitation to ask, not a credential.** It carries the node's fleet
summary — cluster id, whether the fleet is solitary, pending or established, when it was
created, its leader, the node's id and consensus endpoint, up to ten member ids with the
total, and the node's own `0xFC` endpoint — and nothing that proves any of it.

**The signature is checked before any claim is looked at.** Until it verifies, the key the
proof carries is as much a claim as the id, and naming it in a log would be naming bytes
anybody could have sent. A proof that does not verify is counted as *forged* and logged by
the address it came from, and nothing it claimed is repeated.

**The proof signs the whole summary and the key, not just the nonce.** Signing the nonce
alone would let anyone who observed one valid proof replay it with a *different* endpoint
substituted — pointing a known member's id at an attacker's address, which is object
injection into everybody's build. And the summary is what formation decides who yields on,
so every field of it a node acts on is signed: a field outside the signature is one a relay
could rewrite into a yield. The key is inside the message so a signature cannot be
re-attributed to another key, and the message carries a label of its own
(`fastcache-discovery-proof-v3`) so it can never verify as any other construction.

**A challenge is a cookie, answerable once.** The challenger keeps no table of outstanding
challenges — anything on the segment could grow one — but recomputes the cookie from what
the proof claims and the address it arrived from, so a proof answers only a challenge this
node sent, to that address. A challenge is spent once a proof of it **verifies**; a forgery
spends nothing, so nobody who saw a challenge go out can destroy its honest answer. An
unsolicited proof is refused *even when it is signed by a key the roster holds*.

**A node answers a challenge from any cluster.** A solitary machine must see a fleet to
yield to it, and an established one must see a foreign fleet to raise
`foreign-fleet-visible`. What it signs is its public summary, the bytes its beacon already
shouts, so answering announces nothing a listener did not have. Another cluster's proof is
judged by its signature alone and handed to formation; this cluster's roster says nothing
about another's keys.

## Two sockets, not one

A node listens for beacons where every other node on the segment does — port `6681`,
bound on the wildcard and **shared**, because a beacon is a broadcast and a node listening
anywhere else would send perfectly and hear nothing.

It does not *answer* there. Two sockets on one UDP port both receive what is broadcast to
it, and only **one** receives what is unicast to it — measured, Windows 11 hands a unicast
to the first-bound socket and Linux to the last, so there is no behaviour to rely on. Since
the challenge and the proof are both unicast back to wherever the previous datagram came
from, a node answering out of the shared socket would be answering for its whole
*machine*. Two nodes on one host therefore saw each other's beacons and never finished the
handshake, with nothing logged, because every rejection along the way is one the protocol
is supposed to make ([#126](https://github.com/LASTRADA-Software/fastcached/issues/126)).

So every node holds two:

| socket | binds | role |
|---|---|---|
| listener | port `6681` (the `--discovery` port), shared | hears beacons; never sends |
| answering | a port only this node holds | sends everything; receives challenges and proofs |

Every datagram then leaves from an address exactly one node holds, so the sender a peer
replies to names a node rather than a host.

**What this means for a firewall.** Challenges and proofs arrive on the answering port,
not on `6681`. A rule scoped to the program covers it; a rule scoped to `udp/6681` alone
does not, and the symptom is peers that are discovered and never authenticated. The
answering port is kernel-chosen by default; `--discovery-reply-port` pins it where a site
needs to name it — one port per node on the machine, since two nodes cannot share it. The
startup line reports both addresses. On Windows, `--install-service` creates the
program-scoped rule itself (`… discovery-reply udp/any`), or a rule for the pinned port.

## The wire version, and why it moved

The discovery wire is **version 3**, and versions 1 and 2 are refused. Each move was a
GRAMMAR change, which an older reader could only refuse as malformed:

- **2** replaced the proof's 32-byte HMAC under the shared key with a 32-byte public key and
  a 64-byte signature, so its arity and its field widths changed. That is the opposite of
  [#402](https://github.com/LASTRADA-Software/fastcached/issues/402), which changed only
  what the MAC covered and rightly left the version alone.
- **3** put the fleet's signed summary in the beacon and the proof where an id and an
  endpoint were, padded the beacon and the challenge by one field each, and made the proof
  echo the nonce it answers.

A datagram of another version is refused on its version byte, and refused **silently**: no
counter and no log line, so a node on an older build and one on this build simply never see
each other on a shared segment. Upgrade a segment's nodes together.

## What it deliberately does not do

**It does not change membership.** Discovery answers "which known members proved their
keys, and where do they answer", and hands another fleet's summary to formation. A caller
decides what to propose. A membership change is a Raft decision only a leader may make, and
a discovery layer that proposed directly would have every node on the segment proposing the
same change at once.

**It does not state a key.** A discovered member is desired with *no opinion* about its
key: the only key discovery authenticates is the one the roster already holds, and a
desire outlives the moment it was stated — so a key named there would be proposed straight
back over an operator who later re-keyed that member. And the authenticated set is
re-asked of the roster every time it is published, so a proof taken before a revocation is
never handed on after it.

**It does not treat "seen" as "trusted".** Those are separate facts in `PeerDirectory`,
and only a completed handshake under a key the roster holds sets the second. Nor does a
proof here stand in for the consensus wire's own: a member still proves its key again on
every Raft connection it opens or accepts. A peer that changes the endpoint it advertises
**loses** its authenticated status: the proof covered the old endpoint, so carrying it
across would admit an address nobody proved.

**It does not remove anybody.** A peer vanishes from a broadcast for reasons that are
almost never "it left" — a lost datagram, a switch rebooting, a laptop closed for an hour —
so absence proposes nothing, and a learner is never removed for being absent. Removing a
machine is `--cluster-forget`.

**It does not promise delivery.** Beacons are broadcasts and loss is expected. A peer is
remembered for well over a beacon interval, so a lost datagram costs nothing.

## Threat model

| Attacker can… | Outcome |
|---|---|
| Listen to the segment | Learns that fleets exist, which endpoints serve them, their member ids and the public keys their members hold. None of that admits anybody. |
| Send arbitrary datagrams | Can provoke a challenge, and answer it under a key of its own — which is reported and never desired. Cannot grow a table without bound: there is no challenge table, every table a beacon can grow is bounded, and a displacement is counted (`fastcache_discovery_beacons_over_bound_total`). Cannot flood the log: an unaccepted key is reported at most once a minute, with how many it stands for, and every one is counted. Cannot make a node an amplifier: no reply is larger than the datagram that provoked it, and answers are rate-limited per source. |
| Replay a captured proof | Refused: the cookie it answers has been spent, or was issued to another address. |
| Capture a proof and re-aim it at another endpoint | Refused: the endpoint is inside the signature. |
| Claim a known member's id | Reported as an unknown key: the id is a label, and the key the roster holds for it is the credential. |
| Obtain a shared cluster key | There is none: every node proves its own identity key. |
| Obtain one member's identity key | That member's address can be moved by a proof — the same exposure as that member's consensus connections, and ended by revoking the key, which nothing on any other machine has to change for. |
| Mint a fleet of its own on the segment | Every unpinned solitary node that hears it may ask to join it — trust on first use, below. |

A proof under an unknown or a revoked key on a healthy segment is a machine nobody
admitted: a new install waiting to be enrolled, or one that was removed. The warning says
which, names the key whole — it verified, so only its holder could have signed it — and for
an unknown key gives the `--cluster-admit` an operator would run if it belongs. The three
refusals are counted apart
(`fastcache_discovery_proofs_refused_{unknown_key,revoked_key,forged}_total`), because
their remedies are different.

### Threat model: LAN discovery is trust-on-first-use

Everything above is about a member of one cluster proving itself to that cluster. Joining a
fleet is a different question, and on a LAN it is answered by **trust on first use**: a
solitary node yields to the fleet it proves older or established, and a proof binds a KEY,
never the truth of the summary it signs. A key costs nothing to mint, so anybody on the
segment can prove "established, created at 0" and be asked to admit every solitary node that
hears it -- and a node it approves takes on its roster, dispatching its launchers' source to
that fleet's workers and compiling that fleet's jobs.

That exposure is bounded, and the bounds are deliberate:

- **A typed seed outranks every beacon.** A `--fleet-seed` given at install is asked before
  any fleet is chosen, and a fleet found through it wins over one found by beacon whatever
  their ages.
- **Trust on first use is for the FIRST join only.** A machine already in a fleet never
  yields to another one: another established fleet raises `foreign-fleet-visible` instead,
  and the machine leaves its fleet only when an operator forgets it (which archives its store
  and mints it a new cluster of its own).
- **A key pin closes the first join itself**, below.

**A key pin stops a LAN impostor; an unpinned node trusts on first use.** The pin names the
cluster and its voters' identity keys (`--fleet-id=<cluster-id>@<key>[,<key>...]`), because a
cluster id is a name every beacon carries: what a pinned node accepts on its way into its fleet
is what one of those keys signed. An id with no key is refused where it is typed, never read
as a pin.

| Attacker can… | Unpinned | Pinned (`--fleet-id`) |
|---|---|---|
| Mint a fleet and prove it older | Every solitary node that hears it asks to join | Not asked: counted, and raised as `foreign-fleet-visible` |
| Claim the pinned fleet's id under its own key, by beacon or as a seed | Joined, exactly as above | Not asked: counted, and raised naming the impostor's key |
| Answer the node's `Enroll`, or redirect it to a leader of its choosing | Its approval is taken | Refused unless a pinned voter signed it; a leader whose key is not pinned is never asked, and the refusal names the key to add |
| Hold a pinned voter's identity key | -- | That voter's exposure everywhere: revoke the key and re-pin |
| Capture the fleet's quorum after the node joined | Everything any member's quorum gets | The same: the pin is not asked after the join |

**The pin anchors the JOIN; from then on the fleet's applied state is the authority.** A pinned
node asks its pin of every way INTO a fleet -- the summary it yields to, every answer to its ask,
the leader a redirect names, a dissolve, and its record at every start, judged by the key that
signed its admission -- and of nothing once it has joined: it follows its fleet's leaders, takes
the roster the fleet commits, and is granted work by the voters that roster names, pinned or not,
as any member is. That is deliberate, since pinning every leader forever would make every
promotion an outage for every pinned machine, and it bounds what the pin protects: an attacker
who captures the fleet's quorum after a node joined -- its voters' keys, or an approval an
operator should not have given -- gets everything a member's quorum gets, the pinned nodes
included.

The pin is the operator's remedy for exactly this, one setting per machine, while zero-config
formation stays the default: see
[pinning a node to one fleet](../tools/fastcache-compile-node.md#pinning-a-node-to-one-fleet).
`fastcache-cli node` prints the pin to paste (`fleet-id`), so nobody composes keys by hand.

## Implementation notes

| Piece | Where | Nature |
|---|---|---|
| Beacon / challenge / proof wire | `Cluster/DiscoveryWire.hpp` | Header-only, versioned, length-prefixed |
| Who is known, who is proved | `Cluster/PeerDirectory` | Pure; time via `IClock` |
| The exchange, and the challenge cookies | `Cluster/DiscoveryService`, `Cluster/ChallengeCookies` | Drives the above over core-cpp's `core::net::IDatagramSocket` |
| Every bound a beacon can push against | `Cluster/DiscoveryBounds.hpp` | One table |
| Who yields to whom | `Cluster/Encounter.hpp` (`ClassifyEncounter`) | Pure, one table |
| How a node moves between modes | `Cluster/FormationTransitions.hpp`, `Cluster/NodeMode.hpp` | Tables: one row per transition, one per mode |
| The seed sources | `Cluster/SeedSources.hpp` (`SeedSourceTable`, `OrderSeeds`) | Pure; DNS through `ISrvResolver` |
| The pin | `Cluster/FleetPin.hpp` (`AdmitsFleet`) | Pure, one predicate |
| Whose key is whose | `Consensus::IRaftPeerKeys` | The roster the Raft peer wire reads, and this node's own key |
| Datagram I/O | core-cpp's `core::net` UDP socket, and `core::net::testing::DatagramBus` | Real socket, and a whole segment in one process |
| The socket pair | core-cpp's `core/net/SharedPortDatagram.hpp` | Listens shared, answers private; one `IDatagramSocket` |
| Ed25519 | `Core/Ed25519` | RFC 8032 vectors |

`DiscoveryService::PumpOnce` is synchronous, so an entire segment forming a cluster —
including scripted packet loss and a hostile peer — is a loop in a unit test rather than
several processes and a sleep; `RaftFormation_test` and `RaftSplitHeal_test` drive whole
clusters forming, dissolving and healing a split over `RaftClusterHarness`, in one process.
