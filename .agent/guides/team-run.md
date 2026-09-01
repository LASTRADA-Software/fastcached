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
- **The expensive event is the PR being open, not the push.** This is a property of
  `.github/workflows/build.yml`'s triggers, so check it there rather than trusting this
  sentence — and re-check it if they ever move:

  ```yaml
  on:
    push:
      branches: [master, "fix-ci"]
      tags: ["v[0-9]+.[0-9]+.[0-9]+"]
    pull_request:        # no branches: filter — any PR, any base
    merge_group:
      branches: [master]
  ```

  So a push to an ordinary lane branch with **no PR open** creates no check run at all.
  The same push to a branch that **has** one fires the whole job matrix, because
  `pull_request` triggers on `synchronize`. Measured on one afternoon: two pushes to
  branches with #477 and #478 open cost a matrix each, while a push to a branch with no
  PR cost nothing. Same action, opposite cost, and nothing in the action itself tells
  you which you just did.

  **The rule is not "pushing is free".** Recording only the cheap half is what gets a
  lane into trouble in the expensive direction. Three ways an ordinary-looking push is
  expensive: a PR is already open on that branch; the branch is `master` or `fix-ci`,
  the second of which a lane can hit by naming its branch after the thing it is fixing;
  or the ref is a version tag.

  **What this buys, and it is the reason to know it:** during a saturated queue you can
  still finish the work, commit it, and push it. Only *opening the PR* has to wait for
  the manager's call. A lane that reads "hold" as "stop working" idles for no reason;
  a lane that reads it as "pushing is fine" while its PR is open starves the entry the
  manager is trying to land.

  Written down because a manager gave a lane the over-broad version of this rule from
  not having it, and the lane checked the triggers instead of complying.
- **Merging is the manager's.** `gh pr merge --merge --auto`, then
  `gh pr update-branch --rebase` on every `BEHIND`; auto-merge alone never makes a branch
  up to date.
- **The scratchpad is shared, and nothing about its path says so.** Every lane in a run
  writes to one directory. Two sessions wrote a PR body to `scratchpad/pr-body.md`, the
  second replaced the first, and #313 carried #314's description until the session that
  *lost* the file noticed — nothing in the process did. A PR body is at least published;
  a helper script replaced between staging it and running it produces a number attached
  to work that is not yours and reports success either way. **Prefix every scratchpad file
  and directory with the lane that owns it** (`mgr-`, `devc-`, `devd-`, `devl-`, `devn-`),
  and re-stage under a prefixed name before you report a result a generically-named script
  produced. Checked while writing this: one lane's files are already prefixed
  (`devc-322-leasetoken.py`) and the same directory holds `a.patch`, `b2.patch`,
  `claims.sh`, `dbcheck.sh`, `attribute.sh` and `build1.log` from others — so the
  collision is a matter of when, not whether.

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

### Pairwise clean is not serially clean

**Before sequencing two PRs that touch the same file, merge-tree the second onto a
synthetic `master + first` — not onto `master`.** Two branches that each *add* something
at the same anchor are pairwise clean and serially conflicting, always, and every check
anybody runs by reflex answers the pairwise question.

#382 and #391 were both mine, both green, both queued in that order. Each merged cleanly
onto `master`:

```
git merge-tree --write-tree origin/master claude/launcher-replay-320       # rc=0
git merge-tree --write-tree origin/master claude/daemon-target-guard-390   # rc=0
```

Both results were true and neither answered the question that mattered. Build the
sequence instead — commit the first merge's tree, then merge the second onto *that*:

```
tree="$(git merge-tree --write-tree origin/master "$first")"
sim="$(git commit-tree "$tree" -p origin/master -p "$first" -m 'simulated merge')"
git merge-tree --write-tree "$sim" "$second"; rc=$?
```

```
CONFLICT (content): Merge conflict in .agent/rules/testing.md
CONFLICT (content): Merge conflict in AGENT.md
```

The two hunks: both branches add a bullet immediately before
`- A script-driven test naming more than one executable is registered in` in AGENT.md's
testing list, and both add a `##` section immediately before `## Open work` in
`.agent/rules/testing.md`. Neither branch changes a line the other changes — there is no
overlap to see in a diff — and that is exactly why it conflicts: git has two insertions
for one position and no basis to order them.

This is worth a habit because of *where* it goes wrong. `mergeStateStatus` answers the
pairwise question, `gh pr checks` answers the pairwise question, and a green PR page
answers the pairwise question. The failure surfaces only when the first PR merges, at
which point the second is blocked and has to be rebased and resolved by hand — after its
CI has already passed and while it is sitting in a queue somebody is throttling.

#### Judge it by the exit status. The output cannot tell you.

`git merge-tree --write-tree` writes a tree **on conflict as well**, so its success output
and its failure output are the same shape — a hash on the first line:

```
clean:     acaf0e17e2bf4e09ab929e5b54b558629b9af512
conflict:  490e9b1f18761a302133c9e7e66d202cee0475c1
           100644 <blob> 1  AGENT.md          <- stage entries follow, and only then
           100644 <blob> 2  AGENT.md
```

So `[ -z "$tree" ]` reports **clean for every conflict**, confidently and by default. That
is not a hypothetical: the first pass over the full pending order was written that way and
printed `#391 ... clean` — the exact opposite of a conflict that had already been
measured. An instrument with no way to express the failing state reports the passing one.

`rc` is better and is **still not enough**, which is the part to read twice. Measured:

```
clean                          rc=0    tree on stdout
conflicted                     rc=1    tree on stdout, then stage entries
a ref that cannot be resolved  rc=1    NOTHING on stdout; "not something we can merge"
a usage error                  rc=129  usage text
```

**Git returns 1 for a conflict and 1 for a branch name it cannot resolve.** So
`[ $rc -ne 0 ]` — the obvious fix, and the one written after the `-z` version was
caught — calls a stale or mistyped branch a conflict, and a sequencing loop over a
deleted ref reports every pair as conflicting and sends somebody rebasing against
nothing. Same collapse as the first bug, one state over.

Three states, and the discriminator is `rc` **plus** whether stdout carries a tree:

```
out="$(git merge-tree --write-tree "$a" "$b" 2>/dev/null)"; rc=$?
first="$(echo "$out" | sed 1q)"

if [ "$rc" = 0 ]; then
    verdict=clean
elif [ "$rc" = 1 ] && echo "$first" | grep -Eq '^[0-9a-f]{40,64}$'; then
    verdict=conflicted          # a tree was written, so the merge actually ran
else
    verdict="errored rc=$rc"    # no tree: a ref it could not resolve, or worse
fi
```

The shape worth remembering rather than the snippet: **a conflict writes a tree and an
error does not**, so the presence of the tree is what separates the two states that share
an exit code.

#### A third branch is only as tested as the tree beneath it

`merge-tree` carries a conflicted tree forward rather than stopping, so a third PR
simulated on top of an unresolved second is being merged against a tree containing
**conflict markers** — a different question again, and its `clean` is provisional. When
#392 was added to the order it came back clean, but on top of #391's *unresolved* tree, so
that answer stood for nothing until the simulation was re-run against the real resolution.
Label such a result unverified and re-run it once the conflict below it is actually fixed.

#### Verify the tip you pushed, not the tree you were in when you thought to

The resolution above was checked by diffing the resolved files against master and
reporting `0 deletions` in both — presented as *the check that matters*, because an
add/add resolution that deletes nothing has provably kept both sides. The pushed tip
was:

```
AGENT.md                  +8   -0
.agent/rules/testing.md   +40  -2
```

Nothing was wrong with the branch. The two deletions were a deliberate one-line
cross-reference fix made *after* the check ran and disclosed in the same message — so
the prose and the number contradicted each other, and the number was the one carrying
the weight. A reviewer who reads `-0` in a summary and `-2` in the diff stops believing
the rest of a careful write-up, and is right to.

Measure `origin/master...origin/<branch>` — the pushed ref, three dots — as the last
thing before reporting. Anything measured earlier describes a tree that is no longer
what CI, the queue, or a reviewer will see.

#### Two ways the resolution itself can go quietly wrong

**`sed -E '/^=======$/d'` is not a safe way to strip conflict markers.** A bare
`=======` is a valid setext heading underline in markdown, so on a file that has one
the strip silently promotes a heading to a paragraph. Both sides here were checked for
one first — zero in each — but the technique needs that check every time, and reaching
for it on a file you have not checked is how a documentation heading disappears without
a conflict, a warning, or a diff anyone reads.

**A verdict assigned inside `$(...)` is discarded.** The serial simulation returned its
carried-forward tree through a global set inside a function called in a command
substitution, so the subshell's assignment never reached the caller and each step would
have merged onto the *previous* iteration's tree — a plausible-looking sequence for an
order nobody was testing. `set -u` turned it into an unbound-variable error instead of a
wrong answer, which is the entire reason it is worth having on. Return such values
through globals set by a plain call, or print them and parse.

Two more things follow.

**Do not fix it by moving the anchors.** Relocating a rule so two branches stop colliding
organises the file by merge history instead of by subject, and the next reader inherits
both the odd placement and no explanation for it. Take the rebase.

**Do not cancel a running job to pre-empt it either.** The conflict is *scheduled*: it
cannot bite before the first PR merges, and when it does it announces itself as a blocked
queue entry rather than as a surprise. A conflict with a known arrival time and a
mechanical resolution loses to almost anything you would have to cancel to avoid it.

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
