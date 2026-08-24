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

## The shared helpers: `Unwrap` and `ScratchPath`

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

