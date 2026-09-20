#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# A defect the declared clang-tidy build is KNOWN to have is asserted against that build, never remembered.
#
# ## Why
#
# The analyser `.clang-tidy-version` declares crashes on some source shapes. The tree rewrites every site of such a
# shape rather than disable the check (#1404): a crash fails the build closed, and a disabled check is NOLINT by other
# means. But a rewrite with no reason beside it reads as an accident to the next cleanup, and nothing would say when it
# may come out. So each defect is a ROW below, carrying the check it lives in, the issue that documents it, and three
# units planted and run through the declared build:
#
#   crashes   the shape itself, which the build must STILL crash on: exit 139 and a stack dump. Measured on the Linux
#             wheel and on the Windows wheels under Git Bash, which reports the access violation as 139 too.
#   named     the same computation with the offending call named first, which must analyse cleanly -- so the unit
#             parses on this host, and a crash in `crashes` belongs to the shape rather than to a header search.
#   live      a shape the check reports, which it must report -- so the check is enabled and running in this build,
#             and `crashes` not crashing can mean only that the defect is gone.
#
# When the pin moves to an LLVM carrying the upstream fix, `crashes` stops crashing and `--installed` goes red ON THE
# CHANGE THAT MOVED THE PIN, naming what that change removes: the row, and the comment naming its issue at each site,
# which it lists. The rewrites themselves may stay.
#
# **So a row is its own record, and needs no open issue.** The event a known defect waits for is the pin moving, and
# the row is what fires on it; an issue held open for the same event would be a second copy of the tripwire, and the
# one nothing wires. The issue a row names is where the defect is DOCUMENTED, open or closed (#1410). For the same
# reason a table with NO rows is legal in every mode: the change that retires the last defect deletes the last row,
# and the guard it leaves behind must still pass.
#
# ## Modes
#
#   (default)           Every row is well formed, and every source line under `src/` that names a row's check also
#                       names that row's issue -- a site comment is one line for exactly that reason. Needs no
#                       analyser, runs in the default ctest set.
#   --installed <exe>   Run every row's three units through <exe> and require the outcomes above. Run by both
#                       clang-tidy jobs after they identify the install, and by local-gate.sh after it resolves the
#                       analyser. Not a ctest: no build leg has the analyser, so it would skip everywhere (#1135).
#   --self-test         Drive the decision over every outcome, and both other modes end to end over COPIES of this
#                       script carrying a sample table -- one row, a malformed row, and none -- so no case depends on
#                       what the shipped table holds. The shipped tree is checked last, against the shipped table.
#
# bash 3.2: the default and self-test modes run in ctest on macOS. There, `"${Rows[@]}"` on an EMPTY array is an
# unbound variable under `set -u` -- measured on 3.2.57, and fixed only in bash 4.4 -- so every walk of the table is
# `${Rows[@]+"${Rows[@]}"}`. `${#Rows[@]}` is safe on 3.2 and is used as it is.
#
# Usage:
#   bash scripts/check-clang-tidy-known-defects.sh [<source-dir>]
#   bash scripts/check-clang-tidy-known-defects.sh --installed <exe> [<source-dir>]
#   bash scripts/check-clang-tidy-known-defects.sh --self-test [<source-dir>]

set -uo pipefail

UsageError=2

# The analyser was found and CANNOT ANSWER (#1574) -- its own build is unusable here, so no row's outcome is a
# statement about the tree or about the defect the row names.
#
# Its own status, and not a `FAIL` sharing 1 with "a row changed status", because those are opposite diagnoses sent
# to opposite people. Measured on this repository's Windows host: the PyPI clang-tidy wheel `--resolve` finds under
# Git Bash ships no C++ standard library, so every row's live unit failed on `#include <algorithm>` and this script
# refused with *"modernize-min-max-use-initializer-list did not report the live unit ... whether the crashing shape
# still crashes says nothing"* -- a confident, specific claim about #1410's status, from a run that had learned
# nothing about it. The same script, same arguments, under WSL exits 0.
#
# 77, the number this tree already spells "the prerequisite was not there", so a consumer that wants to skip can and
# one that wants to refuse can say WHY. `local-gate.sh` refuses, naming the analyser build rather than the tree.
UnusableAnalyser=77

# name|check|issue|crashes|named|live
# The three units are `printf %b` text: `\n` separates lines, and no unit may contain a `|`.
Rows=(
    "min-max-pointer-call|modernize-min-max-use-initializer-list|1410|#include <algorithm>\n#include <cstddef>\nstd::size_t Widest(std::size_t (*width)(int), std::size_t floor) { return std::max({ floor, width(1) }); }|#include <algorithm>\n#include <cstddef>\nstd::size_t Widest(std::size_t (*width)(int), std::size_t floor) { auto const measured = width(1); return std::max({ floor, measured }); }|#include <algorithm>\n#include <cstddef>\nstd::size_t Widest(std::size_t a, std::size_t b, std::size_t c) { return std::max(a, std::max(b, c)); }"
)

Mode="static"
Argument=""
case "${1:-}" in
    --installed)
        Mode="installed"
        Argument="${2:-}"
        [ -n "$Argument" ] || { echo "FAIL: $1 needs an argument" >&2; exit "$UsageError"; }
        shift 2
        ;;
    --self-test)
        Mode="self-test"
        shift
        ;;
esac
SourceDir="${1:-}"
if [ -z "$SourceDir" ]; then
    SourceDir="$(cd "$(dirname "$0")/.." && pwd)"
fi

# $1 = a row. Sets RowName, RowCheck, RowIssue, RowCrashes, RowNamed, RowLive; returns 1 on a malformed row.
SplitRow() {
    local rest="$1" separators="${1//[^|]/}"
    # Exactly five separators: with fewer, each `${rest#*|}` below would leave the last field standing in for the next.
    [ "${#separators}" -eq 5 ] || return 1
    RowName="${rest%%|*}"; rest="${rest#*|}"
    RowCheck="${rest%%|*}"; rest="${rest#*|}"
    RowIssue="${rest%%|*}"; rest="${rest#*|}"
    RowCrashes="${rest%%|*}"; rest="${rest#*|}"
    RowNamed="${rest%%|*}"; rest="${rest#*|}"
    RowLive="$rest"
    [ -n "$RowName" ] && [ -n "$RowCrashes" ] && [ -n "$RowNamed" ] && [ -n "$RowLive" ] || return 1
    case "$RowCheck" in "" | *[!a-z0-9-]*) return 1 ;; esac
    case "$RowIssue" in "" | *[!0-9]*) return 1 ;; esac
    return 0
}

# $1 = output. True when it carries the stack dump clang prints on a crash.
HasStackDump() {
    case "$1" in
        *"Stack dump:"*) return 0 ;;
        *) return 1 ;;
    esac
}

# Whether the analyser can answer AT ALL, over what two trivial units produced (#1574).
#
# TWO probes and not one, because they are two facts and a reader acts on them differently: an analyser that cannot
# analyse `int Preflight() { return 0; }` is broken or is not clang-tidy, while one that manages that and fails on
# `#include <algorithm>` is a build shipped without a C++ standard library -- which is the shape the Windows PyPI
# wheel has, and the one that produced #1574. Sniffing a `file not found` out of the output would collapse them into
# one string match; an exit status per probe is a fact.
#
# $1 = the bare unit's exit, $2 = the standard-header unit's exit.
# Prints exactly one of: usable, runs-nothing, no-standard-library.
JudgePreflight() {
    [ "$1" -eq 0 ] || { echo runs-nothing; return; }
    [ "$2" -eq 0 ] || { echo no-standard-library; return; }
    echo usable
}

# The DECISION, over what three runs produced, so `--self-test` drives every outcome without an analyser.
# $1/$2 = crashes exit and output, $3/$4 = named, $5/$6 = live, $7 = check.
# Prints exactly one of: present, fixed, not-live, named-broken, unknown.
Judge() {
    local crashesRc="$1" crashesOut="$2" namedRc="$3" namedOut="$4" liveRc="$5" liveOut="$6" check="$7"
    case "$liveOut" in
        *"[$check]"*) [ "$liveRc" -eq 0 ] || { echo not-live; return; } ;;
        *) echo not-live; return ;;
    esac
    if [ "$namedRc" -ne 0 ] || HasStackDump "$namedOut"; then
        echo named-broken
        return
    fi
    if [ "$crashesRc" -eq 139 ] && HasStackDump "$crashesOut"; then
        echo present
        return
    fi
    if [ "$crashesRc" -eq 0 ] && ! HasStackDump "$crashesOut"; then
        echo fixed
        return
    fi
    # Neither the crash this row records nor a clean run: a different failure, which says nothing about the defect.
    echo unknown
}

# $1 = dir. Sets Sites to every source line under `src/` naming $RowCheck, one `path:line:text` per line, and
# SourceCount to how many sources there are. Variables rather than stdout: a `$( )` would lose the count. Returns 1,
# with SitesStatus set, when grep could not read the sources -- which is not the same answer as "no site".
#
# The lines are found by ONE recursive grep, never a shell `read` per line: that walked every line of 866 sources
# and measured 62 s on DrvFs with the host idle, and passed ctest's 120 s under a gate's load, so the gate went red
# on a check with nothing to report (#1410).
SitesOf() {
    local dir="$1" found line
    Sites=""
    SourceCount="$(find "$dir/src" -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) 2>/dev/null | wc -l | tr -d ' ')"
    SitesStatus=0
    [ "$SourceCount" -gt 0 ] || return 0
    # grep answers 0 for a match, 1 for none -- an ordinary empty set here -- and 2 when it could not read.
    found="$(grep -rnF --include='*.cpp' --include='*.hpp' --include='*.h' -- "$RowCheck" "$dir/src" 2>/dev/null)"
    SitesStatus=$?
    [ "$SitesStatus" -le 1 ] || return 1
    SitesStatus=0
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        Sites="$Sites${line#"$dir"/}"$'\n'
    done <<< "$found"
}

# An empty table is legal (see the header), and says so rather than passing in silence: a run that printed nothing
# reads the same whether it found no rows or never got as far as looking.
# $1 = what there is nothing to do, for the sentence.
NoRows() {
    echo "no known defect: the table in $(basename "$0") has no rows, so there is nothing $1"
}

CheckStatic() {
    local dir="$1" row site bad=0 count unnamed
    if [ "${#Rows[@]}" -eq 0 ]; then
        NoRows "to ask of the sources under $dir/src"
        return 0
    fi
    for row in ${Rows[@]+"${Rows[@]}"}; do
        if ! SplitRow "$row"; then
            echo "FAIL: malformed row '${row%%|*}': want name|check|issue|crashes|named|live, a check name, a numeric issue"
            bad=1
            continue
        fi
        if ! SitesOf "$dir"; then
            echo "FAIL: grep could not read the sources under $dir/src (exit $SitesStatus), so which lines name '$RowCheck' is"
            echo "      unknown -- which is not the same as none"
            bad=1
            continue
        fi
        if [ "$SourceCount" -eq 0 ]; then
            echo "FAIL: no C++ source under $dir/src, so no site of '$RowCheck' could have been found"
            bad=1
            continue
        fi
        count=0
        unnamed=0
        while IFS= read -r site; do
            [ -n "$site" ] || continue
            count=$((count + 1))
            case "$site" in
                *"#$RowIssue"*) ;;
                *)
                    echo "FAIL: $site"
                    echo "      names $RowCheck but not #$RowIssue. A site written around a known defect says so on ONE line naming both, so"
                    echo "      the change that retires the row finds every site; a line naming the check for another reason reads the same."
                    unnamed=$((unnamed + 1))
                    bad=1
                    ;;
            esac
        done <<< "$Sites"
        echo "row '$RowName' (#$RowIssue): $count site line(s) naming $RowCheck among $SourceCount source(s), $unnamed not naming #$RowIssue"
    done
    [ "$bad" -eq 0 ]
}

# $1 = the row's sites, as SitesOf found them; printed under the `fixed` remedy, which is the moment they are needed and
# the last moment this script can list them: once the row is deleted, the default mode asks nothing about its check.
ListSitesForRemedy() {
    local dir="$1"
    if ! SitesOf "$dir"; then
        echo "             (grep could not read $dir/src, exit $SitesStatus -- so list them with the default mode BEFORE step 1)"
        return
    fi
    if [ -z "$Sites" ]; then
        echo "             (none: no source line under $dir/src names $RowCheck)"
        return
    fi
    printf '%s' "$Sites" | sed 's/^/             /'
}

# The scratch directory this run planted its units in, removed. A FUNCTION because
# `CheckInstalled` now has three exits and had one: the cleanup sat at the tail, so the
# preflight refusals (#1574) would each have left a directory behind on every gate run of
# a host whose analyser cannot answer -- which is exactly the host that re-runs the gate.
# $1 = the directory.
DropWork() {
    find "$1" -depth -mindepth 1 -delete 2>/dev/null
    rmdir "$1" 2>/dev/null
    return 0
}

# $1 = the analyser. Every row's three units through it.
CheckInstalled() {
    local exe="$1" dir="$2" row work unit rc out verdict bad=0 config
    if ! command -v "$exe" >/dev/null 2>&1; then
        echo "FAIL: '$exe' is not an executable here, so no known defect can be asked about"
        return 1
    fi
    if [ "${#Rows[@]}" -eq 0 ]; then
        NoRows "to ask $exe"
        return 0
    fi
    work="$(mktemp -d "${TMPDIR:-/tmp}/clang-tidy-known-defects.XXXXXX")" || { echo "FAIL: mktemp failed"; return 1; }

    # BEFORE any row runs (#1574). A row's verdict is a claim about a named defect, and an analyser that cannot
    # compile a trivial unit produces that claim without having learned anything -- so the question "can this build
    # answer at all" is asked once, first, and answered in its own words.
    #
    # Under the FIRST row's own check, and not under `-*`: measured, `--config="{Checks: '-*'}"` makes clang-tidy
    # refuse with *"Error: no checks enabled"* and exit 1, so a preflight written that way calls every analyser
    # broken -- this instrument committing the misdiagnosis it exists to remove. A `clang-diagnostic-*` group does
    # not count as a check either. Any row's check would do; the first one is the one that certainly exists, the
    # empty table having returned above, and it is the config shape the rows themselves use.
    SplitRow "${Rows[0]}" || { echo "FAIL: malformed first row; run the default mode"; DropWork "$work"; return 1; }
    PreflightConfig="{Checks: '-*,$RowCheck', WarningsAsErrors: ''}"
    printf 'int Preflight();\nint Preflight() { return 0; }\n' > "$work/preflight-bare.cpp"
    # CAPTURED, not discarded. The bare probe's exit alone cannot say WHY, and this arm's
    # remedy used to assert one cause ("check that it is a clang-tidy") that the exit does
    # not establish: a config whose only check this build does not carry -- a row's check
    # renamed or retired upstream -- makes clang-tidy answer *"Error: no checks enabled"*
    # and exit 1, landing here. That is the row's own `not-live` finding wearing this
    # arm's sentence, which is #1574's misdiagnosis relocated rather than removed. So the
    # output is printed and the remedy names both causes.
    PreflightBareOut="$(cd "$work" && "$exe" --config="$PreflightConfig" preflight-bare.cpp -- -std=c++20 2>&1)"
    PreflightBareRc=$?
    printf '#include <algorithm>\nint Preflight();\nint Preflight() { return 0; }\n' > "$work/preflight-std.cpp"
    PreflightStdOut="$(cd "$work" && "$exe" --config="$PreflightConfig" preflight-std.cpp -- -std=c++20 2>&1)"
    PreflightStdRc=$?
    case "$(JudgePreflight "$PreflightBareRc" "$PreflightStdRc")" in
        usable) ;;
        runs-nothing)
            echo "CANNOT JUDGE: '$exe' could not analyse a trivial translation unit (exit $PreflightBareRc), so it"
            echo "      cannot be asked about any known defect and no row below would be a statement about this tree."
            echo "      NOT a finding about your branch, and not about any issue a row names: nothing here was judged."
            printf '%s\n' "$PreflightBareOut" | tail -4 | sed 's/^/      | /'
            echo "      Two causes reach this, and the lines above are what tells them apart: '$exe' is not a clang-tidy or"
            echo "      cannot run here at all, or it carries no check named '$RowCheck' -- 'Error: no checks enabled' -- in"
            echo "      which case the finding is that the row's check was renamed or retired upstream, and the row is what"
            echo "      moves, not the analyser."
            DropWork "$work"
            return "$UnusableAnalyser"
            ;;
        no-standard-library)
            echo "CANNOT JUDGE: '$exe' runs, and cannot compile '#include <algorithm>' (exit $PreflightStdRc), so it"
            echo "      ships no C++ standard library on its include path. Every row's units need one, so every row would"
            echo "      fail and each failure would read as a claim about the defect that row names (#1574)."
            echo "      NOT a finding about your branch, and not about any issue a row names: nothing here was judged."
            printf '%s\n' "$PreflightStdOut" | tail -4 | sed 's/^/      | /'
            echo "      Measured: the Windows PyPI wheel has this shape, and the same build under WSL does not -- so this"
            echo "      check answers on Linux and macOS, and on Windows only where the analyser was installed with headers."
            DropWork "$work"
            return "$UnusableAnalyser"
            ;;
    esac

    for row in ${Rows[@]+"${Rows[@]}"}; do
        SplitRow "$row" || { echo "FAIL: malformed row '${row%%|*}'; run the default mode"; bad=1; continue; }
        # A config of its own, so neither `.clang-tidy` nor WarningsAsErrors decides an outcome here.
        config="{Checks: '-*,$RowCheck', WarningsAsErrors: ''}"
        for unit in crashes named live; do
            case "$unit" in
                crashes) printf '%b\n' "$RowCrashes" > "$work/$RowName-$unit.cpp" ;;
                named) printf '%b\n' "$RowNamed" > "$work/$RowName-$unit.cpp" ;;
                live) printf '%b\n' "$RowLive" > "$work/$RowName-$unit.cpp" ;;
            esac
            # Relative to the unit's directory, so no shell rewrites a POSIX path for a Windows analyser.
            out="$(cd "$work" && "$exe" --config="$config" "$RowName-$unit.cpp" -- -std=c++20 2>&1)"
            rc=$?
            eval "Rc_$unit=\$rc"
            eval "Out_$unit=\$out"
        done
        verdict="$(Judge "$Rc_crashes" "$Out_crashes" "$Rc_named" "$Out_named" "$Rc_live" "$Out_live" "$RowCheck")"
        case "$verdict" in
            present)
                echo "ok: '$RowName' (#$RowIssue): $exe still crashes on it (exit 139, stack dump), the named unit is clean and"
                echo "    $RowCheck reports the live unit -- so the rewritten sites stay as they are"
                ;;
            fixed)
                echo "FAIL: '$RowName' (#$RowIssue) no longer crashes $exe: the declared build carries the upstream fix, so this"
                echo "      row has done its job, and THIS change -- the one that moved the pin -- is where it retires:"
                echo "        1. delete the row '$RowName' from Rows in scripts/check-clang-tidy-known-defects.sh. The table may be"
                echo "           left empty: every mode of this script passes on an empty table."
                echo "        2. delete the comment naming #$RowIssue on each line below that carries one. They are listed here"
                echo "           because the default mode stops listing them the moment the row is gone:"
                ListSitesForRemedy "$dir"
                echo "        3. keep the rewrites at those sites: they are ordinary code. There is no check to re-enable,"
                echo "           because it was never disabled."
                echo "      Nothing here needs #$RowIssue open: it documents the defect, open or closed, and the row was the record."
                echo "      All of that holds only for the DECLARED build: first see 'check-clang-tidy-version.sh --installed' accept"
                echo "      $exe. Another build proves nothing -- apt's 22.1.8 snapshot, from the same commit, reads the same null"
                echo "      pointer and survives, and this mode reports exactly this for it."
                bad=1
                ;;
            not-live)
                echo "FAIL: '$RowName' (#$RowIssue): $RowCheck did not report the live unit (exit $Rc_live), so this build is not running"
                echo "      that check on it, and whether the crashing shape still crashes says nothing. Its output:"
                printf '%s\n' "$Out_live" | tail -8 | sed 's/^/      | /'
                bad=1
                ;;
            named-broken)
                echo "FAIL: '$RowName' (#$RowIssue): the named unit, which avoids the shape, did not analyse cleanly (exit $Rc_named),"
                echo "      so the planted units do not parse on this host and the crashing one cannot be read. Its output:"
                printf '%s\n' "$Out_named" | tail -8 | sed 's/^/      | /'
                bad=1
                ;;
            unknown)
                echo "FAIL: '$RowName' (#$RowIssue): the crashing unit neither crashed as recorded (exit 139 with a stack dump) nor"
                echo "      analysed cleanly -- exit $Rc_crashes. That is a different failure, and it says nothing about the defect:"
                printf '%s\n' "$Out_crashes" | tail -8 | sed 's/^/      | /'
                bad=1
                ;;
        esac
    done
    DropWork "$work"
    [ "$bad" -eq 0 ]
}

# --- self-test -----------------------------------------------------------------
if [ "$Mode" = "self-test" ]; then
    Work="$(mktemp -d "${TMPDIR:-/tmp}/clang-tidy-known-defects-selftest.XXXXXX")" || {
        echo "FAIL: mktemp failed" >&2
        exit "$UsageError"
    }
    Cleanup() { find "$Work" -depth -mindepth 1 -delete 2>/dev/null; rmdir "$Work" 2>/dev/null; }
    trap Cleanup EXIT
    Cases=0
    Failed=0
    Self="$0"

    # A SAMPLE row, never the shipped table's first: the shipped table may hold no rows at all -- the change that
    # retires the last defect leaves it that way -- and a self-test built on it would then have nothing to build from.
    # Its units carry markers the stub analysers below recognise, and nothing a real analyser would parse.
    SampleRow="sample-defect|sample-known-defect-check|4242|int Crashes() { return CRASH_MARKER; }|int Named() { return 0; }|int Live() { return LIVE_MARKER; }"
    SplitRow "$SampleRow" || { echo "FAIL: the sample row is malformed, so no case can be built from it"; exit 1; }
    Dump=$'PLEASE submit a bug report\nStack dump:\n0.\tProgram arguments: clang-tidy'
    Warning="x.cpp:3:70: warning: do not use nested 'std::max' calls, use an initializer list instead [$RowCheck]"

    # $1 = label, $2 = wanted verdict, rest = Judge's arguments.
    # The preflight decision, driven the same way and for the same reason (#1574): three outcomes, each reached
    # without an analyser.
    DecidesPreflight() {
        local label="$1" want="$2" got
        shift 2
        Cases=$((Cases + 1))
        got="$(JudgePreflight "$@")"
        if [ "$got" = "$want" ]; then
            echo "ok: self-test '$label' ($want)"
        else
            echo "FAIL: self-test '$label': wanted $want, got $got"
            Failed=$((Failed + 1))
        fi
    }
    DecidesPreflight "preflight: an analyser that parses both units is usable" usable 0 0
    DecidesPreflight "preflight: one that cannot parse a bare unit runs nothing" runs-nothing 1 0
    # The order matters and is asserted: a build that fails BOTH is reported as running nothing, which is the
    # broader fact, rather than as one missing a standard library.
    DecidesPreflight "preflight: one that fails both is reported as running nothing" runs-nothing 1 1
    DecidesPreflight "preflight: one that parses a bare unit and not an include has no standard library" \
        no-standard-library 0 1

    Decides() {
        local label="$1" want="$2" got
        shift 2
        Cases=$((Cases + 1))
        got="$(Judge "$@")"
        if [ "$got" = "$want" ]; then
            echo "ok: self-test '$label' ($want)"
        else
            echo "FAIL: self-test '$label': wanted $want, got $got"
            Failed=$((Failed + 1))
        fi
    }
    Decides "today's build: crashes, named clean, live reported" present 139 "$Dump" 0 "" 0 "$Warning" "$RowCheck"
    Decides "a build carrying the fix" fixed 0 "" 0 "" 0 "$Warning" "$RowCheck"
    Decides "the check is not running: nothing reported on the live unit" not-live 0 "" 0 "" 0 "" "$RowCheck"
    Decides "the check is not running, even though the crashing unit is clean" not-live 0 "" 0 "" 0 "unrelated [other-check]" "$RowCheck"
    Decides "the live unit crashes too" not-live 139 "$Dump" 0 "" 139 "$Dump $Warning" "$RowCheck"
    Decides "the named unit does not parse here" named-broken 139 "$Dump" 1 "error: 'algorithm' file not found" 0 "$Warning" "$RowCheck"
    Decides "the named unit crashes as well" named-broken 139 "$Dump" 139 "$Dump" 0 "$Warning" "$RowCheck"
    Decides "exit 139 with no stack dump is not the recorded crash" unknown 139 "" 0 "" 0 "$Warning" "$RowCheck"
    Decides "a stack dump under another status is not the recorded crash" unknown 134 "$Dump" 0 "" 0 "$Warning" "$RowCheck"
    Decides "a parse error on the crashing unit alone" unknown 1 "error: expected ';'" 0 "" 0 "$Warning" "$RowCheck"
    Decides "a clean exit that still printed a stack dump" unknown 0 "$Dump" 0 "" 0 "$Warning" "$RowCheck"

    # $1 = label, $2 = wanted status of SplitRow (0 or 1), $3 = the row.
    Splits() {
        local label="$1" want="$2" got=0
        Cases=$((Cases + 1))
        SplitRow "$3" || got=1
        if [ "$got" = "$want" ]; then
            echo "ok: self-test '$label' (status $want)"
        else
            echo "FAIL: self-test '$label': wanted status $want, got $got"
            Failed=$((Failed + 1))
        fi
    }
    Splits "a well-formed row" 0 "name|some-check|12|a|b|c"
    Splits "a row one field short, whose last field would stand in for the missing one" 1 "name|some-check|12|a|b"
    Splits "a row with a field too many" 1 "name|some-check|12|a|b|c|d"
    Splits "a row whose issue is not a number" 1 "name|some-check|#12|a|b|c"
    Splits "a row whose check is not a check name" 1 "name|Some Check|12|a|b|c"
    Splits "a row with an empty unit" 1 "name|some-check|12|a||c"
    # The sample is split again last, so the cases above cannot leave another row's fields behind.
    SplitRow "$SampleRow"

    # A COPY of this script whose table holds exactly the rows given -- none for an empty table, which is the table
    # the change retiring the last defect leaves. The whole `Rows=(` ... `)` block is replaced, and a copy in which it
    # was not replaced exactly once is refused: one that kept the shipped rows would be testing those instead.
    # $1 = the copy's path, rest = its rows (no `"` in any).
    WithRows() {
        local copy="$1" block="Rows=("$'\n'
        shift
        while [ $# -gt 0 ]; do
            block="$block    \"$1\""$'\n'
            shift
        done
        block="$block)"
        if ! RowsBlock="$block" awk '
            /^Rows=\($/ { print ENVIRON["RowsBlock"]; inBlock = 1; replaced++; next }
            inBlock && /^\)$/ { inBlock = 0; next }
            inBlock { next }
            { print }
            END { if (replaced != 1 || inBlock) exit 1 }
        ' "$Self" > "$copy"; then
            echo "FAIL: the Rows=( ... ) block of $Self was not replaced exactly once in $copy, so no case can use it"
            exit 1
        fi
    }
    WithRows "$Work/sample.sh" "$SampleRow"
    WithRows "$Work/malformed.sh" "malformed|Not A Check|12|a|b|c"
    WithRows "$Work/empty.sh"

    # $1 = label, $2 = the script to run, $3 = want (pass|refuse), $4 = fragments the output must hold, one per line,
    # $5 = a fragment it must NOT hold ("-" for none), rest = arguments to that script.
    Expect() {
        local label="$1" script="$2" want="$3" fragments="$4" absent="$5" output status got fragment missing=""
        shift 5
        Cases=$((Cases + 1))
        output="$("${BASH:-bash}" "$script" "$@" 2>&1)"
        status=$?
        got="pass"
        [ "$status" -eq 0 ] || got="refuse"
        if [ "$got" != "$want" ]; then
            echo "FAIL: self-test '$label': wanted $want, got $got (exit $status)"
            printf '%s\n' "$output" | sed 's/^/      | /'
            Failed=$((Failed + 1))
            return
        fi
        while IFS= read -r fragment; do
            [ -n "$fragment" ] || continue
            case "$output" in
                *"$fragment"*) ;;
                *) missing="$missing '$fragment'" ;;
            esac
        done <<< "$fragments"
        if [ -n "$missing" ]; then
            echo "FAIL: self-test '$label': $want as wanted, but the output lacks$missing"
            printf '%s\n' "$output" | sed 's/^/      | /'
            Failed=$((Failed + 1))
            return
        fi
        if [ "$absent" != "-" ]; then
            case "$output" in
                *"$absent"*)
                    echo "FAIL: self-test '$label': $want as wanted, but the output holds '$absent', which it must not"
                    printf '%s\n' "$output" | sed 's/^/      | /'
                    Failed=$((Failed + 1))
                    return
                    ;;
            esac
        fi
        echo "ok: self-test '$label' ($want)"
    }

    # The EXIT STATUS, which `Expect` cannot see: it folds every non-zero into the one word `refuse`, so a run that
    # answered 1 and a run that answered `$UnusableAnalyser` are the same case to it. That matters because the status
    # is a contract with a SECOND file -- `local-gate.sh` keys a whole outcome on this number, and reads anything else
    # as "a row changed status", which is the confident wrong cause #1574 exists to remove. A constant has two facts,
    # its name and its value; `Expect` tests only the first.
    # $1 = label, $2 = the status wanted, $3 = the script, rest = its arguments.
    ExitsWith() {
        local label="$1" want="$2" script="$3" status
        shift 3
        Cases=$((Cases + 1))
        "${BASH:-bash}" "$script" "$@" >/dev/null 2>&1
        status=$?
        if [ "$status" -eq "$want" ]; then
            echo "ok: self-test '$label' (exit $want)"
        else
            echo "FAIL: self-test '$label': wanted exit $want, got $status"
            Failed=$((Failed + 1))
        fi
    }

    # Synthetic trees for the default mode. $1 = dir, $2 = the issue an `## Open work` entry leads (empty: an entry
    # for another issue only), rest = src/x.cpp lines (none: no source at all). The rulebook is written only so that
    # the "no Open work entry" case below can differ from its neighbour in that one fact.
    Tree() {
        local dir="$1" issue="$2"
        shift 2
        mkdir -p "$dir/.agent/rules" "$dir/src"
        {
            printf '# Build\n\n## Open work\n\n'
            if [ -n "$issue" ]; then
                printf -- '- **[#%s](https://example.invalid/%s)** -- a known defect.\n' "$issue" "$issue"
            fi
            printf -- '- **[#1](https://example.invalid/1)** -- something else.\n'
        } > "$dir/.agent/rules/build-and-toolchain.md"
        if [ $# -gt 0 ]; then
            printf '%s\n' "$@" > "$dir/src/x.cpp"
        fi
    }
    SiteComment="// Named first: $RowCheck crashes on a pointer call inside the list (#$RowIssue)."
    Tree "$Work/good" "$RowIssue" "int a;" "$SiteComment"

    # Stub analysers: they read the unit they are handed and answer the way a build with or without the defect does.
    # Each also refuses a config that does not name the row's check, so a run that passed none cannot look like one.
    # $1 = path, $2 = whether the defect is present (yes|no)
    Stub() {
        {
            echo '#!/usr/bin/env bash'
            echo 'config="" unit=""'
            echo 'for a in "$@"; do case "$a" in --config=*) config="$a" ;; *.cpp) unit="$a" ;; esac; done'
            echo "case \"\$config\" in *\"$RowCheck\"*) ;; *) echo \"stub: no config naming the check\" >&2; exit 3 ;; esac"
            echo '[ -f "$unit" ] || { echo "stub: no unit" >&2; exit 3; }'
            echo 'text="$(cat "$unit")"'
            echo 'case "$text" in'
            if [ "$2" = yes ]; then
                echo '    *CRASH_MARKER*) echo "PLEASE submit a bug report"; echo "Stack dump:"; exit 139 ;;'
            fi
            echo "    *LIVE_MARKER*) echo \"\$unit:1:25: warning: do not use nested calls [$RowCheck]\"; exit 0 ;;"
            echo 'esac'
            echo 'exit 0'
        } > "$1"
        chmod +x "$1"
    }
    Stub "$Work/defective" yes
    Stub "$Work/fixed" no
    printf '#!/usr/bin/env bash\necho "error: no such file" >&2\nexit 1\n' > "$Work/broken"
    chmod +x "$Work/broken"
    # The shape #1574 came from: the analyser RUNS and has no C++ standard library, so a unit with no include
    # passes and one with an include does not. A stub that failed everything (`broken`, above) cannot model it --
    # which is why the only case here for years asserted the wrong diagnosis.
    {
        echo '#!/usr/bin/env bash'
        echo 'unit=""; for a in "$@"; do case "$a" in *.cpp) unit="$a" ;; esac; done'
        echo '[ -f "$unit" ] || { echo "stub: no unit" >&2; exit 3; }'
        echo 'case "$(cat "$unit")" in'
        echo '    *"#include"*) echo "$unit:1:10: error: '"'"'algorithm'"'"' file not found [clang-diagnostic-error]"; echo "Found compiler error(s)."; exit 1 ;;'
        echo 'esac'
        echo 'exit 0'
    } > "$Work/nostdlib"
    chmod +x "$Work/nostdlib"

    # --installed, over the sample table.
    Expect "--installed: a build with the defect" "$Work/sample.sh" pass "still crashes on it" - \
        --installed "$Work/defective" "$Work/good"
    # The remedy is read as its reader will act on it: it names the ROW to delete, LISTS the site lines -- the default
    # mode cannot, once the row is gone -- says the rewrites stay, and no longer sends anybody to a rulebook entry.
    Expect "--installed: a build carrying the fix names the row and the sites the retiring change edits" \
        "$Work/sample.sh" refuse "delete the row 'sample-defect' from Rows
delete the comment naming #$RowIssue
src/x.cpp:2:$SiteComment
keep the rewrites at those sites
Nothing here needs #$RowIssue open" "Open work" --installed "$Work/fixed" "$Work/good"
    # #1574. This case asserted "did not report the live unit" -- a claim about the ROW's defect -- for an analyser
    # that had answered nothing at all, so the self-test pinned the misdiagnosis as correct. The needle now names
    # the analyser, and the absent-needle is what makes the case discriminate: the old sentence must be GONE, or a
    # preflight that reported the environment and then ran the rows anyway would still pass.
    Expect "--installed: an analyser that cannot parse anything names the ANALYSER, not a row's defect" \
        "$Work/sample.sh" refuse "could not analyse a trivial translation unit" "did not report the live unit" \
        --installed "$Work/broken" "$Work/good"
    # The shape the ticket came from, which no stub here could previously model.
    Expect "--installed: an analyser with no standard library says so, and names no row" \
        "$Work/sample.sh" refuse "ships no C++" "did not report the live unit" \
        --installed "$Work/nostdlib" "$Work/good"
    # And the STATUS, for both, because `local-gate.sh` routes on it (#1574). A renumbered `UnusableAnalyser` would
    # keep every `Expect` above green while the gate fell to its `*)` arm and blamed the tree.
    ExitsWith "--installed: an unusable analyser exits with UnusableAnalyser, not 1" "$UnusableAnalyser" \
        "$Work/sample.sh" --installed "$Work/broken" "$Work/good"
    ExitsWith "--installed: so does one with no standard library" "$UnusableAnalyser" \
        "$Work/sample.sh" --installed "$Work/nostdlib" "$Work/good"
    # The DISCRIMINATION: a row whose status moved is a fact about the tree and must NOT arrive on that status, or the
    # gate would call a real finding a host problem and say nothing about the branch.
    ExitsWith "--installed: a build carrying the fix exits 1, not UnusableAnalyser" 1 \
        "$Work/sample.sh" --installed "$Work/fixed" "$Work/good"
    Expect "--installed: no such analyser" "$Work/sample.sh" refuse "is not an executable here" - \
        --installed "$Work/absent" "$Work/good"

    # The PREFIX, which is remedy text and which nothing above can see: `Expect`'s verdict folds every non-zero into
    # `refuse` and `ExitsWith` reads only the number, so both stay green whatever the first word of the message is.
    # It matters because the two populations arrive on the same red and are read by different people: a statement
    # about the CHECK ("this analyser cannot be asked") must not wear the same prefix as one about the SUBJECT ("this
    # row's defect changed status"), or a reader takes the first for the second and goes looking in the tree. That is
    # #1579, where a refusal accurate about its own limit sent somebody to the wrong file twice.
    Expect "--installed: an unusable analyser says CANNOT JUDGE, never FAIL"         "$Work/sample.sh" refuse "CANNOT JUDGE:" "FAIL:" --installed "$Work/broken" "$Work/good"
    Expect "--installed: and so does one with no standard library"         "$Work/sample.sh" refuse "CANNOT JUDGE:" "FAIL:" --installed "$Work/nostdlib" "$Work/good"
    # The POSITIVE CONTROL, and the case that makes the two above mean anything: a row whose status really moved is a
    # finding about the tree and keeps `FAIL:`. Without this, relabelling EVERY arm `CANNOT JUDGE:` would pass both.
    Expect "--installed: a row whose status moved keeps FAIL, so the prefixes discriminate"         "$Work/sample.sh" refuse "FAIL:" "CANNOT JUDGE:" --installed "$Work/fixed" "$Work/good"

    # The default mode, over the sample table.
    Expect "a documented defect with one commented site" "$Work/sample.sh" pass "1 site line(s) naming" - "$Work/good"
    Tree "$Work/nosites" "$RowIssue" "int a;"
    Expect "a documented defect with no site left" "$Work/sample.sh" pass "0 site line(s) naming" - "$Work/nosites"
    # The PASSING direction of the rule this change removed: a row is its own record, so its issue need not lead an
    # Open work entry -- nor be open, which only `rulebook-open-work` could have asked. These two differ from the
    # tree above in that fact alone; with the old rule back, they are the cases that go red.
    Tree "$Work/untracked" "" "int a;" "$SiteComment"
    Expect "a row whose issue leads no Open work entry passes" "$Work/sample.sh" pass "1 site line(s) naming" - \
        "$Work/untracked"
    mkdir -p "$Work/norulebook/src"
    printf '%s\n' "$SiteComment" > "$Work/norulebook/src/x.cpp"
    Expect "a tree with no rulebook at all passes" "$Work/sample.sh" pass "0 not naming #$RowIssue" - "$Work/norulebook"
    Tree "$Work/bare" "$RowIssue" "// $RowCheck crashes here, so the width is named first."
    Expect "a site naming the check without the issue" "$Work/sample.sh" refuse "names $RowCheck but not #$RowIssue" - "$Work/bare"
    Tree "$Work/other" "$RowIssue" "// $RowCheck crashes here (#1)."
    Expect "a site naming another issue" "$Work/sample.sh" refuse "names $RowCheck but not #$RowIssue" - "$Work/other"
    Tree "$Work/empty" "$RowIssue"
    Expect "no source at all" "$Work/sample.sh" refuse "no C++ source under" - "$Work/empty"
    Expect "a malformed row" "$Work/malformed.sh" refuse "malformed row 'malformed'" - "$Work/good"

    # An EMPTY table, which is what deleting the last row leaves: every mode passes, and says why.
    Expect "an empty table: the default mode" "$Work/empty.sh" pass "has no rows, so there is nothing" - "$Work/good"
    Expect "an empty table: the default mode over this tree" "$Work/empty.sh" pass "has no rows" - "$SourceDir"
    Expect "an empty table: --installed" "$Work/empty.sh" pass "nothing to ask $Work/defective" - \
        --installed "$Work/defective" "$Work/good"
    Expect "an empty table: --installed still refuses an analyser that is not there" "$Work/empty.sh" refuse \
        "is not an executable here" - --installed "$Work/absent" "$Work/good"
    # The empty copy's own self-test, once: nested, it skips this case, or each copy would run the next forever.
    if [ -z "${FASTCACHED_KNOWN_DEFECTS_NESTED:-}" ]; then
        FASTCACHED_KNOWN_DEFECTS_NESTED=1
        export FASTCACHED_KNOWN_DEFECTS_NESTED
        Expect "an empty table: --self-test" "$Work/empty.sh" pass "self-test: " - --self-test "$SourceDir"
    else
        echo "self-test: nested inside another self-test, so the empty table's own self-test is not run again from here"
    fi

    # The shipped table over the shipped tree, last. When it is empty, the line to expect is the empty table's.
    if [ "${#Rows[@]}" -eq 0 ]; then
        Expect "the shipped tree" "$Self" pass "has no rows" - "$SourceDir"
    else
        Expect "the shipped tree" "$Self" pass ", 0 not naming #" - "$SourceDir"
    fi

    echo "self-test: $Cases case(s) ran, $Failed failed"
    [ "$Cases" -gt 0 ] || { echo "FAIL: no self-test case ran"; exit 1; }
    [ "$Failed" -eq 0 ]
    exit $?
fi

case "$Mode" in
    static) CheckStatic "$SourceDir" ;;
    installed) CheckInstalled "$Argument" "$SourceDir" ;;
esac
