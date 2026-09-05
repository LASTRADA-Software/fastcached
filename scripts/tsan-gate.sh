#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Run the concurrency-bearing tests under ThreadSanitizer, and refuse to report a
# clean result unless the sanitizer is proven to be instrumenting and reporting.
#
# This is what the `clang-tsan` CI job runs. It can be run locally against any
# build directory configured with the `clang-tsan` preset:
#
#   cmake --preset clang-tsan
#   cmake --build --preset clang-tsan --target FastCacheTest fastcache-compile-node-tests tsan-canary
#   scripts/tsan-gate.sh out/build/clang-tsan
#
# ## Why this is a script rather than a `ctest` line
#
# Three reasons, in descending order of how much they cost when ignored.
#
# **A clean sanitizer run is not evidence until something proves the sanitizer
# would have gone red.** `cmake/portable/Sanitizers.cmake` documents a defect on
# this repository where the cache read ON, the configure log printed "Enabling",
# and `add_compile_options` received no `-fsanitize=` at all -- so the sanitizer
# presets had never once run a sanitizer, locally or in CI, and every signal an
# author would check said yes. A `ctest` invocation cannot tell that apart from a
# tree with no races. This script can, and refuses to.
#
# It asks that question twice, because it is two questions. An undefined
# `__tsan_init` reference in each artefact's OWN OBJECT FILES answers "was this
# artefact instrumented" -- the artefact actually under test, not some other build
# in the same tree, and not the sanitizer runtime the link pulled in. The canary
# answers "does the runtime detect and report" -- which `TSAN_OPTIONS`, a
# suppressions file, or a stripped runtime can each break while leaving the
# instrumentation intact. Neither substitutes for the other.
#
# The first question used to be asked of the BINARIES, and could not be answered
# there: `__tsan_init` is DEFINED by the runtime, so it is present whether or not
# any translation unit was instrumented (#472). See `AssertInstrumented`.
#
# Every way this refuses has a message of its own, because each is fixed
# somewhere different, and each was verified by making it happen rather than
# reasoned about:
#
#   the build directory does not exist    -> "build directory not found"
#   the suppressions file does not exist  -> "suppressions file not found"
#   a path here contains a space          -> "cannot carry a path with a space"
#   `nm` is unavailable                   -> "nm is required"
#   no COMPILE line carried -fsanitize    -> "instrumented nothing"
#   a target is not built                 -> "not built"
#   a target's object dir is missing      -> "no <target>.dir under"
#   two dirs could be a target's objects  -> "cannot tell which is current"
#   a target has no objects at all        -> "no object files under"
#   an object's symbols cannot be read    -> "nm could not read"
#   an object has no undefined __tsan_init-> "built WITHOUT ThreadSanitizer"
#   the canary does not go red            -> "the deliberate race was NOT reported"
#   the canary dies of something else     -> "reported no data race"
#   the canary runs past its deadline     -> "tsan-canary did not finish within"
#   the canary cannot be executed         -> "nothing proves the sanitizer reports"
#   a tag expression matches nothing      -> "tested NOTHING"
#   a target exits 0 with no assertions   -> "refusing to call that clean"
#   a target runs past its deadline       -> "did not finish within"
#   a target cannot be executed           -> "ran no cases at all"
#   an unsuppressed race                  -> "reported an unsuppressed data race"
#
# The right-hand column is what a reader GREPS the output for, so each phrase is
# contiguous in the message it names -- a phrase the code only emits across a
# newline and an indent identifies nothing. Which is also why the target's
# unstartable row says "ran no cases at all" and not the more natural "tested
# NOTHING": that string already discriminates the tag-expression row four lines
# up, and two refusals sharing the one phrase this list tells them apart by would
# make the list stop being able to.
#
# The last three arrived with #488, and each has a `--self-test` case. Not "as
# the rest do" -- five rows here have none, and saying otherwise would be this
# gate's own defect in the prose describing it. The canary rows exist because a
# killed canary used to fall through to "reported no data race", a different
# fault with a different fix, so it needed a row rather than a rewording.
#
# The list is enumerated HERE and nowhere else. An earlier version of it said
# "five refusals" in three files, in three different orders, having silently
# dropped the two that are hardest to reason about -- which is the same shape as
# every other defect this gate is about, in the prose describing it.
#
# The "tested NOTHING" one is what looks like paranoia and is not: a typo in the
# TARGETS table below runs zero cases, and every other signal in the run says
# clean.
#
# **The suite is scoped, and `ctest` has no vocabulary for the scope.** The
# concurrency in this tree is in `Async`, `Consensus`, `Distributed` and the node.
# `catch_discover_tests` registers cases by NAME and this project's Catch2 (3.6)
# predates `ADD_TAGS_AS_LABELS`, so there is no ctest label to select on and the
# scope has to be a Catch2 tag expression (issue #312). Running the two binaries
# directly also collapses ~700 sanitized processes into two: measured at 1.2s and
# 17.6s against several minutes of per-process runtime startup.
#
# The tag list is NOT self-evidently the right one, and an earlier version of it
# was wrong: `[async],[consensus],[distributed]` looks complete and silently
# excluded six of ten `Async/` test files, because the reactor and coroutine tests
# are tagged `[reactor]` and `[task]` and carry no `[async]`. It matched 511 cases
# and they passed. Nothing in the run could have said otherwise.
#
# So the tag list is not trusted here. `scripts/check-tsan-scope.cmake` runs in the
# default ctest set and fails when a test file in one of those directories carries
# no tag this expression selects. It reads the expression out of the TARGETS table
# below rather than restating it, so the two cannot drift apart: a tag removed
# from that table is a tag that check stops accepting, on its next run.
#
# **Known-open races need somewhere to live.** `.tsan-suppressions` is that place
# and every entry names an issue; see the header of that file for why an entry is
# a liability rather than a fix.

set -euo pipefail

BUILD_DIR="${1:-out/build/clang-tsan}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUPPRESSIONS="${REPO_ROOT}/.tsan-suppressions"

# For `run_bounded`, `e2e_bound_outcome` and the outcome names only; see the
# bound below for why this gate stopped resolving `timeout(1)` for itself (#488).
# This is not an e2e fixture and uses none of the rest -- no port is drawn and no
# daemon started -- so it keeps its own `fatal` and its own `note` rather than the
# library's `fail` and `e2e_note`. That is a decision and not an oversight:
# `fatal` annotates for GitHub Actions where `fail` does not, and `note` prints a
# section header where `e2e_note` prints an indented line.
#
# WHICH MEANS `fail` IS NOW IN SCOPE BESIDE `fatal`, and a refusal added later and
# spelled `fail` would exit 1 while silently losing the `::error::` annotation.
# Nothing catches that -- `check-e2e-helpers.sh`'s collision scan looks for a
# script DEFINING a library name, not for one shadowed by import -- and defining
# `fail` here to override it is exactly what that scan refuses. So it is written
# down instead: in this file the refusal is `fatal`.
# shellcheck source=lib/e2e-common.sh
. "${REPO_ROOT}/scripts/lib/e2e-common.sh"

# The scope. One row per binary: the executable, and the Catch2 tag expression it
# is run with (empty means the whole binary). Adding a concurrency-bearing target
# is adding a row.
#
# `scripts/check-tsan-scope.cmake` PARSES this table -- keep the
# `"name|tagExpression"` shape, or that check fails by name rather than silently
# enforcing an empty scope.
TARGETS=(
    "FastCacheTest|[async],[consensus],[distributed],[reactor],[task]"
    "fastcache-compile-node-tests|"
)

# What this gate DID, one entry per row of TARGETS above.
#
# The gate is fail-fast and stays that way: a sanitized run is expensive, and
# forcing a second full run after a known failure is how a gate becomes something
# people stop running (#501 reached the same conclusion for `local-gate.sh`).
# What was missing is not perseverance, it is the ACCOUNT.
#
# Every failure path in `RunTarget` ends in `fatal`, which exits -- so when the
# first target reports a race the second never runs, and the only thing printed
# names the first. **Skipped, absent, unstarted and failed are four states**, and
# this gate rendered the first three as one: silence.
#
# The states, and why the fourth is not padding:
#
#   not-run    the gate stopped before this target; it has reported NOTHING here
#   running    this target STARTED and no verdict was reached -- a killed or
#              timed-out run. Distinct from not-run because "did not run" would
#              be a LIE about a target that ran for ten minutes, and the two are
#              diagnosed in different places (#584 is this state one level up)
#   clean      ran, and this gate is satisfied
#   FAILED     ran, and this gate is not
#
# Parallel INDEXED arrays rather than `declare -A`: macOS ships bash 3.2, which
# has no associative arrays, and this script runs there -- `tsan-gate-selftest`
# is in the default ctest set under `if(NOT WIN32)`.
# Declared EMPTY and never pre-filled: `TargetPairs` reads an absent entry as
# `not-run`, which is the truth for every target until the loop reaches it, and
# is what a failure BEFORE the loop has to report. Filling it in advance would
# only create a second list to keep in step with the first.
TargetVerdicts=()

# Whether the EXIT trap renders the account. `yes` for a real gate run -- which
# is every invocation that is not `--self-test`, including a failing one, since a
# failing run is exactly when the account is worth having.
RenderVerdictsOnExit="yes"

# Record a verdict against a target BY NAME, so a caller never has to know its
# index. An unknown name is a programming error and says so rather than silently
# recording nothing -- a verdict table that quietly drops entries is the defect
# this whole mechanism exists to remove.
SetTargetVerdict() {
    local name="$1" verdict="$2" i=0 _row
    for _row in "${TARGETS[@]}"; do
        if [[ "${_row%%|*}" == "${name}" ]]; then
            TargetVerdicts[$i]="${verdict}"
            return 0
        fi
        i=$((i + 1))
    done
    echo "tsan-gate: internal error: no TARGETS row named '${name}'" >&2
    return 1
}

# Every target's state, as `name=state` pairs in TABLE order.
#
# Split from the renderer so the renderer stays PURE -- the same two-function
# shape `local-gate.sh` uses (`leg_pairs` / `leg_summary`), arrived at for the
# same reason and deliberately not shared: the common core is a `printf` and a
# `case`, while the header, the column width and all four labels are per-caller
# data, and bash 3.2 has no associative array to pass such a table through.
#
# Reading the table here is what makes a silently absent row impossible. The
# green path used to print `all scoped targets clean under ThreadSanitizer` -- a
# sentence about a table it did not read, which stays true-looking when a third
# row is added and never runs.
#
# `:-not-run` is what makes the ABSENCE of an entry mean something rather than
# being an unbound-variable death under `set -u`, which would print a partial
# table with no summary -- indistinguishable from a table that judged everything
# and found nothing to say, this ticket's defect one level down. Unquoted at the
# call site on purpose: these are CMake target names, which carry no whitespace.
TargetPairs() {
    local i=0 row
    for row in "${TARGETS[@]}"; do
        echo "${row%%|*}=${TargetVerdicts[$i]:-not-run}"
        i=$((i + 1))
    done
}

# The account. PURE -- everything it reports arrives as arguments, so the
# self-test drives every verdict in milliseconds without staging a build, and
# without the array save/restore a global-reading renderer forces on its fixture.
#
# Four arms and no fall-through, because `not-run` and *a state this renderer
# does not know* are different facts: the first is "the gate stopped before this
# target", the second is a bug in the gate. Collapsing them is the acceptance
# clause itself -- a fourth state reaching a default arm as a plausible verdict
# recreates this bug one level down.
RenderTargetVerdicts() {
    local pair name verdict label
    echo "==> ThreadSanitizer gate, per-target verdict:"
    # `${1+"$@"}` rather than `"$@"`: before bash 4.4 an empty `$@` is an unbound
    # expansion under `set -u`, and this script runs on macOS's 3.2.
    for pair in ${1+"$@"}; do
        name="${pair%%=*}"
        verdict="${pair#*=}"
        case "${verdict}" in
            clean)   label="clean" ;;
            FAILED)  label="FAILED" ;;
            running) label="STARTED, NO VERDICT -- the run ended while this target was in flight" ;;
            not-run) label="NOT RUN -- the gate stopped before this target, so it has reported nothing about it" ;;
            *)       label="UNKNOWN STATE '${verdict}' -- this is a bug in the gate, not a verdict" ;;
        esac
        printf '      %-32s %s\n' "${name}" "${label}"
    done
}

# `exitcode=66` is TSan's default and is named rather than assumed: this gate
# decides on the exit code, so it must not be something a stray TSAN_OPTIONS in
# the environment can change to 0. `print_suppressions=1` is on for EVERY run,
# canary included, so a suppression that has started matching more than it was
# written for is at least visible rather than silent -- `.tsan-suppressions` says
# that happens on every run, and it only does if it is set in one place.
#
# EXPORTED rather than written as a `TSAN_OPTIONS=... cmd` prefix on each run.
# The runs now go through `run_bounded`, which is a shell FUNCTION, and an
# assignment prefixing a function call is a corner of the language whose scope
# and export rules differ between POSIX mode and bash's default -- on a gate
# whose whole subject is not relying on unexercised behaviour, an `export` that
# is unambiguous on every shell is worth more than a shorter line. It reaches
# `nm` and `find` too, which read none of it.
export TSAN_OPTIONS="halt_on_error=0 exitcode=66 print_suppressions=1 suppressions=${SUPPRESSIONS}"

# Every wait is bounded, which is this repository's oldest testing rule. Running
# the binaries directly rather than through `ctest` loses ctest's own per-test
# TIMEOUT, and TSan changes timing enough that a deadlock is a real outcome: the
# consensus and node tests do real socket and thread work. Unbounded, a hung case
# runs to GitHub's six-hour job limit with nothing saying which target stalled.
# Generous, because the honest measurements are 1.2s and 17.6s -- this is a
# deadlock detector, not a performance budget.
#
# BOUNDED BY `run_bounded`, NOT BY `timeout(1)` (#488). What stood here resolved
# `timeout` then `gtimeout`, justified by two claims that were both false: that
# macOS needs the fallback, and that "this preset runs there". The `clang-tsan`
# job is `runs-on: ubuntu-24.04`, so the fallback had never executed anywhere --
# untested by construction rather than by oversight -- and what GitHub's
# `macos-14` image actually ships is measured and recorded in `run_bounded`'s own
# header, which is neither binary. That measurement is deliberately NOT restated
# here: a figure with two homes loses its conditions at one of them, and this
# paragraph being cited as established knowledge is precisely how it sent #457's
# bounded probe the wrong way.
#
# So the resolver is deleted rather than documented. `run_bounded` needs no
# binary, so there is no platform to refuse and nothing to resolve -- and this is
# not one unexercised path swapped for another. Its expiry, its unstartable case
# and its ceiling being wall-clock are driven by `ctest -R e2e-helpers-selftest`,
# which is in the DEFAULT set under `if(NOT WIN32)` and therefore runs on the
# macOS job. That is strictly more than the resolver it replaces ever had.
#
# It also separates two facts one integer cannot carry, and not merely in
# principle. Catch2 exits `min(255, failed_assertions)`, so a suite that RAN to
# completion with 124 failing assertions exits 124 -- the same number `timeout`
# reports for a kill, which `RunTarget` below read as a deadlock that had not
# happened. The verdict comes from `e2e_bound_outcome`; the status is only the
# convenience.
#
# What `run_bounded` does NOT do is signal the process GROUP, and this is the one
# place the change is a REGRESSION rather than an improvement. GNU `timeout`'s
# manual documents `--foreground` as the way to stop children being timed out, so
# without it they are; `run_bounded` signals the direct child only, and says so
# (`ONE PROCESS DEEP` in its header). `fastcache-compile-node-tests` spawns
# compilers and claims scratch roots, so on expiry a grandchild can outlive the
# target this gate killed.
#
# The obvious fear is that a survivor POISONS THE NEXT RUN by holding something,
# and that was checked rather than assumed, because it is the difference between
# a note and a defect. It does not, and not by luck: the two things a grandchild
# could hold are both already closed where they are taken.
#
#   * a scratch-root claim -- `ScratchClaim.cpp` opens its lock file `O_CLOEXEC`,
#     and its comment says why in these words: the `flock` lives on the open file
#     DESCRIPTION, so an inherited copy would keep the root claimed after the node
#     is gone and the next one would refuse, blaming a node that does not exist;
#   * this gate's own output -- `run_bounded` captures into a FILE rather than a
#     pipe, and unlinks it, so a surviving writer can neither block the read that
#     follows nor corrupt a later one.
#
# What is left is a compiler burning CPU until it finishes on its own, after a
# run that has already stopped with a named refusal somebody is reading. Real,
# bounded, and not worth changing how a SHARED helper signals to remove --
# `run_bounded`'s header rejects both routes on stated grounds: job control
# changes the calling fixture's own signal handling, and a watchdog firing after
# `wait` reaps can signal a REUSED pid.
TargetTimeoutSeconds="${FASTCACHE_TSAN_TIMEOUT:-900}"

# The canary's own bound, which is much tighter because the canary is a few
# milliseconds of deliberate race and nothing else -- there is no suite behind it
# that could legitimately grow. Named rather than written into the call, so the
# refusal can quote the bound it actually enforced and so `--self-test` can drive
# the expiry without waiting a minute for it.
CanaryTimeoutSeconds=60

# Annotate only where a workflow will render it. Every other script in this repo
# prints a plain prefixed failure (`cluster-e2e.sh`, `compile-cache-e2e.sh`,
# `local-gate.sh`, `tidy-sweep.sh`), and a developer running this by hand should
# not get `::error::` noise for the privilege.
fatal() {
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
        echo "::error::tsan-gate: $*" >&2
    else
        echo "TSAN GATE FAILED: $*" >&2
    fi
    exit 1
}

note() { echo "==> $*"; }

# One scratch directory with one EXIT trap, which is this repository's idiom
# (cluster-e2e.sh, compile-cache-e2e.sh, tidy-sweep.sh all do it). Per-log `rm`
# calls scattered down the function were five places for the sixth exit path to
# forget -- and `fatal` exits rather than returning, so a RETURN-scoped trap would
# not have covered them either.
SCRATCH="$(mktemp -d)"
# The verdict is rendered from the EXIT trap, which is the only place that covers
# EVERY way out: `fatal` (which exits), the green path, and a signal -- `e2e_begin`
# installs a TERM trap that ends the run through an ordinary exit, so this runs
# then too. Printing it from the two terminal call sites instead would have left
# the killed case silent, which is the case that most needs an account.
cleanup() {
    # `--self-test` runs no target and intends to run none, so a verdict table
    # there would report NOT RUN -- "the gate stopped before this target" -- about
    # a run that was never a gate. That is this ticket's own defect inverted: an
    # account of work nobody asked for reads exactly like an account of work that
    # was skipped, and both are wrong in a way a reader cannot check.
    if [[ "${RenderVerdictsOnExit}" == "yes" ]]; then
        RenderTargetVerdicts $(TargetPairs)
    fi
    rm -rf "${SCRATCH}"
}
trap cleanup EXIT

# After the EXIT trap, which is `e2e_begin`'s stated contract: it installs a TERM
# trap so a signal ends the run through an ordinary exit and the cleanup above
# still runs. All this gate wants from it is `_e2e_workdir`, which is where
# `run_bounded` puts its capture file and the outcome it records -- so the
# scratch directory this script already removes takes those with it too.
e2e_begin "tsan gate" "$SCRATCH"

# ---------------------------------------------------------------------------
# Question one: is the artefact under test instrumented?
#
# Spelled with an explicit existence check FIRST, and that is not defensive
# padding. `nm` on a path that does not exist prints nothing and a grep for
# `__tsan_init` counts zero -- which is character-for-character what an
# uninstrumented binary looks like. A missing file must not be reportable as a
# missing sanitizer; they need different fixes.
#
# `target/` is where this project's CMake puts executables
# (CMAKE_RUNTIME_OUTPUT_DIRECTORY, set in the top-level CMakeLists). No `find`
# fallback, on purpose. Searching the build tree and taking the first match is a
# GUESS, and a guess that silently picks some other build's binary is the failure
# this whole script exists to refuse -- it would happily verify and run an
# artefact nobody asked about. A layout change should stop the gate with a path in
# the message, which is a one-line fix, rather than quietly test the wrong file.
# ---------------------------------------------------------------------------

BinaryPath() { printf '%s' "${BUILD_DIR}/target/$1"; }

# Where CMake put a target's object files.
#
# Found rather than tabulated, because the three targets do not share a parent:
# `tsan-canary` and `FastCacheTest` sit under `src/tests/CMakeFiles/`, while
# `fastcache-compile-node-tests` is under `src/apps/fastcache-compile-node/`. A
# table would be a second place to edit when a target moves, and the kind that
# goes stale silently.
#
# `find` is used here where `BinaryPath` deliberately refuses it, and the
# difference is the check: taking the FIRST match would be the guess that comment
# objects to, so this requires EXACTLY ONE and refuses otherwise. Zero means the
# layout moved or the target was never built; two means a stale `.dir` from a
# renamed target is still on disk and the gate cannot know which is current.
# Either way it stops with the paths in the message rather than verifying an
# artefact nobody asked about.
ObjectDir() {
    local name="$1" matches count
    matches="$(find "$BUILD_DIR" -type d -name "${name}.dir" 2>/dev/null || true)"
    if [[ -z "$matches" ]]; then
        fatal "${name}: no ${name}.dir under ${BUILD_DIR}, so its objects cannot be checked for instrumentation.
    Either the target was not built, or CMake's object layout moved. This gate
    reads the OBJECTS rather than the binary (see below), so it needs them."
    fi
    count="$(printf '%s\n' "$matches" | grep -c .)"
    if [[ "$count" != "1" ]]; then
        fatal "${name}: ${count} directories named ${name}.dir under ${BUILD_DIR}; cannot tell which is current:
${matches}
    A stale directory from a renamed target will do this. Remove it, or reconfigure."
    fi
    printf '%s' "$matches"
}

# ---------------------------------------------------------------------------
# Question one: is the artefact under test instrumented?
#
# ASKED OF THE OBJECTS, NOT THE BINARY, and that distinction is the whole of
# #472. `__tsan_init` is DEFINED by the sanitizer runtime, which the link pulls in
# whenever `-fsanitize=thread` is on the link line -- so `nm --defined-only` on a
# binary answers yes whether or not a single translation unit was instrumented.
# Measured, same source, two objects, two binaries:
#
#     inst.o   U __tsan_init  yes      inst  binary: __tsan_init defined = YES
#     plain.o  U __tsan_init  no       plain binary: __tsan_init defined = YES
#
# `plain.o` was compiled with no sanitizer flag at all and its binary passed the
# old proof. An object cannot borrow a symbol from a runtime it is not linked to,
# so the UNDEFINED reference is the one that means something. Same symbol as
# before; what changed is `--undefined-only` and looking at the object.
#
# WHY THAT MATTERED MORE FOR THE SUITES THAN FOR THE CANARY. An uninstrumented
# canary fails closed -- it exits 0, reports no race, and `AssertCanaryFires`
# refuses -- though it used to do so while blaming `TSAN_OPTIONS`, the
# suppressions file or the runtime, all three of which were fine. The test
# binaries had no such backstop: `RunTarget` checks the timeout, that the tag
# filter matched, and that assertions were reported, and not one of those can
# tell an instrumented run from an uninstrumented one. An uninstrumented
# `FastCacheTest` linked against the runtime runs, reports assertions, exits 0,
# and the gate prints `all scoped targets clean`. That is the silent fail-open,
# and it is why this check covers every artefact rather than only the canary.
#
# WHY `__tsan_init` AND NOT `__tsan_func_entry`. The latter is emitted per
# instrumented FUNCTION, so an object whose translation unit has no functions
# carries none -- six of `FastCacheTest`'s 135 are exactly that, the
# platform-gated `Iocp*`, `Kqueue*` and `Tls*` test files, which compile to
# nothing on Linux. A rule built on `__tsan_func_entry` needs an
# empty-TU exemption and would otherwise refuse a correct build. Those six DO
# carry `U __tsan_init` and a `tsan.module_ctor`: they were instrumented, there
# was simply nothing to instrument. Measured across the three targets, 185 of 185
# objects carry it and none needs an exemption.
#
# The existence check on the binary comes FIRST and stays. `nm` on a path that
# does not exist prints nothing, and "no symbols" is character-for-character what
# an uninstrumented artefact looks like; a missing file must not be reportable as
# a missing sanitizer, because they need different fixes.
#
# `target/` is where this project's CMake puts executables
# (CMAKE_RUNTIME_OUTPUT_DIRECTORY, set in the top-level CMakeLists). No `find`
# fallback for the binary, on purpose -- see `ObjectDir` above for where a search
# is used and what makes it safe there.
# ---------------------------------------------------------------------------

AssertInstrumented() {
    local name="$1" path objdir
    path="$(BinaryPath "$name")"
    [[ -f "$path" ]] || fatal "${name}: not built (looked in ${BUILD_DIR}/target). Build it before running this gate."
    objdir="$(ObjectDir "$name")"

    local total=0 uninstrumented=0 offenders="" obj undefined nm_rc
    while IFS= read -r obj; do
        total=$(( total + 1 ))
        # `nm` into a variable and match with `case`, never `nm | grep -q`. Under
        # `set -o pipefail` a `grep -q` that matches EARLY closes the pipe, `nm`
        # dies of SIGPIPE, and the pipeline reports failure -- so an instrumented
        # object is diagnosed as an uninstrumented one, ON THE SUCCESS PATH.
        #
        # Measured while writing this, in the census scripts that produced the
        # numbers above: it is not merely a false negative, it is RACY. `printf`
        # sometimes wins the exit race and sometimes does not, so two runs over
        # the same 135 objects returned 129 and 108, and a listing built the same
        # way named 27 offenders against a true 6. A deterministic wrong answer
        # gets caught by whoever checks it once; a racy one gets blamed on the
        # subject.
        nm_rc=0
        undefined="$(nm --undefined-only --no-demangle "$obj" 2>/dev/null)" || nm_rc=$?
        if [[ "$nm_rc" -ne 0 ]]; then
            fatal "${name}: nm could not read ${obj}; cannot verify instrumentation.
    An unreadable object is not an uninstrumented one, so this refuses rather
    than guessing which it was."
        fi
        case "$undefined" in
            *__tsan_init*) ;;
            *)
                uninstrumented=$(( uninstrumented + 1 ))
                offenders="${offenders}        ${obj}
"
                ;;
        esac
    done < <(find "$objdir" -name '*.o')

    # An empty object directory is not a clean bill: it is a target whose objects
    # this gate never saw, which is the same shape as the missing-file case above.
    [[ "$total" -gt 0 ]] \
        || fatal "${name}: no object files under ${objdir}, so nothing proves this artefact was instrumented."

    if [[ "$uninstrumented" -ne 0 ]]; then
        fatal "${name}: ${uninstrumented} of ${total} object files were built WITHOUT ThreadSanitizer
    instrumentation (no undefined __tsan_init reference):
${offenders}    The BINARY may still carry a defined __tsan_init -- the link pulls the runtime
    in whole -- so the binary is not evidence here and this gate no longer asks it.
    A per-file flag override, a stale object in an incremental directory, or a
    compiler-cache replay of a non-instrumented object all land here.
    Check the compile line the generator actually wrote:
        grep -c -- -fsanitize=thread ${BUILD_DIR}/build.ninja
    See cmake/portable/Sanitizers.cmake for the precedent."
    fi
    note "${name}: instrumented (${total} object files)"
}

# ---------------------------------------------------------------------------
# Question two: does the runtime detect and report?
#
# Run WITH the suppressions file, deliberately. That proves in the same breath
# that `.tsan-suppressions` is not broad enough to swallow an obvious race -- a
# wildcard pattern that silences the canary fails here rather than quietly
# disarming the whole gate. It is not a proof about narrow entries: the canary
# races on its own global, in a binary that links no FastCache frame, so a
# `race_top:` naming a real function here could never have silenced it.
# ---------------------------------------------------------------------------

AssertCanaryFires() {
    local path canary_out outcome canary_rc=0
    path="$(BinaryPath tsan-canary)"

    # `run_bounded` merges stderr into what it echoes, so there is no `2>&1` to
    # write here. Its outcome is per RUN and must be read before anything else
    # bounds anything, which is why it is taken on the next line rather than
    # where it is used.
    canary_out="$(run_bounded "$CanaryTimeoutSeconds" "$path")" || canary_rc=$?
    outcome="$(e2e_bound_outcome)"

    # Both bound outcomes are refused BEFORE the race is looked for, and that
    # order is the point rather than tidiness. A canary that reports its race and
    # then hangs would satisfy the `grep` below and license the entire suite --
    # but a bound expiring is not evidence that the runtime reports, it is
    # evidence that nothing here can be concluded, and this gate exists to refuse
    # exactly that substitution.
    # One `case` over the three-state outcome rather than a chain of `if`s, so a
    # fourth state added to `run_bounded` has an obvious place to land here --
    # and matched against the library's NAMES, never against string literals: a
    # mistyped literal falls through to the `finished` path, which is this gate
    # concluding "fine" from "I could not tell".
    case "$outcome" in
        "$E2eBoundOutcomeExceeded")
            fatal "tsan-canary did not finish within ${CanaryTimeoutSeconds}s and was killed, so whether the
    sanitizer reports is UNKNOWN -- which this gate refuses rather than reads as
    either answer. The canary races on its own global and exits; a hang is the
    runtime or the machine, not the race. Output so far:
${canary_out}"
            ;;
        "$E2eBoundUnstartable")
            fatal "tsan-canary at ${path} could not be executed, so
    nothing proves the sanitizer reports. It exists -- AssertInstrumented
    checked that before this ran -- so this is a permission bit, an interpreter,
    or a binary for another architecture, and not a missing build."
            ;;
    esac

    if [[ "$canary_rc" -eq 0 ]]; then
        fatal "the deliberate race in src/tests/TsanCanary.cpp was NOT reported.
    Every object of this canary carries an undefined __tsan_init, which was checked
    before this ran, so the translation unit WAS instrumented -- that is not the
    cause and is not worth investigating. What is left is the runtime side:
    TSAN_OPTIONS, the suppressions file, or the sanitizer runtime itself.
    (Before #472 this message named those three while an uninstrumented canary
    could still reach it, which sent readers to three places that were all fine.)
    A clean suite would be meaningless, so this gate fails instead of passing.
    Canary output:
${canary_out}"
    fi
    grep -q 'data race' <<<"$canary_out" \
        || fatal "the canary exited ${canary_rc} but reported no data race; something else killed it:
${canary_out}"
    note "tsan-canary: race reported as expected (exit ${canary_rc}) -- the sanitizer is live"
}

# ---------------------------------------------------------------------------
# The suite.
# ---------------------------------------------------------------------------

# A refusal that BELONGS to a target: record the verdict, then exit through
# `fatal` as every other refusal here does.
#
# Without this every failing target rendered `STARTED, NO VERDICT`, because
# `fatal` exits and the EXIT trap found the entry still at `running`. That is a
# true statement and the wrong one -- the acceptance asks for `clean`, `FAILED`
# or `NOT RUN`, and a target that RAN and was REFUSED is the second. `running`
# then means what it should: started, and killed before any verdict was reached.
#
# @param 1 target name. @param 2.. the refusal message, passed to `fatal`.
FailTarget() {
    local name="$1"; shift
    SetTargetVerdict "$name" "FAILED" || true
    fatal "$@"
}

RunTarget() {
    local name="$1" tags="$2" path rc=0 log summary outcome
    # Claimed BEFORE anything can fail, so a target killed mid-run renders as
    # STARTED rather than NOT RUN -- it DID run, and saying it did not would be a
    # lie about the ten minutes it spent. Every refusal below goes through
    # `FailTarget` and overwrites this with `FAILED`; what is left at `running`
    # is exactly the case nothing got to decide.
    SetTargetVerdict "$name" "running"
    path="$(BinaryPath "$name")"

    # An empty tag expression means "run the whole binary", which is not the same
    # argument as an empty string -- Catch2 would read `""` as a filter matching
    # nothing. An array is how bash spells "this argument is sometimes absent";
    # `${args[@]+...}` rather than a bare `"${args[@]}"`, because an empty array
    # under `set -u` is an unbound variable in bash 3.2 -- which is what macOS
    # ships, and this script runs there: `tsan-gate-selftest` is in the default
    # `ctest` set under `if(NOT WIN32)`, so the macOS job executes everything
    # `--self-test` reaches. (The `clang-tsan` JOB is Linux-only; that is a
    # different claim, and conflating the two is #488.)
    local args=()
    if [[ -n "$tags" ]]; then
        args=("$tags")
    fi

    log="${SCRATCH}/${name}.log"
    note "running ${name} ${tags:+(${tags})}"
    run_bounded "${TargetTimeoutSeconds}" "$path" ${args[@]+"${args[@]}"} >"$log" 2>&1 || rc=$?
    outcome="$(e2e_bound_outcome)"

    # Named before anything else reads the log: a killed process leaves a partial
    # Catch2 summary, which the checks below would diagnose as "reported no
    # assertions" and send the reader to the tag expression.
    #
    # ASKED OF THE OUTCOME, NEVER OF `rc`. Catch2 exits
    # `min(255, failed_assertions)`, so a suite that ran to completion with 124
    # failing assertions produces the very number a killed one does -- and until
    # #488 this branch read that as a deadlock, printed a bound the run never
    # reached, and sent whoever met it to look for a hang in a suite that had
    # simply failed. `run_bounded` records what it OBSERVED instead of leaving it
    # to be inferred from an integer that means two things.
    case "$outcome" in
        "$E2eBoundOutcomeExceeded")
            cat "$log"
            FailTarget "$name" "${name} did not finish within ${TargetTimeoutSeconds}s and was killed.
    Under ThreadSanitizer that is a deadlock until proven otherwise, and the last
    case in the log above is where to start. FASTCACHE_TSAN_TIMEOUT raises the
    bound if the suite has simply grown."
            ;;
        "$E2eBoundUnstartable")
            FailTarget "$name" "${name} at ${path} could not be executed, so this target ran no cases at all.
    AssertInstrumented found the file and read its objects, so this is a
    permission bit, an interpreter, or a binary for another architecture."
            ;;
    esac

    # A filter that matches nothing is the quietest way for this gate to test
    # nothing and say "clean" -- a typo in the TARGETS table above would do it.
    # Catch2 exits 2 and says so, which would land in the failure branch below,
    # but it would be diagnosed there as a mysterious non-race failure. Name it
    # instead, and assert positively that cases ran rather than trusting the exit
    # code.
    if grep -q 'No test cases matched' "$log"; then
        FailTarget "$name" "${name}: the filter '${tags}' matched no test cases, so this target tested NOTHING.
    Fix the tag expression in the TARGETS table in $(basename "${BASH_SOURCE[0]}")."
    fi

    # On success, the interesting lines only: a passing sanitizer run is hundreds
    # of lines of Catch2 output that nobody reads, and burying the suppression
    # count in it is how a suppression that has started matching more than it was
    # written for goes unnoticed. On failure, everything -- that is when the whole
    # log is the evidence.
    if [[ "$rc" -eq 0 ]]; then
        summary="$(grep -E 'assertions in|Matched [0-9]+ suppressions' "$log" || true)"
        [[ -n "$summary" ]] \
            || FailTarget "$name" "${name}: exited 0 but reported no assertions; refusing to call that clean."
        echo "$summary"
        note "${name}: clean"
        # Recorded where `clean` is DECLARED, not by the caller. RunTarget has
        # exactly one success return and every other path is `fatal`; recording
        # it in the loop instead would mean a `return` added here later for some
        # OTHER reason silently reads as clean, which is this ticket's own defect
        # rebuilt out of the fix. Left at `running`, such a return renders
        # STARTED, NO VERDICT -- wrong, but wrong in the direction that says so.
        SetTargetVerdict "$name" "clean"
        return
    fi

    cat "$log"
    # Distinguish a race from an ordinary test failure, because they are read by
    # different people and fixed in different places. Catch2 prints "All tests
    # passed" even when the process exits 66, so the exit code is what decides and
    # the output is only ever an explanation.
    if grep -q 'WARNING: ThreadSanitizer' "$log"; then
        FailTarget "$name" "${name} reported an unsuppressed data race (exit ${rc}).
    If this is a NEW race, it is its own issue -- file it. If it is a known one,
    it belongs in .tsan-suppressions with its issue number, not deleted from here."
    fi
    FailTarget "$name" "${name} failed (exit ${rc}) without a ThreadSanitizer report."
}

# ---------------------------------------------------------------------------
# `--self-test`: drive this gate's refusals against staged artefacts.
#
# A guard that has never been seen to fire is the thing this repository keeps
# finding, and #472 is precisely that -- a proof that had never been shown to
# refuse anything, because it structurally could not. So each refusal is staged
# here and required to happen.
#
# TWO DECISION PATHS, TWO KINDS OF STAGING, and the section headers below say
# which is which. `AssertInstrumented`'s verdicts are driven against staged
# OBJECT TREES read through a stub `nm`; the bound's three outcomes (#488) are
# driven against staged EXECUTABLES, which need no stub, no objects and no
# symbol table at all. Neither needs a compiler or a sanitizer, which is what
# keeps the whole fixture in the default `ctest` set on every platform.
#
# THE ONE THAT MATTERS IS THE SUITE, NOT THE CANARY. An uninstrumented canary
# already failed closed before #472: it exits 0, reports no race, and
# `AssertCanaryFires` refuses. The test binaries had no backstop at all --
# `RunTarget` checks the timeout, the tag filter and the assertion count, none of
# which can tell an instrumented run from an uninstrumented one -- so a guard
# demonstrated only on the canary would be a guard nobody has seen fire where it
# matters. `suite-object-uninstrumented` is that case and is deliberately first.
#
# HOW THE OBJECTS ARE STAGED. `AssertInstrumented` asks exactly one question of
# each object: what does `nm --undefined-only` print. So a staged `.o` here is a
# TEXT FILE whose content is that output, and a stub `nm` earlier on PATH prints
# it. No compiler, no sanitizer runtime, no build -- which is what lets this run
# in the default `ctest` set on every platform rather than only where a TSan
# toolchain exists. It exercises the real decision code; only the reading of the
# symbol table is stood in for, and that half has no branches.
#
# The binary is a plain file, because `AssertInstrumented` only checks that it
# EXISTS. That is the point of #472 restated: the binary's own symbols answer
# nothing, so the gate no longer reads them, so a staged binary needs no symbols.
# ---------------------------------------------------------------------------

SelfTest() {
    local scratch stub failures=0 ran=0
    scratch="$(mktemp -d)"

    # The stub. `nm` is called as `nm --undefined-only --no-demangle <path>`; the
    # flags do not change what a staged object should answer, so it takes the last
    # argument and prints that file. A file named `*unreadable*` exits non-zero,
    # which is how "nm could not read this" is staged -- an unreadable object must
    # not be reportable as an uninstrumented one.
    stub="${scratch}/stub"
    mkdir -p "$stub"
    cat > "${stub}/nm" <<'STUB'
#!/bin/bash
for arg in "$@"; do :; done
case "$arg" in
    *unreadable*) exit 1 ;;
esac
cat "$arg"
STUB
    chmod +x "${stub}/nm"

    # The verdict, shared by both drivers below. Split out rather than written
    # twice: the `!`-prefix matching is the only fiddly part of this fixture, and
    # a second copy of it would be a second thing to get wrong in the direction
    # that reports a pass.
    #
    # @param 1 case name
    # @param 2 expected exit status
    # @param 3 the status observed
    # @param 4 the output observed
    # @param 5.. text the output must contain; a leading `!` means must NOT
    Expect() {
        local name="$1" want="$2" rc="$3" out="$4"; shift 4
        local pattern
        ran=$(( ran + 1 ))
        if [[ "$rc" != "$want" ]]; then
            echo "  FAIL ${name}: exited ${rc}, expected ${want}" >&2
            printf '%s\n' "$out" | sed 's/^/       | /' >&2
            failures=$(( failures + 1 ))
            return
        fi
        # `${1+"$@"}`: before bash 4.4 -- macOS's /bin/bash, and this runs in the
        # DEFAULT ctest set -- an empty `$@` is an unbound expansion under
        # `set -u`. A case that asserts only an exit status passes no patterns,
        # and would die with a shell error instead. Inside a want-fail assertion
        # that is indistinguishable from the rule firing, which is #723's
        # indistinguishability reached through `set -u` rather than a mode bit.
        for pattern in ${1+"$@"}; do
            if [[ "${pattern:0:1}" == "!" ]]; then
                if [[ "$out" == *"${pattern:1}"* ]]; then
                    echo "  FAIL ${name}: output contains '${pattern:1}' and must not" >&2
                    printf '%s\n' "$out" | sed 's/^/       | /' >&2
                    failures=$(( failures + 1 ))
                    return
                fi
            elif [[ "$out" != *"$pattern"* ]]; then
                echo "  FAIL ${name}: output lacks '${pattern}'" >&2
                printf '%s\n' "$out" | sed 's/^/       | /' >&2
                failures=$(( failures + 1 ))
                return
            fi
        done
        echo "  ok   ${name}"
    }

    # @param 1 case name
    # @param 2 expected exit status
    # @param 3.. text the output must contain; a leading `!` means must NOT
    Case() {
        local name="$1" want="$2"; shift 2
        local out rc=0
        out="$(PATH="${stub}:$PATH" AssertInstrumented "$SelfTestTarget" 2>&1)" || rc=$?
        Expect "$name" "$want" "$rc" "$out" "$@"
    }

    # Build a staged build directory. @param 1 target, @param 2.. object specs of
    # the form `name:instrumented|plain|empty|unreadable`.
    Stage() {
        local target="$1"; shift
        local dir spec obj kind
        BUILD_DIR="${scratch}/build-$RANDOM$RANDOM"
        SelfTestTarget="$target"
        mkdir -p "${BUILD_DIR}/target"
        # The staged binary DEFINES __tsan_init, exactly as a real one does --
        # the link pulls the runtime in whole. Every refusal below therefore
        # happens with the old proof's evidence sitting right there and saying
        # "instrumented". That is #472 reproduced in the fixture rather than
        # merely described in its comment.
        printf '%s
' "0000000000001234 T __tsan_init" > "$(BinaryPath "$target")"
        dir="${BUILD_DIR}/src/tests/CMakeFiles/${target}.dir"
        mkdir -p "$dir"
        for spec in ${1+"$@"}; do
            obj="${spec%%:*}"; kind="${spec#*:}"
            case "$kind" in
                # A real instrumented object: an undefined __tsan_init, plus the
                # per-function hooks a TU with code also gets.
                instrumented) printf '%s\n' "                 U __tsan_init" \
                                              "                 U __tsan_func_entry" \
                                              "                 U __tsan_read4" > "${dir}/${obj}.o" ;;
                # An instrumented TU with NO functions -- the six platform-gated
                # files in FastCacheTest. It carries __tsan_init and nothing else,
                # which is why the rule is built on that symbol and not on
                # __tsan_func_entry: a rule using the hooks would refuse this.
                empty)        printf '%s\n' "                 U __tsan_init" > "${dir}/${obj}.o" ;;
                # Compiled with no sanitizer flag at all. Its BINARY would still
                # define __tsan_init, which is the whole of #472.
                plain)        printf '%s\n' "                 U _ZSt4cout" > "${dir}/${obj}.o" ;;
                unreadable)   printf '%s\n' "unreadable" > "${dir}/${obj}-unreadable.o" ;;
            esac
        done
    }


    # -----------------------------------------------------------------------
    # RunTarget's reading of the bound (#488).
    #
    # THE ONE THAT MATTERS IS 124, and it is deliberately first. `RunTarget` used
    # to decide from the exit STATUS, and 124 is two facts there: `timeout`'s "I
    # killed it", and Catch2's own `min(255, failed_assertions)` for a suite that
    # ran to completion and failed 124 assertions. The second was reported as the
    # first -- a deadlock diagnosis, quoting a bound the run never reached, for a
    # suite that had merely failed. Nothing could have caught it, because the
    # gate's only evidence was a number that means both things.
    #
    # The other two are the direction the ticket's reviewer asked about: a bound
    # that expires, and a target that cannot start, must each still be a REFUSAL
    # naming its own reason. A bound is not an improvement if it lets the gate
    # conclude "fine" from "I could not tell".
    #
    # A staged target here is a SHELL SCRIPT at the path `BinaryPath` computes.
    # Nothing on this path reads a symbol table, so these need no stub `nm`, no
    # objects and no sanitizer -- which is what keeps them in the default `ctest`
    # set beside the cases above, on every platform CI builds.
    #
    # @param 1 case name
    # @param 2 which caller to drive: `target` or `canary`
    # @param 3 the bound, in seconds
    # @param 4 how to stage the artefact: `runnable` or `absent`
    # @param 5 the shell a `runnable` artefact runs
    # @param 6.. text the output must contain; a leading `!` means must NOT
    BoundCase() {
        local name="$1" driver="$2" seconds="$3" staging="$4" body="$5"; shift 5
        local out rc=0 artefact
        # One switch, two columns: which artefact the driver reaches for, and how
        # it is called. `AssertCanaryFires` names `tsan-canary` itself while
        # `RunTarget` is handed a name, so staging follows the driver rather than
        # the case -- and asking that twice is how the two would drift apart.
        local invoke=()
        case "$driver" in
            canary) artefact="tsan-canary";   invoke=(AssertCanaryFires) ;;
            target) artefact="staged-target"; invoke=(RunTarget "staged-target" "") ;;
            # Unreachable while every call site below spells a literal, and kept
            # anyway because a typo would otherwise expand an empty `invoke`
            # under `set -u` and fail somewhere that names bash rather than the
            # case. Routed through `Expect` rather than reporting for itself, so
            # it cannot report a failure the "${ran} cases ran" line has not
            # counted -- which is what an inline `failures=$(( failures + 1 ))`
            # here did.
            *)      Expect "$name" 1 0 "unknown driver '${driver}'" "no such driver"
                    return ;;
        esac

        BUILD_DIR="${scratch}/bound-${name}"
        mkdir -p "${BUILD_DIR}/target"
        # Through `BinaryPath`, which is where this layout is decided. Spelling
        # `${BUILD_DIR}/target/...` here would be a second place that has to move
        # when CMake's output directory does -- and the failure if it did not
        # move is a fixture that stages an artefact the gate never looks at and
        # still reports `ok`.
        if [[ "$staging" == "runnable" ]]; then
            printf '%s\n' '#!/bin/sh' "$body" > "$(BinaryPath "$artefact")"
            chmod +x "$(BinaryPath "$artefact")"
        fi
        TargetTimeoutSeconds="$seconds"
        CanaryTimeoutSeconds="$seconds"
        out="$("${invoke[@]}" 2>&1)" || rc=$?
        Expect "$name" 1 "$rc" "$out" "$@"
    }

    echo "== AssertInstrumented, against staged object trees"

    # THE CASE #472 IS ABOUT. A suite binary with one uninstrumented object.
    # Before this change nothing anywhere refused it: the binary carries a defined
    # __tsan_init from the runtime, and RunTarget cannot tell the difference.
    Stage FastCacheTest a:instrumented b:plain c:instrumented
    Case "suite-object-uninstrumented" 1 \
        "1 of 3 object files were built WITHOUT" "b.o" "the binary is not evidence here"

    Stage tsan-canary only:plain
    Case "canary-object-uninstrumented" 1 "1 of 1 object files were built WITHOUT"

    Stage FastCacheTest a:instrumented b:instrumented c:instrumented
    Case "all-instrumented" 0 "instrumented (3 object files)" "!WITHOUT"

    # No false positive on a translation unit with nothing to instrument.
    Stage FastCacheTest a:instrumented b:empty c:empty
    Case "empty-tu-is-not-a-failure" 0 "instrumented (3 object files)" "!WITHOUT"

    Stage FastCacheTest a:instrumented b:unreadable
    Case "object-cannot-be-read" 1 "nm could not read" "!WITHOUT"

    Stage FastCacheTest a:instrumented
    rm -rf "${BUILD_DIR}/src/tests/CMakeFiles/FastCacheTest.dir"
    Case "no-object-directory" 1 "no FastCacheTest.dir under"

    Stage FastCacheTest a:instrumented
    mkdir -p "${BUILD_DIR}/src/apps/CMakeFiles/FastCacheTest.dir"
    Case "two-object-directories" 1 "cannot tell which is current"

    Stage FastCacheTest a:instrumented
    rm -f "${BUILD_DIR}"/src/tests/CMakeFiles/FastCacheTest.dir/*.o
    Case "no-objects-at-all" 1 "no object files under"

    Stage FastCacheTest a:instrumented
    rm -f "${BUILD_DIR}/target/FastCacheTest"
    Case "binary-not-built" 1 "not built"

    echo
    echo "== the bound's outcomes, against staged artefacts"

    BoundCase "bound-124-is-not-a-timeout" target 30 runnable 'exit 124' \
        "failed (exit 124) without a ThreadSanitizer report" \
        "!did not finish within"

    # Real waits, and the only ones in this fixture: a bound proven by a stub that
    # reports "expired" is a bound nobody has watched expire.
    #
    # What each COSTS, since that is the figure whoever adds a third wait will
    # budget against. `run_bounded` reads bash's integer `SECONDS`, so a bound of
    # 1 is the smallest it can express and enforces 0-1000 ms. Its two-second
    # TERM grace is a CEILING reached only by a child that ignores TERM; `/bin/sh`
    # running `sleep` takes the default disposition and dies on the first poll,
    # so the grace costs one 0.2 s tick. Roughly 0.2-1.2 s each, against a 60 s
    # ctest budget.
    #
    # `sleep 5` and not `sleep 30`: `run_bounded` signals the child and not the
    # process group, so this sleeper is orphaned and outlives the fixture. It only
    # has to outlast the bound plus the worst-case grace, and five seconds does
    # that with room while leaving a stray process around for a sixth as long.
    BoundCase "bound-expiry-is-refused-by-name" target 1 runnable 'sleep 5' \
        "did not finish within 1s and was killed"

    # THE CANARY'S EXPIRY, which is a hole this change closes rather than a
    # branch it merely adds. `AssertCanaryFires` decides "the sanitizer is live"
    # from a non-zero status plus the words `data race` in the output -- and a
    # canary that reports its race and THEN hangs satisfies both. It would have
    # licensed the entire suite on the strength of a run nobody could conclude
    # anything from. So the staged canary here does exactly that, and the bound
    # outcome has to be read BEFORE the grep for the refusal to happen at all.
    BoundCase "canary-expiry-outranks-a-reported-race" canary 1 runnable \
        'echo "WARNING: ThreadSanitizer: data race"; sleep 5' \
        "did not finish within 1s and was killed" \
        "!the sanitizer is live"

    # The third bound outcome, which is neither a slow start nor a clean run.
    #
    # Staged as an ABSENT file rather than as a present one without its execute
    # bit, and that is a measurement rather than a preference: `chmod 644` here
    # left the target runnable and the case reported a CLEAN run, because a mode
    # bit does not mean on every filesystem this script is edited from what it
    # means on ext4. Absence is the one spelling of "this name does not resolve to
    # something executable" that is the same everywhere, and it drives the same
    # `run_bounded` branch. In production the file is always there --
    # `AssertInstrumented` has found it and read its objects before `RunTarget`
    # runs -- so the refusal keeps saying so, and this case exercises the branch
    # rather than that sentence.
    BoundCase "bound-unstartable-is-refused-by-name" target 30 absent '' \
        "could not be executed, so this target ran no cases at all"

    BoundCase "canary-unstartable-is-refused-by-name" canary 30 absent '' \
        "nothing proves the sanitizer reports" \
        "!the sanitizer is live"

    rm -rf "$scratch"
    echo
    # ---------------------------------------------------------------------
    # The per-target verdict (#581).
    #
    # `RenderTargetVerdicts` is PURE -- pairs in, lines out -- so staging its
    # input is passing arguments, and every verdict is exercised in milliseconds
    # with no sanitized build, no staged binaries and no timing. That is the same
    # split the scratch-isolation fixture was rewritten to use: keep acquisition
    # out of the decision, and the decision becomes testable. It is also what
    # keeps this fixture from having to save and restore the gate's own arrays,
    # which a global-reading renderer would force on it.
    #
    # @param 1 case name
    # @param 2 space-separated `name=state` pairs
    # @param 3.. text the output must contain; a leading `!` means must NOT
    VerdictCase() {
        local name="$1" pairs="$2"; shift 2
        local out
        out="$(RenderTargetVerdicts $pairs 2>&1)"
        Expect "$name" 0 0 "$out" "$@"
    }

    # The failure #581 is about: the first target reports a race, `fatal` exits,
    # and the second NEVER RUNS. Before this, the only thing printed named the
    # first -- so a green re-run after the fix looked identical to a run in which
    # the second was never examined.
    #
    # `!UNKNOWN` is not padding. Delete the `FAILED)` arm and the state falls to
    # the unrecognised one, which renders `UNKNOWN STATE 'FAILED'` -- a string
    # that still CONTAINS "FAILED", so an assertion on the word alone stays green
    # while the verdict has become a bug report. Measured by deleting the arm: 22
    # cases ran, 0 failed. A signal that cannot be false in the failing case is
    # not evidence.
    VerdictCase "verdict-stopped-before-the-second-target" \
        "FastCacheTest=FAILED fastcache-compile-node-tests=not-run" \
        "FastCacheTest" "FAILED" "fastcache-compile-node-tests" "NOT RUN" \
        "reported nothing about it" \
        "!clean" "!UNKNOWN"

    # ... and the reverse reading, which is the one that actually misleads: a run
    # where BOTH were examined must be distinguishable from the one above. It was
    # not, because neither printed anything per target.
    VerdictCase "verdict-both-clean-says-so-per-target" \
        "FastCacheTest=clean fastcache-compile-node-tests=clean" \
        "FastCacheTest" "fastcache-compile-node-tests" "clean" \
        "!NOT RUN" "!STARTED" "!FAILED"

    # An early failure, before any target: every row reports that it was not
    # reached. Silence used to be the only signal here, and silence is also what a
    # scrolled-past line and a truncated log look like.
    VerdictCase "verdict-nothing-reached-at-all" \
        "FastCacheTest=not-run fastcache-compile-node-tests=not-run" \
        "NOT RUN" "!clean" "!FAILED"

    # STARTED is not NOT RUN, and it is not FAILED either. A target killed
    # mid-run DID run, and saying it did not would be a lie about the ten minutes
    # it spent; saying it FAILED would be a verdict nothing reached. Three states,
    # three diagnoses, three different people.
    VerdictCase "verdict-started-is-not-unstarted" \
        "FastCacheTest=running" \
        "STARTED, NO VERDICT" "!NOT RUN" "!FAILED"

    # The acceptance clause a fall-through would silently break: a state this
    # renderer does not know is a BUG IN THE GATE, and must not be dressed as the
    # nearest plausible verdict. `not-run` therefore has an arm of its own -- when
    # the two shared one, an unrecognised state rendered as NOT RUN, which reads
    # as a considered verdict about a target nobody looked at.
    VerdictCase "verdict-unrecognised-state-renders-as-unrecognised" \
        "FastCacheTest=wat" \
        "UNKNOWN STATE" "wat" "bug in the gate" \
        "!NOT RUN" "!clean" "!FAILED" "!STARTED"

    # The second acceptance clause, and the one a literal would pass: the account
    # must READ the table. Now that the renderer is pure, the derivation lives in
    # `TargetPairs`, so that is what this drives -- with a THIRD row staged, which
    # must appear WITHOUT this file being edited (#492: a list is exact about what
    # it knows and silent about what it does not). Staged inside the command
    # substitution, which is already a subshell, so the gate's own scope cannot
    # leak into the cases after it.
    #
    # It covers the absence default in the same breath: two verdicts against
    # three rows, and the third must come back `not-run` rather than aborting the
    # render under `set -u`.
    Expect "verdict-pairs-are-derived-from-the-table" 0 0 \
        "$(TARGETS=("FastCacheTest|[async]" "fastcache-compile-node-tests|" "a-third-target|[new]")
           TargetVerdicts=("clean" "clean")
           TargetPairs)" \
        "FastCacheTest=clean" "fastcache-compile-node-tests=clean" "a-third-target=not-run"

    # Every refusal that BELONGS to a target goes through `FailTarget`, checked at
    # the SOURCE rather than left to a reader. A bare `fatal` inside `RunTarget`
    # renders that target `STARTED, NO VERDICT` -- a true statement and the wrong
    # one, since the target ran and was refused. Nothing a pure renderer test can
    # see, because the fault is in the WIRING: `fatal` exits, so the verdict is
    # never recorded and the case would have to survive the exit to notice.
    # `PurgeExpired` is the standing example of a correct, tested mechanism with
    # no production caller -- assert the wiring.
    ran=$(( ran + 1 ))
    bareFatal="$(awk '
        /^RunTarget\(\) \{/ { inFn = 1; next }
        inFn && /^\}/         { inFn = 0 }
        inFn && /(^|[^A-Za-z_])fatal / && !/^[ \t]*#/ { print NR ": " $0 }
    ' "${BASH_SOURCE[0]}")"
    if [[ -n "$bareFatal" ]]; then
        echo "  FAIL verdict-target-refusals-record-FAILED: a bare \`fatal\` inside RunTarget" >&2
        printf '%s\n' "$bareFatal" | sed 's/^/       | /' >&2
        failures=$(( failures + 1 ))
    else
        echo "  ok   verdict-target-refusals-record-FAILED"
    fi

    # A verdict recorded against a name no row carries is a programming error and
    # says so. A table that quietly drops entries is the defect this mechanism
    # exists to remove, so the mechanism must not have it.
    ran=$(( ran + 1 ))
    if SetTargetVerdict "no-such-target-exists" clean 2>/dev/null; then
        echo "  FAIL verdict-unknown-target-is-refused: accepted an unknown name" >&2
        failures=$(( failures + 1 ))
    else
        echo "  ok   verdict-unknown-target-is-refused"
    fi

    echo "tsan-gate --self-test: ${ran} cases ran, ${failures} failed"
    [[ "$failures" -eq 0 ]] || exit 1
    exit 0
}

if [[ "${1:-}" == "--self-test" ]]; then
    SelfTestTarget=""
    RenderVerdictsOnExit="no"
    SelfTest
fi

# ---------------------------------------------------------------------------

[[ -d "$BUILD_DIR" ]] || fatal "build directory not found: ${BUILD_DIR}"
[[ -f "$SUPPRESSIONS" ]] || fatal "suppressions file not found: ${SUPPRESSIONS}"
command -v nm >/dev/null || fatal "nm is required to verify instrumentation"
# No companion check for the bound. There used to be one -- `timeout(1) is
# required` -- and deleting it is not a relaxation: `run_bounded` is bash, so
# there is no binary whose absence could leave this gate running an unbounded
# wait, which is the only thing that refusal protected (#488).

# `TSAN_OPTIONS` is a space-separated list with no quoting mechanism at all, so a
# checkout under `~/My Projects` truncates `suppressions=` at the space and the
# runtime dies before main. That surfaces as "the canary exited N but reported no
# data race" -- a correct refusal naming the wrong cause, which is the one thing
# every message in this script is written to avoid. Say it here instead.
[[ "$SUPPRESSIONS" != *" "* ]] \
    || fatal "TSAN_OPTIONS cannot carry a path with a space in it, and this checkout has one:
    ${SUPPRESSIONS}
    Move the checkout, or set TSAN_OPTIONS yourself and run the binaries by hand."

note "build directory: ${BUILD_DIR}"

# A build that dropped `add_compile_options` and kept `add_link_options` is the
# shape of the defect `cmake/portable/Sanitizers.cmake` records, and this is the
# cheap half of catching it: the rulebook names where to read the compile line --
# `build.ninja`, not the cache and not the configure log, which are the two places
# that lied.
#
# It is the cheap half and not the whole answer. `build.ninja` says what the build
# WOULD compile; the objects on disk are what it DID, and a stale object or a
# compiler-cache replay can differ from both. `AssertInstrumented` reads the
# objects, and is what actually decides.
NINJA_FILE="${BUILD_DIR}/build.ninja"
[[ -f "$NINJA_FILE" ]] || fatal "no ${NINJA_FILE}: this gate reads the COMPILE line from the generator's own file, and only the Ninja generator writes one. Configure with the clang-tsan preset."
if ! grep -q -- '-fsanitize=thread' "$NINJA_FILE"; then
    fatal "the build in ${BUILD_DIR} instrumented nothing: no compile line in build.ninja carries -fsanitize=thread.
    The cache and the configure log are not evidence here -- they are the two that
    lied in cmake/portable/Sanitizers.cmake. Reconfigure with the clang-tsan preset."
fi
note "compile lines carrying -fsanitize=thread: $(grep -c -- '-fsanitize=thread' "$NINJA_FILE")"

# Every instrumentation question first, then the runs. An uninstrumented second
# binary is a one-second answer, and diagnosing it only after the first target's
# full sanitized run wastes the whole of that run.
AssertInstrumented tsan-canary
for row in "${TARGETS[@]}"; do
    AssertInstrumented "${row%%|*}"
done

AssertCanaryFires

for row in "${TARGETS[@]}"; do
    RunTarget "${row%%|*}" "${row#*|}"
done

# No literal here. The per-target table the EXIT trap prints IS the green
# account, and it is derived from TARGETS -- so this line states only the thing
# the table cannot: that the gate reached its end rather than stopping somewhere
# in the middle with every row already marked.
note "ThreadSanitizer gate complete -- every row of TARGETS reached a verdict"
