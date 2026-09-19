# The rulebook

Each file here holds the load-bearing constraints of one part of the system.
`AGENT.md` carries the index and a few tripwire one-liners per file; the reasoning
lives here.

## What a rule in this directory is

<!-- agent-tripwire: Every rule there has already been a bug -->

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

<!-- agent-tripwire: none: the file table is this directory's own index; AGENT.md's rulebook links are the same map -->

<!-- table-total: none -->
| File | Governs |
|---|---|
| [`compile-cache.md`](compile-cache.md) | `apps/fastcache-cc/`, `CompileCache/` — the cache key, path canonicalization, manifests, replay |
| [`distributed-compilation.md`](distributed-compilation.md) | `Distributed/`, `apps/fastcache-compile-node/` — dispatch, workers, the scheduler, node tiers |
| [`consensus-and-cluster.md`](consensus-and-cluster.md) | `Consensus/`, `Cluster/` — Raft, discovery, the identity-key handshakes, membership |
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

<!-- agent-tripwire: without a bullet here fires in no session that does not open its file -->

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

## The `AGENT.md` tripwire markers

<!-- agent-tripwire: none: this documents the marker mechanism itself; the rules it guards carry their own -->

Every `##` section in this directory carries one, directly under the heading, and
`ctest -R rulebook-tripwires` refuses a section that does not — mandatory rather
than opt-in, because an opt-in marker is exact about the sections it knows and
silent about the ones it does not, and silence reads identically to complete
coverage.

```
<!-- agent-tripwire: a distinctive phrase from the AGENT.md bullet -->
<!-- agent-tripwire: none: why this section needs no tripwire -->
<!-- agent-tripwire: untriaged: #123 nobody has decided about this one yet -->
```

The check verifies the quoted phrase appears in `AGENT.md`, which makes *yes* a
claim somebody can be **wrong** about rather than a box ticked. `AGENT.md` is only
ever READ, which is the property that makes this affordable in a file several
sessions edit at once. **Three spellings and not two**, because *deliberately
untripwired* must not be spelled like *forgot* — and the third is safe only
because the check TALLIES untriaged markers and prints the total per issue on
every run.

**Why a nominated phrase rather than a structural correspondence.** #876 asked for
a check over a unit this tree does not have, and each of these kills a cheaper
design:

*The correspondence cannot be heading text.* #876 cited `e12a4c9a` as adding *"A
green local gate says NOTHING when the subject under test is the build
environment"* with no tripwire. `AGENT.md` carries one — *"When the SUBJECT under
test is the build environment, a green local gate is not weak evidence — it is
none."* Same fact, no matchable text between them. Any fuzzy ruler refuses correct
entries and misses incorrect ones, which is the ticket’s own objection to a
threaded-source census, turned on the ticket.

*A tripwire may live under ANOTHER file’s link, and may be a nested bullet.*
[`compile-cache.md`](compile-cache.md)’s `## Two servers on one wire are two
VERSIONS on one wire` is tripwired as a `  - ` sub-bullet under
**[`distributed-compilation.md`](distributed-compilation.md)** — correctly,
because the rule spans both and that is where a reader of the fleet section needs
it. So *a bullet under that file’s link* would refuse a correct tree, and any
reader keyed on `^- ` misses it silently. The phrase is therefore searched over
the WHOLE of `AGENT.md`.

*The rule above is CONDITIONAL.* “Adding a rule” says to add a tripwire *if the
rule is one a reader could plausibly break without noticing*, so a check written
to #876’s acceptance clause would refuse entries this file says need no tripwire.
That is what `none:` is for.

*A census states its PATTERN, not only its number.* Sizing this produced one
disagreement worth keeping, pinned to `abb530ed`, 2026-09-11 — pinned because a
measurement’s conditions are the world at one instant and must not track their
source. One reader counted **350** top-level `AGENT.md` bullets and a second
**348** — near agreement — while their per-file figures differed by **twenty**, 57
against 77 for `distributed-compilation.md`. *Totals that nearly agree while their
parts do not* is a **contradiction, not a rounding difference**: two patterns
merely differing move the total by roughly the sum of the per-file differences, so
two errors cancelling means something is being **attributed** to the wrong file
rather than merely counted differently. Both readers wrote it off as “two
reasonable walkers” before either pulled on it. It resolved completely — neither
walker bounded a file’s section at the next `^## `, so the last section ran to
end-of-file and swallowed 36 bullets from `## Issues and pull requests` onward —
and the reusable half is the tell: **a disagreement whose shape the patterns
explain is a difference; one they do not is a defect.**

Do not restate the live counts here. The check prints them on every run, which is
the one place they cannot drift from the tree.

## Do not `@`-import these

<!-- agent-tripwire: Link these as plain markdown, never as an `@`-prefixed path -->

`CLAUDE.md` imports `AGENT.md`, and Claude Code resolves `@` imports recursively.
An `@`-prefixed reference to a file in this directory, anywhere in `AGENT.md`,
would pull every one of them back into every session and undo the entire point of
the split. Link them as plain markdown.
