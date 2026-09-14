#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# `.clang-format-version` names the clang-format BUILD `Check C++ style` judges this tree
# with, and that statement is true (#1349).
#
# ## Why the tree states a build at all
#
# `CLANG_TOOLS_VERSION` pins a MAJOR, and apt.llvm.org ships rolling snapshots under one
# version number, so "the pinned clang-format" names a family rather than a formatter.
# Two builds of one major can disagree about the same file, and a formatter at the wrong
# build REWRITES code the style job already accepted -- a diff every line of which is
# "just formatting". The contour-workflows Claude Code plugin runs clang-format on every
# C++ edit, and until #1349 it ran whichever binary PATH found first: 22.1.3 bundled with
# Visual Studio, against CI's 22.1.8 snapshot. The remedy reads as already applied --
# there IS a binary called clang-format -- which is what made it cost nothing to miss.
#
# The declaration is what that hook reads: it writes only with a binary whose
# `--version` line is declared, character for character, and otherwise writes nothing
# and says why. `local-gate.sh` asks the same question before its `clang-format -i`.
#
# ## Two questions, two modes, because they need different machines
#
#   (default)            Does the declaration parse, and does it name the major
#                        `build.yml` pins and the `clang-format-<major>` binary every
#                        consumer resolves? Reads two files, needs no formatter, and runs
#                        in the default ctest set -- so a change bumping
#                        `CLANG_TOOLS_VERSION` without the declaration is refused on the
#                        pull request that makes it.
#
#   --installed <exe>    Is <exe> a declared build? Only a machine with the formatter
#                        installed can answer, so this runs in the `Check C++ style` job
#                        right after the install -- every green run of that job is then
#                        a proof that the declaration names the build it judged with --
#                        and in `local-gate.sh` before it formats. It is deliberately NOT
#                        a ctest: every build leg lacks the pinned formatter, so it would
#                        skip everywhere it ran, which is #1135's lesson.
#
# The live mode is the one that turns a snapshot roll on apt.llvm.org into a red
# `Check C++ style` naming the new build, rather than a silent disagreement between CI and
# every developer machine. That red is on every branch at once and is fixed by one line
# in a change of its own, and the refusal says so rather than letting it read as the
# branch's fault.
#
# ## The grammar is the hook's, and deliberately strict
#
# Blank lines and `#` comments; otherwise `version: <the whole --version line>` and
# `binary: <clang-format or clang-format-*>`, each repeatable. Any other line refuses. The
# plugin documents the same grammar and refuses the same way, so a declaration this check
# accepts is one the hook can use -- a laxer reader here would pass a file the hook then
# declines on every edit, which is a silent loss of the formatter rather than a wrong one.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs on every platform CI builds.
#
# Usage:
#   bash scripts/check-clang-format-version.sh [<source-dir>]
#   bash scripts/check-clang-format-version.sh --installed <exe> [<source-dir>]
#   bash scripts/check-clang-format-version.sh --self-test [<source-dir>]

set -uo pipefail

UsageError=2

Mode="static"
Installed=""
case "${1:-}" in
    --installed)
        Mode="installed"
        Installed="${2:-}"
        [ -n "$Installed" ] || { echo "FAIL: --installed needs a formatter" >&2; exit "$UsageError"; }
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

Trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

# Parse a declaration into DeclaredVersions / DeclaredBinaries (newline-terminated rows).
# $1 = the declaration. Prints the refusal and returns 1 when it cannot be used.
ReadDeclaration() {
    local file="$1" line key value lineno=0
    DeclaredVersions=""
    DeclaredBinaries=""
    if [ ! -f "$file" ]; then
        echo "FAIL: $file does not exist, so no clang-format build is declared and the format-on-edit hook falls back to whatever clang-format PATH finds first (#1349)"
        return 1
    fi
    while IFS= read -r line || [ -n "$line" ]; do
        lineno=$((lineno + 1))
        line="$(Trim "${line%$'\r'}")"
        case "$line" in
            ''|'#'*) continue ;;
            *:*) ;;
            *) echo "FAIL: $file line $lineno is not 'key: value': $line"; return 1 ;;
        esac
        key="$(Trim "${line%%:*}")"
        value="$(Trim "${line#*:}")"
        if [ -z "$value" ]; then
            echo "FAIL: $file line $lineno gives '$key' no value"
            return 1
        fi
        case "$key" in
            version) DeclaredVersions="$DeclaredVersions$value"$'\n' ;;
            binary)
                # The backslash is QUOTED: an unquoted `*\\*` matches no backslash on
                # bash 3.2 or 5.2, silently.
                case "$value" in
                    */*|*"\\"*)
                        echo "FAIL: $file line $lineno: binary '$value' is a path; the hook resolves a command NAME on PATH and refuses a path"
                        return 1
                        ;;
                    clang-format|clang-format-*) ;;
                    *)
                        echo "FAIL: $file line $lineno: binary '$value' is not clang-format or clang-format-*, which the hook refuses"
                        return 1
                        ;;
                esac
                DeclaredBinaries="$DeclaredBinaries$value"$'\n'
                ;;
            *)
                echo "FAIL: $file line $lineno has unknown key '$key' (known: version, binary); the hook refuses the whole file on it"
                return 1
                ;;
        esac
    done < "$file"
    if [ -z "$DeclaredVersions" ]; then
        echo "FAIL: $file has no 'version:' line, which the hook reads as an unusable declaration and formats nothing"
        return 1
    fi
    return 0
}

# The pinned major, read from the one place CI states it. $1 = build.yml
ReadPinnedMajor() {
    [ -f "$1" ] || return 0
    sed -n 's/^[[:space:]]*CLANG_TOOLS_VERSION:[[:space:]]*"\{0,1\}\([0-9][0-9]*\)"\{0,1\}[[:space:]]*$/\1/p' "$1" | sed -n '1p'
}

# $1 = source dir. The declaration against the workflow's pin.
CheckStatic() {
    local dir="$1" pinned row major bad=0
    pinned="$(ReadPinnedMajor "$dir/.github/workflows/build.yml")"
    if [ -z "$pinned" ]; then
        echo "FAIL: could not read CLANG_TOOLS_VERSION from $dir/.github/workflows/build.yml, so there is no pin to hold the declaration to"
        return 1
    fi
    ReadDeclaration "$dir/.clang-format-version" || return 1
    while IFS= read -r row; do
        [ -n "$row" ] || continue
        major="$(printf '%s\n' "$row" | sed -n 's/.*clang-format version \([0-9][0-9]*\)\..*/\1/p')"
        if [ -z "$major" ]; then
            echo "FAIL: declared version '$row' is not a clang-format --version line (no 'clang-format version N.')"
            bad=1
        elif [ "$major" != "$pinned" ]; then
            echo "FAIL: declared version '$row' is clang-format $major, but build.yml pins CLANG_TOOLS_VERSION $pinned."
            echo "      Changing the pin changes the formatter Check C++ style runs, so the declaration changes in the same"
            echo "      commit: take its version line from that job's log, where the next run prints the build it installed."
            bad=1
        fi
    done <<< "$DeclaredVersions"
    case $'\n'"$DeclaredBinaries" in
        *$'\n'"clang-format-$pinned"$'\n'*) ;;
        *)
            echo "FAIL: $dir/.clang-format-version declares no 'binary: clang-format-$pinned', the name build.yml and local-gate.sh run,"
            echo "      so the format-on-edit hook would only ever try a bare clang-format"
            bad=1
            ;;
    esac
    [ "$bad" -eq 0 ] || return 1
    echo "ok: .clang-format-version declares clang-format $pinned, matching CLANG_TOOLS_VERSION, and names clang-format-$pinned"
    return 0
}

# $1 = the formatter, $2 = source dir. The installed build against the declaration.
CheckInstalled() {
    local exe="$1" dir="$2" out line banner="" row
    ReadDeclaration "$dir/.clang-format-version" || return 1
    if ! command -v "$exe" >/dev/null 2>&1; then
        echo "FAIL: '$exe' is not installed here, so which build it is cannot be asked"
        return 1
    fi
    out="$("$exe" --version 2>/dev/null)" || true
    while IFS= read -r line; do
        line="$(Trim "${line%$'\r'}")"
        if [ -n "$line" ]; then
            banner="$line"
            break
        fi
    done <<< "$out"
    if [ -z "$banner" ]; then
        echo "FAIL: '$exe --version' printed nothing, so its build is unknown"
        return 1
    fi
    while IFS= read -r row; do
        [ -n "$row" ] || continue
        if [ "$banner" = "$row" ]; then
            echo "ok: $exe is the declared build: $banner"
            return 0
        fi
    done <<< "$DeclaredVersions"
    echo "FAIL: $exe is not a build .clang-format-version declares."
    echo "      installed: $banner"
    printf '%s' "$DeclaredVersions" | sed 's/^/      declared:  /'
    echo "      Same major, different build, is the case this exists for: two snapshots of one major can format one"
    echo "      file differently, and the format-on-edit hook writes with the declared build only (#1349)."
    echo "      If this is the Check C++ style job and nothing in the tree changed, apt.llvm.org shipped a new snapshot"
    echo "      and the declaration is stale for EVERY branch, not this one: replace its version line with the"
    echo "      'installed' line above in a change of its own, then upgrade local formatters to match"
    echo "      (apt-get install --only-upgrade clang-format-<major>)."
    echo "      If this is a developer machine, it is on a different snapshot from CI. OLDER (compare the"
    echo "      timestamps after '++'): upgrade it the same way. NEWER: apt.llvm.org has moved and CI will say"
    echo "      so on its next run of that job; until the declaration follows, check formatting with"
    echo "      --dry-run rather than -i. Either way, do not edit the declaration to match a local binary --"
    echo "      it states what CI runs, and only that job's log can say what that is."
    return 1
}

# --- self-test -----------------------------------------------------------------
if [ "$Mode" = "self-test" ]; then
    Work="$(mktemp -d "${TMPDIR:-/tmp}/clang-format-version-selftest.XXXXXX")" || {
        echo "FAIL: mktemp failed" >&2
        exit "$UsageError"
    }
    trap 'rm -rf "$Work"' EXIT
    Snapshot='Ubuntu clang-format version 22.1.8 (++20260714014902+ca7933e47d3a-1~exp1~20260714135019.80)'
    Bundled='clang-format version 22.1.3 (https://github.com/llvm/llvm-project e9846648fd6183ee6d8cbdb4502213fcf902a211)'
    Cases=0
    Failed=0

    # $1 = dir, $2 = pin (empty for no line), remaining = declaration lines (none: no file)
    Tree() {
        local dir="$1" pin="$2"
        shift 2
        mkdir -p "$dir/.github/workflows"
        if [ -n "$pin" ]; then
            printf 'env:\n  CLANG_TOOLS_VERSION: "%s"\n' "$pin" > "$dir/.github/workflows/build.yml"
        else
            printf 'env:\n  OTHER: "1"\n' > "$dir/.github/workflows/build.yml"
        fi
        if [ $# -gt 0 ]; then
            printf '%s\n' "$@" > "$dir/.clang-format-version"
        fi
    }
    # $1 = path, $2 = banner, $3 = "crlf" for a CRLF line ending
    Fake() {
        mkdir -p "$(dirname "$1")"
        printf '%s\n' "$2" > "$1.banner"
        if [ "${3:-}" = crlf ]; then
            printf '%s\r\n' "$2" > "$1.banner"
        fi
        cat > "$1" <<'FAKE'
#!/bin/sh
cat "$0.banner"
FAKE
        chmod +x "$1"
    }
    # $1 = label, $2 = want (pass|refuse), $3 = required output fragment, rest = arguments
    # to this script, run as a child so each case gets a fresh process
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
        else
            case "$output" in
                *"$fragment"*) echo "ok: self-test '$label' ($want)" ;;
                *)
                    echo "FAIL: self-test '$label': $want as wanted, but for another reason -- no '$fragment' in:"
                    printf '%s\n' "$output" | sed 's/^/      | /'
                    Failed=$((Failed + 1))
                    ;;
            esac
        fi
    }
    Self="$0"

    Tree "$Work/good" 22 "# CI's formatter" "version: $Snapshot" "binary: clang-format-22"
    Expect "a declaration naming the pinned major and binary" pass "matching CLANG_TOOLS_VERSION" "$Work/good"

    Tree "$Work/bumped" 23 "version: $Snapshot" "binary: clang-format-22"
    Expect "the pin moved and the declaration did not" refuse "but build.yml pins CLANG_TOOLS_VERSION 23" "$Work/bumped"

    Tree "$Work/nobinary" 22 "version: $Snapshot"
    Expect "no clang-format-<major> binary row" refuse "declares no 'binary: clang-format-22'" "$Work/nobinary"

    Tree "$Work/absent" 22
    Expect "no declaration at all" refuse "does not exist" "$Work/absent"

    Tree "$Work/noversion" 22 "binary: clang-format-22"
    Expect "a declaration with no version line" refuse "no 'version:' line" "$Work/noversion"

    Tree "$Work/typo" 22 "verison: $Snapshot" "binary: clang-format-22"
    Expect "an unknown key" refuse "unknown key 'verison'" "$Work/typo"

    Tree "$Work/path" 22 "version: $Snapshot" "binary: /usr/bin/clang-format-22"
    Expect "a binary given as a path" refuse "is a path" "$Work/path"

    Tree "$Work/winpath" 22 "version: $Snapshot" 'binary: clang-format-22\..\clang-format'
    Expect "a binary given as a backslashed path" refuse "is a path" "$Work/winpath"

    Tree "$Work/notformatter" 22 "version: $Snapshot" "binary: clang-tidy-22"
    Expect "a binary that is not a formatter" refuse "is not clang-format or clang-format-*" "$Work/notformatter"

    Tree "$Work/notbanner" 22 "version: 22.1.8" "binary: clang-format-22"
    Expect "a version that is not a --version line" refuse "is not a clang-format --version line" "$Work/notbanner"

    Tree "$Work/nopin" "" "version: $Snapshot" "binary: clang-format-22"
    Expect "a workflow with no CLANG_TOOLS_VERSION" refuse "could not read CLANG_TOOLS_VERSION" "$Work/nopin"

    Fake "$Work/bin/declared" "$Snapshot"
    Expect "--installed: the declared build" pass "is the declared build" --installed "$Work/bin/declared" "$Work/good"

    Fake "$Work/bin/crlf" "$Snapshot" crlf
    Expect "--installed: the declared build, CRLF-terminated" pass "is the declared build" --installed "$Work/bin/crlf" "$Work/good"

    # #1349 itself: the same major, a different build.
    Fake "$Work/bin/bundled" "$Bundled"
    Expect "--installed: same major, different build" refuse "installed: $Bundled" --installed "$Work/bin/bundled" "$Work/good"

    Fake "$Work/bin/prefix" "${Snapshot%% (*}"
    Expect "--installed: the declared number without its build suffix" refuse "is not a build .clang-format-version declares" --installed "$Work/bin/prefix" "$Work/good"

    Expect "--installed: a formatter that is not installed" refuse "is not installed here" --installed "$Work/bin/missing" "$Work/good"

    Expect "the shipped declaration" pass "matching CLANG_TOOLS_VERSION" "$SourceDir"

    echo "self-test: $Cases case(s) ran, $Failed failed"
    [ "$Cases" -gt 0 ] || { echo "FAIL: no self-test case ran"; exit 1; }
    [ "$Failed" -eq 0 ]
    exit $?
fi

case "$Mode" in
    static) CheckStatic "$SourceDir" ;;
    installed) CheckInstalled "$Installed" "$SourceDir" ;;
esac
