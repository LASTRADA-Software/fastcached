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
that runs after five cases' worth of connections. Every fixture now draws
20000–31999, below that floor and below macOS's 49152.

A fixture that reports "free" from a probe is answering *"is anything answering
here"*, which is not the same question as *"can I bind here"* — and the gap between
the two is exactly where a smoke test fails for a reason that has nothing to do
with what it tests.

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
below; the fourth was a repository bug the fixture merely reached first, recorded
separately as #390. They are recorded together because the shape repeats:
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

## Open work

- **[#147](https://github.com/LASTRADA-Software/fastcached/issues/147)** — two
  scratch-directory helpers still shadow `Testing::ScratchDirectory`, in
  `PathResolve_test.cpp` and `Stats_test.cpp`. Both correct today; the shape is what
  has been copied wrong before.
