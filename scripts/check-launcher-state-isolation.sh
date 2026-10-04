#!/usr/bin/env bash
# Every fixture handed a launcher runs it against a state directory of the run's own.
#
# The launcher records every invocation in `<state>/fastcache-cc/invocations.log`, and
# `-z` deletes that log. `<state>` is the DEVELOPER's -- `%LOCALAPPDATA%`, or
# `$XDG_STATE_HOME` / `$HOME/.local/state` -- unless the fixture says otherwise, so a
# fixture that does not either deletes their statistics or appends its own records to
# them. The Windows launcher e2e did the first on every run and three fixtures did the
# second, and none of them looked wrong from inside the fixture: the result was right,
# only somebody else's file had changed.
#
# The seam that prevents it is `e2e_launcher_state_enter` (scripts/lib/e2e-common.sh) and
# `Enter-E2ELauncherState` (scripts/lib/E2EEnvironment.psm1). It only protects a fixture
# that calls it -- and, in bash, calls it in order, after `e2e_begin` and before the
# launcher first runs -- so this is the scan that says which ones do. Each `.ps1` fixture
# ALSO scans its own launcher executions
# at startup (`Assert-E2ELauncherFixture`); this is the half that needs no fixture to run,
# and that sees a fixture which never imports the module at all.
#
# With `--self-test` (the `launcher-state-isolation-selftest` registration) it instead
# proves it can fail, against a planted tree, and DRIVES both seams, against a stand-in
# launcher and a stand-in caller whose log is present, empty and ABSENT -- the last being
# every CI runner, and the state a review found ended every `set -e` bash fixture without
# a word while every sentinel here was non-empty.
#
# In the default set, on every platform: it reads files, and the `.ps1` fixtures it is
# mostly about only ever run on Windows. Not part of `e2e-helpers-selftest`, which is not
# RUN on Windows. The PowerShell half needs pwsh and is a named skip without it.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
set -uo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
library="${source_dir}/scripts/lib/e2e-common.sh"
psmodule="${source_dir}/scripts/lib/E2EEnvironment.psm1"
# shellcheck source=lib/third-party-roots.sh
. "${source_dir}/scripts/lib/third-party-roots.sh" \
    || { echo "FAIL: cannot read scripts/lib/third-party-roots.sh" >&2; exit 1; }

# Two registrations, one script: the REAL tree and the real Stats.cpp by default, and
# with `--self-test` the check's own proof that it can fail -- the planted canary and both
# seams driven against a stand-in launcher. Split because they are separate questions
# and, on a saturated host (ctest -j at the core count), the self-test phases alone were
# most of the time bound: each mode keeps its own TIMEOUT 120 rather than one raised one.
LauncherMode=tree
LauncherLabel="launcher-state-isolation"
case "${1:-}" in
    "") ;;
    --self-test) LauncherMode=self-test; LauncherLabel="launcher-state-isolation --self-test" ;;
    *) echo "usage: check-launcher-state-isolation.sh [--self-test]" >&2; exit 2 ;;
esac

ran=0
skipped=0
failures=0
failed_cases=""
# @param 1 The case name, matching the `FAIL <name>:` line beside the call.
note_failure() {
    failures=$(( failures + 1 ))
    case " ${failed_cases} " in
        *" $1 "*) ;;
        *) failed_cases="${failed_cases}${failed_cases:+ }$1" ;;
    esac
}

# A path pwsh can open: under Git Bash that is the Windows spelling of a `/d/...` path.
_launcher_native_path() { if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi; }
have_pwsh=no
command -v pwsh >/dev/null 2>&1 && have_pwsh=yes

# --- the scan ------------------------------------------------------------------
#
# A FIXTURE is anything the build hands a launcher: a script named by an `add_test` that
# passes `$<TARGET_FILE:fastcache-cc>` or any other `$<TARGET_FILE...:fastcache-cc>` --
# directly, or through a `set()` variable carrying it, to any depth and across files -- and
# any tracked script that declares a launcher parameter (`--launcher)` in a `.sh`,
# `[string]$Launcher` in a `.ps1`), registered or not. Each must call its language's seam,
# and a `.sh` fixture must keep its launcher variable to the seam's order
# (`_launcher_seam_orders`): not named above `e2e_begin`, only tested, messaged or
# rewritten -- never inside a command substitution -- between it and the seam, and no EXIT
# trap installed after the seam.
#
# Fails CLOSED on what it cannot classify: a launcher reference -- or a variable carrying
# one -- outside an `add_test`/`set` it can read, a block it cannot close, a registration
# naming no script, a registered script that declares no launcher parameter, a fixture in a
# language with no seam, a bash fixture calling no `e2e_begin`, a seam call naming no
# variable, and a variable never used after it.
#
# Its blind spots, failing OPEN: a fixture that runs the launcher without naming its
# variable -- by a path it re-derived or took from the command line a second time, through
# `${!name}` indirection, or from a function of a sourced library; and a launcher a
# registration reaches without naming `$<TARGET_FILE...:fastcache-cc>` or a variable
# carrying it -- a path spelled from the build tree's layout, or a `find_program` of its
# own. The seam's own checks -- a read-only
# guard, a positive control, and a caller-log check on every exit -- catch the first, per
# fixture, at run time; nothing here catches the second. One rule fails CLOSED where it
# could be wrong: in bash, a function defined BEFORE the seam that runs the launcher when
# called later reads as a use before the seam, and is refused.

# Scripts a registration names BESIDE the fixture it runs. `path:reason`, one per line.
LauncherFixtureRunners="scripts/run-check.sh:runs the check script named after it, and that script is the one judged"

# What reaching the launcher looks like in CMake: its file, or its directory.
LauncherTargetPattern='TARGET_FILE[A-Z_]*:fastcache-cc>'

# Run a command over every file in a newline-separated list, as trailing arguments, from a
# root; nothing when the list is empty. ONE process for the whole list, not one per file:
# a spawn is what this check costs under Git Bash, where a per-file `grep` over the tree's
# CMake files was 17s of a 44s run. Its exit status is not an answer and is not returned:
# `grep -l` reports "no file matched" as 1.
# @param 1 A repository root.
# @param 2 Repository-relative files, one per line.
# @param ... the command
_launcher_over() {
    local root="$1" list="$2"
    shift 2
    list="$(printf '%s\n' "$list" | awk 'NF')" || true
    [ -n "$list" ] || return 0
    ( cd "$root" && printf '%s\n' "$list" | tr '\n' '\000' | xargs -0 "$@" ) || true
}

# The files in @p 2 that mention the launcher, or `${VAR}` for a VAR in @p 3.
# @param 1 A repository root.
# @param 2 Repository-relative CMake files, one per line.
# @param 3 Carrier variables, one per line.
_launcher_mentioning() {
    local pattern="$LauncherTargetPattern" carrier
    while IFS= read -r carrier; do
        [ -n "$carrier" ] && pattern="${pattern}|\\\$\\{${carrier}\\}"
    done <<< "$3"
    _launcher_over "$1" "$2" grep -lE -e "$pattern" --
}

# The variables that carry the launcher, from @p 2's CMake files, to a fixpoint: a set()
# naming the launcher, or naming a variable already known to carry it, in any file. One
# `grep` and one `awk` per pass over every file that mentions either.
# @param 1 A repository root.
# @param 2 Repository-relative CMake files, one per line.
_launcher_carriers() {
    local root="$1" cmake="$2" carriers="" next files
    while :; do
        files="$(_launcher_mentioning "$root" "$cmake" "$carriers")"
        # Exported in a subshell rather than as prefix assignments on a function call, which
        # a bash old enough (macOS's 3.2) is not documented to hand to the function's children.
        next="$( { ( export E2E_LAUNCHER_CARRIERS="$carriers" E2E_LAUNCHER_TARGET="$LauncherTargetPattern"
            _launcher_over "$root" "$files" awk '
                BEGIN { nc = split(ENVIRON["E2E_LAUNCHER_CARRIERS"], cs, "\n") }
                FNR == 1 { inset = 0 }
                { line = $0; sub(/^[[:space:]]*#.*/, "", line) }
                !inset && line ~ /^[[:space:]]*set[[:space:]]*\(/ {
                    inset = 1; text = ""; depth = 0; var = line
                    sub(/^[[:space:]]*set[[:space:]]*\([[:space:]]*/, "", var); sub(/[[:space:]\)].*/, "", var)
                }
                inset { text = text line; t = line; o = gsub(/\(/, "", t); t = line; c = gsub(/\)/, "", t); depth += o - c
                    if (depth <= 0) {
                        hit = (text ~ ENVIRON["E2E_LAUNCHER_TARGET"])
                        for (i = 1; i <= nc; i++) if (cs[i] != "" && index(text, "${" cs[i] "}")) hit = 1
                        if (hit) print var
                        inset = 0 } }
            ' ); printf '%s\n' "$carriers"; } | awk 'NF' | LC_ALL=C sort -u)"
        [ "$next" = "$carriers" ] && break
        carriers="$next"
    done
    printf '%s\n' "$carriers"
}

# Whether each `.sh` fixture calls the seam and keeps its launcher variable to the seam's
# order. One verdict line per fixture: `isolated`, or the first rule broken, in this order:
#
#   * the seam is called, naming a variable, after an `e2e_begin`;
#
#   * nothing names the variable before `e2e_begin`, whose snapshot of the caller's logs
#     is what a run BEFORE the seam is measured against: damage done above it is not seen;
#   * between `e2e_begin` and the seam, only a test, a message or the absolute-path
#     rewrite names it, and none of those may hold a command substitution that does --
#     `[ -n "$("$launcher" -z)" ]` is a test that RUNS it -- nor pipe a message or hand it
#     to `eval`, `source`, `.`, `sh -c` or `bash -c`;
#   * no EXIT trap (`trap ... EXIT` or `trap ... 0`) is installed after the seam, which would replace the check the seam
#     chained in front of the fixture's own -- a background subshell resetting its
#     inherited traps (`( trap - EXIT ...`) is the one exception, since it is not the
#     fixture's shell;
#   * the variable is used after the seam at all, or this scan cannot tell it runs the launcher.
#
# @param 1 A repository root.
# @param 2 The fixtures, repository-relative, one per line.
_launcher_seam_orders() {
    # ONE awk over every file, not a handful of processes per file: under Git Bash a spawn
    # is the unit of cost, and on a saturated host it is the whole of it. Each file is held
    # until the next one starts, because the verdict needs the seam's line before judging
    # the lines above it.
    _launcher_over "$1" "$2" awk '
        function reset() { n = 0; at = 0; begin = 0; v = "" }
        # The seam call, and the variable it names: the LAST call on the part of the line
        # before any `#`, as the greedy `[^#]*` the call is found with would take it.
        function seam_var(s,   pre, rest, m) {
            pre = s; sub(/#.*/, "", pre); m = ""
            rest = pre
            while (match(rest, /e2e_launcher_state_enter[[:space:]]+[A-Za-z_][A-Za-z0-9_]*/)) {
                m = substr(rest, RSTART, RLENGTH); rest = substr(rest, RSTART + RLENGTH)
            }
            sub(/^e2e_launcher_state_enter[[:space:]]+/, "", m)
            return m
        }
        function judge(   i, ref, canon, joined, first, line, text, pre, early, late, used, opened, reset_) {
            if (file == "") return
            if (!at) { print "unisolated " file; return }
            if (v == "") { print "unclassified " file ":" at " -- the seam call names no variable this scan can read"; return }
            if (!begin) { print "unclassified " file " -- calls no e2e_begin, so nothing snapshots the caller'"'"'s logs before the launcher could run"; return }
            if (at < begin) { print "unisolated " file ":" at " -- calls e2e_launcher_state_enter before e2e_begin (line " begin ")"; return }
            ref = "\\$(\\{)?" v "([^A-Za-z0-9_]|$)"
            # The one substitution that may name it: its own absolute-path rewrite.
            canon = "^[[:space:]]*" v "=\"\\$\\(cd \"\\$\\(dirname \"\\$" v "\"\\)\" && pwd\\)/\\$\\(basename \"\\$" v "\"\\)\"[[:space:]]*$"
            joined = ""
            # Continuation lines joined, so a command split over several lines is judged whole.
            for (i = 1; i <= n; i++) {
                if (joined == "") first = i
                line = lines[i]
                if (line ~ /\\$/) { joined = joined substr(line, 1, length(line) - 1) " "; continue }
                joined = joined line
                text = joined; joined = ""
                if (text ~ /^[[:space:]]*#/) { opened = 0; continue }
                if (first < begin) {
                    if (text ~ ref && !pre) pre = first
                } else if (first < at) {
                    if (text ~ ref && text !~ canon) {
                        if (index(text, "$(") || index(text, "`")) { if (!early) early = first }
                        # A message is text only while nothing runs it: not piped (`|`, but not
                        # `||`), not handed to `eval`, `source`, `.` or a `sh -c`/`bash -c`.
                        else if (text ~ /(^|[^|])\|([^|]|$)/ || text ~ /(^|[;&({[:space:]])(eval|source)[[:space:]]/ ||
                                 text ~ /(^|[;&({][[:space:]]*)\.[[:space:]]/ || text ~ /(^|[^A-Za-z0-9_])(ba)?sh[[:space:]]+-c/) {
                            if (!early) early = first
                        }
                        else {
                            gsub(/\[\[[^]]*\]\]/, "", text)
                            gsub(/\[ [^]]* \]/, "", text)
                            gsub(/(echo|printf|skip|fail|e2e_note)[[:space:]]+"[^"]*"/, "", text)
                            if (text ~ ref && !early) early = first
                        }
                    }
                } else if (first > at) {
                    if (text ~ ref) used = 1
                    # `0` is the other spelling of EXIT.
                    if (text ~ /(^|[;&|({][[:space:]]*)trap[[:space:]]/ && text ~ /[[:space:]](EXIT|0)([[:space:]]|;|$)/) {
                        reset_ = (text ~ /\([[:space:]]*trap[[:space:]]+-[[:space:]]/) || (opened && text ~ /^[[:space:]]*trap[[:space:]]+-[[:space:]]/)
                        if (!reset_ && !late) late = first
                    }
                }
                opened = (text ~ /\([[:space:]]*$/)
            }
            if (pre) printf "unisolated %s:%d -- names $%s before e2e_begin (line %d), whose snapshot of the caller'"'"'s logs must come first\n", file, pre, v, begin
            else if (early) printf "unisolated %s:%d -- uses $%s before e2e_launcher_state_enter (line %d); only a test, a message or the absolute-path rewrite may, and none of them may run a command naming it\n", file, early, v, at
            else if (late) printf "unisolated %s:%d -- installs an EXIT trap after e2e_launcher_state_enter (line %d), replacing the check the seam chained in front of the fixture'"'"'s own\n", file, late, at
            else if (!used) printf "unclassified %s:%d -- $%s is never used after e2e_launcher_state_enter, so this scan cannot tell which variable runs the launcher\n", file, at, v
            else print "isolated " file
        }
        FNR == 1 { judge(); reset(); file = FILENAME; sub(/^\.\//, "", file) }
        {
            lines[++n] = $0
            # Word boundaries spelled out: `\b` is a GNU extension to ERE.
            if (!at && $0 ~ /^([^#]*[^A-Za-z0-9_#])?e2e_launcher_state_enter([^A-Za-z0-9_]|$)/) { at = FNR; v = seam_var($0) }
            if (!begin && $0 ~ /^[[:space:]]*e2e_begin([[:space:]]|$)/) begin = FNR
        }
        END { judge() }
    '
}

# @param 1 A repository root.
# @param 2 Repository-relative paths to consider -- its CMake files and scripts -- one per line.
# Prints one verdict per line, sorted: `isolated PATH`, `unisolated PATH[:LINE -- WHY]`, or
# `unclassified WHAT -- WHY`.
_launcher_fixture_verdicts() {
    local root="$1" list="$2" path shells="" powershells="" declaring registered cmake="" carriers orders sealed sh_fixtures ps_fixtures
    while IFS= read -r path; do
        [ -n "$path" ] && [ -f "${root}/${path}" ] || continue
        case "$path" in
            *.sh)  shells="${shells}${path}"$'\n' ;;
            *.ps1) powershells="${powershells}${path}"$'\n' ;;
            CMakeLists.txt|*/CMakeLists.txt|*.cmake) cmake="${cmake}${path}"$'\n' ;;
        esac
    done < <(printf '%s\n' "$list")
    # The scripts that declare a launcher parameter: one `grep` per language, not per script.
    declaring="$( { _launcher_over "$root" "$shells" grep -lE -e '^[[:space:]]*--launcher\)' --
                   _launcher_over "$root" "$powershells" grep -liE -e '\[string\][[:space:]]*\$Launcher([^A-Za-z0-9_]|$)' --; } \
                 | awk 'NF')" || true

    carriers="$(_launcher_carriers "$root" "$cmake")"

    # Every script a launcher-bearing registration names, and every reference it cannot
    # read -- asked only of a file that mentions the launcher or a variable carrying it,
    # all of them in ONE awk that starts over at each file.
    # Blocks are tracked by parenthesis depth, which a regex held in a string can upset,
    # so a file that never mentions the launcher is not read at all: a block it cannot
    # close there is not this scan's business, and failing closed on it would be noise.
    registered="$( export E2E_LAUNCHER_CARRIERS="$carriers" E2E_LAUNCHER_TARGET="$LauncherTargetPattern"
        _launcher_over "$root" "$(_launcher_mentioning "$root" "$cmake" "$carriers")" awk '
            function opens(s,  t) { t = s; return gsub(/\(/, "", t) }
            function closes(s, t) { t = s; return gsub(/\)/, "", t) }
            BEGIN { nc = split(ENVIRON["E2E_LAUNCHER_CARRIERS"], cs, "\n"); for (i = 1; i <= nc; i++) if (cs[i] != "") carriers[cs[i]] = 1 }
            # A carrier handed on by anything but add_test or set -- `list(APPEND ...)`, an
            # argument to a helper function -- is a launcher this scan cannot follow.
            function names_carrier(s,   v) { for (v in carriers) if (index(s, "${" v "}")) return 1; return 0 }
            function judge(   bearing, v, rest, m, found) {
                if (kind == "set") return
                bearing = (text ~ ENVIRON["E2E_LAUNCHER_TARGET"])
                for (v in carriers) if (index(text, "${" v "}")) bearing = 1
                if (!bearing) return
                rest = text; found = 0
                while (match(rest, /\$\{(CMAKE_SOURCE_DIR|PROJECT_SOURCE_DIR|CMAKE_CURRENT_SOURCE_DIR)\}\/[^" )]+\.(sh|ps1|cmake|py)/)) {
                    m = substr(rest, RSTART, RLENGTH); rest = substr(rest, RSTART + RLENGTH)
                    if (m ~ /^\$\{CMAKE_CURRENT_SOURCE_DIR\}/) { sub(/^\$\{CMAKE_CURRENT_SOURCE_DIR\}\//, "", m); m = (dir == "." ? m : dir "/" m) }
                    else sub(/^\$\{[A-Z_]+\}\//, "", m)
                    print "script " m; found = 1
                }
                if (!found) print "unclassified " file ":" start " -- a registration hands over the launcher but names no script this scan can read"
            }
            # A block still open when its file ends: said once, for the file it opened in.
            function unclosed() { if (kind != "") print "unclassified " file ":" start " -- a block this scan never saw close"; kind = ""; depth = 0 }
            FNR == 1 {
                unclosed()
                file = FILENAME; sub(/^\.\//, "", file)
                dir = file; if (!sub(/\/[^\/]*$/, "", dir)) dir = "."
            }
            { line = $0; sub(/^[[:space:]]*#.*/, "", line) }
            depth == 0 && line ~ /^[[:space:]]*(add_test|set)[[:space:]]*\(/ {
                kind = (line ~ /^[[:space:]]*set/) ? "set" : "add_test"
                start = FNR; text = ""; depth = 0
            }
            {
                if (kind != "") { text = text "\n" line; depth += opens(line) - closes(line)
                    if (depth <= 0) { judge(); kind = ""; depth = 0 } }
                else if (line ~ ENVIRON["E2E_LAUNCHER_TARGET"] || names_carrier(line))
                    print "unclassified " file ":" FNR " -- passes the launcher outside an add_test or set this scan can read"
            }
            END { unclosed() }
        ')"

    {
        # A registered script is a fixture, so it must declare the launcher it is handed.
        while IFS= read -r path; do
            case "$path" in
                script\ *)
                    path="${path#script }"
                    # Matched by the shell, not a `grep` per script: literal, whole-line.
                    case $'\n'"$LauncherFixtureRunners" in *$'\n'"${path}:"*) continue ;; esac
                    # What the scan could not read, it cannot say declares nothing. Asked by
                    # OPENING it: under Git Bash `[ -r ]` answers from emulated mode bits and
                    # says yes to a file a Windows ACL denies, which `grep` then cannot read.
                    if [ ! -e "${root}/${path}" ]; then
                        echo "unclassified ${path} -- registered with the launcher, but does not exist"; continue
                    elif ! { : < "${root}/${path}"; } 2>/dev/null; then
                        echo "unclassified ${path} -- registered with the launcher, but cannot be read, so whether it declares a launcher parameter is unknown"; continue
                    fi
                    case $'\n'"$declaring"$'\n' in
                        *$'\n'"$path"$'\n'*) ;;
                        *) echo "unclassified ${path} -- registered with the launcher, but declares no launcher parameter" ;;
                    esac
                    ;;
                unclassified\ *) echo "$path" ;;
            esac
        done < <(printf '%s\n' "$registered")
        # Every script that declares a launcher calls its language's seam -- and a `.sh`
        # one calls it before it first runs the launcher. One process per language, never
        # one per fixture.
        sh_fixtures=""; ps_fixtures=""
        while IFS= read -r path; do
            case "$path" in
                "") ;;
                *.sh)  sh_fixtures="${sh_fixtures}${path}"$'\n' ;;
                *.ps1) ps_fixtures="${ps_fixtures}${path}"$'\n' ;;
                *) echo "unclassified ${path} -- a fixture in a language with no launcher-state seam" ;;
            esac
        done < <(printf '%s\n' "$declaring")
        # A batch fails OPEN where a per-file loop failed closed: a file the one process
        # could not read earns no line at all rather than a refusal. So both halves ask
        # which fixtures were ANSWERED for, and refuse the rest by name.
        orders="$(_launcher_seam_orders "$root" "$sh_fixtures")"
        [ -z "$orders" ] || printf '%s\n' "$orders"
        while IFS= read -r path; do
            [ -n "$path" ] || continue
            case $'\n'"$orders"$'\n' in
                *$'\n'"isolated ${path}"$'\n'*|*$'\n'"unisolated ${path}"$'\n'*) ;;
                *$'\n'"unisolated ${path}:"*|*$'\n'"unclassified ${path}:"*|*$'\n'"unclassified ${path} --"*) ;;
                *) echo "unclassified ${path} -- the order scan returned no verdict for it" ;;
            esac
        done < <(printf '%s\n' "$sh_fixtures")
        # The fixtures that DO call the seam, so one this grep could not read is unisolated.
        # Word boundaries spelled out: `\b` is a GNU extension to ERE.
        sealed="$(_launcher_over "$root" "$ps_fixtures" \
            grep -lE -e '^([^#]*[^A-Za-z0-9_#-])?Enter-E2ELauncherState([^A-Za-z0-9_-]|$)' --)" || true
        while IFS= read -r path; do
            [ -n "$path" ] || continue
            case $'\n'"$sealed"$'\n' in
                *$'\n'"$path"$'\n'*) echo "isolated ${path}" ;;
                *) echo "unisolated ${path}" ;;
            esac
        done < <(printf '%s\n' "$ps_fixtures")
    } | LC_ALL=C sort -u
}

# ======================================================================== --self-test
if [ "$LauncherMode" = self-test ]; then
echo "== the scan, against a planted tree that must earn every verdict"
# The canary, both halves in one planted tree: every verdict the scan can give, each
# asked of a file staged to earn exactly that verdict, and a file it must not mention.
launcher_canary="$(mktemp -d)"
mkdir -p "${launcher_canary}/scripts" "${launcher_canary}/src/sub"
printf 'case "$1" in\n    --launcher) launcher="$2" ;;\nesac\n"$launcher" -z\n' > "${launcher_canary}/scripts/bare.sh"
# The bash order rule, both directions. Each file behind a `|` stripped on the way out, so
# this check does not itself read as a fixture declaring `--launcher)`, which is what the
# scan asks of every tracked script.
# Read by the shell rather than a `sed` per file: a spawn is what this check pays for.
# Fed ONLY from the quoted heredocs below, which is why it may differ from `sed` on a last
# line with no newline (a heredoc ends every line) and on CRLF (`.gitattributes` keeps this file LF).
_launcher_plant() {
    local l
    while IFS= read -r l; do printf '%s\n' "${l#|}"; done > "${launcher_canary}/scripts/$1"
}
_launcher_plant sealed.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|e2e_launcher_state_enter launcher
|"$launcher" -s
PLANTED
# Every shape a fixture uses before the seam legitimately: a test, a message, the
# absolute-path rewrite -- and, after it, a background subshell resetting its own traps.
_launcher_plant guarded.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|trap cleanup EXIT
|e2e_begin t "$w"
|[[ -n "$launcher" && -x "$launcher" ]] || { echo "fastcache-cc not found: '$launcher'; skipping"; exit 77; }
|[ -x "$launcher" ] || skip "no ${launcher}"
|launcher="$(cd "$(dirname "$launcher")" && pwd)/$(basename "$launcher")"
|e2e_launcher_state_enter launcher
|"$launcher" -s
|( trap - EXIT TERM INT HUP; sleep 1 ) &
|(
|    trap - EXIT TERM INT HUP
|    sleep 1
|) &
PLANTED
# A reference above `e2e_begin`: its snapshot would be taken after the damage.
_launcher_plant prebegin.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|[[ -x "$launcher" ]] || exit 77
|e2e_begin t "$w"
|e2e_launcher_state_enter launcher
|"$launcher" -s
PLANTED
_launcher_plant early.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|"$launcher" -z
|e2e_launcher_state_enter launcher
|"$launcher" -s
PLANTED
_launcher_plant continued.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|run() { "$@"; }
|run \
|    "$launcher" -z
|e2e_launcher_state_enter launcher
|"$launcher" -s
PLANTED
# A RUN inside each region the rule exempts as text: a test, a message, and the path
# rebuilt from `dirname`/`basename`. Before `e2e_begin` any of them was a full escape.
_launcher_plant intest.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|[ -n "$("$launcher" -z >/dev/null 2>&1; echo x)" ]
|e2e_launcher_state_enter launcher
|"$launcher" -s
PLANTED
_launcher_plant inmessage.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|e2e_note "cleared: $($launcher -z)"
|e2e_launcher_state_enter launcher
|"$launcher" -s
PLANTED
_launcher_plant rebuilt.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|x=$(basename "$launcher"); "$(dirname "$launcher")/$x" -z
|e2e_launcher_state_enter launcher
|"$launcher" -s
PLANTED
# A message is text only while nothing runs it: piped to a shell, it is a run.
_launcher_plant piped.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|echo "$launcher -z" | sh
|e2e_launcher_state_enter launcher
|"$launcher" -s
PLANTED
# `trap ... 0` is EXIT's other spelling.
_launcher_plant trapzero.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|e2e_launcher_state_enter launcher
|"$launcher" -s
|trap other 0
PLANTED
# An EXIT trap installed after the seam replaces the check the seam chained in front.
_launcher_plant latetrap.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|e2e_launcher_state_enter launcher
|"$launcher" -s
|trap cleanup EXIT
PLANTED
_launcher_plant seamfirst.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_launcher_state_enter launcher
|e2e_begin t "$w"
|"$launcher" -s
PLANTED
_launcher_plant nobegin.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_launcher_state_enter launcher
|"$launcher" -s
PLANTED
_launcher_plant novar.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|e2e_launcher_state_enter
PLANTED
_launcher_plant unused.sh <<'PLANTED'
|case "$1" in
|    --launcher) launcher="$2" ;;
|esac
|e2e_begin t "$w"
|e2e_launcher_state_enter launcher
PLANTED
printf 'param([string]$Launcher)\n# Enter-E2ELauncherState in a comment is not a call\n& $Launcher -z\n' > "${launcher_canary}/scripts/bare.ps1"
printf 'param([string]$Launcher)\n$h = Enter-E2ELauncherState -Launcher $Launcher -Fixture $PSCommandPath\n' > "${launcher_canary}/src/sub/sealed.ps1"
printf 'echo "no launcher here"\n' > "${launcher_canary}/scripts/plain.sh"
printf 'case "$1" in\n    --other) : ;;\nesac\n' > "${launcher_canary}/scripts/undeclared.sh"
# A carrier of a carrier (declared in another file), and the launcher's DIRECTORY: each
# hands a script the launcher, so each script must declare it.
printf 'case "$1" in\n    --cc) cc="$2" ;;\nesac\n' > "${launcher_canary}/scripts/p1.sh"
printf 'case "$1" in\n    --tooldir) dir="$2" ;;\nesac\n' > "${launcher_canary}/scripts/p4.sh"
cat > "${launcher_canary}/CMakeLists.txt" <<'PLANTED'
add_test(NAME a COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/bare.sh" --launcher "$<TARGET_FILE:fastcache-cc>")
add_test(
    NAME b
    COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" "${CMAKE_SOURCE_DIR}/scripts/sealed.sh"
        --launcher "$<TARGET_FILE:fastcache-cc>")
set(_carrier
    --launcher "$<TARGET_FILE:fastcache-cc>")
add_test(NAME c COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/undeclared.sh" ${_carrier})
add_test(NAME d COMMAND "$<TARGET_FILE:fastcache-cc>" --version)
add_test(NAME e COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/plain.sh")
message(STATUS "$<TARGET_FILE:fastcache-cc>")
set(_first "$<TARGET_FILE:fastcache-cc>")
add_test(NAME h COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/p4.sh" --tooldir "$<TARGET_FILE_DIR:fastcache-cc>")
PLANTED
printf 'set(_second "${_first}")\n' > "${launcher_canary}/src/sub/more.cmake"
cat > "${launcher_canary}/src/sub/CMakeLists.txt" <<'PLANTED'
add_test(NAME f COMMAND pwsh -File "${CMAKE_CURRENT_SOURCE_DIR}/sealed.ps1" -Launcher "$<TARGET_FILE:fastcache-cc>")
add_test(NAME g COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/p1.sh" --cc "${_second}")
list(APPEND _handed ${_second})
PLANTED
# A regex in a string upsets the depth count of a file that never mentions the launcher,
# and must earn no verdict at all.
printf 'set(pattern "(")\nset(other "x")\n' > "${launcher_canary}/scripts/noise.cmake"
launcher_canary_list="$(cd "$launcher_canary" && find . -type f | sed 's#^\./##' | LC_ALL=C sort)"
launcher_canary_want="$(LC_ALL=C sort <<'WANT'
isolated scripts/guarded.sh
isolated scripts/sealed.sh
isolated src/sub/sealed.ps1
unclassified CMakeLists.txt:11 -- passes the launcher outside an add_test or set this scan can read
unclassified CMakeLists.txt:9 -- a registration hands over the launcher but names no script this scan can read
unclassified scripts/nobegin.sh -- calls no e2e_begin, so nothing snapshots the caller's logs before the launcher could run
unclassified scripts/novar.sh:5 -- the seam call names no variable this scan can read
unclassified src/sub/CMakeLists.txt:3 -- passes the launcher outside an add_test or set this scan can read
unclassified scripts/p1.sh -- registered with the launcher, but declares no launcher parameter
unclassified scripts/p4.sh -- registered with the launcher, but declares no launcher parameter
unclassified scripts/undeclared.sh -- registered with the launcher, but declares no launcher parameter
unclassified scripts/unused.sh:5 -- $launcher is never used after e2e_launcher_state_enter, so this scan cannot tell which variable runs the launcher
unisolated scripts/bare.ps1
unisolated scripts/bare.sh
unisolated scripts/continued.sh:6 -- uses $launcher before e2e_launcher_state_enter (line 8); only a test, a message or the absolute-path rewrite may, and none of them may run a command naming it
unisolated scripts/early.sh:5 -- uses $launcher before e2e_launcher_state_enter (line 6); only a test, a message or the absolute-path rewrite may, and none of them may run a command naming it
unisolated scripts/inmessage.sh:5 -- uses $launcher before e2e_launcher_state_enter (line 6); only a test, a message or the absolute-path rewrite may, and none of them may run a command naming it
unisolated scripts/intest.sh:5 -- uses $launcher before e2e_launcher_state_enter (line 6); only a test, a message or the absolute-path rewrite may, and none of them may run a command naming it
unisolated scripts/latetrap.sh:7 -- installs an EXIT trap after e2e_launcher_state_enter (line 5), replacing the check the seam chained in front of the fixture's own
unisolated scripts/piped.sh:5 -- uses $launcher before e2e_launcher_state_enter (line 6); only a test, a message or the absolute-path rewrite may, and none of them may run a command naming it
unisolated scripts/prebegin.sh:4 -- names $launcher before e2e_begin (line 5), whose snapshot of the caller's logs must come first
unisolated scripts/trapzero.sh:7 -- installs an EXIT trap after e2e_launcher_state_enter (line 5), replacing the check the seam chained in front of the fixture's own
unisolated scripts/rebuilt.sh:5 -- uses $launcher before e2e_launcher_state_enter (line 6); only a test, a message or the absolute-path rewrite may, and none of them may run a command naming it
unisolated scripts/seamfirst.sh:4 -- calls e2e_launcher_state_enter before e2e_begin (line 5)
WANT
)"
ran=$(( ran + 1 ))
launcher_canary_got="$(_launcher_fixture_verdicts "$launcher_canary" "$launcher_canary_list")"
if [ "$launcher_canary_got" != "$launcher_canary_want" ]; then
    echo "FAIL launcher-state-scan-canary: the scan's verdicts on the planted tree are not the staged ones" >&2
    diff <(printf '%s\n' "$launcher_canary_want") <(printf '%s\n' "$launcher_canary_got") | sed 's/^/     | /' >&2
    note_failure "launcher-state-scan-canary"
fi
rm -rf "$launcher_canary"
fi

# ============================================================================= tree
if [ "$LauncherMode" = tree ]; then
echo "== every fixture handed a launcher records into the run's own state directory"
# The real tree, through the same census the stray walk uses: tracked, first-party.
if git -C "$source_dir" rev-parse --git-dir >/dev/null 2>&1; then
    ran=$(( ran + 1 ))
    launcher_tracked="$(git -C "$source_dir" ls-files 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' '*.sh' '*.ps1')"
    launcher_first_party="$(first_party_paths "$source_dir" "$launcher_tracked")" || {
        echo "FAIL launcher-state-scan: cannot read scripts/lib/third-party-roots.txt" >&2
        note_failure "launcher-state-scan"
        launcher_first_party=""
    }
    launcher_verdicts="$(_launcher_fixture_verdicts "$source_dir" "$launcher_first_party")"
    launcher_isolated="$(grep -c '^isolated ' <<< "$launcher_verdicts" || true)"
    # A census that found no fixture has not said the tree is clean.
    if [ "$launcher_isolated" -eq 0 ]; then
        echo "FAIL launcher-state-scan: found no launcher fixture at all, so it has judged nothing" >&2
        note_failure "launcher-state-scan"
    fi
    if grep -qvE '^isolated ' <<< "$launcher_verdicts"; then
        echo "FAIL launcher-state-scan: a fixture handed a launcher does not keep its state its own, or cannot be classified:" >&2
        grep -vE '^isolated ' <<< "$launcher_verdicts" | sed 's/^/     /' >&2
        echo "     Call e2e_launcher_state_enter (scripts/lib/e2e-common.sh) after e2e_begin and before" >&2
        echo "     any line that runs the launcher, or Enter-E2ELauncherState (scripts/lib/E2EEnvironment.psm1)" >&2
        echo "     inside the try whose finally calls Exit-E2ELauncherState, and run every launcher inside" >&2
        echo "     Use-E2ELauncherState; a .ps1 fixture also calls Assert-E2ELauncherFixture at startup," >&2
        echo "     before its skips, as dist-compile-e2e.ps1 does. An 'unclassified' line names what this" >&2
        echo "     scan could not read: make the registration name its script, or declare the launcher" >&2
        echo "     parameter the script is handed." >&2
        note_failure "launcher-state-scan"
    fi
    echo "   ${launcher_isolated} launcher fixture(s), each calling its launcher-state seam"
else
    echo "SKIPPED launcher-state-scan: not a git work tree, so there is no tracked set to judge" >&2
    skipped=$(( skipped + 1 ))
fi

# --- the readers -----------------------------------------------------------------
#
# The two readers of Stats.cpp give one answer -- the PowerShell one in E2EEnvironment.psm1
# and `e2e_launcher_state_rows` -- per PLATFORM, on the real file AND on planted ones: a
# variable the real file does not read, which is the case "derived, not restated" is
# about, and a variable in each branch of `#if defined(_WIN32)`, which is the case
# "redirect only what the launcher reads here" is about. The bash reader's refusals are
# asked here too; the PowerShell reader's need pwsh.
echo "== the two readers of Stats.cpp"
launcher_reader_dir="$(mktemp -d)"
cat > "${launcher_reader_dir}/extra.cpp" <<'PLANTED'
    [[nodiscard]] std::filesystem::path StateDirectoryImpl()
    {
        std::string base;
        if (auto const local = FastCache::ReadEnvironmentVariable("LOCALAPPDATA"); local.has_value())
            base = *local;
        auto const extra = FastCache::ReadEnvironmentVariable("FC_STATE_EXTRA");
        if (extra.has_value())
            base = *extra + "/sub";
        return base;
    }
PLANTED
cat > "${launcher_reader_dir}/branches.cpp" <<'PLANTED'
    [[nodiscard]] std::filesystem::path StateDirectoryImpl()
    {
        std::string base;
#if defined(_WIN32)
        if (auto const w = FastCache::ReadEnvironmentVariable("WIN_ONLY"); w.has_value())
            base = *w;
#else
        auto const p = FastCache::ReadEnvironmentVariable("POSIX_ONLY");
        if (p.has_value())
            base = *p + "/s";
#endif
        return base;
    }
PLANTED
sed 's#base = \*extra + "/sub";#;#' "${launcher_reader_dir}/extra.cpp" > "${launcher_reader_dir}/never-base.cpp"
sed 's/^#if defined(_WIN32)$/#ifdef __APPLE__/' "${launcher_reader_dir}/branches.cpp" > "${launcher_reader_dir}/other-if.cpp"
printf 'int main() {}\n' > "${launcher_reader_dir}/no-body.cpp"
# Every (file, platform) this section asks the bash reader, answered by ONE subshell that
# sources the library once -- not a subshell per question, which on Git Bash is most of what
# this section costs. Sourcing clears every FASTCACHE_* variable, which this check has no
# reason to do to itself, hence the subshell. A query's index is its line, from 0; each
# answer line is `INDEX:row`, or `INDEX:refused`.
launcher_stats="${source_dir}/src/apps/fastcache-cc/Stats.cpp"
launcher_bash_queries="${launcher_reader_dir}/extra.cpp|windows
${launcher_reader_dir}/extra.cpp|posix
${launcher_reader_dir}/branches.cpp|windows
${launcher_reader_dir}/branches.cpp|posix
${launcher_reader_dir}/never-base.cpp|posix
${launcher_reader_dir}/no-body.cpp|posix
${launcher_reader_dir}/other-if.cpp|posix
${launcher_stats}|windows
${launcher_stats}|posix"
launcher_bash_answers="$( . "$library"
    launcher_i=0
    while IFS='|' read -r launcher_file launcher_platform; do
        if launcher_rows="$(e2e_launcher_state_rows "$launcher_file" "$launcher_platform" 2>/dev/null)"; then
            while IFS= read -r launcher_row; do echo "${launcher_i}:${launcher_row}"; done <<< "$launcher_rows"
        else
            echo "${launcher_i}:refused"
        fi
        launcher_i=$(( launcher_i + 1 ))
    done <<< "$launcher_bash_queries" )"
# The rows answered for query @p 1, one per line, without its index.
_launcher_bash_answer() {
    local line out=""
    while IFS= read -r line; do
        case "$line" in "${1}:"*) out="${out}${line#*:}"$'\n' ;; esac
    done <<< "$launcher_bash_answers"
    printf '%s' "${out%$'\n'}"
}
# Query @p 1 itself, `file|platform`.
_launcher_query() {
    local line i=0
    while IFS= read -r line; do
        if [ "$i" -eq "$1" ]; then printf '%s' "$line"; return 0; fi
        i=$(( i + 1 ))
    done <<< "$launcher_bash_queries"
}
# What the first four queries must read as, in their order.
launcher_index=0
for launcher_want in "LOCALAPPDATA|;FC_STATE_EXTRA|/sub" "LOCALAPPDATA|;FC_STATE_EXTRA|/sub" "WIN_ONLY|" "POSIX_ONLY|/s"; do
    ran=$(( ran + 1 ))
    launcher_query="$(_launcher_query "$launcher_index")"
    launcher_file="${launcher_query%|*}"; launcher_file="${launcher_file##*/}"; launcher_platform="${launcher_query##*|}"
    launcher_got="$(_launcher_bash_answer "$launcher_index" | tr '\n' ';');"
    if [ "$launcher_got" != "${launcher_want};" ]; then
        echo "FAIL launcher-state-reader: ${launcher_file} on ${launcher_platform} read as '${launcher_got}', must be '${launcher_want};'" >&2
        note_failure "launcher-state-reader"
    fi
    launcher_index=$(( launcher_index + 1 ))
done
# The next three must be refused.
for launcher_refused in never-base no-body other-if; do
    ran=$(( ran + 1 ))
    if [ "$(_launcher_bash_answer "$launcher_index")" != refused ]; then
        echo "FAIL launcher-state-reader: ${launcher_refused}.cpp was answered, and it must be refused" >&2
        note_failure "launcher-state-reader"
    fi
    launcher_index=$(( launcher_index + 1 ))
done
if [ "$have_pwsh" = yes ]; then
    # Every (file, platform) pair asked of ONE pwsh, each answer prefixed with its index:
    # a pwsh start costs seconds on Windows, and this runs in the default set. Row: the
    # file, then the bash query answering it on windows and on posix.
    launcher_pairs="${launcher_stats}|7|8
${launcher_reader_dir}/extra.cpp|0|1
${launcher_reader_dir}/branches.cpp|2|3"
    launcher_ps_list=""
    while IFS='|' read -r launcher_source _ _; do
        launcher_ps_list="${launcher_ps_list}${launcher_ps_list:+,}'$(_launcher_native_path "$launcher_source")'"
    done <<< "$launcher_pairs"
    launcher_pwsh_all="$(pwsh -NoProfile -Command "Import-Module '$(_launcher_native_path "$psmodule")' -Force
        \$i = 0
        foreach (\$f in @(${launcher_ps_list})) { foreach (\$p in 'windows', 'posix') {
            try { Get-E2ELauncherStateRows -Path \$f -Platform \$p | ForEach-Object { \"\${i}:\" + \$_.Name + '|' + \$_.BaseSuffix } }
            catch { \"\${i}:refused\" }
            \$i++ } }" 2>&1 | tr -d '\r')"
    launcher_pwsh_index=0
    while IFS='|' read -r launcher_source launcher_on_windows launcher_on_posix; do
        for launcher_platform in windows posix; do
            ran=$(( ran + 1 ))
            if [ "$launcher_platform" = windows ]; then launcher_index="$launcher_on_windows"; else launcher_index="$launcher_on_posix"; fi
            launcher_bash_rows="$(_launcher_bash_answer "$launcher_index")"
            # `if`, never `case`, inside `$( )`: bash 3.2 reads a case pattern's `)` as the
            # substitution's end (#1224) -- at RUN time, which `check-bash32-parse.sh` cannot see.
            launcher_pwsh_rows="$(while IFS= read -r launcher_line; do
                if [[ "$launcher_line" == "${launcher_pwsh_index}:"* ]]; then printf '%s\n' "${launcher_line#*:}"; fi
            done <<< "$launcher_pwsh_all")"
            if [ -z "$launcher_bash_rows" ] || [ "$launcher_bash_rows" = refused ] || [ "$launcher_bash_rows" != "$launcher_pwsh_rows" ]; then
                echo "FAIL launcher-state-readers-agree: the two readers of $(basename "$launcher_source") disagree on ${launcher_platform}" >&2
                diff <(printf '%s\n' "$launcher_bash_rows") <(printf '%s\n' "$launcher_pwsh_rows") | sed 's/^/     | /' >&2
                note_failure "launcher-state-readers-agree"
            fi
            launcher_pwsh_index=$(( launcher_pwsh_index + 1 ))
        done
    done <<< "$launcher_pairs"
    echo "   the readers agree on 3 file(s) x 2 platform(s)"
else
    echo "SKIPPED launcher-state-readers-agree: no pwsh here, so the PowerShell reader cannot be asked" >&2
    skipped=$(( skipped + 1 ))
fi
rm -rf "$launcher_reader_dir"
fi

# ======================================================================== --self-test
if [ "$LauncherMode" = self-test ]; then
# --- the bash seam, driven -----------------------------------------------------------
#
# Every case is a fixture of its own -- `set -euo pipefail`, an EXIT trap that is its
# cleanup, `e2e_begin` -- run as its own process, since `fail` signals the process that
# called `e2e_begin`. The stand-in launcher is generated from this platform's rows and
# honours them in the order the source reads them, the first one set winning:
# `StateDirectoryImpl`'s precedence. Every row's variable points at the case's caller.
#
# A row: `case|caller|outcome|text`, outcome `pass` (exit 0, the case's last line reached,
# the caller's logs byte-identical) or `fail` (exit non-zero, `text` on stderr).
echo "== the bash seam, against a caller log that is present, empty and absent"
LauncherSeamCases="
clean|present|pass|
clean|empty|pass|
clean|absent|pass|
nostats|present|pass|
nostats|absent|pass|
bypass|present|fail|of this run's compiles were recorded in the caller's statistics log
bypass|empty|fail|of this run's compiles were recorded in the caller's statistics log
bypass|absent|fail|of this run's compiles were recorded in the caller's statistics log
prez|present|fail|was DELETED during this run
prez|empty|fail|was DELETED during this run
prez|absent|pass|
misdirected|absent|fail|did not have its state redirected
ampersand|absent|pass|
"
# How many lanes run the cases at once: each case is a process that sources the library
# and spawns a dozen more, so on Git Bash they were most of this check run one by one.
LauncherSeamLanes=6
launcher_seam_dir="$(mktemp -d)"
# The stand-in's interpreter line, the platform, the log version the launcher writes and the
# platform's rows, from ONE subshell that sources the library. The stand-in records in the
# CURRENT version, as the launcher does: a stand-in writing an older layout is one the
# caller-damage check can read while it misreads the real launcher's records.
launcher_seam_answer="$( . "$library"; e2e_bash_shebang; e2e_launcher_state_platform
    e2e_launcher_log_line MISS @SOURCE@; e2e_launcher_state_rows )" \
    || launcher_seam_answer=""
launcher_seam_shebang="${launcher_seam_answer%%$'\n'*}"
launcher_seam_answer="${launcher_seam_answer#*$'\n'}"
launcher_seam_platform="${launcher_seam_answer%%$'\n'*}"
launcher_seam_answer="${launcher_seam_answer#*$'\n'}"
launcher_seam_log_line="${launcher_seam_answer%%$'\n'*}"
launcher_seam_rows="${launcher_seam_answer#*$'\n'}"
case "$launcher_seam_log_line" in v[0-9]*@SOURCE@*) ;; *) launcher_seam_rows="" ;; esac
[ "$launcher_seam_rows" != "$launcher_seam_answer" ] || launcher_seam_rows=""
if [ -z "$launcher_seam_rows" ]; then
    ran=$(( ran + 1 ))
    echo "FAIL launcher-state-seam: cannot read this platform's rows, so the seam cannot be driven" >&2
    note_failure "launcher-state-seam"
else
    launcher_fake="${launcher_seam_dir}/fastcache-cc"
    {
        printf '%s\nbase=""\nlog_line=%q\n' "$launcher_seam_shebang" "$launcher_seam_log_line"
        while IFS='|' read -r launcher_name launcher_suffix; do
            printf 'if [ -z "$base" ] && [ -n "${%s-}" ]; then base="${%s}%s"; fi\n' "$launcher_name" "$launcher_name" "$launcher_suffix"
        done <<< "$launcher_seam_rows"
        cat <<'FAKE'
log="${base}/fastcache-cc/invocations.log"
case "${1-}" in
    --show-stats) if [ -s "$log" ]; then echo "fastcache-cc statistics (${log})"; else echo "fastcache-cc: no statistics recorded yet (${log})."; fi ;;
    -z) rm -f "$log" ;;
    *) if [ -z "${FASTCACHE_NO_STATS-}" ]; then
           mkdir -p "${base}/fastcache-cc"
           for last in "$@"; do :; done
           # Split at the marker, never `${log_line/@SOURCE@/...}`: no spelling of that replacement
           # serves both shells. Unquoted, bash 5.2's `patsub_replacement` reads a `&` in it as the
           # matched pattern; quoted, bash 3.2 keeps the quotes, so every record named a source
           # inside literal `"`s and no tree's prefix matched it -- measured, on macOS.
           printf '%s\n' "${log_line%%@SOURCE@*}${last}${log_line#*@SOURCE@}" >> "$log"
       fi ;;
esac
FAKE
    } > "$launcher_fake"
    chmod +x "$launcher_fake"
    # Each case's body, after the common head.
    _launcher_seam_body() {
        case "$1" in
            clean) printf '%s\n' 'e2e_launcher_state_enter launcher' '"$launcher" "$workdir/a.cpp"' \
                        'e2e_launcher_state_assert_used' '"$launcher" -z' 'e2e_launcher_state_assert_caller_untouched' ;;
            nostats) printf '%s\n' 'e2e_launcher_state_enter launcher' 'FASTCACHE_NO_STATS=1 "$launcher" "$workdir/a.cpp"' \
                        'e2e_launcher_state_assert_used --no-stats' 'e2e_launcher_state_assert_caller_untouched' ;;
            # A launcher run past the shim, and then a failure: the EXIT check must report it.
            bypass) printf '%s\n' 'real="$launcher"' 'e2e_launcher_state_enter launcher' '"$real" "$workdir/b.cpp"' 'false' ;;
            # `-z` BEFORE the seam: measured against e2e_begin's snapshot.
            prez) printf '%s\n' '"$launcher" -z' 'e2e_launcher_state_enter launcher' 'e2e_launcher_state_assert_caller_untouched' ;;
            # The shim's redirect stripped after the guard: the positive control must see it.
            misdirected) printf '%s\n' 'e2e_launcher_state_enter launcher' 'grep -v "^export " "$launcher" > "$workdir/shim"' \
                        'cat "$workdir/shim" > "$launcher"' 'FASTCACHE_NO_STATS=1 "$launcher" "$workdir/c.cpp"' \
                        'e2e_launcher_state_assert_used --no-stats' ;;
            # A source path holding `&`: the stand-in's record must name it exactly, which an
            # unquoted replacement under bash 5.2's `patsub_replacement` does not.
            ampersand) printf '%s\n' 'e2e_launcher_state_enter launcher' 'src="$workdir/t&u.cpp"' '"$launcher" "$src"' \
                        'e2e_launcher_state_assert_used' \
                        'grep -qF "$src" "$_e2e_launcher_state_log" || fail "the record does not name $src: $(cat "$_e2e_launcher_state_log")"' ;;
        esac
    }
    _launcher_caller_hashes() {
        local name suffix log
        while IFS='|' read -r name suffix; do
            log="${1}${suffix}/fastcache-cc/invocations.log"
            if [ -f "$log" ]; then printf '%s ' "$(cksum < "$log")"; else printf 'absent '; fi
        done <<< "$launcher_seam_rows"
    }
    # Staged in table order, run in lanes, judged in table order in THIS shell once every
    # lane has ended -- `e2e-helpers-selftest`'s shape. Each case is a process of its own
    # with a directory of its own, so the lanes share nothing but the stand-in launcher,
    # which only reads. A case whose lane recorded no status never reached a verdict, and is
    # a failure by name.
    launcher_seam_index=0
    while IFS='|' read -r launcher_case launcher_state launcher_outcome launcher_text; do
        [ -n "$launcher_case" ] || continue
        launcher_case_dir="${launcher_seam_dir}/case-${launcher_seam_index}"
        launcher_caller="${launcher_case_dir}/caller"
        mkdir -p "$launcher_caller"
        while IFS='|' read -r launcher_name launcher_suffix; do
            if [ "$launcher_state" != absent ]; then
                mkdir -p "${launcher_caller}${launcher_suffix}/fastcache-cc"
                if [ "$launcher_state" = present ]; then
                    printf 'HIT\tdefault\t1\t2\t/sentinel/a.cpp\t\t0\t0\t0\t0\t0\tNOT_ATTEMPTED\t\n' > "${launcher_caller}${launcher_suffix}/fastcache-cc/invocations.log"
                else
                    : > "${launcher_caller}${launcher_suffix}/fastcache-cc/invocations.log"
                fi
            fi
        done <<< "$launcher_seam_rows"
        _launcher_caller_hashes "$launcher_caller" > "${launcher_case_dir}/before"
        {
            printf 'set -euo pipefail\n. %q\nworkdir="$(mktemp -d)"\ncleanup() { rm -rf "$workdir"; }\ntrap cleanup EXIT\n' "$library"
            printf 'e2e_begin "seam-%s-%s" "$workdir"\nlauncher=%q\n' "$launcher_case" "$launcher_state" "$launcher_fake"
            _launcher_seam_body "$launcher_case"
            printf 'echo CASE-REACHED-END\n'
        } > "${launcher_case_dir}/case.sh"
        echo "$launcher_case_dir" >> "${launcher_seam_dir}/lane-$(( launcher_seam_index % LauncherSeamLanes ))"
        launcher_seam_index=$(( launcher_seam_index + 1 ))
    done <<< "$LauncherSeamCases"
    for launcher_lane in "${launcher_seam_dir}"/lane-*; do
        (
            trap - EXIT TERM INT HUP
            while IFS= read -r launcher_case_dir; do
                (
                    while IFS='|' read -r launcher_name launcher_suffix; do
                        export "${launcher_name}=${launcher_case_dir}/caller"
                    done <<< "$launcher_seam_rows"
                    exec bash "${launcher_case_dir}/case.sh"
                ) > "${launcher_case_dir}/out" 2> "${launcher_case_dir}/err"
                echo "$?" > "${launcher_case_dir}/status"
            done < "$launcher_lane"
        ) &
        echo "$!" >> "${launcher_seam_dir}/pids"
    done
    while IFS= read -r launcher_pid; do
        wait "$launcher_pid" 2>/dev/null || true
    done < "${launcher_seam_dir}/pids"

    launcher_seam_ran=0
    launcher_seam_index=0
    while IFS='|' read -r launcher_case launcher_state launcher_outcome launcher_text; do
        [ -n "$launcher_case" ] || continue
        ran=$(( ran + 1 )); launcher_seam_ran=$(( launcher_seam_ran + 1 ))
        launcher_case_dir="${launcher_seam_dir}/case-${launcher_seam_index}"
        launcher_seam_index=$(( launcher_seam_index + 1 ))
        launcher_why=""
        if [ ! -s "${launcher_case_dir}/status" ]; then
            launcher_why="its lane recorded no exit status, so the case never reached a verdict"
        else
            launcher_rc="$(cat "${launcher_case_dir}/status")"
            launcher_before="$(cat "${launcher_case_dir}/before")"
            launcher_after="$(_launcher_caller_hashes "${launcher_case_dir}/caller")"
            if [ "$launcher_outcome" = pass ]; then
                [ "$launcher_rc" -eq 0 ] || launcher_why="exit ${launcher_rc}, not 0"
                grep -q '^CASE-REACHED-END$' "${launcher_case_dir}/out" || launcher_why="${launcher_why:+$launcher_why; }the case never reached its last line"
                [ "$launcher_before" = "$launcher_after" ] || launcher_why="${launcher_why:+$launcher_why; }the caller's logs changed (${launcher_before}-> ${launcher_after})"
            else
                [ "$launcher_rc" -ne 0 ] || launcher_why="exit 0, and it must fail"
                grep -qF "$launcher_text" "${launcher_case_dir}/err" || launcher_why="${launcher_why:+$launcher_why; }stderr does not say '${launcher_text}'"
            fi
        fi
        if [ -n "$launcher_why" ]; then
            echo "FAIL launcher-state-seam: ${launcher_case} against a caller log ${launcher_state}: ${launcher_why}" >&2
            [ -f "${launcher_case_dir}/err" ] && awk 'NR <= 8 { print "     | " $0 }' "${launcher_case_dir}/err" >&2
            note_failure "launcher-state-seam"
        fi
    done <<< "$LauncherSeamCases"
    echo "   ${launcher_seam_ran} bash seam case(s) driven on ${launcher_seam_platform}"
fi
rm -rf "$launcher_seam_dir"

# --- the PowerShell seam and scan, driven ---------------------------------------------
echo "== the PowerShell seam and its scan"
if [ "$have_pwsh" = yes ]; then
    ran=$(( ran + 1 ))
    launcher_ps_out="$(pwsh -NoProfile -ExecutionPolicy Bypass -Command "Import-Module '$(_launcher_native_path "$psmodule")' -Force
        \$scan = Test-E2EUnwrappedLauncherScan
        if (\$scan) { 'FAIL-SCAN ' + \$scan }
        foreach (\$f in @(Test-E2ELauncherStateSeam)) { 'FAIL-SEAM ' + \$f }" 2>&1 | tr -d '\r')"
    launcher_ps_rc=$?
    printf '%s\n' "$launcher_ps_out" | grep -v '^FAIL-' | sed 's/^/   /'
    if grep -q '^FAIL-SCAN ' <<< "$launcher_ps_out"; then
        echo "FAIL launcher-state-ps-scan: $(awk '/^FAIL-SCAN / && !shown { print substr($0, 1, 600); shown = 1 }' <<< "$launcher_ps_out")" >&2
        note_failure "launcher-state-ps-scan"
    fi
    if grep -q '^FAIL-SEAM ' <<< "$launcher_ps_out"; then
        grep '^FAIL-SEAM ' <<< "$launcher_ps_out" | sed 's/^FAIL-SEAM /FAIL launcher-state-ps-seam: /' | cut -c1-600 >&2
        note_failure "launcher-state-ps-seam"
    fi
    # A self-test that did not run is not one that passed.
    if [ "$launcher_ps_rc" -ne 0 ] || ! grep -q 'launcher-state seam self-test: [1-9][0-9]* case(s)' <<< "$launcher_ps_out"; then
        echo "FAIL launcher-state-ps-seam: the PowerShell self-test did not report its cases (exit ${launcher_ps_rc})" >&2
        note_failure "launcher-state-ps-seam"
    fi
else
    echo "SKIPPED launcher-state-ps-seam: no pwsh here, so the PowerShell seam and scan cannot be driven" >&2
    skipped=$(( skipped + 1 ))
fi

fi

echo
echo "${LauncherLabel}: ${ran} checks ran, ${failures} failed, ${skipped} skipped"
if [ "$failures" -gt 0 ]; then
    echo "  failed: ${failed_cases}"
    exit 1
fi
exit 0
