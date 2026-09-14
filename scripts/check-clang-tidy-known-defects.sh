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
# may come out. So each defect is a ROW below, carrying the check it lives in, the issue tracking it, and three units
# planted and run through the declared build:
#
#   crashes   the shape itself, which the build must STILL crash on: exit 139 and a stack dump. Measured on the Linux
#             wheel and on the Windows wheels under Git Bash, which reports the access violation as 139 too.
#   named     the same computation with the offending call named first, which must analyse cleanly -- so the unit
#             parses on this host, and a crash in `crashes` belongs to the shape rather than to a header search.
#   live      a shape the check reports, which it must report -- so the check is enabled and running in this build,
#             and `crashes` not crashing can mean only that the defect is gone.
#
# When the pin moves to an LLVM carrying the upstream fix, `crashes` stops crashing and `--installed` goes red, naming
# what the change that makes it green removes: the row, the site comments naming its issue, and the rulebook entry.
#
# ## Modes
#
#   (default)           Every row is well formed; its issue leads an entry in the `## Open work` section of
#                       `.agent/rules/build-and-toolchain.md`; and every source line under `src/` that names a row's
#                       check also names that row's issue. A site comment is one line for exactly that reason. Needs
#                       no analyser, runs in the default ctest set.
#   --installed <exe>   Run every row's three units through <exe> and require the outcomes above. Run by both
#                       clang-tidy jobs after they identify the install, and by local-gate.sh after it resolves the
#                       analyser. Not a ctest: no build leg has the analyser, so it would skip everywhere (#1135).
#   --self-test         Drive the decision over every outcome, `--installed` end to end over stub analysers, and the
#                       default mode over synthetic trees.
#
# bash 3.2: the default and self-test modes run in ctest on macOS.
#
# Usage:
#   bash scripts/check-clang-tidy-known-defects.sh [<source-dir>]
#   bash scripts/check-clang-tidy-known-defects.sh --installed <exe> [<source-dir>]
#   bash scripts/check-clang-tidy-known-defects.sh --self-test [<source-dir>]

set -uo pipefail

UsageError=2

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

CheckStatic() {
    local dir="$1" rulebook="$1/.agent/rules/build-and-toolchain.md" row openWork site bad=0 count unnamed
    if [ ! -f "$rulebook" ]; then
        echo "FAIL: $rulebook does not exist, so no row's issue can be found in its Open work"
        return 1
    fi
    # The section from its heading to the next `## ` heading.
    openWork="$(sed -n '/^## Open work$/,/^## /p' "$rulebook")"
    if [ -z "$openWork" ]; then
        echo "FAIL: $rulebook has no '## Open work' section, so no row's issue can be found in it"
        return 1
    fi
    [ "${#Rows[@]}" -gt 0 ] || { echo "FAIL: the defect table is empty, which this check cannot tell from a table it failed to read"; return 1; }
    for row in "${Rows[@]}"; do
        if ! SplitRow "$row"; then
            echo "FAIL: malformed row '${row%%|*}': want name|check|issue|crashes|named|live, a check name, a numeric issue"
            bad=1
            continue
        fi
        case $'\n'"$openWork" in
            *$'\n'"- **[#$RowIssue]("*) ;;
            *)
                echo "FAIL: row '$RowName' is tracked by #$RowIssue, which leads no entry in the '## Open work' section of"
                echo "      $rulebook. A known defect the tree works around is open work until the pinned build carries the fix."
                bad=1
                ;;
        esac
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

# $1 = the analyser. Every row's three units through it.
CheckInstalled() {
    local exe="$1" dir="$2" row work unit rc out verdict bad=0 config
    if ! command -v "$exe" >/dev/null 2>&1; then
        echo "FAIL: '$exe' is not an executable here, so no known defect can be asked about"
        return 1
    fi
    work="$(mktemp -d "${TMPDIR:-/tmp}/clang-tidy-known-defects.XXXXXX")" || { echo "FAIL: mktemp failed"; return 1; }
    for row in "${Rows[@]}"; do
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
                echo "FAIL: '$RowName' (#$RowIssue) no longer crashes $exe: the declared build carries the upstream fix."
                echo "      This is the change that retires #$RowIssue. In it: delete this row, remove the comment naming #$RowIssue at"
                echo "      each site the default mode lists, remove its '## Open work' entry in .agent/rules/build-and-toolchain.md,"
                echo "      and close the issue. The rewrites may stay, they are ordinary code; and there is nothing to re-enable,"
                echo "      because the check was never disabled."
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
    find "$work" -depth -mindepth 1 -delete 2>/dev/null
    rmdir "$work" 2>/dev/null
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
    SplitRow "${Rows[0]}" || { echo "FAIL: the first row is malformed, so no case can be built from it"; exit 1; }
    Dump=$'PLEASE submit a bug report\nStack dump:\n0.\tProgram arguments: clang-tidy'
    Warning="x.cpp:3:70: warning: do not use nested 'std::max' calls, use an initializer list instead [$RowCheck]"

    # $1 = label, $2 = wanted verdict, rest = Judge's arguments.
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
    # The shipped rows are split again last, so the cases above cannot leave another row's fields behind.
    SplitRow "${Rows[0]}"

    # $1 = label, $2 = want (pass|refuse), $3 = required output fragment, rest = arguments to this script.
    Expect() {
        local label="$1" want="$2" fragment="$3" output status got
        shift 3
        Cases=$((Cases + 1))
        output="$("${BASH:-bash}" "$Self" "$@" 2>&1)"
        status=$?
        got="pass"
        [ "$status" -eq 0 ] || got="refuse"
        if [ "$got" != "$want" ]; then
            echo "FAIL: self-test '$label': wanted $want, got $got (exit $status)"
            printf '%s\n' "$output" | sed 's/^/      | /'
            Failed=$((Failed + 1))
            return
        fi
        case "$output" in
            *"$fragment"*) echo "ok: self-test '$label' ($want)" ;;
            *)
                echo "FAIL: self-test '$label': $want as wanted, but the output lacks '$fragment'"
                printf '%s\n' "$output" | sed 's/^/      | /'
                Failed=$((Failed + 1))
                ;;
        esac
    }

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
                echo '    *"width(1) })"*) echo "PLEASE submit a bug report"; echo "Stack dump:"; exit 139 ;;'
            fi
            echo "    *\"std::max(a, std::max\"*) echo \"\$unit:3:70: warning: do not use nested 'std::max' calls [$RowCheck]\"; exit 0 ;;"
            echo 'esac'
            echo 'exit 0'
        } > "$1"
        chmod +x "$1"
    }
    Stub "$Work/defective" yes
    Stub "$Work/fixed" no
    printf '#!/usr/bin/env bash\necho "error: no such file" >&2\nexit 1\n' > "$Work/broken"
    chmod +x "$Work/broken"
    Expect "--installed: a build with the defect" pass "still crashes on it" --installed "$Work/defective" "$SourceDir"
    Expect "--installed: a build carrying the fix names what the retiring change removes" refuse \
        "remove the comment naming #$RowIssue at" --installed "$Work/fixed" "$SourceDir"
    Expect "--installed: an analyser that cannot parse anything" refuse "did not report the live unit" \
        --installed "$Work/broken" "$SourceDir"
    Expect "--installed: no such analyser" refuse "is not an executable here" --installed "$Work/absent" "$SourceDir"

    # Synthetic trees for the default mode. $1 = dir, $2 = Open work lead issue (empty: none), rest = src/x.cpp lines.
    Tree() {
        local dir="$1" issue="$2"
        shift 2
        mkdir -p "$dir/.agent/rules" "$dir/src"
        {
            echo "# Build"
            echo
            echo "## Open work"
            echo
            if [ -n "$issue" ]; then
                echo "- **[#$issue](https://example.invalid/$issue)** -- a known defect."
            fi
            echo "- **[#1](https://example.invalid/1)** -- something else."
            echo
            echo "## Appendix"
        } > "$dir/.agent/rules/build-and-toolchain.md"
        if [ $# -gt 0 ]; then
            printf '%s\n' "$@" > "$dir/src/x.cpp"
        fi
    }
    Tree "$Work/good" "$RowIssue" "int a;" "// Named first: $RowCheck crashes on a pointer call inside the list (#$RowIssue)."
    Expect "a tracked defect with one commented site" pass "1 site line(s) naming" "$Work/good"
    Tree "$Work/nosites" "$RowIssue" "int a;"
    Expect "a tracked defect with no site left" pass "0 site line(s) naming" "$Work/nosites"
    Tree "$Work/untracked" "" "int a;"
    Expect "a row whose issue leads no Open work entry" refuse "leads no entry" "$Work/untracked"
    Tree "$Work/inprose" "" "int a;"
    printf '%s\n' "- **[#1](x)** -- mentions #$RowIssue only in passing." >> "$Work/inprose/.agent/rules/build-and-toolchain.md"
    Expect "an issue cited in prose, not leading an entry" refuse "leads no entry" "$Work/inprose"
    Tree "$Work/elsewhere" "" "int a;"
    printf '\n## Other\n\n- **[#%s](x)** -- in the wrong section.\n' "$RowIssue" >> "$Work/elsewhere/.agent/rules/build-and-toolchain.md"
    Expect "an issue leading an entry outside Open work" refuse "leads no entry" "$Work/elsewhere"
    Tree "$Work/bare" "$RowIssue" "// $RowCheck crashes here, so the width is named first."
    Expect "a site naming the check without the issue" refuse "names $RowCheck but not #$RowIssue" "$Work/bare"
    Tree "$Work/other" "$RowIssue" "// $RowCheck crashes here (#1)."
    Expect "a site naming another issue" refuse "names $RowCheck but not #$RowIssue" "$Work/other"
    Tree "$Work/empty" "$RowIssue"
    Expect "no source at all" refuse "no C++ source under" "$Work/empty"
    mkdir -p "$Work/norulebook/src"
    printf 'int a;\n' > "$Work/norulebook/src/x.cpp"
    Expect "no rulebook" refuse "does not exist" "$Work/norulebook"

    Expect "the shipped tree" pass "0 not naming #$RowIssue" "$SourceDir"

    echo "self-test: $Cases case(s) ran, $Failed failed"
    [ "$Cases" -gt 0 ] || { echo "FAIL: no self-test case ran"; exit 1; }
    [ "$Failed" -eq 0 ]
    exit $?
fi

case "$Mode" in
    static) CheckStatic "$SourceDir" ;;
    installed) CheckInstalled "$Argument" "$SourceDir" ;;
esac
