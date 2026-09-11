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

## Open work

- **[#876](https://github.com/LASTRADA-Software/fastcached/issues/876)** — nothing
  checks that a rule here has a tripwire in `AGENT.md`, and a rule with no tripwire
  is, for most sessions, a rule that does not exist. **The ticket asks for a check
  over a unit this tree does not have, and that is the finding rather than a
  caveat** — a check built against it would assert the wrong correspondence, and
  assert it confidently, in the file every session reads first.

  Measured 2026-09-11 on `abb530ed`, each figure with the pattern that produced it,
  because a census states its pattern and not only its number. `^## ` over
  `.agent/rules/*.md` excluding `README.md`: **112** headings. `^- ` over `AGENT.md`
  from one `**[`.agent/rules/X.md`]**` link to the next link **or the next `^## `
  heading, whichever comes first**: **314** top-level bullets, **322** counting the
  eight nested `  - ` ones.

  **That second bound is load-bearing and this figure was wrong without it.** The
  first reading said 350, because the walker had no heading bound and the LAST
  section therefore ran to end-of-file, sweeping in 36 bullets from
  `## Issues and pull requests` onward — `testing.md`'s row read 71 where it is 35.
  Only the last section is exposed, since no `^## ` falls between two links, which
  is why every other row survived unchanged and the error hid in the total.

  **The tell was there and was nearly missed, and it is the reusable part.** A
  second reader over the same file reported **348** top-level against this one's
  350 — near-agreement — while the per-file figures differed by **twenty**, 57
  against 77 for `distributed-compilation.md`. *Totals that nearly agree while
  their parts do not* is a **contradiction, not a rounding difference**: two
  patterns merely differing move the total by roughly the sum of the per-file
  differences, so two errors cancelling means something is being **attributed** to
  the wrong file rather than merely counted differently. Both readers wrote it off
  as "two reasonable walkers" before either pulled on it.

  It resolved completely, and the resolution is better than the tell. The other
  walker also lacked the `^## ` bound, which is where its 77 came from; and its 348
  was **every top-level bullet in `AGENT.md`** — 312 inside rules-file sections
  plus **the same 36 outside them** that this walker's unbounded last section had
  swallowed. Two different defects converging on one block of 36 bullets from
  opposite ends of the file, one of them a wrong SUBJECT rather than a wrong
  pattern.

  What remains is **314 here against 312 there — two bullets, one each in two
  files — and that is left unreconciled deliberately**, because its shape IS what
  two patterns differing looks like. That is the test worth carrying away: a
  disagreement whose shape the patterns explain is a difference; one they do not is
  a defect. None of it touches the conclusion, which rests on the shape rather than
  on any of these numbers.

  <!-- table-total: none -->

  | file | `##` headings | top-level tripwire bullets |
  |---|--:|--:|
  | `distributed-compilation.md` | 1 (`Open work`) | 57 |
  | `build-and-toolchain.md` | 29 | 72 |
  | `testing.md` | 33 | 35 |
  | `packaging-and-release.md` | 4 | 6 |

  A file with ONE heading carries dozens of tripwires. Most rules here are bullets
  and prose; a `##` entry is a subset, so there is no 1:1 structure to derive a
  correspondence from. The three rows are a sample chosen to show the SHAPE, not a
  census — the totals above the table are the census.

  **Four things a reader reaching for this should know before designing anything.**

  *The cited instance is repaired.* #876 names `e12a4c9a` as adding *"A green local
  gate says NOTHING when the subject under test is the build environment"* with no
  tripwire. `AGENT.md` now carries one — *"When the SUBJECT under test is the build
  environment, a green local gate is not weak evidence — it is none."* The ticket's
  substance stands; its example does not, so do not go hunting a live gap.

  *The correspondence cannot be heading text.* That very pair is the worked
  example: same fact, no matchable text. Any fuzzy rule refuses correct entries and
  misses incorrect ones — the ticket's own objection to the threaded-source census,
  turned on the ticket.

  *A tripwire may live under ANOTHER file's link, and may be a nested bullet.*
  `compile-cache.md`'s `## Two servers on one wire are two VERSIONS on one wire` is
  tripwired at `AGENT.md:580`, a `  - ` sub-bullet under **`distributed-compilation.md`**
  — correctly, because the rule spans both and that is where a reader of the fleet
  section needs it. So "a bullet under that file's link" would refuse a correct
  tree, and any reader keyed on `^- ` misses it silently.

  *The rule this file states is already CONDITIONAL.* "Adding a rule" above says to
  add a tripwire *if the rule is one a reader could plausibly break without
  noticing*. #876 asks to enforce something stronger than the rulebook asks for, so
  a check written to its acceptance clause would refuse entries this file says need
  no tripwire.

  **The design that survives all four**, recorded so it is not re-derived: a marker
  in the RULES file under each `##` heading, mandatory, either naming a distinctive
  phrase from its `AGENT.md` bullet or spelling `none` with a reason — the
  `table-total: none` idiom, so opt-in silence is impossible. The check verifies the
  quoted phrase appears in `AGENT.md`, which makes *yes* a claim somebody can be
  wrong about rather than a box ticked. `AGENT.md` is only READ, which is the
  property that makes it affordable in a file several sessions edit at once. What
  it costs is ~112 markers and a human decision per entry, against a unit the
  tree does not use — which is why it is recorded here rather than built.

  **The sizing ruler is not trustworthy and is not a check.** Word overlap between
  a heading and a file's whole `AGENT.md` section says a tripwire is PLAUSIBLE,
  never that one exists. Its first version reported **0 of 104** matched, including
  the one instance already confirmed by hand — its section walker cleared its
  current file on any line starting at column zero, and these sections carry
  blockquotes and paragraphs that do. Fixed, it reports 66 plausible and 18
  needing a human decision; by hand most of the 18 are covered by a bullet sharing
  no vocabulary. **18 is the number that decides the cost and it is the one least
  worth betting on.**
