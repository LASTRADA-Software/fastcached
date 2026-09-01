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
#   a tag expression matches nothing      -> "tested NOTHING"
#   a target exits 0 with no assertions   -> "refusing to call that clean"
#   a target runs past its deadline       -> "did not finish within"
#   an unsuppressed race                  -> "reported an unsuppressed data race"
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

# `exitcode=66` is TSan's default and is named rather than assumed: this gate
# decides on the exit code, so it must not be something a stray TSAN_OPTIONS in
# the environment can change to 0. `print_suppressions=1` is on for EVERY run,
# canary included, so a suppression that has started matching more than it was
# written for is at least visible rather than silent -- `.tsan-suppressions` says
# that happens on every run, and it only does if it is set in one place.
TsanOptions="halt_on_error=0 exitcode=66 print_suppressions=1 suppressions=${SUPPRESSIONS}"

# Every wait is bounded, which is this repository's oldest testing rule. Running
# the binaries directly rather than through `ctest` loses ctest's own per-test
# TIMEOUT, and TSan changes timing enough that a deadlock is a real outcome: the
# consensus and node tests do real socket and thread work. Unbounded, a hung case
# runs to GitHub's six-hour job limit with nothing saying which target stalled.
# Generous, because the honest measurements are 1.2s and 17.6s -- this is a
# deadlock detector, not a performance budget. `timeout` reports 124.
#
# macOS has no `timeout(1)` -- coreutils installs it as `gtimeout` -- and this
# preset runs there, so look for both rather than refusing a whole platform.
TargetTimeoutSeconds="${FASTCACHE_TSAN_TIMEOUT:-900}"
TimeoutCommand=""
for candidate in timeout gtimeout; do
    if command -v "$candidate" >/dev/null 2>&1; then
        TimeoutCommand="$candidate"
        break
    fi
done

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
cleanup() { rm -rf "${SCRATCH}"; }
trap cleanup EXIT

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
    local path canary_out canary_rc
    path="$(BinaryPath tsan-canary)"

    canary_out="$(TSAN_OPTIONS="${TsanOptions}" "${TimeoutCommand}" 60 "$path" 2>&1)" && canary_rc=0 || canary_rc=$?

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

RunTarget() {
    local name="$1" tags="$2" path rc=0 log summary
    path="$(BinaryPath "$name")"

    # An empty tag expression means "run the whole binary", which is not the same
    # argument as an empty string -- Catch2 would read `""` as a filter matching
    # nothing. An array is how bash spells "this argument is sometimes absent";
    # `${args[@]+...}` rather than a bare `"${args[@]}"`, because an empty array
    # under `set -u` is an unbound variable in bash 3.2, which is what macOS
    # ships and where this preset is also meant to run.
    local args=()
    if [[ -n "$tags" ]]; then
        args=("$tags")
    fi

    log="${SCRATCH}/${name}.log"
    note "running ${name} ${tags:+(${tags})}"
    TSAN_OPTIONS="${TsanOptions}" "${TimeoutCommand}" "${TargetTimeoutSeconds}" \
        "$path" ${args[@]+"${args[@]}"} >"$log" 2>&1 || rc=$?

    # Named before anything else reads the log: a killed process leaves a partial
    # Catch2 summary, which the checks below would diagnose as "reported no
    # assertions" and send the reader to the tag expression.
    if [[ "$rc" -eq 124 ]]; then
        cat "$log"
        fatal "${name} did not finish within ${TargetTimeoutSeconds}s and was killed.
    Under ThreadSanitizer that is a deadlock until proven otherwise, and the last
    case in the log above is where to start. FASTCACHE_TSAN_TIMEOUT raises the
    bound if the suite has simply grown."
    fi

    # A filter that matches nothing is the quietest way for this gate to test
    # nothing and say "clean" -- a typo in the TARGETS table above would do it.
    # Catch2 exits 2 and says so, which would land in the failure branch below,
    # but it would be diagnosed there as a mysterious non-race failure. Name it
    # instead, and assert positively that cases ran rather than trusting the exit
    # code.
    if grep -q 'No test cases matched' "$log"; then
        fatal "${name}: the filter '${tags}' matched no test cases, so this target tested NOTHING.
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
            || fatal "${name}: exited 0 but reported no assertions; refusing to call that clean."
        echo "$summary"
        note "${name}: clean"
        return
    fi

    cat "$log"
    # Distinguish a race from an ordinary test failure, because they are read by
    # different people and fixed in different places. Catch2 prints "All tests
    # passed" even when the process exits 66, so the exit code is what decides and
    # the output is only ever an explanation.
    if grep -q 'WARNING: ThreadSanitizer' "$log"; then
        fatal "${name} reported an unsuppressed data race (exit ${rc}).
    If this is a NEW race, it is its own issue -- file it. If it is a known one,
    it belongs in .tsan-suppressions with its issue number, not deleted from here."
    fi
    fatal "${name} failed (exit ${rc}) without a ThreadSanitizer report."
}

# ---------------------------------------------------------------------------
# `--self-test`: drive AssertInstrumented's verdicts against staged trees.
#
# A guard that has never been seen to fire is the thing this repository keeps
# finding, and #472 is precisely that -- a proof that had never been shown to
# refuse anything, because it structurally could not. So each refusal is staged
# here and required to happen.
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

    # @param 1 case name
    # @param 2 expected exit status
    # @param 3.. text the output must contain; a leading `!` means must NOT
    Case() {
        local name="$1" want="$2"; shift 2
        local out rc=0 pattern
        out="$(PATH="${stub}:$PATH" AssertInstrumented "$SelfTestTarget" 2>&1)" || rc=$?
        ran=$(( ran + 1 ))
        if [[ "$rc" != "$want" ]]; then
            echo "  FAIL ${name}: exited ${rc}, expected ${want}" >&2
            printf '%s\n' "$out" | sed 's/^/       | /' >&2
            failures=$(( failures + 1 ))
            return
        fi
        for pattern in "$@"; do
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
' "0000000000001234 T __tsan_init" > "${BUILD_DIR}/target/${target}"
        dir="${BUILD_DIR}/src/tests/CMakeFiles/${target}.dir"
        mkdir -p "$dir"
        for spec in "$@"; do
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

    rm -rf "$scratch"
    echo
    echo "tsan-gate --self-test: ${ran} cases ran, ${failures} failed"
    [[ "$failures" -eq 0 ]] || exit 1
    exit 0
}

if [[ "${1:-}" == "--self-test" ]]; then
    SelfTestTarget=""
    SelfTest
fi

# ---------------------------------------------------------------------------

[[ -d "$BUILD_DIR" ]] || fatal "build directory not found: ${BUILD_DIR}"
[[ -f "$SUPPRESSIONS" ]] || fatal "suppressions file not found: ${SUPPRESSIONS}"
command -v nm >/dev/null || fatal "nm is required to verify instrumentation"
[[ -n "$TimeoutCommand" ]] || fatal "timeout(1) is required; this gate does not run an unbounded wait. On macOS it is gtimeout, from coreutils."

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

note "all scoped targets clean under ThreadSanitizer"
