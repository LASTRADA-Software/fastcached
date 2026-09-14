#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# `.clang-tidy-version` names the ONE clang-tidy build this tree is judged with, and a binary claiming to be it is
# checked against the wheel it came from rather than taken at its word (#1404).
#
# ## Why a build, and why this identity
#
# `CLANG_TOOLS_VERSION` pins a MAJOR. apt.llvm.org ships rolling snapshots under one version number, keeps only the
# newest, and `clang-tidy-<major> --version` prints `Ubuntu LLVM version <x.y.z>` for every one of them -- so the banner
# check `.clang-format-version` rests on has nothing to compare for the analyser, and two snapshots a month apart
# disagreed on 19 findings in one file (#1198). The declaration therefore names a PyPI release, which is immutable and
# installs identically on every platform CI analyses on.
#
# A binary IS that build when all of these hold, and none of them is a banner:
#   - it sits at `<site>/clang_tidy/data/bin/clang-tidy[.exe]`, the layout the wheel installs;
#   - exactly one `<site>/clang_tidy-*.dist-info` exists beside it, whose METADATA says `Name: clang-tidy` and the
#     declared `Version:`;
#   - that dist-info's RECORD lists the binary with a sha256 equal to the file on disk.
# The last clause is what makes it an identity rather than a label: a binary swapped into the directory, or a wheel
# whose files were touched after install, fails it. Anything outside that layout -- apt's `clang-tidy-22`, choco's
# LLVM, the console-script shim pip puts on PATH -- is UNIDENTIFIABLE, which is its own refusal and never a pass.
#
# ## Modes
#
#   (default)             The declaration parses, names package `clang-tidy`, and its major is the one build.yml pins.
#                         Reads two files, needs no analyser, runs in the default ctest set.
#   --installed <exe>     <exe> is the declared build, by the rule above. Run by both clang-tidy jobs after they
#                         install, and by local-gate.sh before it configures a tidy build. Not a ctest: no build leg
#                         has the analyser, so it would skip everywhere (#1135).
#   --requirement         Print `<package>==<version>` for pip, so no installer restates the version.
#   --locate <site>       Print the binary path inside a `pip install --target <site>`, for this platform.
#   --resolve             Print the analyser a LOCAL run uses, after checking it is the declared build: the binary
#                         `FASTCACHED_CLANG_TIDY` names, or else the one installed in the default site,
#                         `${XDG_DATA_HOME:-$HOME/.local/share}/fastcached/clang-tidy/<version>`. The path goes to stdout
#                         and every diagnostic to stderr, so `local-gate.sh` and `tidy-sweep.sh` ask ONE question
#                         and a refusal names the exact install command for this machine. There is no fallback to
#                         a clang-tidy on PATH: that is the analyser this file cannot identify.
#   --self-test           Drive every verdict against synthetic wheel layouts.
#
# bash 3.2 for the default and self-test modes: they run in ctest on macOS.
#
# Usage:
#   bash scripts/check-clang-tidy-version.sh [<source-dir>]
#   bash scripts/check-clang-tidy-version.sh --installed <exe> [<source-dir>]
#   bash scripts/check-clang-tidy-version.sh --requirement [<source-dir>]
#   bash scripts/check-clang-tidy-version.sh --locate <site> [<source-dir>]
#   bash scripts/check-clang-tidy-version.sh --resolve [<source-dir>]
#   bash scripts/check-clang-tidy-version.sh --self-test [<source-dir>]

set -uo pipefail

UsageError=2

Mode="static"
Argument=""
case "${1:-}" in
    --installed|--locate)
        Mode="${1#--}"
        Argument="${2:-}"
        [ -n "$Argument" ] || { echo "FAIL: $1 needs an argument" >&2; exit "$UsageError"; }
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

Trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

# Parse the declaration into DeclaredPackage / DeclaredVersion. $1 = the file. Prints the refusal, returns 1.
ReadDeclaration() {
    local file="$1" line key value lineno=0
    DeclaredPackage=""
    DeclaredVersion=""
    if [ ! -f "$file" ]; then
        echo "FAIL: $file does not exist, so no clang-tidy build is declared and every consumer would judge with whatever clang-tidy it finds (#1404)"
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
        [ -n "$value" ] || { echo "FAIL: $file line $lineno gives '$key' no value"; return 1; }
        case "$key" in
            package)
                [ -z "$DeclaredPackage" ] || { echo "FAIL: $file line $lineno repeats 'package'; one build is declared, so each key appears once"; return 1; }
                DeclaredPackage="$value"
                ;;
            version)
                [ -z "$DeclaredVersion" ] || { echo "FAIL: $file line $lineno repeats 'version'; one build is declared, so each key appears once"; return 1; }
                DeclaredVersion="$value"
                ;;
            *)
                echo "FAIL: $file line $lineno has unknown key '$key' (known: package, version)"
                return 1
                ;;
        esac
    done < "$file"
    if [ "$DeclaredPackage" != "clang-tidy" ]; then
        echo "FAIL: $file declares package '${DeclaredPackage:-<none>}'; the analyser is PyPI's 'clang-tidy', and an identity is only checkable against that wheel's layout"
        return 1
    fi
    case "$DeclaredVersion" in
        [0-9]*.[0-9]*.[0-9]*) ;;
        *)
            echo "FAIL: $file declares version '${DeclaredVersion:-<none>}', which is not an exact release like 22.1.8; a major or a range names a family of analysers, which is the defect this declaration exists to remove"
            return 1
            ;;
    esac
    case "$DeclaredVersion" in
        *[!0-9.]*)
            echo "FAIL: $file declares version '$DeclaredVersion'; only digits and dots name one immutable PyPI release"
            return 1
            ;;
    esac
    return 0
}

# The pinned major from the one place CI states it. $1 = build.yml
ReadPinnedMajor() {
    [ -f "$1" ] || return 0
    sed -n 's/^[[:space:]]*CLANG_TOOLS_VERSION:[[:space:]]*"\{0,1\}\([0-9][0-9]*\)"\{0,1\}[[:space:]]*$/\1/p' "$1" | sed -n '1p'
}

CheckStatic() {
    local dir="$1" pinned
    pinned="$(ReadPinnedMajor "$dir/.github/workflows/build.yml")"
    if [ -z "$pinned" ]; then
        echo "FAIL: could not read CLANG_TOOLS_VERSION from $dir/.github/workflows/build.yml, so there is no pin to hold the declaration to"
        return 1
    fi
    ReadDeclaration "$dir/.clang-tidy-version" || return 1
    if [ "${DeclaredVersion%%.*}" != "$pinned" ]; then
        echo "FAIL: .clang-tidy-version declares clang-tidy $DeclaredVersion, but build.yml pins CLANG_TOOLS_VERSION $pinned."
        echo "      The compilers follow the pin and the analyser follows this file, so they move in the same commit:"
        echo "      set 'version:' to an exact $pinned.x.y release of PyPI's clang-tidy (pip index versions clang-tidy)."
        return 1
    fi
    echo "ok: .clang-tidy-version declares clang-tidy $DeclaredVersion, matching CLANG_TOOLS_VERSION $pinned"
    return 0
}

# Print the hex sha256 of $1, or nothing when no hashing tool runs here.
Sha256Hex() {
    local out=""
    if command -v sha256sum >/dev/null 2>&1; then
        out="$(sha256sum "$1" 2>/dev/null)"
    elif command -v shasum >/dev/null 2>&1; then
        out="$(shasum -a 256 "$1" 2>/dev/null)"
    fi
    printf '%s' "${out%% *}"
}

# The lowercase hex of the digest a wheel's RECORD spells as unpadded base64url in $1 (PEP 376 / 627), or nothing when
# it does not decode. Decoded through a PIPE, never built in a shell string: a digest holds zero bytes, which no shell
# variable carries and bash 3.2's `printf %b` is not trusted to emit. `base64 -d` is GNU and Git Bash, `-D` macOS.
Base64UrlToHex() {
    local s="$1" bytes
    s="$(printf '%s' "$s" | tr -- '-_' '+/')"
    while [ $(( ${#s} % 4 )) -ne 0 ]; do s="$s="; done
    bytes="$( { printf '%s' "$s" | base64 -d 2>/dev/null || printf '%s' "$s" | base64 -D 2>/dev/null; } | od -An -v -tx1 | tr -d ' \n')"
    printf '%s' "$bytes"
}

# $1 = the binary, $2 = source dir.
CheckInstalled() {
    local exe="$1" dir="$2" bin data package site name suffix distinfos count distinfo metaName metaVersion record
    local recorded recordedHex actualHex
    ReadDeclaration "$dir/.clang-tidy-version" || return 1
    # A bare NAME is looked up the way the caller would run it, so `TIDY=clang-tidy` is judged as the binary it names
    # rather than refused as a missing file.
    case "$exe" in
        */*|*"\\"*) ;;
        *) exe="$(command -v "$exe" 2>/dev/null || printf '%s' "$exe")" ;;
    esac
    if [ ! -f "$exe" ]; then
        echo "FAIL: '$exe' is not a file, so which clang-tidy build it is cannot be asked."
        InstallHint
        return 1
    fi
    bin="$(cd "$(dirname "$exe")" && pwd -P)"
    name="$(basename "$exe")"
    # The Windows wheel ships only `clang-tidy.exe`, and Git Bash answers `[ -f clang-tidy ]` TRUE for it -- so a bare
    # name there is that `.exe`, and is judged as the file RECORD lists. Measured on the win_amd64 22.1.8 wheel.
    if [ "$name" = clang-tidy ] && [ -f "$bin/clang-tidy.exe" ]; then
        exe="$bin/clang-tidy.exe"
        name="clang-tidy.exe"
    fi
    case "$name" in
        clang-tidy) suffix="" ;;
        clang-tidy.exe) suffix=".exe" ;;
        *)
            Unidentifiable "$exe" "its name is '$name', and the wheel installs 'clang-tidy' or 'clang-tidy.exe'"
            return 1
            ;;
    esac
    data="$(dirname "$bin")"
    package="$(dirname "$data")"
    site="$(dirname "$package")"
    if [ "$(basename "$bin")" != bin ] || [ "$(basename "$data")" != data ] || [ "$(basename "$package")" != clang_tidy ]; then
        Unidentifiable "$exe" "it is not at <site>/clang_tidy/data/bin/, where the wheel installs its binary"
        return 1
    fi
    distinfos=""
    count=0
    for distinfo in "$site"/clang_tidy-*.dist-info; do
        [ -d "$distinfo" ] || continue
        distinfos="$distinfo"
        count=$((count + 1))
    done
    if [ "$count" -ne 1 ]; then
        Unidentifiable "$exe" "$site holds $count clang_tidy-*.dist-info director(y/ies), and exactly one says which wheel the binary belongs to"
        return 1
    fi
    distinfo="$distinfos"
    metaName="$(sed -n 's/^Name:[[:space:]]*//p' "$distinfo/METADATA" 2>/dev/null | sed -n '1p' | tr -d '\r')"
    metaVersion="$(sed -n 's/^Version:[[:space:]]*//p' "$distinfo/METADATA" 2>/dev/null | sed -n '1p' | tr -d '\r')"
    if [ "$metaName" != "clang-tidy" ] || [ -z "$metaVersion" ]; then
        Unidentifiable "$exe" "$distinfo/METADATA names '${metaName:-<nothing>}' version '${metaVersion:-<nothing>}'"
        return 1
    fi
    record="clang_tidy/data/bin/clang-tidy$suffix"
    recorded="$(tr -d '\r' < "$distinfo/RECORD" 2>/dev/null | sed -n "s#^${record},sha256=\\([A-Za-z0-9_-]*\\),.*#\\1#p" | sed -n '1p')"
    if [ -z "$recorded" ]; then
        Unidentifiable "$exe" "$distinfo/RECORD lists no sha256 for $record"
        return 1
    fi
    actualHex="$(Sha256Hex "$exe")"
    if [ -z "$actualHex" ]; then
        echo "FAIL: no sha256 tool (sha256sum or shasum) runs here, so '$exe' cannot be identified -- which is not the same as being wrong, and is reported apart from it"
        return 1
    fi
    recordedHex="$(Base64UrlToHex "$recorded")"
    if [ "${#recordedHex}" -ne 64 ]; then
        Unidentifiable "$exe" "$distinfo/RECORD's sha256 for $record ('$recorded') does not decode to a 32-byte digest"
        return 1
    fi
    if [ "$actualHex" != "$recordedHex" ]; then
        Unidentifiable "$exe" "its sha256 ($actualHex) is not the one its wheel recorded ($recordedHex): the file was replaced or modified after install"
        return 1
    fi
    if [ "$metaVersion" != "$DeclaredVersion" ]; then
        echo "FAIL: $exe is clang-tidy $metaVersion (its wheel's METADATA, binary verified against RECORD), and .clang-tidy-version declares $DeclaredVersion."
        echo "      Two releases of one major can report different findings on the same file (#1198), so this analyser's verdict"
        echo "      is not the one CI reaches."
        InstallHint
        return 1
    fi
    echo "ok: $exe is clang-tidy $metaVersion, the declared build (METADATA $distinfo, binary sha256 matches RECORD)"
    return 0
}

# $1 = the binary, $2 = why it cannot be identified.
Unidentifiable() {
    echo "FAIL: '$1' cannot be identified as a clang-tidy build: $2."
    echo "      Only a binary inside an installed PyPI clang-tidy wheel can be: a distribution's clang-tidy-<major> prints one"
    echo "      --version banner for every snapshot, so no check can tell whether it is the build CI judges with (#1404)."
    InstallHint
}

InstallHint() {
    local site
    site="$(DefaultSite)"
    echo "      Install the declared build ($DeclaredPackage==$DeclaredVersion) where local-gate.sh and tidy-sweep.sh look for it:"
    echo "        python3 -m pip install --target '$site' '$DeclaredPackage==$DeclaredVersion'"
    echo "      or anywhere else, and name the binary inside it (--locate <dir> prints it) in FASTCACHED_CLANG_TIDY."
    echo "      A host whose python3 has no pip can unzip the wheel PyPI serves for it into that directory: a wheel IS"
    echo "      that layout, and the identity check reads the same METADATA and RECORD. Restore the execute bit afterwards."
}

# Where a local run looks when FASTCACHED_CLANG_TIDY names nothing: one directory per declared version, so a declaration
# that moves finds nothing rather than a stale install.
DefaultSite() {
    printf '%s/fastcached/clang-tidy/%s' "${XDG_DATA_HOME:-${HOME:-~}/.local/share}" "$DeclaredVersion"
}

# $1 = source dir. The analyser a local run uses, checked; path on stdout, everything else on stderr.
Resolve() {
    local dir="$1" exe site
    ReadDeclaration "$dir/.clang-tidy-version" >&2 || return 1
    if [ -n "${FASTCACHED_CLANG_TIDY:-}" ]; then
        exe="$FASTCACHED_CLANG_TIDY"
    else
        site="$(DefaultSite)"
        exe=""
        # `.exe` first: Git Bash answers `-f clang-tidy` true for `clang-tidy.exe`, and the name must be the recorded one.
        for candidate in "$site/clang_tidy/data/bin/clang-tidy.exe" "$site/clang_tidy/data/bin/clang-tidy"; do
            if [ -f "$candidate" ]; then
                exe="$candidate"
                break
            fi
        done
        if [ -z "$exe" ]; then
            echo "FAIL: no clang-tidy is installed at $site, and FASTCACHED_CLANG_TIDY names none, so there is no analyser" >&2
            echo "      this run can identify as the declared build. There is deliberately no fallback to a clang-tidy on PATH." >&2
            InstallHint >&2
            return 1
        fi
    fi
    CheckInstalled "$exe" "$dir" >&2 || return 1
    printf '%s\n' "$exe"
}

# --- self-test -----------------------------------------------------------------
if [ "$Mode" = "self-test" ]; then
    Work="$(mktemp -d "${TMPDIR:-/tmp}/clang-tidy-version-selftest.XXXXXX")" || {
        echo "FAIL: mktemp failed" >&2
        exit "$UsageError"
    }
    # A fresh directory per run, removed file by file rather than by a recursive delete of a computed path.
    Cleanup() { find "$Work" -depth -mindepth 1 -delete 2>/dev/null; rmdir "$Work" 2>/dev/null; }
    trap Cleanup EXIT
    Cases=0
    Failed=0
    Self="$0"
    # No real install may decide a case: every child sees a scratch data home and no override unless a case sets one.
    export XDG_DATA_HOME="$Work/data"
    unset FASTCACHED_CLANG_TIDY
    # A fixed vector rather than a digest this script computes, or the self-test agrees with itself. Chosen so the
    # digest holds a zero byte (91453e00...) and its base64url a '-': the two places a hex-to-base64url conversion
    # breaks silently. sha256 of 'fake clang-tidy 22.1.8 #1' plus a newline, encoded with Python's hashlib and base64.
    FakeContent='fake clang-tidy 22.1.8 #1'
    FakeDigest='kUU-ABodN1tnYxjMzMVHtUq9vKPzjnm4CZd65F-NegQ'

    # $1 = dir, $2 = pin, rest = declaration lines (none: no file)
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
            printf '%s\n' "$@" > "$dir/.clang-tidy-version"
        fi
    }
    # $1 = site, $2 = METADATA version, $3 = RECORD digest, $4 = binary name (default clang-tidy)
    Wheel() {
        local site="$1" version="$2" digest="$3" name="${4:-clang-tidy}"
        mkdir -p "$site/clang_tidy/data/bin" "$site/clang_tidy-$version.dist-info"
        printf '%s\n' "$FakeContent" > "$site/clang_tidy/data/bin/$name"
        chmod +x "$site/clang_tidy/data/bin/$name"
        printf 'Metadata-Version: 2.2\nName: clang-tidy\nVersion: %s\n' "$version" > "$site/clang_tidy-$version.dist-info/METADATA"
        printf 'clang_tidy/__init__.py,sha256=AAAA,10\nclang_tidy/data/bin/%s,sha256=%s,23\n' "$name" "$digest" \
            > "$site/clang_tidy-$version.dist-info/RECORD"
    }
    # $1 = label, $2 = want (pass|refuse), $3 = required output fragment, rest = arguments
    # rest = arguments to this script, then any NAME=value words for its environment (they come last, so an
    # argument can never be mistaken for one).
    Expect() {
        local label="$1" want="$2" fragment="$3" output status got word
        shift 3
        local arguments=() environment=()
        for word in "$@"; do
            case "$word" in
                FASTCACHED_CLANG_TIDY=*|XDG_DATA_HOME=*) environment[${#environment[@]}]="$word" ;;
                *) arguments[${#arguments[@]}]="$word" ;;
            esac
        done
        Cases=$((Cases + 1))
        output="$(env ${environment[@]+"${environment[@]}"} "${BASH:-bash}" "$Self" "${arguments[@]}" 2>&1)"
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

    Tree "$Work/good" 22 "# the analyser" "package: clang-tidy" "version: 22.1.8"
    Expect "a declaration naming the pinned major" pass "matching CLANG_TOOLS_VERSION 22" "$Work/good"
    Expect "--requirement prints what pip installs" pass "clang-tidy==22.1.8" --requirement "$Work/good"

    Tree "$Work/bumped" 23 "package: clang-tidy" "version: 22.1.8"
    Expect "the pin moved and the declaration did not" refuse "but build.yml pins CLANG_TOOLS_VERSION 23" "$Work/bumped"
    Tree "$Work/absent" 22
    Expect "no declaration" refuse "does not exist" "$Work/absent"
    Tree "$Work/major" 22 "package: clang-tidy" "version: 22"
    Expect "a major instead of a release" refuse "not an exact release" "$Work/major"
    Tree "$Work/range" 22 "package: clang-tidy" "version: 22.1.8rc1"
    Expect "a pre-release suffix" refuse "only digits and dots" "$Work/range"
    Tree "$Work/apt" 22 "package: clang-tidy-22" "version: 22.1.8"
    Expect "a distribution package name" refuse "declares package 'clang-tidy-22'" "$Work/apt"
    Tree "$Work/twice" 22 "package: clang-tidy" "version: 22.1.8" "version: 22.1.7"
    Expect "two versions" refuse "repeats 'version'" "$Work/twice"
    Tree "$Work/twopackages" 22 "package: clang-tidy" "package: clang-format" "version: 22.1.8"
    Expect "two packages before any version" refuse "repeats 'package'" "$Work/twopackages"
    Tree "$Work/typo" 22 "package: clang-tidy" "verison: 22.1.8"
    Expect "an unknown key" refuse "unknown key 'verison'" "$Work/typo"
    Tree "$Work/nopin" "" "package: clang-tidy" "version: 22.1.8"
    Expect "a workflow with no CLANG_TOOLS_VERSION" refuse "could not read CLANG_TOOLS_VERSION" "$Work/nopin"

    Wheel "$Work/site-good" 22.1.8 "$FakeDigest"
    Expect "--installed: the declared wheel, binary matching RECORD" pass "is clang-tidy 22.1.8, the declared build" \
        --installed "$Work/site-good/clang_tidy/data/bin/clang-tidy" "$Work/good"
    Expect "--locate: the binary inside a --target site" pass "$Work/site-good/clang_tidy/data/bin/clang-tidy" \
        --locate "$Work/site-good" "$Work/good"

    Wheel "$Work/site-exe" 22.1.8 "$FakeDigest" clang-tidy.exe
    Expect "--installed: the Windows binary name" pass "the declared build" \
        --installed "$Work/site-exe/clang_tidy/data/bin/clang-tidy.exe" "$Work/good"
    # Git Bash answers `-f clang-tidy` true for `clang-tidy.exe`, so --locate once printed the bare name there and
    # --installed then looked it up in RECORD, which lists only the `.exe` -- the CI Windows leg would have refused its
    # own install. Both spellings present stands in for that here, on any platform -- the bare decoy written FIRST,
    # because on Git Bash a write to `clang-tidy` beside an existing `clang-tidy.exe` lands in the `.exe` (measured: it
    # overwrote the fake binary and the digest case failed instead).
    mkdir -p "$Work/site-both/clang_tidy/data/bin"
    printf 'not the recorded binary\n' > "$Work/site-both/clang_tidy/data/bin/clang-tidy"
    Wheel "$Work/site-both" 22.1.8 "$FakeDigest" clang-tidy.exe
    Expect "--locate: the .exe when both spellings answer" pass "$Work/site-both/clang_tidy/data/bin/clang-tidy.exe" \
        --locate "$Work/site-both" "$Work/good"
    Expect "--installed: a bare name beside the .exe is judged as the .exe" pass "the declared build" \
        --installed "$Work/site-both/clang_tidy/data/bin/clang-tidy" "$Work/good"

    Wheel "$Work/site-old" 22.1.7 "$FakeDigest"
    Expect "--installed: another release of the same major" refuse "is clang-tidy 22.1.7 (its wheel's METADATA" \
        --installed "$Work/site-old/clang_tidy/data/bin/clang-tidy" "$Work/good"

    # The clause that makes it an identity: the recorded digest names DIFFERENT bytes.
    Wheel "$Work/site-swapped" 22.1.8 "13xKycCLcaHiuaKVD5HSs5h1QjeiOz9grLRgEDNGFIM"
    Expect "--installed: a binary whose bytes are not the ones its wheel recorded" refuse "is not the one its wheel recorded" \
        --installed "$Work/site-swapped/clang_tidy/data/bin/clang-tidy" "$Work/good"

    Wheel "$Work/site-norecord" 22.1.8 "$FakeDigest"
    printf 'clang_tidy/__init__.py,sha256=AAAA,10\n' > "$Work/site-norecord/clang_tidy-22.1.8.dist-info/RECORD"
    Expect "--installed: a RECORD that does not list the binary" refuse "lists no sha256" \
        --installed "$Work/site-norecord/clang_tidy/data/bin/clang-tidy" "$Work/good"

    Wheel "$Work/site-two" 22.1.8 "$FakeDigest"
    mkdir -p "$Work/site-two/clang_tidy-22.1.7.dist-info"
    Expect "--installed: two dist-infos beside one binary" refuse "holds 2 clang_tidy-*.dist-info" \
        --installed "$Work/site-two/clang_tidy/data/bin/clang-tidy" "$Work/good"

    # The wheel's own bytes, copied OUT of its tree beside a valid dist-info: the digest matches and the version matches,
    # and it is still not that build in use -- clang-tidy finds its resource headers relative to the binary, so a copy (measured
    # 2026-09-14, the 22.1.8 wheel's binary copied out) reports `'stddef.h' file not found`. Only the layout clause refuses it.
    Wheel "$Work/site-copied" 22.1.8 "$FakeDigest"
    mkdir -p "$Work/site-copied/elsewhere/data/bin"
    cp "$Work/site-copied/clang_tidy/data/bin/clang-tidy" "$Work/site-copied/elsewhere/data/bin/clang-tidy"
    Expect "--installed: the wheel's binary copied out of its tree" refuse "is not at <site>/clang_tidy/data/bin/" \
        --installed "$Work/site-copied/elsewhere/data/bin/clang-tidy" "$Work/good"

    # A distribution's analyser: a real file with the right name, outside any wheel.
    mkdir -p "$Work/usr/bin"
    printf '%s\n' "$FakeContent" > "$Work/usr/bin/clang-tidy"
    Expect "--installed: a binary outside any wheel (apt, choco)" refuse "cannot be identified" \
        --installed "$Work/usr/bin/clang-tidy" "$Work/good"
    printf '%s\n' "$FakeContent" > "$Work/usr/bin/clang-tidy-22"
    Expect "--installed: a versioned distribution name" refuse "its name is 'clang-tidy-22'" \
        --installed "$Work/usr/bin/clang-tidy-22" "$Work/good"
    Expect "--installed: nothing at that path" refuse "is not a file" \
        --installed "$Work/usr/bin/missing" "$Work/good"
    Expect "--installed: the refusal says how to install the declared build" refuse \
        "pip install --target '$Work/data/fastcached/clang-tidy/22.1.8' 'clang-tidy==22.1.8'" \
        --installed "$Work/usr/bin/clang-tidy" "$Work/good"

    # --resolve: what a local run uses. Cases run with a scratch XDG_DATA_HOME, so no real install decides them.
    mkdir -p "$Work/data/fastcached/clang-tidy"
    Wheel "$Work/data/fastcached/clang-tidy/22.1.8" 22.1.8 "$FakeDigest"
    Expect "--resolve: the default site holds the declared build" pass "$Work/data/fastcached/clang-tidy/22.1.8/clang_tidy/data/bin/clang-tidy" \
        --resolve "$Work/good"
    Expect "--resolve: FASTCACHED_CLANG_TIDY names another identified wheel" pass "$Work/site-good/clang_tidy/data/bin/clang-tidy" \
        --resolve "$Work/good" FASTCACHED_CLANG_TIDY="$Work/site-good/clang_tidy/data/bin/clang-tidy"
    Expect "--resolve: FASTCACHED_CLANG_TIDY names a distribution's analyser" refuse "cannot be identified" \
        --resolve "$Work/good" FASTCACHED_CLANG_TIDY="$Work/usr/bin/clang-tidy"
    Expect "--resolve: nothing installed, and the refusal names the install command for that site" refuse \
        "pip install --target '$Work/empty/fastcached/clang-tidy/22.1.8' 'clang-tidy==22.1.8'" --resolve "$Work/good" XDG_DATA_HOME="$Work/empty"
    Expect "--resolve: an installed release that is not the declared one" refuse "is clang-tidy 22.1.7" \
        --resolve "$Work/good" FASTCACHED_CLANG_TIDY="$Work/site-old/clang_tidy/data/bin/clang-tidy"

    Expect "the shipped declaration" pass "matching CLANG_TOOLS_VERSION" "$SourceDir"

    echo "self-test: $Cases case(s) ran, $Failed failed"
    [ "$Cases" -gt 0 ] || { echo "FAIL: no self-test case ran"; exit 1; }
    [ "$Failed" -eq 0 ]
    exit $?
fi

case "$Mode" in
    static) CheckStatic "$SourceDir" ;;
    installed) CheckInstalled "$Argument" "$SourceDir" ;;
    resolve) Resolve "$SourceDir" ;;
    requirement)
        ReadDeclaration "$SourceDir/.clang-tidy-version" || exit 1
        echo "$DeclaredPackage==$DeclaredVersion"
        ;;
    locate)
        ReadDeclaration "$SourceDir/.clang-tidy-version" || exit 1
        # `.exe` first, for the reason `Resolve` gives.
        for candidate in "$Argument/clang_tidy/data/bin/clang-tidy.exe" "$Argument/clang_tidy/data/bin/clang-tidy"; do
            if [ -f "$candidate" ]; then
                echo "$candidate"
                exit 0
            fi
        done
        echo "FAIL: $Argument holds no clang_tidy/data/bin/clang-tidy[.exe]; it is not a 'pip install --target' of $DeclaredPackage"
        InstallHint
        exit 1
        ;;
esac
