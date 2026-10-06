# Upgrading a fleet

**A `fastcache-compile-node` fleet upgrades as a unit. A rolling upgrade — replacing
one node at a time while the others keep serving — is not supported in this release,
and this page is here so that is something you plan around rather than discover during
a rollout.**

Read this before upgrading more than one machine.

## Why

Every `0xFC` exchange carries a wire version. A build accepts exactly one:
`MinSupportedVersion` equals `CurrentVersion` in `CompileCacheWire.hpp`, and has at
every point in this project's history. So the moment one end moves, every peer still on
the old version is refused.

That is a deliberate choice rather than an omission, and the reasoning is recorded at
the constants themselves. In short: a reply on this wire carries a status byte and a
length and *no kind*, so the "step over what you do not understand" property that the
request framing has does not exist on the way back. An old client meeting a new reply
does not skip it — depending on the change it either abandons a compile several minutes
in, as a transport failure naming nothing, or reads a record it does not understand as
the field it expected and continues. Refusing the older version outright is
`UnsupportedVersion`, which names the supported range and arrives before any source is
sent. A loud, immediate, named refusal is the better failure.

Version 9 is such a step: it adds the live-stats stream `fastcache-cli live-stats`
subscribes to ([#1399](https://github.com/LASTRADA-Software/fastcached/issues/1399)), and
every node, every `fastcached` serving `0xFC`, every `fastcache-cc` and every
`fastcache-cli` has to move to it together.

What makes it a *fleet* problem rather than a daemon problem is that a fleet has more
than one process. A cache daemon is upgraded, restarted, and done; twenty nodes cannot
be.

## This release: 0xFC version 15, discovery version 3, consensus version 5

Every one of the three wires changed its grammar, so every node, every `fastcached` serving
`0xFC`, every `fastcache-cc` and every `fastcache-cli` moves together (the supported procedure
below).

- **`0xFC` 15:**
  - AUTH carries a credential kind, so a launcher presents a machine ticket.
  - LEASE carries the workers the client could not reach (at most 16) and the client's
    toolchain label.
  - A grant carries a dial hint (the address the scheduler last saw the worker at, when the
    worker reports it as its own) and the worker's identity key, and a compile reply is signed
    by that key.
  - Workers report their interface addresses in REGISTER and HEARTBEAT.
  - ENROLL is signed and challenged.
  - New verbs: MINT-TICKET, FLEET-SUMMARY, SHARED-FETCH and SHARED-STORE. Retired:
    CLUSTER-ADMIT-CLIENT, CLUSTER-FORGET-CLIENT and CLUSTER-ADMIT-WORKER.
  - A version-14 peer is refused `unsupported-version`, on the four counters below.
- **Discovery 3:** a beacon carries the fleet's signed summary. **A mismatch is silent:** each
  build drops the other's datagrams without a counter or a log line, so the two simply never see
  each other on a shared segment. Finish the upgrade on every machine on a segment.
- **Consensus 5:** see [the consensus peer wire](#the-consensus-peer-wire-version-5).
- **Every cached object misses once.** The launcher's key moved (`objkey-v7`, `manifest-v7`,
  value generation 7). This costs one cold build per fleet and needs no step.
- **`fastcache-cli live-stats` from an older build** refuses the newer layout. Upgrade the
  clients too.

### What refuses to start after the upgrade, and the step

| What | Refused as | Step |
|---|---|---|
| A state directory whose `node-key` other accounts can read (an older install) | the identity-key refusal (`Exposed`), naming the file | Delete the state directory (`%ProgramData%\fastcache-node` on Windows). The key counts as disclosed. The node mints a new identity; forget the old id with `--cluster-forget`. |
| A Raft snapshot older than state version 10, a log entry older than command version 6, or one holding a retired command kind (3, 4 or 6) | `UnsupportedFormatVersion` (the held state's start refusal), or the retired kind by name, naming the directory | Move `raft-state`, `raft-log` and `raft-snapshot` aside ([the consensus state directory](#the-consensus-state-directory)) |
| `<cluster-dir>/roster` left behind | an entry this build keeps nothing for | Delete that file alone ([signed leases](#signed-leases-checked-against-the-state-each-node-applied)) |
| A formation record below format 7 (builds of this branch only; no release wrote one) | `UnsupportedFormatVersion` | Move `formation` aside. The node re-forms as a new solitary cluster: re-enroll it. |
| A configuration file naming `scheduler` (on a serving node), `fleet_member`, `scheduler_token_file`, `cluster_admit_client`, `cluster_forget_client`, `raft_peer`, `raft_join`, `cluster_id`, `serve_scheduler`, `voter_key`, `enroll_from` or `cluster_admit_worker` | refused by name, with its step | Remove the key |
| A service registration replaying any of those flags, or `--scheduler` on a node that serves | refuses to start under the service manager | `--install-service` again. The MSI does this on upgrade. |
| MSI properties `FASTCACHE_NODE_SCHEDULER` and `FASTCACHE_NODE_ADVERTISE` | not read; an upgrade forgets the remembered advertised endpoint | Drop them from scripted installs |

## The supported procedure

1. **Stop the builds.** Anything running `fastcache-cc` against the fleet should be
   quiescent. Builds do not break — see *What a mismatch looks like* — but they compile
   locally, so a build started here is slow rather than wrong.
2. **Stop every node**, including any machine running only the scheduler or only a
   cache tier.
3. **Upgrade every node and every client to the same release.** The launcher
   (`fastcache-cc`) speaks this wire too, so a client left behind stops using the
   cache — saying so on the developer's terminal, where you are not looking.
4. **Start the nodes**, schedulers first if you run them separately.
5. **Confirm** with the counter below before releasing the builds.

The order matters only in that nothing old should be running once anything new is.

## What a mismatch looks like, and why you must go looking

**A version mismatch never fails a build.** `fastcache-cc` treats an unusable cache as a
miss and compiles locally, which is the contract that keeps a cache problem from
becoming a build problem. The cost is quiet: every affected client loses the cache and
every dispatched compile stops being dispatched, and what a client says about it is said
on the machine that ran the build.

The signal is server-side, and it is spread over **four** counters because a fleet has
four places a frame can be refused. Watch all of them: a mismatch shows up only on the
surface the stragglers actually talk to, so seeing zero on one counter tells you
nothing about the others.

| Series | Refused where |
|---|---|
| `fastcache_node_cache_requests_refused_unsupported_version_total` | a node's cache tier |
| `fastcache_worker_frames_refused_unsupported_version_total` | a node's compile worker |
| `fastcached_dispatch_frames_refused_unsupported_version_total` | the fleet scheduler's port |
| `fastcached_cache_frames_refused_unsupported_version_total` | the `fastcached` daemon's compile-cache port |

Any of them non-zero and rising means something is still speaking the other version.
These are the *only* places a mismatch is visible, so an operator who has not been told
to watch them will not see one — which is why they are named here rather than left to
be found during a rollout.

The three node counters are on every node whatever it was started with:
`fastcache-cli node-metrics` reads them over the node's `0xFC` port, and a node with
`--admin-listen` also serves them on `/metrics`. The daemon's is exported only when
`fastcached` runs with `--metrics`. If you cannot read them, you cannot tell whether the
upgrade was complete, which is a reason to do step 5 rather than a reason to skip it.

**The servers count every refusal; a client only mentions it, on its own machine.** A
launcher refused by a cache on the other version prints one line naming it at most every
five minutes, and one refused by the fleet tallies it in `--show-stats` as *this launcher
and the fleet disagree about the wire* — both on a developer's machine, not yours. Check
the servers.

And a fleet on **one wire at two builds** is refused by nothing at all, so none of the
counters above moves. The leader raises the `mixed-node-versions` condition for that,
naming each build and the machines running it (`fastcache-cli fleet`, its conditions section).

## The consensus peer wire, version 5

**Version 5's proof states the session direction it asks for, and the session keys are
derived per direction.** A version-4 member names no direction for the keys that would seal
its session, so the two refuse each other at the handshake, by version. **Only the dialling
side can see it**, because the acceptor challenges first:

- an upgraded member DIALLING a version-4 member meets its version-4 challenge, refuses it,
  and counts `fastcache_raft_peer_dials_refused_no_challenge_total`, with the version in the
  log line;
- a version-4 member dialling an upgraded one refuses the version-5 challenge and closes
  without a proof, so the upgraded ACCEPTOR counts nothing at all.

The consensus members of a fleet upgrade together; nothing else is needed for this step.

**#178 made every consensus connection prove each member's OWN identity key**, where it
proved the cluster's shared key, so the peer wire moved from version 3 to 4 and the two
cannot talk: an older peer proves only the shared key, and accepting it would be the
fallback the handshake exists to refuse. Upgrade a cluster's consensus members together.
The steps below are for moving onto the build that introduced version 4, whose members
still named one another with `--raft-peer`; that flag is gone since
[the formation record decides a cluster's shape](#the-flags-that-carried-a-clusters-shape),
and before you upgraded, you gave each member's `--raft-peer` list every other member's key:

1. On each member, run `fastcache-compile-node --print-identity` with the flags it runs
   with, as the account it runs as. It prints the member's `raft-peer` token, key included.
   A member that already has a state directory keeps its id; a key is minted beside it.
2. Put every token into every member's `--raft-peer` list.
3. Stop all of them, upgrade, start all of them.

A member left out of step 2 is refused by the others as a key never given
(`fastcache_raft_peer_connections_refused_unknown_key_total`) until the cluster records its
key -- which the leader does for itself when it leads, and which `--cluster-admit` with
`@<key>` does for anybody else. A mixed cluster shows as
`fastcache_raft_peer_dials_refused_no_challenge_total` on the new nodes, counted where they
dial an old one; the acceptor challenges first, so an old node dialling a new one closes
before its proof and the new node counts nothing.

## The flags that carried a cluster's shape

`--raft-peer`, `--raft-join`, `--cluster-id` and `--serve-scheduler` are **gone**, with
their configuration-file keys `raft_peer`, `raft_join`, `cluster_id` and
`serve_scheduler`. Which cluster a node is in, whether it founded that cluster or joined
it, and whether it serves the scheduler are the node's **formation record**, kept in its
state directory beside its identity and written by the node itself — its first start mints
a cluster of one — so a flag carrying any of them would be a second author that could
disagree with the record.

A node that names one refuses to start, by name, and so does a service registration that
replays one: remove them from every command line, configuration file and registration
before upgrading. A member upgraded from a build that had them starts as a cluster of one,
and rejoins its fleet by admission, as any machine does — see
[adding a machine to a running cluster](../tools/fastcache-compile-node.md#adding-a-machine-to-a-running-cluster).

The flags that admitted by **address** are gone too, because an address admits nobody any
more: `--fleet-member`, `--scheduler-token-file`, `--cluster-admit-client` and
`--cluster-forget-client`, with their keys `fleet_member`, `scheduler_token_file`,
`cluster_admit_client` and `cluster_forget_client`. A machine is admitted by its identity
key -- a node by the key it proves, a client by the machine ticket its own node mints -- and
forgotten by that key with `--cluster-forget`. Each is refused by name, with the step that
replaces it, on a command line, in a configuration file and in a service registration.

The flags of the retired principal mode are gone the same way: `--enroll-from`,
`--voter-key` and `--cluster-admit-worker`, with their keys `enroll_from`, `voter_key` and
`cluster_admit_worker`. Every machine joins one way, as a learner holding its identity key:
it finds its fleet by discovery or at the node `--fleet-seed` names, asks to enroll, and
`--enroll-approve` admits it, or `--cluster-admit-learner` admits it by its key. No key anchors
a roster, because every machine checks grants against the roster its own consensus applies.

## Signed leases, checked against the state each node applied

**#178 signs every lease with the issuing scheduler's own identity key and has every
worker check it against the cluster's voters**, where a lease was an HMAC under the
shared key. `0xFC` moved to version 13 for it (15 now), the replicated commands to version 4
(`--cluster-forget` now revokes the key of what it forgets, #1555), and the lease format
to 3. The lease format is **4** since a grant names the worker's key and a compile reply is
signed by it: a version-3 grant does not verify. The certified roster that first carried the voters to a worker -- endorsements on
NODE-ANNOUNCE, `--voter-key` anchors, a kept roster file and the `roster-expired` refusal --
is **retired**: every worker is now a member of its fleet, a learner or a voter, and checks a
grant against the state its own consensus applied. Nothing older reads any of it, so this is
the whole-fleet step above, and what each machine is started with gets simpler:

1. **Every node runs consensus, by default.** A first start is a cluster of one, on
   `--listen-raft`'s default port `6680`, with this machine's name as its consensus address;
   nothing is added to a command line. `--voter-key` is gone, and a command line naming it is
   refused by name, with its step.
2. **Every other machine joins the fleet as a learner.** It finds the fleet by beacon, or by
   `--fleet-seed=<host>` where no beacon reaches, asks to join, and is approved on a voter with
   `--enroll-approve` (or under `--enroll-auto-approve`); see
   [cluster discovery](../getting-started/cluster-discovery.md). There is no key to type.
3. **Confirm on the workers.** `fastcache_worker_jobs_refused_lease_no_roster_total`
   rising without stopping means a worker's applied state names no voter yet: it has not been
   admitted, and its leases are refused `grant-unverifiable`.

**A node upgraded in place over a `--cluster-dir` that still holds the kept roster refuses to
start**, naming `<cluster-dir>/roster` as an entry this build keeps nothing by that name for.
An older worker kept its certified roster in that file; nothing reads it any more, and the
node refuses whatever it cannot name in the directory that holds its identity rather than
guess. So, once per machine that ran an older worker:

1. Stop the node.
2. Delete `roster` from its `--cluster-dir` -- that file alone, and a leftover temporary named
   after it if a crash left one; **`node-id`, `node-key` and `formation` stay**, since they are
   the node's identity and how it formed.
3. Start it. Nothing else in the directory needs to change for this step.

## The consensus state directory

**#1449 (learners) changed what a consensus member writes to disk and says to its
peers, so a fleet running consensus crosses it as one step as well.** Three formats
moved, each refused by name rather than misread:

| What | Now | Refused as |
|---|---|---|
| The Raft store in `--cluster-dir` (`raft-state`, `raft-log`, `raft-snapshot`) | format 2: a configuration carries voters and learners, and every log record carries the format it was written in | `UnsupportedFormatVersion` — never the damage code — naming the format it found and the one it reads |
| The consensus peer wire | version 3 (4 since #178, above) | refused at the handshake by its version, never read as this layout -- a fleet's consensus members upgrade together |
| The replicated cluster state (a snapshot, and the `--cluster-status` reply) | version 5 for #1449, which records each member's seat; this build writes version 10 | `UnsupportedVersion`, naming both versions |
| The replicated commands (each log entry) | version 6: a retired command kind (3, 4 or 6) is refused by name | `UnsupportedFormatVersion` at start, naming both versions |

The store has **no conversion**, and a node started on an older one refuses to start,
saying so. **So does a node whose store is this build's but whose snapshot or retained
log entries hold the cluster state or a command in an older encoding** — it names the
directory, which part it could not read and both versions, rather than running on the
part it could (which, for a snapshot, is a cluster with no members and no forget
tombstones). Either way the directory is intact; what an operator does is:

1. Stop the node.
2. Move `raft-state`, `raft-log` and `raft-snapshot` out of its `--cluster-dir`,
   **leaving `node-id`, `node-key` and `formation` where they are** — the identity and the
   record of how the node formed live in the same directory, and a wiped identity is a
   different node, which the cluster would have to admit while it went on counting the
   old one.
3. If the node **joined** its fleet, start it again: its formation record says so, and it
   waits to be admitted instead of bootstrapping a cluster of itself. A cluster that still
   counts it catches it up from the leader; one that has forgotten it admits it again with
   `--cluster-admit` (or `--cluster-admit-learner`). It keeps its identity, so it is admitted
   under the key it already holds.
4. The node that **founded** its fleet bootstraps it again, alone, so move its state aside
   only when every member's was moved aside together. Start it first, admit the others
   again from it, and make again every `--cluster-*` change made since the fleet formed.

A RUNNING member offered a snapshot by a leader on another state format does not stop: it
refuses the snapshot and stays behind, raising the `unreadable-leader-snapshot` condition
(`fastcache-cli node-conditions`, and the fleet page). It follows no change the cluster makes
until it can read what the leader sends, and catches up by itself once it runs the leader's
build — nothing needs moving aside. That is the one condition a mixed fleet shows on its own,
and the reason to finish an upgrade rather than leave it half done.

Whatever the cluster agreed at **runtime** has to be agreed again once a leader is
elected: members admitted with `--cluster-admit` or `--cluster-admit-learner`, and
cluster settings (`--cluster-set`). Discovery admits nobody, so every other machine asks
to join again and is approved again, as a **learner** — promote the ones that voted again
with `--cluster-admit` — and a machine the cluster had forgotten can ask too, because its
revoked key was part of the state that was moved aside; forget it again. Nothing a build writes
into `--cache-dir` is involved.

This is backwards compatibility not being owed yet, and it is stated rather than
discovered: the version on each format is what turns *an older store* into a refusal
that names itself instead of a store read as damage.

## Downgrading

The same procedure, in the same order. Nothing in the on-disk cache format is tied to
the wire version, so a node that is downgraded serves its existing cache; entries it
cannot decode are treated as misses rather than as corruption.

## When this changes

A compatibility window — a build accepting more than one wire version — is a real
possibility rather than a promise, and it would be stated per verb rather than implied
by a single constant, because the verbs differ in how much room they have.
`Op::Compile` has exact arity and is the binding constraint; the cache verbs are
cheaper to widen. Until such a window is documented here, assume none exists and treat
every upgrade as a flag day.
