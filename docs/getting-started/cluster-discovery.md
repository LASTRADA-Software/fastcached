# Cluster discovery

How the nodes of a `fastcached` peer cluster find each other on a LAN, and how one
proves it belongs before the cluster admits it.

> **Status.** The discovery layer is implemented and unit-tested; wiring it into
> the node binary and the Raft membership change it feeds are the remaining
> pieces of the peer-to-peer work tracked in
> [#81](https://github.com/LASTRADA-Software/fastcached/issues/81).

## What problem it solves

Raft membership names a member by **id**. It carries no address — deliberately, so
that agreeing *who* is in the cluster stays independent of *where* they are. The
consequence is that a node the cluster has voted to admit is still unreachable
until something supplies an endpoint for it.

Discovery is that something. A node announces itself on the segment, proves it
holds the cluster's pre-shared key, and the endpoint it proved for is what the
cluster then dials.

## The exchange

```
     ┌───────┐                                        ┌───────┐
     │ node  │                                        │ peer  │
     └───┬───┘                                        └───┬───┘
         │  ── beacon (broadcast) ─────────────────────►  │  cluster, id, endpoint
         │                                                │  no key, nothing derived from it
         │  ◄──────────────── challenge (unicast) ──────  │  a fresh 32-byte nonce
         │                                                │
         │  ── proof (unicast) ────────────────────────►  │  HMAC-SHA256 over
         │                                                │  (cluster, nonce, id, endpoint)
         │                                                │
         │                          [peer may now be proposed for membership]
```

Three properties are worth reading off that diagram, because each is what some
plausible simpler design gets wrong.

**A beacon is an invitation to ask, not a credential.** It carries the cluster id,
the node id and the Raft endpoint, and nothing else — no key, no key hash, nothing
an eavesdropper can replay. Putting anything key-derived in a broadcast would hand
every listener on the segment what it needs to join.

**The proof authenticates a `(node, endpoint)` pair, not just the nonce.** Both are
inside the MAC. Signing the nonce alone would let anyone who observed one valid
proof replay its tag with a *different* endpoint substituted — admitting a
legitimate node id at an attacker's address. Since an admitted node is assigned
compile jobs and returns objects that are cached fleet-wide, that is object
injection into everybody's build.

**A challenge is answerable once.** The nonce is spent when the proof arrives,
valid or not, and an unsolicited proof is refused *even when it carries the real
key* — it answers a nonce nobody here chose.

## Why the key never travels

The pre-shared key authenticates a handshake rather than appearing in beacons.
That is the difference between "an attacker who can listen learns the secret" and
"an attacker who can listen learns that a cluster exists".

Sharing a key across two fleets is still a bad idea, but the cluster id keeps them
apart even when somebody does it: a beacon for another cluster is ignored before a
challenge is issued, and a challenge for another cluster is never answered.

## What it deliberately does not do

**It does not change membership.** Discovery answers "who has proved they hold the
key, and where do they answer". A caller decides what to propose. Admitting a node
is a Raft decision only a leader may make, and a discovery layer that proposed
directly would have every node on the segment proposing the same change at once.

**It does not treat "seen" as "trusted".** Those are separate facts in
`PeerDirectory`, and only a completed handshake sets the second. A peer that
changes the endpoint it advertises **loses** its authenticated status: the proof
covered the old endpoint, so carrying it across would admit an address nobody
proved.

**It does not promise delivery.** Beacons are broadcasts and loss is expected. A
peer is remembered for well over a beacon interval, so a lost datagram costs
nothing; a peer that genuinely goes away is forgotten and Raft handles the rest.

## Threat model

What the pre-shared key does and does not buy you:

| Attacker can… | Outcome |
|---|---|
| Listen to the segment | Learns a cluster exists and which endpoints serve it. Learns nothing about the key. |
| Send arbitrary datagrams | Can provoke a challenge, cannot answer one. Cannot make the challenge table grow — one entry per node id, with a lifetime. |
| Replay a captured proof | Refused: the nonce it answers has been spent. |
| Capture a proof and re-aim it at another endpoint | Refused: the endpoint is inside the MAC. |
| Obtain the key | **Full compromise.** They can join, be assigned compiles, and return objects cached fleet-wide. |

That last row is the important one. Auto-join by shared secret is a materially
larger blast radius than "you typed the scheduler's address", which is what the
non-clustered deployment relies on. Treat the cluster key like a signing key:
distinct per fleet, distributed the way you would distribute a private key, and
rotated if it is ever exposed.

Joins and failed proofs are logged at `info` and `warn` respectively. A failed
proof on a healthy segment does not happen, so when one appears it means either a
misconfigured key or somebody trying to join a fleet they do not belong to — both
worth an operator's attention.

## Implementation notes

| Piece | Where | Nature |
|---|---|---|
| Beacon / challenge / proof wire | `Cluster/DiscoveryWire.hpp` | Header-only, versioned, length-prefixed |
| Who is known, who is proved | `Cluster/PeerDirectory` | Pure; time via `IClock` |
| The exchange | `Cluster/DiscoveryService` | Drives the above over `IDatagramSocket` |
| Datagram I/O | `Net/UdpSocket`, `Net/InMemoryDatagram` | Real socket, and a whole segment in one process |
| HMAC-SHA256 | `Core/Sha256` | FIPS 180-4 / RFC 4231 vectors |

SHA-256 is implemented rather than taken from OpenSSL because OpenSSL is
**optional** in this build (`FASTCACHED_ENABLE_TLS`) while peer authentication is
not — a cluster that could only authenticate its members when TLS happened to be
compiled in would silently accept anybody in the default configuration.

`DiscoveryService::PumpOnce` is synchronous, so an entire segment forming a
cluster — including scripted packet loss and a hostile peer — is a loop in a unit
test rather than several processes and a sleep.
