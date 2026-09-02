#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Sweep this branch's changed translation units with the PINNED clang-tidy.
#
# This is what the `clang-tidy` CI job runs. `scripts/local-gate.sh` gets the
# same checks the other way, through the `clang-debug` preset's build; this is
# for where that preset cannot be built, and for reproducing what CI will say.
# It needs a compile database and nothing else -- no objects, no link.
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

# 4.4 rather than 4.0, and each digit is load-bearing: associative arrays are 4.0,
# `wait -n` is 4.3, and expanding an EMPTY array as `"${a[@]}"` under `set -u` stops
# being an "unbound variable" error at 4.4 -- which this script does on any tree
# with nothing changed. macOS ships 3.2; `brew install bash` is the answer there.
#
# What an interpreter below the floor MEANS depends on what was asked, and the two
# answers are not the same (#588). A sweep genuinely cannot run here, so it refuses.
# The self-test is registered in the DEFAULT ctest set, which runs on every platform
# CI builds -- so on a stock macOS runner it has to produce a visible SKIPPED, and
# the two ways of getting that wrong are symmetric: registering it naively turns a
# silent non-registration into a red macOS leg, and "fixing" that by dropping it on
# macOS at configure time puts it back to a check that does not exist and does not
# say so. `SKIP_RETURN_CODE 77` -- the idiom `fetch-transfer-bound` already uses --
# is how ctest is told which of the four states this is.
#
# A configure-time guard could not answer this anyway: the floor is a property of
# whichever `bash` the machine has at RUN time, and a macOS runner can carry 3.2 at
# /bin/bash and 5.x from Homebrew. Only the interpreter actually running can say.
#
# Asked HERE rather than after the argument loop far below, because it must be
# settled before anything 4.4-only executes. `${1+"$@"}` and not `"$@"`, for the
# very reason 4.4 is the floor: on 3.2 the bare form is an unbound-variable error
# when there are no arguments, so this guard would die instead of reporting.
# `--self-test` AND NOTHING ELSE. The canonical parse is far below, after code this
# interpreter may not be able to run, so this is a second place the command line is
# read -- and two readings that can disagree are worse than one that is coarse.
# `--self-test --all` is a full sweep on a modern bash (the real loop is last-wins),
# so answering "skipped self-test" for it on 3.2 would have the two interpreters
# reporting different outcomes for one command line, in the green-reading direction.
# Anything else falls through to the sweep's refusal, which is what a modern bash
# does with it too.
selfTestRequested=0
if [[ "$#" -eq 1 ]] && [[ "${1:-}" == "--self-test" ]]; then
    selfTestRequested=1
fi

# The decision, as a pure function of the three things it depends on.
#
# Split out because it is otherwise untestable BY CONSTRUCTION: `BASH_VERSINFO` is
# read-only, so the skip path cannot be reached on any machine new enough to run
# the rest of this script -- and every machine this is developed on is one. A guard
# whose interesting branch only executes where nobody can run it is the shape of
# defect the self-test exists to catch, one level down.
#
# Written to bash 3.2 deliberately: it runs BEFORE the floor check that everything
# below it relies on, so it may use nothing the floor is there to guarantee.
#
# @param 1 Major version, or empty when the interpreter is not bash at all.
# @param 2 Minor version.
# @param 3 1 when `--self-test` was among the arguments, else 0.
# @return Prints `ok`, `skip` or `fatal`.
InterpreterVerdict() {
    local major="$1" minor="$2" wantsSelfTest="$3"
    if [[ -n "$major" ]]; then
        if [[ "$major" -gt 4 ]]; then echo ok; return; fi
        if [[ "$major" -eq 4 ]] && [[ "$minor" -ge 4 ]]; then echo ok; return; fi
    fi
    # Below the floor. A sweep cannot run; a self-test has an answer, and that
    # answer is SKIPPED rather than passed or failed.
    if [[ "$wantsSelfTest" -eq 1 ]]; then echo skip; return; fi
    echo fatal
}

# Named once. "bash >= 4.4 is required" without saying what is actually HERE sends
# somebody to check a version the message never showed them.
interpreterIs="${BASH:-bash} is ${BASH_VERSION:-not bash}"

case "$(InterpreterVerdict "${BASH_VERSINFO[0]:-}" "${BASH_VERSINFO[1]:-0}" "$selfTestRequested")" in
    ok) ;;
    skip)
        echo "TIDY SWEEP SELF-TEST SKIPPED: ${interpreterIs}, and this needs bash >= 4.4." >&2
        echo "  A SKIP is not a pass: the scope computation was NOT checked on this machine." >&2
        echo "  Install a newer bash to run it here (on macOS: brew install bash)." >&2
        exit 77
        ;;
    fatal)
        echo "TIDY SWEEP FATAL: ${interpreterIs}, and bash >= 4.4 is required" >&2
        exit 2
        ;;
    *)
        # A verdict this case does not know renders as UNRECOGNISED rather than as
        # the nearest plausible neighbour. Folding it into the `fatal` arm would be
        # cheaper and would report a bash-version problem for what is actually a bug
        # in the function above -- sending somebody to install a shell they already
        # have. Same rule as `leg_summary`'s default arm in `local-gate.sh`, and for
        # the same reason: a fourth state must not arrive silently as a third.
        echo "TIDY SWEEP FATAL: InterpreterVerdict returned an unrecognised verdict; this is a bug in this script, not a problem with ${interpreterIs}" >&2
        exit 2
        ;;
esac

TIDY="${TIDY:-clang-tidy-${CLANG_TOOLS_VERSION:-22}}"
DB="${DB:-out/build/tidy22}"
BASE="${BASE:-origin/master}"
# getconf rather than nproc alone: this runs on macOS too, where nproc does not
# exist and a bare fallback would quietly use four cores of twelve.
JOBS="${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

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
    "*.clang-tidy"                 # the check list and every check's options
    "*.clang-format"               # a reformat can move a finding's line
    "CMakePresets.json"            # the flags the database is generated from
    "*CMakeLists.txt"              # ditto
    "*.cmake"                      # ditto
    "vcpkg.json"                   # the third-party headers every TU parses
    "*.hpp.in"                     # generates a header the include graph cannot see
    "scripts/tidy-sweep.sh"        # this file: prove the new scope logic works
    ".github/workflows/build.yml"  # the job that runs it
)

# What counts as a first-party source, spelled once. Written twice -- as
# `git ls-files` globs and as a `case` pattern -- it is a two-site edit for a
# one-fact change, in a script whose whole thesis is that a wrong scope is
# invisible.
SourceExtensions=(c cc cxx h hh hpp cpp ipp hxx inl)

# Which of those are TRANSLATION UNITS, and therefore what the sweep can hand to
# clang-tidy. Spelled as its own table rather than as a `*.cpp` test buried in
# AffectedTranslationUnits: `c` is on the list above, so a `.c` file added
# tomorrow would otherwise be walked into the graph and then dropped on the way
# out -- unswept, with a clean verdict printed over it. The compile database has
# the final say either way; this only decides what is offered to it.
TranslationUnitExtensions=(c cc cxx cpp)

# True when a path is a translation unit this sweep can hand to clang-tidy.
# @param 1 Repo-relative path.
IsTranslationUnit() {
    local path="$1" extension
    for extension in "${TranslationUnitExtensions[@]}"; do
        [[ "$path" == *".${extension}" ]] && return 0
    done
    return 1
}

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
    #
    # Into a FILE rather than through a process substitution, and the exit status
    # is checked, because that status is the only thing standing between a grep
    # that could not run -- an argument list past this host's ARG_MAX, a file it
    # cannot read -- and an EMPTY include graph. An empty graph is not an error
    # anywhere downstream: it silently narrows the sweep to the changed `.cpp`
    # files, prints a confident count, and reports clean. grep says 1 for "no
    # line matched" and >= 2 for "I failed", and only the second is fatal.
    local scan rc
    scan="$(mktemp)" || fatal "cannot create a scratch file for the include scan"
    grep -HoE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^">]+[">]' \
         -- "${files[@]}" > "$scan"
    rc=$?
    if [[ "$rc" -gt 1 ]]; then
        rm -f "$scan"
        fatal "the include scan failed (grep exit ${rc}); the sweep's scope cannot be trusted"
    fi

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
    done < "$scan"
    rm -f "$scan"
}

# Every `.cpp` reachable from a set of changed files through the include graph,
# the changed `.cpp` files themselves included. Printed one per line.
#
# @param @ The changed first-party files.
AffectedTranslationUnits() {
    local -A seen=()
    local -a queue=("$@")
    local current includer head=0
    # A cursor rather than `queue=("${queue[@]:1}")`, which rebuilds the whole
    # array on every pop and is quadratic over a tree this size.
    while [[ "$head" -lt "${#queue[@]}" ]]; do
        current="${queue[head]}"
        head=$((head + 1))
        [[ -n "${seen["$current"]:-}" ]] && continue
        seen["$current"]=1
        for includer in ${includers["$current"]:-}; do
            [[ -z "${seen["$includer"]:-}" ]] && queue+=("$includer")
        done
    done
    # `LC_ALL=C` because `sort` is COLLATED, not byte-ordered: under `en_US.UTF-8`
    # it ignores case and punctuation at the primary level and puts
    # `src/apps/Direct.cpp` before `src/Deep/User.cpp`, while a CI runner's
    # `C.UTF-8` does the opposite. The sweep does not care about the order, but
    # the self-test below compares against a literal, so a host locale must not
    # be able to decide whether it passes.
    local path
    for path in "${!seen[@]}"; do
        IsTranslationUnit "$path" && printf '%s\n' "$path"
    done | LC_ALL=C sort
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

# The scope computation is the part of this script that can fail SILENTLY -- it
# has no output of its own to be wrong in, it just sweeps too few files and
# prints a confident count. So it gets asserted, on a synthetic tree, with no
# compile database and no clang-tidy needed. The CI job runs this before the
# sweep for the same reason the sweep canaries its binary.
# Did this translation unit contain any code FROM THE FILE ITSELF? (#466)
#
# PURE: reads preprocessed output on stdin, prints `produced` or `empty`, and
# touches nothing else. That split is the whole point. The defect being fixed is
# that a file compiling to an EMPTY translation unit reads as COVERED -- it has a
# compile command, so it never reaches the "not a translation unit here" drop path,
# and five independent signals reported success over nothing: the database listed
# it, clang-tidy processed it and found nothing, gcc built it, clang built it, and
# ctest ran a binary containing zero of its cases. It held a syntax error
# throughout, and only CI's macOS leg could see it.
#
# `not compiled` and `compiled to nothing` look identical from outside and are not
# the same claim. The second is worse, because it produces POSITIVE evidence.
#
# Because the classification is pure, `--self-test` drives every verdict from
# synthesised linemarker text with no compiler, no database and no clang-tidy. Only
# the preprocessor run in `UnitContribution` is impure, and that half decides
# nothing.
#
# Suffix match on a "/" boundary rather than equality: a linemarker spells the path
# the way the compile command did, which is usually absolute, while the plan
# carries a repo-relative one. A bare `==` would answer `empty` for every unit in
# the tree and look like a spectacular finding.
#
# @param 1 The path lines must be attributed to.
ProducedCode() {
    awk -v want="$1" '
    function isTarget(p,   n) {
        if (p == want) return 1
        n = length(p) - length(want)
        if (n > 0 && substr(p, n + 1) == want && substr(p, n, 1) == "/") return 1
        return 0
    }
    /^# / { if (match($0, /"[^"]*"/)) cur = substr($0, RSTART + 1, RLENGTH - 2); next }
    NF > 0 && isTarget(cur) { found = 1; exit }
    END { print (found ? "produced" : "empty") }
    '
}

# The compile command for one unit as a preprocess-only argv: the directory on the
# first line, then one word per line.
#
# `-o` with its argument and `-c` are dropped and `-E` appended -- the same command
# the build runs, asked to stop after preprocessing. Prints nothing when the file is
# not in the database, which the caller reads as `unknown`: "we could not tell" must
# never render as a verdict.
#
# @param 1 Directory holding compile_commands.json.
# @param 2 Repo-relative path of the unit.
PreprocessArgv() {
    python3 - "$1/compile_commands.json" "$2" <<'PYARGV'
import json, shlex, sys
sys.stdout.reconfigure(newline="\n")
try:
    entries = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception:
    sys.exit(1)
target = sys.argv[2].replace("\\", "/")
for entry in entries:
    path = entry.get("file", "").replace("\\", "/")
    if path == target or path.endswith("/" + target):
        argv = shlex.split(entry.get("command") or "") or list(entry.get("arguments", []))
        out, skip = [], False
        for a in argv:
            if skip:
                skip = False
                continue
            if a == "-o":
                skip = True
                continue
            if a == "-c":
                continue
            out.append(a)
        out.append("-E")
        print(entry.get("directory", "."))
        for a in out:
            print(a)
        break
PYARGV
}

# `produced`, `empty` or `unknown` for one planned unit.
#
# `unknown` is a fourth state and is reported as one. A preprocessor that fails, or
# a unit the database does not describe, has told us NOTHING about whether the file
# contributes: folding that into `empty` invents a finding, folding it into
# `produced` hides one.
#
# @param 1 Directory holding compile_commands.json.
# @param 2 Repo-relative path of the unit.
UnitContribution() {
    local words directory out line
    words="$(PreprocessArgv "$1" "$2" 2>/dev/null)" || { echo unknown; return; }
    [[ -n "$words" ]] || { echo unknown; return; }
    directory="$(printf '%s\n' "$words" | head -1)"
    local -a argv=()
    while IFS= read -r line; do
        argv+=("$line")
    done < <(printf '%s\n' "$words" | tail -n +2)
    [[ "${#argv[@]}" -gt 0 ]] || { echo unknown; return; }

    # Through a FILE so the preprocessor's own exit status is observed. A pipeline
    # hands the caller `ProducedCode`'s status, which is always 0 -- so a compiler
    # that failed produced no output, `ProducedCode` correctly said `empty` of an
    # empty stream, and the unit was reported as contributing nothing. That is this
    # ticket's own defect rebuilt inside its fix: an absence of evidence rendered as
    # evidence of absence. Found by mutation testing, which showed nothing could
    # tell `unknown` from `empty` because nothing ever produced `unknown`.
    local preprocessed
    preprocessed="$(mktemp)" || { echo unknown; return; }
    if ! ( cd "$directory" && "${argv[@]}" ) > "$preprocessed" 2>/dev/null; then
        rm -f "$preprocessed"
        echo unknown
        return
    fi
    out="$(ProducedCode "$2" < "$preprocessed")"
    rm -f "$preprocessed"
    [[ -n "$out" ]] || { echo unknown; return; }
    printf '%s\n' "$out"
}

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
    printf '#include "Deep/Base.hpp"\n'                       > "$scratch/src/apps/Legacy.c"

    local here="$PWD"
    cd "$scratch" || fatal "cannot enter the scratch directory"
    BuildIncludeGraph src/Deep/Base.hpp src/Deep/Middle.hpp src/Deep/User.cpp \
                      src/apps/Direct.cpp src/apps/Unrelated.cpp src/apps/Legacy.c
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
    # A header two levels down still reaches the translation unit at the top --
    # and a `.c` unit is one of them, which is the assertion that stops the
    # translation-unit table from silently drifting back to "only `.cpp`".
    Expect "a transitively included header reaches its TU" \
           "src/Deep/User.cpp"$'\n'"src/apps/Direct.cpp"$'\n'"src/apps/Legacy.c" \
           "$(AffectedTranslationUnits src/Deep/Base.hpp)"
    # And only that one -- an unrelated TU is not swept.
    Expect "an unrelated TU is not swept" \
           "src/Deep/User.cpp" \
           "$(AffectedTranslationUnits src/Deep/Middle.hpp)"
    # A changed .cpp is its own translation unit.
    Expect "a changed TU sweeps itself" \
           "src/apps/Unrelated.cpp" \
           "$(AffectedTranslationUnits src/apps/Unrelated.cpp)"
    # A header is never offered to clang-tidy as a unit of its own.
    Expect "a changed header is not itself a unit" \
           "0" \
           "$(AffectedTranslationUnits src/Deep/Base.hpp | grep -c '\.hpp$' || true)"

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
    ExpectForce "vcpkg.json" yes
    ExpectForce "src/FastCache/Core/Logger.hpp" no
    ExpectForce "src/FastCache/Core/Logger.cpp" no
    ExpectForce "README.md" no
    ExpectForce ".agent/rules/testing.md" no

    # Non-vacuity, asserted rather than inferred (#588).
    #
    # FIVE rows above survive an empty input, not one, and the count is measured
    # rather than reasoned: `a changed TU sweeps itself` needs no graph at all (a
    # .cpp is its own unit), and the four `ExpectForce ... no` rows all pass over an
    # EMPTY escalation table because "no" is what an empty table answers. What keeps
    # clause 3 satisfied is that each of them is paired with a positive row that does
    # NOT survive -- the six `yes` rows guard the table, the two reach rows guard the
    # graph.
    #
    # The `.hpp` row was the one with no pair: its expected value IS zero, so it
    # passed perfectly over a graph that had never been built. That is the
    # `node-config-reference` scar -- two empty answers agreeing -- and this
    # assertion is its pair. A new row whose expected value is an absence needs one
    # too.
    Expect "the synthetic include graph was actually built" \
           "yes" \
           "$( [[ "${#includers[@]}" -gt 0 ]] && echo yes || echo no )"

    # Whether a unit produced any code OF ITS OWN (#466). Pure, so every verdict is
    # reachable here from synthesised linemarker text -- no compiler, no database,
    # no clang-tidy. `KqueueSocket_test.cpp` is the real instance: on Linux it
    # preprocesses to 95,527 lines of which ZERO are its own.
    #
    # The first two are the whole distinction. A file that contributes nothing is
    # not evidence about that file, and it looks identical from outside to one that
    # was never compiled -- except that it produces POSITIVE evidence, which is
    # worse.
    Expect "a unit whose own lines survive the preprocessor produced code" \
           "produced" \
           "$(printf '# 1 "src/A.cpp"\nint a;\n' | ProducedCode src/A.cpp)"
    Expect "a unit guarded out to nothing is empty" \
           "empty" \
           "$(printf '# 1 "src/A.cpp"\n# 1 "/usr/include/stdio.h"\nint fromHeader;\n' | ProducedCode src/A.cpp)"

    # The failure that would look like a spectacular finding rather than a bug: a
    # linemarker names the path as the COMPILE COMMAND spelled it, usually
    # absolutely, while the plan carries a repo-relative one. Equality would answer
    # `empty` for every unit in the tree.
    Expect "an absolute linemarker still matches a repo-relative unit" \
           "produced" \
           "$(printf '# 1 "/build/src/A.cpp"\nint a;\n' | ProducedCode src/A.cpp)"
    # And the boundary that stops the suffix match being too eager.
    Expect "a path merely ENDING in the name is not the unit" \
           "empty" \
           "$(printf '# 1 "/build/other/NotA.cpp"\nint a;\n' | ProducedCode A.cpp)"

    # Blank and whitespace-only lines are not code. A preprocessor emits runs of
    # them where the guarded block was, so counting them would call every empty unit
    # `produced` and silently restore the defect.
    Expect "blank lines attributed to the file are not code" \
           "empty" \
           "$(printf '# 1 "src/A.cpp"\n\n   \n\n' | ProducedCode src/A.cpp)"
    # Directives are not code either: an empty TU still carries its own `# 1` line.
    Expect "the file's own linemarker alone is not code" \
           "empty" \
           "$(printf '# 1 "src/A.cpp"\n' | ProducedCode src/A.cpp)"
    # Returning to the file after a header is the ordinary shape of a real unit.
    Expect "code after returning from a header still counts" \
           "produced" \
           "$(printf '# 1 "src/A.cpp"\n# 1 "/usr/include/x.h"\nint h;\n# 2 "src/A.cpp"\nint a;\n' | ProducedCode src/A.cpp)"
    # Nothing at all is not evidence that the file is empty, but it is what an empty
    # stream says; `unknown` is decided by the caller, which can tell a failed
    # preprocessor from a silent one.
    Expect "an empty stream reads as empty, never as produced" \
           "empty" \
           "$(printf '' | ProducedCode src/A.cpp)"

    # The interpreter verdict (#588). Pinned on BOTH sides of the floor, because a
    # bound demonstrated once from the middle is where an off-by-one lives.
    #
    # These are the only way the skip path is ever exercised: it cannot run on a
    # machine new enough to reach this function, and `BASH_VERSINFO` is read-only
    # so it cannot be faked from outside.
    Expect "a modern bash runs the sweep"            "ok"    "$(InterpreterVerdict 5 2 0)"
    Expect "4.4 is at the floor and runs"            "ok"    "$(InterpreterVerdict 4 4 0)"
    Expect "4.3 is below the floor"                  "fatal" "$(InterpreterVerdict 4 3 0)"
    Expect "3.2 refuses a sweep"                     "fatal" "$(InterpreterVerdict 3 2 0)"
    Expect "3.2 SKIPS a self-test rather than failing" "skip"  "$(InterpreterVerdict 3 2 1)"
    Expect "4.3 SKIPS a self-test rather than failing" "skip"  "$(InterpreterVerdict 4 3 1)"
    Expect "a modern bash never skips its own self-test" "ok" "$(InterpreterVerdict 5 2 1)"
    # Not bash at all -- `sh script.sh`. A sweep refuses; a self-test still skips,
    # because "this machine could not check it" is true either way.
    Expect "a non-bash interpreter refuses a sweep"  "fatal" "$(InterpreterVerdict "" 0 0)"
    Expect "a non-bash interpreter skips a self-test" "skip" "$(InterpreterVerdict "" 0 1)"

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

# python3 by name, as `scripts/coverage.sh` already needs it. Never a bare
# `python` fallback: on an old host that finds a Python 2 and the plan below
# fails in a way that reads like an empty database.
command -v python3 >/dev/null 2>&1 \
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
#
# `clang-tidy -p <dir> <file>` does NOT take the first matching entry: libTooling's
# `ClangTool::run` asks the database for the commands of a file and then loops over
# **all** of them. So a file with one command needs nothing special, and a file with
# several must not ALSO be handed to the shared database -- that invocation already
# analyses every one of its commands, and the per-command databases would then
# re-analyse each of them a second time. A file is therefore served by the shared
# database when it has exactly one command, and by one single-entry database per
# command when it has more. The unit of work stays the compile command either way;
# what changes is that each is analysed exactly once.
#
# Reading the database is also the only honest answer to what this platform
# compiles: `IocpReactor.cpp` is a translation unit on Windows and a file on
# Linux, so a list taken from `git ls-files` alone would fail a full sweep on
# sources no target here builds.
#
# **First-party means git knows about the file**, and that is the definition
# rather than an exclusion list of directories. A fetched dependency's sources
# are compile-database entries like any other -- Catch2, yaml-cpp and lz4 are
# ~150 of the 594 here -- and where they LAND is configuration: `_deps/` under
# the build tree by default, but `$CPM_SOURCE_CACHE` anywhere, which in CI is
# `.cache/CPM` INSIDE the workspace. A path-shaped exclusion gets that wrong the
# moment somebody moves the cache, and gets it wrong in the direction that makes
# a full sweep fail on somebody else's code. The intersection with what git
# tracks also subsumes the absolute-path and out-of-tree cases for free.
#
# @param 1 A file of repo-relative paths to keep, or "--all" for every unit.
# @param 2 A file of the repo-relative paths git knows about.
# Prints one tab-separated `<database dir> <file>` row per distinct compile
# command; the caller checks that the result is not empty.
PlanUnits() {
    python3 - "${DB}/compile_commands.json" "$repo_root" "$DB" "${scratch}/db" "$1" "$2" <<'PLAN'
import json, os, sys

# The rows below are split on tabs and newlines by the caller, so the newline has
# to be the one byte the shell expects. Python's stdout translates "\n" to the
# platform line ending, and a trailing "\r" on every path turns the whole plan into
# files that "do not exist".
sys.stdout.reconfigure(newline="\n")

dbPath, root, dbDir, outDir, selectionPath, firstPartyPath = sys.argv[1:7]

def readSet(path):
    with open(path, encoding="utf-8") as handle:
        return {line.strip() for line in handle if line.strip()}

selection = None if selectionPath == "--all" else readSet(selectionPath)
firstParty = readSet(firstPartyPath)

with open(dbPath, encoding="utf-8") as handle:
    entries = json.load(handle)

prefix = root.replace("\\", "/").rstrip("/") + "/"
byFile = {}
for entry in entries:
    path = entry.get("file", "").replace("\\", "/")
    if path.startswith(prefix):
        path = path[len(prefix):]
    if path not in firstParty:
        continue
    if selection is not None and path not in selection:
        continue
    command = entry.get("command") or " ".join(entry.get("arguments", []))
    byFile.setdefault(path, {}).setdefault(command, entry)

if selection is not None:
    for path in sorted(selection - byFile.keys()):
        print("not a translation unit here: " + path, file=sys.stderr)

# A file with a single command is served by the real database, which analyses that
# one command. A file with several is served only by single-entry databases, one
# per command -- the shared database would analyse all of them in one invocation
# and the per-command ones would then repeat every single analysis.
slot = 0
for path, commands in byFile.items():
    if len(commands) == 1:
        print(dbDir + "\t" + path)
        continue
    for entry in commands.values():
        slot += 1
        directory = os.path.join(outDir, str(slot))
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(directory, "compile_commands.json"), "w",
                  encoding="utf-8") as handle:
            json.dump([entry], handle)
        print(directory + "\t" + path)
PLAN
}

# A canary against a real file. Anything that stops clang-tidy from parsing shows
# up here rather than as an entire branch reported clean.
#
# Called only once there is something to sweep. It costs a full translation unit
# with the analyzer on, and a documentation-only pull request must not pay that
# to be told it has nothing to do -- the canary exists to stop a CLEAN VERDICT
# being believed, and a sweep with no units earns no verdict.
Canary() {
    local probe_file probe probe_rc
    probe_file="$(git ls-files 'src/FastCache/Core/*.cpp' | head -1)"
    [[ -n "$probe_file" ]] || fatal "no probe file to canary against"
    probe="$("$TIDY" -p "$DB" --quiet "$probe_file" 2>&1)"
    probe_rc=$?
    [[ "$probe_rc" -ge 126 ]] && fatal "$TIDY could not be executed (exit ${probe_rc})"
    case "$probe" in
        *"Permission denied"*|*"error: no such file or directory: '@"*|*"'stddef.h' file not found"*)
            fatal "$TIDY is not parsing: ${probe}" ;;
    esac
}

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
    declare -a sources=() touched=() globs=()
    for extension in "${SourceExtensions[@]}"; do globs+=("*.${extension}"); done
    # `--cached --others --exclude-standard`, the same definition of first-party
    # the plan below uses. A bare `git ls-files` sees only what is TRACKED, so a
    # header created and not yet added would be missing from the include graph and
    # every translation unit that includes it would drop out of the scope --
    # silently, and for exactly the code nothing has ever checked.
    #
    # Filtered to what is actually on disk, because `--cached` also lists a tracked
    # file deleted from the worktree and not yet staged, and the include scan now
    # treats a file it cannot read as fatal rather than as an empty graph.
    mapfile -t sources < <(git ls-files --cached --others --exclude-standard "${globs[@]}" \
                           | while IFS= read -r candidate; do
                                 [[ -f "$candidate" ]] && printf '%s\n' "$candidate"
                             done)
    for path in "${changed[@]}"; do
        for extension in "${SourceExtensions[@]}"; do
            [[ "$path" == *".${extension}" ]] && { touched+=("$path"); break; }
        done
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

# `--cached --others --exclude-standard` rather than plain `git ls-files`: a new
# source that has been created but not added yet is exactly the code nothing has
# ever checked, and dropping it here would drop it silently.
git ls-files --cached --others --exclude-standard > "${scratch}/first-party"

# Through a file, so the plan's exit status is OBSERVED. `mapfile < <(PlanUnits …)`
# discards it, and every way the plan can fail -- a compile database this build
# cannot parse, a scratch directory it cannot write, a python that dies -- then
# produces exactly zero rows, which reads as "nothing to sweep" and exits 0. A
# green check over nothing analysed is the one verdict this script exists to make
# impossible.
if ! PlanUnits "$selection" "${scratch}/first-party" > "${scratch}/plan"; then
    fatal "could not read ${DB}/compile_commands.json"
fi
mapfile -t plan < "${scratch}/plan"
if [[ "${#plan[@]}" -eq 0 ]]; then
    if [[ "$mode" == all ]]; then
        fatal "no first-party translation units in ${DB}/compile_commands.json"
    fi
    echo "TIDY SWEEP: nothing changed here reaches a translation unit this platform"
    echo "            compiles; nothing to sweep"
    exit 0
fi
echo "TIDY SWEEP: ${#plan[@]} translation unit(s), ${TIDY}, ${JOBS} at a time"
Canary

# One translation unit, into numbered files so the report below is in a stable
# order however the pool interleaves. A refusal to EXECUTE is recorded apart from
# a finding: every way of getting clang-tidy wrong produces silence, and silence
# must not be summed into a clean verdict.
# @param 1 The database directory holding this unit's compile command.
# @param 2 The file to check.
# @param 3 Slot prefix under $scratch.
TidyOne() {
    local database="$1" file="$2" slot="$3" out rc hits contribution
    # Asked BEFORE the analysis, and recorded whatever the analysis then says: a
    # unit that contributes nothing is not evidence about its file even when
    # clang-tidy reports cleanly on it -- that clean report is the defect (#466).
    # Costs one preprocessor run, measured at 5-26% of what clang-tidy costs on the
    # same unit, worst on the cheapest units because those are the empty ones.
    contribution="$(UnitContribution "$database" "$file")"
    case "$contribution" in
        empty)   printf '%s\n' "$file" > "${slot}.empty" ;;
        unknown) printf '%s\n' "$file" > "${slot}.unknown" ;;
    esac
    out="$("$TIDY" -p "$database" --quiet "$file" 2>&1)"
    rc=$?
    if [[ "$rc" -ge 126 ]]; then
        printf '%s (exit %s)\n' "$file" "$rc" > "${slot}.fatal"
        return
    fi
    # Unknown *warning options* are the GCC-only flags a clang build has no use
    # for; everything else is a finding.
    hits="$(printf '%s\n' "$out" | grep -E 'error:|warning:' | grep -v 'unknown-warning-option')"
    if [[ -n "$hits" ]]; then
        printf '=== %s\n%s\n' "$file" "$hits" > "${slot}.out"
    fi
}

index=0
running=0
declare -a skipped=()
for unit in "${plan[@]}"; do
    database="${unit%%$'\t'*}"
    file="${unit#*$'\t'}"
    # A unit the database names and the tree does not have -- a source deleted
    # since the database was generated. Recorded rather than passed over: a count
    # that silently disagrees with the plan is the one thing this script must not
    # print.
    if [[ ! -f "$file" ]]; then
        skipped+=("$file")
        continue
    fi
    index=$((index + 1))
    TidyOne "$database" "$file" "$(printf '%s/%05d' "$scratch" "$index")" &
    # A counter rather than `jobs -rp | wc -l`, which forks twice per unit just
    # to count children.
    running=$((running + 1))
    if [[ "$running" -ge "$JOBS" ]]; then
        wait -n
        running=$((running - 1))
    fi
done
wait

if [[ "${#skipped[@]}" -gt 0 ]]; then
    echo "TIDY SWEEP: ${#skipped[@]} unit(s) in the database are not in the tree and were"
    echo "            not swept: ${skipped[*]}"
fi

# A plan with units in it and nothing swept out of it is not a clean branch: it is
# a database that no longer describes this tree -- every entry stale, or every path
# in it unreachable from here. Left alone it prints "CLEAN (0 translation unit(s))"
# and exits 0, which is the same green check as a real sweep and covers no code at
# all. The empty-plan cases above are the legitimate ones and have already exited.
if [[ "$index" -eq 0 ]]; then
    fatal "none of the ${#plan[@]} planned unit(s) exist in this tree; ${DB} does not describe it"
fi

shopt -s nullglob
fatals=("$scratch"/*.fatal)
[[ "${#fatals[@]}" -gt 0 ]] && fatal "$TIDY failed to run on $(cat "${fatals[@]}")"

status=0
for report in "$scratch"/*.out; do
    cat "$report"
    status=1
done

# The count that means something, and the two that qualify it (#466).
#
# `index` is units ANALYSED. Some of them contribute nothing of their own -- a file
# whose body sits behind a platform or feature guard preprocesses to an empty
# translation unit here, so clang-tidy read nothing of it and reported nothing
# about it. Summing those into one number is what let a syntax error survive five
# green signals.
#
# "produced no code in this configuration" and not "on this platform": of the
# thirteen such units measured in a default Linux build, eleven are platform-gated
# and two (`TlsContext_test.cpp`, `TlsSocket_test.cpp`) are gated on
# `FC_TLS_ENABLED` and would contribute on this very platform with TLS on. Naming
# the platform would tell a reader the file belongs to another OS when it belongs
# to another build -- and this sentence must never read as a fault, because an
# empty unit here is the guard working.
shopt -s nullglob
empties=("$scratch"/*.empty)
unknowns=("$scratch"/*.unknown)
if [[ "${#empties[@]}" -gt 0 ]]; then
    echo "TIDY SWEEP: ${#empties[@]} of ${index} unit(s) produced no code in this configuration,"
    echo "            so this run says NOTHING about them (they are analysed, not covered):"
    sort -u "${empties[@]}" | sed 's/^/              /'
fi
if [[ "${#unknowns[@]}" -gt 0 ]]; then
    echo "TIDY SWEEP: ${#unknowns[@]} unit(s) could not be preprocessed, so whether they"
    echo "            contribute is UNKNOWN rather than either answer:"
    sort -u "${unknowns[@]}" | sed 's/^/              /'
fi

if [[ "$status" -eq 0 ]]; then
    # Unknowns are subtracted too. A unit whose preprocessor failed has not been
    # shown to contribute, and counting it as covered would be the same overstatement
    # in a quieter place -- the number would still be larger than the evidence.
    covered=$((index - ${#empties[@]} - ${#unknowns[@]}))
    echo "TIDY SWEEP CLEAN (${covered} of ${index} translation unit(s) contributed code, ${TIDY})"
fi
exit "$status"
