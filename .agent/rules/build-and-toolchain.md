# Build system, toolchain and language pitfalls

Rules about the things that differ between compilers, standard libraries, hosts
and tool versions — and about the local gate that exists to catch them before CI
does.

Read this before changing a header everything includes, a randomness or timing
seam, `cmake/portable/CompileCache.cmake`, or anything a test harness's
determinism rests on.

## The local gate

`scripts/local-gate.sh` is the gate. Run it before pushing.

- **A hygiene script `ctest` runs is constrained to bash 3.2, because macOS ships
  bash 3.2.** Apple has not shipped bash 4 since the licence change, so `/bin/bash`
  on the macOS runner is from 2007. A script registered in the **default** ctest
  set runs on every platform CI builds, and the constraint is invisible from the
  Linux box such scripts are written on. The bash-4 constructs to avoid are few
  and worth knowing by name:

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
  recorded. `scripts/tidy-sweep.sh` uses `mapfile` and `declare -A` and is fine, but
  only because the `clang-tidy` job pins `ubuntu-24.04` -- so moving that logic into
  a script `ctest` runs would break it the same way. The same holds for the
  `mapfile` and `declare -A` inside `pr-labels.yml`.

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
    questions, because they fail separately: `__tsan_init` in the **test binaries**
    says *this artefact* was instrumented, and `src/tests/TsanCanary.cpp` -- a
    deliberate data race, built by the same `add_compile_options` as everything else
    -- says the runtime still detects and reports. A `TSAN_OPTIONS`, a suppressions
    pattern or a stripped runtime can break the second while the first still holds.
    The canary is run **with the suppressions file active**, so a wildcard broad
    enough to swallow an obvious race fails the gate instead of silently disarming
    it. Every refusal has a message of its own, because each is fixed somewhere
    different, and each was verified by making it happen -- the list lives in that
    script's header and deliberately nowhere else. It said "five refusals" in three
    files, in three orders, having dropped the two hardest to reason about: a hard
    count restated beside the thing it counts is a fact with no owner.
  - **A filter that matches nothing is a suite that tested nothing**, and every
    other signal in that run reads clean. The gate names it, and separately refuses
    an exit of 0 that reported no assertions. This is the same shape as a sweep that
    skips files and a `-header-filter` that matches no path: the tool ran, the
    artefact was fine, and *nothing was examined*.
  - **`producer | grep -q` is a false NEGATIVE under `set -o pipefail`, and it fails
    on the SUCCESS path.** `grep -q` exits the instant it matches, which closes the
    pipe; the producer is killed by SIGPIPE; `pipefail` then takes the producer's
    status and the pipeline reports failure. So `nm "$bin" | grep -q __tsan_init`
    says "no such symbol" precisely *because* the symbol was there. Capture the
    output into a variable and match afterwards
    (`syms="$(nm "$bin")"; [[ "$syms" == *__tsan_init* ]]`), or drop `-q`. This is
    not specific to `nm`: **every "does this artefact contain X" idiom in this tree
    is exposed to it** -- symbol checks, `strings | grep -q`, `objdump | grep -q`,
    any long producer feeding an early-exiting matcher. It bit `scripts/tsan-gate.sh`
    itself, which is the script written to catch exactly this class of thing, and
    that is the first time here that the *checking mechanism* produced the false
    reading rather than the thing being checked.
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
- **A third leg rather than replacing a Release one, and that was measured.**
  Windows costs ~9 and ~7 minutes today; the 21.2 and 15.7 quoted in
  `build.yml`'s own `windows:` comment predate `SCCACHE_GHA_ENABLED` and are stale
  — that comment states them in the past tense, and a reader skimming it for a
  current figure will take the wrong one. Neither leg is near the
  critical path — `clang-tidy` runs ~20 minutes and finishes last — so the Debug
  leg costs about eight runner-minutes and no wall clock. Giving up a Release
  configuration to buy that would have traded coverage for capacity already there.
- **Guarded to `MSVC AND CMAKE_BUILD_TYPE STREQUAL "Debug"`, which means its
  absence elsewhere is normal.** That is worth knowing before debugging it: a
  platform-guarded registration is indistinguishable from a lost one when you look
  at a single platform's `ctest -N`. Check a listing where it *should* be missing
  and one where it should not, and confirm a neighbouring test is still present.

## What CI costs

The workflow's *critical path* is the longest single job, and for a long time that
was one job: `clang-tidy`, at 28.6 minutes out of a 28.6-minute workflow (run
`33243524509`, master, green). Everything here was measured on that run, and the
numbers are kept because each one is the reason a knob is where it is.

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
  `packaging/` change breaks — and `coverage` is not on the critical path. The
  ~17 runner-minutes are real; buying them with a break that reaches master is not
  a trade. `check-release-gate` would not have objected: it asserts the *list* in
  `release.needs`, not that each job runs, so a skipped job still gates. The refusal
  is on the merits, not on the guard.

- **A WSL build belongs under `~`, not under `/mnt/d`, and the cost of getting it
  wrong is threefold.** DrvFs is a 9p bridge to the Windows filesystem, and a build
  is almost entirely small-file I/O. Measured on this machine — same commit, clang
  20, Debug, `USE_COMPILER_CACHE=OFF`, `-j8`, target `fastcached`, a warm configure
  so only the compile is timed:

  | | ext4 (`~`) | DrvFs (`/mnt/d`) | |
  |---|---|---|---|
  | build, wall | **19.6 s** | **60.5 s** | 3.1× |
  | build, user CPU | 125.2 s | 127.0 s | 1.01× |
  | build, sys CPU | 6.9 s | 19.6 s | 2.8× |
  | open+read every `*.hpp` | 0.11 s | 1.02 s | 9.3× |

  The user time is the number that settles it: the two builds did **the same
  compute**, to within 1.5%. Everything DrvFs costs is spent waiting on the
  filesystem, which is also why the header walk — pure I/O, no compute — is the
  worst ratio of the four, and why `sccache-backend-caveat` times out on `/mnt/d`
  without anything being broken.

  This is not a preference, because there is a second, harder reason: a
  **Windows-created worktree is unreadable by WSL git at all** — its `.git` file
  holds a drive-lettered path, so `scripts/local-gate.sh` dies and
  `repository-hygiene` silently skips inside one. A WSL build needs a WSL-created
  checkout, and the moment you are making one anyway, make it under `~`.

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
  file reports nothing at all. Configure a throwaway tree:

  ```sh
  cmake -S . -B out/build/tidy22 -G Ninja \
        -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
        -DFASTCACHED_ENABLE_TLS=ON
  ```

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
## `USE_COMPILER_CACHE` in full

`USE_COMPILER_CACHE` (default ON, `cmake/portable/CompileCache.cmake`) fronts the compiler
with our own `fastcache-cc` when it is on `PATH` and a daemon answers — at
`127.0.0.1:6674` by default, or wherever `FASTCACHE_ADDR=host:port` points;
`FASTCACHE_SOURCE_DIR`/`FASTCACHE_BINARY_DIR` are injected from the source and build
trees. Configure proves the cache works by compiling one tiny file through the
launcher (~0.1 s) and requiring a `HIT`/`MISS`, because a launcher that cannot
reach its daemon still compiles fine and would otherwise cost every TU a failed
connect in silence. When nothing answers it falls back to `sccache` — which is
not free, see below. When
*nothing* is installed, `-DFASTCACHE_AUTO_INSTALL=ON` (default OFF) fetches a
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
  same absolute path replay paths that are correct — CI is normally both, which is
  why the fallback stays automatic rather than becoming opt-in: downstream projects
  run CI with no daemon reachable on purpose and are not exposed. Passing
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
`Windows-cl-release`, `Windows-clangcl-release`, `Linux-clang-release`,
`Linux-gcc-release`, `macOS-clang-release`, `clang-tidy`, the three
`sccache smoke (...)` jobs, `Check C++ style` and `Require a type label`. A
workflow-level path filter stops the workflow from triggering, so no check run is
ever created for any of those, and the pull request waits on a context that will
never arrive.

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

## Open work

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
