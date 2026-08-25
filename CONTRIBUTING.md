<!-- SPDX-License-Identifier: Apache-2.0 -->
# Contributing

Building, testing and the design rules live elsewhere: [`README.md`](README.md)
for what this is, [`AGENT.md`](AGENT.md) for the architecture and the presets,
and [`.agent/rules/`](.agent/rules/) for the constraints that have each already
been a bug. This file covers how issues and pull requests are labeled, because
that part is automated and the automation will stop you if you skip it.

## What you owe, and what CI supplies

CI derives everything it can from the change itself and asks for the one thing
it cannot:

| Axis | Who sets it |
|---|---|
| `area/`, `os/` | **CI**, from the changed paths ([`.github/labeler.yml`](.github/labeler.yml)) |
| `type/` | **You** — unless the title is a conventional commit, in which case CI reads it |
| `priority/`, `status/`, `breaking-change`, `security` | You, when they apply |

A pull request with no `type/` label **fails a check**. That is not bookkeeping:
`type/` decides which section of the release notes the change lands in
([`.github/release.yml`](.github/release.yml)), and nothing downstream can
recover it later.

The quickest way to satisfy it is a conventional-commit title — `fix(protocol):
…`, `feat!: …` — which CI reads directly. `feat`, `fix`, `perf`, `docs`, `test`,
`refactor`, `chore`, `ci` and `build` are recognised, and a `!` before the colon
(or a `BREAKING CHANGE:` footer) adds `breaking-change`. A prose title is equally
welcome; it just means adding the label by hand.

Note that a **scope-only** prefix — `net:`, `node:`, `cluster:`, `cmake:` — names
an area rather than a type, so CI deliberately reads nothing from it and still
asks for a `type/`. Guessing a type from a prefix it does not know is how a
refactor gets published under Features.

## The labels

**`type/` — exactly one per issue and pull request.**

| Label | Means |
|---|---|
| `type/bug` | Behaves incorrectly against its stated contract |
| `type/feature` | New capability, or a user-visible extension of one |
| `type/refactor` | Restructuring with no behavior change |
| `type/perf` | Throughput, latency, or footprint |
| `type/docs` | `docs/`, `site/`, help text, the rulebook |
| `type/test` | Coverage gaps, flaky or unbuilt targets, e2e fixtures |
| `type/chore` | Dependencies, CI plumbing, release mechanics, repo hygiene |

**`area/` — one or more.** These mirror the domains in
[`.agent/rules/`](.agent/rules/README.md), so an area label points at the file
governing the code it covers:

`area/launcher` · `area/compile-cache` · `area/distributed` · `area/cluster` ·
`area/protocol` · `area/net` · `area/storage` · `area/platform` ·
`area/observability` · `area/packaging` · `area/build` · `area/core`

**`priority/` — only when it is not normal.** No label means normal, which is
most things. `priority/critical` is reserved for the failure this project keeps
producing: a wrong answer served under a zero exit code, data loss, or a security
hole. `priority/high` blocks a release or a workflow with no workaround.

**`status/` — transient, at most one**, and removed when it stops being true:
`status/needs-triage` (applied automatically to every new issue),
`status/needs-info`, `status/blocked`.

**`os/` — only when the change is specific to one host.** A portability change
that touches Windows, macOS and Linux together is not `os/*` at all; that is why
CI does not derive these from the per-platform reactor and socket files.

**Flags.** `breaking-change` for anything that moves a wire version, a key
schema, a config key or a CLI surface. `security` for authentication, trust
boundaries, or a remotely reachable defect.

## Opening an issue

State what breaks and how you know — the test, the measurement, or the CI
failure. `.agent/rules/README.md` explains why: most defects here are *silent*,
so an issue that describes only the rule and not its consequence is one the next
reader will argue away, usually correctly.

Deferred work belongs in an issue rather than in prose, and gets linked from the
`## Open work` section of the rulebook file it touches.
