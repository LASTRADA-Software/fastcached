#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The clang-format BUILD this tree is formatted by is one exact release, stated once, and
# everything that formats agrees with it (#1349).
#
# ## Why a build, and why this one
#
# `Check C++ style` used to install `clang-format-$CLANG_TOOLS_VERSION` from apt.llvm.org,
# which pins a MAJOR and ships rolling snapshots under it -- so the formatter CI judged with
# changed whenever the mirror did, while every developer machine kept whichever snapshot it
# last installed. Two builds of one major can format one file differently, and a formatter
# at the wrong build REWRITES code the style job accepted, in a diff every line of which is
# "just formatting". The contour-workflows format-on-edit hook did exactly that on every
# C++ edit, with Visual Studio's 22.1.3 against CI's 22.1.8 snapshot, and the remedy read as
# already applied: there IS a binary called clang-format.
#
# So the formatter is now PyPI's clang-format at `CLANG_FORMAT_VERSION`
# (`.github/workflows/build.yml`), the ONE place its version is stated. A PyPI release is one
# build with one banner -- `clang-format version 22.1.8` -- installable natively on Linux,
# macOS and Windows, and measured to format identically on the first and last of those.
# `.clang-format-version` declares that banner for the tools that cannot read a workflow: the
# hook writes only with a binary whose `--version` line is declared, character for
# character, and `local-gate.sh` formats only with one.
#
# clang-tidy is NOT this pin and nothing here checks it: it stays on apt at
# `CLANG_TOOLS_VERSION`, whose banner cannot tell its snapshots apart at all.
#
# ## Four modes, because they are four questions
#
#   (default)             Does the declaration say EXACTLY the pin -- the pinned banner is
#                         declared, and nothing else is? Reads two files, needs no
#                         formatter, runs in the default ctest set, so a bump of one
#                         without the other is refused on the pull request that makes it.
#                         Both directions, because a declaration still naming the old build
#                         beside the new one lets the hook write with the old one.
#
#   --installed <exe>     Is <exe> a declared build? The `Check C++ style` job asks it of
#                         the formatter it just installed, before formatting, so every green
#                         run proves the declaration. Not a ctest: no build leg installs the
#                         formatter, so it would skip wherever it ran (#1135).
#
#   --resolve             Which clang-format on this machine is the declared build? Prints
#                         its path. Candidates are each declared `binary:` name and then
#                         `clang-format`, and for each name EVERY match on PATH, exactly as
#                         the hook resolves -- so a correct build behind an IDE-bundled one
#                         is still found. None: exit 77, with the install commands for the
#                         pin. `local-gate.sh`, `Check C++ style` and the windows.h check
#                         all format through this, so there is one answer to "which binary".
#
#   --self-test           Every verdict above, against synthetic trees and fake formatters.
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
#   bash scripts/check-clang-format-version.sh --resolve [<source-dir>]
#   bash scripts/check-clang-format-version.sh --self-test [<source-dir>]

set -uo pipefail

UsageError=2
NoDeclaredBuild=77

Mode="static"
Installed=""
case "${1:-}" in
    --installed)
        Mode="installed"
        Installed="${2:-}"
        [ -n "$Installed" ] || { echo "FAIL: --installed needs a formatter" >&2; exit "$UsageError"; }
        shift 2
        ;;
    --resolve)
        Mode="resolve"
        shift
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

# Set `Trimmed` to $1 without leading or trailing whitespace.
Trim() {
    Trimmed="$1"
    Trimmed="${Trimmed#"${Trimmed%%[![:space:]]*}"}"
    Trimmed="${Trimmed%"${Trimmed##*[![:space:]]}"}"
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
        Trim "${line%$'\r'}"
        line="$Trimmed"
        case "$line" in
            ''|'#'*) continue ;;
            *:*) ;;
            *) echo "FAIL: $file line $lineno is not 'key: value': $line"; return 1 ;;
        esac
        Trim "${line%%:*}"
        key="$Trimmed"
        Trim "${line#*:}"
        value="$Trimmed"
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

# Set `Pin` to CLANG_FORMAT_VERSION from $1 (build.yml), or print why not and return 1.
ReadPin() {
    Pin=""
    if [ -f "$1" ]; then
        Pin="$(sed -n 's/^[[:space:]]*CLANG_FORMAT_VERSION:[[:space:]]*"\{0,1\}\([^"[:space:]]*\)"\{0,1\}[[:space:]]*$/\1/p' "$1" | sed -n '1p')"
    fi
    if [ -z "$Pin" ]; then
        echo "FAIL: could not read CLANG_FORMAT_VERSION from $1, so there is no pinned formatter build to hold anything to"
        return 1
    fi
    case "$Pin" in
        *[!0-9.]*|.*|*.|*..*)
            echo "FAIL: CLANG_FORMAT_VERSION '$Pin' in $1 is not an exact X.Y.Z release"
            return 1
            ;;
    esac
    case "$Pin" in
        *.*.*) ;;
        *)
            echo "FAIL: CLANG_FORMAT_VERSION '$Pin' in $1 is not an exact X.Y.Z release -- a major or a minor names a family of builds, which is #1349"
            return 1
            ;;
    esac
    return 0
}

# Set `Banner` to the first non-empty line of `$1 --version`, without a carriage return.
BannerOf() {
    local out line
    Banner=""
    out="$("$1" --version 2>/dev/null)" || true
    while IFS= read -r line; do
        Trim "${line%$'\r'}"
        if [ -n "$Trimmed" ]; then
            Banner="$Trimmed"
            return 0
        fi
    done <<< "$out"
    return 0
}

# Return 0 when $1 is a declared banner.
IsDeclared() {
    local row
    while IFS= read -r row; do
        [ -n "$row" ] || continue
        [ "$1" = "$row" ] && return 0
    done <<< "$DeclaredVersions"
    return 1
}

# How to get the pinned build, and what that does not cover. $1 = the pin.
PrintRemedy() {
    local pin="$1" venv="\$HOME/.local/share/clang-format-$1"
    echo "  Install PyPI's clang-format $pin -- the build Check C++ style runs -- with ONE of:"
    echo "    pipx install clang-format==$pin"
    echo "    uv tool install clang-format==$pin"
    echo "    python3 -m pip install --user clang-format==$pin"
    echo "  The last is refused as externally-managed (PEP 668) by Ubuntu 24.04's and Homebrew's python;"
    echo "  there, without pipx or uv, use a venv and put its bin/ on PATH:"
    echo "    python3 -m venv \"$venv\" && \"$venv/bin/pip\" install clang-format==$pin"
    echo "  (On Debian and Ubuntu a python without pip or venv gets them from apt: pipx, or python3-venv.)"
    echo "  Any clang-format on PATH that prints exactly 'clang-format version $pin' is used; other"
    echo "  clang-formats may stay installed, and nothing is uninstalled or re-pointed."
    echo "  NOT covered: clang-tidy, which stays on apt at CLANG_TOOLS_VERSION and is not checked here;"
    echo "  and which clang-format any tool other than this tree's formatting reaches for."
}

# $1 = source dir. The declaration against the pin, both directions.
CheckStatic() {
    local dir="$1" expected row bad=0
    ReadPin "$dir/.github/workflows/build.yml" || return 1
    ReadDeclaration "$dir/.clang-format-version" || return 1
    expected="clang-format version $Pin"
    if ! IsDeclared "$expected"; then
        echo "FAIL: $dir/.clang-format-version does not declare '$expected', the banner of PyPI's"
        echo "      clang-format==$Pin that CLANG_FORMAT_VERSION pins. The format-on-edit hook and local-gate.sh"
        echo "      would refuse the formatter Check C++ style runs. Change the version line to that banner, in the"
        echo "      same commit as the pin."
        bad=1
    fi
    while IFS= read -r row; do
        [ -n "$row" ] || continue
        if [ "$row" != "$expected" ]; then
            echo "FAIL: $dir/.clang-format-version declares '$row', which is not the pinned build '$expected'."
            echo "      A second accepted build is one the hook would WRITE with while CI judges with the pin, which is"
            echo "      #1349 by declaration. Remove the line."
            bad=1
        fi
    done <<< "$DeclaredVersions"
    [ "$bad" -eq 0 ] || return 1
    echo "ok: .clang-format-version declares exactly '$expected', the build CLANG_FORMAT_VERSION pins"
    return 0
}

# $1 = the formatter, $2 = source dir. The installed build against the declaration.
CheckInstalled() {
    local exe="$1" dir="$2"
    ReadDeclaration "$dir/.clang-format-version" || return 1
    if ! command -v "$exe" >/dev/null 2>&1; then
        echo "FAIL: '$exe' is not installed here, so which build it is cannot be asked"
        return 1
    fi
    BannerOf "$exe"
    if [ -z "$Banner" ]; then
        echo "FAIL: '$exe --version' printed nothing, so its build is unknown"
        return 1
    fi
    if IsDeclared "$Banner"; then
        echo "ok: $exe is the declared build: $Banner"
        return 0
    fi
    echo "FAIL: $exe is not a build .clang-format-version declares."
    echo "      installed: $Banner"
    printf '%s' "$DeclaredVersions" | sed 's/^/      declared:  /'
    echo "      The same release number from another source is still another build; the format-on-edit hook"
    echo "      and local-gate.sh write with the declared build only (#1349). If this is Check C++ style, the"
    echo "      install step did not install CLANG_FORMAT_VERSION from PyPI, or the declaration does not match"
    echo "      the pin -- which ctest -R clang-format-version would also refuse."
    return 1
}

# $1 = source dir. Print the declared build's path on stdout; everything else on stderr.
Resolve() {
    local dir="$1" names name paths candidate seen probed="" pin
    ReadDeclaration "$dir/.clang-format-version" >&2 || return 1
    names="${DeclaredBinaries}clang-format"
    seen=$'\n'
    while IFS= read -r name; do
        [ -n "$name" ] || continue
        paths="$(type -ap "$name" 2>/dev/null)" || true
        while IFS= read -r candidate; do
            [ -n "$candidate" ] || continue
            case "$seen" in
                *$'\n'"$candidate"$'\n'*) continue ;;
            esac
            seen="$seen$candidate"$'\n'
            BannerOf "$candidate"
            if IsDeclared "$Banner"; then
                echo "ok: $candidate is the declared build: $Banner" >&2
                printf '%s\n' "$candidate"
                return 0
            fi
            probed="$probed      $candidate: ${Banner:-<no version output>}"$'\n'
        done <<< "$paths"
    done <<< "$names"
    {
        echo "NONE: no clang-format on PATH is the build .clang-format-version declares."
        printf '%s' "$DeclaredVersions" | sed 's/^/      declared:  /'
        if [ -n "$probed" ]; then
            echo "      found:"
            printf '%s' "$probed"
        else
            echo "      found:     no executable named any of: ${names//$'\n'/ }"
        fi
        if ReadPin "$dir/.github/workflows/build.yml" > /dev/null; then
            PrintRemedy "$Pin"
        else
            echo "  And CLANG_FORMAT_VERSION could not be read, so no install command can be named."
        fi
    } >&2
    return "$NoDeclaredBuild"
}

# --- self-test -----------------------------------------------------------------
if [ "$Mode" = "self-test" ]; then
    Work="$(mktemp -d "${TMPDIR:-/tmp}/clang-format-version-selftest.XXXXXX")" || {
        echo "FAIL: mktemp failed" >&2
        exit "$UsageError"
    }
    trap 'rm -rf "$Work"' EXIT
    Pypi='clang-format version 22.1.8'
    Snapshot='Ubuntu clang-format version 22.1.8 (++20260714014902+ca7933e47d3a-1~exp1~20260714135019.80)'
    Bundled='clang-format version 22.1.3 (https://github.com/llvm/llvm-project e9846648fd6183ee6d8cbdb4502213fcf902a211)'
    Cases=0
    Failed=0
    Self="$0"
    Interpreter="${BASH:-bash}"

    # The resolve cases must not see this host's own formatters, or a machine that has
    # the pinned build installed would answer a case that expects none. So they run with
    # PATH holding only fake formatters and shims for the tools this script calls.
    Tools="$Work/tools"
    mkdir -p "$Tools"
    for tool in sed cat mktemp rm dirname mkdir chmod; do
        real="$(command -v "$tool" 2>/dev/null)" || { echo "FAIL: self-test needs $tool"; exit "$UsageError"; }
        printf '#!/bin/sh\nexec "%s" "$@"\n' "$real" > "$Tools/$tool"
        chmod +x "$Tools/$tool"
    done

    # $1 = dir, $2 = pin line value (empty for no line), remaining = declaration lines
    # (none: no file)
    Tree() {
        local dir="$1" pin="$2"
        shift 2
        mkdir -p "$dir/.github/workflows"
        if [ -n "$pin" ]; then
            printf 'env:\n  CLANG_TOOLS_VERSION: "22"\n  CLANG_FORMAT_VERSION: "%s"\n' "$pin" > "$dir/.github/workflows/build.yml"
        else
            printf 'env:\n  CLANG_TOOLS_VERSION: "22"\n' > "$dir/.github/workflows/build.yml"
        fi
        if [ $# -gt 0 ]; then
            printf '%s\n' "$@" > "$dir/.clang-format-version"
        fi
    }
    # $1 = path, $2 = banner, $3 = "crlf" for a CRLF line ending
    Fake() {
        mkdir -p "${1%/*}"
        if [ "${3:-}" = crlf ]; then
            printf '%s\r\n' "$2" > "$1.banner"
        else
            printf '%s\n' "$2" > "$1.banner"
        fi
        cat > "$1" <<'FAKE'
#!/bin/sh
cat "$0.banner"
FAKE
        chmod +x "$1"
    }
    # $1 = label, $2 = want (pass|refuse|none), $3 = required output fragment,
    # $4 = required exact stdout ("-" for any), $5 = PATH for the child ("-" for this
    # one's), rest = arguments to this script, run as a child so each case is a fresh
    # process. `none` is the resolve mode's exit 77.
    Expect() {
        local label="$1" want="$2" fragment="$3" stdout="$4" path="$5" status got out all
        shift 5
        Cases=$((Cases + 1))
        [ "$path" = "-" ] && path="$PATH"
        out="$(PATH="$path" "$Interpreter" "$Self" "$@" 2>"$Work/stderr")"
        status=$?
        all="$out
$(cat "$Work/stderr")"
        case "$status" in
            0) got="pass" ;;
            "$NoDeclaredBuild") got="none" ;;
            *) got="refuse" ;;
        esac
        if [ "$got" != "$want" ]; then
            echo "FAIL: self-test '$label': wanted $want, got $got (exit $status)"
            printf '%s\n' "$all" | sed 's/^/      | /'
            Failed=$((Failed + 1))
            return
        fi
        case "$all" in
            *"$fragment"*) ;;
            *)
                echo "FAIL: self-test '$label': $want as wanted, but for another reason -- no '$fragment' in:"
                printf '%s\n' "$all" | sed 's/^/      | /'
                Failed=$((Failed + 1))
                return
                ;;
        esac
        if [ "$stdout" != "-" ] && [ "$out" != "$stdout" ]; then
            echo "FAIL: self-test '$label': stdout was '$out', wanted exactly '$stdout'"
            Failed=$((Failed + 1))
            return
        fi
        echo "ok: self-test '$label' ($want)"
    }

    # -- the declaration against the pin, both directions --------------------------
    Tree "$Work/good" 22.1.8 "# CI's formatter" "version: $Pypi"
    Expect "the declaration is exactly the pinned banner" pass "declares exactly 'clang-format version 22.1.8'" - - "$Work/good"

    # The positive control: a planted mismatch must be refused.
    Tree "$Work/planted" 22.1.8 "version: clang-format version 22.1.3"
    Expect "planted mismatch: the declaration names another release" refuse "does not declare 'clang-format version 22.1.8'" - - "$Work/planted"

    Tree "$Work/bumped" 23.1.1 "version: $Pypi"
    Expect "the pin moved and the declaration did not" refuse "does not declare 'clang-format version 23.1.1'" - - "$Work/bumped"

    Tree "$Work/extra" 22.1.8 "version: $Pypi" "version: $Snapshot"
    Expect "the pinned banner plus a second build" refuse "declares '$Snapshot', which is not the pinned build" - - "$Work/extra"

    Tree "$Work/snapshot" 22.1.8 "version: $Snapshot"
    Expect "the same release number from apt.llvm.org" refuse "does not declare 'clang-format version 22.1.8'" - - "$Work/snapshot"

    Tree "$Work/absent" 22.1.8
    Expect "no declaration at all" refuse "does not exist" - - "$Work/absent"

    Tree "$Work/noversion" 22.1.8 "binary: clang-format"
    Expect "a declaration with no version line" refuse "no 'version:' line" - - "$Work/noversion"

    Tree "$Work/typo" 22.1.8 "verison: $Pypi"
    Expect "an unknown key" refuse "unknown key 'verison'" - - "$Work/typo"

    Tree "$Work/path" 22.1.8 "version: $Pypi" "binary: /usr/bin/clang-format"
    Expect "a binary given as a path" refuse "is a path" - - "$Work/path"

    Tree "$Work/winpath" 22.1.8 "version: $Pypi" 'binary: clang-format-22\..\clang-format'
    Expect "a binary given as a backslashed path" refuse "is a path" - - "$Work/winpath"

    Tree "$Work/notformatter" 22.1.8 "version: $Pypi" "binary: clang-tidy"
    Expect "a binary that is not a formatter" refuse "is not clang-format or clang-format-*" - - "$Work/notformatter"

    Tree "$Work/nopin" "" "version: $Pypi"
    Expect "a workflow with no CLANG_FORMAT_VERSION" refuse "could not read CLANG_FORMAT_VERSION" - - "$Work/nopin"

    Tree "$Work/majorpin" 22 "version: $Pypi"
    Expect "a pin naming a major, not a release" refuse "is not an exact X.Y.Z release" - - "$Work/majorpin"

    # -- the installed formatter against the declaration -----------------------------
    Fake "$Work/bin/declared" "$Pypi"
    Expect "--installed: the declared build" pass "is the declared build" - - --installed "$Work/bin/declared" "$Work/good"

    Fake "$Work/bin/crlf" "$Pypi" crlf
    Expect "--installed: the declared build, CRLF-terminated" pass "is the declared build" - - --installed "$Work/bin/crlf" "$Work/good"

    # #1349 itself, both ways round: the same release number from another source.
    Fake "$Work/bin/snapshot" "$Snapshot"
    Expect "--installed: the apt snapshot of the pinned number" refuse "installed: $Snapshot" - - --installed "$Work/bin/snapshot" "$Work/good"

    Fake "$Work/bin/bundled" "$Bundled"
    Expect "--installed: an IDE-bundled build" refuse "installed: $Bundled" - - --installed "$Work/bin/bundled" "$Work/good"

    Expect "--installed: a formatter that is not installed" refuse "is not installed here" - - --installed "$Work/bin/missing" "$Work/good"

    # -- resolving the declared build among everything on PATH ----------------------
    Fake "$Work/path-first/clang-format" "$Bundled"
    Fake "$Work/path-second/clang-format" "$Pypi"
    Expect "--resolve: the declared build behind another clang-format" pass "is the declared build" \
        "$Work/path-second/clang-format" "$Work/path-first:$Work/path-second:$Tools" --resolve "$Work/good"

    Expect "--resolve: no declared build, with the remedy" none "pipx install clang-format==22.1.8" \
        "" "$Work/path-first:$Tools" --resolve "$Work/good"
    Expect "--resolve: the remedy says what it does not cover" none "NOT covered: clang-tidy" \
        "" "$Work/path-first:$Tools" --resolve "$Work/good"
    Expect "--resolve: what was found is named" none "$Work/path-first/clang-format: $Bundled" \
        "" "$Work/path-first:$Tools" --resolve "$Work/good"
    Expect "--resolve: nothing named clang-format at all" none "no executable named any of: clang-format" \
        "" "$Tools" --resolve "$Work/good"

    Tree "$Work/named" 22.1.8 "version: $Pypi" "binary: clang-format-pinned"
    Fake "$Work/path-named/clang-format-pinned" "$Pypi"
    Expect "--resolve: a declared binary name" pass "is the declared build" \
        "$Work/path-named/clang-format-pinned" "$Work/path-first:$Work/path-named:$Tools" --resolve "$Work/named"

    Expect "--resolve: an unusable declaration is a refusal, not 'none found'" refuse "unknown key 'verison'" \
        "" "$Work/path-second:$Tools" --resolve "$Work/typo"

    # -- the tree this script ships in -------------------------------------------------
    Expect "the shipped declaration" pass "the build CLANG_FORMAT_VERSION pins" - - "$SourceDir"

    echo "self-test: $Cases case(s) ran, $Failed failed"
    [ "$Cases" -gt 0 ] || { echo "FAIL: no self-test case ran"; exit 1; }
    [ "$Failed" -eq 0 ]
    exit $?
fi

case "$Mode" in
    static) CheckStatic "$SourceDir" ;;
    installed) CheckInstalled "$Installed" "$SourceDir" ;;
    resolve) Resolve "$SourceDir" ;;
esac
