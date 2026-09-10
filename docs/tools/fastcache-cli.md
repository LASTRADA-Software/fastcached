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

## Where it connects

| | |
|---|---|
| Data port | `--addr=<host:port>`, `$FASTCACHE_ADDR`, default `127.0.0.1:6674` |
| Admin surface | `--admin-addr=<host:port>`, `$FASTCACHE_ADMIN_ADDR`, unset by default |
| Credential | `--token-file=<path>` (preferred) or `$FASTCACHE_TOKEN` |

The same `$FASTCACHE_ADDR` that `fastcache-cc` reads, so a machine configured for
the launcher is already configured for this. A variable that is *set but empty*
counts as unset, which is what a build that wants no configuration exports.

`--addr` takes `host:port` and refuses a bare port, because a bare port names no
machine. IPv6 literals are bracketed: `--addr=[::1]:6674`.

Prefer `--token-file` to the environment variable. The path is not the secret;
the file is, and an environment variable is visible to anything that can read
this process's environment.

## Commands

Run `fastcache-cli --help` for the current list with operand counts. Today:

| | |
|---|---|
| Read | `get`, `mget`, `exists`, `ttl`, `info`, `stats`, `version`, `ping`, `echo` |
| Write | `set`, `del`, `incr`, `decr`, `incrby`, `decrby`, `expire`, `persist`, `flush` |

A modifier that means nothing for a verb is **refused**, not ignored:
`set k v --raw` is a usage error rather than a store that silently prints
nothing. `--ttl`, `--nx` and `--xx` belong to `set`; `--raw` to `get`; `--all` to
`flush`.

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
| `metrics` | the admin surface's `/metrics` | ~127 series here |
| `info` | RESP `INFO` on the data port | 7 fields |

`/metrics` needs no credential — it is served above the dashboard's
authentication gate — but it does need the daemon started with its metrics
listener, and it is on a different port, so `--admin-addr` must name it.

Without one, `stats` falls back and is explicit about the difference:

```console
$ fastcache-cli stats --format=kv
source=info
fastcached_version=fastcached-0.2.0
...
fastcache-cli: RESP INFO on the data port returned 7 field(s); start the daemon with its metrics listener enabled, or pass --admin-port, for the full counter set
fastcache-cli: the admin surface's /metrics endpoint was not asked: no admin address is known; pass --admin-addr or set $FASTCACHE_ADMIN_ADDR
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

## Environment

| | |
|---|---|
| `FASTCACHE_ADDR` | the cache's data port, as `host:port` |
| `FASTCACHE_ADMIN_ADDR` | the admin surface, as `host:port` |
| `FASTCACHE_TOKEN` | the credential to present |
| `FASTCACHE_USER` | username for the two-argument `AUTH` form |
| `NO_COLOR` | set to anything non-empty to suppress colour |

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
- **No REPL and no live view yet.** One command per invocation, which composes
  with `watch`, `ssh` and a pipe.
- `CONFIG GET` and `CLIENT LIST` are deliberately not exposed: the daemon answers
  both with stubs, and relaying a stub as fact is worse than not offering it.
