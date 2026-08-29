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
#   a target's symbols cannot be read     -> "no symbol table"
#   a target has no __tsan_init           -> "built WITHOUT instrumentation"
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

AssertInstrumented() {
    local name="$1" path
    path="$(BinaryPath "$name")"
    [[ -f "$path" ]] || fatal "${name}: not built (looked in ${BUILD_DIR}/target). Build it before running this gate."
    # `nm` into a variable and match afterwards, never `nm | grep -q`: under
    # `set -o pipefail` a `grep -q` that matches EARLY closes the pipe, `nm` dies
    # of SIGPIPE, and the pipeline reports failure -- so a correctly instrumented
    # binary is diagnosed as an uninstrumented one. Verified here, not theorised:
    # that is exactly how this function first behaved.
    #
    # `--no-demangle`, and only the defined global symbols: `__tsan_init` is a C
    # symbol that is never mangled, so demangling buys nothing at all. Measured on
    # this tree, `FastCacheTest` has ~81k symbol table entries and ~42k defined
    # globals -- a few MB into a bash variable, three times a run, well under a
    # second in total.
    local symbols
    symbols="$(nm -g --defined-only --no-demangle "$path" 2>/dev/null || true)"
    [[ -n "$symbols" ]] || fatal "${name}: nm produced no symbol table for ${path}; cannot verify instrumentation (stripped binary, or the wrong nm for this object format)."
    if [[ "$symbols" != *__tsan_init* ]]; then
        fatal "${name}: built WITHOUT ThreadSanitizer instrumentation (no __tsan_init).
    The build carries no -fsanitize=thread even though this gate was invoked.
    Check ENABLE_SANITIZER_THREAD reached the compile line, not just the cache:
        grep -c fsanitize=thread ${BUILD_DIR}/build.ninja
    See cmake/portable/Sanitizers.cmake for the precedent."
    fi
    note "${name}: instrumented"
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

# `__tsan_init` is defined by the sanitizer RUNTIME, which the link line pulls in
# whole -- so it answers "was this artefact linked against TSan" and, on its own,
# would still say yes for a build that dropped `add_compile_options` and kept
# `add_link_options`. That asymmetry is precisely the shape of the defect
# `cmake/portable/Sanitizers.cmake` records. The compile line is the other half of
# the answer, and the rulebook already names where to read it: `build.ninja`, not
# the cache and not the configure log, which are the two places that lied.
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
