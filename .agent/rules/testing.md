# Testing

Rules about how tests are registered, what they may assume, and the ways a test
suite in this repository has previously reported a defect as something else.

Read this before adding a script-driven test, a test that needs a port, or any
test that waits on another process or thread.

The rule that costs the most when broken: **every wait is bounded**. A helper that
spins on a condition a regression never satisfies hangs instead of failing, and a
900-second ctest timeout naming nothing is the least useful way CI can report a
defect — this repository has already paid for that once
(`dist-compile-e2e ***Timeout 900.10 sec`).

And the next clause of the same rule: **a bound is an assumption about the
machine.** A budget that passes on a warm developer laptop encodes that laptop — a
cold two-core runner doing the same work is the machine the test actually has to
survive. Write the budget for the slower machine, and make the wait name **which**
participant it is waiting for, because "saw 1 of 3" says the fleet is short without
saying which process never came up.

`node-scratch-isolation-e2e` cost a CI cycle proving it. It started two compile
nodes at once, each given a bare `--toolchain` — which makes a node compute a
fingerprint by walking the compiler's entire include tree. Warm, that is seconds and
the fixture passed every local run. Cold, on a two-core runner, two of those racing
did not both finish inside 90 seconds, and the fixture failed having never reached a
compile. The one node that did register was the scheduler, because
`--toolchain=scheduler-only=<cc>` names its fingerprint explicitly and is therefore
never probed — and *that* asymmetry, visible only in the log, is what identified the
cause. The budget was not careless; it was measured on the wrong machine.

Two things fixed it, and the second is the one that generalises: the nodes are
started one at a time, and each wait names the node it is waiting for and dumps that
node's log when it expires. Serialising cost the fixture nothing, because what it
exists to catch happens when two workers *compile* at once, not when they start.

One corollary, because reorganising a fixture is how its teeth get quietly pulled:
**a fixture that has been restructured and not re-falsified is a fixture nobody has
checked.** After that change the defect was reintroduced and the fixture confirmed to
still fail for its original reason before the change was believed.

## A bounded wait must also say WHICH KIND of failure it was

"Every wait is bounded" is the rule above, and it is not enough on its own. A wait
that reports only `waited 300s for worker A` cannot tell a loaded machine from a
wedged process, and those are fixed in completely different places -- one is a
budget, the other is a bug. Reading such a failure, nobody can responsibly choose
between raising the timeout and opening a defect, so the timeout gets raised,
because that is the action that makes the red go away.

`node-scratch-isolation-e2e` failed exactly that way on a Windows leg: worker A had
logged `computing the toolchain fingerprint` and nothing further inside its 300s
budget. The artefacts could not distinguish a cold include walk on a contended
runner from a deadlock, and the test's own history made both plausible -- it had
already been re-shaped once, within hours, because two nodes computing fingerprints
at the same time blew the budget.

So a wait that can time out records what it would need to tell them apart:

- **On success, what it actually cost.** Nothing recorded this, so no budget in the
  fixture could be set from data -- every number was a guess that survived by being
  generous. A green run now measures itself, and the next budget argument has
  evidence behind it.
- **Whether the process is still alive**, and its exit code if not. A process that
  DIED is a third case, diagnosed in one line, and it should never be reported as a
  timeout -- it is reported the moment it is noticed, rather than after the budget.
- **Whether the log grew**, and how long ago.
- **How much CPU it consumed during the wait**, and this is the half that is easy to
  miss. The operation this wait covers -- walking a compiler's include tree -- logs
  NOTHING while it runs, so a slow walk and a wedge produce *identical* logs. A
  log-growth heuristic alone would have diagnosed that failure confidently and
  wrongly. Work burns CPU; a block does not.

And where the signals disagree, say **INCONCLUSIVE** and name what is missing. A
confident wrong diagnosis costs more than an honest missing one -- that is the whole
lesson of this rulebook, applied to the reporting rather than to the code.

## What that classifier then got wrong, which is worth more than what it got right

The first version of it shipped, fired on its fifth occurrence, and was wrong:

```
waited 300s for worker A to compute its toolchain fingerprint and bind
  evidence: alive=True logGrew=True lastGrowth=300s ago cpuDuringWait=3.4s
VERDICT: the process was still WORKING when the budget ran out (it consumed CPU
throughout). That is a budget or contention problem, not a hang -- raise the
budget or reduce what runs beside it.
```

Three separate errors, and only the first is about this fixture.

- **When a test binary dies mid-run the DENOMINATOR moves, and the ratio still looks
  healthy.** `AGENT.md` says a count cannot carry a verdict -- "25 of 26 green is
  arithmetic that is true and useless" -- and this is its sharper form: a crash does
  not report as a crash, it reports as a *smaller suite that mostly passed*. Measured
  while deleting a flag in #290 stage 3: removing a row from a `std::array<Row, 4>`
  left the fourth value-initialized to null pointers, the segfault took the rest of
  the run with it, and the summary read `171 cases | 170 passed | 1 failed` for a
  binary holding **276**. Every instinct trained on ratios says that is fine.

  The only signal was that 171 was not 276, which is a number you have to already
  know -- so do not rely on noticing it. **Compare the case count against the previous
  run whenever a change deletes anything**, and treat a drop as a crash until proven
  otherwise. A shrinking suite and a passing suite look identical in the one line most
  people read.

  It is also the project's own table rule arriving in a test file: an extent written
  as a literal outlived its rows. `std::to_array` derives it and the shape becomes
  unrepresentable, which is why the production tables use it.

- **A refusal's wire CODE is not its reason, so a test asserting the code is asserting
  something weaker than it looks.** The production side of this is already written down
  in [`metrics-and-observability.md`](metrics-and-observability.md): *the row is the
  refusal, not the code* -- two refusals share `MalformedFrame` and must not share a
  counter, so a table keyed on the code cannot hold them. The test-side consequence is
  the same fact read backwards: **where two refusals may legitimately answer one code,
  the counter is the reason and the code is not.**

  It is not hypothetical here. #290 merged the cache and compile surfaces onto one
  listener, and that listener now carries two refusals answering `ErrorCode::NotAMember`
  by deliberate design -- `CacheResponder::RefusePeer` refusing a caller that is not
  this machine (`NodeCacheRequestsRefusedNotLocal`), and `RefuseUnlessMember` refusing
  a caller with no claim on this machine's CPU (`WorkerJobsRefusedNotAMember`). One
  code, because a launcher steps over both identically; two counters, because an
  operator does not. A case asserting only the code cannot tell them apart, and after
  the merge they arrive on the same socket.

  Measured on #290's acceptance case, whose whole point is that those two rules
  disagree about one peer: delete the `Increment` from the locality refusal, changing
  nothing else, and the run reads **6 passed, 1 failed** -- the wire code assertion
  **passes**, and the only thing that goes red is
  `NodeCacheRequestsRefusedNotLocal == 1`. The refusal was still correct; what was
  lost was any way for the test, or an operator, to say WHICH rule produced it.

  So on a surface where codes are shared, **assert the counter as well as the code**,
  and read a green code assertion as evidence of very little on its own.

- **A cumulative figure cannot answer a question about now.** `cpuDuringWait` was
  the total over the whole wait and was read as a claim about the process's state
  **at the deadline**. 3.4s spread evenly over 300 seconds and 3.4s burned in the
  first ten seconds followed by a wedge are the same number and opposite
  diagnoses. A *duty cycle* over that same window is the identical mistake with a
  percent sign -- it is the same number divided by the same 300. Only a **recent**
  window can answer a question about now, so that is what the verdict is drawn
  from and the totals are printed as evidence only.
- **No magnitude bar can be calibrated when the healthy range spans orders of
  magnitude.** The whole Windows SDK include tree is 9,487 files and walks warm in
  0.1s at 88% duty; the same walk cold, virus-scanned and contended is I/O bound
  and burns a tiny fraction of that. A bar set high enough to exclude an idle
  runtime's own timer threads would call a genuinely working cold walk BLOCKED,
  which is the opposite error and the worse one -- it sends somebody hunting a hang
  that is not there. **What does not vary is zero**: a blocked thread accrues no CPU
  at any temperature. So the test is *presence* in a recent window rather than
  magnitude, and everything between "idle" and "clearly working" is reported as
  neither.
- **An `-or` is right for two independent CONFIRMATIONS and wrong for two competing
  READINGS.** `$progressing` was False -- the log had not grown in 300 seconds --
  and `$busy` was True, and the disjunction let the weaker one win unopposed. The
  two signals *contradicted each other* and the code combined them as though they
  agreed. This is a reasoning error rather than a scripting one, and reaching for
  `-or` is the reflex. Where two signals can disagree, the disagreement is the
  finding.

`logGrew=True` is the same family from the other side: it was true, and it counted
the two startup lines landing in different polls. **A signal that cannot be false in
the failing case is not evidence**, and printing it beside a conclusion lends it
authority it has not earned.

Two things follow for anything shaped like this.

**Measure the process TREE, not the process.** A step that spawns a compiler sits in
a kernel wait at zero own-CPU while the child does the work, so recent-window
own-CPU alone reports BLOCKED for a healthy compile. The first version documented
that limitation in prose and told the reader to run `Get-Process cl` by hand; a
limitation you can describe is one the instrument can apply.

**An instrument that prints a remedy has to know when the remedy is under dispute,
and it cannot.** "Raise the budget" is sound general advice, was printed by the
branch that fires most often, and is exactly what [#354](https://github.com/LASTRADA-Software/fastcached/issues/354)
refuses -- that budget has been raised twice already. State the finding and stop.

And it is tested, by `ctest -R node-scratch-isolation-e2e-selftest`. **A classifier
that cannot be made to say BLOCKED cannot report a hang**, and the original could
not.

## A fixture waits on what a line MEANS, and a rename is not what changes it

`node-scratch-isolation-e2e` starts three nodes one at a time, and the paragraph
explaining why has been in the file since it was written: two include-tree walks
racing on a two-core runner exceeded the budget and the fixture failed having never
reached a compile. The serialisation rested on waiting for `compile node ready`
before starting the next node, which at the time meant **surveyed** -- a node
fingerprinted before it bound.

[#365](https://github.com/LASTRADA-Software/fastcached/issues/365) made a node bind
and serve first and survey afterwards, for good reasons of its own. The log line was
not renamed, not moved and not reworded; it simply stopped carrying the fact the
fixture was reading out of it. Every node now reached it in about a second, all three
walked at once, and the measured rate on the `clangcl` runner fell to **2-5 file/s**
against the ~30 file/s [#354](https://github.com/LASTRADA-Software/fastcached/issues/354)
measured with a single walker. 5136 files did not fit in the 600 s that
[#428](https://github.com/LASTRADA-Software/fastcached/issues/428) had just moved
onto the registration wait, and an unrelated pull request was ejected from the merge
queue for it.

Three things follow.

**The budget was the symptom and raising it would have buried the cause.** The
fixture asked the machine for three times the concurrent I/O it was written to ask
for; a budget that accommodated that would have made every later run three times
slower and left the next reader with no way to see why.

**Wait on the stage, not on a line that currently coincides with it.** The two
stages now have two waits -- `compile node ready` for **serving**, `serving <compiler>
as <fingerprint>` for the survey -- and they are kept separate rather than folded,
because a stall in one has different costs and different fixes from a stall in the
other, and a fixture that folds them cannot say which one happened.

**And `compile node ready` is not the bind.** This file said "for the bind" and
`AGENT.md` said #365 made the line mean *bound*; both were a shorthand and both were
wrong in the same direction
([#652](https://github.com/LASTRADA-Software/fastcached/issues/652)). Measured: the
accept loop starts at `main.cpp:1128` and the line is logged at `:1454`, with the
credential, the registrars, the startup toolchain count and the heartbeat thread in
between --

```
bound  <  accepting  <  READY (serving)  <  surveyed
```

Relative to the survey the marker is early; relative to the bind it is **late**. The
cost of the shorthand was not theoretical: `node-scratch-isolation-e2e.ps1` waited on
this marker and reported *"did not bind its compile port"*, so a node that bound,
accepted and then stalled in heartbeat setup sent its reader to check a port that was
open and answering -- the one part that was working. The marker and the fact it names
are now a row of `Core/ReadinessMarker.hpp`, whose `ReadinessFact` has **no `Bound`
enumerator**, so the mistake cannot be spelled there
([#654](https://github.com/LASTRADA-Software/fastcached/issues/654)).

**A comment stating the intent did not protect it.** The serialisation paragraph was
correct, prominent and three lines above the wait that stopped implementing it. What
a fixture depends on has to be in what it *does*.

## Do not measure a stand-in through an instrument as costly as the thing measured

The first version of that test drove real processes. Six stand-ins, each arranged
to exhibit one reading -- a growing log, a self-spinner, a quiet parent with a
spinning child, a sleeper, a trickle-then-silence, one that dies -- and it read the
classifier's answer back. Five of the six were robust, because they target a
*presence*, an *absence* or a *death*, and those do not depend on how fast the box
is.

The sixth had to land a **magnitude** strictly inside the classifier's
`(0.15s, 0.50s)` band. It burned until its own consumed process CPU had risen by
250 ms -- already closing the loop on the right quantity rather than spinning for a
duration -- and it still could not be made reliable:

- it passed three consecutive local runs and failed on `Windows-clangcl-release` at
  **0.52s against a 0.50s bound**, because leftover interpreter startup landed
  inside the measured window;
- tightened, it then put the deliberate burn at the recent window's cutoff edge,
  where **0.16s of a measured 0.25s** counted.

Neither was a bound being wrong. **The band is 0.35s wide and a PowerShell
process's own startup costs 0.2-0.5s** -- the instrument's overhead and the
quantity under test were the same magnitude, so there is no stand-in construction
that fixes it. The noise *is* the interpreter the stand-in is made of.

The fix is a seam, not a stand-in. `Get-WaitVerdict` is now pure -- it takes a
record of readings (own recent CPU, descendant recent CPU, counts, log growth and
stall age, alive, exit code, both bounds) and returns the lines to print. It opens
no socket, reads no clock, spawns nothing. Acquisition keeps doing exactly what it
did.

Three things follow, and they are the reason to reach for this shape early rather
than after a red CI leg:

- **Branches that could not be staged become one line each.** The
  could-not-sample-CPU verdict needs a live process whose CPU the script may not
  read, which cannot be arranged; as a record it is `OwnRecent = $null`.
- **Every bound gets pinned on both sides**, not demonstrated once from the middle
  -- at the floor and just above it, at the working bound and just below it. That
  is where an `-and`/`-or` mistake actually lives.
- **It is instant**, so the sweep that proves each branch load-bearing is cheap
  enough to actually run. Mutating each of the seven verdicts and each of the five
  comparisons turns exactly the expected cases red; `EverGrew -and` weakened to
  `-or` -- the original defect's own shape -- turns 14 of 17 red. 53 seconds and a
  `RUN_SERIAL` became 0.3 seconds and neither.

What deliberately stayed untested is **acquisition**: `Win32_Process`, the
parentage walk, the pid-reuse guard by creation time, the locked-log read. All
three real defects here were in that half, and it is exactly as hard to exercise
wherever it lives -- but it now contains no decisions, so nothing silently right or
wrong is hiding in it.

The general rule, which is not about PowerShell: **a decision worth several named
outcomes is worth separating from the ambient facts it reads**, and a fixture that
must exhibit a magnitude must not be measured through machinery whose own cost is
comparable to that magnitude. Both halves of this file got that wrong once, in the
same direction -- towards a confident wrong answer.

## A case name is an ARGUMENT, so it may not begin with `-`

`catch_discover_tests` registers each case as
`add_test(NAME <case> COMMAND <exe> <case>)`. The name therefore reaches Catch2 on
its command line, and Catch2 parses arguments before it parses test specs — so a
leading dash is read as an option. The two halves fail differently and the quiet
one is worse:

- **A recognised flag.** `TEST_CASE("--help wins over whatever follows it")` made
  the runner print its usage and exit 0, so CTest recorded a **pass for a case that
  had never run**. It had been doing that since the day it was written, which is
  the shape this whole file is about: nothing fails, and the thing somebody was
  told is covered is not. `TEST_CASE("--help works with and without a
  sub-command")` in the test client was the same case, one target over.
- **An unrecognised one.** `TEST_CASE("--cache-dir gives the node an on-disk
  tier")` made Catch2 print `Unrecognised token` and exit non-zero, so CTest
  reported a failure the case did not have — and it passed when run by hand. That
  is how the first half was found.

`ctest -R test-name-hygiene` scans for the pattern and refuses a third. It scans
rather than listing the two known names, for the reason every other table here
does: a list of what is wrong today is maintained by the same person who
introduces the next one. Put the flag anywhere but first — "Naming `--cache-dir`
gives the node an on-disk tier" reads no worse and runs.

## The shared helpers: `Unwrap`, `ScratchPath` and `ScriptedSocket`

`src/tests/` holds the helpers every test target shares -- `Unwrap.hpp` and
`ScratchPath.hpp` -- and they are there rather than beside one caller for the same
reason, which the second one learnt the hard way (see below).
clang-tidy's `bugprone-unchecked-optional-access` cannot see a `has_value()`
guard through Catch2's `REQUIRE`, so a plain `*x` after one is a **build failure**
under `WarningsAsErrors` — `Unwrap(x)` goes through `value_or`, which is provably
safe, and the preceding `REQUIRE` still fails first when the optional is empty.
It replaced eleven copies that were *near*-identical rather than identical: each
carried its own abbreviation of that reasoning, so why the idiom exists was
reconstructible from some and not from others, and an author who found a terse
copy had nothing telling them a plain dereference was not simply better. It is
header-only and includes only `<optional>`, so the launcher's and worker's test
binaries can use it without linking `FastCache`. `std::expected` is **not**
covered by that check, so a `*result` after `REQUIRE(result.has_value())` stays as
it is — routing it through `Unwrap` does not even compile.

**A per-process counter is not unique, because every `TEST_CASE` is a process.**
`catch_discover_tests` registers each case as its own ctest test, so a fixture that
names a scratch directory from a `static` counter hands the same path to every
concurrent case -- and the constructors of these fixtures all begin with
`remove_all`, which turns a name collision into **deleted data**: the second case
wipes the files the first is still reading. `tests/ScratchPath.hpp` is the one
definition now (`UniqueScratchPath`, pid + counter, and the RAII
`ScratchDirectory` over it), and four things about how it got there are worth
keeping:

- **It has been written five times.** The first fix was private to
  `Stats_test.cpp`; three later files reintroduced it; and the fifth,
  `RaftStorage_test.cpp`, failed ten cases under `ctest -j 8`. Its class comment
  claimed exactly the guarantee it had -- "two cases running in one binary cannot
  collide" -- and that scope is the bug.
- **The fifth one happened because the shared fix was somewhere it could not be
  included from.** `UniqueScratchPath` lived in `src/apps/fastcache-cc/`, which
  `FastCacheTest` does not have on its include path, so the one suite that could
  not reach it re-derived it. A helper is shared only if it sits where everything
  that needs it can include from; `src` is on all three test targets' paths, which
  is why this and `tests/Unwrap.hpp` live together.
- **The constructor's `remove_all` is safe only because the name carries the
  pid.** What it can then reach is this process's own leftovers or a dead
  process's -- never a live peer's. Removing something you did not create is the
  step that made a collision destructive rather than merely confusing.
- **A caller-supplied name becomes a child of a unique parent, not the whole
  name.** `ScratchTree`/`ScopedTree` take names like `"cache-hit"` and one that is
  itself a nested path, and those names are what a reader recognises; they hang
  under `UniqueScratchPath(prefix)` now, and the destructor removes the *parent*
  so the unique level cannot leak.

**And both CI and the gate run the tests in parallel now, because until this
change nothing did.** `ctest` was invoked bare in every CI job, no preset set a
job count, and `scripts/local-gate.sh` did not either -- which is why five
separate authors could write this bug and no run anywhere would show it. CI sets
`CTEST_PARALLEL_LEVEL` once in the workflow's `env:` rather than adding
`--parallel` to each of the four `run:` lines, so an invocation added later is
covered by construction; the gate passes `--parallel` explicitly (`getconf
_NPROCESSORS_ONLN`, since it runs on macOS too; `FASTCACHE_GATE_JOBS` overrides).
Tests that genuinely cannot share -- a daemon, a fixed port -- carry `RUN_SERIAL`,
which `tls-smoke` was missing while every one of its neighbours had it. Measured
on the full suite: 649s serial, 131s at `--parallel 8`, same 1965 tests; Windows
65s to 37s.

**A fake is a shared helper too, and the same rule caught it a third time.**
`tests/ScriptedSocket.hpp` -- an `ISocket` replaying canned reply bytes, plus
`Replies` and an all-paths-fail `FailingSocket` -- existed as three near-identical
private copies, and **two of them carried the same defect**: `WriteVectored`
answering `0`, a short write, which `SendAll` correctly reads as a failure. Nothing
had tripped on it because `CacheProtocol::Exchange` is the first caller to send a
vectored write and no case reached one. The second copy was found while writing
#340; the first was still carrying it a day later and was found only by reading the
three side by side (#362).

Two of the arguments *for* keeping them separate had been written down, and both
were wrong in an instructive way:

- "Sharing would mean moving ninety lines to reuse thirty." That trades lines
  against a bug fixed in one copy staying live in the others, which is what
  happened. The trace machinery moved **with** the class rather than being dropped,
  so the case that needs it still has it and the two that do not simply never call
  it.
- "These two binaries share nothing but the wire, so a fixture reaching across
  would be the coupling the test denies." That is right about the two **binaries**
  and does not reach the helper: `src/tests/` is neither binary's, includes nothing
  from either app, and is already how both reach `Unwrap`.

The general form is the one `ScratchPath` taught: a helper is shared only if it
sits where everything needing it can include from -- and a fake nobody exercises
does not report its own bugs, so copies of one drift silently and the drift is
found by whichever case walks into it.

## The POSIX fixtures share one helper library, and a bound is read from a clock

`scripts/lib/e2e-common.sh` holds `fail`, `free_port`, `port_answers`,
`wait_until`, `wait_for_port`, `wait_for_log`, `stop_and_require_exit` and
`http_get`. It was seven copies, and every argument the C++ side of this file
makes about `ScriptedSocket` applies unchanged: a shared helper is a shared fake,
a fake nobody exercises does not report its own bugs, and copies drift silently
until whichever caller walks into the drift pays for it.

The drift had already happened, in three directions, and each one is a fixture
that could not report what it saw:

- `check-compile-cache-daemon-start.sh`'s `wait_for_port` checked **no liveness
  at all**, so a daemon that died at `bind()` was reported as `never answered on
  port N` after the full bound had burned — the slow-machine-versus-wedged-process
  confusion this file spends three sections on.
- `fleet-dashboard-e2e.sh`'s `http_get` learnt that `read` returns non-zero on a
  final chunk with no trailing newline, so a naive loop drops it — and the JSON
  document that route serves is one line with no newline at all. The fix was never
  carried back; in the other six copies it is latent by luck about which endpoints
  end in a newline.
- Two `free_port`s had no issued-port ledger.

The library is therefore the **union** of the correct behaviours, not their
intersection: preserving each caller's current behaviour would have preserved all
three defects under the name of compatibility.

**A bound is read from a clock, never counted in iterations.** Every copy said
things like "100 x 0.2s = 20s" and none of those equalities held: `sleep` is an
external command that forks and execs, and it overshoots on a busy machine, so the
product is a *lower* bound that drifts most exactly when a fixture is timing out.
`fleet-dashboard-e2e` had the pure form of it —

    fail "timed out after $((WAIT_TICKS / 10))s waiting for $what to listen on ${port}"

— a duration computed from the loop shape and **never observed**. Whatever the loop
took, the operator was told `WAIT_TICKS / 10`. That message is the one this file
requires to separate a slow machine from a wedged process, and it cannot, because
the reading that would show a slow machine is derived from assuming the machine was
fast. `SECONDS` bounds the loop and `SECONDS` is what gets reported; the sleep stays
as pacing. One second of resolution is the price and it is ample against budgets of
20s and 240s, because it is a measured second rather than an assumed one.

**A bespoke condition is a predicate, never a bespoke loop.** `wait_until` is public
for exactly that: `fleet-dashboard-e2e` had a hand-written poll for a worker to
appear in `/fleet.json`, with its own bound, its own message and no liveness check,
because neither `wait_for_port` nor `wait_for_log` covered it. That is how the next
copy gets written.

**`fail` signals the top-level shell unconditionally, and does not test `BASHPID`.**
`exit` inside `( ... )` ends the subshell only; the obvious guard is to compare
`BASHPID` with the top-level pid and signal only when they differ, and that guard is
correct on bash 4 and **silently inert on 3.2**, where `BASHPID` is unset and the
test reduces to comparing `$$` with itself. So there is no detection: the signal
goes out always and a `trap 'exit 1' TERM` turns it back into an ordinary exit with
the EXIT trap intact. This is not belt-and-braces reasoning — staging the `BASHPID`
guard back in and running the self-test on a bash 5 runner leaves every behavioural
case **green**, because on bash 5 the guard works. The bash-3.2 scan in
`check-e2e-helpers.sh` is the only check in the tree that can see it.

`ctest -R e2e-helpers-selftest` is the test, in the default set, POSIX-only. Its
decision half — which kind of failure a wait suffered — is `_e2e_verdict`, pure and
driven from staged records with both sides of every threshold pinned, for the reason
`Get-WaitVerdict` is. Its acquisition half is driven against real processes that
really die and real logs that really grow. Both halves were falsified: eleven staged
defects, each shown red at the case that names it. Two of the first drafts' cases
could **not** be made to fail and were rewritten — a forty-draw port test that a
deleted ledger passed fifteen runs in sixteen, and a `( fail )` subshell case that
`set -e` was quietly rescuing.

## An in-process fleet, and what a harness has to earn

`tests/FleetHarness.hpp` runs a compile fleet in one process: N `SchedulerService`
instances addressed by endpoint, real `SchedulerProtocol` framing, the launcher's
own `Cc::ExchangeFramed` on the client side, and `Cc::Dispatch` running unmodified.
Time moves only in `Step`. It is the fleet's counterpart to
`Consensus/RaftClusterHarness`, and the argument for it is the one that header
makes: rules about a **sequence** across two machines are not reached by any amount
of single-transition testing.

Three things about it are load-bearing.

**It is in `src/tests/`, not beside `RaftClusterHarness`.** That header lives in
`Consensus/` because everything it touches lives in `Consensus/`. A fleet spans
`Distributed/`, the node and the launcher's client sources, so the same placement
would make a **library directory include an app header**. `ctest -R net-boundary`
exists to stop the milder version of that; library-depending-on-app has no gate at
all, which is a reason to be careful rather than a licence.

**An interleaving has to be arrangeable, not hoped for.** `Cc::Dispatch` leases,
compiles and releases in one call, so a test cannot get between the grant and the
release from outside -- and that gap is exactly where leadership moving is
interesting. `OnCompile` fires inside the compile exchange, on the caller's thread.
This is the whole reason a script that spawns processes cannot assert these
properties: it can wait for an election, but it cannot place one.

**A harness earns its place by asserting something that can be shown red.** Per
#355 a test that cannot fail for the reason it exists is not evidence, and a
harness makes it easy to write a case that exercises a great deal and pins
nothing. The first property here -- the RELEASE goes to whoever ISSUED the lease --
was proven by pointing the release at the configured endpoint instead and watching
four assertions fail, **including a second client's live lease being freed**: two
schedulers number their leases independently and both start at one, so a misrouted
release matches somebody else's. The straight no-redirect case stayed green under
the same break, which is exactly why it is in the file: it cannot tell the two
rules apart, and a suite containing only cases like it would have looked like
coverage.

## Registering a script-driven test

Not every test is a Catch2 case. Script-driven tests are registered in
`src/tests/CMakeLists.txt`: the `smoke`-labelled ones start a real daemon or
invoke a real compiler and report a missing prerequisite as skipped (exit 77 with
`SKIP_RETURN_CODE`), while `repository-hygiene` runs
`scripts/check-repository-hygiene.cmake` through `cmake -P` and is deliberately
*not* labelled `smoke`, since it needs no daemon, socket or compiler and so belongs
in the default `ctest` set. It reports "not a git work tree" by printing `SKIP: `
and exiting 0, matched by `SKIP_REGULAR_EXPRESSION` — a `cmake -P` script cannot
choose its own exit code before CMake 3.29 (`cmake_language(EXIT)`) and this
project supports 3.28, so a `SKIP_RETURN_CODE` it could never return would be dead
configuration.

`net-boundary` is the second of those and takes the same shape, with one
difference worth stating: it has **no skip path at all**. `repository-hygiene`
asks the git index, which an exported source tarball does not have; this reads the
source tree, which such a tarball contains exactly as a checkout does. A skip
condition it could never meet would be the dead configuration the paragraph above
argues against.

**Which CMakeLists registers a script-driven test is load-bearing, not filing.**
`src/apps` walks its app table *in order*, so a test registered beside one binary
cannot name a binary that comes later in that table: at the point
`src/apps/fastcache-cc` is configured, `fastcache-compile-node` is not a target
yet, and a `$<TARGET_FILE:>` guard on it does not fail — it silently skips the
test, forever, with one `message(STATUS)` in a configure log nobody reads.
`src/tests` is added *after* `src/apps`, so every target exists by the time it
runs. That is why `dist-compile-e2e` lives there, and the general rule is that a
script-driven test naming more than one executable belongs in `src/tests`
regardless of which binary it feels closest to. (`compile-cache-e2e` predates the
rule and names only `fastcached`, which the table happens to reach first.)

`dist-compile-e2e` additionally allocates its ports per run rather than fixing
them. It needs four, and four more fixed ports is four more ways to collide with
whatever else a CI runner is doing — a failure that reads as "distribution is
broken" when it means "something else was listening". `cluster-e2e` does the same
with the twelve its four nodes need, and so does `compile-cache-e2e` with the two
its daemons need — which had a fixed port and failed under a parallel `ctest` as
"the first compile was not a MISS", i.e. as a defect in the launcher's key rather
than as a stranger answering. It also re-checks that the daemon still running is
the one it started, because a port that was free at `bind` time says nothing about
who holds it a second later.

**A drawn port is drawn from below the kernel's ephemeral range, and remembered.**
Both halves have now been a red CI run. The ledger came first: nothing is listening
on a port issued a moment ago whose server has not bound yet, and a fixture that
draws every port it needs before binding any of them can hand the same number out
twice. The range came second, and is the half a connect probe *cannot* answer at
all — a port may be the local endpoint of an **outbound** connection, ESTABLISHED
or TIME_WAIT, with nothing listening on it, so the probe says "free" and the
`bind()` still fails with `EADDRINUSE`. A draw of 20000–39999 overlapped Linux's
default `ip_local_port_range` of 32768–60999 by better than one number in three,
and `dist-compile-e2e` duly died at `bind(127.0.0.1:33174) failed: 98` in the case
that runs after five cases' worth of connections. `lib/e2e-common.sh`'s `free_port`
draws 20000–31999, below that floor and below macOS's 49152, and every fixture that
reaches it through the shared helper inherits that. `migrate-storage-e2e.sh` does
not: it spells the helper `port`, draws 40000–59999, and is therefore entirely
inside Linux's default ephemeral range
([#628](https://github.com/LASTRADA-Software/fastcached/issues/628)). A helper
reimplemented under a different name is invisible to `check-e2e-helpers.sh`'s
name-collision scan, which is what that scan's allowlist reasons are for.

A fixture that reports "free" from a probe is answering *"is anything answering
here"*, which is not the same question as *"can I bind here"* — and the gap between
the two is exactly where a smoke test fails for a reason that has nothing to do
with what it tests.

**A body of assertions with a prerequisite the host may lack becomes its own ctest
test**, not another case in an existing one. A script exits once, so a case inside a
suite can do no more than print a line and let the run go green, and skipped and
passed are then the same result. `dist-compile-membership-e2e` is that shape — one
script, two registrations, `--case membership` — and the reasoning is under *A
fixture whose client is always local cannot test who is admitted*, below.

## A shared failure message describes one caller, and lies to the others

`cluster-e2e`'s `find_leader` ended with:

    fail "no node ever answered a cluster question; the cluster never elected a leader"

Correct where it was written -- the first formation -- and false everywhere else it
is called from. In [#388](https://github.com/LASTRADA-Software/fastcached/issues/388)
it fired in phase 6, *after* eight assertions had passed, one of which had printed
the endpoint the cluster was being led from. The cluster had elected; what it could
not do was elect **again**, which is a different defect with a different cause. The
sentence sent the first half hour of the investigation to the wrong place.

So the situation is a **parameter**, named by each caller, and the failure prints
who was asked and what each said. "Nobody answered" is the absence of a finding;
the finding is in the refusals, each of which names the endpoint that node believes
leads.

## A reproduction models the wiring as it is

**A fix at a different layer invalidates the reproduction that found the bug — and
the failing test is the signal, not the nuisance.**

#279's reproduction ran two `CompileJobRunner`s over one scratch root, which is what
two node processes were: each has its own job counter starting at 1, so both derived
`job-1` and everything beneath it. It failed deterministically, which is exactly what
was wanted from it.

The fix claims the root a layer **above**, in the node. `CompileJobRunner` still does
not isolate anything — hand it a shared root and it still collides, because its
*caller* isolates and it does not. So the moment the fix landed, the reproduction
asserted a property the class does not have and never will, and could only ever fail.

The tempting response to a test that fails after a correct fix is to weaken the test.
The right one is to ask **which layer now carries the property**, and move the case
there: the same scenario now sits beside `ScratchClaim`, wired the way `main` wires
it — a claim per node, each runner handed the root its own claim covers, the same
barrier holding both inside the compiler at once. It still fails without the claim,
for the original reason.

A reproduction is therefore a statement about a *design*, not only about a defect.
When the design moves, the reproduction moves with it or it is deleted — never
relaxed until it passes.

## What `cluster-e2e` covers

`cluster-e2e` is the consensus counterpart, and what it covers is deliberately
disjoint from the unit tests rather than a slower repeat of them: three real
processes elect a leader and *keep* that leader for three seconds of polling, a
follower's refusal names an endpoint that a client then successfully dials, a
setting replicates, a member is removed, a fourth machine started with no cluster
at all is admitted and answers a question only the replicated state can answer, and
the cluster re-forms after its leader is killed. The stability half is not
padding: a cluster that re-elects on a timer has exactly one leader at almost every
instant, so a single poll passes against leadership that never settles. Every one
of those is a property of the wire, the transport, the timers and the command line
meeting at once — which `RaftClusterHarness` cannot reach precisely because it
replaces all four. It is POSIX-only for now: the properties are
platform-independent and the fixture is not, so a Windows counterpart would be a
translation rather than new coverage.

## The first real run of a fixture is where its defects are

`launcher-replay-e2e` was reviewed, reasoned about and merged into a CI job, and
its first execution anywhere died on the first line that starts a process. Four
defects sat behind that line, each hidden by the one in front of it. Three are
below; the fourth was a repository bug the fixture merely reached first, and has
its own section two below this one (#390). They are recorded together because the
shape repeats:
**a fixture that has never completed has told you nothing, however carefully it
was read.**

**A flag spelling is checked against the table, not remembered.** The daemon was
started with `--memory-limit 2048mb`. There is no such option — the only
occurrence of that spelling anywhere in the tree was the line that used it — and
the byte suffix is a single character, so `2048mb` would be an unknown unit under
the right name too. The daemon printed usage and exited, and the fixture reported
`the daemon never accepted a connection`: a true sentence about the wrong thing.
Two spellings of the same start-up already existed in `scripts/`; the one that
worked was the one nobody had invented.

**`exit` inside a subshell ends the subshell.** A `fail` helper called from a
`( ... )` group — here the control build, running in one so it can unset the
launcher environment — does not stop the script. It then reported two further
failures about the artefacts the first one explains, so a reader working upward
from the last line starts on the wrong question. Fixed at the helper, not the
call site, because the next subshell will not remember: signal the top-level
shell, and test with `BASHPID` rather than `$$` — bash keeps `$$` at the parent's
value inside a subshell, so the guard would always hold and silently do nothing.

**A guard that proves a fixture bites can itself fail to bite.** The canary
compiled a deliberately wrong object over the object of the first `_test.cpp` in
the compile database — a unit of a *different target*, which the binary under
test does not link. The suite passed with exactly the case count it had passed
with a minute earlier, and the fixture announced `the suite PASSED with a wrong
object linked in`. No wrong object was linked. The observation was true and the
claim attached to it was false, which is worse than either alone: it names the
subject as broken when the instrument is.

So an injection is **asserted**, in the artefact and again in the thing that
consumes it. Every way of poisoning nothing — a substitution that did not fire,
an output path this build does not use, a unit belonging to another target — now
stops with a sentence about the injection rather than a verdict about the cache.

## A canary reports on the guard AND on itself, and cannot say which

The `static_assert` that every cache refusal policy states a counter *or* a
rationale (#491) was watched refusing only on the **third** attempt. The first two
runs both printed *the guard does not bite*, and the guard was correct both times.

Once the canary ran the build as `cmake --build ... | tail -40` and read the
**pipeline's** status, which is `tail`'s zero — so a build that *had* failed on the
assertion was reported as one that succeeded. That is
[`build-and-toolchain.md`](build-and-toolchain.md)'s `producer | grep -q` entry
arriving through the other end of the same pipe, written by someone who had read
that entry the same evening.

Once the assertion **was not in the file at all**: a path-scoped `git checkout --`,
undoing an unrelated botched edit, had discarded it along with the mistake it was
aimed at. The canary correctly reported that a guard which does not exist does not
fire.

*Absent*, *present but toothless* and *the run could not tell* are three states.
A canary that answers in two collapses them, and the sentence it produces blames
the **subject** — which is the same shape as a fixture announcing a wrong object
was linked when none was. What separated them here was neither care nor rereading:
it was **asserting the object count**. Forty-eight units rebuilt, so "compiled
nothing" was excluded and every surviving explanation was about the code.

So a canary: asserts the poisoned edit actually **compiled** something; takes the
status from the **process**, never from a pipe; and confirms the failure is the
**intended** one by matching its text rather than by a non-zero exit — a build that
fails for another reason proves nothing about the guard. Where it cannot establish
those, it reports INCONCLUSIVE and names the reading it is missing.

## Assert what DISTINGUISHES, not what both sides produce

In one evening four lanes found **five** tests that could not fail for the reason
they existed ([#355](https://github.com/LASTRADA-Software/fastcached/issues/355)).
Not the same mistake in the same place — the same mistake in five shapes, and every
one of them green.

Two, which are enough to see the shape:

- **[#286](https://github.com/LASTRADA-Software/fastcached/issues/286)'s acceptance
  did `REQUIRE(held)` on a bind** (implemented by pull request #348).
  `BlockingListener::Bind` returns a **non-null pointer in an errored state**, so a
  failed bind passes it. Both sections — a named port that must refuse, a defaulted
  one that must warn and continue — would then have passed on a `ParseTcpPort`
  refusal rather than on the port conflict they existed to prove.
- **#288's pinned raft test asserted that a refusal happened and that the message
  names `--listen-raft`.** *Both* the old cross-flag rule and the new grammar rule
  name that flag, so it pinned the flag rather than which rule answered — and a
  relocation could land, or be reversed, in silence.

The other three: a reproduction whose fixture **supplied its own refusal** and so
never drove the production validator; an acceptance that passes under a digest taken
at the **wrong layer**, so it would have blessed a fix that catches nothing; and a
progress check using **log growth**, where the include-tree walk logs nothing while
it runs and a slow walk is character-for-character identical to a wedge.

**The shared shape: each asserts something both the healthy and the broken state
produce.** The test is not weak — it is measuring a quantity that does not vary with
the defect. And because it passes in both states nothing ever draws attention to it,
so it reads as coverage forever. A refusal test asserts *which* refusal; a
relocation test asserts the message only the new site emits.

**Prove the test can fail.** Neuter the fix, run it, and check that the failures are
the ones you expect *and only those*. Two of the five were caught exactly that way.
The clearest demonstration of the technique is elsewhere — pull request #353, which is
**not** one of the five — where neutering made the two positive cases fail while three
negative ones correctly still passed: **the asymmetry is the evidence**, since an
all-red run would equally be a harness that had stopped working. A green test nobody
has watched fail is an untested test — the sentence `compile-cache.md` applies to a
guard, applied here to the thing the guard is made of.

**Three of the five were acceptance criteria**, written by the person who understood
the defect best, at the moment they understood it best. That is not carelessness.
*What would prove this fixed* and *what would fail if it were not* are different
questions, and only the second one tests anything — so an acceptance criterion is a
hypothesis, and it is worth asking the second question separately before writing the
test that satisfies it.

Three entries in this file are instances of it: a bounded wait that must say which
KIND of failure it was, a fixture that states which PATH it exercised, and `SUCCEED`
standing in for a skip. **A harness earns its place by asserting something that can be
shown red** above is a fourth, and it already cites this ticket — it is where the rule
was applied before it was stated, including the asymmetry (four assertions failed while
the straight case correctly stayed green). Each was found on its own; none stated the
general form, which is why the fifth still had to be found by whoever happened to be
careful that day.

`Unwrap(x)` after `REQUIRE(x.has_value())` is deliberately **not** in that list. It
looks like one and is not: it exists because clang-tidy cannot see the guard through
`REQUIRE`, so a bare `*x` fails the build. That is a build-guard rule, and folding it
in would put one rule under two headings — which is what the census entry below means
by an instance filed where nobody looking for it will read it.

## A query that FAILED is not an observation about the subject

A required-context checker was written for the shape
[#542](https://github.com/LASTRADA-Software/fastcached/issues/542) names: read the
eleven names, report each one's state, and keep ABSENT distinct from pending so a
context that was never created cannot hide. Its first version captured
`gh pr checks ... 2>/dev/null`. `gh` was not on `PATH` in that environment, so
`command not found` went to the null device, the capture was empty, and the checker
printed **ABSENT for all eleven required contexts** — while the data sat there,
retrievable, one working invocation away
([#557](https://github.com/LASTRADA-Software/fastcached/issues/557)).

Keep the output, because the rule is easy to nod at and the failure is what makes it
stick: **eleven absent required contexts is catastrophic, plausible and
actionable-looking.** Nothing in it resembles a tool error. A reader would have gone
hunting for a workflow that had stopped dispatching. The instrument reproduced, one
level up, the exact defect it was built to prevent.

**A tool invocation's own failure is a state of the instrument, never a reading of
the subject.** An instrument that cannot express *I could not tell* will express it
as whatever its empty case means — and the empty case is usually the alarming one,
because instruments are written to make absence visible.

So a checker that can report *absent* must be able to report *I could not tell*, and
must refuse to say anything about the subject while that state is set. Zero rows is
not a verdict; it is the absence of one, and the **shape** of a response is checked
before any conclusion is drawn from it.

**The aggravating pattern to grep for is `2>/dev/null` on a command whose absence is
plausible** — a tool missing from `PATH`, which this repository hits routinely. That
redirection converts a missing binary into a confident finding.

This is the third shape of one family, and the other two are recorded in
[`build-and-toolchain.md`](build-and-toolchain.md): `producer | grep -q` under
`pipefail` reports absence *because* the symbol is present, and `| tail` returns the
pipe's status so a failed build reads as a passing one. There the query ran and its
status was misread; here **the query never ran at all**.

**And a failed query does not only MISS a finding — it can invent one, which is the
worse direction and the one that gets acted on.** Measured on `gh issue view`, which a
rulebook-staleness checker would resolve entries with:

```
number resolves to nothing   exit=1  stdout=0B   stderr: Could not resolve to an issue ...
repository does not exist    exit=1  stdout=0B   stderr: Could not resolve to a Repository ...
not a number at all          exit=1  stdout=0B   stderr: invalid issue format: "not-a-number"
a real issue (control)       exit=0  stdout=29B  stderr: —
```

Three faults, one exit status, one empty stdout — and **two of the three are faults in
the checker's own invocation**: a wrong `--repo`, a network failure, an expired
credential. A checker reading `exit != 0` as *this entry names a dead issue* therefore
reports a stale rulebook entry when its own argument was wrong, and somebody then
**edits a correct entry to satisfy a broken check**. Missing a finding costs a defect;
inventing one costs a correct file.

So such a checker has **four** outcomes, not two — pass, stale, bad reference, and
*could not run* — which is *skipped, absent, unstarted and failed* arriving inside the
tool written to enforce this rulebook. Prefer a route whose transport separates them:
`gh api repos/{owner}/{repo}/issues/N` answers with an HTTP status and hands back the
`pull_request` key in the same call, which also settles *is this number an issue or a
pull request* without a second request. And the suite needs a case for it: **a
deliberately broken invocation that must report `could not run` and must NOT report a
stale entry.** Nothing else proves the checker does not blame its subject for its own
faults.

The same rule reaches a measurement, where its direction is worth stating because it
is not symmetric. A counted loop that reports *N failures out of N* may be measuring
a subject that was never invoked — a stale binary, a mistyped filter, a case that no
longer exists — and Catch2 exits non-zero for `No tests ran` exactly as it does for a
failure. **A result containing at least one success is self-controlled against that**,
since a subject that was never reached cannot produce a pass. An all-failing result
therefore proves nothing until a positive control says the subject exists — and an
all-*passing* one needs the same control for the mirror reason, because a filter that
silently matched nothing also passes everything.

## A path-scoped revert is scoped to the FILE, not to the mistake

`git checkout -- <path>` took two completed review fixes with it — a `static_assert`
and a repaired documentation block — because they were uncommitted in the same file
as the botched scripted rewrite it was aimed at. Nothing reported the loss: the
revert succeeded, `git status` said *clean*, and *clean* was true.

It surfaced only because a canary written for an unrelated purpose then failed. Had
that canary passed for any other reason, the branch would have shipped without the
guard.

**Commit before running an experiment that edits the tree.** The red-proof scripts
here already refuse to start against a dirty tree, and this is the same rule
pointing at the recovery rather than at the setup. Where a revert cannot wait,
restore the specific hunks rather than the path. And afterwards check what came
**back**, not what went away: *validate the reverted tree* below is the same lesson,
and it did not fire here because the loss was in the file being reverted rather than
in the build describing it.

## Attribution by adjacency is a guess, and a guard built on one is a lottery

The same fixture reported `115 hit(s), 106 miss(es)` on a build of identical
source and passed, on the strength of "something hit". Either reading of that
number was available and nothing could choose between them: over half a build
missing is alarming, and it is also exactly what a correct cache does when the
third-party sources live *inside* the build directory and the two build
directories differ. Attributed, all 106 were third-party and all 74 of this
project's own units hit. The total had buried the only column that meant
anything.

The attribution is the part worth remembering. Reading the `[n/m] Building CXX
object ...` line above each outcome is the obvious move and is unsound: ninja
interleaves the output of concurrent edges, and two runs of the same build split
one unit differently — 74/41 against 75/40. A guard resting on that fails a build
for a scheduling accident, and would have been believed the first time it did.

**Ask the process, do not infer from its neighbours.** A one-line wrapper that
writes the unit to the same stream, from the same process, inside the same edge,
and then `exec`s the real program cannot be separated from the output it
labels — and an outcome that arrives with no label is counted as *neither* and
refused by name, rather than folded into whichever bucket was adjacent.

## A registration names a binary that may not have been built

**`$<TARGET_FILE:x>` naming a target that was not built is not a test that gets
skipped. It is a hard error at GENERATE time**, so the whole configure fails:

```
CMake Error at src/tests/CMakeLists.txt:943 (add_test):
  Error evaluating generator expression: $<TARGET_FILE:fastcached>
  No target "fastcached"
```

That is #390. A block registering a test that starts a binary is guarded on
**`TARGET x`** as well as on whatever feature makes the test interesting; the two
conditions are independent, because a feature can be enabled while the binary it
needs is not built. `tls-smoke` asked only `FASTCACHED_ENABLE_TLS` and the sccache
smokes only `SCCACHE`, so `-DFASTCACHED_BUILD_DAEMON=OFF` could not be configured
at all.

**It was conditional on the developer's machine rather than on the tree**, which is
why it survived. The sccache rows are themselves gated on finding sccache, so a
checkout without sccache installed never reaches the broken reference and the flag
appears to work. The error then names CMake rather than the option, on some
machines and not others — so the natural conclusion is "my checkout is broken".

And the rule was already written down. `src/apps/fastcache-cc/CMakeLists.txt`
spells it out in four lines directly above the block that obeys it, while two
blocks one directory away did not. A rule one file explains and another ignores is
a rule that needs a check, not a better comment: `ctest -R target-file-guards`
(`scripts/check-target-file-guards.cmake`), in the default set. It reads which
targets are optional from `src/apps/CMakeLists.txt`'s app table rather than
restating them, so a new app comes under the guard by existing.

Three ways it refuses to pass vacuously, because each is how such a check reports
success for work it did not do: an app table it could not read, a scan that found
no reference at all, and a file whose `if` nesting it could not follow to the end.
All three are named failures.

## A fixture whose client is always local cannot test who is admitted

`scripts/dist-compile-e2e.sh` had twelve cases, three real processes each, and every
leg of every one of them was loopback. `ClusterMembership::Classify` admits loopback
**before** it consults a member list — deliberately, because that is what makes an
unconfigured node useful on its own machine — so the branch underneath was reached by
nothing:

```cpp
if (IsLoopbackHost(peerAddress))
    return Membership::Member;                            // every case in the suite
...
return any_of(_hosts, SameHost) ? Member : Outsider;      // nothing at all
```

That is how [#235](https://github.com/LASTRADA-Software/fastcached/issues/235)
survived. A worker that admitted **only** its own machine — and therefore refused
every dispatched compile the documented setup sent it — passed the whole distributed
suite, because the suite only ever asked it to admit its own machine. The fixture
proved dispatch works and could not have noticed that dispatch works for nobody else.

**The cheap fix does not work, and it looks exactly like it does.** A second loopback
address is the obvious move and was the ticket's own first suggestion:
`IsLoopbackHost` matches `127.` rather than `127.0.0.1`, and says so in its own
comment, so a client on `127.0.0.2` takes the same early return. The fixture would
change, the addresses in the log would differ, and the test would go on proving what
it proved before — which is the shape of the defect, applied to its own repair.

**What reaches the branch is the host's own non-loopback address**, and it needs no
second machine: bind the worker there, let the scheduler grant that endpoint, and the
connection the worker accepts arrives from outside `127/8`.

Four things about the shape, and the last two are the ones that generalise.

- **Assert both directions; only the pair proves anything.** Listed in
  `--fleet-member` → dispatched and served. Absent → refused `not-a-member`, the build
  compiles locally, and `fastcache_worker_jobs_refused_not_a_member_total` **moves** —
  the assertion #235 needed and did not have. The admitted leg alone passes over
  loopback too, since loopback is admitted above the list, so it cannot show the
  address ever reached the list; the refusing leg is what shows it, because a loopback
  peer would have been admitted there as well. Measured, with the address forced back
  to `127.0.0.1`: the admitting leg stays **green** and the refusing leg goes red. That
  asymmetry is the finding — one of the two legs cannot tell the difference, which is
  why a suite made of legs like it reported nothing for twelve cases.
- **The scheduler admits the client in both legs.** #235's shape is a lease that is
  granted and then a worker that refuses it, which is invisible from the side anybody
  watches — no scheduler counter moves. A refusal arriving from the scheduler instead
  is a different test passing under the same name, so the client-side assertion matches
  `refused the job: rejected (not-a-member)` rather than the error code alone.
- **The baseline is not zero, and pretending it is hides the instrument.** The
  fixture's own `wait_for_port` dials the compile port from that same address, so the
  refusing leg has already refused one caller before its compile runs. That reading is
  taken *after* startup and the compile is asserted as a **delta** from it — and its
  being at least one is itself an assertion, because a zero there would mean the probe
  arrived as a loopback caller and the leg proves nothing.
- **A machine with no non-loopback address reports SKIPPED, loudly.** This is the only
  body of assertions in that file with a prerequisite a host may lack, which is why it
  is its own ctest test (`ctest -R dist-compile-membership-e2e`) rather than a
  thirteenth case: a script exits once, so a case inside the suite could do no more
  than print a line and let the run go green. A quiet fall back to loopback would be a
  pass reported for a case that never ran — this ticket's own failure mode, wearing the
  hat of its fix.

## A fixture must state which PATH it exercised

`check-catch-skip-return-code` derives the files it scans from `git ls-files`, and
falls back to a directory walk where there is no git index. Its selftest had six cases
and they all passed. CI then failed on a vendored registration the check should never
have seen — and there was no contradiction between those two facts:

> a synthetic tree is not a git repository, so all six cases exercised the **fallback**
> path while CI exercised the **git** path — the mode under test was not the mode in
> use.

**A guard that passes because it is testing something else.** Nothing about the suite
could have revealed it: every case was correct, every assertion was real, and none of
them ran the code that runs in production.

- **Any code with a primary path and a fallback needs its fixtures to say which one
  they exercised**, or the cheap-to-construct path silently becomes the only one
  tested. A synthetic tree with no git index is the easy fixture, so it is the one that
  gets written — and it is the one CI never runs.
- **The mode is therefore part of the output**, not an internal detail: the check
  prints `via git ls-files` or `via directory walk (no git index)`, and the fixtures
  assert it. Without that, a silent fall back to the walk passes for the wrong reason,
  which is the same failure one level down.
- **Assert it on BOTH sides.** Covering only the path that broke swaps which half is
  unexercised rather than fixing it. One case requires the walk and one requires git,
  so neither can quietly become the only one tested — and each is shown red by making
  the check misreport its mode.
- **The fallback is exercised where it is actually used, too.** A real `git archive`
  export has no `.git`, takes the walk, says so, and reaches the same verdict — which
  is what makes the fallback sound there and unsound in a working checkout, where a
  dependency cache lives inside the source tree.

## A SKIP that ctest scores as a failure

A Catch2 case that calls `SKIP(...)` exits **4**, and `SKIP_RETURN_CODE` is the only
thing that tells ctest an exit code means *skipped* rather than *failed*. None of the
five `catch_discover_tests` registrations set it, so the binary printed `1 skipped`
and ctest printed `***Failed` **for the very same run** (#499).

- **Measured, not inferred.** One forced skip: `test cases: 1 | 1 skipped` with
  `BINARY EXIT: 4`, ctest `***Failed`; after `PROPERTIES SKIP_RETURN_CODE 4`,
  `***Skipped` and `100% tests passed`.
- **It is a false RED, which is the insidious direction.** All seven `SKIP` sites in
  this tree are environment-conditional — three on `AvailableCodecs().size() < 2`,
  four on being able to bind loopback — so they fire on a constrained runner or a
  build configured without codecs and report a regression that is not there. Nobody
  had hit it because those conditions had not arisen on CI, which is **luck rather
  than coverage**: the codec skips depend on how the build is configured and the
  loopback ones on what the runner allows, and neither is a property of the code.
- **It makes `SKIP` unusable as a tool.** Whoever adds one, watches the suite go red
  and concludes the skip was wrong will delete the skip rather than suspect the
  registration — the wrong lesson, learned permanently, by someone who did nothing
  wrong.
- **The value is part of the rule.** `77` is the GNU convention the eleven
  script-driven `add_test` registrations use, because a shell script picks its own
  exit code. A Catch2 binary does not: it exits **4**. `SKIP_RETURN_CODE 77` on a
  Catch2 registration is present, reads correctly in a diff, satisfies any
  "is the property there" scan, and still scores every skip as a failure.
- **A comment was never what was missing.** The mechanism was already written down in
  `src/tests/CMakeLists.txt` and applied eleven times, one directory away from five
  registrations that did not apply it. So the guard is a check —
  `ctest -R catch-skip-return-code`, reading the registrations from the tree rather
  than restating them — because **a sixth test binary would reopen this by omission
  exactly as the fifth did**.
- **`catch_discover_tests` writes its list at BUILD time.** A `POST_BUILD` step, so
  editing the CMakeLists and reconfiguring is not enough: measured, the old list
  survived a full successful `cmake --preset` and the case still reported `***Failed`;
  only a REBUILD turned it into `***Skipped`. **A stale result reads exactly like a
  current one**, so verify this property by hand only after rebuilding the target.
- **The guard is shown failing on every direction it claims**
  (`ctest -R catch-skip-selftest`): a bare registration, a wrong value, a
  registration split across lines that must NOT be reported, and a tree with no
  registrations at all, which must report a broken scan rather than a clean tree. Its
  own first version walked into `_deps/catch2-src` and reported vendored third-party
  code — `file(GLOB_RECURSE)` recurses from the base directory and treats only the
  last component as a pattern, so `src/*/CMakeLists.txt` does not mean one level down.

## A `SUCCEED` where the case could not RUN is a false GREEN

The converse of the rule above, and the direction nobody investigates. `SUCCEED("...")`
records a **passing assertion**, so a case that bails out because its environment could
not be arranged reports a pass for a property nothing established (#685).

**`SUCCEED` is right when the case RAN and there was nothing to assert. It is wrong when
the case could not run.** That is the whole distinction, and `SKIP("reason")` is the
whole fix — it exits 4, every `catch_discover_tests` registration carries
`SKIP_RETURN_CODE 4`, and ctest scores it as skipped.

- **It fires where coverage is already thinnest.** All twenty-one sites converted for
  #685 were environment-conditional: no loopback listener, no IPv6 stack, no symlink
  privilege on a Windows agent, running as root, a test port already bound. Those are
  precisely the runs on which a green is least justified and most believed — a
  constrained CI runner reports the same colour as a full one, and nothing distinguishes
  them.
- **The four states again.** Skipped, absent, unstarted and failed. A case that could not
  run is *skipped*, and rendering it as *passed* collapses two of them at exactly the
  layer that decides whether anybody looks.
- **A comment does not stop it, because it spread by IMITATION.**
  `DirectManifest_test.cpp` carried the idiom under a comment that was *correct* — it
  said the rule being demonstrated is unconditional, which is the reason the `SUCCEED`
  was wrong — and the next test written was copied from it, comment and all. The
  argument was there to be read and was not the thing being copied.
- **What a check can see, and what it cannot.** `ctest -R succeed-not-skip` fires on two
  signals: a `SUCCEED` whose next statement is a bare `return` — on the following line or
  on its own, since `if (!ready) { SUCCEED("x"); return; }` is the same defect written in
  one — and a message in skip vocabulary, "unavailable", "could not", "privilege",
  "exercised by", held as a table with one row per phrase. A silent bail-out with a
  message in neither vocabulary and no `return` is invisible to it, and so is a `SUCCEED`
  sitting between two block comments on one line, because CMake's regex engine is greedy
  and strips from the first `/*` to the last `*/`. Those limits are stated in the script
  rather than papered over: the check closes the door the defect came through, and this
  rule covers the rest.
- **A check that refuses its own documentation has no escape hatch.** This rule is
  written out in prose that quotes the idiom, and test files cite it in comments. The
  scan therefore strips `//` comments and tracks `/* ... */` blocks across lines — a
  leading-`//` test is not enough, because a block written without leading stars carries
  whole paragraphs and its body is exactly what a historic note looks like. A contributor
  tripped by that could make the build green only by rewording the comment.
- **A `SUCCEED` that NAMES a mechanism reads like an assertion about it, and is not one.**
  `BlockingSocket_test.cpp`'s `SO_NOSIGPIPE` arm said "SO_NOSIGPIPE arms the descriptor
  here, so a raw sender needs no extra flag" and observed nothing — while the checkable
  fact was one line away, `NoSignalSendFlags() == 0`, and is the fact the case exists for:
  `MSG_NOSIGNAL` may be *defined* by a macOS SDK while `CMAKE_OSX_DEPLOYMENT_TARGET` lets
  the binary run on an older kernel, and a flag the kernel does not know fails the send.
  Widening that arm is the exact regression, and it would have reported a pass. It was
  classified as legitimate twice — in the ticket and by the sweep — and only review caught
  it, by reading the code rather than the message. **The two signals a check can see do
  not reach this shape**: no bail-out `return`, no skip vocabulary. Which is why the rule
  is the thing that must be internalised and the check is only the door the defect came
  through.
- **"Covered by another test" is a reason to SKIP, never a reason to pass.**
  `ServiceControl_test.cpp` deliberately does not call `InstallService` on Windows and
  macOS — a unit test must not register a service on the host — and said so in a
  `SUCCEED`. The end-to-end scripts really do cover it; *this* case still observed
  nothing.
- **Inconclusive is refused, not passed.** A `SUCCEED` the check cannot read to a closing
  parenthesis on one line is reported as its own outcome. A site it could not read is not
  a site it cleared.
- **The guard is shown failing on every direction it claims**
  (`ctest -R succeed-not-skip-selftest`): thirteen synthetic trees, one verdict each, and
  each refusal asserted by the phrase naming *that* refusal — so a mutation that reddens
  everything fails the selftest instead of looking thorough. Measured against six mutants:
  removing each of the bail-out, one-line, block-comment and deleted-file guards reddens
  exactly its own case, emptying the vocabulary reddens the message-driven ones, and
  refusing everything reddens all seven cases that assert silence. Seven of the thirteen
  exist only to assert the check stays **quiet** — on `SUCCEED()` with no message, on a
  `SUCCEED` that ends a case, and on the idiom quoted in a comment of either kind.
- **A fixture that could not be STAGED says so.** The selftest's git cases build a real
  repository, and under a long scratch path `git add` failed with "Filename too long" —
  the tree then had no index, the check fell back to the directory walk, and two verdicts
  came back wrong for a reason that had nothing to do with the check. Every git command
  the fixture runs is checked, and a case that could not be arranged reports *that*,
  naming what git said. The scratch directory is also the root rather than a directory
  inside it, which is one long path component fewer.

## A leader-pinned command goes to whoever leads NOW

`$leader_endpoint` is derived by whichever section of `cluster-e2e.sh` needed it
first, and leadership may legitimately move before a later section runs. A cluster
that has ELECTED is not one that has FORMED, a slow runner blows any election
timeout, and admission puts the cluster back into the state with no slack. So a
command put to the node that led a moment ago is a command put to a node that now
answers "ask somebody else" -- and the fixture reports that as the cluster refusing
something legitimate.

Two halves, and the second is the one that gets lost.

**A leader-pinned MUTATING command is put to whoever leads now, re-derived, never
to an endpoint recorded earlier.** Reads are exempt: any node answers
`--cluster-status`, and the one read that is leader-pinned re-derives inside its own
loop.

**The retry keys on the answer the caller ASSERTS, never on a recognised refusal
wording.** That refusal has two spellings -- one for "somebody else leads", one for
"an election is in progress" -- and a fixture matching them stops retrying the day
either sentence is reworded, silently, and back to failing the way it used to. The
contract is therefore "the answer carries this substring", never "the command
succeeded", which is also what lets the same helper assert a REFUSAL: the typo'd
setting case asserts that an unknown setting is refused BY NAME, and its substring
is the typo.

What makes this one worth writing down is where the rule already was.
`submit_setting` **was** this rule, correct in both halves, at line 778 of the same
file -- 117 lines above the `--cluster-admit` that lacked it and 212 above the
`--cluster-forget`. The argument was written down in a comment above it before
either of the sessions that read the file noticed the sites it did not cover.
Learned at [#117](https://github.com/LASTRADA-Software/fastcached/issues/117),
learned again at [#172](https://github.com/LASTRADA-Software/fastcached/issues/172).
A helper that implements a rule does not spread it; only a call site using the
helper does.

## A first failure masks its identical siblings

`--cluster-forget` in section 5 of `cluster-e2e.sh` was one-shot against a pinned
leader endpoint, byte-identical in shape to the `--cluster-admit` in section 4, and
had **never once been observed failing**. Not because it was sound: because the
admit fails first. Under a moving leadership section 4 always failed, so section 5
was never reached, and the flake's second instance sat behind the first with a clean
record.

Two consequences, both worse than the original defect.

**Fixing only the observed site relocates the flake rather than removing it.** The
admit is repaired, the fixture gets further, and the forget starts failing at the
rate the admit used to.

**The relocated one then presents as a regression introduced by the fix.** It has no
failure history, it appears in the run immediately after the change, and everything
about it points at the person who just fixed the thing above it. That is expensive
to diagnose and it discredits a correct fix.

Which is the argument for auditing rather than trusting instinct: the sibling was
found by walking every call site of the pattern, and it was found *because* the walk
was mechanical. Nothing about section 5 looked suspicious, and it never had been.
When a failure is fixed, the question is not "is this site now correct" but "what
else has this shape, and would I have seen it fail?"
([#172](https://github.com/LASTRADA-Software/fastcached/issues/172))

## An identifier is READ, never reconstructed, and zero rows is not a verdict

Two instrument failures in one session shared one shape, and neither returned an
error. A query was answered about a set that did not contain the subject, and the
caller promoted the empty result into a verdict
([#683](https://github.com/LASTRADA-Software/fastcached/issues/683)).

**A fabricated SHA.** A lane read a head as the abbreviated `b4331297`, needed forty
characters for an API call, and **padded it** instead of reading the full value. `gh`
answered `422 No commit found`; the script piped that into `awk`, the one-line error
JSON matched no red rows and produced no pending rows, and the watcher reported
**"ALL COMPLETE, NOTHING RED"** for a commit that does not exist.

What makes that one expensive is that the fabricated SHA shares its prefix with the
real one, so **every human-readable trace of the mistake reads correctly**. The
transcript, the log line, the summary a reviewer skims -- all of them show the eight
characters that are right. Only the raw response disagrees, and the raw response is
the one thing nobody reads.

**An absence nobody searched for.** In the same session a CI log was grepped for the
ctest summary block, fixture output was not found *there*, and "CI produces no test
output" was reported as a fact. `CMakePresets.json` sets
`"output": { "outputOnFailure": true }` on every test preset, and the evidence was
already in the log file on disk. A lane then spent a cycle diagnosing a fixture
defect that was not the failure.

So:

- **An abbreviated identifier is a display form.** The full one is read, never
  reconstructed. Padding, truncating or re-deriving one produces a well-formed value
  for a thing that does not exist -- which is strictly worse than a malformed value,
  because a malformed value is refused.
- **Validate a response's SHAPE before drawing a verdict from it.** N rows with the
  fields you expect, or it is not an answer. A query that does not parse is a hard
  failure, never a quiet retry.
- **Zero rows is not a verdict, it is the absence of one.** This is the same
  distinction the required-context scan draws between absent and passing, and the
  same one a `SUCCEED` gets wrong when it stands in for a skip: an instrument that
  cannot tell "no answer" from "a good answer" reports the good one.
- **Refuse a malformed identifier at the boundary.** A forty-hex-character check is
  one line, and the padding that caused this is invisible in a shell argument.
- **Before concluding "nothing there", state what was searched and whether that
  search could have found it.** "I grepped the log for the ctest block" and "CI
  produces no test output" are different claims, and only the first one was made.
- **A finished job's log is available while its RUN is still in progress — and the
  CLI says otherwise.** That is the fact; everything below is how to act on it.
  It matters because the moment somebody wants a failing job's log is precisely
  the moment the rest of the run is still going, so the tool's refusal arrives
  exactly when it is most convincing and least true.

  ```
  gh api repos/<owner>/<repo>/actions/jobs/<id>/logs --allow-escape-sequences
  ```

  **A tool that will not GIVE you the answer says so in a sentence that reads like
  "there is no answer".** Both of these are false negatives and neither is an
  error:

  ```
  gh run view --job <id> --log
      -> "run is still in progress; logs will be available when it is complete"
         ... while the log is retrievable RIGHT NOW by another route

  gh api repos/<owner>/<repo>/actions/jobs/<id>/logs
      -> a 99-byte refusal about terminal escape sequences, which looks like an
         empty result
  ```

  The log was available the whole time. Cost of not knowing this: an hour treating a retrievable log as gated, and an
  intermittent attributed to three successive wrong hypotheses while the answer
  sat one flag away. **Two different messages, both of which say "nothing here",
  neither of which means it** — which is this rule's own subject arriving in the
  tool you reach for to apply it.

This is the `producer | grep -q` false negative's family, recorded in
[`build-and-toolchain.md`](build-and-toolchain.md) -- same outcome, different
mechanism. There the pipeline corrupted the answer; here the caller corrupted the
question.

### A third instance, found while writing this rule

Worth recording because of when it happened. Auditing whether any Catch2 case name
contains a comma -- which would be a spec metacharacter -- the census was:

```sh
git grep -h '^TEST_CASE("' -- 'src/**/*_test.cpp' | sed 's/^TEST_CASE("//'   | sed 's/".*$//' | grep -c ','
0
```

Zero of 3135. The real answer is **428**, and one of them is at
`src/CowTree/BasicCowTree_test.cpp:285`.

This repository sets `grep.lineNumber = true`, so `git grep -h` emits
`<lineno>:<content>` rather than `<content>`. The `^TEST_CASE("` anchor therefore
matched nothing, the first `sed` stripped nothing, and the second truncated at the
first quote and left `57:TEST_CASE(` -- a string with no comma in it. Every line
produced an answer, the count was well formed, and it was false.

Three properties made it convincing, and they are the ones to distrust:

- **The pipeline did not fail.** No stage errored, no stage was empty, and the
  arithmetic was over 3135 real lines.
- **Zero was the expected answer.** A tree with no comma in any case name is exactly
  what a healthy tree looks like, so the result confirmed the prior.
- **The defect was in the environment, not the command.** The same pipeline is
  correct on a machine without that config, which is why reading it does not reveal
  anything.

What caught it was cross-checking with a second extraction that had no shared
assumption -- a Python `re.search` for the first quoted string -- and then confirming
the disagreement against a raw `git grep` of one name. **A census that returns zero
gets a positive control**: find one instance by other means, and check the census
sees it. Otherwise "there are none" and "I did not look where they are" are the same
output.

## Open work

- **[#147](https://github.com/LASTRADA-Software/fastcached/issues/147)** — two
  scratch-directory helpers still shadow `Testing::ScratchDirectory`, in
  `PathResolve_test.cpp` and `Stats_test.cpp`. Both correct today; the shape is what
  has been copied wrong before.
- **[#627](https://github.com/LASTRADA-Software/fastcached/issues/627)** —
  `launcher-replay-e2e.sh` keeps its own `fail`, carrying the
  `[ "${BASHPID:-$$}" = "$top_pid" ]` guard the bash-3.2 table above bans by name:
  correct on bash 4, silently inert on macOS 3.2, where a `fail` inside `( ... )`
  then ends only the subshell. Allowlisted in `check-e2e-helpers.sh`'s
  helper-collision scan until it takes the shared `fail`.
- **[#628](https://github.com/LASTRADA-Software/fastcached/issues/628)** —
  `migrate-storage-e2e.sh` keeps its own `fail` and, worse, spells `free_port` as
  `port` and draws 40000–59999, entirely inside Linux's default ephemeral range.
  The name scan cannot see the second one, which is why its allowlist row says so
  in prose.
