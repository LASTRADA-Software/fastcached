#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Runs the CTest suite under Clang's source-based instrumentation and renders the
# coverage report. Invoked by the `coverage` target that cmake/Coverage.cmake
# defines, which is how CI runs it too -- one code path, so what a developer sees
# locally and what the workflow publishes cannot drift.
#
# Usage (the target passes all of this):
#   scripts/coverage.sh --build-dir DIR --source-dir DIR --llvm-profdata PATH \
#                       --llvm-cov PATH --python3 PATH -- OBJECT [OBJECT...]
#
# Every tool is passed in rather than looked up here, so a missing or
# version-mismatched one is a configure failure (cmake/Coverage.cmake) instead of
# something discovered at the end of a full test run.
#
# Outputs, all under <build-dir>/coverage:
#   html/index.html   the browsable report
#   coverage.lcov     lcov-format export, for Codecov and anything else
#   summary.json      llvm-cov's own per-file summary
#   report.txt        the per-file table, as printed
#   percent.txt       the line-coverage percentage, alone on one line

set -euo pipefail

build_dir=""
source_dir=""
llvm_profdata=""
llvm_cov=""
python3_bin=""
objects=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)      build_dir="$2";      shift 2 ;;
        --source-dir)     source_dir="$2";     shift 2 ;;
        --llvm-profdata)  llvm_profdata="$2";  shift 2 ;;
        --llvm-cov)       llvm_cov="$2";       shift 2 ;;
        --python3)        python3_bin="$2";    shift 2 ;;
        --)               shift; objects=("$@"); break ;;
        *) echo "coverage.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

for required in build_dir source_dir llvm_profdata llvm_cov python3_bin; do
    if [[ -z "${!required}" ]]; then
        # build_dir -> --build-dir, python3_bin -> --python3: the trailing _bin
        # is there only because `python3` would shadow nothing useful as a
        # variable name, so it is stripped before the underscores become dashes.
        flag="${required%_bin}"
        echo "coverage.sh: --${flag//_/-} is required" >&2
        exit 2
    fi
done

if [[ ${#objects[@]} -eq 0 ]]; then
    echo "coverage.sh: at least one object is required after --" >&2
    exit 2
fi

# What is measured, in one place, and expressed as an ALLOW-list rather than a
# list of things to drop.
#
# `src/` is where every first-party source in this repository lives, so handing
# llvm-cov that directory is the whole third-party exclusion: the standard
# library under /usr, the configure-time generated headers under the build tree,
# and CPM's dependency sources are all outside it, wherever they happen to sit.
# That last one is why this is an allow-list. A deny-list naming `/_deps/` looks
# correct locally, where CPM checks out into <build>/_deps -- and silently leaks
# in CI, where CPM_SOURCE_CACHE moves those trees to <workspace>/.cache/CPM
# instead. Catch2's headers instantiate into every instrumented test TU, so the
# figure would have been quietly diluted with third-party template code on
# exactly the runs that publish it, and on none of the runs that check it.
first_party="$source_dir/src"

# Two exclusions remain, because they are inside src/ and are the apparatus
# doing the measuring rather than the code being measured:
#
#   /src/tests/   the shared harness -- test_main, ScratchPath, Unwrap.
#   _test.cpp     and this is the load-bearing one. Tests in this repository live
#                 NEXT TO the implementation, so ~150 *_test.cpp files under
#                 src/FastCache/** and src/apps/** compile straight into the test
#                 binaries. Leave them in and the report measures the tests
#                 testing themselves: thousands of near-100% lines that move the
#                 total a long way and mean nothing whatever.
ignore_regex='(/src/tests/|_test\.cpp$|/test_main\.cpp$)'

coverage_dir="$build_dir/coverage"
raw_dir="$coverage_dir/raw"
profdata="$coverage_dir/coverage.profdata"

rm -rf "$coverage_dir"
mkdir -p "$raw_dir"

# Strays from earlier runs, and from catch_discover_tests: it runs the test
# binary at build time to enumerate cases, with no LLVM_PROFILE_FILE set, so an
# instrumented build drops a default.profraw into the build tree every time it
# links. Merging that in would be harmless but untraceable; deleting it is one
# line.
find "$build_dir" -name '*.profraw' -delete

# %8m rather than %p, and this is the whole reason source-based coverage was
# picked over gcov. The suite is ~2000 Catch2 cases, each its own process
# (catch_discover_tests), plus every daemon and launcher the script-driven tests
# spawn -- %p would leave one multi-megabyte raw profile per process, tens of
# gigabytes of them. %m keys the file on the binary's module signature instead
# and merges into it under a lock, so each binary lands in a pool of at most 8
# files no matter how many processes ran, and two different binaries can never
# collide in the same file.
#
# The path is absolute because the tests chdir, and a relative LLVM_PROFILE_FILE
# would scatter profiles across every scratch directory the suite creates.
#
# The suite's exit status is recorded rather than acted on, and re-raised at the
# very end. A failing suite must not publish a number -- coverage from a red run
# describes a build nobody would ship -- but the report is exactly what someone
# wants to read while working out why it went red, and a `set -e` abort here
# would throw away a profile that took the whole suite to produce.
echo "==> Running the suite under instrumentation"
suite_status=0
LLVM_PROFILE_FILE="$raw_dir/%8m.profraw" \
    ctest \
        --test-dir "$build_dir" \
        --output-on-failure \
        --parallel "${CTEST_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN)}" \
    || suite_status=$?

# Not mapfile: that is bash 4+, and macOS still ships 3.2 as /bin/bash, where
# this would die AFTER the whole instrumented suite had run.
raw_profiles=()
while IFS= read -r profile; do
    raw_profiles+=("$profile")
done < <(find "$raw_dir" -name '*.profraw' | sort)

if [[ ${#raw_profiles[@]} -eq 0 ]]; then
    echo "coverage.sh: the suite ran but wrote no .profraw files -- the binaries are not" >&2
    echo "             instrumented. Configure with the clang-coverage preset." >&2
    exit 1
fi

echo "==> Merging ${#raw_profiles[@]} raw profiles"
"$llvm_profdata" merge -sparse -o "$profdata" "${raw_profiles[@]}"

# llvm-cov wants one binary positionally and every other one behind -object.
extra_objects=()
for object in "${objects[@]:1}"; do
    extra_objects+=(-object "$object")
done

# Built once and reused by all four llvm-cov invocations below, so the object
# list, the profile and the exclusions cannot drift between the HTML a developer
# reads, the lcov CI uploads and the percentage that ends up on the badge.
report_args=(
    "${objects[0]}"
    "${extra_objects[@]}"
    -instr-profile="$profdata"
    -ignore-filename-regex="$ignore_regex"
)

# `show` and `report` take the source filter as a trailing positional argument
# while `export` wants it behind --sources. The same filter either way; only the
# spelling differs, and getting it wrong is silent -- export would just report on
# everything it found.
echo "==> Rendering the report"
"$llvm_cov" show "${report_args[@]}" \
    -format=html \
    -show-line-counts-or-regions \
    -output-dir="$coverage_dir/html" \
    "$first_party"

"$llvm_cov" export "${report_args[@]}" -format=lcov --sources "$first_party" \
    > "$coverage_dir/coverage.lcov"
"$llvm_cov" export "${report_args[@]}" -summary-only --sources "$first_party" \
    > "$coverage_dir/summary.json"

# python3 rather than jq: jq is not installed by default on every developer
# machine this has to run on, python3 effectively is, and the alternative --
# scraping the TOTAL row out of `llvm-cov report` -- is a column layout that has
# changed between LLVM releases before.
"$python3_bin" - "$coverage_dir/summary.json" > "$coverage_dir/percent.txt" <<'PY'
import json
import sys

with open(sys.argv[1]) as handle:
    totals = json.load(handle)["data"][0]["totals"]

print(f'{totals["lines"]["percent"]:.2f}')
PY

echo
# Kept as a file as well as printed, so CI can quote the totals row into its job
# summary without re-running llvm-cov or parsing the JSON a second time.
"$llvm_cov" report "${report_args[@]}" "$first_party" | tee "$coverage_dir/report.txt"
echo
echo "Line coverage: $(cat "$coverage_dir/percent.txt")%"
echo "HTML report:   $coverage_dir/html/index.html"
echo "lcov export:   $coverage_dir/coverage.lcov"

if [[ $suite_status -ne 0 ]]; then
    echo
    echo "coverage.sh: the suite FAILED (ctest exited $suite_status). The report above was" >&2
    echo "             rendered anyway, to debug against -- but it measures a red build," >&2
    echo "             and this exit status is what stops CI publishing the number." >&2
    exit "$suite_status"
fi
