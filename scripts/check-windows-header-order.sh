#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The `.clang-format` row that keeps `<windows.h>` ahead of every other Windows SDK
# header still does what it was added to do.
#
# ## Why the configuration needs a guard at all
#
# `windows.h` establishes the target architecture the rest of the SDK depends on. Left
# in the generic `.h` bucket it is sorted ALPHABETICALLY among its peers, so any SDK
# header whose name sorts earlier is placed in front of it and the build stops at
# `winnt.h(169): fatal error C1189: #error: "No Target Architecture"`. That cost five
# Windows CI jobs on #146 (#1135), and `IncludeBlocks: Regroup` means writing the
# includes in the right order by hand does not survive a format run.
#
# The remedy was a configuration row, which makes the wrong ordering impossible rather
# than detected. **The configuration is then the guard, and nothing watched the guard.**
# A row can be deleted, renumbered, or overtaken by a later row added above it, and the
# only symptom is Windows CI going red on a change that has nothing to do with it.
#
# ## Why this is NOT the scan that was rejected
#
# A scan over source files was considered and refused, and the reason is worth keeping
# because it is what decides the shape of this one: whether a given SDK header tolerates
# being included first is **per-header and not knowable from its name**. Such a scan
# would refuse the three correct files standing in the tree today, and would still admit
# whatever header somebody adds tomorrow.
#
# This asks a different question. Not *which headers are dangerous*, but *does the row
# still sort `<windows.h>` first*. It reads no source file and has nothing to say about
# any of them.
#
# ## Why it refuses a formatter it was not pinned to
#
# Successive clang-format releases disagree about include ordering, so a verdict from an
# unpinned binary describes a tool nobody runs. A missing formatter is a **skip** -- an
# external prerequisite, the same shape as the other `smoke` checks -- but a formatter of
# the WRONG version is a **refusal**, because a wrong verdict is worse than no verdict and
# this one would be wrong in the reassuring direction.
#
# The pinned version is READ from `.github/workflows/build.yml`, never restated here. Two
# scripts already spell `${CLANG_TOOLS_VERSION:-22}` as a fallback default; a third copy
# is a third thing to be wrong when CI's pin moves.
#
# ## How the probe avoids the tree
#
# clang-format resolves `--style=file` by walking up from the file's directory, so a
# probe written outside the repository would silently get the DEFAULT style and a control
# there could never fire. It also must not write into the tree: `local-gate.sh` guards a
# dirty tree at both ends, and a check that dirties it would be caught by that guard on a
# good branch. So the probe is fed on **stdin** with `--assume-filename` and an explicit
# relative `--style=file:.clang-format` -- no file created, and the config named rather
# than discovered.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs on every platform CI builds.

set -uo pipefail

SkipMissingTool=77
UsageError=2

SourceDir="${1:-}"
SelfTest="no"
if [ "${1:-}" = "--self-test" ]; then
    SelfTest="yes"
    SourceDir="${2:-}"
fi
if [ -z "$SourceDir" ]; then
    SourceDir="$(cd "$(dirname "$0")/.." && pwd)"
fi
if [ ! -f "$SourceDir/.clang-format" ]; then
    echo "FAIL: no .clang-format under '$SourceDir'" >&2
    exit "$UsageError"
fi

Workflow="$SourceDir/.github/workflows/build.yml"

# The pinned major, read from the one place CI states it.
PinnedVersion=""
if [ -f "$Workflow" ]; then
    PinnedVersion="$(sed -n 's/^[[:space:]]*CLANG_TOOLS_VERSION:[[:space:]]*"\{0,1\}\([0-9][0-9]*\)"\{0,1\}[[:space:]]*$/\1/p' "$Workflow" | sed -n '1p')"
fi
if [ -z "$PinnedVersion" ]; then
    # Not a skip: the check cannot know what to enforce, which is a defect in the
    # check or in the workflow rather than a missing tool on this machine.
    echo "FAIL: could not read CLANG_TOOLS_VERSION from $Workflow" >&2
    exit 1
fi

# Prefer the explicitly versioned binary, exactly as `local-gate.sh` does, then a bare
# one -- but a bare one is ACCEPTED only once its version confirms it is the pinned
# major, never merely because it exists. That fallback used to be unconditional, and
# it is what took every Linux build leg red: those runners carry clang-format 18 and
# only the `Check C++ style` job installs the pinned one, so this check picked 18,
# found the wrong major and reported the TREE as bad (#1135).
#
# A machine carrying ONLY some other version has no pinned formatter, which is the
# `SKIP_RETURN_CODE 77` state this registration already declares -- not a finding
# about the source. The distinction is the whole fix: *the tool is missing* and *the
# tree is wrong* are two states, and one of them was being reported as the other.
#
# Which version was seen is NAMED in the skip. A skip that does not say what it found
# reads as "clang-format is not installed" on a machine that has three of them.
ReadMajor() { # $1 -> the major version, or empty
    "$1" --version 2>&1 | sed -n 's/.*clang-format version \([0-9][0-9]*\).*/\1/p' | sed -n '1p'
}

Formatter=""
Seen=""
for candidate in "clang-format-$PinnedVersion" "clang-format"; do
    command -v "$candidate" >/dev/null 2>&1 || continue
    candidateVersion="$(ReadMajor "$candidate")"
    if [ -z "$candidateVersion" ]; then
        echo "FAIL: could not read a version from '$candidate --version'" >&2
        exit 1
    fi
    Seen="$Seen $candidate=$candidateVersion"
    if [ "$candidateVersion" = "$PinnedVersion" ]; then
        Formatter="$candidate"
        break
    fi
done
if [ -z "$Formatter" ]; then
    if [ -z "$Seen" ]; then
        echo "SKIP: no clang-format on PATH (this check needs the pinned clang-format-$PinnedVersion)"
    else
        # NOT a refusal. Judging include order with the wrong major would describe a
        # tool nobody runs, and would most likely PASS -- the direction that gets
        # believed -- so the check declines to judge rather than judging wrongly.
        echo "SKIP: no clang-format at the pinned major $PinnedVersion (found:$Seen)."
        echo "      Install clang-format-$PinnedVersion (pip download clang-format==$PinnedVersion.1.0)"
        echo "      to have this check actually run here."
    fi
    exit "$SkipMissingTool"
fi
FoundVersion="$PinnedVersion"

# Every one of these sorts BEFORE "windows.h" alphabetically, so each would be placed
# in front of it by the generic bucket. `processthreadsapi.h` is the one that actually
# broke #146; the rest are ordinary SDK headers this tree already includes somewhere.
ProbeHeaders="processthreadsapi.h accctrl.h aclapi.h crtdbg.h fileapi.h"

# Format one probe and print the first `<...h>` include in the result.
# $1 = the config file to use, relative to $SourceDir
# $2 = the SDK header to place after <windows.h>
FirstHeaderAfterFormat() {
    local config="$1" header="$2" formatted=""
    formatted="$(printf '#pragma once\n#if defined(_WIN32)\n\n    #include <windows.h>\n    #include <%s>\n\n#endif\n' "$header" \
        | (cd "$SourceDir" && "$Formatter" --style="file:$config" --assume-filename=src/tests/probe.hpp 2>/dev/null))" || return 1
    # `sed -n '1p'` rather than `head -1`, deliberately. `head` exits after its line,
    # the producer dies of SIGPIPE, and under `pipefail` the pipeline reports the
    # PRODUCER's status -- the same family as the `| grep -q` idiom
    # `check-e2e-helpers.sh` scans for, which is a false negative on the SUCCESS path.
    # `sed -n '1p'` consumes the whole stream, so there is nothing to signal.
    printf '%s\n' "$formatted" | sed -n 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*<\([A-Za-z0-9_]*\.h\)>.*/\1/p' | sed -n '1p'
}

# Assert <windows.h> comes first for every probe header.
# $1 = config file, $2 = "want-first" or "want-not-first", $3 = label
AssertOrdering() {
    local config="$1" want="$2" label="$3" header="" first="" bad=0
    for header in $ProbeHeaders; do
        first="$(FirstHeaderAfterFormat "$config" "$header")"
        if [ -z "$first" ]; then
            echo "FAIL: $label: the formatter produced no include at all for <$header>" >&2
            return 1
        fi
        if [ "$want" = "want-first" ] && [ "$first" != "windows.h" ]; then
            echo "FAIL: $label: adding <$header> put <$first> ahead of <windows.h>." >&2
            echo "      That is #1135: the build stops at winnt.h C1189 \"No Target Architecture\"." >&2
            echo "      Check the '^<windows\\.h>' row in .clang-format still sorts ahead of the" >&2
            echo "      generic '<[[:alnum:]_]+\\.h>' bucket." >&2
            bad=1
        fi
        if [ "$want" = "want-not-first" ] && [ "$first" = "windows.h" ]; then
            echo "FAIL: $label: <windows.h> stayed first with the row removed, so this check" >&2
            echo "      cannot tell the two configurations apart and proves nothing." >&2
            bad=1
        fi
    done
    return "$bad"
}

if [ "$SelfTest" = "yes" ]; then
    # Both directions. A guard nobody has watched refuse is not a guard, and one nobody
    # has watched accept is not known to work -- so the real configuration must ACCEPT
    # and a configuration with the row deleted must REFUSE. Without the second case this
    # check passes with the row gone and reports nothing, which is the failure it exists
    # to prevent, one level up.
    cases=0
    Neutered="$(mktemp "$SourceDir/.clang-format-neutered.XXXXXX")" || {
        echo "FAIL: mktemp failed" >&2
        exit "$UsageError"
    }
    trap 'rm -f "$Neutered"' EXIT
    # Rewrite the configuration to what master spelled BEFORE #1135: the `^<windows\.h>`
    # row and its `Priority` line gone, and the generic bucket back to 82 so `windows.h`
    # sorts with its peers again. Merely deleting the row is not the same thing and
    # would leave a gap rather than the old behaviour.
    #
    # **awk, not `sed -e '/re/,+1d'`.** That address form is a GNU extension -- measured:
    # `sed --posix` rejects it with *"unexpected `,`"* -- and BSD sed on the macOS runner
    # would error rather than no-op. This check is in the default `ctest` set, so it runs
    # on `macOS-clang-release`, which is a required context.
    #
    # Worth naming rather than just fixing: a guard for a hazard no Linux build can
    # observe, written with a construct whose failure no Linux build can observe. Same
    # mechanism as the bug it guards, twice in one change. That is the reason for this
    # paragraph -- so the next person does not reach for a GNU-ism here either.
    #
    # It REFUSES when either edit matches nothing. A neutering that silently neutered
    # nothing produces a "row deleted" file byte-identical to the real one, and then both
    # self-test arms pass while proving nothing -- the failure one level up from the one
    # this check exists to prevent.
    if ! awk '
        /^[[:space:]]*-[[:space:]]*Regex:.*<windows/ { removed++; skipNext = 1; next }
        skipNext == 1                                { skipNext = 0; next }
        /^[[:space:]]*Priority:[[:space:]]*83[[:space:]]*$/ {
            sub(/83/, "82"); renumbered++
        }
        { print }
        END {
            if (removed != 1 || renumbered != 1)
            {
                printf("FAIL: neutering matched %d row(s) and %d priority line(s), wanted 1 and 1\n",
                       removed, renumbered) > "/dev/stderr"
                exit 1
            }
        }
    ' "$SourceDir/.clang-format" > "$Neutered"; then
        echo "FAIL: could not build the neutered configuration -- see above." >&2
        exit 1
    fi
    NeuteredName="$(basename "$Neutered")"

    if AssertOrdering ".clang-format" "want-first" "real configuration"; then
        echo "ok: the shipped .clang-format keeps <windows.h> first"
        cases=$((cases + 1))
    else
        exit 1
    fi
    if AssertOrdering "$NeuteredName" "want-not-first" "row deleted"; then
        echo "ok: with the row deleted, an SDK header IS sorted ahead of <windows.h>"
        cases=$((cases + 1))
    else
        exit 1
    fi
    echo "self-test: $cases case(s) ran, both directions"
    exit 0
fi

if AssertOrdering ".clang-format" "want-first" "shipped configuration"; then
    echo "ok: <windows.h> sorts ahead of every probe SDK header ($Formatter $FoundVersion, CI pin $PinnedVersion)"
    exit 0
fi
exit 1
