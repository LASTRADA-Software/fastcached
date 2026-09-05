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
# than a platform one -- `TlsSocket_test.cpp` is empty without `FASTCACHED_ENABLE_TLS`
# and analysed with it. Modelling the preprocessor to decide this is the failure mode
# `check-net-boundary`'s header warns about: a model that is subtly wrong fails in the
# confident direction. So this asks the OBJECTS what the compiler actually emitted.
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
# Usage: check-tidy-blind-spots.sh <build-dir>
set -uo pipefail

# Named `refuse` rather than `fail`: `scripts/lib/e2e-common.sh` owns a shared `fail`
# for the e2e fixtures, which prints that suite's wording and signals the top-level
# shell. A hygiene check needs neither, and a second definition of a shared helper's
# name is what `check-e2e-helpers.sh`'s helper-scan refuses -- correctly, since the
# two would drift.
refuse() { echo "CMake Error: tidy-blind-spots: $*" >&2; exit 1; }

build="${1:-}"
[[ -n $build ]] || refuse "usage: check-tidy-blind-spots.sh <build-dir>"
db="$build/compile_commands.json"
[[ -f $db ]] || refuse "no compile database at '$db'; configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON"

# `nm` is the instrument. A missing one is a REFUSAL and never a skip: a check that
# cannot measure has established nothing, and reporting that as a pass is the defect
# this file exists to catch, one level up.
nm_bin="${NM:-nm}"
command -v "$nm_bin" >/dev/null 2>&1 || refuse "'$nm_bin' not found; set NM= to the binutils nm for this toolchain"

# The two-symbol signature is the SANITIZER's -- `asan.module_ctor` and
# `___asan_globals_registered` are what an otherwise-empty object still defines. In a
# build without ASan an empty TU defines nothing, but so does a TU that only defines
# inline or template entities, and the check would then report both alike. Stated as a
# precondition rather than tuned around.
grep -q -- '-fsanitize=address' "$db" \
    || refuse "this build has no AddressSanitizer, and the empty-object signature is the sanitizer's; configure the sweep's preset (clang-debug) to run this"

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

measured="$(NM_BIN="$nm_bin" DB="$db" REPO="$repo" python3 "$here/tidy-blind-spots-scan.py")" || refuse "measurement failed (see above)"

analysed="$(printf '%s' "$measured" | grep -c . || true)"

expected="$(grep -v '^[[:space:]]*#' "$table" | grep . | cut -f1 | sort)"
actual="$(printf '%s\n' "$measured" | grep . | sort)"

rc=0
while IFS= read -r f; do
    [[ -z $f ]] && continue
    echo "CMake Error: tidy-blind-spots: '$f' is analysed by nothing and is not in scripts/tidy-blind-spots.txt." >&2
    echo "  It compiles to an empty object on the platform the sweep runs on, so no analyser this project" >&2
    echo "  runs reads a line of it. The sweep counts it; nothing enforces it. Add a row with a reason," >&2
    echo "  or give it an analyser." >&2
    rc=1
done < <(comm -13 <(printf '%s\n' "$expected") <(printf '%s\n' "$actual"))

while IFS= read -r f; do
    [[ -z $f ]] && continue
    if [[ ! -f "$repo/$f" ]]; then
        echo "CMake Error: tidy-blind-spots: scripts/tidy-blind-spots.txt names '$f', which does not exist." >&2
    else
        echo "CMake Error: tidy-blind-spots: '$f' is listed as analysed by nothing, but it is analysed now." >&2
        echo "  Delete its row: a stale exemption hides the next real one." >&2
    fi
    rc=1
done < <(comm -23 <(printf '%s\n' "$expected") <(printf '%s\n' "$actual"))

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
config="AddressSanitizer on, FASTCACHED_ENABLE_TLS on, $(uname -s)"
if [[ $rc -eq 0 ]]; then
    echo "tidy-blind-spots: [$config] $analysed translation unit(s) analysed by nothing, all accounted for in scripts/tidy-blind-spots.txt"
else
    echo "tidy-blind-spots: [$config] measured $analysed translation unit(s) analysed by nothing; the table above does not describe them"
fi
exit "$rc"
