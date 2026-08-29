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
# It asks that question twice, because it is two questions. `__tsan_init` in the
# TEST BINARIES answers "was this artefact instrumented" -- the artefact actually
# under test, not some other build in the same tree. The canary answers "does the
# runtime detect and report" -- which `TSAN_OPTIONS`, a suppressions file, or a
# stripped runtime can each break while leaving the instrumentation intact.
# Neither substitutes for the other.
#
# Five ways this refuses, each with its own message because each is fixed
# somewhere different, and each verified by making it happen rather than reasoned
# about:
#
#   1. a target is not built            -> "not built"
#   2. a target carries no __tsan_init  -> "built WITHOUT instrumentation"
#   3. the canary does not go red       -> "the deliberate race was NOT reported"
#   4. a tag expression matches nothing -> "tested NOTHING"
#   5. an unsuppressed race             -> "reported an unsuppressed data race"
#
# (4) is the one that looks like paranoia and is not: a typo in the TARGETS table
# below runs zero cases, and every other signal in the run says clean.
#
# **The suite is scoped, and `ctest` has no vocabulary for the scope.** The
# concurrency in this tree is in `Async`, `Consensus`, `Distributed` and the node.
# `catch_discover_tests` registers cases by NAME and this project's Catch2 (3.6)
# predates `ADD_TAGS_AS_LABELS`, so there is no ctest label to select on and the
# scope has to be a Catch2 tag expression. Running the two binaries directly also
# collapses ~700 sanitized processes into two: measured at 1.2s and 17.6s against
# several minutes of per-process runtime startup.
#
# The tag list is NOT self-evidently the right one, and an earlier version of it
# was wrong: `[async],[consensus],[distributed]` looks complete and silently
# excluded six of ten `Async/` test files, because the reactor and coroutine tests
# are tagged `[reactor]` and `[task]` and carry no `[async]`. It matched 511 cases
# and they passed. Nothing in the run could have said otherwise -- which is
# refusal (4) failing at one level up from where it can see.
#
# So the tag list is not trusted here. `scripts/check-tsan-scope.cmake` runs in the
# default ctest set and fails when a test file in one of those directories carries
# no tag this expression selects, which is what makes the convention load-bearing
# rather than aspirational. Change one and it will tell you about the other.
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
TARGETS=(
    "FastCacheTest|[async],[consensus],[distributed],[reactor],[task]"
    "fastcache-compile-node-tests|"
)

fatal() {
    echo "::error::tsan-gate: $*" >&2
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
# Locate the binaries.
# ---------------------------------------------------------------------------

[[ -d "$BUILD_DIR" ]] || fatal "build directory not found: ${BUILD_DIR}"

BinaryPath() {
    # `target/` is where this project's CMake puts executables
    # (CMAKE_RUNTIME_OUTPUT_DIRECTORY, set in the top-level CMakeLists).
    #
    # No `find` fallback, on purpose. Searching the build tree and taking the
    # first match is a GUESS, and a guess that silently picks some other build's
    # binary is the failure this whole script exists to refuse -- it would happily
    # verify and run an artefact nobody asked about. A layout change should stop
    # the gate with a path in the message, which is a one-line fix, rather than
    # quietly test the wrong file.
    printf '%s' "${BUILD_DIR}/target/$1"
}

# ---------------------------------------------------------------------------
# Question one: is the artefact under test instrumented?
#
# Spelled with an explicit existence check FIRST, and that is not defensive
# padding. `nm` on a path that does not exist prints nothing and a grep for
# `__tsan_init` counts zero -- which is character-for-character what an
# uninstrumented binary looks like. A missing file must not be reportable as a
# missing sanitizer; they need different fixes.
# ---------------------------------------------------------------------------

AssertInstrumented() {
    local path="$1" label="$2"
    [[ -n "$path" && -f "$path" ]] || fatal "${label}: not built (looked in ${BUILD_DIR}/target). Build it before running this gate."
    command -v nm >/dev/null || fatal "nm is required to verify instrumentation"
    # `nm` into a variable and match afterwards, never `nm | grep -q`: under
    # `set -o pipefail` a `grep -q` that matches EARLY closes the pipe, `nm` dies
    # of SIGPIPE, and the pipeline reports failure -- so a correctly instrumented
    # binary is diagnosed as an uninstrumented one. Verified here, not theorised:
    # that is exactly how this function first behaved.
    #
    # `--no-demangle`, and only the defined global symbols: `__tsan_init` is a C
    # symbol that is never mangled, so demangling a Debug+TSan symbol table of
    # several hundred thousand entries -- into a bash variable, three times a run
    # -- buys nothing at all.
    local symbols
    symbols="$(nm -g --defined-only --no-demangle "$path" 2>/dev/null || true)"
    [[ -n "$symbols" ]] || fatal "${label}: nm produced no symbol table for ${path}; cannot verify instrumentation (stripped binary, or the wrong nm for this object format)."
    if [[ "$symbols" != *__tsan_init* ]]; then
        fatal "${label}: built WITHOUT ThreadSanitizer instrumentation (no __tsan_init).
    The build carries no -fsanitize=thread even though this gate was invoked.
    Check ENABLE_SANITIZER_THREAD reached the compile line, not just the cache:
        grep -c fsanitize=thread ${BUILD_DIR}/build.ninja
    See cmake/portable/Sanitizers.cmake for the precedent."
    fi
    note "${label}: instrumented"
}

# ---------------------------------------------------------------------------
# Question two: does the runtime detect and report?
#
# Run WITH the suppressions file, deliberately. That proves in the same breath
# that `.tsan-suppressions` is not broad enough to swallow an obvious race -- a
# pattern that silences the canary fails here rather than quietly disarming the
# whole gate.
# ---------------------------------------------------------------------------

AssertCanaryFires() {
    local path canary_out canary_rc
    path="$(BinaryPath tsan-canary)"
    AssertInstrumented "$path" "tsan-canary"

    canary_out="$(TSAN_OPTIONS="halt_on_error=0 exitcode=66 suppressions=${SUPPRESSIONS}" "$path" 2>&1)" && canary_rc=0 || canary_rc=$?

    if [[ "$canary_rc" -eq 0 ]]; then
        fatal "the deliberate race in src/tests/TsanCanary.cpp was NOT reported.
    The binary is instrumented, so the runtime is not reporting: TSAN_OPTIONS, the
    suppressions file, or the sanitizer runtime itself. A clean suite would be
    meaningless, so this gate fails instead of passing.
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
    local name="$1" tags="$2" path rc=0 log
    path="$(BinaryPath "$name")"
    AssertInstrumented "$path" "$name"

    log="${SCRATCH}/${name}.log"
    note "running ${name} ${tags:+(${tags})}"
    # `exitcode=66` is TSan's default and is named rather than assumed: this gate
    # decides on the exit code, so it must not be something a stray TSAN_OPTIONS
    # in the environment can change to 0.
    if [[ -n "$tags" ]]; then
        TSAN_OPTIONS="halt_on_error=0 exitcode=66 print_suppressions=1 suppressions=${SUPPRESSIONS}" \
            "$path" "$tags" >"$log" 2>&1 || rc=$?
    else
        TSAN_OPTIONS="halt_on_error=0 exitcode=66 print_suppressions=1 suppressions=${SUPPRESSIONS}" \
            "$path" >"$log" 2>&1 || rc=$?
    fi

    # On success, the interesting lines only: a passing sanitizer run is hundreds
    # of lines of Catch2 output that nobody reads, and burying the suppression
    # count in it is how a suppression that has started matching more than it was
    # written for goes unnoticed. On failure, everything -- that is when the whole
    # log is the evidence.
    if [[ "$rc" -eq 0 ]]; then
        grep -E 'assertions in|Matched [0-9]+ suppressions|No tests ran' "$log" || true
    else
        cat "$log"
    fi

    # A filter that matches nothing is the quietest way for this gate to test
    # nothing and say "clean" -- a typo in the TARGETS table above would do it.
    # Catch2 exits 2 and says so, which lands in the failure branch below, but it
    # would be diagnosed there as a mysterious non-race failure. Name it instead,
    # and assert positively that cases ran rather than trusting the exit code.
    if grep -q 'No test cases matched' "$log"; then
        fatal "${name}: the filter '${tags}' matched no test cases, so this target tested NOTHING.
    Fix the tag expression in the TARGETS table in $(basename "${BASH_SOURCE[0]}")."
    fi
    if [[ "$rc" -eq 0 ]] && ! grep -q 'assertions in' "$log"; then
        fatal "${name}: exited 0 but reported no assertions; refusing to call that clean."
    fi

    if [[ "$rc" -ne 0 ]]; then
        # Distinguish a race from an ordinary test failure, because they are read
        # by different people and fixed in different places. Catch2 prints "All
        # tests passed" even when the process exits 66, so the exit code is what
        # decides and the output is only ever an explanation.
        if grep -q 'WARNING: ThreadSanitizer' "$log"; then
            echo "::error::tsan-gate: ${name} reported an unsuppressed data race (exit ${rc})" >&2
            echo "If this is a NEW race, it is its own issue -- file it. If it is a known one," >&2
            echo "it belongs in .tsan-suppressions with its issue number, not deleted from here." >&2
        else
            echo "::error::tsan-gate: ${name} failed (exit ${rc}) without a ThreadSanitizer report" >&2
        fi
        exit 1
    fi
    note "${name}: clean"
}

# ---------------------------------------------------------------------------

[[ -f "$SUPPRESSIONS" ]] || fatal "suppressions file not found: ${SUPPRESSIONS}"

note "build directory: ${BUILD_DIR}"
AssertCanaryFires

for row in "${TARGETS[@]}"; do
    RunTarget "${row%%|*}" "${row#*|}"
done

note "all scoped targets clean under ThreadSanitizer"
