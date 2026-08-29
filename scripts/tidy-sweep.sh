#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Sweep this branch's changed translation units with the PINNED clang-tidy.
#
# This is what the `clang-tidy` CI job runs, and what `scripts/local-gate.sh`
# asks for where the `clang-debug` preset cannot be built. It needs a compile
# database and nothing else -- no objects, no link.
#
# **It verifies the tool actually RUNS before believing a clean result.** That is
# the whole reason this is a script rather than a one-line loop: a wrapper that
# cannot exec, a binary extracted without its execute bit, or one that cannot find
# its own resource headers all produce *silence*, and silence filtered through a
# grep for "error:" reads exactly like success. That mistake has already sent a
# branch to CI twice with findings a local sweep had reported clean.
#
# **What it sweeps is the diff plus everything the diff can break.** A changed
# header is not a translation unit, so tidying only the changed `.cpp` files would
# let an edit to `Logger.hpp` land a finding in fifty files nobody checked. The
# scope is therefore the changed `.cpp` files UNION every `.cpp` that transitively
# includes a changed header -- and any change to a file that decides how *every*
# translation unit is interpreted (`.clang-tidy`, a `CMakeLists.txt`, a `.cmake`
# module, this script) escalates to the full sweep instead. Everything about the
# scope errs towards sweeping MORE: an over-approximation costs minutes, an
# under-approximation costs a red build on master for code a pull request was
# told was clean.
#
# Getting the pinned binary (no root needed; see
# .agent/rules/build-and-toolchain.md):
#
#   pip download "clang-tidy==${CLANG_TOOLS_VERSION}.1.0" -d /tmp/ct --no-deps
#   python3 -m zipfile -e /tmp/ct/clang_tidy-*.whl /tmp/ct22
#   chmod +x /tmp/ct22/clang_tidy/data/bin/clang-tidy    # the wheel loses the bit
#
# Run it from where it was unpacked, or through a wrapper that does: clang-tidy
# resolves its resource headers relative to the binary.
#
# And the database must be a CLANG one with module scanning off -- the `@…modmap`
# flags a module-scanning generator emits do not exist until that target has been
# built, and a translation unit that fails to parse reports nothing:
#
#   cmake -S . -B out/build/tidy22 -G Ninja \
#         -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
#         -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
#         -DFASTCACHED_ENABLE_TLS=ON
#
# Usage:  [TIDY=clang-tidy-22] [DB=out/build/tidy22] [BASE=origin/master] \
#         [JOBS=4] scripts/tidy-sweep.sh [--all|--self-test]
#
#   --all        sweep every translation unit in the database, whatever changed.
#   --self-test  check the scope computation against a synthetic tree and exit.
#                Needs no compile database and no clang-tidy.
set -u

if [[ -z "${BASH_VERSINFO:-}" || "${BASH_VERSINFO[0]}" -lt 4 ]]; then
    echo "TIDY SWEEP FATAL: bash >= 4 is required (associative arrays)" >&2
    exit 2
fi

TIDY="${TIDY:-clang-tidy-${CLANG_TOOLS_VERSION:-22}}"
DB="${DB:-out/build/tidy22}"
BASE="${BASE:-origin/master}"
JOBS="${JOBS:-$( { nproc 2>/dev/null || echo 4; } )}"

fatal() { echo "TIDY SWEEP FATAL: $*" >&2; exit 2; }

# ---------------------------------------------------------------------------
# Scope
# ---------------------------------------------------------------------------

# A change to any of these decides how EVERY translation unit is interpreted, so
# a diff-scoped sweep would be answering the wrong question. One row per reason,
# matched as a bash pattern against the repo-relative path (`*` matches `/`
# here, which is why `*.cmake` covers `cmake/portable/` too).
#
# Deliberately NOT here: documentation, `.agent/`, `packaging/` assets and the
# sources themselves. A README typo must not cost a full sweep.
SweepEverythingWhen=(
    ".clang-tidy"                  # the check list and every check's options
    ".clang-format"                # a reformat can move a finding's line
    "CMakePresets.json"            # the flags the database is generated from
    "*CMakeLists.txt"              # ditto
    "*.cmake"                      # ditto
    "*.hpp.in"                     # generates a header the include graph cannot see
    "scripts/tidy-sweep.sh"        # this file: prove the new scope logic works
    ".github/workflows/build.yml"  # the job that runs it
)

# True when a changed path forces the full sweep.
# @param 1 Repo-relative path.
ForcesFullSweep() {
    local path="$1" pattern
    for pattern in "${SweepEverythingWhen[@]}"; do
        # shellcheck disable=SC2053  # the right-hand side is a pattern on purpose
        [[ "$path" == $pattern ]] && return 0
    done
    return 1
}

# includers[<repo path>] is the space-separated list of first-party files that
# `#include` it. Populated by BuildIncludeGraph, read by AffectedTranslationUnits.
declare -A includers=()

# Build the reverse include graph over a set of first-party files.
#
# An include spelling is resolved by longest path SUFFIX, so `<FastCache/Core/
# Logger.hpp>`, `"Core/Logger.hpp"` and `"Logger.hpp"` all reach
# `src/FastCache/Core/Logger.hpp` without this script having to know a single
# include directory. An ambiguous suffix resolves to every candidate, and a
# spelling that resolves to nothing is a system header and is dropped -- both
# failure directions land on "sweep more", which is the safe one.
#
# @param @ The first-party files to scan.
BuildIncludeGraph() {
    local -a files=("$@")
    [[ "${#files[@]}" -gt 0 ]] || return 0

    local -A suffixOwners=()
    local path suffix rest
    for path in "${files[@]}"; do
        suffix="$path"
        while true; do
            suffixOwners["$suffix"]="${suffixOwners["$suffix"]:-} $path"
            rest="${suffix#*/}"
            [[ "$rest" == "$suffix" ]] && break
            suffix="$rest"
        done
    done

    # One grep over the whole tree rather than one per file: 1400 process spawns
    # is the difference between a second and a minute, and on Windows a lot more.
    local line file spelling owner
    while IFS= read -r line; do
        file="${line%%:*}"
        spelling="${line#*:}"
        spelling="${spelling#*[<\"]}"
        spelling="${spelling%[>\"]}"
        for owner in ${suffixOwners["$spelling"]:-}; do
            [[ "$owner" == "$file" ]] && continue
            includers["$owner"]="${includers["$owner"]:-} $file"
        done
    done < <(grep -HoE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^">]+[">]' \
                  -- "${files[@]}" 2>/dev/null)
}

# Every `.cpp` reachable from a set of changed files through the include graph,
# the changed `.cpp` files themselves included. Printed one per line.
#
# @param @ The changed first-party files.
AffectedTranslationUnits() {
    local -A seen=()
    local -a queue=("$@")
    local current includer
    while [[ "${#queue[@]}" -gt 0 ]]; do
        current="${queue[0]}"
        queue=("${queue[@]:1}")
        [[ -n "${seen["$current"]:-}" ]] && continue
        seen["$current"]=1
        for includer in ${includers["$current"]:-}; do
            [[ -z "${seen["$includer"]:-}" ]] && queue+=("$includer")
        done
    done
    local path
    for path in "${!seen[@]}"; do
        [[ "$path" == *.cpp ]] && printf '%s\n' "$path"
    done | sort
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

# The scope computation is the part of this script that can fail SILENTLY -- it
# has no output of its own to be wrong in, it just sweeps too few files and
# prints a confident count. So it gets asserted, on a synthetic tree, with no
# compile database and no clang-tidy needed. The CI job runs this before the
# sweep for the same reason the sweep canaries its binary.
SelfTest() {
    local status=0
    local scratch
    scratch="$(mktemp -d)" || fatal "cannot create a scratch directory"
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    mkdir -p "$scratch/src/Deep" "$scratch/src/apps"
    printf '#pragma once\n'                                  > "$scratch/src/Deep/Base.hpp"
    printf '#pragma once\n#include <Deep/Base.hpp>\n'         > "$scratch/src/Deep/Middle.hpp"
    printf '#include "Deep/Middle.hpp"\n#include <string>\n'  > "$scratch/src/Deep/User.cpp"
    printf '#include <Deep/Base.hpp>\n'                       > "$scratch/src/apps/Direct.cpp"
    printf '#include <string>\n'                              > "$scratch/src/apps/Unrelated.cpp"

    local here="$PWD"
    cd "$scratch" || fatal "cannot enter the scratch directory"
    BuildIncludeGraph src/Deep/Base.hpp src/Deep/Middle.hpp \
                      src/Deep/User.cpp src/apps/Direct.cpp src/apps/Unrelated.cpp
    cd "$here" || fatal "cannot return from the scratch directory"

    Expect() {
        local what="$1" want="$2" got="$3"
        if [[ "$want" == "$got" ]]; then
            echo "  ok   ${what}"
        else
            echo "  FAIL ${what}: want [${want}] got [${got}]"
            status=1
        fi
    }

    echo "TIDY SWEEP SELF-TEST"
    # A header two levels down still reaches the translation unit at the top.
    Expect "a transitively included header reaches its TU" \
           "src/Deep/User.cpp"$'\n'"src/apps/Direct.cpp" \
           "$(AffectedTranslationUnits src/Deep/Base.hpp)"
    # And only that one -- an unrelated TU is not swept.
    Expect "an unrelated TU is not swept" \
           "src/Deep/User.cpp" \
           "$(AffectedTranslationUnits src/Deep/Middle.hpp)"
    # A changed .cpp is its own translation unit.
    Expect "a changed TU sweeps itself" \
           "src/apps/Unrelated.cpp" \
           "$(AffectedTranslationUnits src/apps/Unrelated.cpp)"

    ExpectForce() {
        local path="$1" want="$2" got=no
        ForcesFullSweep "$path" && got=yes
        Expect "escalation: ${path}" "$want" "$got"
    }
    ExpectForce ".clang-tidy" yes
    ExpectForce "cmake/portable/CompileCache.cmake" yes
    ExpectForce "src/FastCache/CMakeLists.txt" yes
    ExpectForce "scripts/tidy-sweep.sh" yes
    ExpectForce "src/FastCache/Core/Version.hpp.in" yes
    ExpectForce "src/FastCache/Core/Logger.hpp" no
    ExpectForce "src/FastCache/Core/Logger.cpp" no
    ExpectForce "README.md" no
    ExpectForce ".agent/rules/testing.md" no

    [[ "$status" -eq 0 ]] && echo "TIDY SWEEP SELF-TEST PASSED"
    return "$status"
}

# ---------------------------------------------------------------------------

mode=diff
for arg in "$@"; do
    case "$arg" in
        --all) mode=all ;;
        --self-test) mode=selftest ;;
        *) fatal "usage: $0 [--all|--self-test]" ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" || fatal "cannot locate the repository"
cd "$repo_root" || fatal "cannot enter ${repo_root}"

if [[ "$mode" == selftest ]]; then
    SelfTest
    exit $?
fi

command -v "$TIDY" >/dev/null 2>&1 || fatal "$TIDY is not on PATH"
"$TIDY" --version >/dev/null 2>&1 || fatal "$TIDY will not run"
[[ -f "${DB}/compile_commands.json" ]] || fatal "no compile database at ${DB}"

PYTHON="$(command -v python3 || command -v python)" \
    || fatal "python3 is needed to read the compile database"

scratch="$(mktemp -d)" || fatal "cannot create a scratch directory"
# shellcheck disable=SC2064  # expand $scratch now, not at trap time
trap "rm -rf '$scratch'" EXIT

# The unit of work is a COMPILE COMMAND, not a file, and that is what keeps this
# equivalent to the build-driven tidy it replaced. A source compiled into two
# targets is two translation units with two preprocessor states -- `Stats.cpp`
# builds into `fastcache-cc`, `fastcache-cc-tests` and `fastcache-compile-node`,
# and those targets do not agree about `FC_COMPRESSION_ENABLED` -- so one
# invocation per file would silently stop checking the other configurations.
# `clang-tidy -p <dir> <file>` always takes the FIRST matching entry, so the
# second and later commands for a file each get a one-entry database of their own.
#
# Reading the database is also the only honest answer to what this platform
# compiles: `IocpReactor.cpp` is a translation unit on Windows and a file on
# Linux, so a list taken from `git ls-files` would fail a full sweep on sources no
# target here builds. Third-party sources under `_deps/` are somebody else's.
cat > "${scratch}/plan.py" <<'PLAN'
"""Emit one `<database dir>\t<file>` line per distinct compile command."""
import json, os, sys

db_path, root, outdir, selection_path = sys.argv[1:5]
selection = None
if selection_path != "--all":
    with open(selection_path, encoding="utf-8") as handle:
        selection = {line.strip() for line in handle if line.strip()}

with open(db_path, encoding="utf-8") as handle:
    entries = json.load(handle)

prefix = root.replace("\\", "/").rstrip("/") + "/"
byFile = {}
order = []
for entry in entries:
    path = entry.get("file", "").replace("\\", "/")
    if path.startswith(prefix):
        path = path[len(prefix):]
    if path.startswith("/") or "/_deps/" in path or ":" in path.split("/")[0]:
        continue
    if selection is not None and path not in selection:
        continue
    command = entry.get("command") or " ".join(entry.get("arguments", []))
    if path not in byFile:
        byFile[path] = {}
        order.append(path)
    byFile[path].setdefault(command, entry)

if selection is not None:
    missing = sorted(selection - set(order))
    for path in missing:
        print("not a translation unit here: " + path, file=sys.stderr)

slot = 0
for path in order:
    for index, entry in enumerate(byFile[path].values()):
        if index == 0:
            print(os.path.dirname(db_path) + "\t" + path)
            continue
        slot += 1
        directory = os.path.join(outdir, str(slot))
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(directory, "compile_commands.json"), "w",
                  encoding="utf-8") as handle:
            json.dump([entry], handle)
        print(directory + "\t" + path)
PLAN

# @param 1 A file of repo-relative paths to keep, or "--all" for every unit.
# Prints the plan; the caller checks that it is not empty.
PlanUnits() {
    "$PYTHON" "${scratch}/plan.py" "${DB}/compile_commands.json" \
              "$repo_root" "${scratch}/db" "$1"
}

# A canary against a real file. Anything that stops clang-tidy from parsing shows
# up here rather than as an entire branch reported clean.
probe_file="$(git ls-files 'src/FastCache/Core/*.cpp' | head -1)"
[[ -n "$probe_file" ]] || fatal "no probe file to canary against"
probe="$("$TIDY" -p "$DB" --quiet "$probe_file" 2>&1)"
probe_rc=$?
[[ "$probe_rc" -ge 126 ]] && fatal "$TIDY could not be executed (exit ${probe_rc})"
case "$probe" in
    *"Permission denied"*|*"error: no such file or directory: '@"*|*"'stddef.h' file not found"*)
        fatal "$TIDY is not parsing: ${probe}" ;;
esac

selection="--all"
if [[ "$mode" != all ]]; then
    # Committed changes, everything different from HEAD, and files not yet added.
    #
    # The last two are not a nicety, and the middle one is easy to get wrong: a new
    # source file is untracked until `git add` and then *staged*, at which point it is
    # invisible to both `git diff` (worktree against index -- no difference) and
    # `git ls-files --others` (no longer "other"). `git diff HEAD` is what sees it,
    # because it compares against the commit rather than the index. Get this wrong and
    # the sweep skips exactly the code nothing has ever checked, and prints a confident
    # file count while doing it.
    #
    # A base that does not resolve escalates to the full sweep rather than to an
    # empty diff: "we could not tell what changed" must never read as "nothing did".
    #
    # `--exclude-standard` is what keeps a derived tree out of this. CI's CPM cache
    # lives at `.cache/CPM` inside the workspace and is only invisible here because
    # `.gitignore` says `.cache/`; a build or dependency tree that is NOT ignored
    # puts its own `.cmake` files in front of the escalation table and every sweep
    # becomes a full one. That is the safe direction — slow, never wrong — but it
    # is also why an unignored build directory is worth noticing.
    if ! git rev-parse --verify --quiet "${BASE}^{commit}" >/dev/null; then
        echo "TIDY SWEEP: ${BASE} does not resolve; sweeping everything"
        mode=all
    fi
fi

if [[ "$mode" != all ]]; then
    mapfile -t changed < <({ git diff --name-only "${BASE}...HEAD"
                             git diff --name-only HEAD
                             git ls-files --others --exclude-standard; } | sort -u)
    for path in "${changed[@]}"; do
        if ForcesFullSweep "$path"; then
            echo "TIDY SWEEP: ${path} changed; sweeping everything"
            mode=all
            break
        fi
    done
fi

if [[ "$mode" != all ]]; then
    declare -a sources=() touched=()
    mapfile -t sources < <(git ls-files '*.c' '*.h' '*.hpp' '*.cpp' '*.ipp' '*.hxx' '*.inl')
    for path in "${changed[@]}"; do
        case "$path" in
            *.c|*.h|*.hpp|*.cpp|*.ipp|*.hxx|*.inl) touched+=("$path") ;;
        esac
    done
    if [[ "${#touched[@]}" -eq 0 ]]; then
        echo "TIDY SWEEP: no source changed against ${BASE}"
        exit 0
    fi
    BuildIncludeGraph "${sources[@]}"
    AffectedTranslationUnits "${touched[@]}" > "${scratch}/selection"
    selection="${scratch}/selection"
    echo "TIDY SWEEP: ${#touched[@]} changed source(s) reach $(wc -l < "$selection") candidate file(s)"
fi

mapfile -t plan < <(PlanUnits "$selection")
if [[ "${#plan[@]}" -eq 0 ]]; then
    if [[ "$mode" == all ]]; then
        fatal "no first-party translation units in ${DB}/compile_commands.json"
    fi
    echo "TIDY SWEEP: nothing changed here reaches a translation unit this platform"
    echo "            compiles; nothing to sweep"
    exit 0
fi
echo "TIDY SWEEP: ${#plan[@]} translation unit(s), ${TIDY}, ${JOBS} at a time"

# One translation unit, into numbered files so the report below is in a stable
# order however the pool interleaves. A refusal to EXECUTE is recorded apart from
# a finding: every way of getting clang-tidy wrong produces silence, and silence
# must not be summed into a clean verdict.
# @param 1 The database directory holding this unit's compile command.
# @param 2 The file to check.
# @param 3 Slot prefix under $scratch.
TidyOne() {
    local database="$1" file="$2" slot="$3" out rc hits
    out="$("$TIDY" -p "$database" --quiet "$file" 2>&1)"
    rc=$?
    if [[ "$rc" -ge 126 ]]; then
        printf '%s (exit %s)\n' "$file" "$rc" > "${slot}.fatal"
        return
    fi
    # Unknown *warning options* are the GCC-only flags a clang build has no use
    # for; everything else is a finding.
    hits="$(printf '%s\n' "$out" | grep -E 'error:|warning:' | grep -v 'unknown-warning-option')"
    [[ -n "$hits" ]] && { printf '=== %s\n%s\n' "$file" "$hits" > "${slot}.out"; }
    return 0
}

index=0
for unit in "${plan[@]}"; do
    database="${unit%%$'\t'*}"
    file="${unit#*$'\t'}"
    [[ -f "$file" ]] || continue
    index=$((index + 1))
    TidyOne "$database" "$file" "$(printf '%s/%05d' "$scratch" "$index")" &
    while [[ "$(jobs -rp | wc -l)" -ge "$JOBS" ]]; do wait -n; done
done
wait

shopt -s nullglob
fatals=("$scratch"/*.fatal)
[[ "${#fatals[@]}" -gt 0 ]] && fatal "$TIDY failed to run on $(cat "${fatals[@]}")"

status=0
for report in "$scratch"/*.out; do
    cat "$report"
    status=1
done

[[ "$status" -eq 0 ]] && echo "TIDY SWEEP CLEAN (${index} translation unit(s), ${TIDY})"
exit "$status"
