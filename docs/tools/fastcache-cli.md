# `fastcache-cli`

An operator's client for a running `fastcached`. It reads and writes the
keyspace, and it gathers statistics — the part that has otherwise needed
`telnet`, a browser, or a `redis-cli` that knows nothing about this daemon's
richer surfaces.

```console
$ fastcache-cli set greeting hello
$ fastcache-cli get greeting
hello
$ fastcache-cli stats --format=json | jq .keyspace_hits
```

`fastcache-cli --help` is generated from the same tables that drive parsing, so
it is never out of date with what the binary accepts. This page carries the parts
a table cannot: why the output and the exit codes are shaped the way they are.

`fastcache-cli help` is accepted as well and does the same thing — exit `0`, on
stdout, coloured when the terminal takes colour. It is the word people type, and
answering it with *unknown command* while printing the help text anyway was the
worst of both: the text appearing looked like success while the exit code was `2`
and the output went to stderr, where a pipe swallows it. The word is only a help
request in the **command** position: `get help` reads a key called `help`, and
`set k help` stores that value, because a cache stores arbitrary bytes.

Nothing after `--help` or `--version` can change *what* they answer — that is
what makes them questions about this binary rather than about the command line —
but `--color` still says how the answer is drawn, on either side of them. An
unknown flag after them is ignored rather than replacing the answer with a
complaint about a flag you are no longer going to use.

## Where it connects

| | |
|---|---|
| Data port | `--addr=<host:port>`, `$FASTCACHE_ADDR`, default `127.0.0.1:6674` |
| Admin surface | `--admin-addr=<host:port>`, `$FASTCACHE_ADMIN_ADDR` — **usually unnecessary**, see below |
| Credential | `--token-file=<path>` (preferred) or `$FASTCACHE_TOKEN` |

The same `$FASTCACHE_ADDR` that `fastcache-cc` reads, so a machine configured for
the launcher is already configured for this. A variable that is *set but empty*
counts as unset, which is what a build that wants no configuration exports.

`--addr` takes `host:port` and refuses a bare port, because a bare port names no
machine. IPv6 literals are bracketed: `--addr=[::1]:6674`.

Prefer `--token-file` to the environment variable. The path is not the secret;
the file is, and an environment variable is visible to anything that can read
this process's environment.

**`--admin-addr` is an override, not a requirement.** A `fastcache-compile-node`
knows which port its admin surface bound and whether it is TLS, and it will say so
over the `0xFC` wire — so `stats` asks it rather than making an operator who can
already reach the node supply a second address. The flag stays for the deployments
where the node is reached through something that rewrites ports, and when it is
given it wins.

The *host* is never taken from the node. A node reports a **port**; the host dialled
is the one this client already reached it on, because that is the only address known
to route from here — a node behind NAT would otherwise hand out an address only its
own network can use.

### Three kinds of endpoint

`fastcache-compile-node` speaks the `0xFC` compile-cache wire and **nothing else**:
no RESP, no memcached text. Pointed at one, this tool used to report *the server
closed the connection without answering* — true, and useless, because it describes
what happened rather than what to do.

So when a verb's own wire cannot answer, the endpoint is identified and named:

```console
$ fastcache-cli get some-key --addr=127.0.0.1:6674
fastcache-cli: 127.0.0.1:6674 is a fastcache-compile-node: it speaks the 0xFC compile-cache wire only, holds no user keyspace, and cannot answer `get` -- try `node` and `node-metrics` here, or point --addr at a fastcached
fastcache-cli: the server closed the connection without answering
```

That probe runs on the **failure path only**, so an ordinary command against a
healthy daemon pays nothing for it.

A verb whose *question* a node can also answer is answered rather than explained.
`version` is the one such verb today:

```console
$ fastcache-cli version --addr=127.0.0.1:6674
client       0.2.0-125-g6ba32b30
server       0.2.0-125-g6ba32b30
server_kind  fastcache-compile-node
```

`server_kind` is there because a node and a daemon version alike and are different
programs; two bare version strings would read as two builds of one binary.

A cache verb has no `0xFC` equivalent and never will — a compile node holds no user
keyspace — so those are refused by name rather than retried somewhere they cannot
work.

## Commands

Run `fastcache-cli --help` for the current list with operand counts. Today:

| | |
|---|---|
| Read | `get`, `mget`, `exists`, `ttl`, `info`, `stats`, `version`, `ping`, `echo` |
| Write | `set`, `del`, `incr`, `decr`, `incrby`, `decrby`, `expire`, `persist`, `flush` |
| Read (memcached) | `gat`, `gats`, `inspect`, `mc-stats` |
| Write (memcached) | `touch`, `add`, `replace`, `append`, `prepend`, `cas`, `cache-memlimit` |
| Node (`0xFC`) | `node`, `node-metrics` |
| Cluster (`0xFC`) | `cluster-members`, `cluster-settings`, `cluster-set`, `cluster-forget`, `cluster-admit` |

### The cluster verbs

They go to the same `0xFC` address as `node` and `node-metrics` — there has been one
such port since #290 — so they need no extra flag. The leader answers them; anyone
else refuses with `NotLeader`, which this tool **follows** rather than relays:

```console
$ fastcache-cli cluster-members --addr=10.0.0.8:6674
fastcache-cli: 10.0.0.8:6674 does not lead the cluster; ask 10.0.0.7:6674 instead
```

An election in progress is a different fact, and gets a different sentence — there
is no address to offer, and saying *ask nobody instead* would be worse than saying
nothing. The two are separated by whether the refusal's message parses as an
address, never by whether it is empty: an empty one never reaches the wire.

`cluster-members` and `cluster-settings` both ask the one wire verb
(`ClusterStatus`) and report different halves of its answer, because each is a
table in its own right and this tool's unit is a table `--format=json` can carry.

`cluster-settings` lists **every setting this build knows**, whether or not the
cluster has agreed one — the question is usually *what can I set*, and a report
showing only what somebody already set answers it wrongly by omission. A setting
the cluster has agreed and this build does not know keeps its row too, with no
summary: a fleet is permanently mid-upgrade, and dropping the row would hide a live
fact because the reader is the older binary.

Absent is not empty in either table. A member that has never led carries no
scheduler endpoint — a leader announces its own record on election — so that cell
reads as absent rather than as an address nothing answers at.

The three changing verbs report **accepted**, never committed. The leader cannot
know the difference until a majority answers.

The same four verbs are also spelled `fastcache-compile-node --cluster-status`,
`--cluster-set`, `--cluster-forget` and `--cluster-admit`. Prefer these: that path
needs `--scheduler`, which is also a **startup** flag, so putting it in a unit file
to run one admin command points that node at one scheduler forever — a registration
replays its command line. A `--fleet-member` client has no node binary at all.

A modifier that means nothing for a verb is **refused**, not ignored:
`set k v --raw` is a usage error rather than a store that silently prints
nothing. `--ttl`, `--nx` and `--xx` belong to `set`; `--raw` to `get`; `--all` to
`flush`; `--ttl` also to `add`, `replace` and `cas`, but **not** to `append` or
`prepend`, whose server-side path takes no expiry at all — accepting it there
would discard it silently.

### The node verbs

`node` and `node-metrics` travel over `0xFC` and are the **only** verbs a
`fastcache-compile-node` answers.

`node` reports what the endpoint is:

```console
$ fastcache-cli node
version                        0.2.0-125-g6ba32b30
node-id                        -
uptime-seconds                 15
components                     cache-tier, worker
toolchains                     surveying
toolchains-served              0
toolchains-discovered          3
compile-slots                  8
compiles-in-flight             0
registrars-registered          0
registrars-total               3
last-registration-seconds-ago  -
admin-port                     36742
admin-tls                      false
```

A surface the node does not run gets **no field at all** rather than a zero port —
a `0` renders as a dialable-looking number in every format, and an operator who
tries it reaches nothing and reports the surface as down. The `0xFC` port is
reported by nobody on purpose: a client learns it by dialling it, and a field that
can only ever be right or stale is worse than none.

`components` is a *reading*, so a node running none says `none` rather than going
absent. A component bit this client has no name for is reported as
`unknown(0x…)` beside the ones it does know — an older client meeting a newer node
says *there is something here I do not understand* instead of quietly
under-reporting.

`toolchains` is what `components` **cannot** tell you. That mask carries a `worker`
bit which is a constant on the node binary — it compiles, that is what it is for —
so it reads identically whether the worker is still identifying its toolchains or
is serving compiles. A node *serves while it identifies them*, and that walk has
been observed running past 300 s on a cold machine, so the state an operator most
often needs is exactly the one the bit could not express:

- **`surveying`** — the first survey has not finished, and `toolchains-served` of
  `toolchains-discovered` is how far it has got.
- **`serving`** — a survey finished and this node serves `toolchains-served`
  toolchains.
- **`nothing-to-serve`** — a survey finished and this node serves none, so it
  accepts no compiles until a later one finds a compiler. The node stays up and
  keeps looking; its log says why.

The three go together or not at all. A node that published none of them — one too
old to carry the record — reports all three **absent** rather than
`surveying, 0 of 0`, which is a reading somebody would act on.

Beside them, what that worker is offering and whether anyone knows about it:

- **`compile-slots`** and **`compiles-in-flight`** — how many concurrent compiles this
  node offers and how many are running. Both or neither: a slot count with no in-flight
  figure invites the reading that the node is idle. A node running no worker tier
  reports neither, which is **not** the same as reporting none free.
- **`registrars-registered`** and **`registrars-total`** — one registration per
  toolchain served, so read them as a pair. `2 of 3` is ordinary while a survey is
  still finishing and alarming an hour later.
- **`last-registration-seconds-ago`** — when a scheduler last accepted one. **Absent
  means never**, and that is the whole reason it is not a number: a node whose
  `--scheduler` has never answered and one that registered an hour ago are exactly the
  two states you are trying to separate. A round that accepts nothing does not reset
  it, so an unreachable scheduler shows a value that keeps growing rather than
  disappearing.
- **`scheduler-role`** and **`leader`** — `leader`, `follower` or `undecided`, and
  where the leader answers. This is the question `components` cannot reach: a leading
  scheduler and a following one both report `scheduler`, and a follower's registry is
  empty and reads exactly like an idle fleet. `undecided` means an election is in
  progress, so a node running **no** scheduler reports no role at all rather than
  claiming to be in one; an `undecided` node reports the `leader` cell as absent,
  because no leader is known yet.

There is deliberately no *limited-by* field beside the slot count. Which ceiling bound a
worker's slots is the **scheduler's** conclusion — derived on the leader from what the
worker reported plus its live load — and a worker recomputing it would be a second
spelling of that arithmetic that can disagree with the first.

`node-metrics` reports every counter the endpoint's build carries, **zeroes
included**. A counter is a tally, so zero is the truth about events that never
happened; dropping the zero rows would make *nothing happened* and *this build has
no such counter* the same answer.

Both are gated on **fleet membership** rather than on a credential, which is what
keeps them usable on a single-machine install: the credential on that listener
belongs to the scheduler, so a node running none has none to check, and demanding
one would leave these verbs permanently unauthenticated on exactly the deployment
they exist for. Loopback is always a member. A remote caller is refused by name:

```console
fastcache-cli: 10.0.0.7:6674 refused `node`: not-a-member (this node reports its identity and counters to fleet members only)
```

The remedy is on the node — `--fleet-member` — not here.

### The memcached-only verbs

The two protocols are not supersets of one another, and the second block above is
what only the memcached text protocol can reach. Three of them are the reason it
is worth speaking at all:

- **`inspect <key>`** is the `me` inspector, and nothing else in this project
  reports a key's last-access time, its cas token or its stored size. The flags
  are renamed for a reader — `exp` becomes `ttl_seconds`, `la` becomes
  `last_access_seconds`, `size` becomes `value_bytes` — and a flag this client
  has no name for keeps its wire spelling rather than being dropped.
- **`cas <key> <value> <cas>`** is the compare-and-swap this cache has and RESP
  does not expose. `gats` and `inspect` are where the token comes from.
- **`mc-stats [family]`** reaches `settings`, `items`, `slabs`, `sizes` and
  `conns`, none of which `stats` above can see. `reset` is deliberately not
  offered: the daemon answers it `RESET` while resetting nothing, so relaying it
  would report a reset that did not happen.

Two spellings are worth reading twice, because both mirror the wire rather than
tidying it:

- **`gat` and `gats` take the expiry FIRST** (`gat 60 key...`) while `touch`
  takes it last (`touch key 60`). That inconsistency is memcached's and this
  daemon's; reordering it here would make a packet capture and `--help`
  disagree. Either spelling fails loudly on the other's input, since one operand
  must be a number.
- **A key carrying a space, a tab or a control character is refused before
  anything is sent.** This protocol has no quoting and no escaping, so such a key
  would arrive as two tokens and address a different one — or, with a carriage
  return in it, end the line early and inject whatever followed as a command.

`cache-memlimit` changes the byte budget **now and not durably**: the value lasts
until the daemon restarts, which then reads `--max-memory` again.

## Output

Human-readable aligned columns by default; `--format=json`, `kv`, `tsv` or `csv`
for anything that parses.

**The format does not change when stdout is a pipe.** Only colour does. A command
whose shape depends on whether it is piped is one that works by hand and breaks
in the script somebody wrote by copying it.

**Remarks go to stderr**, in every format including the human one — which stats
source answered, that a value came back as bytes rather than text, that a key
exists but has no expiry. That is what keeps stdout parseable in all five
formats. Silence them with `2>/dev/null` or `--quiet`.

### Absent is not zero

A value nobody reported renders as a dash in the human format and as `null` in
JSON. It is never `0`, because `0` is a claim: a cache that has served no reads
has *no* hit rate rather than one of 0%.

The converse holds too. A **counter** that has counted nothing renders `0`, since
that is the truth about events that did not happen.

`ttl` is the clearest case, and the reason it reports a record rather than a
number. RESP answers `-2` for *no such key* and `-1` for *exists, no expiry*; as
a bare number both would come out as an absence, separable only by the exit code.
So `exists` is named separately:

```console
$ fastcache-cli ttl b --format=kv     # a live expiry
key=b
exists=true
ttl=299

$ fastcache-cli ttl a --format=kv     # exists, no expiry set
key=a
exists=true
ttl=

$ fastcache-cli ttl gone --format=kv  # no such key; exits 1
key=gone
exists=false
ttl=
```

`--absent=<text>` names a placeholder for the line-oriented formats. It
deliberately does **not** affect JSON: a consumer with a real `null` available
does not need a sentinel, and turning one into the string `"-"` would hand them a
value that parses and lies.

!!! warning "Reading TSV with an absent field"
    `IFS=$'\t' read` does not work when a field can be absent: tab is IFS
    whitespace, so an empty field collapses and shifts every field after it. Use
    `--absent` to name a placeholder, or prefer `csv` or `json`.

### Values that are not text

A cached value is an arbitrary byte string. One that is not valid UTF-8 is shown
base64-encoded, with a remark saying so — not repaired into `U+FFFD`, because a
silent repair is the failure nobody notices.

`get --raw` writes the bytes to stdout untouched, with no formatting and no
trailing newline, for piping to a file. It is the one escape from that
classification.

## Exit codes

Six outcomes, because one code cannot answer six questions:

| | | |
|--:|---|---|
| 0 | `ok` | the command was answered |
| 1 | `no` | answered, and the answer is no — a miss, or no such key |
| 2 | `usage` | the command line was wrong; nothing was sent |
| 3 | `unreachable` | the server could not be reached, or the connection failed |
| 4 | `refused` | the server answered and declined |
| 5 | `protocol` | the reply could not be read; the peer may not be a `fastcached` |

The pairs that matter:

- **1 against 3** — a cache miss and a dead daemon. A script that retries one and
  gives up on the other cannot be written if they agree.
- **4 against 5** — the server said no, versus the server said something this
  client could not read. Different people fix those.
- **3 against 4, pointed at a compile node** — a `fastcache-compile-node` serves no
  keyspace, so `get` there cannot work and never will. It exits **4**, not 3: the
  endpoint answered, and `3` is the code that reads as *retry, the daemon may be
  down*. The advisory names what the endpoint is; the exit code is the same
  correction for a script, which reads nothing else. An endpoint that sends no frame
  at all is still 3 — nothing was established there to correct it with.

```sh
if fastcache-cli get "$key" > value.txt; then
    echo "hit"
elif [ $? -eq 1 ]; then
    echo "miss"          # a real answer
else
    echo "could not ask" # unreachable, refused, or unintelligible
fi
```

## `stats`, and which source answered

There are sources of very different richness and no single best one, so `stats`
walks a ladder, richest first, and **says which rung answered** as a `source`
field of its own output:

| `source` | Where | Size |
|---|---|---|
| `metrics` | the admin surface's `/metrics` | 137 series, measured against a node here |
| `node-metrics` | the node's own `NodeMetrics` verb over `0xFC` | 99 counters, same node |
| `info` | RESP `INFO` on the data port | 7 fields |

`/metrics` needs no credential — it is served above the dashboard's
authentication gate — but it does need the daemon started with its metrics
listener, and it is on a different port. Against a node that port is **discovered**
over `0xFC`; `--admin-addr` is only needed when the discovered answer is wrong for
your topology.

`node-metrics` sits below `/metrics` because it is *narrower*, not worse: it carries
the counter catalogue and not the storage or per-tier series the Prometheus renderer
adds. It sits above `INFO` because it is an order of magnitude wider than seven
fields — and it is the only rung that answers at all against a
`fastcache-compile-node`.

Against a node with no admin surface at all:

```console
$ fastcache-cli stats --format=kv
source=node-metrics
fastcached_connections_total=0
...
fastcache-cli: the node's own NodeMetrics verb over 0xFC returned 99 field(s); this is the counter catalogue only; /metrics adds the storage and per-tier series
fastcache-cli: the admin surface's /metrics endpoint was not asked: 127.0.0.1:36751 runs no admin surface
```

Against a daemon with no metrics listener, `stats` falls back to `INFO` and is
explicit about the difference:

```console
$ fastcache-cli stats --format=kv
source=info
fastcached_version=fastcached-0.2.0
...
fastcache-cli: RESP INFO on the data port returned 7 field(s); start the daemon with its metrics listener enabled, or pass --admin-addr, for the full counter set
fastcache-cli: the admin surface's /metrics endpoint was not asked: no admin address is known and no 0xFC connection was opened to discover one
```

Note **was not asked**, not *did not answer*. Those are different states, and
reporting the second for an endpoint nothing dialled would send an operator to
check a listener that was never contacted. When nothing answers at all, every
source is named along with which of the two states it ended in.

The `source` field is on stdout rather than only in a remark because *which
numbers am I looking at* is a question a script asks too: a dashboard that cannot
tell a 127-series scrape from a 7-field reply will draw the missing 120 as
zeroes.

## Authentication

`--requirepass` on the daemon gates the RESP surface. Present the credential with
`--token-file` or `$FASTCACHE_TOKEN`; `--user` supplies the two-argument `AUTH`
form, which is rarely needed.

A credential configured against a daemon that has **no** password is a remark,
not a failure — the command still runs. That is deliberate: refusing would give a
client with `$FASTCACHE_TOKEN` set a permanent failure against a server that
never needed one. A *wrong* credential, which is about the credential rather than
the server, is fatal.

### The memcached verbs cannot authenticate at all

**The memcached text protocol has no `AUTH` verb.** Against a daemon with
`--requirepass` set, it answers every command but `version` and `quit` with
`CLIENT_ERROR authentication required` and ends the session — deliberately, since
a refused storage command leaves its data block unread and continuing would parse
those bytes as the next command. So every verb in the memcached block above is
unavailable there, and no credential can change that.

The client does **not** pre-emptively refuse them when a credential is
configured, because a credential being configured does not mean the server
requires one — that is the same false inference the remark above avoids, and
refusing on it would decline verbs that work fine against every daemon with no
password. It asks, and explains the refusal when one arrives: the server's own
sentence does not mention that the protocol lacks the verb, so an operator would
otherwise read it as *supply a credential* with no way to.

Use the RESP verbs on the same `--addr` where they cover the need — `get`, `set`,
`del`, `ttl` and `expire` between them cover most of what `touch`, `add` and
`replace` are for.

## Environment

| | |
|---|---|
| `FASTCACHE_ADDR` | the cache's data port, as `host:port` |
| `FASTCACHE_ADMIN_ADDR` | the admin surface, as `host:port` |
| `FASTCACHE_TOKEN` | the credential to present |
| `FASTCACHE_USER` | username for the two-argument `AUTH` form |
| `NO_COLOR` | set to anything non-empty to suppress colour. It governs the **default**, so an explicit `--color=always` still colours |

Precedence is defaults, then the environment, then the command line — each
overriding the last, applied in that order rather than merged field by field.

## What it does not do yet

Stated so it is not rediscovered:

- **No key enumeration.** There is no `list`, because the server exposes no
  `KEYS`, `SCAN` or `stats cachedump` at any layer a client can reach. It needs a
  cursor on the storage engine first.
- **No fleet or cluster verbs yet.** `fastcache-compile-node --cluster-status`
  and the `/fleet` dashboard remain where they are; bringing them here is
  tracked work.
- **The memcached verbs are not routed over the binary protocol.** That protocol
  has SASL and would let them work under `--requirepass`, and doing so is out of
  scope here rather than impossible.
- **No REPL and no live view yet.** One command per invocation, which composes
  with `watch`, `ssh` and a pipe.
- `CONFIG GET` and `CLIENT LIST` are deliberately not exposed: the daemon answers
  both with stubs, and relaying a stub as fact is worse than not offering it.
