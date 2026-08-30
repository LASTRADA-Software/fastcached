#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The gate .agent/rules/build-and-toolchain.md asks for, as a script rather than
# as a paragraph.
#
# "Build at least one release configuration and one non-clang compiler locally
# before pushing" is advice that has to be remembered, and this branch has twice
# paid a full CI cycle for forgetting it -- once for a GCC -O3 `-Wnull-dereference`
# through an inlined `memcpy`, which clang does not emit at any level, and once for
# a clang-tidy check the version on PATH had never heard of.
#
# Both are configurations a developer HAS and does not run. So this runs them.
#
# What it covers, and why each earns its minutes:
#
#   clang-format  at the version CI pins. Successive LLVM releases disagree about
#                 formatting, so a tree clean under whatever is on PATH can still be
#                 rejected -- for code nobody mis-wrote.
#   clang-debug   PEDANTIC + ASan + UBSan + clang-tidy, the ANALYSER pinned to the
#                 same version. The default agent preset, the only place sanitizers
#                 run at all, and the only preset here that tidies anything -- which
#                 is why the other one says so out loud rather than leaving a reader
#                 to infer that the gate's tidy coverage is both.
#   gcc-release   The second compiler, at -O3. A different warning set, and
#                 optimizer-dependent diagnostics that appear at no other level.
#
# What it deliberately does NOT cover: MSVC and clang-cl, which need Windows, and
# macOS/libc++, which needs a Mac. Those stay CI's job, and the point of this script
# is that everything reproducible locally is reproduced locally.
#
# Usage:  scripts/local-gate.sh [--no-format] [--self-test]
#
#   --no-format  skip the clang-format pass. It does NOT loosen the clang-tidy pin:
#                those are two tools and the flag names one of them.
#   --self-test  check the configure decision against synthetic CMake caches and
#                exit. Needs no compiler, no cmake and no clang-tidy, which is what
#                lets `ctest -R local-gate-selftest` run it everywhere.
#
# Exits non-zero on the first configuration that fails, having printed its errors.
# It never runs ctest against a build that did not complete -- a stale binary
# reporting a green suite is the failure mode this ordering exists to prevent, and
# it has happened here.
#
# **bash 3.2**, because `--self-test` is in the default ctest set and macOS ships a
# 2007 `/bin/bash`. No `mapfile`, no `declare -A`, no `local -n` -- and no bare
# `"${arr[@]}"` on an array that can be EMPTY, which is an unbound-variable error
# under `set -u` before 4.4. Every array below is non-empty by construction.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root" || exit 1

format=1
self_test=0
for arg in "$@"; do
    case "$arg" in
        --no-format) format=0 ;;
        --self-test) self_test=1 ;;
        *) echo "usage: $0 [--no-format] [--self-test]" >&2; exit 2 ;;
    esac
done

# The version CI pins, named rather than taken from PATH. A machine carrying both
# 20 and 22 resolves the bare name to whichever comes first, and the preset's own
# `CMAKE_CXX_CLANG_TIDY=clang-tidy` inherits that -- so a "clang-tidy clean" build
# can mean nothing, with the version it used printed nowhere.
#
# That paragraph described THIS SCRIPT until the pin below existed. It named the
# version for clang-format and then handed the analyser to PATH order: the argument
# was written down and not carried one call further, so the gate's own comment
# documented the defect it had. The pin now reaches the configure, and the run
# prints which binary it used.
tools_version="${CLANG_TOOLS_VERSION:-22}"

fail() { echo "GATE FAILED: $*" >&2; exit 1; }

# What each preset is here for, and whether the analyser pin has to reach it.
#
# A table rather than two calls with a flag threaded through them, because the
# second column is a fact about `CMakePresets.json` -- `clang-debug` sets
# `ENABLE_TIDY=ON` and `gcc-release` does not -- and a reader asking "does this gate
# tidy both configurations?" should find the answer written down rather than infer
# it from the absence of an argument. The answer is no, and a run says so.
#
#   preset|tidy      the preset runs clang-tidy, so the pin must reach it
#   preset|no-tidy   it does not, and the run prints that rather than staying silent
gate_presets=(
    "clang-debug|tidy"
    "gcc-release|no-tidy"
)

# The analyser CMake actually cached for a build directory, or empty when there is
# no such entry.
#
# `CLANG_TIDY_EXE` and not `CMAKE_CXX_CLANG_TIDY`, because that is the entry
# `cmake/portable/ClangTidy.cmake`'s `find_program` fills -- and `find_program`
# never revisits a filled cache entry, so this value outlives every reason it was
# chosen. `CLANG_TIDY_EXE-NOTFOUND` is a value like any other here and compares
# unequal, which is the point: a directory configured on a machine that had no
# clang-tidy must not be accepted as one that has the right clang-tidy. That is the
# same shape as the stale `FASTCACHE_CC-NOTFOUND` which kept whole build trees on
# sccache without ever saying so.
#
# One `awk` and no pipe. `sed ... | head -1` would be the obvious spelling and is
# the `producer | grep -q` trap in another costume: `head` exits at the first line,
# the producer dies of SIGPIPE, and `pipefail` reports the producer's status on the
# SUCCESS path.
# @param 1 Path to a CMakeCache.txt.
cached_tidy() {
    [[ -f "$1" ]] || return 0
    awk '/^CLANG_TIDY_EXE:/ { sub(/^[^=]*=/, ""); print; exit }' "$1"
}

# Why this preset has to be configured, or empty when it does not.
#
# Split out because it is exactly what `--self-test` can check without a toolchain,
# and because getting it wrong is silent in the direction that matters: a gate that
# skips the re-configure keeps the wrong analyser and reports clean.
# @param 1 Path to the build directory's CMakeCache.txt.
# @param 2 `tidy` or `no-tidy`, from the table above.
# @param 3 Absolute path of the pinned analyser.
configure_reason() {
    local cache="$1" analyser="$2" wanted="$3"

    # `cmake --build --preset` on a directory that does not exist fails with
    # "<path> is not a directory", which names neither the preset nor the fix and is
    # what a FRESH CHECKOUT gets from the one script everybody is told to run.
    if [[ ! -f "$cache" ]]; then
        echo "no build directory yet"
        return 0
    fi

    # Otherwise only when the analyser is wrong: a re-configure costs over a minute
    # every run to do nothing. But "the cache file exists" was the WHOLE test until
    # now, and that is precisely what let a build directory keep the analyser it
    # first found forever -- re-running the gate could not fix it, because re-running
    # the gate is what skipped the configure.
    if [[ "$analyser" == "tidy" ]]; then
        local have
        have="$(cached_tidy "$cache")"
        if [[ "$have" != "$wanted" ]]; then
            echo "cached clang-tidy is ${have:-absent}, not $wanted"
            return 0
        fi
    fi
}

if [[ "$self_test" -eq 1 ]]; then
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT
    self_test_failures=0

    # @param 1 What is being checked. @param 2 Expected. @param 3 Actual.
    expect() {
        if [[ "$2" != "$3" ]]; then
            echo "SELF-TEST FAILED: $1: expected '$2', got '$3'" >&2
            self_test_failures=$((self_test_failures + 1))
        fi
    }

    printf 'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-22\n' > "$scratch/right"
    printf 'CLANG_TIDY_EXE:FILEPATH=/usr/bin/clang-tidy-20\n' > "$scratch/wrong"
    printf 'CLANG_TIDY_EXE:FILEPATH=CLANG_TIDY_EXE-NOTFOUND\n' > "$scratch/notfound"
    printf 'CMAKE_BUILD_TYPE:STRING=Debug\n' > "$scratch/absent"

    expect "reads the cached analyser" \
        "/usr/bin/clang-tidy-20" "$(cached_tidy "$scratch/wrong")"
    expect "reads nothing when the entry is absent" \
        "" "$(cached_tidy "$scratch/absent")"
    expect "reads the NOTFOUND sentinel as a value rather than as absence" \
        "CLANG_TIDY_EXE-NOTFOUND" "$(cached_tidy "$scratch/notfound")"

    expect "a missing build directory is configured" \
        "no build directory yet" \
        "$(configure_reason "$scratch/nope" tidy /usr/bin/clang-tidy-22)"
    expect "a directory already on the pinned analyser is left alone" \
        "" "$(configure_reason "$scratch/right" tidy /usr/bin/clang-tidy-22)"

    # The three that the old `if [[ ! -f CMakeCache.txt ]]` answered "leave it
    # alone", each of which is a gate reporting on an analyser nobody asked for.
    expect "a directory on the WRONG analyser is re-configured" \
        "cached clang-tidy is /usr/bin/clang-tidy-20, not /usr/bin/clang-tidy-22" \
        "$(configure_reason "$scratch/wrong" tidy /usr/bin/clang-tidy-22)"
    expect "a directory that never found one is re-configured" \
        "cached clang-tidy is CLANG_TIDY_EXE-NOTFOUND, not /usr/bin/clang-tidy-22" \
        "$(configure_reason "$scratch/notfound" tidy /usr/bin/clang-tidy-22)"
    expect "a directory with no analyser entry at all is re-configured" \
        "cached clang-tidy is absent, not /usr/bin/clang-tidy-22" \
        "$(configure_reason "$scratch/absent" tidy /usr/bin/clang-tidy-22)"

    # And the other direction, which costs minutes rather than correctness: a preset
    # that runs no analyser must never be re-configured over one, or `gcc-release`
    # rebuilds from scratch on every run of the gate chasing a tool it does not use.
    expect "a no-tidy preset ignores the analyser entirely" \
        "" "$(configure_reason "$scratch/wrong" no-tidy /usr/bin/clang-tidy-22)"

    # The table drives both of the above, so a row that stopped parsing would make
    # every check here vacuous while every one of them passed.
    expect "the preset table still has two rows" "2" "${#gate_presets[@]}"
    for row in "${gate_presets[@]}"; do
        case "${row#*|}" in
            tidy|no-tidy) ;;
            *) echo "SELF-TEST FAILED: unknown analyser column in '$row'" >&2
               self_test_failures=$((self_test_failures + 1)) ;;
        esac
    done

    [[ "$self_test_failures" -eq 0 ]] || exit 1

    # The interpreter is named for the same reason the gate names its analyser: this
    # script is written to bash 3.2 because macOS ships one, and "it passed on some
    # bash" is the shape of claim this whole file exists to stop making. A runner
    # with a newer bash first on PATH proves the checks and not the constraint, and
    # the log is the only place that difference is visible.
    echo "LOCAL GATE SELF-TEST PASSED (bash ${BASH_VERSION})"
    exit 0
fi

# Resolved once, to an absolute path, and checked before anything is built -- the
# treatment `clang-format` already had, for the same reason: a gate whose tool is
# missing must refuse BY NAME rather than fall back to whatever PATH offers, because
# the fallback is a clean report about a different analyser.
#
# Independent of `--no-format`, which names the formatter and not this.
#
# Only demanded when some preset in the table actually tidies. A gate that refused
# to start over a tool none of its configurations use would be a gate people stop
# running.
tidy=""
tidy_path=""
for row in "${gate_presets[@]}"; do
    if [[ "${row#*|}" == "tidy" ]]; then
        tidy="clang-tidy-${tools_version}"
        tidy_path="$(command -v "$tidy" 2>/dev/null || true)"
        [[ -n "$tidy_path" ]] || fail "$tidy not found, and this gate will not fall back to whatever clang-tidy is on PATH; install it (pip download clang-tidy==${tools_version}.1.0) or set CLANG_TOOLS_VERSION"
        break
    fi
done

if [[ "$format" -eq 1 ]]; then
    formatter="clang-format-${tools_version}"
    command -v "$formatter" >/dev/null 2>&1 \
        || fail "$formatter not found; install it or pass --no-format"
    git ls-files '*.h' '*.hpp' '*.cpp' | xargs "$formatter" -i --style=file \
        || fail "clang-format"
    echo "== formatted with $formatter"
fi

# @param 1 The preset to build and test.
# @param 2 `tidy` or `no-tidy`, from the table.
run_preset() {
    local preset="$1"
    local analyser="$2"
    local log
    log="$(mktemp)"

    # The build directory path is spelled rather than asked for, and it is coupled
    # to CMakePresets.json's single `binaryDir` of
    # `${sourceDir}/out/build/${presetName}`. A preset that moved its build
    # directory would configure once too often, which is the harmless direction.
    local cache="out/build/${preset}/CMakeCache.txt"

    # The analyser is passed as a cache entry rather than through the preset,
    # because `find_program` short-circuits on a cache entry that is already set --
    # verified, and the reason `cmake/portable/ClangTidy.cmake` needs no change and
    # stays generic. `-D` on the command line sets that entry even on a directory
    # that already cached a different one.
    #
    # A non-empty array by construction: bash 3.2 under `set -u` treats
    # `"${arr[@]}"` on an empty array as an unbound variable, and this gate runs on
    # macOS.
    local -a configure
    configure=(cmake --preset "$preset")
    if [[ "$analyser" == "tidy" ]]; then
        configure+=("-DCLANG_TIDY_EXE=${tidy_path}")
        echo "== $preset: clang-tidy pinned to $tidy ($tidy_path)"
    else
        echo "== $preset: no clang-tidy (ENABLE_TIDY is off in this preset)"
    fi

    local reason
    reason="$(configure_reason "$cache" "$analyser" "$tidy_path")"
    if [[ -n "$reason" ]]; then
        echo "== $preset: configure ($reason)"
        if ! "${configure[@]}" > "$log" 2>&1; then
            tail -40 "$log"
            fail "$preset configure (full log: $log)"
        fi
    fi

    echo "== $preset: build"
    if ! cmake --build --preset "$preset" > "$log" 2>&1; then
        grep -E 'error:|FAILED' "$log" | head -40
        fail "$preset build (full log: $log)"
    fi

    # Parallel, and that is the point rather than the speed. Every TEST_CASE is
    # its own process under catch_discover_tests, so a fixture that names a
    # scratch directory from a per-process counter hands two concurrent cases the
    # same path and the second wipes the first. That bug has been written five
    # times in this repository and nothing has ever run the tests in the shape
    # that shows it -- CI does not, and neither did this gate. The tests that
    # genuinely cannot share (a daemon, a fixed port) carry RUN_SERIAL.
    #
    # getconf rather than nproc: this gate runs on macOS too.
    local jobs="${FASTCACHE_GATE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

    echo "== $preset: test (--parallel $jobs)"
    if ! ctest --preset "$preset" --parallel "$jobs" > "$log" 2>&1; then
        grep -E '\*\*\*Failed|\*\*\*Timeout|tests passed' "$log" | head -30
        fail "$preset tests (full log: $log)"
    fi
    grep -E 'tests passed' "$log" | head -1
    rm -f "$log"
}

for row in "${gate_presets[@]}"; do
    run_preset "${row%%|*}" "${row#*|}"
done

echo
echo "LOCAL GATE PASSED (clang-debug + gcc-release)"
