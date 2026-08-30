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

### The claim is not the work

A branch says what a session has **pushed**. It says nothing about what is sitting
uncommitted in that session's worktree, and that is where work is actually lost.

A branch whose tip is already an ancestor of master, with zero unique commits, looks
from the outside like a session that did nothing. It can just as easily be a session
holding two hundred uncommitted lines. `git log` cannot tell those apart:

```
git -C path/to/worktree status --porcelain
```

That is the check. It found #275 as eight modified files in a worktree named for #175,
in a session that had been silent for two hours, on a branch that looked empty —
after a check of the *commits* had already concluded nothing was at risk.

So: a session that goes quiet gets its **working tree** looked at, not its branch. And
push early, including work that is half finished. An incomplete pushed branch is
recoverable; an unpushed one disappears with the worktree.

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
- **`origin/master` moves under you without you fetching.** Every worktree here shares one
  `.git`, so any other session's `git fetch` advances the ref for all of them. Anything you
  compare against `origin/master` is compared against a ref another session may have moved
  seconds ago, and nothing in your own shell will have hinted at it.
- **Never `git reset --soft origin/master`** to squash after the base moved: the reset
  re-parents onto the new master while the index still holds the old tree, so everything
  that landed in between becomes a deletion in your commit. Use `rebase -i`. Before pushing,
  read the diffstat and check `git diff --diff-filter=D`.

  **Three dots, always:**

  ```
  git diff --stat origin/master...HEAD
  ```

  Two-dot compares master's *tip* to your HEAD, so the moment master moves, its new
  commits render as **your deletions** — the exact signature of the disaster this check
  exists to catch. It false-alarms on every upstream merge, and a check that cries wolf
  routinely is one nobody believes on the day it is right. Three-dot compares from the
  merge base and shows only your work. It still catches the real accident: after a
  `reset --soft`, the merge base *is* the new master, so the deletions show up either way.
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

The decisive signal is **why** it is red, not that it is red — and one of the three
causes below is not a cancellation at all:

- Red **immediately after a multi-label create or edit** — self-inflicted by the
  concurrency group, and a queued run is already on its way to clearing it. **Wait.**
  Firing another label event cancels the survivor and starts the cycle over.
- Red on a PR whose **labels have been stable for minutes**, nothing queued — genuinely
  stuck. Re-apply **one** accurate missing `area/*` label to fire a fresh event.
- Red while the PR **visibly has its `type/` label** — `actions/labeler` destroyed a
  correctly-applied one. It finishes with an unconditional `setLabels()` seeded from
  the labels it read when its run started, so anything applied between that read and
  that write is lost, whatever `sync-labels` says and whatever the config names
  (#347). This is the one you will meet most often, and it does not look like either
  case above: the label may be gone, or applied-destroyed-restored so that
  `gh pr view <n> --json labels` shows it present while the gate is still red from
  the moment it was not. `--json name,state` says `FAILURE`, not `CANCELLED`.
  Re-apply the label. Since #350 the workflow restores what the replacement
  destroyed and emits a `::warning::` naming it — if that warning is in the run log,
  this is your cause and there is nothing further to diagnose.
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

### Sweeping clang-tidy

A skipped file and a clean file look identical in the output, so the sweep has to be
built to distinguish them:

- Derive the file list from `git diff --name-only origin/master...HEAD`, never from a
  list you maintain by hand.
- **Fail loudly on any file absent from `compile_commands.json`** rather than skipping it.
- Canary that clang-tidy actually executed. A sweep that could not run reads exactly like
  a clean one — the same trap `scripts/tidy-sweep.sh` guards in CI.

Beware any rule whose last clause tells you which files you may skip. One session swept
three of five changed files, having recorded that "production sources are where the
findings actually land"; the finding was in a test file, and it reported clean. The
comfortable generalisation is the part that fails.

Satisfying the linter is not the same as taking its suggestion. `return { 8 * 1024, 'A' }`
compiles only because `8192` narrows and makes `std::string`'s `initializer_list<char>`
candidate non-viable — change the value to one that fits in a `char` and it silently
becomes a two-character string. `NOLINT` is banned here because the fix is meant to be a
thought.
