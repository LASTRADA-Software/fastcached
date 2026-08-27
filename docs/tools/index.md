# Tools

The project ships three executables and one test client.

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
not limited to the cores of the machine running it. Workers register with a
`fastcached` acting as scheduler, and clients are handed one on a miss.

It is not a cache and not a scheduler: it holds no keys, stores nothing, and is
given no cache credentials — the object goes back to the client, which stores
it. A job names a *toolchain fingerprint*, never a program, and the worker maps
that to a compiler from its own configuration; that is what keeps a build
accelerator from being a remote shell.

Every refusal — no matching toolchain, no free slot, an unreachable worker —
falls back to a local compile, so distribution cannot fail a build.

Full reference: [fastcache-compile-node](fastcache-compile-node.md).

## `compile-cache-testclient` — the protocol probe

Test infrastructure, not a product: a low-level client for the `0xFC` protocol
used to validate the canonicalization contract end-to-end. It is not built or
installed by default.

Reference: [compile-cache-testclient](compile-cache-testclient.md).

## Which do I want?

| Goal | Use |
|------|-----|
| Speed up C/C++ compiles across machines | `fastcache-cc` + a `fastcached` daemon |
| Compile on other machines too, not just cache | add `fastcache-compile-node` workers |
| Back an existing sccache setup, on GCC or Clang | `fastcached` alone, via `SCCACHE_MEMCACHED` / `SCCACHE_REDIS` |
| A memcached- or Redis-compatible cache | `fastcached` alone |
| Verify path canonicalization while hacking on the cache | `compile-cache-testclient` |

The sccache row is the one with a condition on it, and the condition is the
compiler rather than the goal:

--8<-- "sccache-backend-caveat.md"
