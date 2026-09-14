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

Run `fastcache-cli --help` for the current list with operand counts. It groups the
commands under the **wire** each one declares — which kind of server answers it — since
that is the one axis that decides whether a verb can work at all, and it is a fact the
verb table already carried statically while an operator had to discover it by dialling.

The table below splits those same commands by read and write as well, which is the
question you have *after* the first one is settled. Neither list is written by hand:
the help renders `Verbs()` grouped by `WireSpec::heading`, and
`ctest -R cli-verb-docs` refuses a verb that appears in one and not the other.

| | |
|---|---|
| Read | `get`, `mget`, `exists`, `ttl`, `info`, `stats`, `live-stats`, `version`, `ping`, `echo` |
| Write | `set`, `del`, `incr`, `decr`, `incrby`, `decrby`, `expire`, `persist`, `flush` |
| Read (memcached) | `gat`, `gats`, `inspect`, `mc-stats` |
| Write (memcached) | `touch`, `add`, `replace`, `append`, `prepend`, `cas`, `cache-memlimit` |
| Node (`0xFC`) | `node`, `node-metrics`, `fleet` |
| Cluster (`0xFC`) | `cluster-members`, `cluster-settings`, `cluster-set`, `cluster-forget`, `cluster-admit` |

### The fleet verb

Every fleet table — machines, workers, outstanding leases, members, cache tiers — used
to be reachable from a browser and from nowhere else. `/fleet.json` is the only other
door and it needs a JSON parser the operator supplies; `jq` is not on a Windows build
box, and this tool has no JSON *parser* of its own — it only emits one.

`fleet <section>` asks the node's **admin** surface for `/fleet.txt` and renders that
section as a table, so `--format` works on it exactly as on every other verb:

```console
$ fastcache-cli fleet workers --addr=10.0.0.7:6674 --format=json
$ fastcache-cli fleet machines --addr=10.0.0.7:6674 | column -t
```

**It needs no second address.** The node reports which port its admin surface bound,
over the same `0xFC` connection every other node verb uses, so `--admin-addr` is an
override for deployments that rewrite ports rather than something to supply. A node
running no admin surface, one serving it over TLS this client cannot speak, and one
naming the port already being talked `0xFC` to are each refused **by name** — none of
them is "the fleet is down".

`kpi` is one of the sections, and it is the one that is not a row table: the page's
headline strip, one line per figure, keyed by a name rather than by a page label. What
it carries is the number and its scale — never `/ 32 slots`, which a reader would have
to parse a figure back out of.

The section is required, and the reason is the unit: this verb's answer is one table,
and the whole document is every section behind a marker. A default would silently pick
one of them. (No count is written here on purpose — the set grew by one at `kpi` and a
number in this paragraph would now be wrong.) A wrong guess is refused by the leader
with the accepted keys and what each holds, and that refusal is relayed verbatim:

```console
$ fastcache-cli fleet worker --addr=10.0.0.7:6674
fastcache-cli: /fleet.txt?section=worker answered HTTP 400: unknown section; this build serves:
  machines  one row per machine; the grain a fleet total is computed over
  workers   one row per (toolchain, endpoint) registry entry
  ...
```

**The leader answers it and nobody else can**, exactly as for the page: a follower's
registry holds whatever registered against *it*, so it replies `503` naming the leader,
and that is relayed too rather than reported as an unreachable fleet.

Cells arrive as the leader escaped them. A display name holding a tab is `\t` here and
not a real tab — text a peer chose cannot be allowed to forge a column boundary.

They are **not** unescaped on the way through, and the reason is that this tool is a
relay rather than a second author of the rule: what the leader wrote is the leader's
statement about its own fleet, and re-deriving it here would be the same convention
spelled twice, in two binaries, free to drift. Unescaping would also put a real tab back
into a cell that `--format=csv` carries through raw and that the human format prints
into its own aligned columns.

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

In `cluster-members`, `scheduler` is an address or absent, and `scheduler-state`
says which: `announced`, `never-announced` (a member that has not led, which is
ordinary), or `cleared` (a re-admit wiped the endpoint it had; it returns when that
member next leads).

`cluster-settings` lists **every setting this build knows**, whether or not the
cluster has agreed one — the question is usually *what can I set*, and a report
showing only what somebody already set answers it wrongly by omission. A setting
the cluster has agreed and this build does not know keeps its row too, with no
summary: a fleet is permanently mid-upgrade, and dropping the row would hide a live
fact because the reader is the older binary.

Absent is not empty in either table. A member that has never led carries no
scheduler endpoint — a leader announces its own record on election — so that cell
reads as absent rather than as an address nothing answers at.

Two of the three changing verbs report **accepted**, never committed. The leader
cannot know the difference until a majority answers.

`cluster-admit` reports more, and the extra is deliberate rather than a courtesy.
What the leader **recorded** it knows the instant it builds the command, with no
majority involved, so that much comes back:

- `member-id-as-received` — the id, byte for byte as it arrived
- `consensus-endpoint-as-recorded` — the address that goes into the replicated
  configuration
- `state` — **appended, not committed**, which is as strong a claim as a leader can
  truthfully make here

Hold both values against the machine being brought in: the id it minted into its own
`--cluster-dir`, and the address it answers consensus on (`--raft-self` together with
`--listen-raft`). They are two spellings of one thing and nothing else compares them.
When they disagree the member sits in the cluster's configuration and contacts
nobody, which at three members or more presents as an election storm that then
settles — so the symptom points at consensus rather than at the character that was
mistyped, and one address costs an afternoon.

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

### TSV escapes what it cannot carry

A cell in `--format=tsv` has `\t`, `\n`, `\r` and `\` spelled out. Values are not all
ours — a hostname is chosen by the machine that registered it — and a raw tab would
shift every later column while a raw newline would invent a row, both silently, since
the output stays well-formed TSV and only describes different values than it holds.

It is the same convention, in the same spelling, as the node's `/fleet.txt`, so one
script can read both. The backslash is escaped along with the other three so the
mapping can be inverted: without it a name containing a literal `\t` and one containing
a tab would arrive identical.

**CSV is unaffected**, and deliberately: a tab is not special in RFC 4180, so
`--format=csv` carries one through raw and quotes only `,` `"` CR and LF.

### Values that are not text

A cached value is an arbitrary byte string. One that is not valid UTF-8 is shown
base64-encoded, with a remark saying so — not repaired into `U+FFFD`, because a
silent repair is the failure nobody notices.

`get --raw` writes the bytes to stdout untouched, with no formatting and no
trailing newline, for piping to a file. It is the one escape from that
classification.

## Exit codes

One code per outcome, because one code cannot answer several questions. The rows are
`OutcomeTable`'s, word for word — `--help` renders that table, and
`ctest -R cli-exit-code-docs` refuses this listing the moment a code, a name or a meaning
differs from it:

| | | |
|--:|---|---|
| 0 | `ok` | the command was answered |
| 1 | `no` | the command was answered and the answer is no (a miss, or no such key) |
| 2 | `usage` | the command line was wrong; nothing was sent |
| 3 | `unreachable` | the server could not be reached, or the connection failed |
| 4 | `refused` | the server answered and declined |
| 5 | `protocol` | the reply could not be read; the peer may not be a fastcached |
| 6 | `local` | this machine could not carry the command out; the server is not implicated |

The pairs that matter:

- **1 against 3** — a cache miss and a dead daemon. A script that retries one and
  gives up on the other cannot be written if they agree.
- **4 against 5** — the server said no, versus the server said something this
  client could not read. Different people fix those.
- **4 against 6** — the server declined, versus this machine could not do it.
  `live-stats` on a terminal it cannot draw on exits **6**: nothing at the server
  is wrong, and the remedy — redirecting the output, for one line per sample — is
  on this machine.
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
fastcache-cli: the admin surface's /metrics endpoint was not asked: 127.0.0.1:36751 runs no admin surface. Start the node with --admin-listen to open one: it is off unless asked for, so a node without one is configured rather than broken
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

## `live-stats`

`stats` answers once. `live-stats` keeps answering: it draws one subject on the
terminal and redraws it at every sample, or, with its output redirected, writes one
row per sample for a script.

```console
$ fastcache-cli live-stats                              # cache or node, whichever --addr is
$ fastcache-cli live-stats fleet --addr=10.0.0.7:6674   # the fleet, from the node that leads it
$ fastcache-cli live-stats node --format=tsv --samples=30 > node.tsv
```

### Three subjects, three panels

| Subject | Served by | The panel |
|---|---|---|
| `cache` | a `fastcached` | the hit rate, and operations, connections, evictions and expiries per second, each with its trend; connections, items and bytes in use against their limits, per storage tier |
| `node` | a `fastcache-compile-node` | compiles and refusals per minute, the mean compile and its trend; the slots in use against the slots available and the limit that bounds them; the cache tier's fill; the host's CPU and free memory; the node's identity, toolchains, registrars and the leader |
| `fleet` | the node that leads the fleet | the headline figures as tiles; one table per section (machines, workers, leases, members, tiers); each machine's CPU over time |

The subject may be left out, and then it is what `--addr` is: a `fastcached` is
watched as `cache`, and a compile node as `node`. **`fleet` is never inferred.** The
node at `--addr` is a node whichever question is asked of it, and the whole fleet is
a different question from that one machine, so it is asked for by name.

A panel is laid out for the terminal it is drawn on and again whenever the terminal
is resized. At 80x24 it draws the layout the panel was designed at. A taller terminal
gives the rows nothing else wants to a **history chart**: the node's compiles per
minute first, then its refusals, its CPU and its mean compile. Every band gets a row
before any band gets a second one. A wider terminal draws a longer span, one sample
per cell. The chart carries the same explanations at every size:

- a title with the span it covers;
- each band's newest figure and what its top stands for;
- a time axis ending at `now`;
- a legend.

A terminal smaller than a panel's minimum gets one line, `needs <columns>x<rows>, have
<columns>x<rows>`, rather than a frame with pieces missing.

### Keys

| Key | What it does |
|---|---|
| `q`, `Q`, `Esc`, `Ctrl-C` | quit, restoring the terminal |
| `Tab`, `→` / `←` | the fleet's next / previous section |
| `1` … `9` | the fleet's sections by position |
| `m` `w` `l` `c` `t` | the fleet's sections by name: machines, workers, leases, members (`c`, for cluster: `m` is taken) and tiers |
| `PgDn` / `PgUp`, `Home` | scroll the fleet's table a page, or back to its top |
| `/` | filter the fleet's table: type, `Enter` keeps the filter, `Esc` clears it, `Backspace` edits it |

A section starts at its top with no filter. The filter keeps the rows with a cell
containing the typed text, ignoring ASCII case, and the table says how many rows of
how many it kept. While a filter is being typed, every key is text except
`Ctrl-C`, so a `q` in a machine's name does not end the session.

### How it draws: Sixel, Unicode, ASCII

What the terminal can draw is asked once, when the session starts, and decides
the glyphs every panel draws with for the rest of it.

| Rung | When | Charts |
|---|---|---|
| Sixel | the terminal advertises Sixel graphics **and** reports its cell size in pixels | an image over the chart's rows, with a colour scale in the legend |
| Unicode | the locale names UTF-8 (`LC_ALL`, then `LC_CTYPE`, then `LANG`; on Windows, the console's output code page is 65001) | rows of block elements, `▁` for zero |
| ASCII | anything else, including a locale that says nothing | rows of `_ . : #`, `_` for zero |

A terminal that advertises Sixel but does not report its cell size gets one of the
text rungs. An image cannot be sized in cells without the pixel size, and a guessed
size would draw over the text around the image.

On every rung **zero is not absence**. A reading of zero draws the floor mark. A
sample that was not read, because the server did not answer or an interval could
not be measured across a restart, leaves its cells blank. A figure nobody reported
is drawn as `-`, or as the `--absent` text.

### Piped: one row per sample

When stdout is not a terminal, or `--format` names anything but `human`, nothing is
drawn. Each sample becomes one record in the format asked for: a header row once,
then one row per sample for `human`, `tsv` and `csv`, one JSON document per line
for `json`, and one block per sample for `kv`. The keys are the figures the panel
draws, under the names the panel's table gives them, so a script and the screen
never disagree about what a figure is called.

- **The header is written after the first sample that was read**, and its columns
  never move after that. A figure the server did not report in a later sample is an
  absent cell, not a missing column.
- **A sample that failed is a row of absent cells**, so the time between rows stays
  the interval. A failure before anything was read writes nothing.
- Remarks, such as a sample that failed and why, go to stderr, as for every verb.

`--samples=<n>` ends the run after `n` samples. Without it the run ends at `q` or
`Ctrl-C` on a terminal, or at `Ctrl-C` when piped.

`--interval=<ms>` sets the time between samples. It defaults to 2000 for `cache`
and `node` and 5000 for `fleet`. It must lie between the subject's floor (500 for
`cache` and `node`, 1000 for `fleet`) and 60000; a value outside is refused before
anything is sent, naming the bound, rather than quietly changed. The title's
`every` states the cadence the server **granted**. A server of another build that
grants a different one says so in a remark.

### Where the samples come from

`live-stats` **subscribes over the `0xFC` wire** on `--addr`, the port `node` and
`fleet` already use. The server pushes a sample at every tick until the session
ends. Nothing is polled and no HTTP is spoken:

- **no `--admin-listen` is needed** on the node, and no `/metrics` or `/fleet.txt` is
  fetched;
- a `cache` or `node` sample is a binary snapshot of the same model `/metrics`
  renders as text, a small fraction of the text's size;
- a `fleet` sample is the leader's fleet text, the same body `/fleet.txt` serves.

A `fastcached` serves the `cache` subject. It refuses `node` and `fleet` by name,
since it is not a compile node.

Who may subscribe follows the verbs' own rules:

- **`cache` and `node`** are served to **fleet members**, exactly as `node` and
  `node-metrics` are. Loopback is always a member, and a remote client is refused
  `not-a-member` until the node names it with `--fleet-member`. Membership is asked
  again at every tick, so a member removed by a reload stops receiving samples. On a
  daemon with `--requirepass`, a session that never authenticated loses its stream
  when the password takes effect.
- **`fleet`** needs the dashboard's credential when the node was started with
  `--dashboard-token-file`: pass the same secret with
  `fastcache-cli live-stats fleet --dashboard-token-file=<path>`. It is its own
  flag, never `--token-file`, because the dashboard's credential is a secret of its
  own on the node, and it has no environment variable. A node with no token file
  streams the fleet to **this machine only**, because the fleet names every machine
  and where it answers.
- **Only the leader serves `fleet`.** A follower answers with the leader's address,
  and `live-stats` subscribes there instead. It follows at most three such
  redirections in a row.

What ends a session and what is only a gap:

| What happened | Outcome |
|---|---|
| not a member, or a wrong or missing credential | the session ends, exit **4**; a credential refusal names `--token-file` or `--dashboard-token-file`, and membership is the node's `--fleet-member` |
| the stream's layout, frame or wire version is not this build's | the session ends, exit **5**: retrying cannot succeed, and the remedy is upgrading one end |
| no leader is known yet, the server is at its subscriber limit, the connection is lost, or the server ends the stream | a gap in the samples with its reason in a remark, then a new subscription at the next tick |

A session that never read a sample exits with the outcome of its first failure. One
that read at least one exits **0** however it ends, since a restart the view drew as a
gap is the view working.

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
- **No enrollment verbs, and no `--print-surfaces`.** The fleet tables (`fleet`)
  and the cluster verbs (`cluster-*`) are here; these are not.
  - Opening an enrollment window, and listing, approving or rejecting what waits
    at it, are `fastcache-compile-node --enroll-open`, `--enroll-list`,
    `--enroll-approve`, `--enroll-reject` and `--enroll-close`.
  - Joining a cluster is `--enroll-from`, run on the machine that is joining.
  - The ports a node's configuration would open are `--print-surfaces`.

  `fastcache-cli --help` says so in its NOTES, and `ctest -R cli-node-flags`
  requires every flag named there to be an option of `fastcache-compile-node --help`.
- **The memcached verbs are not routed over the binary protocol.** That protocol
  has SASL and would let them work under `--requirepass`, and doing so is out of
  scope here rather than impossible.
- **No REPL.** One command per invocation, which composes with `ssh` and a pipe;
  `live-stats` is the one command that keeps running, and it still answers one
  subject.
- `CONFIG GET` and `CLIENT LIST` are deliberately not exposed: the daemon answers
  both with stubs, and relaying a stub as fact is worse than not offering it.
