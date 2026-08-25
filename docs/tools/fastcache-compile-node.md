# fastcache-compile-node

A compile worker. It takes translation units that missed the cache and compiles
them, so a build is not limited to the cores of the machine running it.

It is **not** a cache and **not** a scheduler: it holds no keys, stores nothing,
and is given no cache credentials. The object it produces goes back to the client
that asked for it, and the *client* stores it.

## How the pieces fit

```
 fastcache-cc                 fastcached                  fastcache-compile-node
 ────────────                 ──────────                  ──────────────────────
                          :6674 cache                          :6676
                          :6675 dispatch  ◄──── register + heartbeat ────┘

 preprocess ─► key ─► FETCH ──hit──► done
        │
       miss
        ▼
   ask for a worker  ──────────►  match the toolchain exactly
   ◄── endpoint + lease token     pick the least-loaded one
        │                          (or refuse: compile locally)
        ├── send the preprocessed TU ──────────────────────────►
        ◄────────── object + diagnostics ───────────────────────
        │
        └─► write the object ─► STORE it (the client, not the worker)
```

**Every refusal ends in a local compile.** No matching toolchain, no free slot,
another client already compiling this key, an unreachable worker — all of them
fall back. Distribution cannot fail a build; that is what makes it safe to leave
switched on in a fleet where machines come and go.

## Quick start

Three processes. A cache:

```sh
fastcached --listen=0.0.0.0:6674
```

A scheduler, which is a compile node rather than the cache — handing out capacity
is a decision only one node may make at a time, and the cache cannot establish
which node that is:

```sh
fastcache-compile-node \
    --listen-scheduler=0.0.0.0:6675 --fleet-open \
    --scheduler=127.0.0.1:6675 \
    --advertise=scheduler.internal:6676 \
    --toolchain=/usr/bin/g++
```

One of `--fleet-member` or `--fleet-open` is required: a scheduler with no member
list refuses every caller, which is the right default and not a working
configuration.

A worker, on each machine that should take work:

```sh
fastcache-compile-node \
    --scheduler=build-cache.internal:6675 \
    --advertise=worker-01.internal:6676 \
    --toolchain=/usr/bin/g++
```

And the client, which is the launcher you already use:

```sh
export FASTCACHE_ADDR=build-cache.internal:6674
export FASTCACHE_SCHEDULER=build-cache.internal:6675
cmake -DCMAKE_CXX_COMPILER_LAUNCHER=fastcache-cc ...
```

Unset `FASTCACHE_SCHEDULER` and every miss compiles locally again, which is the
behaviour without this feature.

## `--advertise` is the flag to get right

The scheduler hands your string to clients **verbatim**. A worker that
advertises `127.0.0.1` is leased and then never answers, and the symptom is a
build that mysteriously falls back to local compiles on every machine but one.

It defaults to `--bind` and `--port`, which is correct only when those already
name an address other machines can reach.

## Toolchains

```sh
--toolchain=/usr/bin/g++          # this node computes the fingerprint
--toolchain=<fingerprint>=/usr/bin/g++   # or pin it explicitly
```

A job names a **fingerprint, never a program**. The worker maps that fingerprint
to a compiler from its own configuration and refuses one it does not have — which
is the difference between a build accelerator and a remote shell, and is why
there is deliberately no default compiler.

The fingerprint is a digest of the compiler's version banner **and its whole
include tree**, so two machines with the same compiler at different install
prefixes match, while two machines whose headers differ do not. Matching is
byte-identical and cannot be loosened: an over-strict match costs a local
compile, an over-loose one produces a silently wrong object that is then stored
under a key other machines fetch.

Computing it walks the include tree, which takes a few seconds the first time a
machine sees a toolchain and is cached afterwards.

### When no worker matches

The launcher reports `not dispatched (rejected (no-worker): ...)`. Ask both ends
what they think the fingerprint is:

```sh
fastcache-cc --print-toolchain-fingerprint /usr/bin/g++   # on the client
journalctl -u fastcache-compile-node | grep 'serving'     # on the worker
```

They must be identical. If they are not, the two machines really do have
different toolchains — different patch releases, different SDKs, a vendored
header that differs. `--print-toolchain-fingerprint` recomputes rather than
reading the cache, so it also repairs a stale entry on its way past.

## A cache of its own

A node can hold a cache tier in front of the shared `fastcached`, and point the
launcher at itself:

```sh
fastcache-compile-node \
    --listen-cache=6677 --cache-memory=8g \
    --upstream=build-cache.internal:6674 \
    --scheduler=scheduler.internal:6675 \
    --advertise=worker-01.internal:6676 \
    --toolchain=/usr/bin/g++
```

```sh
export FASTCACHE_ADDR=127.0.0.1:6677   # the node, not the shared cache
```

### Why a second copy is not redundant

The shared cache already holds every object, so caching them again on the node
looks like waste. What the tier saves is not the compile — it is the **round
trip**. A developer who rebuilds the same tree twenty times a day pays that trip
twenty times for objects that never left their machine, and on a slow or lossy
link that is the difference between a cache that helps and one that hurts.

Four rules, and none of them is the obvious choice:

- **A local hit does not consult the upstream at all.** Revalidating would move the
  round trip rather than remove it. It is safe by construction: an object key is a
  digest over the preprocessed text, the arguments, the compiler identity and the
  dependency set, so a key that matches names the same object.
- **A local miss populates the tier from the upstream.** Without that the tier is a
  proxy and the second build is exactly as slow as the first.
- **A store writes local first, then offers upstream.** The local write must not fail
  for a reason the network chose. Offering it to the fleet is best-effort: a shared
  cache that cannot be reached costs the fleet one entry and costs this machine
  nothing.
- **An unreachable shared cache is a miss, not an error.** Every caller compiles
  either way, so a build never fails because a cache was down.

`scripts/dist-compile-e2e.sh` asserts the first of those by **stopping the shared
cache** and requiring the next compile to still hit, with a byte-correct object.

### `--upstream` may be empty

That is the honest configuration for one developer's machine, not a broken one: the
tier caches locally and never tries to reach a fleet. `--cache-memory=0` turns the
local cache off, which is what a node that only compiles for others wants; a node
that serves this machine's builds should keep it.

### Reading it

Seven counters on `/metrics`, and the splits are the point:

| Series | Says |
|---|---|
| `fastcache_node_cache_hits_total` | Served without touching the network. |
| `fastcache_node_cache_misses_total` | The local tier did not hold it. |
| `fastcache_node_cache_upstream_hits_total` | The shared cache answered after a local miss. |
| `fastcache_node_cache_fill_failures_total` | The upstream supplied it and the local tier refused. |
| `fastcache_node_cache_store_failures_total` | A local write failed — this one is reported to the client. |
| `fastcache_node_cache_upstream_stores_total` | The fleet accepted an object this node offered. |
| `fastcache_node_cache_upstream_store_failures_total` | The fleet would not take it. |

A high **upstream**-hit rate against a low **local**-hit rate means the tier is too
small for this machine's working set — a different problem from a fleet that is
missing a lot, and a different fix. An upstream *store* failure says the fleet is
unreachable; a local store failure says this node is broken.

### It answers where `fastcache-cc` already looks

`--listen-cache` defaults to **`127.0.0.1:6674`** — the address the launcher uses
when nobody sets `FASTCACHE_ADDR`, and the one `cmake/portable/CompileCache.cmake`
passes. So the whole thing works with no configuration: start a node, build, and
the launcher finds it.

That port is also `fastcached`'s. On a machine running both, one of them loses the
bind and the node refuses to start saying so — which is the right outcome, because
they are two ways to serve one port and a node silently losing the race would leave
your builds talking to the daemon while its own tier sat unused. Give one of them a
different port, or run one.

### Who may use it

**Local clients and cluster members. Nobody else, by default.**

| Caller | Cache (`--listen-cache`) | Fleet (`--listen-scheduler`) | Compile (`--port`) |
| --- | --- | --- | --- |
| A process on this machine | always | always | always |
| A `--fleet-member` peer | yes | yes | yes |
| Anyone else | refused | refused | refused |

The **compile port** is the one that matters most. `--bind` defaults to `0.0.0.0`
because peers have to dial it, so without a check anybody who could route to that
port could have this machine run their compiler on source they chose. It is refused
before the request payload is read — a caller with no claim on this machine must not
be able to make it buffer a multi-megabyte translation unit first, which would be a
memory-exhaustion hole opened by the check meant to close one.

Two mechanisms, and both are needed:

- **The bind.** `--listen-cache` takes loopback for a bare port, the opposite of
  `--listen-scheduler`'s wildcard. A scheduler no peer can dial does nothing, while
  a cache any host can dial is this machine's entire build output served to
  strangers.
- **The membership check.** A bind is not a policy. If you widen the cache to share
  the tier with your peers — `--listen-cache 0.0.0.0:6674` — only this machine and
  your `--fleet-member` hosts are still admitted; everyone else gets a typed
  `not-a-member` refusal rather than a dropped connection.

**This machine is always a member of its own fleet**, whatever `--fleet-member`
says. Anti-leeching exists to stop *other* machines spending capacity they do not
contribute; a process here already has this machine's CPU. Without that rule a node
whose operator had listed their peers would refuse their own builds — a fleet that
looks configured and serves nobody locally.

It is deliberately stricter than `fastcached`'s own cache, which serves non-members
on purpose. That one is shared infrastructure somebody operates; this is a
developer's private tier. The two are different things that happen to speak one
protocol.

On a shared multi-user machine, "local" means every account on it. That is the same
trust level the daemon assumes; use `--requirepass` if it is not the one you want.

## A cluster, and who leads it

Run one node and it leads itself: it schedules for its own machine, nobody else's,
and that needs no configuration. That is the common deployment and the default.

Run several and exactly one of them must schedule at a time. Without consensus
every node believes it does — and two nodes handing out the same machine's slots is
not a degraded fleet, it is the one thing the architecture says only one node may
do. So a fleet gives each node an identity and tells it who its peers are:

```sh
fastcache-compile-node     --node-id=n1 --listen-raft=6680     --raft-peer=n1=10.0.0.1:6680     --raft-peer=n2=10.0.0.2:6680     --raft-peer=n3=10.0.0.3:6680     --listen-scheduler=6675 --advertise=10.0.0.1:6676     --toolchain=/usr/bin/g++
```

Each peer is an **identity and an address in one token**, because they are one
fact. A member id with no address is a node the cluster counts towards every quorum
and cannot reach: the fleet is then one node short of forming one, and nothing says
why.

A node must name itself among its own peers, and it is refused at startup if it
does not — such a node could never win a vote and could never be voted for, so it
would stand for election forever against a cluster that has never heard of it.

### Two ports, and why a member records both

`--raft-peer` names the **consensus** port. A client that is redirected to the
leader needs the **scheduler** port, which is a different number, so a member
carries both — and that is a correctness matter rather than a convenience. A
follower refusing a client answers `NotLeader` *with the leader's endpoint*, and
while only one address was recorded that endpoint was the consensus one: the client
took the advice and spoke the scheduler protocol at a socket that has never heard of
it.

Only the *leader's* scheduler port matters, and only the node itself knows it — no
peer ever dials it, so there is nothing to learn it from. So **a node announces its
own record when it becomes leader**, and the address it announces is the host from
its own `--raft-peer` entry with the port its scheduler surface actually bound.
Neither half can supply the other: `--listen-scheduler=6675` binds the wildcard,
which no client can dial, while the consensus endpoint is dialable by construction
and names the wrong port.

A member that has never led carries no scheduler endpoint, which is not a fault:
there is nowhere to redirect to a node that does not lead, and a follower answering
`NotLeader` with nothing is exactly the "an election is in progress" case a client
already handles by compiling locally.

### What the log carries

Cluster configuration and nothing else: **who is a member, where they answer**, and
the handful of settings every member must agree on. Not the cache — a log is
replicated to every member and kept until it is snapshotted, and multi-megabyte
objects written constantly are the opposite of what belongs in one. Cached objects
live in the `fastcached` this state merely names.

| Setting | Means |
| --- | --- |
| `upstream` | host:port of the shared `fastcached` every member reads through to |
| `fleet-open` | `1` to admit every caller to the fleet, `0` for members only |

A key the build does not know is **refused when it is proposed**, not stored. The
alternative is a typo replicated to every node, snapshotted, carried across
restarts — and doing nothing, with the only symptom being that the thing you
configured did not happen.

### Membership at runtime

`--raft-peer` is the **bootstrap** set. Once the cluster is running, membership is
a replicated log entry, and that is what makes a node admitted at runtime survive a
restart without anybody editing a config file on every other machine.

The agreed member set becomes the fleet's admission policy directly, so a node the
cluster admitted is served by all three surfaces at once — its compile port, the
scheduler, and every member's cache tier. `--fleet-member` is the answer before a
cluster exists; this is the answer once one does.

**It reaches consensus too.** The leader moves the *quorum* to match the member set
one machine at a time, so a node the cluster admitted votes, is counted, and is
dialled by the peers that admitted it. Growing a cluster no longer means restarting
its existing members with a longer `--raft-peer` list — only the new machine is
started, and only it names anybody.

### Adding a machine to a running cluster

Two commands, on two machines. The joining node is started with `--raft-join`:

```sh
fastcache-compile-node \
    --node-id=n4 --raft-join \
    --listen-raft=6680 \
    --raft-peer=n4=10.0.0.4:6680 \
    --raft-peer=n1=10.0.0.1:6680 --raft-peer=n2=10.0.0.2:6680 --raft-peer=n3=10.0.0.3:6680 \
    --listen-scheduler=6675 --advertise=10.0.0.4:6676 \
    --toolchain=/usr/bin/g++
```

and any member of the cluster is then told to admit it:

```sh
fastcache-compile-node --scheduler=10.0.0.1:6675 --cluster-admit=n4=10.0.0.4:6680
```

**`--raft-join` is not optional and its absence is not a smaller mistake.** Without
it that command line bootstraps a cluster *of n4*: it elects itself, takes a term
and a log of its own, and afterwards refuses `AppendEntries` from every leader its
own configuration does not name. A cluster that admitted such a node would be
counting towards its quorum a machine that answers nobody, and two clusters cannot
be merged by any local rule — so the joining node must never form one.

**Under `--raft-join` the `--raft-peer` list means something else**: these are nodes
this one can *reach*, not a cluster it belongs to. It still needs the cluster's
addresses, and that is load-bearing rather than convenient. A leader admitting a new
member starts replicating at its own last index; the joiner's log is empty and
refuses that; and the leader only walks back to the beginning when the refusal
reaches it. A joiner that cannot send one is admitted, dialled, and permanently
silent.

With `--discovery` the same node names only itself, because discovery supplies the
addresses — see below.

**Nothing about the existing members changes.** They are not restarted, their
command lines are not edited, and the new member survives *their* restarts as well
as its own, because it is a log entry rather than a flag.

### Changing it while it runs

The log carries the cluster's configuration so it can be changed without editing a
file on every machine and restarting them. Three flags ask a running cluster
directly, and each exits when it has an answer:

```sh
fastcache-compile-node --scheduler=10.0.0.1:6675 --cluster-status
fastcache-compile-node --scheduler=10.0.0.1:6675 --cluster-set=upstream=cache.internal:6674
fastcache-compile-node --scheduler=10.0.0.1:6675 --cluster-admit=n4=10.0.0.4:6680
fastcache-compile-node --scheduler=10.0.0.1:6675 --cluster-forget=n3
```

`--cluster-status` prints the members, the settings, and **every key this build
knows** — because the question an operator usually has is "what *can* I set", and a
report listing only what somebody had already set would answer it wrongly by
omission.

**They go through the same gate as everything else on that port**, which for a read
is worth stating: a follower's copy of the state is perfectly valid and merely
older, so `--cluster-status` could have been answered by any member. Refusing and
naming the leader keeps one rule for the whole surface — a verb added without the
gate is the regression the arrangement exists to make impossible — and it sends you
to the node you would have needed anyway to change anything. A follower answers with
where to ask instead:

```
fastcache-compile-node: this node does not lead the cluster; ask --scheduler=10.0.0.2:6675 instead
```

**A non-member is refused too**, and here anti-leeching is not about capacity: a
stranger who could set `upstream` would point the whole fleet's cache at a host of
their choosing.

**A node running no cluster says so** rather than answering as though it had one. A
single node started without `--node-id` leads itself and has no replicated state,
which is a different fact from "ask somebody else" — being sent elsewhere would have
you looking for a node that does not exist.

**A change is reported as accepted, not as committed.** The leader appends the entry
and answers; whether a majority has taken it is not something it knows yet. Ask for
the status again to see the result — which is the round trip you were going to make
anyway.

**`--cluster-admit` takes the same token `--raft-peer` does**, and for the same
reason: an id with no address is a node the cluster counts towards quorum and never
reaches. One verb covers adding a member and recording that one has *moved*, because
they are one intention — a node that moved has the same identity and a new address,
and making an operator remove it first would leave a window in which the cluster has
agreed it does not exist.

**`--cluster-forget` is the one membership change nothing automatic makes.**
Discovery only ever adds, for the reason below, so removing a machine that has left
for good is a decision somebody makes on purpose. It takes the member out of the
quorum as well as out of the fleet — but only a member that was **admitted at
runtime**, which is what tells "the operator forgot it" apart from "nobody ever
wrote it down". A member a machine names in its own `--raft-peer` list is a member
by that operator's assertion: forgetting it removes the record every surface reads,
and the quorum goes on counting it. Taking a *typed* member out of the quorum means
dropping it from `--raft-peer` on the machines that name it and restarting them —
a leader never proposes removing a member its own bootstrap list asserts.

### Finding peers instead of typing them

`--raft-peer` works and needs no network magic, but it means editing a file on every
machine each time one joins. `--discovery` replaces the editing with a broadcast:

```sh
head -c 32 /dev/urandom | base64 > /etc/fastcached/cluster.key   # once, per fleet
chmod 600 /etc/fastcached/cluster.key                            # then copy it around

fastcache-compile-node \
    --node-id=n1 --listen-raft=6680 --raft-peer=n1=10.0.0.1:6680 \
    --discovery=255.255.255.255:6681 \
    --cluster-key-file=/etc/fastcached/cluster.key \
    --cluster-id=build-farm \
    --listen-scheduler=6675 --toolchain=/usr/bin/g++
```

Every node still names **itself** in `--raft-peer`, because that is the address its
peers dial and only it knows it. What it no longer has to name is anybody else.

**The key authenticates a handshake; it never travels in a beacon.** A beacon says
what a node *is* — cluster, id, consensus endpoint — and nothing derived from the
key, because a broadcast reaches every listener on the segment and anything
key-derived in one hands them what they need to join. The key appears only inside an
HMAC over a nonce the challenger chose, and that MAC covers the `(node, endpoint)`
**pair**: signing the nonce alone would let anyone who observed one valid proof
replay its tag with a different endpoint substituted, admitting a legitimate node id
at an attacker's address.

**`--cluster-key-file` is a file and not a flag**, and that is the whole reason it
exists in that shape. A command line is readable through `ps` on every POSIX system,
and a service's arguments end up in a unit file or a registry key that more accounts
can read than can read a mode-0600 file. A leaked key admits a node, an admitted node
is assigned compile jobs, and the objects it returns are cached fleet-wide — so it is
object injection into everybody's build.

**`--cluster-id` is routing, not authentication.** It is plain text in every beacon,
so treating it as a credential would be the mistake. What it buys is that two
unrelated fleets on one segment ignore each other, which holds even when somebody
shares a key across fleets — which they should not.

**Discovery never changes membership by itself.** It answers who proved the key and
where they answer; the *leader* proposes, and only the leader, because admitting a
node is a Raft decision. Every node on the segment sees the same peers and all but
one of them do nothing about it.

**A node joining a discovered fleet still needs `--raft-join`**, and needs nothing
else: discovery supplies the addresses that a typed join has to list by hand. One
node bootstraps the cluster and the rest join it — and exactly one, because two
nodes that each bootstrapped a cluster of themselves cannot be merged. A membership
change proposed against such a node never commits, and the leader says so once the
wait becomes unreasonable rather than leaving it to be inferred.

**It never proposes a removal either.** A peer vanishes from a broadcast for reasons
that are almost never "it left" — a lost datagram, a switch rebooting, a laptop
closed for an hour — and a cluster that re-computed its membership from reachability
could shrink itself below a majority and never come back. Raft already tolerates a
member that does not answer. Removing one stays an operator decision.

### Where its state lives

`--cluster-dir`, defaulting to `fastcache-cluster/<node-id>`. Durable by necessity
rather than by preference: a node that answered a vote and forgot it would vote
twice in one term after a restart, which is two leaders in one term.

## Running it as a service

### Linux

The package ships a socket-activated unit. Enable the **socket**, not the
service:

```sh
sudoedit /etc/fastcached/compile-node.env     # scheduler, advertise, toolchains
sudo systemctl enable --now fastcache-compile-node.socket
```

Socket activation means systemd owns the port: it answers from boot, so a client
that leases this worker never races its startup, and an idle worker costs
nothing — which suits a compile fleet, where misses on a warm shared cache are
bursty and rare.

The service runs as its own `fastcache-node` account, deliberately not
`fastcached`'s: a worker runs a compiler on input that arrived over the network,
while `fastcached` owns the cache storage, and sharing an account would let a
compromised compile rewrite every cached object.

`systemctl edit fastcache-compile-node` for local overrides; the shipped unit is
replaced on upgrade.

### macOS and Windows

```sh
fastcache-compile-node --install-service \
    --scheduler=cache.internal:6675 \
    --advertise=worker-01.internal:6676 \
    --toolchain=/usr/bin/c++ \
    --service-scope=user            # macOS: registers a launchd agent for you
```

Every other flag on that command line is **baked into the registration** and
reused at every start, so this is also where a wrong one is expensive. Three
things are therefore refused at install time rather than at the next boot:

| Missing | Why it is refused here |
|---|---|
| `--advertise` | Without it the registration bakes in `{--bind}:{--port}`, and the default `0.0.0.0` is not an address a client can dial. Such a worker registers, heartbeats, is leased out, and is never reached — with no error at either end. |
| `--scheduler` | The service would start and exit at every boot. |
| `--toolchain` | The worker would register and then refuse every job sent to it. |

`--requirepass` is refused too, for the reason it is on the daemon: a supervisor
records launch arguments where every local account can read them, and for a
worker that token is what the scheduler authenticates it *by*. Put it in a
config file the service account can read, or set it with a supervisor override.

**macOS scope.** `--service-scope=user` registers a LaunchAgent that runs as
you, which is the per-developer case and works today. `--service-scope=system`
registers a LaunchDaemon that must run as the unprivileged `fastcache-node`
account — the same one the Linux unit uses — and **is refused until that account
exists**, because a system job with no account named runs as *root*, and this
process compiles input that arrived over the network. Creating it is packaging
work that has not landed ([#87](https://github.com/LASTRADA-Software/fastcached/issues/87)).

**Windows** registers an SCM service (auto-start, left stopped; `sc start
FastCacheCompileNode`). The default service name is `FastCacheCompileNode`, not
the daemon's `FastCached`, so a machine can run both without one install
displacing the other.

Remove a registration with `--uninstall-service` (and the same
`--service-scope`, on macOS: which domain a job lives in is decided at install
time and re-probing would boot out one that was never there).

## Capacity

Say nothing and the worker sizes itself. It takes its hardware threads, clamps
that by what its memory supports, and subtracts what its **node class** reserves:

| `--node-class` | Cores held back | For |
| --- | --- | --- |
| `workstation` (default) | 2 | a machine somebody is sitting at |
| `dedicated` | 0 | a machine nobody is sitting at |

`workstation` is the default because it is the **safe** answer, not the common
one. A node whose class nobody set is somebody's desktop until proven otherwise,
and getting that backwards is a failure the person experiences as "my editor
stutters" and never connects to a build fleet. Two cores rather than one: a
modern editor, its language server and a browser will each want one, and the
point of the reserve is that the machine stays usable while it contributes.

```sh
# A build server. Nobody is at it, so drive it to its limit.
fastcache-compile-node --node-class dedicated ...

# A workstation whose owner wants four cores kept free, not two.
fastcache-compile-node --reserve-cores 4 ...

# A workstation whose owner wants none kept free. This is NOT the same as
# omitting the flag -- see below.
fastcache-compile-node --reserve-cores 0 ...
```

`--reserve-cores 0` and omitting `--reserve-cores` are different instructions.
Omitting it means "reserve whatever the class reserves"; typing zero means
"reserve nothing". A worker that could not tell them apart would have to pick
one, and one of the two answers is somebody's desktop becoming unusable.

### The memory clamp

One job per core is wrong on a machine whose cores outrun its RAM. A C++
translation unit with heavy template instantiation routinely peaks in the
hundreds of megabytes, so a 128-thread box with 32 GiB asked for 128 concurrent
compiles swaps, or the OOM killer takes them — and those come back as refusals
the client retries locally, so **distribution appears to work while making the
build slower than not distributing at all**. The worker therefore also caps
itself at one job per gigabyte of physical memory. It is deliberately generous
enough not to bind on any machine with a gigabyte per thread, which is every
ordinary build host.

### `--slots` overrides all of it

A number given to `--slots` is the answer, not a hint: it is neither capped at
the core count nor reduced by the class reserve nor clamped by the memory
heuristic. You are the person whose machine this is. Capping it would silently
refuse the deliberate oversubscription an I/O-heavy build wants, and subtracting
the reserve on top of it would make `--slots 4` on a workstation quietly offer
two, which is not what the flag says.

Whatever the number ends up being, it is advertised to the scheduler **and**
enforced locally, from one calculation — a worker running to one number while
the scheduler leases against another is exactly the overload the cap exists to
prevent. A job over the cap is refused rather than queued, because the client has
a local compile waiting either way and queueing only hides the overload from the
scheduler trying to route around it.

### Withdrawing while the machine is busy

The class reserve is static — two cores held back permanently. What happens when
the machine's owner starts using six more of them is the other half, and it rides
on the heartbeat: every 20 seconds a worker reports its host CPU, its available
memory and the free space where it compiles, and the scheduler subtracts what
that leaves from the slots it may be given.

Three things about it are worth knowing:

- **The fleet's own jobs are subtracted first.** Without that, giving a machine
  work raises its CPU, which withdraws the capacity that let it take the work,
  which frees the CPU — a fleet that oscillates for reasons nobody can see from
  either end. So only load that is *not* this fleet's counts against it.
- **A worker can withdraw to zero, and come back.** Unlike the registered slot
  count, which never reaches zero, the live figure may: a machine whose scratch
  filesystem has filled cannot compile anything, and continuing to send it jobs
  would only produce refusals. It is picked again as soon as it says so.
- **Absent is not zero.** A worker whose platform will not report its CPU is
  scheduled on everything else, not treated as idle *or* as saturated.

The refusal an operator sees distinguishes the two cases, because the fixes are
opposite:

| Refusal | Counter | What to do |
| --- | --- | --- |
| `no-worker` | `fastcached_dispatch_leases_no_worker_total` | Nothing serves that toolchain — a fingerprint mismatch. |
| `no-capacity` | `fastcached_dispatch_leases_no_capacity_total` | The fleet is full of your own build. Add machines. |
| `withdrawn` | `fastcached_dispatch_leases_withdrawn_total` | The machines are there and unavailable — somebody is using them, or a disk is full. |

Never sum them. `withdrawn` folded into `no-capacity` reads as "the fleet is too
small", so a fleet whose build hosts have all filled their scratch disks would
send you shopping for hardware you already own.

### How the scheduler picks

Among workers with a byte-identical toolchain fingerprint, the one with the most
**free slots** wins — not the one running the fewest jobs. Absolute counts treat
every machine as an identical box, so a 64-slot server running 8 jobs looks
busier than a 4-slot laptop running 2, when the server has 56 slots free and the
laptop has none. Across a fleet of mixed machines, which is the ordinary case,
that sends work to the smallest machines first and leaves the big ones idle.
Equal headroom is broken by utilization, so between two workers with four slots
free the one with proportionally more of itself left takes the job.

## Watching one

`--admin-listen` serves `/metrics` and `/healthz`, and is **off unless you ask
for it**: a scrape surface reachable from the network is a decision, not a
default. A bare port binds loopback, so `--admin-listen 6675` is reachable from
the machine and nowhere else; write `--admin-listen 0.0.0.0:6675` when you mean
the network.

```sh
fastcache-compile-node --scheduler cache.internal:6674 \
                       --toolchain /usr/bin/c++ \
                       --admin-listen 6675
curl -s localhost:6675/healthz     # 200 while the worker is answering
curl -s localhost:6675/metrics     # Prometheus exposition
```

It is the same endpoint and the same renderer the daemon serves — a worker has
no cache, so the cache series are **absent** rather than present and zero, which
a dashboard would otherwise read as an empty unbounded cache rather than as no
cache at all.

`/healthz` is worth wiring even if you never scrape: without it a supervisor can
tell that the process is alive but not that it is *answering*, which is the state
a wedged worker is in. It is what `systemd`'s and Kubernetes' probes want.

What the counters mean is tabulated under
[Distributed compilation](../getting-started/distributed-compilation.md#confirming-it-works).

## What a worker will not do

- **Run a program a client named.** The compiler comes from `--toolchain`.
- **Touch a path a client named.** The object path and the directory are the
  worker's own, inside a per-job scratch directory it creates and removes. A
  command line carrying anything that could name a file is refused outright, on
  both ends — the client's check protects an honest client from dispatching
  something that would not work, and the worker's protects it from a client that
  is not honest.

  The one thing a client does get to choose is what its translation unit is
  **called**, because a compiler records the name of the file it was handed and an
  object built under an invented name is gratuitously different from a locally
  built one. The name is reduced to a single component and an allow-listed shape
  before it becomes a path — no separators, no parent-directory segments, no
  drive letters, a bounded length, an extension from a fixed set, and never a
  Windows device name such as `CON` — and anything failing that is compiled as
  `tu.cpp` rather than refused. **The language never rides on it:** the client
  states the language explicitly (`-x c++-cpp-output`, `/TP`), so a name the
  worker had to invent cannot decide how the text is compiled.
- **Write to the cache.** Workers get no cache credentials.

## Security

The `0xFC` surface is authenticated: start the scheduler with `--requirepass` and
give workers and clients the same secret (`--requirepass` on the worker,
`FASTCACHE_TOKEN` on the client).

Keep `--listen-scheduler` off any network you would not run a compiler for. That
is why it is a separate process from the cache: the cache may reasonably be
reachable across a build LAN, while the surface that makes a compiler *run* on
another machine should be firewalled separately. A scheduling verb arriving at a
`fastcached` listener is refused with a typed reply naming where the scheduler
went.

`--fleet-member` restricts which hosts may reach **all three** of this node's
surfaces: its compile port, its scheduler, and its own cache tier. This machine is
always admitted, whatever the list says, so a node is useful to its owner with no
configuration and closed to the network until they name a peer.

Note where this differs from `fastcached`. There, membership is a policy about
*contribution* and a non-member still reads and writes the shared cache — that
cache is shared infrastructure somebody operates. A node's tier is a developer's
own build output, and its compile port is its own CPU, so both are closed by
default. Membership still complements `--requirepass` rather than replacing it: one
is about who you are, the other about what you know.

For anything beyond a trusted build network, put mTLS in front of every port.

## Known limitations

- **Preprocessing does not distribute.** The client must preprocess to compute
  the cache key before it knows there is a miss, so at roughly 45 ms against
  compiles of 300 ms–2 s the ceiling is about 10–40×, not linear.
- **`-g` embeds the worker's scratch path** in DWARF. Use
  `-fdebug-prefix-map`/`-ffile-prefix-map` if that matters to you.
- **Diagnostics from a failed remote compile are not shown.** A worker that
  reports a non-zero exit is retried locally and the *local* result is what you
  see, which also regenerates diagnostics with correct line numbers.
- **On MSVC a dispatched object is not byte-identical to a locally compiled
  one — the code in it is.** Measured on MSVC 14.51 and clang-cl, three things
  differ and no more:

    - every MSVC-family driver stamps the **clock** into the COFF header (two
      compiles of one file to one path two seconds apart differ in exactly byte
      4; `/Brepro` is what suppresses it);
    - `cl` records the **absolute path of the object file** in `.debug$S`, even
      without `/Zi`;
    - `cl` hashes the source file it opened into `.chks64`, and a worker opens
      its own scratch file.

    Everything carrying code or data is byte-identical: same compiler, same
    flags, same preprocessed input. What it affects is debugging, in the same way
    `-fdebug-prefix-map` addresses for GCC and clang — a debugger will need
    `/PDBALTPATH` or an equivalent source-path mapping to find your sources.

    So the Windows end-to-end fixture compares **section by section** against a
    per-driver table of what may differ, and clang-cl's table is *empty*: it
    records only the source's base name, which the worker is told, so its objects
    differ by the clock alone. The POSIX fixture asserts strict byte-identity and
    should — GCC and clang embed nothing path-dependent without `-g`. If your
    build compares object bytes across machines, compare sections.

## Not yet done

- The macOS package does not create the `fastcache-node` account, so
  `--install-service --service-scope=system` refuses there
  ([#87](https://github.com/LASTRADA-Software/fastcached/issues/87)).
  `--service-scope=user` works, and is the right answer on a developer machine
  anyway.
