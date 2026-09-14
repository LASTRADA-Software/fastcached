#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The clang-format BUILD this tree is formatted by is one exact release, stated once in
# `.clang-format-version`, and everything that formats agrees with it (#1349, #1407).
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
# So the formatter is PyPI's clang-format release, which is one build with one banner --
# `clang-format version 22.1.8` -- installable natively on Linux, macOS and Windows. Measured
# (#1407): over all 778 tracked first-party C++ files, as they are and with indentation
# stripped, the Linux wheel, the Windows wheel and the apt snapshot built from the same tag
# produce byte-identical output. `.clang-format-version` is the ONE statement of the release:
# the hook reads its banner, and `--requirement` hands installers the pip requirement derived
# from it, so no workflow restates the version.
#
# The banner is the identity, and for a formatter it is enough: a bare release banner is
# printed only by builds of that release tag, and a formatter needs no resource directory, so
# the copied-binary hazard that makes `check-clang-tidy-version.sh` identify its analyser by
# the wheel's RECORD has no formatter counterpart. clang-tidy is declared separately, in
# `.clang-tidy-version`, and nothing here examines it.
#
# ## Five modes, because they are five questions
#
#   (default)             Does the declaration state exactly ONE build, as a bare release
#                         banner `clang-format version X.Y.Z` -- no vendor prefix, no build
#                         suffix -- whose major is the `CLANG_TOOLS_VERSION` build.yml pins?
#                         Reads two files, needs no formatter, runs in the default ctest set.
#                         A second declared build is one the hook would WRITE with; a snapshot
#                         banner names a build no installer can ask for; and a major other
#                         than the toolchain's is a formatter from another LLVM than the
#                         analyser and the compilers.
#
#   --requirement         Print `clang-format==X.Y.Z` for pip, derived from the declaration,
#                         after the default mode's checks. `Check C++ style` installs this.
#
#   --installed <exe>     Is <exe> a declared build? `Check C++ style` asks it of the formatter
#                         it just installed, before formatting, so every green run proves the
#                         declaration. Not a ctest: no build leg installs the formatter, so it
#                         would skip wherever it ran (#1135).
#
#   --resolve             Which clang-format on this machine is the declared build? Prints its
#                         path. `FASTCACHED_CLANG_FORMAT`, when set, is the only candidate and is
#                         held to the banner like any other -- an override that is not the
#                         declared build is refused, never stepped over. Otherwise each declared
#                         `binary:` name and then `clang-format`, and for each name EVERY match on
#                         PATH, exactly as the hook resolves -- so a correct build behind an
#                         IDE-bundled one is still found. None: exit 77, with the install commands.
#                         `local-gate.sh`, `Check C++ style` and the windows.h check all format
#                         through this, so there is one answer to "which binary".
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
# The default mode is STRICTER than the grammar about what this tree declares (one bare
# release); it never accepts a line the hook would refuse.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs on every platform CI builds.
#
# Usage:
#   bash scripts/check-clang-format-version.sh [<source-dir>]
#   bash scripts/check-clang-format-version.sh --requirement [<source-dir>]
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
    --requirement|--resolve|--self-test)
        Mode="${1#--}"
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

# Set `PinnedMajor` to CLANG_TOOLS_VERSION from $1 (build.yml), or print why not and return 1.
ReadPinnedMajor() {
    PinnedMajor=""
    if [ -f "$1" ]; then
        PinnedMajor="$(sed -n 's/^[[:space:]]*CLANG_TOOLS_VERSION:[[:space:]]*"\{0,1\}\([0-9][0-9]*\)"\{0,1\}[[:space:]]*$/\1/p' "$1" | sed -n '1p')"
    fi
    if [ -z "$PinnedMajor" ]; then
        echo "FAIL: could not read CLANG_TOOLS_VERSION from $1, so there is no toolchain major to hold the formatter to"
        return 1
    fi
    return 0
}

# $1 = source dir. The declaration: one bare release, at the toolchain's major. Sets `Release`
# (X.Y.Z) on success; prints every refusal and returns 1 otherwise.
CheckDeclaration() {
    local dir="$1" count=0 row banner="" major
    Release=""
    ReadPinnedMajor "$dir/.github/workflows/build.yml" || return 1
    ReadDeclaration "$dir/.clang-format-version" || return 1
    while IFS= read -r row; do
        [ -n "$row" ] || continue
        count=$((count + 1))
        banner="$row"
    done <<< "$DeclaredVersions"
    if [ "$count" -ne 1 ]; then
        echo "FAIL: $dir/.clang-format-version declares $count builds. It declares exactly one: every declared build is one"
        echo "      the format-on-edit hook would WRITE with, while Check C++ style judges with the one it installs."
        printf '%s' "$DeclaredVersions" | sed 's/^/      declared:  /'
        return 1
    fi
    case "$banner" in
        "clang-format version "*) Release="${banner#clang-format version }" ;;
    esac
    case "$Release" in
        ""|*[!0-9.]*|.*|*.|*..*) Release="" ;;
        *.*.*.*) Release="" ;;
        *.*.*) ;;
        *) Release="" ;;
    esac
    if [ -z "$Release" ]; then
        echo "FAIL: $dir/.clang-format-version declares '$banner', which is not a bare release banner"
        echo "      'clang-format version X.Y.Z'. A vendor prefix or a build suffix names a distribution's build -- a"
        echo "      snapshot no installer can ask for by name, which goes stale when its mirror moves (#1349) -- and a"
        echo "      shorter version names a family. Declare the banner of PyPI's clang-format release, which is that"
        echo "      bare line on every platform."
        return 1
    fi
    major="${Release%%.*}"
    if [ "$major" != "$PinnedMajor" ]; then
        echo "FAIL: $dir/.clang-format-version declares clang-format $Release, but build.yml pins CLANG_TOOLS_VERSION $PinnedMajor."
        echo "      The formatter moves with the toolchain it formats for: bump both in one change, and take the new"
        echo "      banner from PyPI's clang-format release of that major (its --version line)."
        return 1
    fi
    return 0
}

# $1 = source dir.
CheckStatic() {
    CheckDeclaration "$1" || return 1
    echo "ok: .clang-format-version declares exactly one build, clang-format $Release, whose major is CLANG_TOOLS_VERSION"
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

# How to get the declared build, and what that does not cover. $1 = the release.
PrintRemedy() {
    local release="$1" venv="\$HOME/.local/share/clang-format-$1"
    echo "  Install PyPI's clang-format $release -- the build Check C++ style runs -- with ONE of:"
    echo "    pipx install clang-format==$release"
    echo "    uv tool install clang-format==$release"
    echo "    python3 -m pip install --user clang-format==$release"
    echo "  The last is refused as externally-managed (PEP 668) by Ubuntu 24.04's and Homebrew's python;"
    echo "  there, without pipx or uv, use a venv and put its bin/ on PATH:"
    echo "    python3 -m venv \"$venv\" && \"$venv/bin/pip\" install clang-format==$release"
    echo "  A host whose python has no pip or venv at all can unzip the wheel another host downloads for it"
    echo "  (a wheel is a zip; the binary is clang_format/data/bin/clang-format):"
    echo "    pip download --no-deps --only-binary=:all: --platform manylinux_2_27_x86_64 clang-format==$release -d <dir>"
    echo "  Any clang-format on PATH that prints exactly 'clang-format version $release' is used; other"
    echo "  clang-formats may stay installed, and nothing is uninstalled or re-pointed. FASTCACHED_CLANG_FORMAT"
    echo "  may name the binary instead, for this script and local-gate.sh -- but the format-on-edit hook reads"
    echo "  PATH only, so a build it should write with belongs on PATH."
    echo "  NOT covered: clang-tidy, which .clang-tidy-version declares on its own"
    echo "  (bash scripts/check-clang-tidy-version.sh --resolve); and which clang-format any other tool reaches for."
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
    echo "      install step did not install what --requirement names, which is a defect in that step."
    return 1
}

# $1 = source dir. Print the declared build's path on stdout; everything else on stderr.
Resolve() {
    local dir="$1" names name paths candidate seen probed=""
    ReadDeclaration "$dir/.clang-format-version" >&2 || return 1
    if [ -n "${FASTCACHED_CLANG_FORMAT:-}" ]; then
        # An override is the operator's answer, so it is the only candidate: stepping over a wrong one to
        # something on PATH would format with a binary nobody named.
        if ! command -v "$FASTCACHED_CLANG_FORMAT" >/dev/null 2>&1; then
            echo "FAIL: FASTCACHED_CLANG_FORMAT names '$FASTCACHED_CLANG_FORMAT', which is not an executable here" >&2
            return 1
        fi
        BannerOf "$FASTCACHED_CLANG_FORMAT"
        if IsDeclared "$Banner"; then
            echo "ok: $FASTCACHED_CLANG_FORMAT (FASTCACHED_CLANG_FORMAT) is the declared build: $Banner" >&2
            printf '%s\n' "$FASTCACHED_CLANG_FORMAT"
            return 0
        fi
        {
            echo "FAIL: FASTCACHED_CLANG_FORMAT names '$FASTCACHED_CLANG_FORMAT', which is not the declared build, and an"
            echo "      override is not stepped over to whatever PATH holds."
            echo "      found:     ${Banner:-<no version output>}"
            printf '%s' "$DeclaredVersions" | sed 's/^/      declared:  /'
        } >&2
        return 1
    fi
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
        if CheckDeclaration "$dir" > /dev/null; then
            PrintRemedy "$Release"
        else
            echo "  And the declaration is not one bare release (run this script without arguments), so no install"
            echo "  command can be named."
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
    # No host override may decide a case: every child sees an empty FASTCACHED_CLANG_FORMAT unless one sets it.
    CaseOverride=""

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

    # $1 = dir, $2 = CLANG_TOOLS_VERSION (empty for no line), remaining = declaration lines
    # (none: no file)
    Tree() {
        local dir="$1" major="$2"
        shift 2
        mkdir -p "$dir/.github/workflows"
        if [ -n "$major" ]; then
            printf 'env:\n  CTEST_PARALLEL_LEVEL: 4\n  CLANG_TOOLS_VERSION: "%s"\n' "$major" > "$dir/.github/workflows/build.yml"
        else
            printf 'env:\n  CTEST_PARALLEL_LEVEL: 4\n' > "$dir/.github/workflows/build.yml"
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
    # process. `none` is the resolve mode's exit 77. `CaseOverride` is the child's
    # FASTCACHED_CLANG_FORMAT.
    Expect() {
        local label="$1" want="$2" fragment="$3" stdout="$4" path="$5" status got out all
        shift 5
        Cases=$((Cases + 1))
        [ "$path" = "-" ] && path="$PATH"
        out="$(FASTCACHED_CLANG_FORMAT="$CaseOverride" PATH="$path" "$Interpreter" "$Self" "$@" 2>"$Work/stderr")"
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

    # -- the declaration: one bare release at the toolchain's major -------------------
    Tree "$Work/good" 22 "# CI's formatter" "version: $Pypi"
    Expect "one bare release at the toolchain's major" pass "declares exactly one build, clang-format 22.1.8" - - "$Work/good"

    # The positive control for the major hold: the same declaration against another toolchain.
    Tree "$Work/othermajor" 23 "version: $Pypi"
    Expect "planted mismatch: the declared major is not CLANG_TOOLS_VERSION" refuse "pins CLANG_TOOLS_VERSION 23" - - "$Work/othermajor"

    Tree "$Work/extra" 22 "version: $Pypi" "version: clang-format version 22.1.7"
    Expect "two declared builds" refuse "declares 2 builds" - - "$Work/extra"

    Tree "$Work/snapshot" 22 "version: $Snapshot"
    Expect "the apt snapshot banner of the same number" refuse "is not a bare release banner" - - "$Work/snapshot"

    Tree "$Work/vendor" 22 "version: Homebrew clang-format version 22.1.8"
    Expect "a vendor-prefixed banner" refuse "is not a bare release banner" - - "$Work/vendor"

    Tree "$Work/suffixed" 22 "version: clang-format version 22.1.8 (https://github.com/llvm/llvm-project ca7933e47d3a)"
    Expect "a banner with a build suffix" refuse "is not a bare release banner" - - "$Work/suffixed"

    Tree "$Work/minor" 22 "version: clang-format version 22.1"
    Expect "a banner naming a minor, not a release" refuse "is not a bare release banner" - - "$Work/minor"

    Tree "$Work/fourpart" 22 "version: clang-format version 22.1.8.1"
    Expect "a four-part version" refuse "is not a bare release banner" - - "$Work/fourpart"

    Tree "$Work/withbinary" 22 "version: $Pypi" "binary: clang-format-22"
    Expect "a binary line stays legal, as the hook's grammar has it" pass "declares exactly one build" - - "$Work/withbinary"

    Tree "$Work/absent" 22
    Expect "no declaration at all" refuse "does not exist" - - "$Work/absent"

    Tree "$Work/noversion" 22 "binary: clang-format"
    Expect "a declaration with no version line" refuse "no 'version:' line" - - "$Work/noversion"

    Tree "$Work/typo" 22 "verison: $Pypi"
    Expect "an unknown key" refuse "unknown key 'verison'" - - "$Work/typo"

    Tree "$Work/path" 22 "version: $Pypi" "binary: /usr/bin/clang-format"
    Expect "a binary given as a path" refuse "is a path" - - "$Work/path"

    Tree "$Work/winpath" 22 "version: $Pypi" 'binary: clang-format-22\..\clang-format'
    Expect "a binary given as a backslashed path" refuse "is a path" - - "$Work/winpath"

    Tree "$Work/notformatter" 22 "version: $Pypi" "binary: clang-tidy"
    Expect "a binary that is not a formatter" refuse "is not clang-format or clang-format-*" - - "$Work/notformatter"

    Tree "$Work/nopin" "" "version: $Pypi"
    Expect "a workflow with no CLANG_TOOLS_VERSION" refuse "could not read CLANG_TOOLS_VERSION" - - "$Work/nopin"

    # -- the pip requirement, derived and never restated --------------------------------
    Expect "--requirement: derived from the declaration" pass "clang-format==22.1.8" "clang-format==22.1.8" - --requirement "$Work/good"
    Expect "--requirement: nothing for a declaration the default mode refuses" refuse "is not a bare release banner" "" - --requirement "$Work/snapshot"
    Expect "--requirement: nothing for a major the toolchain does not pin" refuse "pins CLANG_TOOLS_VERSION 23" "" - --requirement "$Work/othermajor"

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
    Expect "--resolve: the remedy reaches a host with no pip" none "--platform manylinux_2_27_x86_64 clang-format==22.1.8" \
        "" "$Work/path-first:$Tools" --resolve "$Work/good"
    Expect "--resolve: the remedy says what it does not cover" none "NOT covered: clang-tidy, which .clang-tidy-version declares" \
        "" "$Work/path-first:$Tools" --resolve "$Work/good"
    Expect "--resolve: what was found is named" none "$Work/path-first/clang-format: $Bundled" \
        "" "$Work/path-first:$Tools" --resolve "$Work/good"
    Expect "--resolve: nothing named clang-format at all" none "no executable named any of: clang-format" \
        "" "$Tools" --resolve "$Work/good"

    Tree "$Work/named" 22 "version: $Pypi" "binary: clang-format-pinned"
    Fake "$Work/path-named/clang-format-pinned" "$Pypi"
    Expect "--resolve: a declared binary name" pass "is the declared build" \
        "$Work/path-named/clang-format-pinned" "$Work/path-first:$Work/path-named:$Tools" --resolve "$Work/named"

    Expect "--resolve: an unusable declaration is a refusal, not 'none found'" refuse "unknown key 'verison'" \
        "" "$Work/path-second:$Tools" --resolve "$Work/typo"

    # The override: the declared build outside PATH is used; a wrong one is refused even with the right one on PATH.
    Fake "$Work/elsewhere/clang-format" "$Pypi"
    CaseOverride="$Work/elsewhere/clang-format"
    Expect "--resolve: FASTCACHED_CLANG_FORMAT names the declared build outside PATH" pass "(FASTCACHED_CLANG_FORMAT) is the declared build" \
        "$Work/elsewhere/clang-format" "$Work/path-first:$Tools" --resolve "$Work/good"
    CaseOverride="$Work/path-first/clang-format"
    Expect "--resolve: FASTCACHED_CLANG_FORMAT names another build, and PATH is not consulted" refuse "is not stepped over" \
        "" "$Work/path-second:$Tools" --resolve "$Work/good"
    CaseOverride="$Work/elsewhere/absent"
    Expect "--resolve: FASTCACHED_CLANG_FORMAT names nothing" refuse "which is not an executable here" \
        "" "$Work/path-second:$Tools" --resolve "$Work/good"
    CaseOverride=""

    # -- the tree this script ships in -------------------------------------------------
    Expect "the shipped declaration" pass "whose major is CLANG_TOOLS_VERSION" - - "$SourceDir"
    Expect "the shipped declaration's requirement" pass "clang-format==" - - --requirement "$SourceDir"

    echo "self-test: $Cases case(s) ran, $Failed failed"
    [ "$Cases" -gt 0 ] || { echo "FAIL: no self-test case ran"; exit 1; }
    [ "$Failed" -eq 0 ]
    exit $?
fi

case "$Mode" in
    static) CheckStatic "$SourceDir" ;;
    requirement)
        # The requirement alone on stdout, so an installer can take it whole; every refusal on stderr.
        CheckDeclaration "$SourceDir" >&2 || exit 1
        echo "clang-format==$Release"
        ;;
    installed) CheckInstalled "$Installed" "$SourceDir" ;;
    resolve) Resolve "$SourceDir" ;;
esac
