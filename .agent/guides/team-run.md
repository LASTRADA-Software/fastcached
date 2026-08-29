# Running the production-readiness work as a team

This describes **how** the `fastcache-compile-node` production run is executed, so a new
session — or a different account — can take it over without reconstructing the setup.

It deliberately contains **no board state**. What is done, in progress and left is in
GitHub: issue #255 is the ordering, and the issues it links are the work. Duplicating that
here would create a second source of truth that goes stale the moment a PR merges, which
is the failure this repository already documents everywhere else.

**To take over:** read this file, open #255, look at what open PRs reference. That is the
whole handover.

## Roles

One **manager** session and two or three **developer** sessions.

The manager owns the board, assignment, `origin/master` synchronisation, all PR merging,
and bug intake. It does not write feature code — when it does, it stops being able to
review, and reviewing its own work is how a wrong finding gets ratified twice.

Developers each work in their **own git worktree** on their own branch, one ticket at a
time.

## Lanes

Tickets are assigned by **component**, not by priority. Priority decides *order*; component
decides *who*, because that is what keeps three branches from colliding.

| Lane | Owns |
|---|---|
| **L — launcher** | `src/apps/fastcache-cc/` |
| **N — node** | `src/apps/fastcache-compile-node/` |
| **D — library, tests, packaging** | `src/FastCache/{Distributed,Consensus,Cluster}/`, `scripts/`, `packaging/`, `.github/` |

A ticket spanning two lanes is **split into two tickets** with an explicit ordering, or held
until the blocking lane lands. The manager sequences those; developers never coordinate
directly, because two developers agreeing on an interface is two developers writing it
twice.

## Which session is working on which ticket

**The branch is the claim.** A ticket is in progress if and only if an open branch or PR
references it (`Refs #NNN` / `Closes #NNN`).

Nothing else records assignment — not a label, not a title prefix, not a name in a file.
Those all bind a ticket to a *session*, and a session is the least durable thing here: it
dies, it compacts, it gets resumed under another account. A stale `dev-2` label on a ticket
whose session ended is worse than no label, because it reads as coverage. The branch cannot
go stale in that way — it either exists or it does not, and GitHub already renders the link
on the issue.

## Coordination protocol

Each of these is a scar, not a preference.

- **Nobody touches `D:/fastcached`.** It is the main checkout and other sessions hold
  uncommitted work in it.
- **Nobody restarts the local `FastCacheCompileNode` service.** Every developer's build
  goes through it. A ticket needing a restart or reinstall goes back to the manager, who
  serialises it.
- **Rebase on the manager's signal.** `origin/master` moves fast here and a build that was
  running when a rebase landed is **poisoned** — mixed object vintages, a result describing
  a tree that never existed. Check the base before starting anything long.
- **Never `git reset --soft origin/master`** to squash after the base moved: the reset
  re-parents onto the new master while the index still holds the old tree, so everything
  that landed in between becomes a deletion in your commit. Use `rebase -i`. Read the
  diffstat before pushing and check `git diff --diff-filter=D`.
- **New build directories need `--fresh`.** Existing trees can have `FASTCACHE_CC-NOTFOUND`
  baked into `CMakeCache`, and `find_program` never revisits a filled entry — they fall
  back to sccache silently. Confirm the `[cache] Enabling fastcache-cc` configure line.
- **Local gate caveat.** A worktree created from Windows is unreadable by WSL git, so
  `scripts/local-gate.sh` dies at step one and `repository-hygiene` silently *skips*.
  Create worktrees from **inside WSL**, or rely on CI as the gate — and say which you did.
- **Merging is the manager's.** `gh pr merge --merge --auto`, then
  `gh pr update-branch --rebase` on every `BEHIND`; auto-merge alone never makes a branch
  up to date.

### The type-label check

A PR with no `type/` label fails a check. Applying several labels at once fires several
`labeled` events, and the concurrency group cancels the earlier runs — so the check can
show red while a queued run is already on its way to fixing it.

`gh pr checks` renders `CANCELLED` and `FAILURE` identically. Tell them apart:

```
gh pr checks <n> --json name,state
```

- Type-label row `CANCELLED` **and** anything `QUEUED` → **wait**. Firing another label
  cancels the survivor.
- Type-label row `FAILURE` with nothing queued → re-apply **one** accurate missing `area/*`
  label to fire a fresh event.
- Never `gh run rerun` — it races the concurrency group and makes it worse.

## Review gates

Every developer, every PR, all scoped explicitly as `origin/master..<branch>`:

- **End of each phase within a PR** — `/simplify`, then `/code-review medium --fix`
- **Before handing the PR to the manager** — `/simplify`, then `/code-review high --fix`

`/simplify` runs **first** by design: it is quality-only and does not hunt for bugs, so
shrinking the change before the correctness pass means the review examines less code and
its findings land on code that will actually ship, not on lines about to be deleted.

The explicit scope is not optional. Skills fork into the primary working directory, so a
bare invocation operates on the wrong tree. A review that silently examined the wrong branch
and came back clean is worse than no review, and a `/simplify` that *edited* the wrong tree
is worse still.

## Documentation is part of every PR

No PR merges leaving the docs describing behaviour it just changed.

- **`docs/`** — operator-facing. Anything changing a flag, a port, a default, who may reach
  a surface, or a failure mode updates it in the same PR.
- **`AGENT.md`** — a tripwire bullet when the PR establishes a constraint a future session
  must not re-break.
- **`.agent/rules/*.md`** — when the PR fixes something that *was a bug*, which is what every
  rule in there is. Deferred work goes in that file's `## Open work` as a linked issue,
  never as prose.

The manager rejects a PR at merge review if it changes observable behaviour and touches no
documentation, or if it claims something the code does not do.

## Bug intake

A developer finding a defect outside its ticket either **fixes it inline** (trivial and
in-lane) or **reports it to the manager** with file:line and a failure scenario. The manager
files the issue and schedules it. Nothing is silently deferred and nothing is silently
widened.

## Verification expectations

Reproduce the finding **first**. Several tickets came out of a review pass and are
unverified; a developer who cannot reproduce one reports back rather than "fixing" it.

Then: Catch2 tests next to the implementation, `clang-format` and `clang-tidy` at the
version CI pins in a build directory of their own, and CI green. Prove a regression test
fails without the fix, not merely that it passes with it.
