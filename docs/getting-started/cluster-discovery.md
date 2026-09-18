# Cluster discovery

How the nodes of a `fastcache-compile-node` cluster find each other on a LAN, and
how one proves which member it is before anything it claims is believed.

It is off unless you ask for it. Turn it on with `--discovery`, which needs
`--listen-raft` and is refused without it; without it a cluster is exactly the
`--raft-peer` list an operator typed, which works and is the right answer for a fleet
that does not change. A discovery proof is a signature by the node's OWN identity key,
the same key every consensus connection proves — see
[Raft peer authentication](../operations/cluster-communication.md#raft-peer-authentication).
The flags, and what a deployment looks like end to end, are under
[finding peers instead of typing them](../tools/fastcache-compile-node.md#finding-peers-instead-of-typing-them);
where this exchange sits among everything else a fleet says to itself — and which
ports it needs open — is
[Cluster communication](../operations/cluster-communication.md).

## What problem it solves

Raft membership names a member by **id**. It carries no address — deliberately, so
that agreeing *who* is in the cluster stays independent of *where* they are. The
consequence is that a member whose address changed is unreachable until something
supplies the new one.

Discovery is that something. A node announces itself on the segment, proves which
key it holds, and — when that is the key the cluster's roster holds for its id — the
endpoint it proved for is what the cluster then dials.

**It supplies addresses, never admissions**
([#178](https://github.com/LASTRADA-Software/fastcached/issues/178)). A machine whose
key the roster does not hold proves it perfectly well and is still only *reported*:
counted, and logged with its id, the address it came from, its key and the
`--cluster-admit` that would admit it. Admitting a machine is an operator's act —
[an enrollment](../tools/fastcache-compile-node.md#enrolling-a-machine-instead-of-typing-it)
or `--cluster-admit ...@<key>`. Under the shared key it was not: a proof then showed
possession of the fleet's key, possession was membership, and any machine holding the
file on the right segment was admitted. That is what ended.

## The exchange

```
     ┌───────┐                                        ┌───────┐
     │ node  │                                        │ peer  │
     └───┬───┘                                        └───┬───┘
         │  ── beacon (broadcast) ─────────────────────►  │  cluster, id, endpoint
         │                                                │  nothing that proves anything
         │  ◄──────────────── challenge (unicast) ──────  │  a fresh 32-byte nonce
         │                                                │
         │  ── proof (unicast) ────────────────────────►  │  id, endpoint, public key, and an
         │                                                │  Ed25519 signature by that key over
         │                                                │  (label, cluster, nonce, id, endpoint, key)
         │                                                │
         │                 [signature verifies] -> [roster holds that key for that id?]
         │                     yes: may be desired   no: reported (unknown, or revoked)
```

Four properties are worth reading off that diagram, because each is what some
plausible simpler design gets wrong.

**A beacon is an invitation to ask, not a credential.** It carries the cluster id,
the node id and the Raft endpoint, and nothing else.

**The signature is checked before any claim is looked at.** Until it verifies, the key
the proof carries is as much a claim as the id, and naming it in a log would be naming
bytes anybody could have sent. A proof that does not verify is counted as *forged* and
logged by the address it came from, and nothing it claimed is repeated.

**The proof signs a `(node, endpoint)` pair and the key, not just the nonce.** Signing
the nonce alone would let anyone who observed one valid proof replay it with a
*different* endpoint substituted — pointing a known member's id at an attacker's
address. Since a member is assigned compile jobs and returns objects that are cached
fleet-wide, that is object injection into everybody's build. The key is inside the
message so a signature cannot be re-attributed to another key, and the message carries
a label of its own (`fastcache-discovery-proof-v2`) so it can never verify as a Raft
handshake signature or the reverse.

**A challenge is answerable once.** The nonce is spent when the proof arrives,
valid or not, and an unsolicited proof is refused *even when it is signed by a key the
roster holds* — it answers a nonce nobody here chose.

## Two sockets, not one

A node listens for beacons where every other node on the segment does — the port
`--discovery` names, bound on the wildcard and **shared**, because a beacon is a
broadcast and a node listening anywhere else would send perfectly and hear
nothing.

It does not *answer* there. Two sockets on one UDP port both receive what is
broadcast to it, and only **one** receives what is unicast to it — measured,
Windows 11 hands a unicast to the first-bound socket and Linux to the last, so
there is no behaviour to rely on. Since the challenge and the proof are both
unicast back to wherever the previous datagram came from, a node answering out of
the shared socket would be answering for its whole *machine*. Two nodes on one
host therefore saw each other's beacons and never finished the handshake, with
nothing logged, because every rejection along the way is one the protocol is
supposed to make ([#126](https://github.com/LASTRADA-Software/fastcached/issues/126)).

So every node holds two:

| socket | binds | role |
|---|---|---|
| listener | the `--discovery` port, shared | hears beacons; never sends |
| answering | a port only this node holds | sends everything; receives challenges and proofs |

Every datagram then leaves from an address exactly one node holds, so the sender
a peer replies to names a node rather than a host.

**What this means for a firewall.** Challenges and proofs arrive on the answering
port, not on the `--discovery` port. A rule scoped to the program covers it; a
rule scoped to `udp/<discovery-port>` alone does not, and the symptom is peers
that are discovered and never authenticated. The answering port is kernel-chosen by
default; `--discovery-reply-port` pins it where a site needs to name it — one port
per node on the machine, since two nodes cannot share it. The startup line reports
both addresses.

## The wire version, and why it moved

The discovery wire is **version 2**, and version 1 is refused. A proof used to carry a
32-byte HMAC under the shared key; it now carries a 32-byte public key and a 64-byte
signature, so its arity and its field widths changed — a GRAMMAR change, which a
version-1 reader could only refuse as malformed. That is the opposite of
[#402](https://github.com/LASTRADA-Software/fastcached/issues/402), which changed only
what the MAC covered and rightly left the version alone. Upgrade the consensus members
of a segment together; a node on an older build is refused rather than misread.

## What it deliberately does not do

**It does not change membership.** Discovery answers "which known members proved
their keys, and where do they answer". A caller decides what to propose. A
membership change is a Raft decision only a leader may make, and a discovery layer
that proposed directly would have every node on the segment proposing the same change
at once.

**It does not state a key.** A discovered member is desired with *no opinion* about
its key: the only key discovery authenticates is the one the roster already holds, and
a desire outlives the moment it was stated — so a key named there would be proposed
straight back over an operator who later re-keyed that member. And the authenticated
set is re-asked of the roster every time it is published, so a proof taken before a
revocation is never handed on after it.

**It does not treat "seen" as "trusted".** Those are separate facts in
`PeerDirectory`, and only a completed handshake under a key the roster holds sets the
second. Nor does a proof here stand in for the consensus wire's own: a member still
proves its key again on every Raft connection it opens or accepts. A peer that changes
the endpoint it advertises **loses** its authenticated status: the proof covered the
old endpoint, so carrying it across would admit an address nobody proved.

**It does not promise delivery.** Beacons are broadcasts and loss is expected. A
peer is remembered for well over a beacon interval, so a lost datagram costs
nothing; a peer that genuinely goes away is forgotten and Raft handles the rest.

## Threat model

| Attacker can… | Outcome |
|---|---|
| Listen to the segment | Learns a cluster exists, which endpoints serve it and which public keys its members hold. None of that admits anybody. |
| Send arbitrary datagrams | Can provoke a challenge, and answer it under a key of its own — which is reported and never desired. Cannot make the challenge table grow: one entry per node id, with a lifetime. Cannot flood the log: an unaccepted key is reported at most once a minute, with how many it stands for, and every one is counted. |
| Replay a captured proof | Refused: the nonce it answers has been spent. |
| Capture a proof and re-aim it at another endpoint | Refused: the endpoint is inside the signature. |
| Claim a known member's id | Reported as an unknown key: the id is a label, and the key the roster holds for it is the credential. |
| Obtain the cluster key file | Nothing here: discovery no longer reads it. |
| Obtain one member's identity key | That member's address can be moved by a proof — the same exposure as that member's consensus connections, and ended by revoking the key, which nothing on any other machine has to change for. |

A proof under an unknown or a revoked key on a healthy segment is a machine nobody
admitted: a new install waiting to be enrolled, or one that was removed. The warning
says which, names the key whole — it verified, so only its holder could have signed it
— and for an unknown key gives the `--cluster-admit` an operator would run if it
belongs. The three refusals are counted apart
(`fastcache_discovery_proofs_refused_{unknown_key,revoked_key,forged}_total`), because
their remedies are different.

## Implementation notes

| Piece | Where | Nature |
|---|---|---|
| Beacon / challenge / proof wire | `Cluster/DiscoveryWire.hpp` | Header-only, versioned, length-prefixed |
| Who is known, who is proved | `Cluster/PeerDirectory` | Pure; time via `IClock` |
| The exchange | `Cluster/DiscoveryService` | Drives the above over `IDatagramSocket` |
| Whose key is whose | `Consensus::IRaftPeerKeys` | The roster the Raft peer wire reads, and this node's own key |
| Datagram I/O | `Net/UdpSocket`, `Net/InMemoryDatagram` | Real socket, and a whole segment in one process |
| The socket pair | `Net/SharedPortDatagram` | Listens shared, answers private; one `IDatagramSocket` |
| Ed25519 | `Core/Ed25519` | RFC 8032 vectors |

`DiscoveryService::PumpOnce` is synchronous, so an entire segment forming a
cluster — including scripted packet loss and a hostile peer — is a loop in a unit
test rather than several processes and a sleep.
