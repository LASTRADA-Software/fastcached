# Tools

The project ships two executables and one test client.

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

## `compile-cache-testclient` — the protocol probe

Test infrastructure, not a product: a low-level client for the `0xFC` protocol
used to validate the canonicalization contract end-to-end. It is not built or
installed by default.

Reference: [compile-cache-testclient](compile-cache-testclient.md).

## Which do I want?

| Goal | Use |
|------|-----|
| Speed up C/C++ compiles across machines | `fastcache-cc` + a `fastcached` daemon |
| Back an existing sccache setup | `fastcached` alone, via `SCCACHE_MEMCACHED` / `SCCACHE_REDIS` |
| A memcached- or Redis-compatible cache | `fastcached` alone |
| Verify path canonicalization while hacking on the cache | `compile-cache-testclient` |
