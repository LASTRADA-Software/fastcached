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

## The supported procedure

1. **Stop the builds.** Anything running `fastcache-cc` against the fleet should be
   quiescent. Builds do not break — see *What a mismatch looks like* — but they compile
   locally, so a build started here is slow rather than wrong.
2. **Stop every node**, including any machine running only the scheduler or only a
   cache tier.
3. **Upgrade every node and every client to the same release.** The launcher
   (`fastcache-cc`) speaks this wire too, so a client left behind is a client that
   silently stops using the cache.
4. **Start the nodes**, schedulers first if you run them separately.
5. **Confirm** with the counter below before releasing the builds.

The order matters only in that nothing old should be running once anything new is.

## What a mismatch looks like, and why you must go looking

**A version mismatch never fails a build.** `fastcache-cc` treats an unusable cache as a
miss and compiles locally, which is the contract that keeps a cache problem from
becoming a build problem. The cost is silent: every affected client loses the cache and
every dispatched compile stops being dispatched.

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

All four are exported only when `--metrics` is set. If you do not scrape, the honest
statement is that you cannot tell whether the upgrade was complete, which is a reason
to do step 5 rather than a reason to skip it.

The asymmetry is worth stating plainly: **the server can see this and the client
cannot.** Check the servers.

## The consensus state directory

**#1449 (learners) changed what a consensus member writes to disk and says to its
peers, so a fleet running consensus crosses it as one step as well.** Three formats
moved, each refused by name rather than misread:

| What | Now | Refused as |
|---|---|---|
| The Raft store in `--cluster-dir` (`raft-state`, `raft-log`, `raft-snapshot`) | format 2: a configuration carries voters and learners, and every log record carries the format it was written in | `UnsupportedFormatVersion` — never the damage code — naming the format it found and the one it reads |
| The consensus peer wire | version 3 | refused at the handshake by its version, never read as this layout -- a fleet's consensus members upgrade together |
| The replicated cluster state (a snapshot, and the `--cluster-status` reply) | version 5: each member records its seat | `UnsupportedVersion`, naming both versions |

The store has **no conversion**, and a node started on an older one refuses to start,
saying so. The store is intact; what an operator does is:

1. Stop the node.
2. Move `raft-state`, `raft-log` and `raft-snapshot` out of its `--cluster-dir`,
   **leaving `node-id` where it is** — the identity lives in the same directory, and a
   wiped identity is a different node.
3. Start it again. It comes back with an empty log under its bootstrap configuration
   (`--raft-peer`), or waiting to be admitted if it was started with `--raft-join`.

Whatever the cluster agreed at **runtime** has to be agreed again once a leader is
elected: members admitted with `--cluster-admit` or `--cluster-admit-learner`, and
cluster settings (`--cluster-set`). Members found by `--discovery` are re-admitted by
discovery itself, as **learners** — promote the ones that voted again with
`--cluster-admit` — and so is a machine the cluster had forgotten, because its tombstone
was part of the state that was moved aside; forget it again. Nothing a build writes
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
