#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Refuse when a translation unit is analysed by NOTHING.
#
# The clang-tidy sweep runs on `ubuntu-24.04` (`.github/workflows/build.yml`, the
# `clang-tidy` job), and `scripts/local-gate.sh` tidies through the Linux presets, so
# a TU whose entire body sits inside a guard that is false on Linux preprocesses away
# to nothing and is analysed by no analyser this project runs
# ([#682](https://github.com/LASTRADA-Software/fastcached/issues/682)).
#
# **The sweep already OBSERVES this, and that is not what this check adds.** #466 gave
# `tidy-sweep.sh` a per-unit produced/empty/unknown verdict; it prints the count, lists
# the in-scope files and qualifies its own verdict -- measured, `13 of 372 file(s)
# produced no code in this configuration` above a `CLEAN (359 of 372 …)` line. #682's
# premise that the sweep silently reports clean is FALSE, and this header said so too
# until it was checked against the real sweep rather than against the ticket.
#
# What is missing is enforcement, and it is four specific things:
#
#   * The sweep is INFORMATIONAL -- `rc=0` whatever the number. Nothing has to read it.
#   * It names only files in the CHANGE's scope, so a unit going blind elsewhere is
#     counted and unnamed.
#   * It cannot refuse a SWAP: one unit leaves the set, another joins, count unchanged.
#   * Its number carries no CONFIGURATION, and the number depends on one.
#
# So the honest claim is that the sweep observes this and does not enforce it. Anything
# stronger overstates, which is the defect this file is about.
#
# ## Why this MEASURES rather than greps
#
# A source scan cannot answer it. `#else`-less platform guards also appear at the END
# of files that are otherwise ordinary, and a TU can go blind on a FEATURE flag rather
# than a platform one -- `Net/TlsSocket_test.cpp` was empty without
# `FASTCACHED_ENABLE_TLS` and analysed with it, until #1596 moved it into core-cpp.
# Modelling the preprocessor to decide this fails the way any subtly wrong model does:
# in the confident direction. So this asks the OBJECTS what the compiler actually emitted.
#
# A TU that contributed nothing defines exactly the two symbols the sanitizer adds to
# every object; anything real defines hundreds. The threshold is therefore not a
# tuning knob, it is the gap between 2 and 672.
#
# ## Why the table cannot be the evidence
#
# `scripts/tidy-blind-spots.txt` lists what is known blind, with a reason each. This
# check compares it against the MEASURED set and refuses in BOTH directions: a TU that
# goes blind without a row is a new hole, and a row that is no longer blind is a claim
# that has gone stale. Neither can be silenced by editing the table, because the table
# is the expectation and never the input.
#
# Usage: check-tidy-blind-spots.sh <build-dir>   -- the measurement plus the table rules
#        check-tidy-blind-spots.sh --table-only  -- the table rules alone, no build needed
set -uo pipefail

# Named `refuse` rather than `fail`: `scripts/lib/e2e-common.sh` owns a shared `fail`
# for the e2e fixtures, which prints that suite's wording and signals the top-level
# shell. A hygiene check needs neither, and a second definition of a shared helper's
# name is what `check-e2e-helpers.sh`'s helper-scan refuses -- correctly, since the
# two would drift.
refuse() { echo "CMake Error: tidy-blind-spots: $*" >&2; exit 1; }

# `--table-only` is the half of this check that needs NO build (#589).
#
# The measurement below needs a compile database, the objects and `nm`, which is why
# `tidy-blind-spots` is registered on Linux with ASan and TLS on and runs NOWHERE else.
# But the TABLE's own rules -- every reached-by column names a leg that exists, and
# every `none` row carries a REASON -- are properties of two text files and the
# workflow. Gating those behind a build meant they were asserted on one configuration
# and on no other, which is the same shape as the residual the registration block
# records: a rule that reports nothing reads like a pass.
#
# #589's retargeted acceptance asks for exactly this: compiler-free, in the default
# `ctest` set, deriving the legs from the workflow rather than restating them, and
# refusing when either scan matches nothing.
#
# `--python3` is passed in rather than resolved here, following `scripts/coverage.sh`.
# The registration hands over the interpreter `find_package(Python3)` VALIDATED by
# running it -- which is what screens out Windows' App Execution Alias, a zero-byte
# reparse point named `python3.exe` that a bare `python3` on PATH finds happily and
# that does not run Python. The default keeps a hand-run working on a POSIX host.
tableOnly=0
python3_bin="python3"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --table-only)
            tableOnly=1
            shift
            ;;
        --python3)
            [[ $# -ge 2 ]] || refuse "--python3 needs a path"
            python3_bin="$2"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        -*)
            refuse "unknown option '$1'; usage: check-tidy-blind-spots.sh [--python3 <exe>] <build-dir> | --table-only [--python3 <exe>]"
            ;;
        *)
            break
            ;;
    esac
done

build="${1:-}"
if [[ $tableOnly -eq 0 ]]; then
    [[ -n $build ]] || refuse "usage: check-tidy-blind-spots.sh <build-dir> | --table-only"
    db="$build/compile_commands.json"
    [[ -f $db ]] || refuse "no compile database at '$db'; configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON"
fi

# `nm` is the instrument. A missing one is a REFUSAL and never a skip: a check that
# cannot measure has established nothing, and reporting that as a pass is the defect
# this file exists to catch, one level up.
nm_bin="${NM:-nm}"
if [[ $tableOnly -eq 0 ]]; then
    command -v "$nm_bin" >/dev/null 2>&1 || refuse "'$nm_bin' not found; set NM= to the binutils nm for this toolchain"
fi

# The two-symbol signature is the SANITIZER's -- `asan.module_ctor` and
# `___asan_globals_registered` are what an otherwise-empty object still defines. In a
# build without ASan an empty TU defines nothing, but so does a TU that only defines
# inline or template entities, and the check would then report both alike. Stated as a
# precondition rather than tuned around.
if [[ $tableOnly -eq 0 ]]; then
    grep -q -- '-fsanitize=address' "$db" \
        || refuse "this build has no AddressSanitizer, and the empty-object signature is the sanitizer's; configure the sweep's preset (clang-debug) to run this"
fi

# The tree being judged, overridable so the selftest can point this at a staged one.
# Derived from `BASH_SOURCE` otherwise -- and NOT simply copied next to a staged tree,
# because a script whose root comes from its own location judges wherever it happens
# to live, which is how a fixture ends up testing the real repository by accident.
# Where THIS script lives, for finding its sibling scan. Distinct from `repo` below
# on purpose: a sibling ships with this file and is found by `BASH_SOURCE`, while the
# tree under test must never be.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

repo="${FASTCACHED_SOURCE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
table="$repo/scripts/tidy-blind-spots.txt"
[[ -f $table ]] || refuse "missing $table"

measured=""
analysed=0
if [[ $tableOnly -eq 0 ]]; then
    measured="$(NM_BIN="$nm_bin" DB="$db" REPO="$repo" "$python3_bin" "$here/tidy-blind-spots-scan.py")" || refuse "measurement failed (see above)"
    analysed="$(printf '%s' "$measured" | grep -c . || true)"
fi

# Which analyser legs EXIST, derived from the workflow rather than restated here.
# A second copy of this list would be a second thing to be wrong, and the failure
# would be silent: rows claiming coverage from a leg that was renamed away.
legs="$("$python3_bin" "$here/tidy-blind-spots-legs.py" "$repo/.github/workflows/build.yml")" || refuse "a step runs tidy-sweep.sh in a mode this check does not recognise; see UNKNOWN-SWEEP-MODE above. A mode it cannot classify would make a real leg invisible and refuse every row naming it."
[[ -n $legs ]] || refuse "no clang-tidy leg found in .github/workflows/build.yml; the reached-by column cannot be checked"

expected="$(grep -v '^[[:space:]]*#' "$table" | grep . | cut -f1 | sort)"
actual="$(printf '%s\n' "$measured" | grep . | sort)"

rc=0
# Both directions compare the table against the OBJECTS, so neither runs in
# `--table-only`. Skipped explicitly rather than by accident: a mode that names its
# set may not report clean over a member it could not cover, and the verdict below
# says which half ran.
while IFS= read -r f; do
    [[ $tableOnly -eq 1 ]] && break
    [[ -z $f ]] && continue
    echo "CMake Error: tidy-blind-spots: '$f' is analysed by nothing and is not in scripts/tidy-blind-spots.txt." >&2
    echo "  It compiles to an empty object on the platform the sweep runs on, so no analyser this project" >&2
    echo "  runs reads a line of it. The sweep counts it; nothing enforces it. Add a row with a reason," >&2
    echo "  or give it an analyser." >&2
    rc=1
done < <(comm -13 <(printf '%s\n' "$expected") <(printf '%s\n' "$actual"))

while IFS= read -r f; do
    [[ $tableOnly -eq 1 ]] && break
    [[ -z $f ]] && continue
    if [[ ! -f "$repo/$f" ]]; then
        echo "CMake Error: tidy-blind-spots: scripts/tidy-blind-spots.txt names '$f', which does not exist." >&2
    else
        echo "CMake Error: tidy-blind-spots: '$f' is listed as analysed by nothing, but it is analysed now." >&2
        echo "  Delete its row: a stale exemption hides the next real one." >&2
    fi
    rc=1
done < <(comm -23 <(printf '%s\n' "$expected") <(printf '%s\n' "$actual"))

# Every row's reached-by column must name legs that EXIST, or `none`. A row naming a
# leg that was renamed or deleted is a claim of coverage nothing provides, and it is
# the shape that fails silently: the unit stays listed, the table stays plausible,
# and nobody is analysing it.
#
# And every row carries a REASON. The format line at the head of the table says so,
# and on a `none` row the reason is the only account there is of why nothing reads the
# unit: the leg names on the other rows explain themselves, `none` explains nothing.
# A `none` row with the reason blanked is a row that has stopped being an exemption
# and become a bare fact, which is `RefuseUntriaged` spelled as `Refuse` -- *forgot*
# in the vocabulary of *decided*.
rows=0
unreached=0
while IFS=$'\t' read -r path reached why; do
    [[ -z ${path:-} ]] && continue
    [[ $path == \#* ]] && continue
    rows=$((rows + 1))

    # Whitespace-only counts as absent. A reason column holding a space satisfies a
    # `-n` test and says nothing, so the test is for a non-blank character.
    if [[ ! ${why:-} =~ [^[:space:]] ]]; then
        echo "CMake Error: tidy-blind-spots: '$path' carries no reason in its third column." >&2
        if [[ $reached == "none" ]]; then
            echo "  It is a 'none' row, so the reason is the only account of why no analyser reads this unit." >&2
            echo "  Say what makes it empty on every leg -- a guard, a platform, a feature flag -- or give it a leg." >&2
        else
            echo "  Format: <path><TAB><reached-by><TAB><reason>. Say what makes it empty on the legs that do not read it." >&2
        fi
        rc=1
    fi

    if [[ $reached == "none" ]]; then
        unreached=$((unreached + 1))
        continue
    fi
    IFS=',' read -ra names <<< "$reached"
    for leg in "${names[@]}"; do
        leg="${leg// /}"
        if ! grep -qx -- "$leg" <<< "$legs"; then
            echo "CMake Error: tidy-blind-spots: '$path' claims coverage by '$leg', which is not a clang-tidy leg in .github/workflows/build.yml." >&2
            echo "  A row naming a leg that does not exist is a claim of coverage nothing provides." >&2
            rc=1
        fi
    done
done < <(grep -v '^[[:space:]]*#' "$table" | grep .)

# A scan matching nothing is a REFUSAL, not a clean run -- in `--table-only`, where the
# table and the legs are the only two scans there are. An empty table makes every rule
# above pass vacuously (no row claims a dead leg, no row lacks a reason) and the check
# then reports exactly what a healthy tree reports. That is #589's "refuse when either
# scan matches nothing"; the legs scan has its own refusal above.
#
# Scoped to that mode on purpose, and this is not a softening. With a build the TABLE
# is not the evidence -- the measurement is, and the check says so at the head of this
# file. An empty table there is the ordinary expectation *nothing is blind*, which the
# measurement falsifies by name if it is wrong. Refusing it would be refusing a
# well-formed claim for being small, and it would take six selftest cases that stage a
# comment-only table with it.
if [[ $tableOnly -eq 1 ]]; then
    [[ $rows -gt 0 ]] || refuse "scripts/tidy-blind-spots.txt has no rows, so every rule above it passes vacuously. A table that matches nothing cannot be evidence."
fi

# The number #858 is about, printed on every run and not only on a refusal. A leg
# that closes part of the hole must not be able to read as closing it.
echo "tidy-blind-spots: $unreached translation unit(s) are reached by NO analyser leg (#858)"
if [[ $unreached -gt 0 ]]; then
    grep -v '^[[:space:]]*#' "$table" | grep . | awk -F'\t' '$2 == "none" { print "    " $1 }'
fi

# The count is printed whatever the verdict, because "how much of this tree does the
# analyser never see" is the question the ticket asks and a pass that says nothing
# answers it for nobody. But the two verdicts get DIFFERENT sentences: a refusing run
# that also printed "all accounted for" would contradict itself in the direction of
# reassurance, which is the failure this check exists to catch.
# The verdict names the CONFIGURATION it measured, not only the number. The blind set
# is a property of a build, not of a tree -- the same tree gives 11 here and 26 in a
# Release build without ASan -- so a count reported without its configuration is the
# figure-without-its-conditions mistake this project already has a rule about, and is
# exactly how the first census of this set came out wrong.
#
# `--table-only` gets its own two sentences for the same reason. A mode that could
# not measure must not borrow the measuring mode's wording: "all accounted for in
# scripts/tidy-blind-spots.txt" is a claim about the OBJECTS, and printing it after a
# run that never opened one is the four-state collapse this check is an instance of --
# *the table is well-formed* read as *the tree is covered*. It names the half it ran
# and the half it did not, and it says which check answers the other half.
if [[ $tableOnly -eq 1 ]]; then
    if [[ $rc -eq 0 ]]; then
        echo "tidy-blind-spots: [table only, no build] $rows row(s) well-formed against $(grep -c . <<< "$legs") analyser leg(s) derived from .github/workflows/build.yml"
        echo "  The blind SET is not measured here -- that needs the objects, and 'tidy-blind-spots' is the test that measures it."
    else
        echo "tidy-blind-spots: [table only, no build] $rows row(s) read from scripts/tidy-blind-spots.txt; the table does not satisfy its own format"
    fi
    exit "$rc"
fi

config="AddressSanitizer on, FASTCACHED_ENABLE_TLS on, $(uname -s)"
if [[ $rc -eq 0 ]]; then
    # "contribute nothing HERE", not "analysed by nothing" -- since a second leg
    # exists those are different numbers, and the one above is the smaller and the
    # one that matters. Saying 11 are analysed by nothing while reporting 5 reached
    # by no leg would contradict itself in the direction of alarm.
    echo "tidy-blind-spots: [$config] $analysed translation unit(s) contribute nothing in this configuration, all accounted for in scripts/tidy-blind-spots.txt"
else
    echo "tidy-blind-spots: [$config] measured $analysed translation unit(s) contributing nothing in this configuration; the table above does not describe them"
fi
exit "$rc"
