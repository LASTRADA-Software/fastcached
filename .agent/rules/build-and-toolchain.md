# Build system, toolchain and language pitfalls

Rules about the things that differ between compilers, standard libraries, hosts
and tool versions — and about the local gate that exists to catch them before CI
does.

Read this before changing a header everything includes, a randomness or timing
seam, `cmake/portable/CompileCache.cmake`, or anything a test harness's
determinism rests on.

## The local gate

`scripts/local-gate.sh` is the gate. Run it before pushing.

- **A hygiene check traverses each directory ONCE, not once per file pattern.**
  `file(GLOB_RECURSE var a b c)` traverses once PER PATTERN and the call site reads as
  one glob, so `check-sccache-backend-caveat` handed it ~19 patterns per root across
  two calls and cost **78.7 s against a 60 s budget** -- the documented local gate could
  not go green on a DrvFs checkout, a supported working environment (#502). Walk once
  and `list(FILTER ... INCLUDE REGEX)` the result; **never raise the timeout**, because
  a budget raised to accommodate a redundancy hides the redundancy.
- **That last clause is scoped to the redundancy, and the redundancy is gone.** Read
  unqualified it forbids the fix to #479, so it has to say what it was about. After the
  walk-once change there is one traversal per root and what remains is *irreducible* I/O:
  837 files read individually, ~12 MB, of which `src/` is 633 because a non-prose file
  naming a backend variable has to be forced into the exemption table rather than passing
  silently. There is no fat left for a larger budget to hide, so
  `sccache-backend-caveat`'s bound was raised to 180 s **and** — the part that matters —
  the check now MEASURES itself and prints the cost, the band and the headroom on every
  run. A bound may grow only beside an instrument that reports the margin it is spending;
  a number alone drifts back into the same state and nobody sees it happen.
- **Three figures for one check, all honestly taken, none carrying its conditions — that
  was the actual defect in #479.** The registration comment asserted `~0.2-0.5 s ... still
  far inside the timeout`; a quiet WSL 9p checkout measures ~4.5 s; two lanes measured
  53-57 s against the 60 s ceiling while several gates ran at once. Two orders of
  magnitude, and each read as a fact about the check when all three are facts about a
  **filesystem**. The oldest and most confident of them had nothing that could ever
  falsify it. Replacing an assertion with an observation is the fix; the timeout number is
  incidental to it. The bands and their conditions live in
  `FastCachedSccacheScanCostBands` — point at it, never copy a number out of it — and each
  row says whether its per-file figure was **measured or derived**, because two of the
  three are somebody else's wall time divided by a corpus count taken separately.
- **A bound on a check that cannot wedge is a backstop, not a deadline**, and conflating
  the two is what produced a 60 s ceiling this check spends 7% of on a quiet machine and
  95% of under ordinary lane load. `sccache-backend-caveat` opens no socket, spawns no
  subprocess, waits on nothing and has no unbounded loop — it reads N files and exits — so
  there is no hang for a wall clock to catch here, and the only thing it can catch is a
  filesystem slower than the widest measured band, which is a condition rather than a
  defect. Do not tighten it as though tightening a real deadline. It is defensible **only
  while a run going long says so**: the check narrates its measured per-file cost past
  `FastCachedSccacheScanNarrateAfterSeconds` — per scan root while walking, per hundred
  files while reading — and ctest captures the output of a test it kills, so a kill
  arrives with its reason attached. The one window it cannot narrate inside is a single
  `file(GLOB_RECURSE)`, which is uninterruptible and cannot be subdivided without
  reintroducing the per-pattern traversals #502 removed. The threshold and the bound
  cannot drift apart quietly either: the script **refuses** a budget below twice the
  threshold, because a run killed before narration could start is a bare timeout again —
  the signal that cannot say which kind of failure it was. `ctest -R sccache-backend-caveat-selftest` drives it, including the two negative
  directions that rot silently: narration ABSENT at defaults, and `SOURCE_DATE_EPOCH`
  reported as a frozen clock rather than as `0 ms/file`, which is an instrument measuring
  nothing while reporting the best possible filesystem.
- **Read that figure with its conditions, or it misleads in both directions.** The cost
  is per `stat` on a filesystem where each crosses a translation layer, so it is
  invisible on CI and on ext4 and it varies with page-cache warmth:

  <!-- table-total: none -->
  | check | filesystem | standalone | single gate | two lanes | budget |
  |---|---|---|---|---|---|
  | `sccache-backend-caveat`, before #502 | 9p | 78.7 s | 60.0 s (timeout) | -- | 60 s |
  | `sccache-backend-caveat`, after #502 | 9p | 4.0-5.1 s | -- | **53-57 s** (#479, three or four lanes) | 180 s |
  | `byte-order-qualifier` | 9p | 19.6 s | 28.6 s | **60.9 s** | 60 s |
  | `byte-order-qualifier` (#665) | **local ext4** | **0.217 s** | -- | **0.837 s** worst under load | 60 s |
  | `worker-refusals-counted` | 9p | 5.5 s | 9.7 s | -- | 60 s |
  | `net-boundary` | 9p | 4.0 s | 8.6 s | -- | 60 s |

  The two `sccache-backend-caveat` rows are the same check on the same filesystem with
  the redundant traversals removed in between, and they are the argument for reading the
  column headings: **standalone fell 16x and the contended figure did not follow it**,
  because what the walk-once change removed was traversals and what remains is 837
  individual file reads over a bridge. Do not read either row as the cost of that check;
  read the one whose conditions you are in, which is what the header row is for. The
  53-57 s column is the one figure here neither lane could afterwards reproduce, the
  machine having gone quiet — so it is DERIVED conditions, recorded as such.

  **The ext4 row is the falsification the model wanted, and it is why a filesystem
  column now exists.** Same check, same 645 files, same day: **0.217 s** quiet (8 runs,
  worst 0.223 s) and **0.655 s** average under load with a worst of **0.837 s** (8 runs,
  load average bracketed at 6.8 rising to 9.7). Both MEASURED, not derived. Against the
  9p row's 60.9 s that is a factor of roughly 280 for the standalone figure — and the
  check, the corpus and the code are identical, so **every one of these numbers is a
  reading of a filesystem and none is a property of a check.** A reader on ext4 who
  measures 0.2 s and concludes the budget is generous has made the same mistake as one
  on 9p who measures 28.6 s single-gate and concludes it is comfortable; both read a row
  that was not theirs.

  That is also why `byte-order-qualifier`'s registration comment said *"finishes in well
  under a second"* and went unchallenged for so long
  ([#665](https://github.com/LASTRADA-Software/fastcached/issues/665)): on the author's
  filesystem it was **true**, and it is true on CI. An assertion that is correct
  everywhere its author can run it, and two orders of magnitude wrong where the gate
  actually fails, has nothing that can falsify it from the inside — which is the same
  observation #479 made about the number it replaced.

  One traversal of `src/` is **2.09 s** (stable to 16 ms over five runs), and
  38 x 2.09 = 79 s predicted 78.7 s measured -- the model is falsifiable, which is what
  makes it worth trusting. **`byte-order-qualifier` has only ever exceeded its budget
  while a SECOND lane's gate was running**, and this repository routinely has two and
  three lanes gating at once. A reader who measures it single-gate gets 28.6 s and
  concludes it is comfortable; the condition is the finding, not the number.
- **The build tree was NOT the cause, and assuming it was would have shipped a fix that
  changed nothing.** Deleting `out/` entirely takes `sccache-backend-caveat` from 100 s
  to 79 s -- measured A/B, ratios 1.0-1.3x across all ten globbing checks -- so "no
  check walks a build tree" would have been implemented, measured as done, and the gate
  would still have been red. The ticket's own mechanism section said so before the
  wrong scope was written over it: *`out/` is not a scan root; it is the source tree,
  walked 126 times.* Globbing a build tree is a real and separate mistake, and one this
  project has also made.
- `ctest -R glob-traversals` enforces one pattern per call and refuses an unquoted
  `${globs}` expansion -- ONE argument and as many traversals as the list is long,
  which is what the original defect was and what an argument count alone would pass. It
  is a **proxy** for traversals-per-root and its header says so: nineteen
  single-pattern calls in a loop cost the same and pass it, so tidying an argument list
  into a loop makes the guard greener while reintroducing the cost.
  `glob-traversals-selftest` drives seven trees, two of which exist because the check
  got them wrong while it was being written -- it reported its own remediation example
  printed from a `message()`, and a stray `]` in a comment merged lines under CMake's
  list grouping so it named line 217 for a call at line 307.
- **Read a hygiene check by the COUNT of what it says it examined, never by whether it
  passed.** `check-script-check-signals` was green while reading 3 of 21 registrations
  — 14% of its subject — and no verdict could have shown it, because pass/fail was
  green on both sides of the blindness. It was found by running every check against
  master's tree and the branch's tree and diffing the counts each one reports; exactly
  one differed. Two of the five bracket-vulnerable splitters could ONLY be caught this
  way. A vacuous-pass refusal is orthogonal and not a substitute: it catches TOTAL
  blinding and is structurally incapable of catching partial, since 3 is not 0. And
  loud-versus-silent is a property of WHERE the bracket lands, not of the check — the
  same check refused outright under one bracket position and passed at 14% under
  another — so no check may be assumed safe because it once failed loudly. And only an
  UNBALANCED bracket groups: `[[nodiscard]]` is completely harmless. The tree said
  `[` and `]` were "reserved", which is broader than the truth, and acting on the
  broad rule is how the fix for this took `check-worker-refusals-counted` from three
  refusal spellings to ZERO — neutralising brackets destroys text a check matches on.
  Where brackets must survive, do not neutralise them: read the lines without ever
  building a CMake list, which is immune by construction rather than by convention. And know
  what the method CANNOT see. A count detects a check examining a DIFFERENT NUMBER of
  subjects; it is blind to one examining the same number of DIFFERENT subjects. So it
  validates a change that alters HOW MUCH a check reads, and never one that alters WHAT
  it matches — a dropped escape or a normalised tab leaves the line count identical and
  the matched text different. The consolidation in
  [#495](https://github.com/LASTRADA-Software/fastcached/issues/495) was nearly
  validated this way, by the very method this bullet recommends. A tool that cannot
  fail in the way a change fails is not validation, it is decoration.
- **A bracket-vulnerable reader is judged per (reader, file, surviving lines) — never
  per script — and the verdict flips on the corpus alone.** Measured across the six
  remaining `file(STRINGS)` readers, by injecting one unbalanced `]` into a comment on a
  line the reader KEEPS:

  <!-- table-total: readers=rows -->
  | reader | verdict |
  |---|---|
  | `check-tsan-scope:109` | clean pass -> hard refusal, **LOUD** |
  | `check-node-config-reference:75` | clean pass -> hard refusal, **LOUD** |
  | `check-net-boundary:169` | passed over a REAL violation, **SILENT** |
  | `check-psk-signing-seam:216` | 15 calls counted -> 14, still passed, **SILENT** |
  | `check-vslang-probe-only:122,198` | unchanged — **by coincidence, not defence** |
  | `check-script-check-signals:256` | unchanged — `LIMIT_COUNT 1`, immune by construction |

  The last two rows are the point. A merge can only reduce a count where ONE FILE holds
  two or more matching lines with the bracket on a non-final one; `check-psk-signing-seam`
  and `check-vslang-probe-only` are the same reader shape and land on opposite sides
  purely because one corpus has a two-match file and the other does not. Injecting into a
  one-match file changes nothing, which is why the first attempt on the psk check came
  back clean and would have closed it as safe. **A reader left alone because today's files
  happen to be safe is a defect scheduled for later**, and `LIMIT_COUNT 1` is the only
  row in that table that is safe for a reason rather than by luck.
- **The remedy is not one remedy, and applying the usual one to `check-tsan-scope` breaks
  it.** Blanking `[` and `]` before splitting is right where the brackets are punctuation;
  it is wrong where they are the DATA. A Catch2 tag is spelled `[async]`, so the neutralising
  split made that check report the table as naming no tags at all and refuse on a perfectly
  good tree — measured, because that was the first attempt at the fix. The standing rule
  above already says *where brackets must survive, read the lines without ever building a
  CMake list*; that reader is now a `FIND`/`SUBSTRING` offset walk, immune by construction,
  and the rule is carried by the reader it governs rather than by a file three directories
  away.
- **`if(VAR STREQUAL "")` does not fire when VAR is UNSET, and the one place that matters
  is a glob that came back empty.** `check-net-boundary`'s empty-directory guard — whose
  own comment calls it *"the one failure mode a boundary test must not be allowed to
  have"* — could never fire. `file(GLOB_RECURSE)` over an empty directory yields an empty
  list, `set(x ${empty})` **unsets** x, and CMake then reads the left operand as the
  literal string `unitSources`, which is not `""`. Measured:

  ```
  set(empty_list)
  if(empty_list STREQUAL "")       did NOT fire
  if("${empty_list}" STREQUAL "")  FIRED
  if(NOT empty_list)               FIRED
  ```

  The sibling accumulators in the same file are safe only because a `set(x "")` above
  DEFINES them; the broken one is assigned inside a loop from a glob. So the hazard is not
  the idiom, it is the idiom applied to a variable whose assignment can vanish — and the
  check reported *"0 source(s) across 2 directory/directories reach only themselves"* and
  exited 0 over a tree with no sources in it at all. **Quote the variable.**
- **A clean-tree injection understates a check that only reports violations.** #518
  classified `check-net-boundary` as PARTIAL — summary byte-identical, full output changes
  — from exactly that experiment, and the classification is too kind: everything a merged
  element swallows is something the check had nothing to say about, so on a clean tree
  nothing changes at all. Three arms with a violation PLANTED:

  ```
  violation alone                     exit 1, violation NAMED, 33 lines
  violation + a stray `]` before it   exit 0, violation NOT named, 1 line
  stray `]` alone                     exit 0, clean
  ```

  The third arm is not decoration: without it a check that refused every bracket would
  pass the second for the wrong reason. **An injection is evidence only if the thing it is
  meant to hide is there to be hidden** — the STORE lane's *a green probe of the wrong
  shape is not a refutation*, met from the other direction.
- **A census states its PATTERN, not only its number.** Two independent audits of the
  same file set differed by exactly one, and neither party had miscounted — they had
  counted through different patterns:

  <!-- table-total: none -->
  | pattern | cac9bda, all | cac9bda, excl. selftests | HEAD |
  |---|---|---|---|
  | `scripts/check-*.cmake` (the glob a ticket names) | **20** | **18** | **34** |
  | `grep 'check-.*\.cmake$'` (unanchored) | 21 | 19 | 36 |

  The whole difference is `scripts/script-check-canary.cmake` — and at HEAD also
  `script-check-warning-canary.cmake` — because *"script-**check-**canary.cmake"*
  contains `check-`. The unanchored form matches them; the glob does not. That is the
  `pkill -f` lesson arriving in a `grep`, twice in two days: **a pattern is broader than
  its author reads it as**, and a bare number carries no way to tell which pattern
  produced it.

  Worth recording that the second instance was a MANAGER quoting a corrected number back
  to a developer — the correction was wrong for the same reason as the thing it was
  correcting. Nobody is outside this; the remedy is that the pattern travels with the
  figure, not that people count more carefully.
- **A stated total beside a table is DERIVED from it, or it is a second claim.** A
  hand-maintained number describing a hand-maintained list is two sources of truth
  wearing one hat: editing the table does not update the number, nothing checked that it
  did, and the failure is silent because the sentence still reads correctly. Measured on
  ONE table across THREE consecutive commits — `.agent/rules/metrics-and-observability.md`'s
  four-states table — where each commit fixed the previous count and introduced the next,
  all three by the same author, in the same file, on the same day, with the rule about
  instruments that miscount open in the next tab
  ([#780](https://github.com/LASTRADA-Software/fastcached/issues/780)). At three
  occurrences it is not a discipline problem: prose did not prevent it and re-reading did
  not prevent it. It is the `RowsInEnumeratorOrder` argument arriving in markdown — the
  count comes from the table or it is not a count.

  `ctest -R table-totals` derives it. Two decisions are recorded in the check rather than
  in a commit message, because picking either silently is what the ticket refuses:
  - **The multipliers are PARSED, not banned.** That table's rows carry `(twice)` and
    `(three times)`, so 8 rows describe 12 collapses and the prose states BOTH figures. A
    checker that counts rows is wrong for exactly the table that motivated it, and wrong
    in the direction that looks right — `8` beside a table of 8 rows is entirely
    plausible. Two quantities, two modes, both derived. A multiplier the check cannot
    read is a REFUSAL, never a silent 1.
  - **The marker is MANDATORY, and `none` is how a table says it has no total.** Not
    opt-in: an opt-in marker is exact about the tables it knows and silent about the ones
    it does not (#492). That is the `Refuse` / `RefuseWithoutCounter` idiom from
    [`metrics-and-observability.md`](metrics-and-observability.md) — *deliberately
    uncounted must not be spelled like forgot* — costing one comment line per table and
    buying the property that a table ARRIVING cannot join the tree unchecked.

  Three things the implementation got wrong first, all found by watching rather than by
  reading, and all the same family as the bullet above:
  - **The scope census was wrong by 100%.** An `^\|`-anchored scan found 10 tables and
    was silent about 10 more, because a table INSIDE a bullet is indented. The scope
    decision was then argued from the wrong number, and the miss surfaced only because
    one skipped table happened to catch the eye — which is the census bullet's own lesson
    met while implementing the check for it.
  - **The figure NEAREST a table is the one that describes it.** A first-match search read
    the four-states table as claiming 4, because its paragraph legitimately states the
    older "five separate times, in four different instruments" as well as the current
    "twelve collapses across eight instruments". The check refused a CORRECT tree, which
    is how it was found.
  - **A number need not sit immediately before its noun** — "the six remaining
    `file(STRINGS)` readers" is two words apart — and a marker forced to spell the
    intervening words would be a copy of the sentence, which is a third source of truth.

  **What is deliberately NOT checked**, recorded because the ticket asked: `AGENT.md`'s
  tripwire restates *"five times in four instruments in one session"*. That figure is not
  derived from the table — it describes the FIRST session, five of the twelve — so
  asserting it against the table would assert a wrong thing.
- **A scan whose needle can appear in its own source, or in the text it reports, will
  match itself.** The guard added for
  [#678](https://github.com/LASTRADA-Software/fastcached/issues/678) scans
  `check-e2e-helpers.sh` for direct increments of its failure counter, because the
  property it protects — that `note_failure` is the only thing that touches the counter,
  so a failure cannot be counted without being named — holds only while that is true.
  It scans its own file. Its first run reported **three** increments where there is one,
  and it was right: the needle appeared verbatim in the needle, in the `grep` argument
  and again in the `grep -n` that prints the offending lines.

  It failed in the safe direction — it refused rather than reporting a wrong verdict
  about the subject — and it was still wrong, which is the half worth keeping.
  **Failing closed is not the same as being correct, and a guard that refuses valid
  input is a guard somebody deletes.**

  **The remedy is construction, not exclusion.** Spell the needle so it cannot be its
  own match — a regex with `[+]` where the literal has a `+`, an anchor, a character
  class — rather than subtracting the known self-hit. **An exclusion list grows and a
  construction cannot**: the next site that happens to contain the needle is a defect
  the list has to learn about, while a needle that cannot match itself is finished.

  Distinct from the pattern-breadth entry above, and the two are easy to confuse
  because both end in a count that is too high. There the needle matched something
  **else** that legitimately contained it (`script-check-canary.cmake` contains
  `check-`); here it matched the **scan's own text**. The tell is whether removing the
  scan changes the count.

  A second instance was reported in the same session and is **not verifiable from this
  tree** — a settle check written as `grep -q "tests failed"`, which matches inside
  `0 tests failed out of 57` and so reads a clean run as a failing one. Recorded as
  reported rather than measured, because the code was never committed; it belongs here
  because it is the *other* half of the rule, where the needle appears in the text the
  instrument **reports** rather than in the text it is written in.
- **"Only 4 of 16 defend" was wrong when it was written, and no slicing recovers it.**
  #510's denominator matches nothing on its own cited ref: not 20, not 18, and not the
  **13** that read file content. Today there are 34, none added by the branch that
  audited them. **A ticket cannot be closed against a count that no longer describes the
  tree**, so the acceptance became a per-reader audit and the consolidation stayed with
  [#495](https://github.com/LASTRADA-Software/fastcached/issues/495), which is where
  `check-glob-traversals.cmake`'s own comment already sent it. Absorbing it would have
  closed one ticket by silently swallowing another.
- **A count asserted in prose is a claim nothing verifies.** Two comments went stale in
  one session, both because the thing they counted changed underneath them and nothing
  was watching. "This is the fourth copy of this idiom" was counting the SPLITTER when
  it was written and now reads as a claim about `fastcached_globs_to_regex`, of which
  there are three. A note asserting `src/tests/CMakeLists.txt` holds 25
  `$<TARGET_FILE:>` references survived a rebase that added a registration only because
  that registration happened to use `${CMAKE_COMMAND}` — true by luck, not by
  construction, and it would have gone false silently. Both were written by someone who
  had just spent hours on exactly this failure mode. So prefer prose that states the
  INVARIANT ("each root is walked once") over prose that states a census ("there are
  three copies"), and where a number is genuinely load-bearing, put it where a CHECK
  reads it rather than where only a human does — which is what `check-glob-traversals`
  does for the count that matters.
- **An instrument whose failure mode is an empty or unexpected value must not have a
  predicate that sorts it into a named outcome.** Two instruments invented their
  subject in one session, both by a negation quietly catching a value nobody
  enumerated.
  - A `gh ... --jq` query whose escaping the shell had mangled printed a parse error
    and returned nothing; the surrounding `${res:-JOB NOT PRESENT}` turned that
    nothing into a confident sentence, **identically for all four runs queried**. The
    reading was the exact opposite of the truth — the job runs and passes. The tell
    was that all four rows agreed *perfectly*, which is what a broken query looks like
    and also what a real pattern looks like, so agreement is not corroboration when
    the instrument can fail wholesale.
  - `gh run view --json jobs` renders a **running** job's `conclusion` as the empty
    string, not `null`, so `select(.conclusion != null and ...)` matched every
    in-progress job and reported it as FAILED. That is *unfinished* classified as
    *failed* — the four-states rule in
    [`metrics-and-observability.md`](metrics-and-observability.md), violated by the
    predicate rather than by the author. The tell was walked past: a required check
    failing in a merge queue after passing on the PR, on a byte-identical tree, is not
    a plausible event.

  So: check the query's own exit status rather than only its output; **enumerate the
  states and print counts**, so an unexpected value shows up as unexpected instead of
  falling into whichever bucket a negation happens to catch; and never let a default
  substitution convert "I could not see" into a statement about the subject. A query
  that failed and a subject that is absent are different findings, and a `:-` default
  spells them the same way.
- **A hygiene script `ctest` runs is constrained to bash 3.2, because macOS ships
  bash 3.2.** Apple has not shipped bash 4 since the licence change, so `/bin/bash`
  on the macOS runner is from 2007. A script registered in the **default** ctest
  set runs on every platform CI builds, and the constraint is invisible from the
  Linux box such scripts are written on. The bash-4 constructs to avoid are few
  and worth knowing by name:

  <!-- table-total: none -->
  | avoid | use |
  |---|---|
  | `mapfile` / `readarray` | `while IFS= read -r x; do a+=("$x"); done < <(...)` |
  | `declare -A` | parallel arrays, or a `case` |
  | `${var^^}` / `${var,,}` | `tr '[:lower:]' '[:upper:]'` |
  | `local -n` | pass the value, or use a global with a stated name |

  Keep the **process substitution** when replacing `mapfile`: a pipeline into the
  loop reintroduces the `pipefail` trap recorded below, where a `grep` that matches
  nothing takes the script down.

  This is the section's own subject arriving through a door it did not name.
  `merge-queue-contexts` used `mapfile`, passed everywhere it was developed, and
  failed only on `macOS-clang-release` with `mapfile: command not found`. And the
  constraint was **already known**: `scripts/coverage.sh` carries a comment saying
  exactly this, in a place only a reader of `coverage.sh` would find it. A lesson
  recorded where it cannot be reached by the next person who needs it has not been
  recorded. The same holds for the
  `mapfile` and `declare -A` inside `pr-labels.yml`.

  **`scripts/tidy-sweep.sh` is the exception, and how it is one is the rule.** It
  uses `mapfile`, `declare -A` and `wait -n`, which are intrinsic to the scope
  computation rather than incidental to it, so it cannot be written to 3.2 the way
  the fixtures above are. It is registered in the default `ctest` set anyway
  (#588), because the alternative was the state it was in: the only thing running
  its self-test was the `clang-tidy` job whose scope it computes, which is not an
  independent check of anything. What makes that safe is that **below its floor it
  exits `77` and the registration carries `SKIP_RETURN_CODE 77`**, so a stock macOS
  runner reports SKIPPED rather than red -- and the message says a skip is not a
  pass. Skipping is the fourth state, not a quiet success.

  Two ways to get this wrong, and they are symmetric: registering such a script
  without the skip code turns a silent non-registration into a red macOS leg, and
  "repairing" that by dropping the registration on macOS at configure time puts it
  back to a check that does not exist and does not say so. The guard cannot live at
  configure time in any case -- the floor is a property of whichever `bash` the
  machine has at RUN time, and a macOS runner can carry 3.2 at `/bin/bash` and 5.x
  from Homebrew, which `if(NOT WIN32)` cannot tell apart.

  It is also a live instance of [#336](https://github.com/LASTRADA-Software/fastcached/issues/336):
  `local-gate.sh` cannot run in an agent-created Windows worktree, so nothing
  exercised this before CI did.

- **A local gate cannot see a configuration it does not build, and advice nobody
  runs is not a gate.** `scripts/local-gate.sh` is that advice as a script:
  clang-format at the pinned version, then `clang-debug` and `gcc-release`, refusing
  to run `ctest` against a build that did not complete. It exists because this
  paragraph was already here and was skipped twice in one branch -- once for a GCC
  `-Wnull-dereference` through an inlined `memcpy` that clang emits at no level, and
  once for a clang-tidy check the binary on `PATH` had never heard of. Both cost a
  full CI cycle for a configuration the developer already had.

  The default agent
  preset is `clang-debug`: one compiler, one standard library, `-O0`, sanitizers on.
  CI is four more — GCC at `-O3`, clang-cl, MSVC, and clang against **libc++** on
  macOS — and each of the three defects that reached CI on the Raft branch was
  invisible to every configuration below it. GCC 14 at `-O3` reports
  `-Wnull-dereference` inside `std::optional::value_or` where clang does not;
  clang-tidy 22 knows checks clang-tidy 20 has never heard of; and libc++'s
  `uniform_int_distribution` is a different function from libstdc++'s. Before
  pushing a change that touches a header everything includes, a randomness or
  timing seam, or anything a test harness's determinism rests on, build **at least
  one release configuration and one non-clang compiler** locally —
  `cmake --preset gcc-release` and `clang-release` both exist and both run in WSL.
  - **libc++ does not have every C++23 library feature libstdc++ has, and the
    failure is a compile error on one platform only.** `std::views::enumerate`
    compiles under GCC 13's libstdc++ and does not exist in libc++ at all, so
    `FleetView.cpp` built clean locally and broke the macOS package job -- a
    configuration whose *first* compile of the change is in CI. This is a different
    shape from the `uniform_int_distribution` case above: not two implementations
    disagreeing, but one of them not shipping the header's contents. Before reaching
    for a C++23 *library* facility that this codebase does not already use
    somewhere macOS compiles, check that it does -- `grep` for it in non-test code
    is enough, since the package job builds the library and every app. The
    workaround is nearly always a C++20 spelling: `std::views::iota` over the
    index range says what `enumerate` says and is ten years older.
  - **And the mirror holds: a Windows verification is green about a smaller set of
    questions than it looks.** The paragraph above counts what a Linux-only gate
    misses; this direction is measured too. One session's four tickets produced
    **four defects a fully green MSVC run of ~2997 tests could not have reported** --
    a partial designated initializer (`-Wmissing-designated-field-initializers`,
    which clang makes an error and MSVC does not diagnose at all), a leaked coroutine
    frame in a test fixture (ASan; no Windows preset runs a sanitizer), and two
    clang-tidy findings on code MSVC compiles happily
    (`readability-redundant-typename`, `performance-unnecessary-copy-initialization`).
    Not one of the four is reachable by running *more tests* on Windows: three exist
    only in the analyser and the fourth needs a sanitizer. So the inference to refuse
    is "the Windows run was green, therefore it was green about everything" -- the
    Windows leg answers a different question, not a weaker version of the same one.
    Four in four tickets is recorded rather than one anecdote, because a rule argued
    from a single instance reads as an accident.
- **`clang-format` and `clang-tidy` after every change — at the version CI pins.** Both jobs
  run the `$CLANG_TOOLS_VERSION` binary (`.github/workflows/build.yml`), and successive LLVM
  releases do not agree with each other: the style job compares against a *newer formatter*,
  and the clang-tidy job enables *checks that did not exist* in an older one. So a tree that is
  clean under whichever binary happens to be on `PATH` can still be rejected — a red build for
  code nobody mis-wrote, and one no local run catches unless it uses the same version. Name the
  version explicitly rather than relying on `PATH`:
  `git ls-files '*.h' '*.hpp' '*.cpp' | xargs clang-format-$V --dry-run --Werror --style=file`,
  and `-DCMAKE_CXX_CLANG_TIDY=clang-tidy-$V` **in a build directory of its own**. Found three
  times in one branch: four files reformatted by 22 after 20 had passed them, four
  `find(...) != npos` tests that only 22 reports as `readability-container-contains`, and five
  `std::lock_guard`s that only 22 reports as `modernize-use-scoped-lock`. **The preset alone is
  not that sweep**, and that is the trap: `clang-debug` sets `CMAKE_CXX_CLANG_TIDY=clang-tidy`,
  which on a machine carrying both resolves to whichever `PATH` finds first — 20 in this
  project's WSL image, where 22 sits right beside it as `clang-tidy-22`. So a `clang-debug`
  build reports "clang-tidy clean" in exactly the way that means nothing, and the version it
  used is printed nowhere. Configure a second build directory naming the version, and run that.
- **A script that NAMES a tool version must name it everywhere that version matters, and
  the gate that says so had the defect it documents.** `local-gate.sh` resolved
  `clang-format-$V` by name, and then let the `clang-debug` preset take its analyser from
  `PATH` -- with the paragraph above written in its own header, four lines from the code that
  ignored it. An argument carried one call short is the shape to look for: the reasoning is
  present, correct, and applied to one of the two tools it was written about. The gate now
  resolves `clang-tidy-$V` to an absolute path, **before anything is built**, refuses by name
  when it is absent rather than falling back, passes it to the configure, and prints which
  binary it used -- and none of that is loosened by `--no-format`, which names the other tool.
  - **A cached `find_program` result outlives every reason it was chosen**, so the pin has to
    be checked against the build directory and not only passed to it. `find_program` never
    revisits a filled cache entry, and the gate configured only when `CMakeCache.txt` was
    ABSENT -- so a tree kept whichever `clang-tidy` it first found forever, and **re-running
    the gate could not repair it, because re-running the gate is what skipped the configure.**
    Measured on this repository's own WSL image: a `clang-debug` directory the gate itself had
    created sat on `CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy`, which is 20, while every run
    reported clean. The same shape as the stale `FASTCACHE_CC-NOTFOUND` that kept build trees
    on sccache silently, and `CLANG_TIDY_EXE-NOTFOUND` is that shape exactly -- a value, which
    compares unequal, never an absence to be tolerated.
  - **`cmake/portable/ClangTidy.cmake` needs no change and must not get one.** It stays
    generic and droppable into other repositories, and a pre-set `CLANG_TIDY_EXE` cache entry
    already short-circuits its `find_program` -- verified, on a fresh directory and over an
    existing entry, rather than assumed. `-D` on the configure line is the whole mechanism.
  - **The gate tidies ONE of its two presets**, because `ENABLE_TIDY` is on in `clang-debug`
    and off in `gcc-release`. That is a fact a reader would otherwise infer from the absence
    of an argument, so it is a column of the script's preset table and a run prints it.
  - `ctest -R local-gate-selftest` drives the decision against synthetic CMake caches -- a
    directory on the wrong analyser, one that never found any, one with no entry, one already
    correct -- because the gate itself builds two whole configurations and cannot be a test,
    while the part that can be silently wrong is pure.
- **A gate that stops at the first red leg must SAY what it skipped, or stopping early
  becomes a lie.** `local-gate.sh` fails fast, which is correct -- forcing a full second
  configuration after a known failure makes the gate something people stop running, and
  the script's own header already names that hazard. What was wrong is that the refusal
  was the *whole* report: `GATE FAILED: clang-debug tests` and nothing else, so the
  reader supplied "and the rest passed" from optimism. It had not been asked
  ([#501](https://github.com/LASTRADA-Software/fastcached/issues/501)).

  Measured in #493: gate runs 1, 3, 4, 5 and 7 all failed inside `clang-debug` and
  **none of them reached `gcc-release`**, while a GCC-only
  `-Werror=maybe-uninitialized` -- raised by GCC 14 at `-O3` inlining a ternary into
  `<optional>`'s own `operator!=`, and emitted by clang at no optimisation level and by
  MSVC not at all -- sat in the tree through every one of them. That is almost
  word-for-word the case the script's header gives as the reason `gcc-release` exists.
  It surfaced only because a developer treated an unrun leg as *unrun* rather than as
  covered.

  So **every** run now ends with a per-leg verdict -- `passed`, `FAILED`, or `NOT RUN`
  with the words "has reported NOTHING" -- on the green path too, because a summary
  that appears only on failure is one nobody has read when it matters. Three
  consequences worth keeping:
  - **The renderer is pure**, taking `preset=state` pairs as arguments. The defect was
    a REPORTING defect, and a report that can only be exercised by building two whole
    configurations is the same defect one level up. `ctest -R local-gate-selftest`
    drives every verdict in milliseconds, including the inequality the acceptance
    actually turns on: `FAILED` and `NOT RUN` for one preset must never coincide.

    **This is the same move `node-scratch-isolation-e2e` needed, and it is now the
    standing answer for an instrument whose own defect is invisible.** That fixture's
    verdict logic could only be reached by running the expensive thing it wrapped;
    splitting the DECISION out as a pure function over a record and leaving
    acquisition alone took it from 53 seconds and a `RUN_SERIAL` to 0.3 seconds and
    neither, and made every branch cheap enough to mutate one at a time
    (`.agent/rules/testing.md`). At least four instruments here have had that shape.
    When a gate, fixture or classifier cannot be tested without paying for what it
    measures, split the decision from the acquisition rather than accepting that the
    instrument is untestable.
  - **An unrecognised state renders as unrecognised.** A fourth state arriving at the
    default arm as `passed` would recreate the bug exactly, one level down. Skipped,
    absent, unstarted and failed are four states here as everywhere.
  - **The states are derived from the preset table**, and the green line no longer
    spells "(clang-debug + gcc-release)" as a literal -- a third row would have been
    silently missing from the gate's own statement of what it had just done.

  Reading a red log is the same trap once more: ninja prints `FAILED:` and puts the
  error text several lines BELOW it, so a wrapper filtering on `error|warning:` alone
  surfaced an unrelated zstd warning and none of the actual failure. **A filter that
  reports nothing and a run that found nothing render identically.** Read the raw log
  the message names, or filter on `FAILED:` as well.

- **A run that never concluded is the fifth state, and a trap cannot report it here.**
  The verdict came from `fail()` and from the green path, and NEITHER runs when the
  process is killed -- so a run terminated by a signal ended with no `GATE FAILED`, no
  `LOCAL GATE PASSED` and, after #501, no leg block either. The only thing separating
  it from a completed run was the ABSENCE of both lines, and absence is equally what a
  scrolled-past line, a truncated log and a lost terminal look like
  ([#584](https://github.com/LASTRADA-Software/fastcached/issues/584)). It is not
  hypothetical: every lane on a shared machine runs these script names out of different
  worktrees, so a name-matched `pkill -f local-gate.sh` in one lane hits every other
  lane's run -- the worktree path appears nowhere in the pattern. Twice in one night.

  **The trap route was measured and rejected, and the measurement is the entry.** Bash
  defers a TRAPPED signal until the running foreground command returns, and this gate's
  foreground commands are `cmake`, `ninja` and `ctest`. On bash 5.3, a SIGTERM sent at
  **t=2s** into a 6s foreground command ran the handler at **t=6s** -- a trap that fires
  only after `ninja` finishes is close to no trap at all. The standard workaround,
  backgrounding the command and `wait`ing for it, does fire immediately (**t=2s**,
  measured) and is **worse**: the child is orphaned and reparented, so the gate would
  print a tidy `did-not-conclude` while leaving `ninja` still writing into the build
  directory. Measured on a real killed run of this gate mid-build-leg: **34 descendant
  processes** left alive under the worktree, which had to be cleaned up by matching
  `/proc/<pid>/cwd`. A developer's re-run then races an orphaned build in the same
  `binaryDir` -- the two-gates-in-one-build-directory defect already listed above. So
  the deferral is not a detail to work around; it is the argument for the other route.

  The route is a **start marker plus a rule**, and the rule is: *a log with a start
  marker and no terminal line DID NOT CONCLUDE -- re-run it, never interpret it, and
  never read it as a red gate.* `did-not-conclude` is a distinct outcome from `failed`
  because they are fixed in different places: one is somebody else's `pkill`, the other
  is your code. Four things make that more than prose:
  - **The marker names its own subject** -- pid, worktree and commit. A verdict that
    does not say what it is a verdict about has already cost a run here, when a `/tmp`
    wipe left one gate's log where another's goes and it reported a failure that was
    already fixed. The pid is there because the cause is a name-matched kill, and the
    tree because that is the field such a kill does not look at.
  - **The rule is a function, not prose**: `local-gate.sh --classify=<log>` answers
    `passed` / `failed` / `did-not-conclude` / `no-gate-run`, each with an exit status
    of its own, so the tooling around a run can tell. Pure -- log text in, one word out
    -- which is the standing answer above applied once more. `passed` and `failed` keep
    the gate's own `0` and `1`; `2` is skipped because it is the script's USAGE status,
    and `--classify=$LOG` with an unset `LOG` lands there -- a caller reading only the
    status would take *you typed that wrong* for *the run was killed*. Those two are not
    commensurable, so the self-test asserts no outcome sits on it, and the constant is
    defined once and read by both the argument loop and the table rather than spelled
    twice.
  - **The LAST marker wins, and a terminal line counts only after one.** A log file
    gets reused, so a green run followed by a killed one is `did-not-conclude`, which
    `grep -c PASSED` gets backwards; and a terminal line with no marker before it is
    `no-gate-run` rather than the verdict it resembles, because it cannot be attributed
    to a run the log can show. Both escalate not-knowing to *re-run it*, which is the
    only direction that cannot silently vouch for a tree.
  - **Capture both streams.** The failure line goes to stderr and the pass line to
    stdout, so a log holding one of them can answer half the question.

  Watched refusing, which for this one takes two forms because the mechanism has two
  halves. The classifier: four breaks -- `did-not-conclude` given `failed`'s status, the
  first marker winning instead of the last, substring matching instead of line-anchored,
  and a terminal line counting with no marker before it -- each reddened a named case.
  The marker: a **real** SIGTERM to a **real** run mid-build-leg, by pid and never by
  name match, which exited 143, left a log with a start marker and zero terminal lines,
  and classified `did-not-conclude` at exit 3. A start-marker scheme never interrupted
  is untested in the only case it exists for.

  And the self-test gained a `N checks ran, M failed` line in the same change, because
  without it none of the four breaks above could be believed: `expect` is silent when
  it passes, so a run that died half way through is indistinguishable from one where
  everything passed, and *no failures printed* reads as *the guard did not fire*. That
  is this ticket's own confusion one level down. It immediately earned itself -- one of
  the new checks claimed to assert that `did-not-conclude` and `failed` have different
  exit statuses and actually compared whole table ROWS, whose sentences differ whatever
  the statuses do. It read green for exactly the collapse it names, and only watching it
  fail to fire found it. **A signal that cannot be false in the failing case is not
  evidence**, and a check is not evidence that it fires until it has been seen to.

- **A reference build passes `-DUSE_COMPILER_CACHE=OFF`, and the gate is a reference build.**
  `local-gate.sh` invoked `cmake --preset` without it, and `USE_COMPILER_CACHE` defaults to ON,
  so both its configurations were fronted by whichever launcher happened to be installed, at
  whatever version, with no check and no mention. Measured: 148 `LAUNCHER = ` lines in
  `clang-debug` and 618 in `gcc-release`, all pointing at an installed `fastcache-cc` hundreds
  of commits behind master and subject to [#368](https://github.com/LASTRADA-Software/fastcached/issues/368)
  ([#471](https://github.com/LASTRADA-Software/fastcached/issues/471)).

  **The rule was already standing, in a file sitting beside the one that ignored it.**
  `scripts/launcher-replay-e2e.sh` names "the standing `-DUSE_COMPILER_CACHE=OFF` rule" in its
  own header and records what it is for: in #319 a cache-backed build of a test binary
  segfaulted where the same commit built cache-off passed, and nothing in CI could have
  reported it. `CMakePresets.json` carries the same value on `clang-coverage` for an
  independent reason. The project had decided this twice and written it down twice, and the
  gate was the only reference build in the tree that dissented. **Stating a rule in the file
  that obeys it is how the file that does not obey it never learns about it** -- the same
  mechanism as `tsan-gate.sh` documenting that macOS has no `timeout(1)` while `cluster-e2e.sh`
  called it anyway. A rule with one instance is a comment; a rule with two needs a check.
  That paragraph is gone (#488 replaced the resolver with `run_bounded`), and how it ended is
  the sharper half: it also claimed the `clang-tsan` preset runs on macOS, which it never has,
  so the fallback it justified had never executed anywhere. **What failed to travel was not a
  fact but a plausible sentence** -- which is why the remedy was a shared implementation plus a
  scan (`check-e2e-helpers.sh`), and not a better comment. A reader cannot tell a measured
  sentence from a remembered one, and neither can a reviewer.
  - **Pinning the other two tools is the argument for REMOVING this one, not for versioning
    it.** `clang-format` and `clang-tidy` are pinned because their version changes the verdict
    and there is a canonical version to pin *to*. A compiler cache has neither: no canonical
    version, and it is supposed to be verdict-**neutral**. When it is not, it substitutes an
    object the tree did not produce. The tempting symmetry -- require the launcher to be the
    build of the current tree -- is unsound twice over. `git describe` on a dirty tree yields
    `X.Y.Z-N-gsha-dirty`, and two different working trees produce that same string, so it is
    not an identity; and the only launcher that could ever match is one built from the tree
    under gate, which routes the gate's objects through the very change being gated. That is
    worse than an unknown-vintage launcher, which is at least independent of the diff.
  - **Passing the flag is not the same fact as no launcher being in effect.**
    `cmake/portable/CompileCache.cmake` returns early when `CMAKE_CXX_COMPILER_LAUNCHER` was
    set externally -- a preset, a toolchain file, an older `-D` -- and leaves it untouched. So
    the gate reads the **generated build**: `LAUNCHER = ` in `build.ninja`, which is already
    this project's idiom for the question, since `launcher-replay-e2e.sh` checks the same
    string from the other side to prove a launcher *is* in use. The first design read
    `CMAKE_CXX_COMPILER_LAUNCHER` back out of `CMakeCache.txt` and would have reported "no
    launcher" against both live gate directories: `CompileCache.cmake` sets it as an ordinary
    directory-scope variable and never as a cache entry, so that guard could not fire -- inside
    the fix for a ticket about guards that cannot fire. It was caught by checking the two real
    caches rather than by reasoning about the CMake documentation.
  - **Whatever the refusal reads is also a reason to configure, or the gate cannot repair the
    state it refuses.** CMake enters `-D` values into `CMakeCache.txt` and writes that file
    even when the configure then FAILS, leaving the old `build.ninja` untouched -- so a first
    run can leave a cache reading `OFF` with the right analyser beside a launcher-fronted
    build. Judged from the cache alone, every later run finds nothing to configure, refuses on
    the stale build, and blames an external `CMAKE_CXX_COMPILER_LAUNCHER` that nobody set --
    and re-running can never fix it, because re-running is what skips the configure. That is
    the analyser bug above, reopened one file over. So `configure_reason` reads the generated
    build too, through the same `launcher_verdict` the refusal uses: one observable, not two
    that can disagree, and the refusal's message is then true because the only state that
    reaches it is one that survived a configure which actually ran.
  - A missing `build.ninja` is `unknown`, one that cannot be READ is `unreadable`, and neither
    is ever folded into a count of zero: zero is a reading, the other two are the absence of
    one and the failure to take one. `awk` on a file it cannot open exits **without running
    its `END` block**, so it prints nothing -- and an empty answer reaching the caller's
    default arm renders a failed reading as the worst positive one, refusing a build nobody
    could read while blaming a launcher nobody set. A `[[ -f ]]` that passes for a file whose
    permissions deny it is exactly where the fourth state hides. `USE_COMPILER_CACHE` absent
    reads as ON, since that is the option's default.
  - The match is **not** anchored to ninja's two-space indent, deliberately: anchoring fails
    OPEN if that spelling ever changes, because a count of zero reads as a clean build. Loose
    can only over-count, which for a gate is the safe direction -- and since
    `CMAKE_<LANG>_LINKER_LAUNCHER` emits the same binding, the number is reported as
    launcher-fronted **edges** rather than as compile edges.
  - **The gate does not own its build directories, and says so.** Every preset has one
    `binaryDir`, `out/build/${presetName}`, and these are the trees this project tells
    developers and agents to build in; `-D` writes a cache entry and `option()` never
    overrides one, so a gate run turns the compiler cache off there *permanently*. That is
    accepted as a STATED cost -- the run prints what it did, that it is permanent, and how to
    undo it -- and gate-owned directories are
    [#487](https://github.com/LASTRADA-Software/fastcached/issues/487). A silent change to how
    a tree builds is the defect this whole entry is about, so the fix for it must not commit a
    quieter version of it one directory over.
  - The one-time cost is stated by the run that incurs it. Dropping the launcher rewrites every
    compile command, so ninja rebuilds the configuration from scratch once; a developer
    watching that with no explanation files it as breakage. An explained cost is a cost.
  - **What it costs, measured rather than asserted** -- and as a table of conditions, because a
    single number here is the wrong quantity in whichever direction it is quoted. One
    `gcc-release` configuration, 630 object edges, from scratch, 32 jobs, WSL2 on a 9p mount:

    <!-- table-total: none -->
    | leg | wall clock |
    |---|---|
    | launcher on, nothing stored yet (629 fronted edges) | 224 s |
    | **launcher off** (0 fronted edges) | **230 s** |
    | launcher on, everything replayed | 94 s |

    So a **full** rebuild of one configuration costs ~136 s more, about 2.4x -- and a MISSING
    cache costs the same as no cache, which is why the first row must not be quoted as the
    price of this change. Neither may the third: the gate is incremental, so a from-scratch
    build happens once when this lands, and thereafter only when something invalidates
    everything anyway. Note also that the warm row is exactly the exposure -- 629 objects
    replayed from an earlier build is the state in which "the objects need not match this tree"
    is a live claim rather than a theoretical one.
- **When `clang-debug` cannot be built, get the sanitizer from GCC instead.** That
  preset is the only one that runs ASan, and on a host where it cannot configure at
  all the tempting conclusion is that no sanitizer coverage is available locally. It
  is: GCC has ASan too, and a throwaway tree costs one configure.

  ```sh
  cmake -S . -B out/build/gcc-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
        -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
        -DFASTCACHED_ENABLE_TLS=ON -DPEDANTIC_COMPILER_WERROR=OFF
  ```

  **ASan only, never `address,undefined`.** UBSan's instrumentation defeats GCC's
  constant evaluation of the option tables' lambda addresses, so `Options.hpp`'s
  `TableIsWellFormed` `static_assert` stops compiling -- a build error with nothing
  to do with the code under test, and an easy reason to abandon the idea.

  A use-after-free reached CI through two jobs while this was thought unavailable.
  The local run that catches it takes about a minute.

- **A memory bug reports only if something disturbs the freed block, so run the
  WHOLE suite.** The case that failed in CI passes under ASan on its own: nothing
  reuses the allocation, so nothing is read across a boundary the interceptors
  watch, and the test reports success. The same held for a standalone repro --
  decode, read, nothing in between: clean; sixty allocations inserted between the
  two: `heap-use-after-free` immediately. A quiet sanitizer run on one test is not
  evidence that a diagnosis is wrong.

- **After reverting files to reproduce a bug, check that the build SUCCEEDED.**
  Reverting two files to the parent commit left an unrelated translation unit
  failing to compile; ninja stopped, and the binary that ran was the *previous* one.
  It passed, which looked like proof the bug was not real. `ninja: build stopped`
  scrolls past above a green test run, and a reproduction that did not rebuild is
  not a reproduction.

- **A sanitizer that is on in the cache is not a sanitizer that is on in the build.**
  `cmake/portable/Sanitizers.cmake` initialised `SANITIZER_COMPILE_OPTIONS` to `""`
  as a normal variable, published the real flags through
  `set(... CACHE INTERNAL ...)`, and then `list(APPEND)`ed one more flag to the same
  name. Under **CMP0126 NEW** -- which `cmake_minimum_required(3.28)` selects -- a
  cache `set` no longer removes a normal variable of that name, so the empty one
  keeps shadowing the cache. The append therefore starts from nothing and
  `add_compile_options` receives `-fno-sanitize-recover=undefined` and no
  `-fsanitize=` at all, while `CMakeCache.txt` says
  `ENABLE_SANITIZER_ADDRESS:BOOL=ON` and the configure log prints
  `[Sanitizers] Enabling: address,undefined`. Every signal an author would check says
  yes; not one object is instrumented -- and not on a re-configure, on a **completely
  fresh build directory**, which is what CI makes. So the project's sanitizer gate had
  never run: not locally, and not in the `clang-tidy` job that ends with
  `ctest --preset clang-debug`. Turning it on found a heap-use-after-free in
  `EpollSocket::Close`, a leaked coroutine frame per dial that `IReactor::CancelPending`
  now prevents, and a plain use-after-free in a test fake that had been passing 300
  consecutive runs.
  The flags are assembled in locals and published once now. Two things to keep: the
  check that actually answers the question is `grep -o -- '-fsanitize=[a-z,]*'` **on
  `build.ninja`**, since the cache and the log are precisely the two places that lie;
  and this is the same class as the `USE_COMPILER_CACHE` configure probe -- a tool
  that silently does nothing is worse than one that is off, because the second is
  visible.
  - **A guard written above an existing one can be silently discarded, and it
    disappears from exactly the cases that already needed thinking about.** Adding
    a step-level `if:` to `.github/workflows/build.yml` inserted a second `if:` key
    above three steps that already carried
    `if: startsWith(matrix.preset, 'clang')`. YAML keeps the **last** duplicate key,
    so the new gate evaporated on precisely the conditional steps -- the ones a
    reader is least likely to re-check, because they visibly have a condition. No
    warning from YAML, none from Actions, and the workflow ran. Same family as the
    sanitizer above: the knob reads set, and nothing it names is in effect. Merge
    into one expression (`gate && (original)`) rather than stacking, and scan for
    adjacent `if:` lines afterwards -- `awk` over the file is enough, and it is the
    only thing that distinguishes "gated" from "gated on paper".
  - **So a sanitizer job proves the tree only once something proves the sanitizer.**
    `scripts/tsan-gate.sh` will not report clean until it has answered two separate
    questions, because they fail separately: an **undefined** `__tsan_init` reference
    in each artefact's **own object files** says *this artefact* was instrumented,
    and `src/tests/TsanCanary.cpp` -- a deliberate data race, built by the same
    `add_compile_options` as everything else -- says the runtime still detects and
    reports. A `TSAN_OPTIONS`, a suppressions
    pattern or a stripped runtime can break the second while the first still holds.
    The canary is run **with the suppressions file active**, so a wildcard broad
    enough to swallow an obvious race fails the gate instead of silently disarming
    it. Every refusal has a message of its own, because each is fixed somewhere
    different, and each was verified by making it happen -- the list lives in that
    script's header and deliberately nowhere else. It said "five refusals" in three
    files, in three orders, having dropped the two hardest to reason about: a hard
    count restated beside the thing it counts is a fact with no owner.
  - **`__tsan_init` in a BINARY is a property of the link, not of any translation
    unit** -- so asking `nm --defined-only` of a binary cannot prove instrumentation,
    and for three years that is what this gate did. The symbol is *defined by the
    sanitizer runtime*, which the link pulls in whole whenever `-fsanitize=thread` is
    on the link line. Measured, same source, two objects, two binaries: a canary
    whose TU was compiled with **no sanitizer flag at all** produced a binary
    carrying `__tsan_init` and passed the proof. Ask the **objects** instead, where
    the reference is *undefined* and cannot be borrowed from a runtime the object is
    not linked to. Same symbol; what changed is `--undefined-only` and what it is
    asked of.
    Three things that only appear once you look at where the proof is *used*:
    **the canary was never the exposed artefact.** An uninstrumented canary fails
    closed -- it exits 0, reports no race, and the gate refuses -- though it used to
    do so while blaming `TSAN_OPTIONS`, the suppressions file and the runtime, all
    three of which were fine. The **test binaries** had no backstop: `RunTarget`
    checks the timeout, the tag filter and the assertion count, and not one of those
    distinguishes an instrumented run from an uninstrumented one, so an
    uninstrumented `FastCacheTest` ran, reported assertions, exited 0, and the gate
    printed *all scoped targets clean*. A fix covering only the canary would have
    closed the issue with its stated harm untouched.
    **The obvious per-function symbols are the wrong ones.** `__tsan_func_entry`,
    `__tsan_read` and `__tsan_write` are emitted per instrumented *function*, so an
    object whose TU has no functions carries none -- six of `FastCacheTest`'s 135,
    the platform-gated `Iocp*`, `Kqueue*` and `Tls*` files that compile to nothing
    on Linux. They do carry `__tsan_init` and a `tsan.module_ctor`: instrumented,
    with nothing to instrument. A rule built on the hooks needs an empty-TU
    exemption; one built on `__tsan_init` needs none, and holds 185 of 185 objects
    across the three targets. That is the empty-translation-unit state showing up in
    an *instrument* -- naming it rather than collapsing it into pass or fail is the
    four-states rule again.
    **And the guard is exercised**: `ctest -R tsan-gate-selftest` drives the
    instrumentation verdicts against staged object trees -- a suite binary with one
    uninstrumented object first, since that is the half with no backstop -- through a
    stub `nm`, and since #488 drives the BOUND's three outcomes against staged
    executables, which need no object tree at all. Neither half needs a compiler, so
    both run in the default set. A guard that has never been
    seen to fire is what this whole file is about, and this one could not fire at
    all.
  - **A filter that matches nothing is a suite that tested nothing**, and every
    other signal in that run reads clean. The gate names it, and separately refuses
    an exit of 0 that reported no assertions. This is the same shape as a sweep that
    skips files and a `-header-filter` that matches no path: the tool ran, the
    artefact was fine, and *nothing was examined*.
  - **A gate that is red a few percent of the time is disarmed as thoroughly as one
    that was deleted**, and it happens through habit rather than code: people learn
    to re-run it. The TSan canary raced two threads on one `int` and was SILENT --
    the race happened, ThreadSanitizer said nothing -- in a few runs per thousand
    (#473). So the acceptance for a change to `src/tests/TsanCanary.cpp` is a RATE
    over at least a few hundred runs, recorded, and reproducible by whoever next
    doubts it: `scripts/tsan-canary-rate.sh`, which is the deliverable rather than a
    paragraph claiming it was checked. Measured at 5000 runs per arm, the old shape
    was 0.220% silent unpinned and **0.700% pinned to two CPUs** -- the wrong
    direction, since a two-core runner is the machine it has to be certain on.
    The fix is what the numbers said, not what the mechanism suggested. The obvious
    hypothesis -- shadow-cell eviction from hammering one address -- predicts that
    more accesses are worse, and it is refuted: 1 increment each is 2.50% silent, 10
    is 2.80%, 1000 is 0.20%, 100000 is 0.00%. More work on one address is SAFER, and
    a single unsynchronised pair, which is unambiguously a race and which TSan
    decides on happens-before rather than on overlap, is the least reliable shape of
    all. **No mechanism is claimed in the file, deliberately** -- a wrong explanation
    would send the next reader somewhere expensive. What the data shows is which
    dimensions move it: distinct LOCATIONS matter most (1024 touched once each is
    reliable where one touched 1024 times is not), repetition helps but needs orders
    of magnitude more, so the canary uses both with margin in each.
  - **And a figure is a quantity with an UNCERTAINTY.** This is the sibling of *a
    performance figure is a quantity under conditions*
    ([`compile-cache.md`](compile-cache.md)), and it was missing from it: a count
    with no interval attached reads like a measurement and is a hope.
    **Zero is where this is easiest to forget, because zero reads as the absence of
    doubt rather than as a value with an interval around it.** Zero silent runs in
    5000 is not a rate of zero -- the rule of three bounds it at ~0.06% at 95%, so a
    300-run zero-tolerance gate built on that figure expects 0.18 events and would
    fire about **one run in six**: red one time in six to prevent a canary that was
    silent one time in five hundred, which is #473's own failure mode one layer up
    and harder to argue with for looking principled. The converse omission is how
    two honest measurements nearly became one number -- 4/100 silent on one machine
    is P = 7.8e-5 against another's 0.220%, and 5.5e-3 against that machine's harder
    0.700%, so the samples disagree about the MACHINES rather than about the rate
    and both rows stand. Attach the interval, and say what it is an interval on --
    "P is small" without naming the hypothesis it is small under is the same
    omission wearing a number.
  - **A flag combination that cannot express the question still returns an answer.**
    The species `pkill -f`, `grep -c` read as a position and `grep -q` under
    `pipefail` all belong to: **the shell obliges regardless, and none of them
    errors.** The cleanest specimen is `grep -Lq`, where the contradiction is
    INTERNAL — `-L` lists files WITHOUT a match, `-q` suppresses output and exits at
    the first match, and the two cannot both be honoured. Instead of complaining it
    picks one, and it named two files as lacking a symbol they contain.

    Three things make that the worst of the family, and the second is the one to lead
    with.

    **It was a POSITIVE CONTROL** — the instrument added specifically to stop a wrong
    conclusion. A broken control is worse than no control, because its presence is
    what licenses the confidence. And it generalises past shells: **a control is the
    one instrument nobody checks, because checking it is what it was for.** Anybody
    instructing a team to add positive controls owes them that caveat.

    **Its error pointed at refuting a claim**, which is the direction that gets acted
    on — *because refuting feels like diligence*. It was one step from a correct rule
    being reported as overstated.

    **And nothing about the output looked wrong.** Two files is a plausible answer;
    unlike a wrapped message or an empty capture there is no tell. The only defence is
    re-deriving the control by a different construction — here an explicit loop, which
    gave **0 files lacking it** and turned "consistent with" into "confirmed".
  - **`producer | grep -q` is a false NEGATIVE under `set -o pipefail`, and it fails
    on the SUCCESS path.** `grep -q` exits the instant it matches, which closes the
    pipe; the producer is killed by SIGPIPE; `pipefail` then takes the producer's
    status and the pipeline reports failure. So `nm "$bin" | grep -q __tsan_init`
    says "no such symbol" precisely *because* the symbol was there. Capture the
    output into a variable and match afterwards
    (`syms="$(nm "$obj")"; [[ "$syms" == *__tsan_init* ]]`), or drop `-q`. This is
    not specific to `nm`: **every "does this artefact contain X" idiom in this tree
    is exposed to it** -- symbol checks, `strings | grep -q`, `objdump | grep -q`,
    any long producer feeding an early-exiting matcher. It bit `scripts/tsan-gate.sh`
    itself, which is the script written to catch exactly this class of thing, and
    that is the first time here that the *checking mechanism* produced the false
    reading rather than the thing being checked.
    **And it is worse than a false negative: it is RACY.** Measured while fixing
    #472, in the census scripts written to count instrumented objects. Whether
    `printf` finishes before `grep -q` closes the pipe is a scheduling accident, so
    two runs over the same 135 objects returned **129 and 108**, and a listing built
    the same way named **27** offenders against a true **6**. A deterministic wrong
    answer is caught by the first person who checks it twice; a racy one is
    attributed to the subject, which is how it survives. Two runs agreeing is
    therefore not evidence that a pipeline of this shape is sound.
  - **An edit script asserts its anchor MATCHED, and a generator that produced
    nothing fails rather than reporting success.** The same family as the two above,
    reached from the authoring side rather than the checking side, and it happened
    three times in one night here:

    - a `sed`/Python edit that added compiler defines matched no anchor, changed
      nothing, and printed its success message anyway — found only because the same
      compile failed identically twice;
    - a PowerShell test fixture whose writer process silently wrote no lines, so the
      case it was written to exercise never ran and the test passed;
    - and a step-level `if:` inserted above an existing one, silently discarded by
      YAML's last-key-wins.

    Every one of them *reported success for work it did not do*, which is the thing
    this whole file is about. So: `assert t.count(old) == 1` before every
    replacement — **count, not presence**, because `>= 1` hides an ambiguous anchor
    that then edits the wrong occurrence, and `== 1` catches "missing" and "not
    unique" in one line. Print what was changed (how many, and which) rather than
    that the script finished; a run that reports `patched 0 of 3` is a run whose
    author notices, and `done` is not. The assertion earns itself the first time an
    anchor drifts under a rebase, which is not a rare event here.

    **And `count == 1` is necessary, not sufficient.** It proves the anchor was found
    and was unique. It says nothing about whether the insert was *correct*, because
    what an insertion breaks is usually not the text it replaced -- it is the text on
    the other side of it. **An anchored insert is checked against what FOLLOWS the
    anchor, not only against what precedes it.**

    Measured here, in this file, by the commit that added *A probe's result is only
    evidence if the probe could have produced the other one*: the anchor was the last
    bullet of a section, matched once, and the insert landed between that bullet and
    that section's *closing* paragraph -- a paragraph reading
    "Every finding in **both sections** is an instance of this" and listing five
    findings. Pushed below the inserted section, "both sections above" now pointed at
    that one instead, which contains none of the five. Nothing was misplaced and no rule
    was wrong; **a neighbouring paragraph was made false**, which is worse, because it
    now misinstructs a reader who never sees the edit.

    And a third correction inside this one entry, which is the point rather than an
    embarrassment: the first two accounts of this said the paragraph was stranded "four
    sections downstream". The commit that did it -- *a probe's result is only evidence
    if the probe could have produced the other one*, cited by subject because a rebase
    invalidated the hash this sentence first carried, which is the "name, not direction"
    rule one line of scope up -- inserted **one** section, and there were two by the
    time it was noticed. Nobody had counted; four was repeated from the first telling
    into a message and then into this file. **A number in a post-mortem is a
    measurement and decays like one** -- the incident is over, so nothing will ever fail
    to make it wrong again, and a rulebook is exactly where such a number goes to be
    believed forever.

    **And its mirror image, which that rule does not cover: a VERSION in a bug report
    is a measurement of the ENVIRONMENT, and the environment can change under it
    without anybody editing the report.** In the post-mortem case the claim decays
    while the world holds still; here the world moves and the claim is left describing
    a machine that no longer exists. #715 was filed against `g++ 16.1.1 20260515`; the
    host updated to `16.2.1 20260819` mid-session -- confirmed with `rpm -q gcc-c++`
    rather than inferred from the discrepancy, since "I misread it earlier" and "the
    package changed" look identical at a glance. The version was the only thing in that
    report saying WHICH compiler it was about, so it was load-bearing and had gone
    stale silently. Re-measured rather than assumed: it reproduces identically, same two
    files, same two `-Werror` categories, which is the answer that makes an update
    reportable rather than alarming -- and had it NOT reproduced, the report would have
    been describing a fixed bug while reading as an open one. **A reproduction is a
    claim about a machine as well as about a tree.** Name the version, and re-take it
    when the tool under it moves.

    **A prose diff cannot show you the sentence your insertion falsified.** The hunk
    was `@@ -898,6 +898,45 @@`: the added lines plus **six** untouched context lines,
    three leading and three trailing. Only the trailing three matter to the argument,
    and they held the first three lines of the paragraph that went false while the
    broken sentence sat four lines further down -- outside the window. So the diff was
    clean, minimal and reviewable, and the defect was invisible in it. (An earlier draft
    said "three", counting one side of a symmetric default. The paragraph below about
    post-mortem numbers was already written when that was found.) Re-read the whole enclosing section afterwards, from its heading
    to the next one, rather than the diff. In prose the specific hazard is a
    **back-reference**: "the section above", "both sections above", "the two entries
    below", "as stated earlier". Those are the sentences an insert silently redirects, so
    grep the neighbourhood for positional words before inserting between two things that
    refer to each other.

    That grep caught this very paragraph one minute after it was written: the sentence
    above said "the commit that added the two sections above", and those sections are
    **below** it. Which is the real remedy rather than the grep -- **point at a NAME, not
    at a direction.** A title survives an insertion, a rebase and a move; "above" is a
    claim about layout that every later edit can silently invalidate, and it is the only
    kind of claim in a rulebook that nothing in the tree can check.
  - **A tool given a path that does not exist reports nothing, which is what "absent"
    also looks like.** `nm` on a missing file prints no symbols, so a grep for one
    counts zero -- character-for-character identical to a binary that was built
    without instrumentation, and fixed somewhere else entirely. Test existence first
    and say which of the two it was; a single message covering both sends the reader
    to the wrong place.
  - **A second copy of a list is not a cross-check; it is a second thing to be
    wrong.** The scope check shipped in this branch first kept its own copy of the
    gate's Catch2 tags and its header claimed the two "are checked against each
    other". Nothing checked them: delete `[task]` from the gate's `TARGETS` row and
    `Async/Task_test.cpp` still matches the surviving copy, still reports covered,
    and six coroutine cases leave the sanitized scope with every signal green --
    the branch's own bug, one level up, asserted to be impossible. It parses the
    expression out of `scripts/tsan-gate.sh` now, and a `TARGETS` table it cannot
    parse is a hard failure rather than an empty scope. Duplicate-and-verify is a
    real pattern here (`check-service-accounts.cmake` reads three files to prove
    they agree), but the verify half has to actually be written.
  - **A known race lives in `.tsan-suppressions` with its issue number, never in a
    deleted check.** Every entry is an open bug; removing it is part of closing that
    bug. Write it `race_top:`, never `race:` — `race:` matches a function name
    anywhere in *either* stack, so it also silences a future, unrelated race that
    merely passes through that frame, and a teardown path like `Close()` is
    traversed by a great many stacks. `race_top:` matches only the frame the racy
    access is in. Either way an entry outlives the report it was written for, which
    is why the gate sets `print_suppressions=1` on **every** run, canary included,
    and prints ThreadSanitizer's own `Matched N suppressions` line.

## A retry makes every one of these disappear without fixing it

Eleven separate ways the gate reported something that was not about the tree under
test have turned up across four tickets — six while fixing
[#493](https://github.com/LASTRADA-Software/fastcached/issues/493), two more while
fixing [#247](https://github.com/LASTRADA-Software/fastcached/issues/247), one while
fixing [#243](https://github.com/LASTRADA-Software/fastcached/issues/243), and two
while fixing [#292](https://github.com/LASTRADA-Software/fastcached/issues/292). Not
one of them announced itself. Every one presented as an ordinary flake, and **a re-run would
have cleared all of them without fixing any of them** — which is the whole reason they
are written down here rather than in those pull requests.

The specific traps are examples. The rule is the mechanism.

- **`local-gate.sh | tail`** reports the *pipeline's* last stage, so a run whose log
  ended in `GATE FAILED` came back as exit 0. Same shape as `producer | grep -q`
  under `pipefail`, above. Capture the status into a variable first; print the log
  after it.
- **Quoting through three parsers.** PowerShell → `wsl` → `bash -lc` silently dropped
  an escaped `$?`; the command collapsed, wrote no log, and exited 0. **A gate that
  did not run is not a gate that passed** — and an absent log is indistinguishable
  from a gate that produced nothing, unless you keep the log somewhere it survives.
  Author the commands in a script file and run the file.
- **Two gates in one build directory.** A run that had already *reported its verdict*
  was still driving `ninja` when the next one started. This is the worst of them,
  because the reading belongs to *neither* tree and neither process can detect it:
  from inside each one, everything looks entirely normal. `flock`, non-blocking —
  queued, the incident becomes invisible, which is the same defect as the failure it
  prevents.
- **A dirty or moving tree.** CI reads a commit; a local gate reads whatever is on
  disk as each phase touches it, so a long run can straddle an edit and different
  phases can judge different trees. An accurate verdict about something nobody asked
  about. Name the commit before starting, re-check it at the end, and refuse when the
  tree is dirty.
  - **An intermediate reading is BIASED, not noisy, and that is why the remedy cannot
    be "read it carefully".** A tree sampled mid-build can only be MISSING artefacts —
    never carrying extra ones — so its error is one-sided by construction: always
    toward failure, never toward success. Measured twice in one task: `ctest -L hygiene`
    at 235 of 429 build edges reported 2 failures where the settled tree reports
    **60/60**, and an earlier sample at 330 of 429 reported 1.
    The consequence is the part that changes behaviour. A mid-build failure is exactly
    what a real regression looks like, so the natural response — investigate it — is
    the wasted motion, and no amount of care in INTERPRETING the reading recovers
    anything, because the reading contains no information about the finished tree. So
    the discipline is **do not take the reading**: wait for the build's completion
    signal rather than sampling a tree that is still being written. Both hits above
    were caught only by re-measuring afterwards, and in both the alarming number came
    first.
- **The log under `/tmp`.** `wsl -e bash` detaches, so a build outlives the wrapper
  that reports on it — and the wrapper is what keeps the WSL session alive, so
  *killing it to tidy up* starts an idle countdown that takes the VM, the orphaned
  build and `/tmp` with it. Write the log to `/mnt/c`.
- **The instrument edited while it was measuring.** bash reads a script incrementally,
  from a byte offset, so editing the wrapper the gate was running re-ran its header
  block and shifted an `exec 9>` redirect into a different path — and the run
  continued **with no lock while reporting as though it held one**. A guard that
  silently stopped guarding.
  - **It reports in whichever direction the read pointer happens to land**, and the
    second direction is the one that gets believed. Editing a gate wrapper *to improve
    it* -- adding the commit stamp the entry below asks for -- while that wrapper was
    mid-run shifted every offset after the insertion, and bash resumed inside a later
    line: `syntax error near unexpected token '('` on a `(` that had been inside
    double quotes since the file was written. The gate itself had already **passed**
    both legs; the corruption landed in the *reporting* step, so the task reported
    `exit 2` for a green tree and never copied the log out. A false PASS hides a
    defect, a false FAIL invents one, and a false FAIL that a re-run clears is how a
    working gate gets a reputation for flakiness.
  - So: **launch long-running scripts from an immutable per-run copy**, and do not
    edit an instrument that is currently running -- *including to improve it*, which
    is the version that feels safe and is not.
- **A path mangled between shells, and the wrapper exiting `0` having run nothing.**
  `nohup wsl.exe -e bash /mnt/c/...` launched from Git Bash becomes
  `C:/Program Files/Git/mnt/c/...`: Git Bash rewrites a leading `/mnt` as though it
  were a POSIX path inside its own install prefix. `bash` then reports
  `No such file or directory` — on stderr, from the launched process, which the
  backgrounding discarded — and the launcher returns **success**. This is the purest
  specimen in the set: not a wrong answer, an *absent* one wearing a right answer's
  exit code. Run `wsl` from PowerShell, where the path survives, and check that the
  log the run was supposed to write exists before reading anything into its contents.
- **The wrapper dying on its own log while leaving the work alive.** The gate log
  lived on `/mnt/c`; DrvFs refused the redirect once — `No such file or directory` on
  a directory that existed and was writable seconds later — so the wrapper exited 1
  having run nothing **and left the gate child running**. That is worse than a clean
  failure: an orphaned run means the next reading is ambiguous between "nothing
  happened" and "something is still happening", and the tree it is building is no
  longer the one anybody is looking at. **An instrument must not depend on the
  filesystem it exists to measure** — write the log to ext4 and copy it to `/mnt/c`
  at the end, which keeps it surviving a WSL idle-out without putting DrvFs in the
  path of opening it.

- **The instrument edited the tree and then measured what it had edited.** Not a
  mix-up: no wrong path, no stale ref, no forgotten `cd`. `scripts/local-gate.sh` runs
  `clang-format -i` as its first leg, so a commit that was not already formatted is
  **rewritten**, and the build and test legs then judge the rewritten tree. The gate
  measured exactly the tree in front of it, and the tree in front of it was no longer
  the commit *because the gate had changed it*. An observer effect rather than a
  confusion — and every `--fix`-mode tool has the same shape.
  - **The guard failure is structural, not an omission.** A dirty-tree check that
    samples **once, at the start** cannot detect a change caused by the thing it is
    guarding: the tree was clean when it looked. That is why "check the tree is clean
    before you start" is insufficient on its own, and why the entry above says *name
    the commit before starting, re-check it at the end*. Sampling at both ends is the
    only shape that works.
  - **Implementing half of a two-clause rule looks exactly like compliance.** The
    wrapper that met this printed `N_GATE_COMMIT` and `N_GATE_DIRTY` before the run and
    never again — the first clause faithfully, the second not at all. Nothing in the
    output says a clause is missing, which makes this the most common way a two-part
    rule fails.
  - **"It's only formatting" is a claim, not a fact.** The delta was line wrapping and
    one include reorder, which is where almost anyone stops. An include reorder changes
    a translation unit's contents and can change what compiles. Amend the formatting
    into the commit and **re-run** the legs that measured the other tree, rather than
    reasoning about whether whitespace could matter.

- **A `ctest` total is only a total for the target set it was configured with.**
  `-DFASTCACHED_BUILD_TESTCLIENT` and `-DFASTCACHED_BUILD_BENCHMARKS` default OFF, so
  two runs of *one commit on one platform* report different totals, and **nothing in
  the output says which you asked for**. A gated-off target is invisible to the run
  rather than absent from it: the suite passes, the count looks like a count, and the
  tree simply appears smaller. This is the rule about a clang-tidy sweep being only as
  complete as its compile database's targets, arriving at a different instrument and a
  different artefact.
  - **So a total needs three conditions before it is comparable**, not two: the same
    tree, the same platform, **and the same target set**. Two were already written
    down; the third is the one that reads as a smaller codebase rather than as an
    error.
  - **Caught by arithmetic, and only because the number could not be lower.** A branch
    adding five cases reported 2982 where a sibling branch adding one reported 2992 on
    the same base — impossible in that direction. Reconciled, both fall out of one
    base once the conditions match: 2996 = 2991 + 5 against 2992 = 2991 + 1. A
    plausibly-*higher* wrong number would have passed unexamined, which is the honest
    boundary of this check rather than a reason not to make it.

- **An artefact that does not identify its subject cannot be told from a current
  one.** A WSL idle-out emptied `/tmp` mid-cycle, and the durable copy of the
  PREVIOUS run's log was left sitting exactly where a reader looks for the current
  one — reporting a failure that had already been fixed, in a file that had every
  appearance of being this run's. What distinguished them was a file timestamp that
  happened to predate the commit under test: an **accident**, not a property. Two
  runs landing in the same minute, or a clock that is off by a few, and there is
  nothing left to read at all.
  - **A verdict must carry what it is a verdict about.** The commit and the branch go
    *inside* the log, written before the gate is invoked — not only onto the
    wrapper's stdout, which is what dies with the terminal — for the same reason the
    gate prints its gitdir in its own header.
  - This is a **different mechanism** from the `/tmp` wipe it was discovered through,
    and the worse of the two. The wipe destroys the current answer, which is loud;
    this one leaves a stale answer looking current, which is read and acted on.

Two that are not about shells at all, and are the ones a reader is most likely to
recognise in themselves:

- **A finding fixed at the line rather than at the rule comes back.** clang-tidy
  reported `bugprone-unchecked-optional-access` at two lines; the rule is "clang-tidy
  cannot see that Catch2's `SKIP` does not return, so a bare dereference after one is
  always this error". Fixing the two lines left the identical defect in the half of
  the tree the report had not happened to name, and a refactor then moved the line
  numbers so it did not resurface for two more gate cycles. Read a finding as a
  statement about the tree, not as a list of locations.
- **Presence is not usability.** A probe asks whether the tool *did the job*, never
  whether it exists. A `clang-cl` with no MSVC SDK is on `PATH`, spawns perfectly and
  can build nothing; so does a driver whose toolset was uninstalled under it. The
  only question that survives is "did an object appear?" — which is the same lesson
  `dist-compile-e2e.ps1` records for `Get-Command`, and the same one behind
  `exitCode == NotSpawned` being a guard about *running*, not about *working*.

The asymmetry is what justifies the cost. **Looking costs minutes; retrying turns a
wrong reading into a permanent one**, because the evidence that something was off is
gone and the next run starts clean. Every one above was caught only
because something looked slightly wrong and got *looked at* rather than re-run — a
log sitting at zero bytes for slightly too long, an output file containing a single
backslash, an exit code disagreeing with its own log's last line, a finding
reappearing that had supposedly been closed.

## A claim about a tool is checked against the tool

The section above is about instruments reporting on the wrong tree. This one is about
the step before: **reasoning about what a tool does instead of reading what it does.**
Both were found in the same hour as the last two entries above, and they are the same
error one level up — a general claim standing in for a specific one.

- **A pattern is broader than its author reads it as.** `pgrep -f "scripts/local.gate"`
  was written to name one file and is a **regex**: the unescaped `.` matches the `-`
  in `local-gate.sh`, so it matched a family. Distinct from a pattern that is
  deliberately broad — this one *looks* exact. It also matched its own invoking
  shell, which is why it returned 143 and why the count it reported included itself.
  `pgrep -F` for a fixed string, or anchor it. And a cleanup that is about to send a
  signal identifies its targets by the **process tree**, walking to an ancestor that
  owns a directory this session owns — never by a command-line match, and not by the
  leaf's `cwd` either: a wrapper's `cwd` is where it was launched, and CMake's is a
  `TryCompile` scratch directory. Judging a process by an attribute inherited from its
  launcher is the adjacency-attribution rule with a third way to be adjacent.
- **A guard that has never been seen to refuse is not a guard.** A dependency clone
  wedged for 43 minutes in `read()` on a socket that had delivered zero bytes, because
  `http.lowSpeedLimit`/`http.lowSpeedTime` are unset and git therefore applies no
  timeout to a transfer that stops delivering. Setting them turns a permanent hang
  into a named failure after 60 seconds of silence — and the next run **succeeded**,
  which proves nothing about the bound: a cold `CPM_SOURCE_CACHE` still fetches once,
  so that populate crossed the network too and merely did not stall. The bound is
  **untested, not proven.** Reporting it as vindicated because the run it accompanied
  passed is the same defect as every entry above, and the same one
  `tsan-canary-rate.sh` exists to prevent.

- **The tree you measured is not necessarily the tree in question.** A grep for a
  constant found it at `main.cpp:135` with two uses, contradicting a report that it
  had been deleted — except the primary checkout was parked on a branch merged days
  earlier. Against `origin/master` the constant is gone. This is the worst-behaved
  member of the family: the others produce a missing log or an absent answer, and this
  one produces a **confident correction**, which propagates — it is about to be sent to
  somebody who will act on it. The tell is not in the output, because the output looks
  exactly like a finding. It is that **the answer was too convenient**: it made a
  teammate wrong and the reader right, with no effort. Check what you grepped —
  `git rev-parse HEAD` in the directory you searched — before reporting a contradiction
  as a fact. It happened twice in one night, both times in the same direction.

**The general form, and it is the one worth carrying away.** "A SIGTERM does not take
55 seconds to arrive" was offered as evidence in an incident review. It is true of
`local-gate.sh` — and *only* because that script traps `EXIT` and not `TERM`. bash
defers a **trapped** signal until the running foreground command returns, so a signal
trap on the same line would have made a TERM at second four produce a death at second
sixty-nine, and `143` is what bash reports either way. The general claim and the
specific one point opposite directions, and nothing about the general one advertises
that it does not apply. **Check the claim against the tool in front of you, not
against how tools of that kind behave.** Every finding in both sections is an instance
of this: a pipeline's exit code, a `.` in a pattern, a `cwd`, an unset git timeout, a
signal disposition — each one a fact somebody knew generally and did not verify
locally.

## A probe's result is only evidence if the probe could have produced the other one

The two sections above are about an instrument reporting on the wrong tree, and about
reasoning where reading was called for. This is the third: **a measurement that was
honestly taken, of the wrong thing, and read as an answer about the right one.** Both
halves below came out of one investigation (#726) in a single session, they point in
opposite directions, and neither was a wrong answer — each was a **non-answer wearing
an answer's colour**. One was a red that meant nothing; the other a green that meant
nothing.

- **A green probe of the wrong shape is not a refutation.** A review reported that
  under `Fsync` the commit after a one-slot meta recovery overwrites the surviving
  slot. Two probes were built and both came back clean, and "the finding is wrong" was
  written down before the third existed. Both seeded the store through raw `CowTree`,
  one flush per commit — which *keeps* the `txnId` slot parity, and the broken parity
  is the whole precondition. Only the `CowTreeStorage`-shaped seed reproduces it, where
  the FIRST `Open` commits the format marker -- `EnsureFormatVersion` stamps it on a
  brand-new store only, so the two sessions total 6 commits then 5 and the parity ends
  up broken. Written as "`Open` itself commits the marker" at first, which would make
  the total even and the fault unreachable. The finding
  was true. **Before recording a negative, state what would have had to be true for
  the positive to appear, and check the probe had it** — a fixture that cannot exhibit
  the fault cannot vote on it. Same defect as a suite exercising the fallback path
  while CI exercises the git path, and as a `want-fail` case satisfied by the shell
  refusing to start.
- **A single observation is a snapshot, not a trend, and cannot tell a transient from
  a fixed point.** The probe that finally did reproduce it committed **once**, so
  "after" meant "after one commit" — and that was published as "every subsequent
  commit hammers the same slot, so the store stays permanently one torn write from
  unopenable". Measured over five consecutive commits: `valid: 1` after the first,
  `valid: 2` from the second on, because the offending write restores the parity and
  the next commit repairs the damaged slot. A real defect, overstated into a different
  false claim, which then shipped in an operator page. **Iterate the operation, do not
  merely perform it.** The tell was available and unread: the same filing already noted
  the parity being restored, two sentences from the claim that contradicts it.

The pair is worth more than either alone, because the mistake is symmetric and the
instinct is not: a green probe invites you to close the question, and a red one invites
you to describe it — and the second is where a bounded fault becomes an unbounded
sentence. The storage-side instance is in
[`storage.md`](storage.md); the general form is here.

## A correction is a search, not a recollection

The section above is about acquiring evidence. This one is about **repairing a claim
after you have it**, which fails its own way: a wrong statement is rarely written down
once, and the copies you can list are not the copies that exist.

**A claim corrected in the places you remember writing it is not corrected.** One false
sentence about the `fsync` exposure above went into an issue body, a pull request that
another session then merged, and a rulebook paragraph. Two were fixed from memory; the
third survived, and survived a rebase, because it was never re-read — `grep -n "hammer"`
found it in four seconds after recollection had already failed twice. So the fix is a
search over the tree and the tracker, not a list of the places you think you touched.

**And a search is only as good as its pattern.** The follow-up search for surviving
copies came back clean, correctly — but the same session's check for the **corrected**
text also came back empty, which was impossible, since it had merged that text itself.
The content was there and the pattern could not reach it: the prose is line-wrapped and
carries markdown emphasis *inside* the phrase, so no phrase-grep matches it.

```
So on `fsync` the dangerous moment is the **first write after a degraded
start**, not the indefinite future.
```

That is the census rule from [`testing.md`](testing.md) — a zero that was expected, and
therefore never questioned — arriving in prose rather than in a test count. **Give a
search that returns nothing a positive control**: find one instance by other means and
check the pattern sees it. On wrapped markdown, match a single distinctive word, or
strip the wrapping first; never a phrase that spans a newline or a `**`.

## A comment can be true in its premise and false in its conclusion

`pr-labels.yml` carried this, and every word before the comma was correct:

> `sync-labels: true` ... only ever touches labels named in the config, so
> `type/`, `priority/` and `status/` are out of its reach by construction.

`.github/labeler.yml` genuinely contains no `type/` row, and the action's deletion
logic genuinely is config-scoped. The conclusion was still false, and it cost three
red `Require a type label` gates in one evening — presenting as three different
problems, which is why it survived being seen repeatedly.

- **The deletion is config-scoped; the WRITE is not.** `actions/labeler` seeds its
  set from the labels it read when the run started, then finishes with
  `setLabels(...)`, a full replacement of the pull request's labels —
  **unconditionally, whether or not `sync-labels` is set**. So any label added
  between that read and that write is destroyed regardless of prefix or config.
  **Turning `sync-labels` off would not have fixed it**, which is worth stating
  because that was the obvious fix and it was aimed at the wrong mechanism.
- **It is a lost update, so it is intermittent, so it reads as flakiness.** The
  action only writes `if (!isEqual(labelsToAdd, preexisting))`, so the window bites
  only on runs that actually had a label to add. Labelled while such a run is in
  flight: lost. Labelled when nothing is in flight: kept. Three pull requests in one
  evening showed label-stripped, label-kept, and a `CANCELLED` gate — one defect,
  three presentations, none of which looked like a labeller bug.
- **Read the timeline before theorising.** `gh api .../issues/<n>/timeline` names the
  actor and the second for every label event, and it settled in one query what two
  rounds of plausible hypotheses had not. Both of the hypotheses on offer — "the
  gate evaluated before the label landed" and "v5 removes unmatched labels" — were
  wrong, and each was confident enough to have been acted on.
- **Verify a third-party action against its source, not its README.** The behaviour
  that matters here is one `setLabels` call that neither the input's name nor its
  documentation suggests.
- **The repair is a compensating action and says so.** Nothing in a workflow can
  stop the replacement, so `pr-labels.yml` brackets the action: remember every
  label the config cannot produce, and restore what the write destroyed. It emits a
  `::warning::` when it fires, because a silent repair would leave nobody knowing
  how often the race occurs — which is exactly how the original survived.

## A `cmake -P` check cannot fail its own test

This section used to open by stating, as a measurement, that `message(FATAL_ERROR)`
in script mode prints `CMake Error ...` and then exits **0** on CMake 3.28 — this
project's declared minimum — so that every `cmake -P` hygiene check reported
PASSED however loudly it had just objected. **Thirteen registrations, and this
rule, rest on that sentence, and it does not reproduce** (#565).

- **What reproduces.** Exit **1**, on 3.22.6, 3.25.2, 3.27.9, 3.28.3, 3.31.6 and
  4.3.0, for six script shapes — bare, inside `if()`, inside `function()`, inside
  `foreach()`, inside `macro()`, and `SEND_ERROR` — each reading taken with
  controls proving the harness could report a zero. Two machines in the ticket
  read the same on Ubuntu's 3.28.3 and on Windows. The claim is now **asked**
  rather than remembered: `ctest -R fatal-error-exit`, in the default set, so
  every platform CI builds answers it on every run. A red one is a discovery —
  the condition has been found somewhere — not a defect in the check.
- **How a measured claim came out wrong, which is the transferable part.** A TRUE
  neighbouring fact carried one clause too far. A `-P` script genuinely cannot
  **choose** its exit code before CMake 3.29 — `cmake_language(EXIT)` is 3.29, and
  on 3.28.3 it is an unknown command, measured — which is why the SKIP direction
  correctly uses `SKIP_REGULAR_EXPRESSION` rather than `SKIP_RETURN_CODE 77`.
  *Cannot choose its exit code* then became *cannot signal failure by exit code*,
  and `message(FATAL_ERROR)` hands you exactly one code you did not choose, `1`,
  which is the only one a failure needs. Both reproducible ways to see a 0 are
  instrument shapes rather than CMake versions: an unguarded pipeline (`cmake -P …
  | tail` reports the pipe's status) and a nested `cmake -P` whose
  `RESULT_VARIABLE` is unread. The first is this file's own eleven-ways bullet;
  the second is now what `script-check-canary` is built out of.
- **A census over CORRECTED prose must CLASSIFY, and a census that only counts is
  wrong in the direction that looks like unfinished work.** Retiring the claim
  above left thirteen places quoting it in order to refute it, because this file's
  house style is to leave the wrong sentence visible — that is what lets a reader
  tell *corrected* from *never said*, and a silent swap would have made the
  retirement unauditable from outside. The cost is that **the pattern which finds
  the claim finds its refutation too.** Three readers grepped for it after the
  correction landed and each reported a live copy; all three had read a
  retraction, and the worst-looking hit —
  `src/tests/CMakeLists.txt:143`, which carries *"measured, not inferred"*, the
  phrase this repository uses to mark a claim as trustworthy — has that phrase
  **inside the quotation marks**, three lines above its own refutation.

  The method, because a figure without its pattern is the defect this section is
  already about. State the query, then classify each hit rather than totalling it:

  ```
  git grep -nE 'exits? \*?\*?0\*?\*? on (CMake )?3\.28|cannot fail by exit code|exits 0 on 3\.28'
  ```

  then, per hit, read ±6 lines for a retraction marker (*does not reproduce*,
  *paragraph said*, *stated ground*, *until #565*) and report ASSERTION or
  RETRACTION. Asked at `7952c61f`: **13 hits, 13 retractions, 0 assertions** — the
  claim held, which is an outcome worth recording precisely because it is the one
  nobody writes down. **14 on the tree that carries this bullet**, the extra being
  the query above matching itself: state which tree, as always, and note that a
  documented census counts its own documentation.

  **Zero is not a verdict, so the classifier gets a positive control.** A broken
  classifier and a clean tree produce the same output. Plant one bare assertion
  far from any marker — `scripts/check-test-names.cmake:4` was used — confirm the
  census reports `ASSERTION`, then restore. Without that step "0 assertions" is an
  absence of evidence rendered as evidence of absence, which is this rulebook's
  own four-states rule arriving in an audit.

  **And the classifier is a triage aid, not an oracle — measured, it is blind
  around this very paragraph.** The marker list two paragraphs up is prose in the
  same file, so it puts a false-RETRACTION zone ±6 lines around itself: a bare
  assertion planted three lines from that list was classified **RETRACTION**,
  which is the check being defeated by its own documentation. A second positive
  control catches it — plant the assertion *adjacent to the marker list* as well
  as far from it — and the standing rule is that **any hit inside the block
  documenting the census is read rather than classified.** The failure is
  contained (it can only ever excuse a hit, never invent one) but it fails in the
  direction that reports a clean tree, so it is stated rather than left for the
  next reader to rediscover.

  **And this is the companion to *a first failure masks its identical siblings*
  (#172), not a replacement for it.** Both are true and they fire in OPPOSITE
  directions: the sibling rule says a correction may have missed copies, so go
  looking — which is exactly what sent a lane looking here, correctly. This one
  says the looking will also turn up the correction itself. A count satisfies
  neither. **One correction does not find its own siblings, and one correction
  does not find its own FALSE POSITIVES either.**

  The reason this is a rule rather than a note about care: the three readers each
  knew the hazard, and one of them had written it down one paragraph above
  reporting the false positive. **A hazard that defeats people who know about it
  is a design problem**, and the fix that suggests itself — marking each quoted
  line so a line-oriented search cannot pick it up — was considered and REFUSED,
  because it optimises the file for `grep` at the cost of the reader, and the
  retraction being readable is the property that made the audit resolvable in the
  first place.
- **The verdict is still read from the output, for two reasons that are true and
  neither of which was the stated one.** `message(WARNING)` exits **0** on every
  version above while printing `CMake Warning`, so only an output verdict can hear
  a check that warns (#517) — which is why the pattern covers both words and why
  `script-check-warning-canary` is the half that proves the mechanism. And a check
  that shells out to another CMake without reading `RESULT_VARIABLE` exits 0 with
  its child's error on the output. `FAIL_REGULAR_EXPRESSION
  "${FASTCACHED_SCRIPT_CHECK_FAILED}"`, one spelling defined once in
  `src/tests/CMakeLists.txt`: a pattern restated per registration is one that
  drifts, and a drifted pattern stops matching without saying so.
- **A canary carried by its exit code is vacuous, and correcting the note is what
  exposed it.** A test fails when its exit code is non-zero **OR** the pattern
  matches, and `WILL_FAIL` inverts that whole verdict — so `script-check-canary`,
  a bare `message(FATAL_ERROR)` exiting 1, passed on its exit code alone and would
  have gone on reporting green with `FAIL_REGULAR_EXPRESSION` deleted from every
  registration in the tree. Measured under ctest, three cases: the old shape
  passes without the property, the new shape passes with it and **fails** without
  it. So the canary now exits **0** and prints a real `CMake Error` from a nested
  `cmake -P`, which leaves the pattern as the only thing that can fail it. That
  exit status is now load-bearing and therefore **asserted rather than
  remembered**: `check-script-check-signals` runs every `WILL_FAIL` registration's
  script and refuses a non-zero one, with a positive control refusing a scan that
  found no canary at all — otherwise "it exits 0" would be the next sentence
  somebody carries one clause too far. Fixing a wrong *reason* without re-deriving
  what it justified is how a guard survives as decoration.
- **A failure signal that is a property needs a canary and an omission check, and
  they answer different questions.** `script-check-canary` is a script whose only
  job is to fail, registered `WILL_FAIL TRUE`: it is green exactly while the
  mechanism works, and red the moment CTest stops honouring the property — naming
  the mechanism rather than whichever real check happened to have something to say
  that day. That covers nothing about whether a given registration *uses* the
  property, which is the far likelier failure and the one that reads as a working
  check; `script-check-signals` (`scripts/check-script-check-signals.cmake`)
  covers that, deriving the set of `cmake -P` registrations from
  `src/tests/CMakeLists.txt` rather than restating it.
- **A scan that attributes nothing reports nothing.** `check-script-check-signals`
  pairs each `-P` with the most recent `NAME` above it and **reports** a `-P` it
  could not attribute rather than skipping it: an unattributed registration means
  the scan lost track, and every verdict after that point was drawn from the wrong
  place. It also refuses to conclude when it matched no registrations at all.
- **Every `cmake -P` script declares `cmake_minimum_required(VERSION 3.28)`, because
  script mode declares no policies at all.** There is no project, so every policy starts
  UNSET and a policy-gated construct means whatever the CMake running it decides.
  Measured on 3.28.3 — the declared minimum and the version CI runs — against the same
  script with the declaration added:

  <!-- table-total: none -->
  | construct | policy | bare `cmake -P` on 3.28.3 | declared, or on 4.x |
  |---|---|---|---|
  | `if("b" IN_LIST haystack)` | CMP0057 | `CMake Error: Unknown arguments`, **exit 1** — errors before answering | answers |
  | `if(TRUE)` | CMP0012 | exit 0, `TRUE` read as a variable name, **branch NOT taken** | branch taken |

  **The half with no other remedy is the REVIEWER's machine.** CMake 4.x removed both
  policies, so the NEW behaviour is unconditional there and neither finding reproduces at
  all — a reviewer on 4.x reading one of these scripts is not testing what CI runs, and
  cannot falsify a claim about it. That is why the declaration is the fix and "reviewers
  should be more careful" is not: their tooling cannot see the problem. It happened both
  ways in one night (#497): a lane hit CMP0012 on 3.28 and the manager could not
  reproduce it on 4.3.1.
- **Under `ctest` these are LOUD; the silence is one level down.** Nothing in this tree
  passes `-Wno-dev` and `FASTCACHED_SCRIPT_CHECK_FAILED` is `"CMake Error|CMake Warning"`,
  so a registered check that trips a policy goes **red** on the 3.28 leg. What is silent
  is a **selftest's sub-run harness**: all six grep the captured output for `CMake Error`
  and none for `CMake Warning`, so a construct that merely warns and inverts is invisible
  there. That is where it actually bit — a #479 mutation warned, inverted, and scored as a
  successful red-proof of a case that had never been staged.
- **`script-check-signals` requires the declaration rather than enumerating the
  constructs.** The set of policy-gated constructs grows with every CMake release, so a
  guard that lists them is stale by construction. It walks the same registrations it
  already reads for `FAIL_REGULAR_EXPRESSION`, so the subject is derived and there is no
  second list. It lives there rather than in a check of its own because of the PARSER —
  a separate script would duplicate forty lines of NAME/`-P` pairing or restate the list.
  Not because "a new script would itself need the declaration", which sounds load-bearing
  and is false: it would be registered like every other check and covered by this pass. It matches `^cmake_minimum_required\(` anchored at line
  start: a word-match scored a file as compliant on the strength of a **comment saying it
  had none**, and returned 10 where the answer is 11.
- Do not "fix" this by raising the minimum to 3.29 and using
  `cmake_language(EXIT)`. The property works on every version, costs nothing, and
  is what makes the canary and the omission check possible; a version bump would
  buy an exit code and still leave nothing asserting that any given check can
  fail.

## The Windows Debug leg exists for the RUNTIME, and proves it is live

`build.yml`'s Windows matrix ran `cl-release` and `clangcl-release` only, so the
one Debug configuration on Windows was never built in CI — while being the default
preset every developer here debugs with. A `cl-debug`-only defect could therefore
live on master indefinitely with every status check green (#315).

- **The value is `_ITERATOR_DEBUG_LEVEL=2`, not the compiler.** MSVC's Debug CRT
  traps invalidated iterators, out-of-range indexing and mismatched container
  iterators *at the point they happen* — the class a Release build tolerates in
  silence and a sanitizer would otherwise have to find. So the leg runs `ctest`;
  a Debug leg that only compiles exercises none of what it was added for.
- **Nothing in this project states that level, which is why it is asserted rather
  than assumed.** It follows from `_DEBUG`, which follows from the runtime library
  flavour, which follows from `CMAKE_BUILD_TYPE` and `CMAKE_MSVC_RUNTIME_LIBRARY`.
  Any of those moving removes the checks with no warning, and the leg would still
  build, still run the suite, and still report green. `iterator-debug-canary` is a
  program that must die, and `scripts/iterator-debug-gate.ps1` refuses a build
  where it survives — the same answer as `tsan-canary`, deliberately the same
  shape rather than a second idiom.
- **A non-zero exit is not proof.** The gate requires the runtime's own
  `subscript out of range` diagnostic, exactly as the TSan gate requires
  `data race`: a canary that died of something else is a canary proving nothing.
- **The canary sets its own CRT report modes**, and that is load-bearing rather
  than defensive: an unhandled debug assertion pops a modal dialog, and on a
  runner that means the job hangs to its timeout instead of failing in a second.
- **A third leg rather than replacing a Release one, and that was measured.** No
  Windows leg is the longest job in any sampled pull-request run, so this leg costs
  runner-minutes and no wall clock, and giving up a Release configuration to buy
  that would trade coverage for capacity already there. The per-leg figures are
  *What CI costs* below and are not restated here — the last two written down in
  this spot went stale twice over, once when `SCCACHE_GHA_ENABLED` landed and again
  when the matrix grew a third leg, and both times they read as current fact. So
  does "`clang-tidy` finishes last", which has now gone stale a third time and in
  the other direction, #554 having diff-scoped the queue's sweep. Which job is
  longest is the last column of the table in *What CI costs*, per event; this
  paragraph deliberately no longer says what it holds.
- **Guarded to `MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug"`, which means its
  absence elsewhere is normal.** That is worth knowing before debugging it: a
  platform-guarded registration is indistinguishable from a lost one when you look
  at a single platform's `ctest -N`. Check a listing where it *should* be missing
  and one where it should not, and confirm a neighbouring test is still present.

## What CI costs

The workflow's *critical path* is the longest single job, and for a long time that
was one job: `clang-tidy`, at 28.6 minutes out of a 28.6-minute workflow (run
`33243524509`, master, green). The bullets whose figures name that run were
measured on it, and the numbers are kept because each one is the reason a knob is
where it is.

**A CI figure without its EVENT is not a figure.** This one workflow costs three
different things depending on what triggered it, and the longest job is a
different job in each — so "the critical path is N minutes" is answerable only
once you say which. Measured over 99 completed, green `build.yml` runs between
2026-09-01T09:28Z and 2026-09-02T19:03Z, on the three-leg Windows matrix, with
`SCCACHE_GHA_ENABLED` in force; runner-minutes sum the non-skipped jobs, run wall
clock is the last job's end minus the first job's start, and job durations come
from `repos/:owner/:repo/actions/runs/:id/jobs` rather than from impressions.
Some of the bullets below quote **step** timings, and those come from separate
samples taken on 2026-09-02 — how far they overlap these 99 runs was not
recorded, so do not reconcile a step mean against a job range below to the tenth.
**Every one of these figures is the state BEFORE the merge-queue sweep was
diff-scoped**, which is the bullet three below and is what makes them worth
re-measuring rather than citing forward:

<!-- table-total: none -->
| event | runs | runner-min, mean | longest job, mean | run wall, mean | which job is longest |
|---|--:|--:|--:|--:|---|
| `pull_request`, code-touching | 36 | 102.7 | 19.0 | 24.1 | `clang-tidy` (18 of 36) |
| `pull_request`, docs-only | 7 | 0.6 | 0.1 | 1.1 | — (7 jobs, none over 10 s) |
| `merge_group` | 33 | 113.6 | 23.5 | 31.7 | `clang-tidy`, full sweep (32 of 33) |
| `push` on master | 23 | 109.8 | 22.7 | 27.0 | `clang-tidy`, full sweep (23 of 23) |

**Runner-minutes and wall clock are different questions and the answer differs by
which you asked.** Landing one code change costs **326.1 runner-minutes** — the
`pull_request` code-touching row plus `merge_group` plus `push`, named rather than
counted off by position, since the docs-only row sits between them and a reader
following "the top two rows" arrives at 24.1 + 1.1. Only the first two of those
three are on the path from "ready" to "merged", because the master `push` fires
AFTER the merge. So the CI wall clock a contributor waits through is
**24.1 (`pull_request`) + 31.7 (`merge_group`) = 55.8 minutes**, and the push's
109.8 runner-minutes buy **zero** merge latency. A lever that removes cheap
parallel jobs buys minutes and no latency; one that shortens the longest job buys
both. Say which you are selling.

The other thing the older figures did not carry: **on a pull request the sweep is
bimodal, not average.** Over 19 pull-request sweeps whose per-STEP timings were
pulled — a subset of the 36 runs above, because the step sample was collected
separately and is not the row's denominator — 11 were diff-scoped (0.0–8.4 min) and
8 escalated to a full sweep (21.8–25.5). That 8-of-19 escalation rate and the row's
`clang-tidy`-is-longest count of 18-of-36 are two different questions over two
different samples and do not have to agree; neither is the other's check. The mean
of 12.8 describes no run in either, and any prediction from it is a prediction about
a run that does not happen — including the projection three bullets below, which
makes it anyway and says so there.

- **The `push` on master is LOAD-BEARING, and it is the only full `clang-tidy`
  sweep left.** It reads like the workflow's most obviously redundant run and was
  documented that way until #554; the bullet below is that case, kept because the
  measurements in it are still true and still the reason the trigger is shaped as it
  is. What changed is not a number: the merge queue now diff-scopes its sweep
  against its own `base_sha`, and *what makes that sound* is that master@N was
  proved fully clean by the push that landed it. Narrowing `on: push` to tags would
  therefore now remove the analysis every other run's scoping rests on, on top of
  freezing the cache scope only a push writes. It has TWO unique products, not one.

- **The `push` on master otherwise rebuilds a tree the merge queue has already
  proved, and its other unique product is a handful of cache entries.** A
  `merge_group` run and the
  `push` on master that follows it carry the *same* `head_sha` — 6 of 6 pairs
  checked on 2026-09-02 — so the master push recompiles, retests and repackages a
  tree the queue has already proved green. That is a whole bottom row of the table
  above, per merge. Measured on the docs-only #530: **0.5** runner-minutes at
  pull-request time, which is the scoping working, then **108.9 + 95.0** to land
  it.

  Per *merge* that is a doubling, and only at a queue **batch of one** — which is
  what this repository has had, and is the condition on those six pairs. GitHub
  batches queue entries, and a batch of N dispatches N `merge_group` events and
  lands as ONE push, so the ratio is `(N + 1) / N` and the per-merge price falls as
  batches grow. The duplicated *run* does not: there is one redundant full matrix
  per batch whatever N is. `build.yml`'s `changes` job already records the batching
  from the other side.

  It is not pure waste, and the difference is measurable rather than argued.
  *Measured*: on the same `head_sha` the queue run had just compiled, each Windows
  leg of the master push still took **6–7 sccache misses out of 668 requests** —
  it re-stored objects the queue run had already built and cached. *Inferred*, from
  GitHub's documented cache scoping rather than from an experiment here: a run
  restores from its own ref and from the default branch, and writes only to its own
  ref, so the queue's `gh-readonly-queue/master/…` entries are unreachable by
  anything and the master push is what publishes those objects for every later pull
  request and queue entry to restore. Narrowing the push trigger to tags — NOT
  deleting `on: push`, whose `tags:` row is the only way the `release` job is ever
  reached — would therefore trade a bottom-row run per merge for a cache that goes
  cold at whatever rate master changes: a real trade, not an obvious one. **A third
  measurement, 2026-09-02**: `refs/heads/master` holds **2521 cache entries
  totalling 7.9 GB**, and no other ref can be written to it — the inference above
  confirmed from the `actions/caches` listing rather than from the documentation. It
  says how much would go cold and not how many minutes that costs, so the *size* of
  the second half is still unpriced; what settles the trade today is the bullet
  above it rather than this number.

- **A pull request and a merge-queue entry diff-scope the `clang-tidy` sweep;
  every other event sweeps everything, which today means the master `push`.** The
  step was `--all` for every event but a pull request, so the workflow's critical
  path was the same full sweep three times per landed change. Measured on the 99-run
  sample above: the `Sweep` step costs **22.50 min** in the queue against **12.80**
  on a pull request, in a job whose remaining steps — checkout, apt, configure, the
  scope self-test — come to about **a minute**. So the sweep *is* the job, and the
  queue was paying for the same 497 translation units the push was about to walk
  again. That last figure is a difference between a job mean and a step mean drawn
  from two samples, which the header of this section says not to reconcile to the
  tenth; it is quoted to an order of magnitude for that reason and nothing rests on
  its precision.

  Applying the pull request's scoping to the queue takes it from 113.6 to **104.0**
  runner-minutes, its longest job from 23.5 to **15.0**, and its wall clock from
  31.7 to **24.5** — which is **55.8 → 48.6 minutes of merge-path wall clock**, the
  only lever anyone has found that moves that number by more than a couple of
  minutes. Per change, 326.1 → 316.5 runner-minutes.

  **Those four numbers are the prediction this section forbids, made deliberately
  and with its bias named.** They substitute the pull request's 12.80 sweep mean
  into the queue, and that mean describes no run: the distribution is bimodal, so
  the queue after this change is a mixture of ~1-minute entries and ~23-minute ones,
  not a population of 15-minute ones. The bias has a direction, and it is
  optimistic — a queue entry can batch several pull requests and escalates if *any*
  member touches a `SweepEverythingWhen` path, so its escalation rate is at least the
  pull request's 8-of-19 and 12.80 is an under-estimate for it. It is used because
  nothing better exists before the change lands. **Re-measure after merging**; until
  then read these as a direction with a floor, not as figures.

  **What it stops checking, and where that is still caught.** In the queue: findings
  in translation units the change cannot reach. Still caught by the master push's
  `--all`, and the union is complete — master@N proved fully clean, plus a scope
  that over-approximates everything a change can reach, covers master@N+1.

  **Both halves of that are wiring, and wiring is asserted** (`ctest -R
  tidy-sweep-scope`). Take the `--all` off the push and every run in this repository
  stays green while the analysis is permanently narrower than this bullet claims:
  no failing check, no slower job, no log line, just a confident count over fewer
  translation units — the failure `tidy-sweep.sh` exists to prevent one level down,
  reached one level up. Delete the report step and the full sweep still runs, still
  fails and still reaches nobody. So the check reads the workflow for four things:
  that the sweep step **consults the mapping** (`--ci`) and restates none of it;
  that the queue entry's own `base_sha` reaches that mapping **under the
  environment name `tidy-sweep.sh` reads it from**, which the check takes from that
  script rather than restating — matching only the expression cannot see a rename,
  and a rename is the one edit that breaks the wiring while leaving the expression
  in place, silently, in the direction where the queue diffs against master; that
  some step reads
  the sweep STEP's conclusion for `refs/heads/master`; and that the job grants the
  `issues: write` without which that reader is decorative.

  **The mapping itself is one table**, `CiScopeTable` in `scripts/tidy-sweep.sh`,
  driven by that script's own `--self-test` (#570). It was two GitHub expressions —
  a `BASE:` env expression and an `--all` argument expression — composed with the
  shell inside the script, so "what happens on event X" had to be read out of two
  languages, and nothing stated it. The reader's `if:` is a third per-event
  expression in that workflow and deliberately did **not** move: it decides who is
  TOLD about a failure rather than how wide the analysis is, and rule C still
  checks it where it stands. The
  workflow now hands over the three FACTS its context holds
  (`GITHUB_EVENT_NAME`, `GITHUB_BASE_REF`, `MERGE_GROUP_BASE_SHA`) and decides
  nothing, which is why the check's first rule is now *do not decide here* rather
  than *spell the guard this way*.

  That respelling is not a tidy-up, and what it preserves is worth keeping written
  down. The old guard had to EXCLUDE the two diff-scoped events rather than NAME
  the event that gets the full sweep: `event_name == 'push'` and `event_name !=
  'pull_request' && event_name != 'merge_group'` select the same events today and
  fall opposite ways tomorrow — a trigger added to `on:` later diff-scopes under
  the first and sweeps everything under the second, and on master a diff-scoped run
  has an EMPTY diff, so `tidy-sweep.sh` prints `no source changed` and exits 0
  having analysed nothing. Narrowing silently is the failure; re-widening is merely
  slow. A table with a **default row** is that property made structural: the events
  that diff-scope are listed, everything else falls to `all`, and there is no
  spelling left to get backwards. The paragraph this replaces said the reverse of
  the old rule until a review caught it, which is worth leaving recorded, because
  an agent reading this file before touching the area would have rewritten the
  guard on its authority and been failed by the check.

  Each rule is driven red on a GENERATED workflow by `--self-test`, in both
  directions, because a guard nobody has seen bite has told you nothing. Generated
  rather than sed-edited: the first shape edited a correct fragment with
  `/pat/,+2d`, a GNU extension BSD sed rejects, and checked no exit status — so on
  macOS the edit failed, the fragment came out empty, the check failed because
  there was no workflow at all, and the case printed `ok` for a break it had never
  staged. Every case now also asserts its fragment DIFFERS from the correct one, so
  a knob that does nothing fails instead of passing. Two cases run the other way and
  assert a pure REFLOW is not read as a deletion — the `run:` block collapsed to one
  line, the reader's `if:` wrapped as a folded scalar — because a red build accusing
  an author of deleting the sweep they only reformatted is worse than no check.

  **The residual is narrower than it first reads, and the narrowing is the part that
  matters.** A clang-tidy finding depends on a translation unit's own sources and its
  include closure, both of which the scope covers; `SweepEverythingWhen` covers the
  changes that alter how every unit is read. So a *code change* plants a finding
  outside its own scope only where the scope under-approximates, and `tidy-sweep.sh`
  names its one such direction in its own header: an include spelling that resolves
  to no first-party file is taken for a system header and dropped. That is a real
  gap and it is not new — the diff-scoped pull-request sweep has always had it — but
  it is why this reads "only where the include graph is complete" rather than "never".

  What is left beyond that is the analyser drifting under us — apt.llvm.org publishing
  a new `clang-tidy-22`. The diff-scoped PULL REQUEST sweep catches such drift only
  where it lands inside the scope it already sweeps, which is a fraction of it and was
  a fraction of it before this change too; a deliberate version bump escalates anyway,
  because `CLANG_TOOLS_VERSION` lives in `build.yml`, itself a `SweepEverythingWhen`
  row. Drift therefore surfaces at the master push, and failing that at the next
  pull request whose change escalates — 8 of 19 sampled, so hours rather than weeks.
  It is a check that MOVED, not one that was lost.

  **Evidence the escalation was not paying, honestly bounded.** Four `merge_group`
  `clang-tidy` failures in the window (`1312bc6c`, `45760033`, `d04bb8ab`,
  `f77f4fbf`). All four are one root cause — a batch that was pairwise clean and
  conflicted serially, `FrameEndpoint_test.cpp: no viable conversion from
  'chrono::seconds' to 'DialOptions'` — and at each of them **13 other jobs failed
  too**, every Linux, Windows and macOS build among them. So in this sample the full
  sweep caught nothing the ordinary builds did not. Four failures with one cause is
  not a proof that it never pays, and it is not offered as one.

- **A master push blocks nothing and reaches nobody, so moving a check onto it means
  putting a reader on it.** Checked three ways on 2026-09-02, all negative: every one
  of the last 30 master-push runs was triggered by `github-merge-queue[bot]`, so
  GitHub's failure mail goes to an app account with no mailbox; nothing under
  `.github/` sends a notification of any kind; and **all 18 failed master-push runs
  back to 2026-06-10 sit at `run_attempt: 1`**, never once re-run. That last one is
  not proof nobody read them, and it is 18 for 18 of nobody acting.

  **And the remedy does not get to skip that test.** An issue opened by
  `github-actions[bot]` reaches a person only if somebody queries the label —
  GitHub's default "Participating and @mentions" does not notify on a bot-opened
  issue — so the reader this bullet installs is *asserted*, on the same evidence
  standard the bullet just refused to accept: none. It is a strictly better place
  for the signal than a run log nobody opens, because `status/needs-triage` is a
  query this repository already runs, and it is not a measured improvement. If the
  issue sits untouched, that is the same finding one level up and the answer is a
  louder channel, not a second unmeasured one.

  So the sweep's own failure opens or updates one issue — and the shape of that is
  the whole point. It keys on `steps.sweep.conclusion` **in addition to** the
  `failure()` status function, never on the job's result alone; `failure()` is not
  redundant and removing it gives a step that never runs at all, because GitHub
  skips every step after a failed one unless a status function says otherwise. **Over those same 18 failed master-push runs** — one window, one
  denominator — a notifier on any failing job fires **18 times**, and `clang-tidy`
  was the failing job in exactly **one** of them. The margin is wider than that one
  suggests, and the direction is worth having exactly rather than roughly: in that
  single run the failing STEP was `Test`, in the job's pre-split shape, so the
  step-keyed condition that ships here would have fired **zero** times across the
  whole window. 18 against 0, not 18 against 1. That is the whole comparison; a rate
  over "push runs" would need a third window and is not what decides it, and what
  those 18 failures were *about* was not classified run by run — the count is what
  the argument rests on, not a characterisation of the causes. The test a new alarm
  has to pass here is whether a firing means something happened, which is the test
  that withdrew a counter in #447.

  Zero firings is also the weaker half of the evidence and is not offered as more:
  it says the alarm is quiet, not that it is sensitive. What makes it sensitive is
  that the condition was watched firing on a forced failure (PR #561, run
  `33677107395`) with a negative control keyed on a passing step skipped beside it.

  One OPEN issue, updated by comment — the lookup is `--state open`, so a report
  somebody closed is not found and the next failure opens a fresh one. That is
  wanted (a closed report means it was dealt with) and is not what "reopen" would
  promise, so the word is avoided: commenting on a closed issue does not reopen it.
  Keyed on the **title**, read out of a `--label`
  listing rather than a `--search`: search goes through GitHub's issue index, which
  lags creation, so two master pushes failing inside that window would each find
  nothing and each open one. The labels are `CONTRIBUTING.md`'s own — a private
  `ci/…` key would be a label every documented filter is blind to — and
  `status/needs-triage` is applied by the step rather than left to
  `issue-triage.yml`, which never sees this issue: an issue opened with
  `GITHUB_TOKEN` triggers no workflow, so the repository's one reader-attachment
  mechanism does not fire and the report would land outside every triage query.

  It is a STEP inside the tidy job rather than a job of its own, and that is forced:
  `check-release-gate` asserts every job in the file appears in `release.needs`, and
  a job that is skipped on a tag push — which any push-failure notifier is, always —
  skips `release` with it. That is this file's never-arrives failure aimed at the
  release instead of at a required context.

- **A ccache hit does not skip clang-tidy, so caching harder was never the fix.**
  That job reported **535 hits out of 592 cacheable calls (90.4%)** and its build
  step still took **23.8 minutes**. CMake wires the analyser in through
  `cmake -E __run_co_compile`, which runs the compiler behind its launcher and the
  analyser as two independent commands: a replayed object buys the compile back and
  not one second of the analysis. The only way to pay less clang-tidy is to run it
  over fewer translation units, which is why the job stopped building at all and
  became `scripts/tidy-sweep.sh` over a compile database.

- **The sweep's scope is the diff plus everything the diff can break, and every
  way it can be wrong errs towards sweeping more.** A changed header is not a
  translation unit: tidying only the changed `.cpp` files would let one edit to
  `Logger.hpp` land findings in seventy files nobody checked, and the sweep would
  print a confident count while doing it. So the scope is the changed `.cpp` files
  **union** every `.cpp` that transitively includes a changed header — resolved by
  longest path suffix, so an ambiguous spelling reaches every candidate and an
  unresolvable one is dropped as a system header. An over-approximation costs
  minutes; an under-approximation costs a red master for code a pull request was
  told was clean. A base ref that does not resolve escalates to the full sweep for
  the same reason: "we could not tell what changed" must never read as "nothing
  did". Measured on this tree: a leaf `.cpp` sweeps 1 unit, a mid-level header 23,
  `Core/Logger.hpp` 71, and a `.cmake` all 329.

- **A change that decides how EVERY translation unit is read escalates to the full
  sweep, and that is a table** (`SweepEverythingWhen`): `.clang-tidy`, any
  `CMakeLists.txt`, any `.cmake`, `CMakePresets.json`, `vcpkg.json`, `*.hpp.in`
  (it generates a header the include graph cannot see), the sweep script and the
  workflow. A README typo is deliberately not on it.

- **The unit of work is a compile command, not a file, and that is what makes the
  swap lossless.** `Stats.cpp` builds into `fastcache-cc`, `fastcache-cc-tests`
  and `fastcache-compile-node`, which do not agree about `FC_COMPRESSION_ENABLED`.
  `clang-tidy -p <dir> <file>` does **not** take the first matching entry:
  libTooling's `ClangTool::run` loops over *every* command the database returns for
  that file. So a file with one command needs nothing special, and a file with
  several is served **only** by one single-entry database per command — handing it
  to the shared database *as well* analyses each of its commands twice.
  Measured against a CI-shaped database: 594 entries, 445 of them first-party, and
  329 distinct files — so a per-file sweep would have stopped checking a quarter of
  what the build checked. 445 is also exactly what the old job tidied
  (`include(ClangTidy)` runs *after* the `CPMAddPackage` calls, so a fetched
  dependency's targets never carried `CMAKE_CXX_CLANG_TIDY`), which is the sense in
  which this change costs no coverage. Measured scopes on that database: a leaf
  `.cpp` 1 unit, `Distributed/SchedulerService.hpp` 29, `Core/Logger.hpp` 83,
  `Stats.cpp` 4 (one file, four commands), a `.cmake` all 445, a README nothing.

- **The translation units come from the compile database, never from
  `git ls-files`.** `IocpReactor.cpp` is a translation unit on Windows and a file
  on Linux, so a full sweep taken from the index would fail on sources no target
  here builds — and a diff-scoped one would report findings for a compile command
  that does not exist.

- **But *first-party* means git tracks the file, and that is a definition rather
  than a list of directories.** A fetched dependency's sources are compile-database
  entries like any other — Catch2, yaml-cpp and lz4 are ~150 of the 594 entries here
  — and **where they land is configuration**: `_deps/` under the build tree by
  default, `$CPM_SOURCE_CACHE` anywhere, which in CI is `.cache/CPM` *inside* the
  workspace. An exclusion written as a path (`/_deps/`) was measured to let all
  ~150 of them through under CI's own layout, which turns a full sweep into a
  failure inside lz4's `xxhash.h`. Intersecting with
  `git ls-files --cached --others --exclude-standard` gets it right wherever the
  cache is put, and subsumes the absolute-path and out-of-tree cases for free.
  `--others` is in there deliberately: a source created and not yet added is
  exactly the code nothing has ever checked.

- **A database generated for clang-tidy needs `CMAKE_CXX_SCAN_FOR_MODULES=OFF`
  named explicitly, even though `CompileCache.cmake` sets it.** That module turns
  the scan off only when it *picks a launcher*, and the tidy job installs no ccache
  for it to pick. Without the flag every compile command carries `@…modmap`
  arguments that do not exist until the target is built, every translation unit
  fails to parse, and the sweep reports clean having checked nothing — which is the
  exact failure `scripts/tidy-sweep.sh` canaries against, so it is caught rather
  than believed.

- **And it must be configured with the same *target set* CI builds.** Its sibling
  above is about a database whose commands do not work; this is about a database
  that is missing commands, and it fails in the opposite direction — quietly, with a
  number attached. A sweep whose scope is derived from a database is only as complete
  as that database's target set, and **a target gated off by default is invisible to
  the sweep rather than absent from CI**: the option table's default is what the
  local `cmake` line inherits, and the workflow's own `-D…=ON` is what CI builds.
  Those two disagreeing is not a misconfiguration anybody would notice, because both
  sides are behaving as written. Measured on a review sweep here: a database
  configured with the defaults produced **15** translation units for a diff that CI
  tidies as **20**. The five were the launcher-facing sources reachable only through
  the two default-OFF app targets, and every one of them was inside the change. The
  sweep does not error on them — they simply are not in the file list, and it prints
  a confident count of what it did check.

  And **the script cannot catch this for you**, which is why it is a rule and not a
  guard. `tidy-sweep.sh` drops a changed file with no compile command silently, and
  it is *right* to: the bullet above requires exactly that, because `IocpReactor.cpp`
  in a diff on Linux is a file this platform does not compile and must not fail the
  sweep. Absent-because-this-platform and absent-because-that-target-is-off are
  indistinguishable from the file list alone. Only a full plan of zero units is
  fatal, and under-covering by five of twenty is not that.

  So the check belongs to whoever configures the database, before running it:
  compare the units the sweep reports against `git diff --name-only origin/master...HEAD`
  and account for **every** file it did not reach. The answer is either "this
  platform does not compile it" or "I configured the wrong target set" — and when
  adding an app target the question is never "is it built by default" but "does any
  CI job build it", because if yes its flag belongs in whatever line generates a
  database.

  **A file the sweep DID reach can still be uncovered, and that half is now the
  script's** (#466). A translation unit whose file is guarded out to nothing has a
  compile command, so it never meets the drop rule above — it is planned, analysed,
  reported clean, and the clean report is the defect: `TlsSocket_test.cpp` carried a
  syntax error while the database listed it, clang-tidy passed it, both compilers
  built it and ctest ran a binary holding none of its cases. Five signals agreed
  about nothing. So the sweep preprocesses each unit once more and reports
  **contributed / produced no code in this configuration / could not be
  preprocessed**, per FILE, since a file has several commands and they need not
  agree. There are now **two** lists to reconcile against the diff, not one, and the
  headline counts files rather than commands. An empty unit is a guard working and
  is never a fault — the leg where that guard is ACTIVE is what covers the file, so
  the reconciliation is against the matrix, not against this run.

  What the script still cannot answer is whether any CI leg compiles it with the
  guard active. That is a different instrument over the matrix, and it is the half
  with teeth: this one reports, and a green job's log is not read.

- **The sanitizer test run moved to `clang-asan-ubsan`; it did not go.** The
  `clang-debug` preset is the only configuration in the workflow with ASan and
  UBSan on, so that job's `ctest` is the project's entire sanitizer coverage in CI.
  It keeps the `clang-debug` preset with `-DENABLE_TIDY=OFF` rather than switching
  to the identically named `clang-asan-ubsan` preset, which does not set
  `PEDANTIC_COMPILER_WERROR` — a tidier preset name is not worth the sanitizer
  job's warnings-as-errors.

- **A ccache cap is judged by `Cleanups`, not by the hit rate.** The Debug + ASan +
  UBSan job finished at **99.76% of a 256M cache having run 44 cleanups** — it was
  evicting its own objects inside a single build, while reporting a 90% hit rate
  that looked healthy. It is 1G now. The Release jobs finished at 69.7% full with
  zero cleanups and are deliberately left at 256M: the whole repository shares a
  10 GB Actions cache budget, and raising a cap that is not full spends it on
  nothing.

- **`sccache` on the Windows jobs was running, and caching into a directory that
  is deleted with the runner.** Its statistics said `Cache location  Local disk:
  "C:\Users\runneradmin\AppData\Local\Mozilla\sccache\cache"` — so nothing it
  stored ever survived the job that stored it, and the two Windows build jobs paid
  a full cold compile every run: 17.2 and 11.8 minutes of build, the second-biggest
  block in the workflow. `mozilla-actions/sccache-action` does **not** set
  `SCCACHE_GHA_ENABLED` for you — its README tells you to — and without it sccache
  never looks at the Actions cache at all. The action must also be **v0.0.11 or
  newer**: GitHub's cache service v1 is gone, and only the newer action exports the
  `ACTIONS_RESULTS_URL` / `ACTIONS_RUNTIME_TOKEN` pair the backend reads.

- **Read those statistics before `ctest`, or they are somebody else's.** The
  action's own post step reported `Compile requests 0` for both Windows build
  jobs, which reads as "the launcher was never wired in" and is not what happened:
  sccache keeps its counters **in the server**, and this suite's
  `sccache-smoke-*` tests restart that server with `SCCACHE_MEMCACHED` pointed at
  a fastcached daemon — zeroing them, minutes before the post step reads them.

  That is the shared-cache configuration `docs/snippets/sccache-backend-caveat.md`
  exists for, and naming it here without saying so is what
  `ctest -R sccache-backend-caveat` refuses: under **MSVC** and **clang-cl**
  sccache hashes `/EP` output, which carries no paths, and replays a hit's
  `/showIncludes` with the absolute paths of the checkout that stored it, so two
  checkouts record each other's headers; `fastcache-cc` is the remedy, because it
  rewrites a hit's paths into the consuming checkout and refuses a hit whose
  replayed dependency is not there. Those smoke jobs compile with `g++-14`, where
  the hazard does not arise at all — which is the only reason a test in this suite
  may point sccache at a daemon.

  The
  job that does not run `ctest`, `compile-cache E2E (Windows)`, reported **231
  compile requests, 2 hits, 229 misses** from the same configuration, which is both
  the proof that the launcher works and the proof that the disk cache is cold every
  run. So the assertion that sccache handled the build runs **immediately after the
  build step and before the test step**, and a number read anywhere else in a job
  that runs the suite means nothing.

- **The Windows jobs cache now, and the cache is no longer the Windows question.**
  With `SCCACHE_GHA_ENABLED` the statistics read `ghac` rather than `Local disk`.
  Measured over 10 runs × 3 legs on 2026-09-02, every leg issues **668 or 669
  compile requests** and hits **537–663** of them, 80.4% to 99.3% — and the spread
  is *how many translation units the change touched*, not which event it was: the
  leg that missed 6 of 668 hit 99.1%, the one that missed 131 hit 80.4%, and both
  shapes occur on pull requests and on master pushes alike. What is left is not
  cacheable at all:

  <!-- table-total: none -->
  | leg | `Build`, mean | `Test`, mean | whole job, mean (min–max) |
  |---|--:|--:|--:|
  | `Windows-cl-release` | 4.7 | 3.5 | 8.5 (6.0–12.1) |
  | `Windows-clangcl-release` | 3.1 | 4.0 | 7.8 (5.6–11.3) |
  | `Windows-cl-debug` | 3.5 | 4.5 | 9.1 (6.2–14.8) |

  Roughly half of each Windows job is `ctest`, which no compiler cache touches, so
  the next Windows minute has to be bought out of the test run or not at all. The
  ticket that asked whether these jobs cache is answered; a ticket that asks how to
  make them faster is a different question with a different subject.

- **Sharing sccache entries across Windows CI runs is safe, and the reason is not
  "it seems fine".** The `/showIncludes` hazard `CompileCache.cmake` warns about
  needs an *incremental* build across checkouts at *different* absolute paths.
  Every runner builds from scratch at `D:\a\fastcached\fastcached`, so neither half
  holds. Change either — a persistent Windows runner, or a checkout somewhere else
  — and this stops being true.

- **Packaging and coverage stay on pull requests, and `package-macos` is why.** It
  is the only job that compiles with `/usr/bin/clang` and Apple's own libc++; the
  `macos` job uses Homebrew LLVM's much newer one. That is two standard libraries,
  and it is precisely the split that let `std::views::enumerate` build clean
  everywhere and break the package job. Moving it off pull requests would move that
  class of failure from the pull request to master. `package-windows` is the only
  place the MSI and the service registration are exercised at all — which is what a
  `packaging/` change breaks. Buying those minutes with a break that reaches master
  is not a trade.

  **The price was understated and one supporting clause was simply false**, and
  both are worth having straight, because a refusal has to survive being re-argued
  with correct numbers. It was re-measured on 2026-09-02 over 13 pull-request runs,
  which found the price understated (~17 claimed against 26.7 actual) and one
  supporting clause simply false ("coverage is not on the critical path"); those
  figures are superseded by the 99-run sample below and are not kept here, because
  three stacked measurements of one quantity in one bullet is how a grep finds the
  dead one first.

  Neither guard objects, so neither can be leaned on as though it were the reason.
  `check-release-gate` asserts the *list* in `release.needs`, not that each job
  runs, so a skipped job still gates. And **none of these four jobs was a required
  context** when the `default-master` ruleset was read on 2026-09-02 — neither
  `Code coverage` nor any `Package (…)` was among its contexts — so gating their
  execution would not have produced the never-arrives failure this file records
  three ways.

  Read the ruleset again before acting on that, and do **not** substitute
  `RequiredContexts` in `scripts/check-merge-queue-contexts.sh` for it. That table
  agrees today, but it is a MIRROR of a server-side setting and its own header says
  which direction it cannot see: *a context added to the ruleset and not added here
  is not caught.* Absence from the mirror is therefore not evidence of absence from
  the enforced list, and it is the enforced list that decides whether skipping a
  job strands a queued pull request. The recipe is in that header
  (`gh api …/rulesets`). The refusal is on the merits regardless, and the merits
  are `package-macos`.

  **Re-measured a third time on the 99-run sample, and the question has MOVED.**
  Dropping all four from pull requests is **102.7 → 76.6 runner-minutes (−25%)**,
  longest job 19.0 → 16.4, wall clock **24.1 → 21.4 (−11%)** — so it is still
  mostly a runner-minutes lever and barely a latency one, because `clang-tidy` is
  the longest pull-request job in 18 of 36 runs and remains so afterwards. What
  changed is where the interesting instance is: with the queue's sweep diff-scoped,
  **`Code coverage` at 14.0 min is the job most often longest in the merge queue**,
  which is on the merge path in a way the pull-request copy never was. Anyone
  reopening this should reopen it there. The `package-macos` merits are unchanged
  and still decide it.

  Read 14.0 against the 15.0 projected two sections above as *the usual winner*
  against *the mean of the maxima*, not as two answers to one question. A bimodal
  sweep is longest in the minority of entries where it escalates and is a minute in
  the rest, so the mean longest job can exceed the job that is usually longest, and
  both numbers are projections carrying the bias named there.

- **The merge queue's wall clock sits 8.2 min above its longest job, and that gap
  is runner acquisition, not the `changes` gate.** Worth knowing before anyone
  proposes ungating the queue to save it. Measured on the same sample: run creation
  to `changes` starting is **1.39 min**, but `changes` finishing to `clang-tidy`
  starting is **6.55 min** — ~20 jobs asking for hosted runners at once, across
  three events that overlap. Removing the gate buys at most the first number and
  breaks the docs-only scoping that makes a `pull_request` cost 0.6 runner-minutes.
  It follows that anything cutting the total number of concurrently queued jobs buys
  wall clock *indirectly*, and that second-order effect is **unquantified** — it is
  recorded as a direction, not as a figure.

- **`Code coverage` is four-fifths an uncached compile, and it cannot be cached.**
  Step timings over 10 code-touching pull-request runs on 2026-09-02: `Build`
  **11.05 min** (max 13.03); `Measure coverage` — the whole ~2000-case suite under
  instrumentation *plus* `llvm-profdata` and `llvm-cov` — **2.31 min** (max 2.37, a
  60 ms spread); no other step averaging over 20 seconds. So the natural guess, that only
  the report-merging step is cheap to move, is correct and useless: that step is
  already cheap. The expense is a full instrumented build with no compiler cache in
  front of it, and `cmake/Coverage.cmake` refuses to configure with one because a
  replayed object's coverage mapping names the tree it was built in. **This job
  gets shorter by running less often, never by building faster** — which puts it
  back on the `package-macos` merits above, rather than on anything a cache can do.

- **A WSL build belongs under `~`, not under `/mnt/d`, and the cost of getting it
  wrong is threefold.** `/mnt/*` is a bridge to the Windows filesystem — served
  over **9p** on WSL2, which is what `df -T` reports; DrvFs is the WSL1 mechanism
  and is what the older notes in this tree call it — and a build is almost entirely
  small-file I/O. Measured on this machine — same commit, clang 20, Debug,
  `USE_COMPILER_CACHE=OFF`, `-j8`, target `fastcached`, a warm configure so only
  the compile is timed:

  <!-- table-total: none -->
  | | ext4 (`~`) | 9p (`/mnt/d`) | |
  |---|---|---|---|
  | build, wall | **19.6 s** | **60.5 s** | 3.1× |
  | build, user CPU | 125.2 s | 127.0 s | 1.01× |
  | build, sys CPU | 6.9 s | 19.6 s | 2.8× |
  | open+read every `*.hpp` | 0.11 s | 1.02 s | 9.3× |

  The user time is the number that settles it: the two builds did **the same
  compute**, to within 1.5%. Everything the bridge costs is spent waiting on the
  filesystem, which is also why the header walk — pure I/O, no compute — is the
  worst ratio of the four, and why the hygiene checks that read every file under
  `src/` cost an order of magnitude more there than on ext4 without anything being
  broken. This passage used to say `sccache-backend-caveat` **times out** on
  `/mnt/d`; after #502 and #479 it does not — measured 3.8–5.1 s and ~4 ms/file on
  this bridge, a few percent of its budget — and it now prints that figure on every
  run rather than leaving anyone to infer it from here.

  This is not a preference, because there is a second, harder reason: a
  **Windows-created worktree is unreadable by WSL git at all** — its `.git` file
  holds a drive-lettered path, so `scripts/local-gate.sh` dies and
  `repository-hygiene` silently skips inside one. A WSL build needs a WSL-created
  checkout, and the moment you are making one anyway, make it under `~`.

- **How you PROBE that penalty decides which number you get, by two orders of
  magnitude — so measure the syscalls, never a shell loop.** A probe that spawns
  `stat` and `rm` per iteration charges ~0.4 ms of fork and exec to *both*
  filesystems, which drags the ratio towards 1; a build issues the syscalls and
  spawns nothing per file. Same machine as the table above, WSL2 kernel
  `5.15.167.4-microsoft-standard-WSL2`, 32 cores, load average under 0.3, best of
  three, 2026-09-02:

  <!-- table-total: none -->
  | probe | ext4 (`~`) | 9p (`/mnt/d`) | ratio |
  |---|--:|--:|--:|
  | 6000 ops through a shell loop (`: >`, `stat`, `rm`) | 1.80 s | 12.02 s | 6.7× |
  | 6000 `open`+`fstat`+`unlink`, no spawns | 0.017 s (350k ops/s) | 4.12 s (1460 ops/s) | **240×** |
  | 2000 warm `stat()` | 0.0006 s (3.4M ops/s) | 1.60 s (1250 ops/s) | **~2700×** |
  | 256 MiB write + `fsync` | 0.092 s (2.8 GiB/s) | 1.14 s (224 MiB/s) | 12.4× |

  The middle two rows are what a build feels: **a warm `stat` costs about 0.3 µs on
  ext4 and about 0.8 ms on 9p**, because ext4 answers out of the dentry cache while
  9p makes a round trip to the Windows host for every one. `src/` holds **677
  entries** (648 files, 29 directories), so at 1250 stat/s a single traversal of it
  costs about **0.5 s** of bare `stat` where ext4 charges 0.2 ms — which is the
  same order as the **2.09 s** `GLOB_RECURSE` traversal independently measured in
  *The local gate* above, and why the checks that walk the tree are invisible on CI
  and on ext4 and blow their budget here. The remaining ~4× is the glob's own
  round trips per entry — `readdir`, symlink resolution, the `stat` this row times
  — and has NOT been measured, so do not read 0.5 s as a prediction of 2.09 s. A
  shell loop over the same two filesystems reports 6.7× and would justify nothing.

  Two things this does *not* say, because both were reached for while measuring it.
  A warm `ninja -n` over the whole tree runs in **0.09 s on 9p** — ninja caches and
  the page cache is warm — so anyone probing with that measures nothing and
  concludes there is no penalty. And these are probes, not a build: the build
  numbers are the table above, and a re-measurement of *those* is worth nothing
  while another lane is compiling on the same machine, which is the usual state of
  this repository.

  And a build measurement that gives each side its own `CPM_SOURCE_CACHE` makes
  both sides re-clone Catch2, zstd and lz4 — thousands of small files, which is
  precisely the access pattern the table above prices — so the configure dominates
  the thing being timed and can fail outright. That is a per-worktree cost nobody
  budgets, it is paid independently by every lane, and no per-job timing anywhere
  shows it;
  [#545](https://github.com/LASTRADA-Software/fastcached/issues/545) is where it is
  being dealt with. Point a measurement at the machine's populated cache instead.

## Language and ABI pitfalls

- **A return type is not part of a function's name on Linux, and MSVC's mangling
  hides that.** `Core/HostPort.hpp` added an `inline FastCache::ParsePort(
  std::string_view)` returning `std::optional<std::uint16_t>` while
  `Config/CliParser` already had a `FastCache::ParsePort(std::string_view)`
  returning `std::expected<std::uint16_t, ConfigError>`. That is **not an
  overload**, and no compiler can say so: each translation unit sees exactly one
  of the two declarations, so both compile, and the Itanium ABI does not encode a
  return type in a free function's mangled name -- so both definitions claim the
  identical symbol, the linker keeps `CliParser`'s strong one over the header's
  weak inline, and every caller of the header version silently reaches the other.
  It reads an `expected` as an `optional`: a SIGSEGV on the first call, from code
  that is correct in isolation. Renamed to `ParseTcpPort`, with the reason at the
  declaration. Three things worth keeping:
  - **Windows cannot find this and will report the tree as green.** MSVC's
    mangling *does* include the return type, so the two are distinct symbols
    there and both link. This branch had 1730 passing MSVC tests at the moment
    Linux was segfaulting, which is the whole argument for running the Linux
    gate locally rather than discovering it in CI a phase later.
  - **A standalone reproducer will not reproduce it**, because the bug is in the
    *link*, not the code: the same calls compiled against the header alone are
    correct and pass under ASan. What identified it was `nm -C` on the library
    object, showing a strong `T FastCache::ParsePort(...)` that the test binary
    had no business resolving to.
  - **The two implementations were not merged**, deliberately. The CLI's version
    distinguishes "not a number" from "out of range" because an operator needs
    to be told which; an `optional` cannot carry that. Collapsing them to share
    one body would trade a real diagnostic for a de-duplication nobody asked for
    -- the same reasoning that keeps the dispatch counters split.

- **`main` is not exempt from cognitive complexity, and the fix is extraction rather
  than a raised threshold.** The node's `main` reached 70 against clang-tidy's limit
  of 60 as the admin endpoint was wired in. Both blocks that came out --
  `AdminEndpoint::Start` and `AdoptActivatedListener` -- are coherent decisions with
  one answer each, which is why the number was a symptom worth listening to rather
  than a rule to argue with. The six bare `return 2`s it left behind became
  `ExitUsage` for the same reason: seven copies of a magic exit code is the
  table-shaped defect this list keeps recording.

- **Never run `clang-format -i` with a version other than the pinned one.** As a
  *checker* an older binary is worth something; as a *formatter* it rewrites code
  the pinned version already blessed, and the diff is invisible in review because
  every line of it is "just formatting". Running `clang-format-18 -i` on
  `FleetView.cpp` to tidy three added lines reflowed all three column tables --
  code the change never touched -- and `Check C++ style` rejected 80 lines at
  clang-format 22, none of them new.

  The repair is not to reformat again but to **restore the untouched region
  byte-for-byte** from the last commit that passed the style job, then re-insert
  only the new lines, and prove it: a diff of that region against the good commit
  must show insertions and *zero* deletions.

  So: run a non-pinned binary as `--dry-run` on the lines **you** added, never with
  `-i`, and never let it touch a file you are only passing through.

  **"The apt mirror has no `clang-format-22`" is not a reason to format with 18.**
  LLVM ships the official binaries on PyPI, so the pinned version is one download
  away on any host with outbound HTTPS and no root:

  ```sh
  pip download "clang-format==${CLANG_TOOLS_VERSION}.1.0" -d /tmp/cf --no-deps
  python3 -m zipfile -e /tmp/cf/clang_format-*.whl /tmp/cf22
  install -m755 /tmp/cf22/clang_format/data/bin/clang-format ~/.local/bin/clang-format-22
  ```

  With that on `PATH`, `scripts/local-gate.sh` finds it by name and the whole tree
  can be formatted exactly as `Check C++ style` will judge it -- which is strictly
  better than hand-matching a style guide and then finding out in CI. Reach for the
  byte-for-byte restore above only when even this is unavailable.

  **`clang-tidy` ships the same way, and it matters more.** Successive releases add
  *checks*, so an older binary is not merely a laxer formatter -- it is silent about
  entire categories. `modernize-use-scoped-lock` and
  `readability-math-missing-parentheses` do not exist in 18, and 22's
  `bugprone-unchecked-optional-access` follows a value through a binding that 18's
  does not, so a file clean under 18 arrived at CI with thirteen findings.

  ```sh
  pip download "clang-tidy==${CLANG_TOOLS_VERSION}.1.0" -d /tmp/ct --no-deps
  python3 -m zipfile -e /tmp/ct/clang_tidy-*.whl /tmp/ct22
  ```

  **Run it in place, or through a wrapper that does.** `clang-tidy` finds its own
  resource headers *relative to the binary*, so copying just the executable out of
  the wheel produces `'stddef.h' file not found` -- and every check then reports
  against a translation unit that did not parse, which looks like a wall of real
  findings and is nothing of the kind.

  It also needs a **clang** compile database, generated with **module scanning
  off**. Pointed at a GCC one it inherits flags clang does not know; and a database
  from a module-scanning generator carries `@…modmap` arguments that do not exist
  until that target has been built, so the translation unit fails to parse and the
  file reports nothing at all. Configure it **through the preset the CI job
  configures**, into a directory of its own:

  ```sh
  cmake --preset clang-debug -B out/build/tidy22 -DENABLE_TIDY=OFF \
        -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DFASTCACHED_ENABLE_TLS=ON \
        -DFASTCACHED_BUILD_BENCHMARKS=ON -DFASTCACHED_BUILD_TESTCLIENT=ON
  ```

  **Not a hand-rolled `cmake -S . -B …` equivalent**, which is what stood here and
  what `tidy-sweep.sh` documented until #454. A bare configure line inherits
  `PEDANTIC_COMPILER` and `PEDANTIC_COMPILER_WERROR` OFF, so its database carries
  neither `-Wall -Wextra -Wconversion -pedantic` nor the `-Wno-…` suppressions that
  accompany them — **29 flags apart on the same translation unit**. `.clang-tidy`
  enables `clang-diagnostic-*`, so those flags decide what the sweep reports, and it
  diverges *in both directions at once*: findings CI suppresses appear, and findings
  CI raises do not. Measured: a `double`→`int` return that CI's database reports as
  `clang-diagnostic-float-conversion` produced **nothing** from the hand-rolled one.
  The noisy direction wastes a cycle; the blind direction ships to CI, and both read
  like a clean tree.

  That is one of #454's two halves. The other is below, under
  "`PEDANTIC_COMPILER_WERROR` decides fatality, not which warnings exist": the
  three `clang-diagnostic-c2y-extensions` findings it reported are what a database
  with `PEDANTIC_COMPILER` **ON** and `WERROR` **OFF** produces, which is what a
  build directory reused across presets holds — and which no correction to the
  configure line could have explained. A symptom with two mechanisms reads as
  unreproducible the moment either one alone is ruled out.

  It is also how the target-set bullet above gets violated by the very page that
  states it: the old line omitted the two default-OFF app targets, so four
  first-party translation units were absent from the database rather than clean.
  A rule whose own example contradicts it is worse than no example.

  `-B` overrides the preset's `binaryDir`, which is what keeps this out of the
  `out/build/clang-debug` tree a DEVELOPER builds in — configuring the sweep's
  database must not turn `ENABLE_TIDY` and module scanning off in that one. (It
  used to be the tree `local-gate.sh` built in as well; since
  [#487](https://github.com/LASTRADA-Software/fastcached/issues/487) the gate owns
  `out/build/gate-*` and reaches this one no more than the sweep does. The
  reasoning is unchanged — only the list of things that would otherwise collide in
  there got shorter.)

  The line is the `clang-tidy` job's `Configure` step verbatim plus `-B`, and
  `ctest -R tidy-sweep-database` asserts it stays that way: nothing else connects
  the workflow to the two places that document it.

  Verbatim duplication is right *under the constraint it was written under* and
  wrong permanently — [#615](https://github.com/LASTRADA-Software/fastcached/issues/615)
  carries the `tidy22` preset that removes it, and must be taken by whoever can
  change `build.yml` in the same change: a preset CI does not adopt is a fourth
  copy, not a consolidation. The check moves to comparing against the preset in
  that same change, or it fails correctly and confusingly against documentation
  that is still right.

- **A sweep that cannot prove the tool ran is worth nothing, and reads like
  success.** Every way of getting clang-tidy wrong above -- an unset execute bit
  (the wheel does not carry one), a wrapper that cannot exec, a missing resource
  dir, an unparseable `@modmap` -- produces *silence*, and silence filtered through
  `grep 'error:'` is indistinguishable from a clean file. This branch shipped to CI
  twice on sweeps that had reported clean while executing nothing.

  `scripts/tidy-sweep.sh` is that lesson in executable form: it canaries the binary
  against a real source file first, treats exit codes ≥ 126 as fatal rather than as
  findings, and refuses to print a clean verdict it did not earn. Use it, and if you
  write a one-off loop instead, make it fail loudly the same way.

  "Refuses a verdict it did not earn" is four separate refusals, and each one closes
  a path that ends in `CLEAN` over nothing: the plan's exit status is *observed*
  (`mapfile < <(plan)` throws it away, and every way a compile database can fail to
  parse then yields zero rows, which reads as "nothing to sweep"); the include scan
  is fatal when `grep` **fails** rather than merely matching nothing (an empty graph
  silently narrows the scope to the changed `.cpp` files); a plan with units in it
  and none of them present in the tree is fatal, not `CLEAN (0 translation
  unit(s))`; and the extension table that decides what may be handed to clang-tidy
  is a table, so a `.c` unit cannot be walked into the graph and dropped on the way
  out. The self-test also pins `LC_ALL=C` on its ordering — under `en_US.UTF-8`
  `sort` collates case-insensitively and the assertions fail on a developer's
  machine while passing on a runner's `C.UTF-8`.

  And the sharper form of the same failure, because the canary above does **not**
  catch it: **the tool can run, exit 0, and have analysed nothing.** Every guard in
  this paragraph asks whether clang-tidy *executed*; none asks whether it had any
  check enabled while it did. `--checks=-*,clang-diagnostic-c2y-extensions` selects
  no check at all in clang-tidy 22 — `clang-diagnostic-*` is a **filter over compiler
  diagnostics, not a check that can be enabled by a glob** — so a sweep spelled that
  way reports `0 findings` for every translation unit, with a successful exit status,
  a proven-executable binary and a database that parses. Measured while investigating
  #454: six full sweeps across four compile databases, all reading zero, all
  worthless. The only thing that exposed it was a deliberately planted finding that
  *also* read zero. So a `--checks` expression narrowed for speed is itself an
  instrument, and it is calibrated the way every other instrument here is — plant a
  finding the expression must report, and watch it report it, before believing a run
  of zeroes. `Error: no checks enabled` on stderr is the tell, and `--quiet` plus a
  `grep` hides it.

- **A `bool` in the middle of a config struct costs seven bytes, and four of them
  fail the build.** `clang-analyzer-optin.performance.Padding` permits 24 bytes more
  padding than an optimal field order would give, and `NodeConfig` is almost entirely
  `std::string` and `std::filesystem::path` -- so a one-byte member between two of
  those is padded out to a full eight rather than costing one. Adding `--dashboard`
  and `--tls-self-signed` beside the settings they configure took the struct to 38
  bytes against an optimal 6, and the `clang-tidy` job -- the only one that runs the
  analyzer -- rejected it.

  Keep every `bool` and byte-wide enum in **one run**, which that struct already half
  did with its trailing flags. Two of the four moved were pre-existing (`logLevel`,
  `serviceScope`), and that was the point: moving only the two new ones lands on
  *exactly* 24, which passes and leaves the next contributor's `bool` to break it
  again. Moving all four lands on 8. A field's position there is a layout constraint,
  not where it reads most naturally, so the run says so in a comment.

  The check is `BaselinePad - OptimalPad > 24` and both terms are arithmetic over
  `sizeof`, so it reproduces **without the analyzer**: sum the field sizes, subtract
  from `sizeof(T)`, and compare against the same sum laid out by descending
  alignment. Worth knowing because it gives an exact number where re-running CI gives
  a yes or no -- and because a host without the pinned clang-tidy can still check it.

- **A `static_assert` that anchors a table's length on an enumerator BY NAME is a
  guard that fires only when nothing is wrong.** Casting an enumerator to an index
  is safe exactly while the table holds one row per enumerator in enumerator order,
  and this tree used to spell that rule five ways across eight tables -- of which
  **six failed open in the one situation the rule exists for**. Four were anchored
  by name:

  ```cpp
  static_assert(Table.size() == static_cast<std::size_t>(Enum::TheLastOneToday) + 1);
  ```

  Append an enumerator and *forget* the row: `size()` still equals
  `TheLastOneToday + 1`, so it compiles, and the lookup then reads past the end --
  reproduced verbatim under ASan as a `global-buffer-overflow` four bytes past
  `RefusalTable`. Append one and *remember* the row: the size is now one greater
  and the assert fails. It is inverted, and nothing about reading it says so. Two
  more tables had no length tie at all, and one of those, `Consensus::RoleTable`,
  was reached by a linear scan that fell back to `RoleTable.front()` -- so a
  missing row returned a **follower's** traits for a leader, silently, which is
  worse than the crash. `Core/EnumTable.hpp` is the one spelling now: `Last`
  states the enum's own count, `EnumTable<Enum, Row>` takes its extent from that
  so a short table cannot be *declared*, and `RowsInEnumeratorOrder(table,
  &Row::member)` checks the extent and every row's position in one assert.
  Consequences that are each load-bearing:
  - **The row carries the enumerator it describes, and that is what makes the
    order checkable at all.** A bare array of values can only have its length
    asserted -- which is why the three bare ones (`RoleNames`,
    `ProposalRefusals`, `PickErrorTable`) recorded their order in trailing
    comments no compiler reads. Giving them struct rows is not decoration; it is
    the difference between a checked invariant and a documented hope.
  - **`Last` is a cost to measure, not to assume.** It was free for these six
    because nothing in the tree switches over any of them -- grepped, not
    guessed. Adding a sentinel to an enum whose callers deliberately omit
    `default:` turns every one of those `switch`es into a build error, and this
    codebase omits `default:` on purpose in several places (`PathCanon::Anchor`,
    `DirectManifest::PathRole`) precisely so a new state is a compile error. That
    trade has to be made rather than inherited.
  - **A proved order turns the lookup into an index rather than a search.**
    `TraitsOf` scanned `RoleTable` and carried an eight-line comment re-deriving
    the `std::array`-iterator portability argument that `Core/Ranges.hpp` already
    documents; with the position guaranteed there is nothing to search for, and
    the comment went with the scan.
  - **The guard was verified by reintroducing the defect at all six sites.** One
    throwaway enumerator appended with no row, and each of the six now fails to
    compile naming its own table. A completeness guard that has never been seen
    to fail is exactly the thing this entry is about.

## What a `char` is

- **Every Windows executable declares UTF-8 as its process code page, and the
  declaration is applied by walking the build system rather than by a line per
  target.** Windows keeps command lines, environment blocks and paths as UTF-16 and
  transcodes them for a narrow caller through the process's *active code page*,
  whose default is the host's legacy one — 1252 on a Western install. That one
  setting decides `argv`, `getenv`, every `...A` API this tree calls
  (`CreateProcessA` in the launcher's process runner, `CreateServiceA` and
  `GetModuleFileNameA` in `ServiceControl`, `RegQueryValueExA` in `Registry`) and
  `std::filesystem::path`'s narrow conversions in BOTH directions — MSVC's
  `__std_fs_code_page()` answers `CP_ACP` unless the CRT locale is UTF-8. Since #141
  the fleet refuses a registration whose fields are not valid UTF-8, so a non-ASCII
  `--toolchain` or `--advertise` typed on a Windows console was refused for a reason
  invisible from where it was typed (#155).

  **The obvious fix is the wrong one.** `GetCommandLineW` + `CommandLineToArgvW` +
  `WideCharToMultiByte` in a seam every `main` calls converts ONE boundary and
  leaves the rest on the legacy page — which turns a wrong encoding into a *split*
  one: UTF-8 `argv` handed to `std::filesystem::path` decodes as CP-1252 and names a
  different file, and handed to `CreateProcessA` spawns the compiler with a mangled
  command line. Both work today. Closing that gap means policing a second convention
  across 45 path constructions, 122 `.string()` calls and six `...A` call sites,
  forever, with no compiler enforcement. `cmake/Utf8CodePage.cmake` carries the
  measurements.

- **`activeCodePage` is honoured from Windows 10 1903 / Server 2022 and ignored in
  silence below it**, which is the one thing a build-time setting must not be
  allowed to be. `FastCache::NarrowTextIsUtf8()` reports the OUTCOME rather than the
  intent, a Catch2 case asserts it — red at code page 1252, green at 65001 — and the
  node names the active code page in a parse refusal when it is not UTF-8, which on
  such a host is the whole answer and no other surface would ever give it.

- **`utf8-argv-*` is the only end-to-end proof there can be.** The defect is in what
  the OS hands a process, so nothing *inside* a process can observe it: a Catch2 case
  can assert the code page a test binary ended up with, and only running a real
  executable says what an argument BECAME on the way in. Each binary with an option
  table is run with an unrecognised flag spelled with U+00FC and must echo those
  exact bytes; the argument is built with `string(ASCII 103 114 195 188 110 …)`, raw
  byte values, so no file's own encoding is what is under test.

- **`std::filesystem::path`'s narrow constructor THROWS once the active code page is
  UTF-8** and the bytes are not — `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
  …)` refuses them — and it throws *before* any `std::error_code` overload downstream
  is reached, so a call site that carefully takes an `error_code` for every operation
  is still not protected. `Platform::PathFromNarrowText` is the one `catch` in this
  project's own code, and one rather than none because the standard library states
  this failure by throwing and offers no `error_code` overload of the constructor to
  ask instead. It is a guard, not a decision: where readability *decides* something,
  the caller asks `Utf8FromNarrowText` and says what it does about the answer.

- **A CHILD process has a code page of its own, and `cl.exe` does not use even that
  for output.** It writes the paths in `/showIncludes` in the CONSOLE OUTPUT code
  page — measured on this tree's toolchain at `C3 BC` for U+00FC under CP 65001 and
  `81` under CP 850. See `.agent/rules/compile-cache.md` for what the launcher does
  about it.

- **`/utf-8` is on for MSVC, so the compiler agrees with the runtime about what a
  narrow literal is.** Without it MSVC reads a source file in the host's ANSI code
  page and re-encodes its narrow literals into that same page: byte-identical on a
  CP-1252 or CP-65001 host, different bytes anywhere else. This tree has such
  literals (a Redis error reply, the Windows service description, an SVG chart
  caption), and the SVG one is the sharp end — an XML parser refuses a whole
  document whose encoding does not hold. Directory-scoped and applied after every
  CPM dependency has been added, exactly as `PedanticCompiler` and `Sanitizers` are,
  so a third-party source carrying a byte it would refuse never sees the flag.

## Line endings

Line endings are LF everywhere, and that is a `.gitattributes` rule
(`* text=auto eol=lf`) rather than an instruction to set `core.autocrlf`. The
config is per-clone and per-developer, so without the rule two people editing one
file disagree about what a line ending is — and the disagreement is invisible
until a diff comes back as *every line changed* for a two-line edit, which is how
it was found. Stored content was already LF, so the rule changed nothing that is
committed, only what lands on disk at checkout. `*.sh` keeps a row of its own even
though the general rule covers it, because the consequence there is specific: a
CRLF shebang makes the kernel look for an interpreter whose name ends in a
carriage return, so such a script does not misbehave — it fails to start at all.
## A reference build that is not one is wrong in BOTH directions

- **The gate REFUSES a launcher-fronted build rather than warning about it, and the
  reason is the direction nobody investigates.** The observed case
  ([#626](https://github.com/LASTRADA-Software/fastcached/issues/626)) was five
  metrics counter tests failing in four files the branch never touched, on a
  launcher-fronted MSVC build, all clearing with `-DUSE_COMPILER_CACHE=OFF` — a
  false alarm, which costs somebody an investigation. **The same substituted object
  can equally HIDE a real failure, and nothing would say so.** A verdict about a
  tree that was not built cannot be read in either direction, so there is no safe
  way to continue past it — which is what makes it a refusal. That sentence lives
  in the refusal's own text, because a developer who meets it should learn why it
  is not a warning.

- **The guard was already there and the ticket did not know.** `launcher_verdict`
  in `scripts/local-gate.sh` reads `build.ninja`, refuses rather than warns, states
  its count, and separates `unknown` and `unreadable` from `0` — all of it landed
  under #319/#368, citing neither #626 nor #454. Check a ticket's premise against
  the tree before building to it; a body describing a solved half is #779's shape.

- **The count is LAUNCHER BINDINGS, not edges, and it was called edges.** Ninja
  emits one `LAUNCHER = ` line per RULE where the value is uniform, so this tree on
  Linux reads **5, covering 501 compile edges**, while #626's Windows reproduction
  recorded **669**. Those are not comparable and nothing said so. The verdict is
  unaffected — any count above zero refuses — but a refusal message is read by
  somebody holding another platform's number, which is the whole reason it exists.
  **A unit error in a refusal message is how two numbers get compared that should
  not be.**

- **And the guard is blind to a compiler that IS a cache.** Measured on
  `gate-clang-debug`, the gate's own preset with the gate's own pin: `launcher_verdict`
  answers **0** — correctly, there is no `LAUNCHER` binding — while
  `CMAKE_CXX_COMPILER` resolves to `/usr/lib64/ccache/clang++`, because the preset
  spells a bare `clang++` and `/usr/lib64/ccache` precedes `/usr/bin` on that host's
  `PATH`. The blind spot is **total**, not partial, and it is
  [#804](https://github.com/LASTRADA-Software/fastcached/issues/804).

- On Linux the #626 symptom **does not reproduce** in either cache state: three full
  builds of one tree — populated shared caches, cold caches, and a reference build —
  and three full suites, with all five metrics tests passing in every arm. Stated as
  an analogue: #626's reproduction is Windows/MSVC/sccache and was not executed.
  **And the populated arm is weaker than it looks** — the cold build recorded exactly
  131 ccache hits, which is exactly the number of duplicate compilations in the tree,
  so every hit was intra-build and the populated one replayed only a handful more.
  A cache keyed per build directory is [#816](https://github.com/LASTRADA-Software/fastcached/issues/816).

## `PEDANTIC_COMPILER_WERROR` decides fatality, not which warnings exist

- **A flag and the suppressions it makes necessary are governed by ONE condition.**
  `cmake/portable/PedanticCompiler.cmake` added `-pedantic` and
  `-Wmissing-declarations` under `PEDANTIC_COMPILER`, and the suppressions those
  make necessary — `-Wno-c2y-extensions`, `-Wno-c++20-extensions`,
  `-Wno-missing-declarations`, `-Wno-class-memaccess` — inside the
  `PEDANTIC_COMPILER_WERROR` block. Two conditions over one concern, so **the four
  presets that inherit `PEDANTIC_COMPILER=ON` from `base` and leave `WERROR` off
  got the warnings and none of the suppressions**: `clang-coverage`,
  `clang-asan-ubsan`, `clang-tsan`, `clang-tracy`
  ([#611](https://github.com/LASTRADA-Software/fastcached/issues/611)).

  Measured on `0db96dc8`, each preset in its OWN fresh build directory against
  `/usr/bin/clang++` pinned by absolute path with zero `LAUNCHER =` edges: **7049
  `-Wc2y-extensions` warnings across 195 translation units, the same number in all
  four**, every one of them `'__COUNTER__' is a C2y extension` out of Catch2's
  `TEST_CASE` — so the count is a function of how many tests exist. The other three
  suppressions cover diagnostics that fire **zero** times on this tree at clang
  22.1.8: three suppress nothing and one suppresses everything, which is why
  nobody's intuition about which flag mattered would have been right.

  **Nothing could have reported it, in any direction somebody would look.** Those
  four presets have `WERROR` off, so 7049 warnings are 7049 warnings and every
  build exits 0. The preset people actually run — `clang-debug` — has `WERROR` on
  and is therefore correct. And the one place it becomes *visible* is a clang-tidy
  database from a build directory configured that way, where it reads as a stale
  cache rather than as a rule. That is exactly how
  [#454](https://github.com/LASTRADA-Software/fastcached/issues/454) came to be
  filed and then believed unreproducible: its three `clang-diagnostic-c2y-extensions`
  findings are this, and `-Wc2y-extensions` being pedantic-only in clang 22 made the
  hand-rolled configure line look like the whole story when it was half of it.

- **Split by what a flag DOES, never by where it happened to live.** `-Wno-X`
  selects a *diagnostic* and belongs beside the flag that makes it necessary;
  `-Werror` and `-Wno-error=X` decide *fatality* and belong under `WERROR`.
  `-Wno-error=X` says "keep X visible, do not fail on it", so a preset without
  `-Werror` that SEES X is getting precisely what that flag asks for — it is not
  this defect, and a rule that refused it would be turned off rather than obeyed.

- **Copying a suppression outward while leaving it inward does not close this.**
  Two conditions naming one diagnostic *is* the defect; the second copy is a second
  thing to keep in step. Move the flag.

- The `-Wno-error=` half is **belt-and-braces WHERE IT IS PAIRED, not dead code by
  proof**. A diagnostic disabled by `-Wno-X` can never be an error, so a paired
  `-Wno-error=X` matters only on a compiler where the `-Wno-X` probe fails while its
  own succeeds. Measured on clang 22.1.8 and GCC 16.2.1, the two halves of every
  PAIR succeed and fail **together** — clang rejects both spellings of
  `class-memaccess`, GCC rejects both of `c2y-extensions` — so those four are
  currently inert. That is a property of two compilers, not of the flags, which is
  why they stay.
  - **An UNPAIRED `-Wno-error=X` is the opposite: load-bearing by construction**, and
    reading the paragraph above as covering the whole family is how it goes stale.
    `-Wno-error=array-bounds` and `-Wno-error=maybe-uninitialized` have no `-Wno-`
    half and are what lets GCC 16.2.1 build this tree at all
    ([#805](https://github.com/LASTRADA-Software/fastcached/issues/805)): both fire
    inside libstdc++'s headers, from inlining, on code with no defect to fix, and
    g++-13/g++-14 report neither. They are gated to `GNU` — only GCC produces them,
    and ungated they would cost clang `-Werror` on two diagnostics that name real
    bugs. Deleting one as belt-and-braces takes `gate-gcc-release` red on every
    branch at once, which is the state #805 exists to have ended.
  - Retiring them is a MEASUREMENT, never a guess: a later GCC fixing the false
    positive is what makes them inert, and the check is that the tree still builds
    with the row removed. Three source rewrites of `EncodeCompileValue` were each
    measured to silence one compiler and break another — one of them shipped
    (166880b6) and took `Linux-gcc-release` red — so **the source is not the place**,
    and a fourth attempt needs a GCC 16 AND a g++-13/14 before it lands.

- **`ctest -R pedantic-suppressions` is the reader, and it DRIVES the file rather
  than scanning it.** It includes `PedanticCompiler.cmake` twice per compiler
  persona — the pair `CMAKE_CXX_COMPILER_ID`/`CMAKE_CXX_COMPILER_FRONTEND_VARIANT`,
  because `clang-cl` has clang's ID and MSVC's frontend and takes the other branch —
  with `add_compile_options` replaced by a recorder and `check_cxx_compiler_flag`
  stubbed through `CMAKE_MODULE_PATH`, and refuses any difference between the two
  `WERROR` settings that is not fatality-only. A text scan would have to model
  `if`/`elseif`/`else`/`endif` nesting to answer *which condition is this line
  under*, which is the one question that matters here, and a model of CMake that is
  subtly wrong fails in the confident direction. Including the file uses CMake's own
  answer. The stub answers **yes to every probe** deliberately: a probe that failed
  would drop a flag for a reason unrelated to the rule, and a dropped flag cannot
  violate it — so a real compiler would weaken the check on exactly the hosts where
  a flag is unavailable, invisibly.

- A guard nobody has watched refuse is not a guard, and *"it was seen failing before
  the fix"* is a fact about one commit that stops reproducing the moment the fix
  lands. `ctest -R pedantic-suppressions-selftest` runs **ten cases** -- eight
  synthetic subjects plus two absent inputs -- and names every one on every run,
  because a figure that disagrees with what the tool prints is the defect #723
  exists to prevent. Three are shapes an unwatched guard accepts: the suppression
  **copied** outward rather than moved, a subject that no longer adds `-pedantic`,
  and an MSVC arm that adds nothing.

- **The copied case is why the difference is a COUNT and not a set.** A suppression
  added outside the block *and* left inside it is present at both `WERROR`
  settings, so as sets the difference is empty and the check passes -- permanently
  and by construction -- while its own refusal says *"move the flag, do not copy
  it"*. A guard that reads as enforcing something it cannot is worse than no guard,
  because the trailer sends people at the one shape it does not catch. Measured on
  the real subject with the copy planted: the membership form reports success, the
  counting form refuses by name.

- **And a selftest that matches a `FATAL_ERROR` must flatten its whitespace first.**
  CMake word-wraps that message at about 76 columns with indented continuations, so
  where a line breaks depends on the length of whatever is interpolated into it --
  and two of these messages begin with a PATH. Measured: with the selftest's scratch
  directory 17-24 characters one case failed, at 28-48 a different one failed, and
  at 52 or more both passed. `cmake -B /tmp/b` would have failed the test on a clean
  tree while this repository's own deep `out/build/<preset>` path passes, so CI
  stays green and only some developers see the red -- and what it prints accuses the
  check of drift when the only variable is `$PWD`.

## The portable compile-cache module

- **`cmake/portable/CompileCache.cmake` must stay stock-CMake-only, and must never fail a
  configure.** Same constraint as `Cli/UsageDoc` and `Protocol/CompileCacheWire`,
  for the same reason: the file is *meant* to be copied verbatim into other
  projects, so a dependency on anything else here breaks it where nobody in this
  repository would notice. It is also included at `CMakeLists.txt:164`, before CPM
  is bootstrapped at `:183`, so `CPMAddPackage`/`FetchContent` are not available to
  it even locally — the `FASTCACHE_AUTO_INSTALL` fetch therefore uses bare
  `file(DOWNLOAD)`. Not with `EXPECTED_HASH`, which aborts the configure on a
  mismatch *even when `STATUS` is captured* (measured); the SHA-256 the release
  publishes is compared by hand instead, which is the same guarantee without the
  abort. Every other way the fetch can fail — unpublished platform, no network, a
  binary that will not run here — ends the same way, in one `message(STATUS)` and a
  fall-through, because a project that vendored this file to get a *faster* build
  must not lose the ability to build at all when GitHub is unreachable.
  `ctest -R compile-cache` covers both halves offline, the decline paths through a
  sandbox and the install path through a `file://` mirror.
  - **The ways it can fail a configure are not obvious, and two of them arrived in one
    change.** `check_<lang>_compiler_flag` is a hard `CMake Error` — "C: needs to be
    enabled before use" — for a language the project has not enabled, and this module
    is included from a `project()` that lists CXX first, so asking it unconditionally
    ended the configure outright. Ask `get_property(GLOBAL PROPERTY ENABLED_LANGUAGES)`
    first; a language nobody enabled also has no compile lines to put a flag on, so
    skipping it is right rather than merely safe. And a bad flag left in
    `CMAKE_<LANG>_FLAGS` fails the compiler ABI check, which is a configure failure
    too — so a flag is **checked** rather than gated on a compiler-ID string. `cl`
    answers an unknown `-f...` with warning D9002 and exits 0, so the check has to be
    kept away from it by the guard rather than expected to reject it.
  - **Anything this module computes and appends is a function, checked as a
    computation.** `_fc_debug_prefix_map_rules` + `ctest -R debug-prefix-map-rules`,
    which is in the default set and needs no compiler. The alternative is two
    ~40-second configures per layout, so the layouts that break it — out-of-tree,
    another mount, in-source — are exactly the ones nobody has locally and nobody
    tests. It was wrong twice before it was first RUN: `file(RELATIVE_PATH)` answers
    with a trailing separator, and out-of-tree it answers
    `../../mnt/d/.../checkout`, which is *relative* and carries the whole checkout
    path. **Relative does not imply machine-independent**, and a value that looks
    portable is worse than one that obviously is not.
## `USE_COMPILER_CACHE` in full

`USE_COMPILER_CACHE` (default ON, `cmake/portable/CompileCache.cmake`) fronts the compiler
with our own `fastcache-cc` when it is on `PATH` and a daemon answers — at
`127.0.0.1:6674` by default, or wherever `FASTCACHE_ADDR=host:port` points;
`FASTCACHE_SOURCE_DIR`/`FASTCACHE_BINARY_DIR` are injected from the source and build
trees. Configure proves the cache works by compiling one tiny file through the
launcher (~0.1 s) and requiring a `HIT`/`MISS`, because a launcher that cannot
reach its daemon still compiles fine and would otherwise cost every TU a failed
connect in silence. When nothing answers it falls through to `ccache`; `sccache`
sits between the two in preference order but is **opt-in**, see below. When
*nothing usable* is installed, `-DFASTCACHE_AUTO_INSTALL=ON` (default OFF) fetches a
prebuilt `fastcache-cc` for the host from the latest stable release instead,
staged per user so a machine downloads it once; `cmake/README.md` is the note
for projects vendoring the module. A cache hit reproduces only the object file,
so with either launcher active the module scan and precompiled headers are
turned off and MSVC debug info is forced to `/Z7`
(a modmap flag makes the launcher's preprocess step fail, and a PCH or shared
PDB is a second artefact no hit can reproduce).

- **Under MSVC and clang-cl the `sccache` fallback can silently produce a wrong
  build, and the configure warns about it now rather than leaving it to be
  discovered.** sccache replays a cache hit's `/showIncludes` stream verbatim —
  the **absolute** paths spelled by the build that *stored* it — while the text it
  hashes to find that hit carries no paths at all, because it preprocesses MSVC
  with `/EP` and `/EP` emits no line markers. Two checkouts therefore share
  entries and then record each other's headers as their dependencies. Measured on
  this repository: two worktrees at one commit, stock configure, no daemon —
  **137** cross-worktree cache hits, **1097** dependency edges recorded pointing
  at the *other* worktree and **none** at its own, and `ninja: no work to do`
  after a real edit to the checkout's own `Logger.hpp`.

  **GCC and Clang are not exposed, and the negative result is the reason the
  warning is scoped rather than unconditional.** Their preprocessed output carries
  `# n "path"` line markers, so with the absolute include paths CMake generates
  the hashed text differs between checkouts and there is no hit to replay
  (measured on Ubuntu 24.04 with g++-14: 0 hits, 2 misses). Spell the same compile
  with relative paths and there *is* a hit — but then the depfile is relative too
  and resolves inside the consuming tree. Self-consistent either way. A warning
  that fired on the majority of this project's CI, where it cannot happen, is one
  contributors would learn to skip.

  The symptom is not a stale build you notice. It is a green build and a crash
  somewhere unrelated: adding a virtual to `IStorage` linked objects compiled
  against the old vtable, and five `ShardedStorage` tests segfaulted inside `Get`.

  **It bites an incremental build across two checkouts that share one cache.** A
  clean build has no dependency graph to corrupt, and checkouts that all sit at the
  same absolute path replay paths that are correct — CI is normally both, and
  downstream projects run CI with no daemon reachable on purpose. That used to be
  the argument for the fallback staying automatic; **#815 overturned it, for a
  reason the hazard analysis could not see** — see the next entry. Passing
  `-DSCCACHE=` falls through to ccache, whose default empty `base_dir` means it
  does not rewrite absolute paths and so does not share entries between checkouts.

  `fastcache-cc` does not have this failure mode by construction — `MaterializeHit`
  runs every stored region through `PathCanon::LocalizeRegion` before replaying it,
  and `MissingReplayedDependency` refuses a hit whose replayed dependency is not
  there. That is the fix, not a workaround: install it and run a daemon, or
  configure with `-DUSE_COMPILER_CACHE=OFF`.

  A launcher's caveat is a **row in the candidate table** (`_fc_cache_<id>_caveat`),
  not an `if` on its name, and `ctest -R compile-cache-caveat` asserts both halves —
  that sccache warns, and that the launchers without a hazard stay silent. A warning
  that fired for every launcher would be one nobody reads.

- **sccache is never selected automatically, and a version refusal is announced
  rather than reported (#815).** Two rules, one defect, and the second is the half
  that survives the first.

  The reporting host ran a packaged `/usr/bin/fastcached` 0.1.0 while
  `/usr/local/bin/fastcache-cc` was a current build. Wire 3 against a server
  speaking 1..1 — correct on both sides, a deployment mismatch and not a bug — so
  every exchange was refused, `CompileCache.cmake` fell through, and the project
  built through sccache for weeks while believing it dogfooded its own launcher.
  **Every mechanism worked.** The launcher degraded exactly as designed, recorded
  the refusal, and tallied it in `--show-stats` (1003 compiles, 2.1%, reaching no
  cache). The configure's own probe passed the fallback silently because it tests
  **reachability, not acceptance** — it accepts a `HIT`/`MISS` and a refusal is
  neither, so the row was skipped at `STATUS` among a hundred other status lines.

  - **The gate.** `_fc_cache_sccache_requires` is `${ALLOW_SCCACHE_FALLBACK}`,
    default OFF — a value in a column that already existed, not a branch. Opted
    into, sccache keeps its rank above ccache, because asking for it is asking for
    it. The alternative remedy — have CI set `CMAKE_C/CXX_COMPILER_LAUNCHER`
    externally — would have worked and is **wrong**: this module returns early when
    a launcher is already set, so it would have taken the `/showIncludes` caveat
    warning above out of the log with it, silently, on exactly the platform where
    that hazard exists. Gating the row keeps the warning. The option name carries no
    project prefix, like `USE_COMPILER_CACHE`, because the file is vendored verbatim.
  - **The three Windows jobs opt in**, and that is a hard requirement rather than a
    convenience: `windows`, `compile-cache-e2e-windows` and `package-windows` front
    the compiler with sccache and measured 80–99% hit rates. A job that lost the flag
    would compile *cold*, not break — which is the failure CI is worst at noticing —
    so the existing `compile_requests -eq 0` assertion in all three is what catches
    it. That assertion is the guard; there is no separate check.
  - **Dropping sccache does not end the silence, it RELOCATES it.** On the reporting
    host ccache is installed, so the same refusal now falls through to ccache with
    the same absence of announcement. So the second rule: a rejection whose reason
    matches the row's `predicts` column is a `message(WARNING)` **whatever replaced
    it, including nothing**. #658 had already built the machinery and conditioned it
    on the winner carrying a caveat — which is a property of the COMPILER, true on
    MSVC and clang-cl and false on GCC and Clang — so on the host that filed #815 it
    was silent by construction. Three replacements, three wordings, one warning:
    safe, hazardous, and none at all. The last is the loudest, because the build then
    compiles uncached, and it is the one that used to emit nothing at all: the loop
    lived inside `if(_fc_cache_chosen)`.
  - **A `message(WARNING)`, never `SEND_ERROR`.** The module may not fail a
    configure (its own header, and `check-compile-cache-caveat`'s "a caveat must
    never fail a configure" row), and a daemon out of step with a launcher is the
    normal state during a rollout. A warning is the loudest level that leaves the
    build buildable, and that is the whole of the decision.
  - **The remedy names the direction**, which is #815's clause 2. `unsupported wire
    version 3; this server speaks 1..1` is accurate and stops one step short of
    *your daemon is older than your launcher* — and it cannot be fixed where it is
    written, because the sentence comes from the **daemon**
    (`CompileCacheHandler.cpp`), so an old daemon goes on sending the old wording
    forever. It is read at `_fc_cache_fastcache_cc_predicts_detail` instead.
  - **A launcher installed and not used is announced too.** A row skipped because
    its `requires` is falsy used to print nothing, which would have rebuilt #815's
    shape one row over the moment sccache became opt-in. `_fc_cache_<id>_requires_note`
    is the column; a row whose absence explains itself carries none, which is why
    `fastcache_cc` has none — an emptied `FASTCACHE_ADDR` is a documented opt-out
    (#372).
  - **Auto-install had to move with it.** `FASTCACHE_AUTO_INSTALL` fires only when
    nothing else is installed, on the reasoning that "a launcher the user installed
    is a decision already made". An installed-but-not-opted-into sccache is not that
    decision, and counting it as one would leave a machine that explicitly asked for
    auto-install with no cache at all.
  - Coverage: `ctest -R version-fallback-warning` drives the decision as a pure
    function over eleven rows (the situation needs two binaries at disagreeing wire
    versions, so it cannot be staged), and `ctest -R compile-cache-caveat` drives the
    gate through real configures in both directions — opt-in absent and sccache
    passed over even with ccache present beside it, opt-in set and sccache selected
    with the caveat still printed. Both were shown failing against the pre-#815
    module before being believed.

- **The same hazard has a second audience, and that warning cannot reach it.**
  Pointing sccache at a fastcached daemon — `SCCACHE_MEMCACHED` / `SCCACHE_REDIS`,
  which `README.md` pitches, the docs repeat and this project's own `--help` prints
  — is *definitionally* **one cache shared by every checkout and every machine
  pointed at it**, the maximal form of the entry above. Those users have their own
  build system and never configure `CompileCache.cmake`, so the `message(WARNING)`
  it emits never happens to them.

  Measured here in two compiles per compiler, which is all it takes: with sccache
  0.14.0 and MSVC 14.51 a second directory took a **cache hit from the first**, and
  its `/showIncludes` named the *first* directory's header; under g++ 14 the same
  two compiles were **0 hits and 2 misses**. `README.md` had asserted the opposite
  — that sccache's entries are "not portable between checkouts at different paths"
  — which is true of GCC and Clang and **false of the exact compilers that are
  exposed**, so the one caveat that was there read as reassurance. Silence would
  have been better than that sentence.

  `docs/snippets/sccache-backend-caveat.md` is the one wording; the MkDocs pages
  include it, and `README.md` and `--help` restate it because neither can include
  anything. `ctest -R sccache-backend-caveat` fails any file naming either variable
  without, within 40 lines of it, the three facts a reader is otherwise right to
  skip past: **which compilers**, **the mechanism** (`/showIncludes`, `/EP`), and
  **the remedy** (`fastcache-cc`). Wording is free; those three are not. Prose
  drifts exactly the way an include graph drifts — nothing fails, nothing warns,
  and the next page pitching it is written by someone who never saw the caveat on
  the other four.

## Running the launcher is not testing it

`compile-cache-e2e` and the `fastcache-cc smoke` job compile synthetic single files
and never execute what came out, so they prove the launcher **runs** and produces
**an** object — not that it is the **right** object. That gap is why #319 (a
cache-backed build segfaulting while the same commit at `-DUSE_COMPILER_CACHE=OFF`
passed) could only be found by a developer noticing a crash, and why #368 could
recur with nothing reporting it.

`scripts/launcher-replay-e2e.sh` closes it by building a real target three times —
a cache-off **control**, a **cold** build that stores, a **warm** build that
REPLAYS — and running the replayed binary's own tests. Replay is where a wrong
object comes from, so a fixture that only builds cold tests nothing.

- **Objects are compared cold-against-warm, never control-against-warm.**
  `CompileCache.cmake` disables PCH and module scanning when a launcher is active,
  so a cache-on and a cache-off build are configured differently and their objects
  legitimately differ. Cold against warm is the only apples-to-apples comparison
  available, and it is the one that matters. The control participates at the level
  it can: its suite result must agree.
- **Two guards make a green run mean anything.** The cold build must be observed
  USING the launcher (`LAUNCHER = ` in `build.ninja` — the same check the standing
  `-DUSE_COMPILER_CACHE=OFF` rule is verified by from the other side), and the warm
  build must be observed HITTING. A warm build that missed everything compiles
  correctly, passes every assertion, and has replayed nothing.
- **Injecting a wrong object needs the linker, not `touch`.** The obvious staging —
  replace the `.o`, touch it so it looks newer than its source, rebuild — does not
  work under ninja: ninja records each output's mtime in `.ninja_log`, so a replaced
  object reads as **dirty** and is **rebuilt**, silently undoing the injection and
  leaving a canary that always passes. The first version of that canary did exactly
  that, and a throwaway probe on a five-line project caught it in seconds. Take the
  link command (`ninja -C <dir> -t commands <target> | tail -1`) and run only that.
  It bypasses the dirty check and is also the truer simulation: a wrong cached
  object is one the build system has no reason to touch again.

## Code coverage

`cmake --preset clang-coverage`, build, then `--target coverage`. That target runs the
**whole** CTest suite under instrumentation and writes
`out/build/clang-coverage/coverage/`: `html/index.html` to browse, `coverage.lcov` for
Codecov, `report.txt` and `percent.txt`. CI runs that same target, so there is one code
path (`scripts/coverage.sh`) rather than two that drift.

**Clang source-based, never gcov — because of how many processes this suite is.**
`catch_discover_tests` gives each of ~2000 `TEST_CASE`s its own process, and the
script-driven tests spawn daemons and launchers besides. gcov merges counters into a
shared `.gcda` per object file as each process exits, so concurrent writers race; that
race is what every `lcov --ignore-errors mismatch,inconsistent` on the internet is
suppressing, and a suppressed error there is under-counted coverage reported as a clean
run. LLVM keys each raw profile on the binary's own module signature and merges into it
under a lock, so no suppression and no serialization is needed.

**`%8m`, never `%p`.** Measured on this tree, 2228 test processes produce **55** raw
profiles totalling 20 MB. `%p` would have written one multi-megabyte file *per process*.

**A compiler cache and coverage cannot be combined.** Coverage mapping data is embedded
in the object file and names its sources by **absolute path**, while `fastcache-cc`
exists precisely so an object built under one checkout root can serve a compile under
another. A hit replays a perfectly valid object carrying the *producer's* paths, and
llvm-cov then reports files that do not exist here — a report about somebody else's
tree, with nothing failing to say so. The preset sets `USE_COMPILER_CACHE=OFF`, and
`cmake/Coverage.cmake` refuses to configure if a launcher is set anyway, since
`-DCMAKE_CXX_COMPILER_LAUNCHER=` bypasses that option.

**`*_test.cpp` is excluded from the report, and that is load-bearing.** Tests here live
*next to* the implementation, so ~150 `*_test.cpp` files compile straight into the test
binaries. Counting them measures the tests testing themselves — thousands of near-100%
lines that move the total a long way and mean nothing. The one `ignore_regex` in
`scripts/coverage.sh` is the single source of truth for what is measured; `.github/codecov.yml`
restates it only so Codecov's view cannot silently disagree.

**Every misconfiguration is a `FATAL_ERROR`, not a warning that disables itself.** A
non-Clang compiler, Windows, an enabled sanitizer, no test suite, a missing `python3`, a
compiler-cache launcher, or an `llvm-profdata` whose major version differs from the
compiler's — the raw profile format is versioned, and a mismatch otherwise surfaces as a
complaint about the *file* rather than about the tool. A coverage build that quietly
instruments nothing still compiles, still runs the suite and still writes a report, so
every signal an author would check says it worked. Same reasoning as the sanitizer entry
above, which had already been found in exactly that state.

**And "a missing `python3`" means one that does not RUN, which is not what the obvious
lookup asks** (#568). `find_program(NAMES python3)` returns the first name match on PATH
and never executes it; `find_package(Python3 COMPONENTS Interpreter)` validates by running
it. Measured on Windows with CMake 4.3.1, one tree, one configure: `find_program` selected a **0-byte** Windows App
Execution Alias — a reparse point Windows puts on PATH that refuses to execute when no
Store package backs it — while `find_package` produced a real **171744-byte** interpreter.
**Attach the right mechanism to that second half**, because the obvious reading of it is
wrong and the wrong reading is the useful-sounding one: the working interpreter was NOT
further along the same PATH, it was not on PATH at all, and FindPython reached it through
`Python3_FIND_REGISTRY` (registry first, on Windows only). On the POSIX legs there is no
such second source — measured on CMake 3.28.3, `find_package` stops at the FIRST name
match, validates it, gives up, and does not go on to a working `python3` or `python3.12`
later on the same PATH. So what this buys is **validation, never better selection**, and
a rule claiming otherwise would be cited into a design that cannot work.

What validation is worth on its own is the whole point: reproduced end to end on Linux
with a `python3` that cannot exec, the old spelling put it in the `coverage` target's
command line and configure SUCCEEDED — the late failure the gate exists to prevent, let
through because the gate tested presence rather than function. The trade is deliberate and
has a cost worth stating: a broken shim early on PATH now refuses the configure outright
where it used to configure and fail at the end of the run, so the diagnostic must name
`-DPython3_EXECUTABLE=` or it strands an operator who has a perfectly good interpreter
installed. Guard on `Python3_Interpreter_FOUND` and never `Python3_EXECUTABLE` —
FindPython leaves the latter naming the candidate it just rejected, so the tidier spelling
restores the bug.

The rule generalises as *a tool the build will EXECUTE is located by something that
executes it*, and it has **reasoned exceptions that must not be "fixed"**. `find_package`
needs a toolchain, so `cmake/Version.cmake` cannot use one — it runs before `project()`,
and says so. Where no validating find module exists, validate by running: the two
`llvm-*` lookups in `cmake/Coverage.cmake` do, and `cmake/Packaging.cmake` and
`cmake/portable/ClangTidy.cmake` do not, which is unexamined rather than decided. The two
shell fixtures that locate python (`scripts/tidy-sweep.sh`,
`scripts/launcher-replay-e2e.sh`) use `command -v`, which does not run it either; shell
has no validating equivalent and both run only on the POSIX legs. **Nothing enforces any
of this** — it is four names in a comment, not a closed set — which is
[#607](https://github.com/LASTRADA-Software/fastcached/issues/607).

**The CI job publishes and gates on nothing.** `coverage` in `build.yml` reports the
figure to the job summary, uploads the HTML, and pushes lcov to Codecov once
`CODECOV_TOKEN` exists; both Codecov statuses are `informational`. The long-term target
is >90% and raising it is separate work — a threshold added before anyone has had the
chance to move the number only teaches everyone to ignore the signal. A failing suite
still gets its report rendered, to read while debugging, and then re-raises so no number
measured from a red build is published.

## Scoping the matrix to what a change can affect

**A `paths-ignore` filter on a workflow whose checks are REQUIRED converts "slow"
into "unmergeable".** Master here is protected by a ruleset (`default-master`,
enforcement `active`), not by classic branch protection — so the
`/branches/master/protection` endpoint answers `404 Branch not protected`, and
anything reasoning from that endpoint is reading a lie. Its required contexts are
`RequiredContexts` in `scripts/check-merge-queue-contexts.sh`, which every tool
that needs them reads — this paragraph used to enumerate them and went stale the
moment two were promoted (#408, #629), which is the *count* problem in its worse
form: **a stale list reads as complete**, so a leg checked against it is concluded
unrequired rather than merely miscounted. A workflow-level path filter stops the
workflow from triggering, so no check run is ever created for any of those, and the
pull request waits on a context that will never arrive.

A **job-level** `if:` is a different mechanism: the job is created and reports a
`skipped` conclusion, which a required check accepts. That is what
`.github/workflows/build.yml` does — one `changes` job publishes `code=true|false`
and every heavy job is gated on it.

- **The classification is `scripts/ci-scope.sh`, not a YAML glob**, so it is
  testable without a runner: `ctest -R ci-scope` runs its table against a
  throwaway git repository, in both directions. A glob living only in workflow
  YAML is a rule nobody can exercise until it is wrong in production.
- **Every way of not knowing escalates to "build everything".** An unresolvable
  ref, a failed diff, an empty diff, a path matching no row — all `code=true`,
  for the reason `tidy-sweep.sh` states about its own base ref: *we could not tell
  what changed* must never read as *nothing did*. The cost of being wrong that way
  is a matrix run nobody needed; the cost of being wrong the other way is a merge
  no job ever compiled, with every required check green.
- **`mkdocs.yml` is code and `.agent/**` is not**, which is the kind of judgement
  that has to be written down: the Documentation workflow builds the former
  `--strict`, while the rulebook is read by people and sessions rather than by any
  job. If a rule file ever generates something, it stops being documentation.
- **The cost this removes is not one matrix run per pull request.** The ruleset
  sets `strict_required_status_checks_policy: true` — branches must be up to date
  before merging — so every merge puts every other open pull request behind and
  forces a rebase and a full re-run. The matrix is therefore paid once per pull
  request *per merge that lands while it is open*, and that multiplier is the
  larger half of the argument.
- **A skipped MATRIX job never expands, so its per-leg contexts never exist.**
  This is the trap inside the fix, and it was found by probing rather than by
  reading: with a job-level `if:` on `linux` and `windows`, a docs-only run
  reported one context literally named `Linux-${{ matrix.preset }}` and another
  named `Windows-${{ matrix.preset }}` — while the four names the ruleset actually
  requires (`Linux-clang-release`, `Linux-gcc-release`, `Windows-cl-release`,
  `Windows-clangcl-release`) reported nothing at all. That is the same
  never-arrives failure as `paths-ignore`, reintroduced one level down, and every
  non-matrix job skipping correctly is what makes it easy to miss. So a matrix job
  is gated on its **steps**: the job starts, the matrix expands, each leg reports
  under its real name, and no step does any work. It costs one runner start per
  leg and buys the only thing that matters.
- **Three of those steps already carried an `if:`, and the gate silently vanished
  from them.** That is the duplicate-key trap recorded under the local gate above,
  met here for the first time: YAML keeps the last of two `if:` keys, so merge into
  `gate && (original)` rather than stacking, and scan for adjacent `if:` lines.
- **A gated job must still gate the release.** `check-release-gate` asserts
  statically, with `yq`, that every job key appears in `release.needs`; it never
  asks whether a job ran, so a job that no-ops on a docs change still counts. The
  `changes` job is itself a row there. On a tag or a push the classifier answers
  `code=true` unconditionally, so nothing is ever skipped underneath a release.

## A merge queue is a third door to the same never-arrives failure

`strict_required_status_checks_policy: true` means every merge puts every other
open branch behind, so the matrix is paid once per pull request **per merge that
lands while it is open**. Measured: 77 `build.yml` runs to land about ten pull
requests in one night, 25 of them cancelled. A merge queue is what that shape
calls for.

It is not a ruleset checkbox. **A merge queue dispatches the `merge_group` event,
and a workflow that does not listen for it produces no check run at all** — so a
required context never reports and a queued pull request does not fail, it sits
there. Same failure as `paths-ignore` above, third door. And it presents as the
feature working, right up until the first pull request enters the queue.

- **Both workflows must trigger on it, not just the obvious one.** `Require a type
  label` is required and lives in `pr-labels.yml`, which is `pull_request_target`
  — an event a queue **does not produce at all**. That is not a missing row in an
  `on:` list; the whole workflow is built around an event that is not there, for
  the security reason its header states, which must not be undone. The gate job
  therefore runs on both events and its queue leg says what it checked.
- **What a queue leg may legitimately assert is narrower than what a pull-request
  leg asserts, and it has to SAY so.** The queue branch is
  `gh-readonly-queue/master/pr-<n>-<sha>`, so the pull request can usually be
  recovered and the label re-checked for real — which catches the one thing entry
  to the queue cannot, a label removed while the entry was waiting. When the ref
  names none, that is stated and the leg passes on what entry proved. A leg that
  passes because something else already checked is fine; a leg that passes and
  does not say why is a stub, and reads exactly like a working gate.
- **A skipped job REPORTS, and a skipped required context is read as passing.**
  Measured here, on the head of the docs-only #356 (`b4777aa`): `Check C++ style`,
  `clang-tidy` and `macOS-clang-release` — all three required — each produced a
  check run with `conclusion: skipped`, and the pull request merged. The opposite
  claim is *also* true, of a different situation, which is why both get made: a
  skipped **matrix** job never expands, so its per-leg contexts never exist and
  nothing reports at all. On that same commit `Linux-*` and `Windows-*` came back
  `success`, because those are gated on their steps for exactly that reason. One
  hangs, one passes; the difference is the matrix.
- **And the converse, which is worse: a GREEN required context is not evidence that
  anything ran.** The bullet above ends at the skip. Finish the thought, because the
  two errors point the same way and an audit by conclusion is therefore wrong twice
  over. Measured on the docs-only #585 (run `33688370443`): `Linux-clang-release`,
  `Linux-gcc-release`, `Windows-cl-release`, `Windows-cl-debug` and
  `Windows-clangcl-release` all reported **`success`** in **3-9 seconds each**, with
  every step inside them skipped — `actions/checkout` included, so not one of them
  even had the tree. The whole run was about **38 runner-seconds**. Nothing was
  compiled, nothing was tested, and five required contexts went green saying so to
  nobody.

  That is the anti-hang design working exactly as intended and is not a defect: a
  matrix job must not be skipped at the job level, so `linux` and `windows` are
  gated `if: !cancelled()` with all ten of their steps gated on the scope instead.
  The defect is only ever in the READING. A skip at least looks unusual in the UI
  and invites a second glance; a green check looks precisely like the thing you
  wanted, so the wrong conclusion is the comfortable one. **The only discriminator
  is opening the job and reading its step list** — `conclusion` cannot carry this,
  in either direction, and no count of green contexts can either. Ask "did CI
  exercise this change" of the steps, never of the checks.
- **So a dependency's failure must not be allowed to skip a required gate.**
  `apply` is skipped inside a queue — there is no pull request to label — and a
  skipped dependency skips its dependents by default, which by the above is not a
  stall but a **stub**: green, and indistinguishable from a working gate. The same
  holds when `apply` *fails*: a condition excluding that result skips the gate, the
  skip reads green, and a pull request with no `type/` label becomes mergeable
  because the labeler broke. `Apply derivable labels` is not itself required, so
  nothing else closes it. The gate does not need `apply` to have succeeded — it
  reads the labels **fresh** from the API, so a human-applied label is readable
  either way — so the condition is `if: ${{ !cancelled() }}` and the gate checks
  for real. `!cancelled()` and not `always()`, which runs even when the run is
  being cancelled; any status function already overrides the `needs:` success
  requirement.
- **Check the concurrency key.** `pr-labels-${{ github.event.pull_request.number }}`
  collapses to the constant `pr-labels-` on `merge_group`, and with
  `cancel-in-progress` each queue entry then cancels the one before it. A
  cancelled required context is not a reported one. `build.yml`'s key is
  `github.ref`, which inside a queue is the temporary branch and is already unique
  per entry — but that is now load-bearing rather than incidental.
- **State the event in the scope classifier rather than letting it fall through.**
  `merge_group` reached `code=true` through the `changes` job's non-pull-request
  default. Correct, by accident, with nothing recording that anything depended on
  it. A merge candidate builds everything **by design**: it could be scoped, since
  `merge_group.base_sha...head_sha` is a real diff, but a batched group is built on
  top of the entries ahead of it, and the saving is one matrix per *merged* pull
  request — not the re-runs this queue exists to remove.
- **A failing queue entry is ejected, not merged, and the branch is fine.** GitHub
  removes the pull request from the queue, comments on it naming the failed check,
  and deletes the temporary branch; the pull request's own head is untouched and it
  can be fixed and re-queued. Nothing needs cleaning up by hand. The one thing to
  know is that the failure is reported on the *queue* run, so a red check on a
  merged-looking pull request wants the `merge_group` run, not the `pull_request`
  one.
- **Adding a JOB to `build.yml` for this would drag the release behind it**, since
  `check-release-gate` asserts every job there appears in `release.needs`. So this
  is triggers and existing jobs only — which is also why `pr-labels.yml` is a
  separate workflow in the first place, as its own header records.
- **And the workflow must not invert its own script's principle one level up.**
  `ci-scope.sh` says every way of not knowing escalates to `code=true`. The
  workflow reading it said `if: needs.changes.outputs.code == 'true'`, so a
  `changes` job that **failed** published no output, the comparison was false,
  sixteen jobs were skipped — and by the measurement above a skipped required
  context reads as passing, while `changes` is not itself required. A green,
  mergeable pull request that nothing had compiled: exactly what `ci-scope.sh`'s
  own header warns about, arriving through the workflow rather than the script.
  Every condition is now `!= 'false'`, so *did not answer* means build-everything,
  and every job that consults the classifier carries `!cancelled()` so a failed
  dependency cannot skip it before its condition is read.

  **The repository had been relying on the matrix trap to save it here, and no
  longer is.** With `linux` and `windows` step-gated and carrying no job-level
  condition, a failed `changes` skipped them too — and being matrices their
  per-leg contexts never existed, so the pull request *hung* instead of merging
  green. That accident was the only thing standing between a dead classifier and
  a merged, uncompiled change. Both matrix jobs now carry `if: ${{ !cancelled() }}`,
  which is the one job-level condition on a matrix job that is safe: it is false
  only while the run is being cancelled, so the matrix **always expands and every
  leg reports under its real name** — the exact opposite of relying on
  non-expansion. **The dependency on the accident is gone, not reduced.** Do not
  reintroduce a job-level `if:` on either believing the accident is still there to
  catch you — it is not, and it never should have been load-bearing.

  `ctest -R gated-jobs-fail-safe` (`scripts/check-gated-jobs.sh`) asserts both
  rules, derived from the workflow rather than tabulated, so the seventeenth job
  cannot be added wrong. `release` needs `changes` too and is excluded by
  construction: it never reads the classifier's output. What it proves is the
  **shape** of the conditions — the `changes` job cannot be made to fail on
  demand, so the behaviour it relies on is the `b4777aa` measurement above rather
  than a demonstration.

`ctest -R merge-queue-contexts` (`scripts/check-merge-queue-contexts.sh`) asserts
all of it from the workflow files, because the property cannot be demonstrated
before the fact: a `merge_group` event only exists once a queue is enabled, and a
queue that stalls is the outcome being avoided. It derives which job produces
which context rather than tabulating it — a second hand-written list is not a
cross-check, it is a second thing to be wrong — and the only copied datum is the
required-context list itself, whose provenance and the `gh api` call that reads
the live one are in the script header.

## A gate that does not report reads as a gate that passed

The three doors above are all about a required context that never arrives. These
two are the same disease with the *required* clause removed, and they were found
together because they are one shape: **a failure nobody is shown is
indistinguishable from a success.**

### A non-required job failing in a merge group is reported nowhere (#684)

The queue holds on the required contexts and on nothing else, which is
correct — the packaging and coverage jobs are deliberately unrequired because
they are slow and because a packaging failure should not block ordinary work.
What is *not* correct is that such a failure reaches no surface at all: the pull
request page is green (its own run passed on the head), the queue reports success
(every required context passed), master is green afterwards (the same job passes
on the merge commit), and the only trace is an `event=merge_group` run that no
part of the normal flow ever lists.

**Measured, over every failing `merge_group` `Build` run the API still held on
2026-09-04 — six of them:**

<!-- table-total: of them=rows -->
| run | failing job | required? | pull request |
|---|---|---|---|
| 33783939363 | `Code coverage` | no | #689 merged |
| 33782559943 | `macOS-clang-release` | **yes** | #686 ejected, fixed, merged |
| 33760836218 | `Windows-cl-debug` | no | #667 merged |
| 33749317968 | `Package (macOS .pkg)` | no | #669 merged |
| 33665118078 | `Package (macOS .pkg)` | no | #546 merged |
| 33650121718 | `Windows-cl-debug` | no | #539 merged |

Five of six, four distinct jobs, five pull requests merged with nobody told. The
one required failure behaved exactly as designed, and that contrast is the whole
argument: this is a **reporting** gap, not a gating one, and closing it must not
turn those jobs into gates by the back door.

- **The unit is the CONTEXT, never the job key.** `Windows-cl-debug` is a leg of
  the same matrix job as `Windows-cl-release`, which is required. Two of the six
  rows are that case, so a per-job classification would have been wrong a third of
  the time on the very data it was derived from.
- **Not a job in `build.yml`**, because `check-release-gate` requires every job
  there to appear in `release.needs` and a notifier job is *skipped on a tag push,
  always* — which skips the release with it. That is the never-arrives failure
  aimed at the release, and it is why the clang-tidy job reports through a step.
  **Not a step in each unrequired job** either: a step sees only its own job, so
  it is ten copies of one decision, ten grants of `issues: write`, and it still
  cannot express the per-leg distinction above. So `workflow_run` — one workflow,
  every job's conclusion at once, running *after* the queue concluded, producing
  no status anything waits on.
- **`workflow_run` only ever runs the copy on the DEFAULT branch**, so nothing on
  a pull request exercises it and its first run anywhere is after it merges. The
  answer is the standing one: split the DECISION out
  (`scripts/ci-merge-group-report.sh`), drive it exhaustively against **captured
  real records** (`ctest -R merge-group-report-selftest`), keep acquisition thin,
  and assert the wiring statically (`ctest -R merge-group-report`).
- **The one claim that could not be measured is made self-revealing rather than
  quietly bet on.** That `workflow_run` fires for a run whose own event was
  `merge_group` is *reasoned* from GitHub's documentation, not measured, and it
  cannot be measured from a branch. So the trigger carries **no `branches:`
  filter**: the notifier fires on ordinary pull-request runs too and prints
  `event is 'pull_request', not merge_group`. From the first run after it merges,
  *it never fires* and *it fires and has nothing to say* are two visible states
  instead of one silence. A `gh-readonly-queue/**` filter would save a runner
  start and buy back exactly that ambiguity, so `check-merge-group-report` refuses
  one.
- The required-context list is READ out of `check-merge-queue-contexts.sh`. With
  an empty list every failure looks unrequired, so an empty read is refused rather
  than treated as *nothing is required*.
- **Neither the SET nor its COUNT is written in prose — this bullet is where that is
  recorded, and every other mention of it points here.** Every consumer reads
  `RequiredContexts`; the check prints `all <n> required contexts ...` on every run.
  Both were restated anyway, and promoting `clang-asan-ubsan` and `clang-tsan`
  ([#408](https://github.com/LASTRADA-Software/fastcached/issues/408),
  [#629](https://github.com/LASTRADA-Software/fastcached/issues/629)) meant editing all
  thirteen sites by hand with nothing to catch a miss: **eleven sentences across nine
  files** restating the count (`.github/workflows/build.yml`,
  `.github/workflows/merge-group-report.yml` twice, `scripts/ci-merge-group-report.sh`,
  `scripts/check-merge-queue-contexts.sh`'s own bash-3.2 guard comment,
  `src/tests/CMakeLists.txt`, `AGENT.md`, this file, `.agent/rules/testing.md`,
  `.agent/guides/team-run.md` twice), and **two more enumerating the whole set**
  (`build.yml`'s `changes` job and this file's own `paths-ignore` section). Eleven
  sentences in nine files, because two files carry it twice.

  **The first count of this said ten, and the miss is the interesting part.** The
  eleventh sentence was inside an anecdote about a past incident, so it was classified
  as a RECORD and left alone — and its verb was present tense: *"the required set **has**
  eleven members"*. A record of what was measured then and a claim about now are
  different things, and a paragraph can be the first while one of its sentences is the
  second. Sort by the VERB, not by the paragraph's subject.
  `git diff origin/master...HEAD | grep '^-' | grep -iE 'eleven|\b11\b'` returns eleven
  lines, one per sentence — but that is a count of LINES and agrees only because no
  sentence carries the word twice, so read it as corroboration and not as the census.
  One of the eleven sat **four lines below**
  `merge-group-report.yml`'s own header stating *a figure others will cite lives in ONE
  place they point at* — a file obeying a rule in one paragraph and breaking it in the
  next, which is how this shape survives review.

  **A stale LIST is worse than a stale count, and no count-shaped search finds one.**
  It reads as complete, so a leg checked against either enumeration would have been
  concluded *unrequired* rather than merely miscounted — and an enumeration carries no
  number, so the census that finds every restatement finds neither of them.

  **It is not guarded by a scan, and that is a decision rather than an omission.** The
  obvious guard — refuse a number adjacent to *required context* unless it matches
  `${#RequiredContexts[@]}` — was measured against the tree and matches six historical
  records that are not cardinality claims at all (*three required contexts came back
  `skipped`*, *five required contexts went green*, *ten required contexts went
  unwatched*, and #557's *ABSENT for all eleven*). Marking those as counts would
  misdescribe them, and narrowing the pattern to the cardinality phrasings means
  guessing at English determiners — which is [#495](https://github.com/LASTRADA-Software/fastcached/issues/495)'s
  and the `pgrep -f` lesson's warning that a pattern is broader than its author reads
  it. A guard whose false positives outnumber its true ones teaches people to work
  around it. [#830](https://github.com/LASTRADA-Software/fastcached/issues/830) carries
  the analysis; the enforcement here is that **the count is not written down**, so there
  is nothing for a scan to check.

### Doc-subject checks were skipped on doc-only changes (#687)

`ci-scope.sh` answers `code=false` for a `docs/**`, `.agent/**` or `*.md` change
and every heavy job is gated on it — right for a compiler, and backwards for the
handful of checks whose SUBJECT is prose. **Prose drifts by being edited, so a
prose-only edit is precisely the change those checks never saw.** Both live
findings #462 fixed came from doc-side hand edits.

Reproduced end to end on one commit: a four-line addition to `docs/how-it-works.md`
recommending the sccache-over-fastcached setup — naming one of the two backend
environment variables `check-sccache-backend-caveat.cmake` scans for — with no
caveat after it. `ci-scope.sh` classifies that commit `code=false`, so on master
today no `ctest` runs at all; `doc-subject-checks.sh` on the same tree exits **1**
and names `sccache-backend-caveat`.

**And then the same guard caught this very paragraph.** Written with the variable
spelled out, it *was* a page naming the marker with no caveat within forty lines,
so `ctest -L hygiene` went red on an edit that touched only `.agent/**` and
`AGENT.md` — which is the ticket's whole thesis arriving unprompted, in the commit
that fixes it, on the one class of change that used to run no check at all. The
fix is to describe the marker rather than spell it: this paragraph reports a
reproduction, it does not recommend a configuration, and `check-sccache-backend-caveat.cmake`
cannot tell those apart from a text scan — which is exactly why its exemption
table takes a per-row reason. Adding a row for the rulebook would have been the
wrong move: the caveat rule is right and the prose was wrong.

- **Reporting is not gating, and this needs the second.** A doc check in an
  unrequired job is #684 one file over. So the step lives in `check-clang-format`
  — whose `name:` is the required context `Check C++ style` — restructured to the
  shape `linux`/`windows`/`macos` already use: job-level `if: ${{ !cancelled() }}`,
  the scope gate moved onto the clang-format steps, the doc-subject step ungated.
  What changes for a docs-only pull request is that this context now reports
  success or FAILURE where it used to report `skipped`, and a skipped required
  context is read as passing. The `name:` is a wire constant; renaming it renames
  a required context.
- **No new job**, for `check-release-gate`'s reason above.
- **A check that reads BOTH SIDES is the answer when a diagnostic is unreachable
  by unit test.** `docs/operations/corrupt-store.md` quotes startup diagnostics
  verbatim and exactly one of them was asserted anywhere (#633) — because the
  daemon's `StorageOpenFailure` sits in an ANONYMOUS NAMESPACE in `main.cpp`,
  behind `add_executable(fastcached main.cpp)` with no test target, and the node's
  sentence is composed in a third file in a different binary. Asserting them by
  unit test needs the app restructured; a `docs-subject` check reading the page
  AND the composing sources needs no linkage at all, and covers both binaries.
  The page's usefulness *is* that an operator recognises their own console output
  in it, so the thing to pin is that the source can still compose what the page
  shows.
  - **Assert PRESENCE in the composing source, never equality with a formatted
    line** — the latter is a change-detector that fails on every wording tweak,
    which is what #633 exists to avoid. The shard sentence's field-level
    assertion is the `'{}'` that carries the path.
  - **The exception proves it rather than breaking it**: for the
    `StorageError(code={} system={} context={})` rendering the WHOLE format string
    is pinned, because that rendering *is* the verbatim quote. A change-detector
    is exactly right where the literal is what moved onto the page.
  - **A sentence composed in three places needs a per-FRAGMENT table, not a
    per-sentence one.** The node's refusal is `--cache-dir {}` and
    `cannot open {}` in `CacheTier.cpp` plus `refusing to start` in its own
    `main.cpp`; a check reading only the tier misses the suffix and reports clean.
  - And the set is derived from the PAGE, so **a quoted diagnostic no row covers
    is REFUSED**, never passed over. An uncovered quote is precisely the silent
    gap the check exists to close, and a table that quietly ignores one is the
    check vouching for something it never read.
- **The set is the `docs-subject` ctest LABEL**, read out of
  `src/tests/CMakeLists.txt` along with each check's `-D` arguments, its
  `SKIP_REGULAR_EXPRESSION` and the one `FASTCACHED_SCRIPT_CHECK_FAILED` spelling.
  Nothing is restated, so a check that grows an argument cannot silently start
  running with the wrong one.
- **Every way of not being able to run is a REFUSAL, never a skip**: a label
  matching nothing (#687's own acceptance clause), a command still holding a
  build-tree variable, a missing script, a missing fail-regex, and a run in which
  every check skipped. That last one is *absence of the negative is not the
  positive* — nothing failed because nothing ran.
  It has been watched refuse on the real tree: `sccache-backend-caveat` was
  labelled while `-DFASTCACHED_SCAN_BUDGET_SECONDS=${FastCachedSccacheCaveatBudgetSeconds}`
  could not be resolved, and it was named rather than skipped. Resolving
  file-local `set()` variables is what fixed it, and it keeps that budget one
  value serving both the argument and the ctest `TIMEOUT`.
- **`compile-cache-caveat` is deliberately NOT labelled** and the gap is stated:
  it takes five build-tree facts and configures a throwaway project (600 s
  timeout). The refusal above is what stops somebody closing the gap by labelling
  it and getting a silent skip.
- `ctest -R gated-jobs-fail-safe` gained a third rule over both halves — the step
  is ungated, and its job produces a required context — and, for the first time,
  a `--self-test` that drives all three rules against generated workflows. They
  had only ever been seen to pass.

### Seven ways an instrument reported on something other than its subject

Every one of these was written by somebody who had just read this file, and every
one produced a green or a red that was about the tool rather than the tree. They
are recorded together because the shape is one shape.

- **A COMMENT is not a call site — twice, in two different checks.**
  `check-gated-jobs.sh` matched its own explanatory comment naming
  `doc-subject-checks.sh` and attributed it to whichever step block was open,
  reporting the same step twice; `check-merge-group-report.sh`'s header explains
  that it refuses `always()` and `cancel-in-progress`, and both rules fired
  against a correct workflow. **A rule satisfied — or refused — by prose is a rule
  that passes with the code deleted.** Strip full-line comments (`^[[:space:]]*#`)
  and never a trailing `#`, which in YAML needs preceding whitespace to start a
  comment. Both directions are self-tested: the baseline workflow deliberately
  carries a comment naming every forbidden spelling, and a second baseline carries
  no comments at all.
- **`bash <path>`, never a bare path — in a self-test AND in a workflow, and the
  file mode is not the whole reason.** It fired twice in one branch, in the two
  places whose failures look nothing alike.

  In a self-test, `check-gated-jobs.sh --self-test` re-invoked itself as a bare
  `"$0"`. These check scripts were mode **644** — ctest runs them as `bash <path>`
  and nothing needed the executable bit — so it exited **126, Permission denied**,
  having run no check at all, and every `want-fail` case passed because the SHELL
  refused rather than because the rule fired: **eight cases, entirely green,
  testing nothing.** Caught by the one case that expects a PASS, which is the
  argument for a baseline case rather than only negative ones.
  [#720](https://github.com/LASTRADA-Software/fastcached/issues/720) is the mode
  across fourteen scripts;
  [#723](https://github.com/LASTRADA-Software/fastcached/issues/723) is the rule
  that survives a chmod.

  In a workflow it is louder and reaches further. `build.yml`'s new doc-subject
  step shipped as a bare `run: scripts/doc-subject-checks.sh` and CI reported
  **`Check C++ style` — a REQUIRED context — as FAILED**, on step 3 of 5 with both
  clang-format steps skipped behind it. Reproduced locally against the same mode
  and the same invocation: exit **126**. It was found by reading `git ls-files -s`
  against the call site before the run finished, not by the run — which is the
  only way to find it, since a local run types `bash` in front of the script by
  habit and can never reproduce it.

  **The mode is the smaller half.** Both scripts are 755 now, so the bare form
  would work — and every call site names its interpreter anyway, because a call
  that fails to START fails for reasons a chmod does not cover: a moved file, a
  `noexec` mount, a lost mode bit. Inside a `want-fail` assertion any of those is
  indistinguishable from the rule firing, which is the eight-green-cases failure
  again with a different cause. Name the interpreter regardless.
- **`IFS=$'\t' read -r a b c d` does not read tab-separated fields.** Tab is one
  of bash's three IFS *whitespace* characters, so a run of tabs collapses to one
  delimiter and leading ones are dropped — a record with an empty field silently
  SHIFTS every field after it. The empty field is exactly the case that matters
  here, since `gh` renders an unfinished job's conclusion as the empty string.
  Split by hand (`${line%%$'\t'*}`) and count the delimiters first.
- **A fixture built on `message(FATAL_ERROR)` cannot test the rule that verdicts
  are read from OUTPUT.** It exits **1**, so the exit status alone is sufficient
  and deleting the output half of the verdict left the self-test GREEN. The mode
  under test was not the mode in use. There is now a fixture per arm: one that
  prints the failure text and exits 0, one that exits non-zero with clean output.

  This bullet said the exit code was **0** on 3.28 and **1** on 4.3, which made
  the defect sound like a property of the reviewer's host. It is not — the exit is
  1 on every version measured (#565) — and the correction makes the finding
  **stronger**, not weaker: the fixture was insufficient on every machine, always,
  rather than only on a modern one. It is left recorded because a wrong premise
  under a right conclusion is the hardest kind to notice, and this bullet is the
  same defect as the section above it, one level down.

- **CMake WRAPS its diagnostic messages, so a phrase you grep for may exist in the
  output and in no single LINE of it.** A mutation harness reported all three of its
  arms as `SELFTEST STAYED GREEN -- this mutation is NOT covered` while the self-test
  was in fact RED with four cases named. The phrase it matched on,
  `did not behave as claimed`, straddled the wrap point. Measured:

  <!-- table-total: none -->
  | message type | matches for a phrase crossing column 74 |
  |---|---|
  | `message(STATUS)` | **1** — does not wrap |
  | `message(WARNING)` | **0** |
  | `message(FATAL_ERROR)` | **0** |
  | any of them, whitespace flattened first | **1** |

  **The `STATUS` row is the trap inside the trap, and there are TWO ways to write the
  negative test wrong.** It does not wrap, so a test written with `STATUS` reproduces
  nothing and reads as a refutation of the whole claim; and in a file emitting BOTH, the
  unwrapped `STATUS` copy satisfies the grep and masks the wrapped one — which is what
  the first attempt here did, returning 1 match. Both failure modes read as a
  refutation, so a reader who tests only the first still concludes the rule is false.
  Same structure as *a green probe of the wrong shape is not a refutation*, in a
  different tool.

  **And this is a property of every verdict this repository reads, not a trap somebody
  might hit.** A `cmake -P` check's verdict travels through the DIAGNOSTIC channel by
  construction — its registration is read by `FAIL_REGULAR_EXPRESSION`, which must also
  hear a check that merely WARNS, and `message(WARNING)` exits 0 on every CMake. (This
  said *"`message(FATAL_ERROR)` prints and exits 0 on 3.28"*; that does not reproduce —
  exit **1** on 3.22.6 through 4.3.0, #565 — and the conclusion never needed it, since
  what makes the channel load-bearing is the warning half, not the exit code.)

  Measured: **37 of 37** `scripts/check-*.cmake` carry `message(FATAL_ERROR)` — on the
  branch that added this entry; **35 of 35** on the master it branched from, the two
  extra being that change's own; and **43 of 44** after #565 landed beside it. Every
  figure is right for the tree it was taken on, which is the census rule arriving a
  third time: not a different pattern here, the *same* pattern on a different tree.
  State which tree, or the next person measures 35 and concludes the rule overstates
  itself.

  **And the 44th is the one to read.** `scripts/check-fatal-error-exit-selftest.cmake`
  carries no `message(FATAL_ERROR)` at all — it reports through `message("CMake Error:
  …")`, which is what several checks here already do. So the census was a PROXY for
  "reports through the diagnostic channel" and the proxy has now parted company with the
  property: the conclusion above still holds for all 44, and the count that stood in for
  it does not. A census whose pattern is a proxy states which one it measured.
  Every one of them is a check whose verdict cannot be read by a substring match that
  crosses column 74. So flatten (`tr '\n' ' ' | tr -s ' '`)
  before matching any verdict text, or match a phrase short enough that it cannot
  straddle the wrap.

  This is not any of the six above. Those are wrong buckets, wrong populations, a
  substring of a count, a comment matching itself. **This one is: the text you are
  matching does not exist in the form you are matching it in, and the tool that
  printed it is what changed the form.** The subject was fine and the reading was
  fabricated by the formatter in between.

  The tell is the one worth keeping: **three arms agreeing perfectly is what a broken
  instrument looks like as well as what a real pattern looks like.** Refusing "all
  three uncovered" and printing the raw output is what found it — the same move as
  treating two legs disagreeing at 3220/3216 as evidence about the instrument rather
  than about the tree.
- **A self-test that stops early must not look like one that judged something.**
  A generator ending in `[[ ... ]] && echo` returns 1 when the condition is false;
  under `set -e` a plain call to it took the whole self-test down after eight
  cases — exit 1, no case named, indistinguishable from a real failure. The
  self-tests now print the number of cases they ran, so a truncated run is
  visibly truncated. The count is printed rather than compared against a number
  restated in the file: a second copy of the expected total is a second thing to
  be wrong.

### A counterfactual must fail through the rule, not merely fail

Its own section rather than a sixth bullet above, and the reason is the section
above's own heading: those are **six ways in ONE branch**, written by people who
had just read this file. This one is from a different branch and a different
ticket, so filing it there would have made a counted, scoped claim false --
silently, because the heading sits outside any diff window that shows the
insertion. A clean textual merge cannot see that, which is the same failure this
file records against the store lane's insert.

**A counterfactual must fail THROUGH the rule, not merely fail — and a deletion
is the easiest one to write and the likeliest to be invalid.** Removing the branch
under test is the obvious way to show a guard bites, and it changes two things at
once: the behaviour, and whether the tree still compiles. `SecretCameFromConfigFile`'s
provenance branch is the whole use of its `cli` parameter, so deleting it left the
parameter unused, `-Werror` refused the build, and the arm went red for a
COMPILATION reason while claiming to demonstrate the rule
([#384](https://github.com/LASTRADA-Software/fastcached/issues/384)). Nothing in the
red output says which of the two it was.

**This is the mirror of the `want-fail`-satisfied-by-exit-126 rule above**, and both
polarities are now on record: there an arm PASSED for the wrong reason (the shell
refused to start the script), here an arm FAILED for the wrong reason. One missing
state underneath both — a run that did not happen is neither a pass nor a failure —
which is why the harness that caught this one is the harness that reports
**INCONCLUSIVE** rather than scoring a build failure as red
([#747](https://github.com/LASTRADA-Software/fastcached/issues/747) is that state
generally; this is it existing in one script and earning its keep).

So a counterfactual is a **plausible WEAKENING** rather than an amputation: for the
provenance gate, one that still consults the bit and only excuses an already-empty
secret — a mistake somebody would actually make, which compiles, and which can fail
only through the rule. Where an amputation genuinely is the right shape, the harness
has to be able to say the build broke; a script that reports "red" for both is an
instrument that cannot tell you what it measured.

### An insert is judged by what its CONTAINER claims, not by its neighbours

**Adjacency is not the test, and checking it is not enough.** A rule inserted into
this file was read against both junctions -- the paragraph above it, which it
explicitly references, and the one below, which is independent. Both read
correctly. The insert was still wrong, because the sentence it falsified was
**neither neighbour**: it was the heading of the section it had joined,
*"Six ways an instrument reported on something other than its subject, in one
branch"*.

Two claims broken at once, and they are different KINDS of claim:

- **A count.** "Six ways" is arithmetic over the section's members. A new member
  changes it.
- **A scope.** "in one branch", and the opening *"Every one of these was written by
  somebody who had just read this file"*. The inserted rule came from
  [#384](https://github.com/LASTRADA-Software/fastcached/issues/384) -- a different
  branch, a different ticket -- so it was never one of "these", however well it fit
  the theme.

Neither sentence is anywhere near the hunk. **No diff shows it and no merge
algorithm can**: `git` reported a clean rebase, correctly, because the hunks were
disjoint. Disjoint hunks are a statement about text, and a document makes claims
about its own structure that text adjacency cannot see. This is the store lane's
"a clean textual merge can still leave a neighbouring sentence false", one level
out: not the neighbouring sentence, the ENCLOSING one.

So the question to ask of an insert is not *"do the adjacent paragraphs still
read"* but:

> **What does the enclosing structure claim about its members, and is my member one
> of them?**

The shapes that make that question bite are worth naming, because they are easy to
read past: a **counted heading** ("Six ways", "The three doors above"), a **scoped
intro** ("in one branch", "all found together", "every one of these"), a
**demonstrative** ("these two are the same disease"), and a **back-reference** from
a later section that counts what came before. Where the answer is "my member is not
one of them", the fix is a section of its own rather than a smaller edit to the
claim -- the claim is usually load-bearing prose somebody measured.

**The person who produced this defect was the person enforcing the rule on other
lanes that same day**, and had checked the junctions deliberately rather than
skipping the check. That is the evidence, and it is why this is a rule rather than
an anecdote: knowing about stale neighbouring sentences, looking for them, and
finding none is entirely consistent with having broken the container's claim.

**Worked once, here, on this very section.** The `##` heading these subsections sit
under opens *"These two are the same disease…"*, and there are now five of them —
which looks like the same defect until the question is actually asked. It is not:
"these two" names the two TICKETS the section was written about (#684 and #687),
and master already carried a third subsection under it before any of this. The
claim is scoped to two findings rather than to the subsection list, so a further
subsection does not falsify it. That analysis is recorded because the next person
will otherwise redo it — and because *asking and finding the claim intact* is the
outcome this rule expects most of the time. A rule whose answer is always "you
broke something" is one people stop asking.

**A counted heading is itself the weaker half of this**, and the BUILD lane reached
it from the other side: *"Six ways"* counts **instances, not bullets** -- five
bullets, two of which describe two instances each -- and the unit is nowhere
stated, so the number is unfalsifiable by reading and grows wrong as the section
does. Its proposal is to drop the number from that heading. The two findings are
one finding: this rule says an insert must check the container's claim, and that
one says a container should avoid making a claim nobody can check. Neither
supersedes the other, and the BUILD lane's branch touches this same file.

### A counterfactual must be RE-RUN after a refactor -- a refactor can move what it was catching

**A counterfactual establishes that a guard bites the code as it is written. Rewrite
the code and it establishes nothing, even when it still passes** -- and the way it
fails is to keep passing while testing something that is no longer the risky part.

Measured on #658. The decision "does this rejection reason predict the hazard the
winning launcher carries" started as a hardcoded predicate inside a function, and
its counterfactual was to widen that predicate: doing so warned on three rejection
reasons that must stay silent, and the check went red. Good guard.

The `/simplify` pass then made it **data-driven**, which was the right call for an
otherwise strictly table-driven file: the pattern became a `predicts` column on the
candidate row, and the decision became winner-agnostic. Every row of the check
stayed green, as it should have -- the behaviour was unchanged.

Then the same counterfactual was re-run against the new shape. **Widening the
column to `.` left every row green.** The check drove the pure function with a
pattern of its OWN and never read what the module declared, so the column -- the
thing a maintainer would actually get wrong -- was covered by nothing at all.

**The generalisation had relocated the untested part, and looked like an
improvement while doing it.** That is the clause that makes this a rule rather than
an anecdote: the refactor was correct, the tests were green, the design was better
by every stated principle in this file, and the guard had quietly stopped guarding.
Nothing in the diff, the review or the suite said so. Only re-running the
counterfactual did.

The repair is the same idiom as everywhere else here -- **read the literal from the
file that owns it, never restate it.** The check now reads the `predicts` column out
of `cmake/portable/CompileCache.cmake` and `UnsupportedVersion`'s `.name` out of
`src/FastCache/Protocol/CompileCacheWire.hpp` and asserts they agree, which also
closed a pre-existing coupling nobody had connected: the launcher reports the wire
name, and a rename there would have disarmed the warning with every test green.

So:

> **When a refactor moves the thing a counterfactual was aimed at, the
> counterfactual moves with it. Re-run it against the new shape and watch it fail,
> or it is now asserting something you did not intend to assert.**

Two things this is NOT. It is not an argument against generalising -- the column is
the better design and stays. And it is not the "deriving is necessary and not
sufficient" rule above wearing a different hat: that one is about a guard that was
never sufficient, this one is about a guard that WAS sufficient and stopped being,
without anybody touching it. A green suite distinguishes neither.

## A branch behind master is unverified, and only a build says otherwise

A pull request's CI ran against the master it was branched from. Every green check
on it is a statement about *that* tree, and it stays a statement about that tree
however many times it is re-read. When master has moved, the only thing that
restores the claim is building the branch on top of what master is now.

Two ways of judging that risk without building are in use, and both are wrong.

**The amount a branch is behind is not a measure of the risk.** It is a measure of
elapsed time, and the defect is a semantic collision that either exists or does not.
One commit can change a signature every branch calls; a hundred can be documentation.
A branch that is two behind and a branch that is twenty behind are equally unverified,
and ranking a queue by that number sorts it by nothing.

**File overlap is evidence only in the direction that says there IS a hazard.**
Two branches touching one file will conflict, or will merge into something neither
author read -- worth knowing. But **the absence of overlap is evidence of nothing**,
and reading it as safety is how this gets missed.
[#520](https://github.com/LASTRADA-Software/fastcached/pull/520) and
[#525](https://github.com/LASTRADA-Software/fastcached/pull/525) share no file at
all: #520 changed `IConnector::Connect`'s third parameter from
`std::chrono::milliseconds connectTimeout` to `DialOptions options`, across
`IConnector` and every connector implementing it; #525 adds callers of `Connect` in
`FrameEndpoint`, a file #520 never touches. Both pairwise CI runs were green, and
both were green *correctly* -- each was built against a master without the other.

Neither branch has ever gone red, and that is not a contradiction: it is the point.
The break exists only in the combination, and the combination is the one tree nothing
built. A red run cannot warn about a tree that was never compiled, so the absence of
one here carries no information at all.

A compiler finds this in seconds and no amount of reading finds it: the hazard is a
name shared between two changes, not a line shared between two diffs, and a diff
cannot show a name it does not mention. Which is also why cost is not the argument
against doing it -- the N lane's worktree for
[#292](https://github.com/LASTRADA-Software/fastcached/issues/292) was nine commits
behind, and rebasing and rebuilding it took about two minutes and came back clean.
That is what the check costs when it passes -- and passing is what it usually does,
which is precisely why skipping it feels free.

So the sequencing rule is that a branch is rebased and rebuilt before it merges, not
inspected; `gh pr update-branch --rebase` is what converges a queue. And where two
branches are in flight at once, the second one's green is provisional until it has
been built on the first.

## A rulebook entry that has gone false instructs the next person

Every `## Open work` entry in this directory names an issue that is still open, and
`ctest -R rulebook-open-work` is what says so. Seven of thirty-nine did not when the
check was written (#619) -- and the cost is not the minute a reader spends on a
finished residual. It is the shape #395 found: an entry asserting that a field's
borrow *"cannot be tested"*, with a plausible short-string-optimisation argument
attached, in a file `AGENT.md` routes every session to before they touch `Net/` or
`Protocol/`. The rulebook was telling the next person **not to attempt** the guard,
in the register of a current argument rather than a stale one. An ASan trace
disproved it directly: allocated, freed, read, three lines apart.

Four things about that check are load-bearing, and three of them are the difference
between a check and a check-shaped thing that passes:

- **An entry is the bullet's LEADING reference; a citation in its prose is not one.**
  #619's own measurement named `#195` twice as a stale entry. Both are citations
  inside *another* bullet -- "#200 … since #195 that banner is the compiler's
  identity" -- naming the landed change that CREATED the residual, and both are
  correct as written. A check built to that description invents findings on a correct
  rulebook, somebody edits a correct entry to satisfy it, and it gets disabled for
  noise. What makes the narrow reading safe is the refusal beside it: a top-level
  bullet in such a section that opens with no issue link is `unparsed-entry`, so a
  reformatted bullet cannot quietly leave the scanned set.
- **The object's KIND is asserted, not only its state.** `gh issue view <n>` falls
  back to pull requests and so does `repos/{owner}/{repo}/issues/N`. The failure is
  asymmetric: a MERGED pull request answers `closed`, fails a `state == open` test,
  and *looks like the check working* -- while an OPEN one answers `open` and passes
  while naming no issue at all. Resolving to the wrong kind of object is a way of not
  having resolved it.
- **There are FOUR outcomes: pass, stale, bad-reference, could-not-run.** Measured:
  a nonexistent issue, a repository that is not there, and a bad token all exit 1
  with an empty result, and **two of those three are faults in the checker's own
  invocation**. Reading `exit != 0` as *this entry names a dead issue* makes the
  check blame its subject for its own fault -- it invents a finding, and the repair
  is somebody editing a correct entry. The verdict comes from the HTTP status, and
  `gh api rate_limit` is asked first as a liveness anchor, so a broken checker is
  told from a broken rulebook before either could be blamed. Without that anchor one
  typo in a repository slug is a 404 on every entry: thirty-nine findings from one
  mistake, each of them saying the wrong thing.
- **A self-test whose only negative case is a closed issue passes under all three of
  those bugs.** `rulebook-open-work-selftest` drives a case per outcome against a
  stub `gh`, plus the open-PR and merged-PR cases, plus a control that must PASS and
  a citation-of-a-closed-issue case that must also pass.

And the markdown side is where a narrow pattern hides, which is the same defect one
layer down. Four shapes were measured passing a closed issue or refusing a correct
file before they were fixed, none of them exotic: a `*` or `+` bullet, or a
one-space-indented `-`, was not a *bullet* at all — so it became no entry AND reached
no refusal, and simply vanished; a fenced markdown SAMPLE containing `## Open work`
opened a real section, so the check reported `stale` about a documentation example
and told the reader to delete it (`README.md` carries such a sample); a fenced repro
block inside a real section was refused as `unparsed-entry`; and a heading test of
`^#` rather than `^#+ ` refused wrapped prose beginning `#340; …`, of which this
rulebook has four lines. An emptied section was also only refused when the whole FILE
emptied. The lesson is the one the refusal was written for: **a refusal is only as
wide as the pattern that feeds it**, and every one of these was found by constructing
the input and running it rather than by reading.

The network decision is explicit rather than implied. `rulebook-open-work` is the
grammar and opens no socket, so it is in the default set; `rulebook-open-work-state`
resolves and is `smoke`. Folding the first into the second would make a machine
without `gh` skip the assertions that need nothing. What may skip is bounded to a
missing PREREQUISITE detected BEFORE any entry is resolved; a fault appearing
mid-run, with some entries already resolved, is a FAILURE, because a partial answer
over a set is not an answer about the set. A repository probe that fails on anything
but 404 is the prerequisite case and not a finding — it shares a rate limit and a
transport with every query behind it, so treating it as one is a new way for a
healthy rulebook to go red.

And the GRAMMAR half carries the `docs-subject` label, which is load-bearing rather
than tidy: `ci-scope.sh` classifies `.agent/**` as docs-only, every `linux` job is
gated on that, so a pull request editing only `.agent/rules/*.md` runs **no ctest at
all** — the ungated doc-subject step is the only thing that runs. Unlabelled, this
check would be green-by-absence on exactly the change class that edits an Open work
entry. The resolving half stays out, because that step is inside the required
`Check C++ style` context and a required context that needs credentials and a network
fails for reasons that are not about the tree.

An emptied or renamed heading is refused for the reason `node-config-reference`
carries: two empty lists agree perfectly, so a `## Open Work` or a heading with
nothing under it would take one file's entries out of the scanned set forever while
the total stayed healthy.

## A green local gate says NOTHING when the subject under test is the build environment

**Measured across one change, in one afternoon: three platforms, three defects, none of
them visible locally.** A check that measures which translation units the analyser sees
went green on the local gate and then failed CI three times running:

<!-- table-total: none -->
| platform | defect |
|---|---|
| Linux (`clang-asan-ubsan`) | the third-party filter was a **denylist** naming `/_deps/`, and CI caches CPM sources under `.cache/CPM/` **inside the repository**, so ~40 vendored units read as analysed-by-nothing and the check refused a healthy tree |
| Windows | the selftest's fake `nm` was a POSIX shell stub spawned through Python -- `CreateProcess`, **ENOEXEC** |
| macOS | **bash 3.2 cannot parse a here-document inside a command substitution**, so the check died at PARSE time and all eight selftest cases refused for a reason unrelated to what they test |


**They are one failure, not three.** The subject under test was the build environment, and
that is the one variable a local run holds fixed. So the rule is stronger than "a local
gate says less here": **on this class of change it says nothing**, and a green one is not
weak evidence but no evidence.

Three rules fall out, each generalising past this change:

- **An exclusion list is a bet on the world's layout; an inclusion list is a statement
  about your own.** `/_deps/` was not the wrong *path* -- a denylist was the wrong *shape*,
  because it can only be right about locations it already knows. An allowlist naming this
  project's `src/` cannot be wrong about a directory it never mentions.
- **Knowing a rule and having just applied it is not protection.** That Windows ENOEXEC is
  the same defect the same author had fixed in another selftest **three hours earlier**,
  reproduced in a file written afterwards -- and the bash 3.2 hazard was already written
  down in this file. **A fix that is not structural does not transfer to the next file you
  write**, which is the argument for the scan living in its own `.py` sibling rather than
  as a heredoc, and for registering a check by its SUBJECT (what the *Linux* sweep cannot
  see) rather than by its mechanics.
- **A local run and a CI run can read different trees without either being wrong.** Catch2
  is a distribution package on the developer host and a CPM checkout in CI, so its sources
  were never in the local compile database at all. Not a misconfiguration -- two
  environments answering honestly about themselves.

## Open work

- **[#829](https://github.com/LASTRADA-Software/fastcached/issues/829)** — six
  contexts are still `Undecided` in `check-merge-queue-contexts.sh`'s binding table
  (`Windows-cl-debug`, `Code coverage`, both `compile-cache E2E` legs, the
  `fastcache-cc` smoke and `Docker image`). They cited #408, which the sanitizer
  promotion settles, so they were re-pointed at #829 in the same change: **an
  `Undecided` row naming a CLOSED issue is the "forgot" state the three spellings
  exist to keep distinguishable from a decision** — it still reads as *somebody will
  settle this*, the tally goes on printing it every run, and nobody owns it. The
  check asserts a row NAMES an issue and cannot tell an open one from a closed one,
  so nothing offline catches this; it was found by hand. `NotBinding` with a reason
  is a complete answer for most of these — a verdict is what is missing, not a
  promotion.
- **[#835](https://github.com/LASTRADA-Software/fastcached/issues/835)** — `clang-tsan`
  is the ONLY job in `build.yml` that sets `timeout-minutes`, so the jobs behind **twelve
  of the thirteen** required contexts inherit GitHub's 360-minute default. Raised while
  promoting the two sanitizer legs, and the survey is what stops it being filed as a
  regression from that: the promotion added two instances of a condition eleven required
  contexts were already in, and the likeliest leg to wedge is `clang-tidy`, the slowest in
  the workflow and required all along. The cost is **not** "six hours of held queue" — the
  ruleset's `check_response_timeout_minutes` is 60, so the entry is EJECTED at an hour
  (#359 already has) — it is an hour of queue latency plus up to six hours of runner time
  after the queue gave up. A bound needs measured durations: one set below a leg's real p99
  is a flake generator, and `clang-tsan`'s own 60 is a leg-specific argument about bounding
  its BUILD rather than a house number to copy.
- **[#830](https://github.com/LASTRADA-Software/fastcached/issues/830)** — nothing
  stops the required set or its count being restated in prose again. There is no
  drift today; what is missing is the guard, and the obvious one was measured against
  the tree and rejected rather than skipped. The measurement and the alternatives it
  rules out are on the ticket, and the rule it enforces is the
  *neither-the-set-nor-its-count* bullet above.
- **[#723](https://github.com/LASTRADA-Software/fastcached/issues/723)** — nothing
  checks that a workflow's script invocation agrees with that script's file mode.
  The complement of [#720](https://github.com/LASTRADA-Software/fastcached/issues/720)
  rather than a duplicate of it, and the more durable of the two: #720 makes the
  mode consistent so a documented bare invocation works, which is what a HUMAN
  hits; #723 is that a call site names its interpreter regardless, which is what a
  HARNESS hits silently. A `chmod` does not close it, because a moved file, a
  `noexec` mount or a later-lost mode bit each make a call fail to START, and
  inside a `want-fail` assertion every one of those is indistinguishable from the
  rule firing. Both instances are in the section above.
- **[#724](https://github.com/LASTRADA-Software/fastcached/issues/724)** — a `gh`
  listing that came back AT its `--limit` is a real answer about a set that is not
  the whole set, and nothing distinguishes it, so the cap reads as the total.
  Measured twice: `gh project item-list` truncating silently produced two wrong
  board figures reported onward (300 and 121-with-44-Todo against 356 and
  147-with-52), both wrong in the smaller and therefore unsuspicious direction;
  and `build.yml`'s clang-tidy notifier stops de-duplicating past 500 open issues,
  at 161 today. `scripts/ci-report-issue.sh` is the only caller that currently
  warns at the cap rather than vouching for a set it did not see. Raising a limit
  moves the cliff and hides that there is one, which is the timeout argument from
  the top of this file wearing a different number.
- **[#717](https://github.com/LASTRADA-Software/fastcached/issues/717)** — the
  open-or-update-an-issue decision exists twice: `scripts/ci-report-issue.sh`,
  which `merge-group-report-selftest` drives against a stub `gh`, and the inline
  copy in `build.yml`'s clang-tidy job. Deliberately not folded together as part
  of #684: that step is inside a REQUIRED context and is reachable only on a
  master push whose sweep has already failed, so editing its YAML to remove a
  duplication risks every lane's pull request for no gain #684 needs.
- **[#858](https://github.com/LASTRADA-Software/fastcached/issues/858)** — the eleven
    translation units no analyser sees are GATED but still unanalysed. #682 is CLOSED:
    #857 measures the set, refuses in both directions and pins the configuration in its
    verdict, so a unit going blind can no longer pass unnoticed. What remains is giving
    them an analyser — six Iocp need a Windows tidy leg, five kqueue a macOS one — and
    the caution that made #682 sit still applies: the ticket's fifteen pre-existing
    diagnostics would land a permanently RED context, a worse instrument than the gap,
    and neither leg can be developed from a Linux host.
- **[#589](https://github.com/LASTRADA-Software/fastcached/issues/589)** — the sweep
  now reports which files produced no code (#466), and that is the half that only
  *reports*: a green job's log is not read. Nothing asserts that a file guarded out
  here is compiled with its guard **active** somewhere, so the syntax error that
  started #466 could recur and every required check would still be green. The check
  is over **guards**, not platforms — `TlsContext_test.cpp` and `TlsSocket_test.cpp`
  are gated on a build OPTION, not an OS — and its union is over
  **(leg × configuration)**, never over legs: a leg that builds with an option off
  has run without covering anything that option gates. It is a different instrument
  over the CI matrix, not a deeper `tidy-sweep.sh`, which by construction sees one
  configuration and cannot know what the macOS or Windows legs compile.
- **[#605](https://github.com/LASTRADA-Software/fastcached/issues/605)** —
  `PreprocessArgv` re-parses the whole compile database once per translation unit,
  a second and looser lookup rule for a question `PlanUnits` already answers.
  The cost is the smaller half (14.1 s of CPU a sweep against 141 ms batched, ~0.3%
  of wall clock); the correctness half is that the plan row splits with
  `${unit#*$'\t'}`, which strips through the **first** tab, so a third column folds
  into `$file`, the lookup misses, and the unit is classified from a command that is
  not its own with nothing reporting. The row shape is fixed before the batching, and
  a batched pass is accepted only on identical classifications over the real
  database — a 100× speedup that moves one verdict is a regression, because the
  verdicts are the entire job.
- **[#607](https://github.com/LASTRADA-Software/fastcached/issues/607)** — #568 made the
  two CMake sites agree on the lookup that RUNS the interpreter, and `cmake/Coverage.cmake`
  names the four places python is located and which two do not validate. **Nothing reads
  that comment.** A third `find_program(NAMES python3)` passes every check in the tree and
  the comment goes false silently, which is #568's own divergence one level up. A check
  would be a glob over `**/*.cmake` and `**/CMakeLists.txt` and never a file list (#492),
  a general needle rather than the exact line removed, and it must express
  `cmake/Version.cmake` as a REASONED exception — its `find_program` for git is
  deliberate, because the module is included before `project()` where `find_package` has
  no toolchain — or the check fails a correct file. Roughly 3x the diff it protects, so it
  is recorded rather than urgent.
- **[#260](https://github.com/LASTRADA-Software/fastcached/issues/260)** — the one
  entry in `.tsan-suppressions`: `AdminEndpoint` closes its listener from the main
  thread while its own accept thread is still inside `Accept()`. Removing the entry
  is part of closing the issue — with it gone the gate goes red on the real report,
  which is what makes it a suppression rather than a hole.
- **[#311](https://github.com/LASTRADA-Software/fastcached/issues/311)** — nothing in
  CI catches an uninitialised read, and no sanitizer that runs today can: ASan does
  not, UBSan does not, and neither does ThreadSanitizer. That is MemorySanitizer,
  which needs an instrumented standard library, or valgrind memcheck over the
  existing release test binaries. It is the other half of #132, deliberately left
  out of the TSan job rather than folded into it.
- **[#316](https://github.com/LASTRADA-Software/fastcached/issues/316)** — the TSan
  gate **suppresses a known race in a module it does not scan.** Its scope is
  three directories (`Async`, `Consensus`, `Distributed`), and the one entry in
  `.tsan-suppressions` is `race_top:FastCache::BlockingListener::Close` — a `Net/`
  class. The report only reaches the gate at all because the node binary happens
  to be run whole; a regression of that race reached through a `Net/` unit test
  would leave the job green while it carries a suppression naming the very thing
  that broke. That is the gate's own failure mode, inside the gate. `Net/` and
  `Cache/` also spawn threads in `ThreadedAddressResolver_test.cpp`,
  `HealthProbe_test.cpp`, `EpollSocket_test.cpp`, `ShardedStorage_test.cpp`
  (`[sharded][concurrency][stress]`, the tree's one explicit concurrency stress
  case) and `Core/Clock_test.cpp` — none selected by the gate's tags, none in
  `FastCachedTsanScopeDirs`, so `check-tsan-scope` does not flag them either.
- **[#317](https://github.com/LASTRADA-Software/fastcached/issues/317)** —
  `scripts/check-tsan-scope.cmake` proves a FILE is in scope, not a test CASE: one
  selected tag anywhere in a file covers it, so a case added to
  `Distributed/FleetHistory_test.cpp` tagged only `[fleetchart]` leaves the
  sanitized scope while the check reports covered. Same shape as the bug the file
  exists for, one level down. Closing it means matching each
  `TEST_CASE`/`TEST_CASE_METHOD`/`SCENARIO` tag string, which
  `check-test-names.cmake` already has the macro pattern for — with the wrinkle
  that the tag string is usually on the line *after* the macro.
- **[#318](https://github.com/LASTRADA-Software/fastcached/issues/318)** —
  `clang-tidy` and `clang-asan-ubsan` both `actions/cache@v4` the same
  `cpm-Linux-clang-debug-*` key, so on a `CMakeLists.txt` change both upload the
  same archive and the loser discards it after paying for it. `clang-tsan` uses
  `actions/cache/restore@v4` rather than becoming a third writer.
- **[#312](https://github.com/LASTRADA-Software/fastcached/issues/312)** — the TSan
  scope is a bash tag table (`TARGETS` in `scripts/tsan-gate.sh`, cross-checked by
  `scripts/check-tsan-scope.cmake`) rather than a `ctest -L` selection, because this
  project's Catch2 (3.6) predates `ADD_TAGS_AS_LABELS` and so exports no tag to
  CTest. When Catch2 moves, both collapse into a label filter.
