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
#   clang-debug   PEDANTIC + ASan + UBSan + clang-tidy. The default agent preset and
#                 the only place sanitizers run at all.
#   gcc-release   The second compiler, at -O3. A different warning set, and
#                 optimizer-dependent diagnostics that appear at no other level.
#
# What it deliberately does NOT cover: MSVC and clang-cl, which need Windows, and
# macOS/libc++, which needs a Mac. Those stay CI's job, and the point of this script
# is that everything reproducible locally is reproduced locally.
#
# Usage:  scripts/local-gate.sh [--no-format]
#
# Exits non-zero on the first configuration that fails, having printed its errors.
# It never runs ctest against a build that did not complete -- a stale binary
# reporting a green suite is the failure mode this ordering exists to prevent, and
# it has happened here.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root" || exit 1

format=1
for arg in "$@"; do
    case "$arg" in
        --no-format) format=0 ;;
        *) echo "usage: $0 [--no-format]" >&2; exit 2 ;;
    esac
done

# The version CI pins, named rather than taken from PATH. A machine carrying both
# 20 and 22 resolves the bare name to whichever comes first, and the preset's own
# `CMAKE_CXX_CLANG_TIDY=clang-tidy` inherits that -- so a "clang-tidy clean" build
# can mean nothing, with the version it used printed nowhere.
tools_version="${CLANG_TOOLS_VERSION:-22}"

fail() { echo "GATE FAILED: $*" >&2; exit 1; }

if [[ "$format" -eq 1 ]]; then
    formatter="clang-format-${tools_version}"
    command -v "$formatter" >/dev/null 2>&1 \
        || fail "$formatter not found; install it or pass --no-format"
    git ls-files '*.h' '*.hpp' '*.cpp' | xargs "$formatter" -i --style=file \
        || fail "clang-format"
    echo "== formatted with $formatter"
fi

run_preset() {
    local preset="$1"
    local log
    log="$(mktemp)"

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

run_preset clang-debug
run_preset gcc-release

echo
echo "LOCAL GATE PASSED (clang-debug + gcc-release)"
