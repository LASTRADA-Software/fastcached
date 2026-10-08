# Roaming node addresses — design

Status: approved in brainstorming, 2026-10-08.
Scope: `fastcache-compile-node` in every role (worker, scheduler, Raft voter) on Linux, macOS and Windows.

## Problem

A laptop node's IP changes over time — DHCP renewals, Wi-Fi ↔ wired LAN, moving between networks.
On v0.4.0 (5c468351):

- Linux and macOS start **no network-change watcher** (`StartNetworkChangeWatcher` returns null).
- The advertised 0xFC endpoint changes **only** on SIGHUP with an edited `advertise:`; the FQDN
  default is resolved once at startup (`HostNaming`) and never again.
- A voter's `raftEndpoint` **never** moves at runtime: `--raft-self` is not reloadable, the
  formation roster pins a node's own Raft endpoint, and `NODE-ANNOUNCE` moves only
  `schedulerEndpoint`. A moved voter is permanently unreachable (peers dial its old address; its
  replies ride their dials). A moved leader cannot commit its own new endpoint.
- One-way Raft sessions have no idle bound and no TCP user timeout: a moved node writes into
  dead sockets until the kernel gives up, and each move parks a stale inbound session on its peers.
- Socket activation **refuses to start** without `--advertise`, so the stock Linux package fails
  out of the box; the work-around (an IP literal) disables dial hints, i.e. disables roaming.

## Goals

- A node keeps working across address changes with no config edit and no restart.
- Clients and peers reach it again within seconds (target ≈3 s, worst case bounded by the 10 s
  debounce ceiling, plus a 30 s periodic re-probe as a safety net).
- Holds for workers, schedulers and Raft voters, including the leader.
- A stock package install works with no `--advertise`.

## Non-goals

- Voters behind NAT (unreachable for inbound dials). A later spec may make voter sessions
  two-way like learner sessions.
- Every voter moving simultaneously — existing discovery/seed recovery applies.
- mDNS / DNS-SD.

## Decisions

1. **Self-published IPs**, not DNS: the node derives its own address and pushes it to the cluster.
2. **`auto` is the default everywhere** (replaces the FQDN default). Names are opt-in.
3. **Single-endpoint formats are kept.** The persisted `ClusterMember`, Raft log commands, roster
   and `FleetSummary` stay one `host:port` string each.

## Design

### Components

```
OS address/link change
  └─ NetworkChangeWatcher ── Windows: existing Notify* callbacks
                          ── Linux:   netlink RTM_NEWADDR/DELADDR/NEWLINK/DELLINK   (new)
                          ── macOS:   PF_ROUTE RTM_NEWADDR/DELADDR/IFINFO           (new)
        └─ NetworkChangeRelay (existing debounce 2 s quiet / 10 s ceiling)
              └─ HostEventHub ── HostEvent::NetworkChanged
                    ├─ EndpointResolver           (new)  marks its probe stale
                    ├─ HostEventInbox / Presence  (existing) announce now
                    └─ CachedLocalityOracle       (existing) re-probe interfaces
EndpointResolver ── publishes ──> AnnouncedEndpoint (0xFC)  ──> WorkerTier re-registers, presence announces
                 └─ publishes ──> AnnouncedEndpoint (Raft)  ──> ConsensusTier (1 s reconcile) resets sessions,
                                                                leader asserts it; discovery beacon reads it
```

- **`IRouteProbe`** (Platform): "which local address would the kernel use to reach host H" —
  UDP `connect()` + `getsockname()`, sends nothing. Injected; tests use a fake.
- **`EndpointResolver`** (compile-node): the single place deciding "my address right now".
  Inputs: the advertise policy (below), the bound listener ports, `IRouteProbe`, probe targets
  in order: recorded voters' / applied schedulers' hosts, configured seeds / scheduler, then a
  fixed default-route probe target (`192.0.2.1` TEST-NET-1 for v4, `2001:db8::1` for v6 — only
  routing is consulted, nothing is sent). Results that are loopback, link-local, unspecified or
  multicast are rejected. Publishes into the existing `AnnouncedEndpoint` (0xFC) and a new
  `AnnouncedRaftEndpoint` (Raft). Recomputes on `NetworkChanged` and every 30 s.
- **POSIX watchers** fill `StartNetworkChangeWatcher`'s empty branch, feeding the existing
  `NetworkChangeRelay`; started after the daemon fork. The message parsing is a pure function
  over bytes, separate from the socket.

### Advertise policy (data-driven)

`--advertise` and `--raft-self` (YAML `advertise:`, `raft-self:`) each take:

<!-- table-total: none -->
| Value | Mode | Recomputed on network change | Dial hints |
|---|---|---|---|
| unset / `auto` | Auto (default) | yes | yes |
| `auto:<port>` | Auto, advertised port overridden | yes | yes |
| `<name>:<port>` | PinnedName | no (client re-resolves per dial) | yes |
| `<ip>:<port>` | PinnedLiteral | no — logs once that roaming is off | vetoed (as today) |

One descriptor table (`AdvertiseModeRow`) carries those properties; code reads the row. Both
flags are reloadable. The port defaults to the bound listener's port. A non-wildcard listener
bind (or socket-activated descriptor bound to a specific IP) yields PinnedLiteral of that address,
as today.

The probed address is a runtime fact on the configuration (`NodeConfig::routeHost`, beside
`hostNames`): `AdvertisedEndpoint` / `RaftSelfEndpoint` use it for a wildcard bind in Auto mode,
falling back to today's FQDN / withheld chain when no route exists (offline at start).
`HostNames` stay start-time only (SRV seeding, display). The resolver is the **sole publisher** of
both endpoints; `WorkerTier` re-registers when the published endpoint differs from the one it
registered under.

The formation roster no longer pins this node's **own** `raftEndpoint`: the resolver's live value
is this node's desire.

### Socket activation

`ActivatedDescriptor` reads the inherited socket's bound address/port (existing
`BoundAddressOf` / `BoundPortOf`). Wildcard → port + resolver host (Auto). Specific IP →
PinnedLiteral. The "--advertise is required under socket activation" refusal is removed.

### Wire and consensus flow

**No wire change.** `MinSupportedVersion == CurrentVersion`, so any NODE-ANNOUNCE arity change is a
fleet flag day. Instead the leader derives a member's new Raft endpoint from its announced `0xFC`
endpoint by a host-coupling rule (below). `interfaceAddresses` already exists in `CapacityFields`;
the presence path now fills it.

1. Watcher fires → debounced `NetworkChanged`.
2. `EndpointResolver` re-probes; publishes changed `0xFC` / Raft endpoints.
3. `ConsensusTier` on the moved node closes its dialled peer sessions (new public
   `RaftPeerTransport::ResetSessions()`, built on the existing `CloseSockets(nullopt)`), so its
   senders redial at the next message instead of writing into dead paths.
   A moved **leader** needs no explicit step-down: its followers' replies ride their dials to the
   old address, so it loses quorum contact and the existing CheckQuorum (`RelinquishLeadership`)
   steps it down within about one election timeout.
4. The presence loop sends `NODE-ANNOUNCE` at once (existing `AnnounceNow`), carrying the new
   `0xFC` endpoint.
5. The leader's `NoteAnnouncedEndpoint` (proven caller id, recorded members only) feeds
   `AnnouncedEndpointDesires`, which now applies the **host-coupling rule**: when the member's
   recorded `raftEndpoint` host equals its recorded `schedulerEndpoint` host, the desire moves the
   Raft host too (keeping the recorded Raft port). Auto mode always produces equal hosts; a node
   that pins them apart is never coupled. `MembershipProposals` already re-proposes a differing
   `raftEndpoint` with seat and key kept; one in-flight change per member, newest desire wins.
6. Commit → every node's `LearnMembers` → `RaftPeerTransport::Learn` redials the new address and
   closes the stale socket. Redirects and `AppliedSchedulers` carry the new `schedulerEndpoint`.
7. A leader asserts its own live Raft endpoint on its self desire every reconcile pass, as it
   already does for its `schedulerEndpoint`.

**Dead-path hygiene:** the Raft acceptor keeps **one one-way session per proven dialler**: a new
one-way session from an id supersedes (closes) the older one, so a move no longer parks a stale
inbound session against `maxConnections`. (An idle bound on one-way sessions is ruled out by the
consensus rulebook: follower↔follower sessions are legitimately silent.)

**Trust is unchanged:** a node re-addresses only itself (id from the proven Ed25519 connection,
never the payload); the endpoint must pass `IsPeerDialableEndpoint`; a mismatch with the
observed source is logged (#242), not refused, so multi-homing keeps working.

**Discovery:** the beacon's `FleetSummary.raftEndpoint` reads the resolver's live value instead of
the start-time `FixedFleetSummary`, giving a second, same-segment path through the existing
authenticated-beacon desire.

### Visibility

- Info log per change: old → new endpoint (and Raft endpoint).
- Two counters: `fastcache_node_endpoint_changes_total`, `fastcache_node_raft_endpoint_changes_total`.
- (`fastcache-cli node-status` is NOT extended: a reply field is a wire flag day.)

### Packaging and docs

- Packaged `.socket`/`.service`/YAML comments describe `auto`; no `--advertise` needed.
- Docs: `docs/tools/fastcache-compile-node.md`, `docs/operations/cluster-communication.md`,
  `docs/operations/windows-office-fleet.md`, `.agent/rules/distributed-compilation.md`,
  `.agent/rules/consensus-and-cluster.md` (+ AGENT.md tripwires).
- Release-note upgrade hint: remove a literal `--advertise` work-around or roaming stays off.

## Error handling

<!-- table-total: none -->
| Situation | Behaviour |
|---|---|
| Watcher cannot start | Warn once; 30 s periodic re-probe still roams (slower). |
| No usable address | Keep last published endpoint, never publish loopback; status `offline`. |
| Flapping | Debounce + one in-flight proposal per member; newest desire wins. |
| Announce during in-flight proposal | Queued as newest desire. |

Fallible APIs return `std::expected<…, RouteProbeError>` (enum class).

The route probe opens a UDP socket and only `connect()`s it (no datagram is sent or received);
it is registered as a second justified row in the `udp-opener` guard.

## Testing

- Unit: `SystemRouteProbe` against loopback; `EndpointResolver` (fake `IRouteProbe`, one case per
  policy row, stale-on-event, no-route keeps last); netlink / route-socket classifiers on byte
  fixtures; advertise / raft-self parsing; `AnnouncedEndpointDesires` host coupling (coupled moves,
  uncoupled never); one-way session supersede; activated-descriptor bind adoption.
- Cluster: `Cluster/MembershipCluster_test.cpp` (policy over a real Raft cluster): an announced
  move of a coupled member commits its new Raft endpoint.
- Node: `WorkerTier` re-registers (withdraw old, register new) when the published endpoint moves.
- Real kernel: the Linux / macOS watcher starts (non-null) without privileges.
