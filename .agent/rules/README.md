# The rulebook

Each file here holds the load-bearing constraints of one part of the system.
`AGENT.md` carries the index and a few tripwire one-liners per file; the reasoning
lives here.

## What a rule in this directory is

**Every rule here has already been a bug.** That is the entry criterion, and it is
why the prose keeps the failure it prevents rather than stating the rule alone: a
rule with no consequence attached is one the next reader will argue away, usually
correctly, because the simpler design really does look better until you know what
it does.

Most of them are also *silent* failures — the shape this project keeps producing:

- a cache that stops sharing between machines while every unit test passes;
- a service that registers successfully and then cannot do its job;
- a counter that increments correctly and is exported nowhere;
- a fleet that never distributes a single translation unit and goes green anyway;
- an object from an unrelated translation unit, served under a zero exit code.

If a rule's failure mode is loud, it usually does not need to be written down —
the build or the test suite already says it.

## Files

<!-- table-total: none -->
| File | Governs |
|---|---|
| [`compile-cache.md`](compile-cache.md) | `apps/fastcache-cc/`, `CompileCache/` — the cache key, path canonicalization, manifests, replay |
| [`distributed-compilation.md`](distributed-compilation.md) | `Distributed/`, `apps/fastcache-compile-node/` — dispatch, workers, the scheduler, node tiers |
| [`consensus-and-cluster.md`](consensus-and-cluster.md) | `Consensus/`, `Cluster/` — Raft, discovery, the PSK handshake, membership |
| [`wire-and-protocol.md`](wire-and-protocol.md) | `Protocol/`, `Net/` — framing, the auth gate, sockets |
| [`platform-service-and-config.md`](platform-service-and-config.md) | `Platform/`, `Config/` — service registration, config lookup and trust, the CLI table |
| [`metrics-and-observability.md`](metrics-and-observability.md) | `Metrics/` — the counter table, refusal codes, scrape surfaces |
| [`packaging-and-release.md`](packaging-and-release.md) | `packaging/`, `cmake/Packaging.cmake`, `cmake/Version.cmake`, the release job |
| [`build-and-toolchain.md`](build-and-toolchain.md) | What differs between compilers, standard libraries, hosts and tool versions |
| [`testing.md`](testing.md) | How tests are registered and what they may assume |

Two neighbours: [`../guides/profiling-tracy.md`](../guides/profiling-tracy.md) is a
how-to rather than a rulebook, and
[`../reference/source-map.md`](../reference/source-map.md) is the annotated source
tree.

## Adding a rule

Add it to the file that governs the code it constrains, under the existing
headings, and state three things: what the rule is, what breaks when it is
violated, and how you know — the test, the measurement, or the CI failure that
proved it. Then add a one-line tripwire to that file's entry in `AGENT.md` if the
rule is one a reader could plausibly break without noticing.

Deferred work does **not** belong here. Open a GitHub issue and link it from the
file's `## Open work` section — a residual recorded only in prose is one nobody
diffs. An *accepted trade-off* is different and does belong here, under
`## Accepted trade-offs`, so that nobody "fixes" it without reopening the argument.

An entry there is a top-level bullet whose **leading** reference is the issue:

```
- **[#123](https://github.com/LASTRADA-Software/fastcached/issues/123)** — what is left.
```

`ctest -R rulebook-open-work` reads that grammar and `rulebook-open-work-state`
resolves each one, because an entry whose issue has since closed is a rule that has
gone false — and the expensive version of that is an entry saying something *cannot*
be done, which instructs the next session not to try. Further issue links inside a
bullet's prose are citations and are deliberately not resolved: naming the closed
change that produced the residual is correct. The heading is spelled `## Open work`
exactly, and a section with no entries under it is refused rather than tolerated —
delete the heading when the last entry goes, or the file leaves the scanned set
without anything saying so. The reasoning is in
[`build-and-toolchain.md`](build-and-toolchain.md).

## Do not `@`-import these

`CLAUDE.md` imports `AGENT.md`, and Claude Code resolves `@` imports recursively.
An `@`-prefixed reference to a file in this directory, anywhere in `AGENT.md`,
would pull every one of them back into every session and undo the entire point of
the split. Link them as plain markdown.
