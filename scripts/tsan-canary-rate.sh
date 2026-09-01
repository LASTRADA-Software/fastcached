#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Measure how often the ThreadSanitizer canary's deliberate race goes UNREPORTED.
#
#   cmake --preset clang-tsan
#   cmake --build --preset clang-tsan --target tsan-canary
#   scripts/tsan-canary-rate.sh out/build/clang-tsan 300
#
# ## Why this exists rather than a paragraph
#
# `scripts/tsan-gate.sh` runs the canary ONCE and refuses the whole suite if the
# race is not reported. That is the correct thing to do -- a suite whose sanitizer
# cannot be shown live is meaningless -- but it makes the canary's reliability a
# property of the CI job's flake rate rather than of anything anyone can inspect.
#
# The canary used to be silent in 2-4% of runs (#473), and the cost of that is not
# the red build. It is that a gate which is red a few percent of the time teaches
# everyone to re-run it, and a gate people reflexively re-run is disarmed exactly
# as thoroughly as one that was deleted. That failure arrives through habit, not
# through code, so "it passed when I tried it" is not the evidence that settles
# it. A RATE is.
#
# So the acceptance for a change to `src/tests/TsanCanary.cpp` is a rate over at
# least a few hundred runs, recorded, and reproducible by whoever next doubts it.
# This is that instrument. Measured at roughly 27 ms a run on a 32-core box --
# about eight seconds for the default 300, and a couple of minutes for the five
# thousand this canary was accepted on. That cost is a process spawn and a
# sanitized start-up rather than the race itself, so it is set by the machine
# and not by the loop: 29.2 ms for the current canary's sixteen thousand
# increments against 26.0 ms for the thousand-increment one it replaced.
#
# ## What it does NOT tell you
#
# A rate measured here is a rate on THIS machine, at this core count, under this
# load. The silent runs are a timing phenomenon, so a different box can differ --
# which is why the pinned arm exists: `--pin 0,1` narrows the machine to two CPUs
# and is the closest thing available locally to a two-core CI runner. Report both
# numbers, or the figure carries a condition it does not state.
#
# Exit codes: 0 = the rate is at or below the threshold; 1 = it is above it, or a
# prerequisite is missing. It is not registered as a ctest: a few hundred runs of
# a sanitized binary is not something to put in the default set, and pinning it to
# one threshold in CI would make it the flaky gate it exists to prevent.
set -euo pipefail

BUILD_DIR=""
RUNS=300
PIN=""
# The bar. Zero observed silent runs is what a fixed canary looks like; this
# refuses at the first one rather than at some tolerated fraction, because the
# thing being measured is supposed to be certain and a "small" rate is what #473
# already was.
MAX_SILENT=0

Usage() {
    cat <<'EOF'
usage: tsan-canary-rate.sh <build-dir> [runs] [--pin CPUS] [--max-silent N]

  <build-dir>      a directory configured with the clang-tsan preset
  runs             how many times to run the canary (default 300)
  --pin CPUS       run under `taskset -c CPUS`, e.g. --pin 0,1 for a two-core box
  --max-silent N   fail above N silent runs (default 0)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)     Usage; exit 0 ;;
        --pin)         PIN="$2"; shift 2 ;;
        --max-silent)  MAX_SILENT="$2"; shift 2 ;;
        -*)            echo "unknown option: $1" >&2; Usage >&2; exit 1 ;;
        *)
            if [[ -z "$BUILD_DIR" ]]; then BUILD_DIR="$1"; else RUNS="$1"; fi
            shift ;;
    esac
done

fatal() { echo "tsan-canary-rate: $*" >&2; exit 1; }

[[ -n "$BUILD_DIR" ]] || { Usage >&2; exit 1; }
[[ -d "$BUILD_DIR" ]] || fatal "build directory not found: ${BUILD_DIR}"

CANARY="${BUILD_DIR}/target/tsan-canary"
[[ -x "$CANARY" ]] || fatal "no tsan-canary at ${CANARY}.
    Build it first:  cmake --build --preset clang-tsan --target tsan-canary"

runner=("$CANARY")
if [[ -n "$PIN" ]]; then
    command -v taskset >/dev/null 2>&1 \
        || fatal "--pin needs taskset(1), which this system does not have."
    runner=(taskset -c "$PIN" "$CANARY")
fi

# `exitcode=66` so a reported race is distinguishable from the program's own exit,
# and `halt_on_error=0` so a run that reports several still finishes. NO
# suppressions file: `.tsan-suppressions` never matches the canary -- it races on
# its own global in a binary linking no FastCache frame -- and leaving it out of
# this measurement keeps the two questions apart. The gate runs the canary WITH it
# on purpose, which is a different assertion (that no pattern is broad enough to
# swallow an obvious race).
export TSAN_OPTIONS="halt_on_error=0 exitcode=66"

reported=0
silent=0
declare -a silentCounts=()

started=$SECONDS
i=0
while [[ "$i" -lt "$RUNS" ]]; do
    # Captured and matched afterwards, never `"${runner[@]}" | grep -q`: under
    # `set -o pipefail` a `grep -q` that matches early closes the pipe, the
    # producer dies of SIGPIPE, and the pipeline reports failure ON THE SUCCESS
    # PATH. It is also racy rather than deterministic, so two runs of a harness
    # built that way disagree -- measured while fixing #472.
    out="$("${runner[@]}" 2>&1 || true)"
    if [[ "$out" == *"data race"* ]]; then
        reported=$(( reported + 1 ))
    else
        silent=$(( silent + 1 ))
        # The increment count is kept for the silent runs only, because that is
        # the correlation #473 turned on: every silent run printed a count showing
        # the race HAD happened. A silent run whose count shows no lost increments
        # would be a different finding and should not be folded in quietly.
        silentCounts+=("$(printf '%s\n' "$out" | sed -n 's/.*: \([0-9]*\) increments observed.*/\1/p' | head -1)")
    fi
    i=$(( i + 1 ))
done
elapsed=$(( SECONDS - started ))

echo "canary   : ${CANARY}"
echo "runs     : ${RUNS}${PIN:+   (pinned to CPUs ${PIN})}"
echo "cores    : $( (nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo '?') )"
echo "elapsed  : ${elapsed}s"
echo "reported : ${reported}"
echo "silent   : ${silent}"
if [[ "$silent" -gt 0 ]]; then
    echo "silent-run increment counts: ${silentCounts[*]}"
fi

if [[ "$silent" -gt "$MAX_SILENT" ]]; then
    echo
    echo "FAILED: ${silent} of ${RUNS} runs did not report the race (allowed: ${MAX_SILENT})." >&2
    echo "    The canary's job is to be caught every time. A few percent is enough to" >&2
    echo "    teach people to re-run a red gate, which disarms it as thoroughly as" >&2
    echo "    deleting it would. See #473." >&2
    exit 1
fi

echo
echo "OK: the race was reported in all ${RUNS} runs."
