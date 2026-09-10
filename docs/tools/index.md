# Tools

The project ships four executables, and these are all of them: every target
carrying an `install()` rule appears below.

## `fastcached` — the cache daemon

The server. It speaks four wire protocols off a single storage engine and
auto-detects which one a client is using from its first bytes, so one listening
port serves memcached clients, Redis clients, and the compile cache at once.

Start here: [Quickstart](../getting-started/quickstart.md).

## `fastcache-cc` — the compiler launcher

A drop-in replacement for [ccache](https://ccache.dev/) and
[sccache](https://github.com/mozilla/sccache) that uses `fastcached` as its
backing store. It fronts each compile, serves cache hits by reproducing the
object file and replaying the compiler's output, and falls back to a real
compile whenever anything goes wrong.

What it does that the alternatives do not: its entries are **portable across
checkout paths**. Paths under the configured source root and build tree are
rewritten to tokens before the cache key is computed, and rewritten back to the
consuming machine's layout on a hit. A CI runner with the checkout at
`/ci/w/1/s` and a developer with it at `/home/alice/proj` therefore share cache
entries instead of each maintaining their own.

Full reference: [fastcache-cc](fastcache-cc.md).

## `fastcache-compile-node` — the compile worker

Takes translation units that missed the cache and compiles them, so a build is
not limited to the cores of the machine running it. Workers register with **one
of their own** — a node started with `--serve-scheduler`, never `fastcached` —
and clients are handed one on a miss.

It is the fleet's only binary and wears several hats: every node compiles, holds
a cache tier of its own by default, and may additionally schedule, run consensus
and serve the fleet dashboard. What it never does is write to the shared cache —
it is given no credentials for it, and the object goes back to the client, which
stores it. A job names a *toolchain fingerprint*, never a program, and the worker
maps that to a compiler it discovered on its own machine; that is what keeps a
build accelerator from being a remote shell.

Every refusal — no matching toolchain, no free slot, an unreachable worker —
falls back to a local compile, so distribution cannot fail a build.

Full reference: [fastcache-compile-node](fastcache-compile-node.md).

## `fastcache-cli` — the operator's client

A `redis-cli`-shaped client for everything the other three expose. It reads and
writes the keyspace (`get`, `set`, `mget`, `del`, `ttl`, `expire`, `incr`, …) and
it gathers statistics, which is the part an operator on an SSH session has
otherwise had to do with `telnet` or a browser.

Two things make it worth having rather than reaching for `redis-cli`:

**It answers in five registers.** Human-readable aligned columns by default, and
`--format=json|kv|tsv|csv` for anything that parses. The output shape does not
change when stdout is a pipe — only colour does — so a command that works by hand
works in a script.

**An absent value is not a zero.** A field the chosen source could not supply
renders as a dash, or as `null` in JSON, and never as `0`. A counter that has
counted nothing does render `0`, because that is the truth about events that did
not happen. `stats` reports *which* source answered as a field of its own output,
because the difference between a 127-series `/metrics` scrape and a 7-field
`INFO` reply is not something a dashboard should have to guess at.

Its exit codes distinguish six outcomes rather than success and failure: a cache
**miss** exits 1 and an **unreachable** daemon exits 3, so a script can retry one
and give up on the other.

Full reference: [fastcache-cli](fastcache-cli.md).

## Which do I want?

| Goal | Use |
|------|-----|
| Speed up C/C++ compiles across machines | `fastcache-cc` + a `fastcached` daemon |
| Compile on other machines too, not just cache | add `fastcache-compile-node` workers |
| Back an existing sccache setup, on GCC or Clang | `fastcached` alone, via `SCCACHE_MEMCACHED` / `SCCACHE_REDIS` |
| A memcached- or Redis-compatible cache | `fastcached` alone |
| Read, write or measure a running cache from a terminal | `fastcache-cli` |

The sccache row is the one with a condition on it, and the condition is the
compiler rather than the goal:

--8<-- "sccache-backend-caveat.md"
