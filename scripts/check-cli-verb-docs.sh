#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Every verb `fastcache-cli` accepts is documented, and every verb the docs claim it
# accepts exists.
#
# ## Why this exists
#
# `docs/tools/fastcache-cli.md` carries a Commands table, and nothing read it. That is
# the shape `check-resp-command-docs.sh` was written for one phase earlier, in the same
# tree, about the same kind of table: `RedisResp.hpp` had said for months that its table
# was the source of truth "if this prose drifts", and the prose drifted -- because a
# sentence is not a reader.
#
# The CLI's table is the one most likely to drift, and for a reason the RESP one does
# not share: it grows a block at a time. This check landed alongside eleven verbs added
# in one commit, which is precisely the edit where updating a doc list by hand is
# forgotten and precisely the size at which nobody notices it was.
#
# A stale list here is not cosmetic. `--help` is derived from the table, so it stays
# right; the DOC is what an operator reads before installing the thing, and a verb
# missing from it is a capability nobody knows exists. The other direction is worse: a
# documented verb that does not exist is a command somebody puts in a script.
#
# ## What it does NOT cover, stated rather than left to be discovered
#
#   * Operand counts, modifiers and summaries. `--help` derives those from the row and
#     is the reference; the doc's job is to say a verb EXISTS and what family it is in.
#   * The prose outside the Commands table. `### The memcached-only verbs` explains
#     several verbs in bullets, and a scan over prose could only compare spellings
#     inside sentences -- a sentence is not a list.
#   * Which block a verb is filed under. Putting `touch` in the Read row would pass.
#     That is a judgement about a category, not a false statement about what the tool
#     accepts.
#
# ## Constraints
#
# bash 3.2 (macOS ships a 2007 `/bin/bash` and this runs in the default ctest set): no
# `mapfile`, `declare -A`, `${var^^}` or `local -n`. `fail` is unconditional rather than
# guarded on `BASHPID`, which is bash 4.0+.
set -uo pipefail

self="${0##*/}"
selfTest="no"
sourceDir=""

while [ $# -gt 0 ]; do
    case "$1" in
        --self-test) selfTest="yes"; shift ;;
        -h|--help)
            echo "usage: ${self} <source-dir>"
            echo "       ${self} --self-test"
            exit 0
            ;;
        -*) echo "${self}: unknown argument: $1" >&2; exit 2 ;;
        *)
            if [ -n "$sourceDir" ]; then
                echo "${self}: unexpected argument: $1" >&2; exit 2
            fi
            sourceDir="$1"; shift
            ;;
    esac
done

failures=0
note() { echo "$*"; }
refuse() {
    echo "FAIL ${self}: $*" >&2
    failures=$(( failures + 1 ))
}

# Verb names out of `VerbTable`'s array literal.
#
# **Bounded to the literal rather than grepping the file**, and here that is not a
# precaution but a correction: `McFlagTable` and `McStatsSubs` sit in the same
# translation unit, a few dozen lines above, and both spell `.name = "..."`. A
# file-wide scan would report `ttl_seconds`, `slab_class`, `settings` and `conns` as
# undocumented verbs -- a confident wrong list, which is worse than no check.
#
# @param 1 path to CliVerbs.cpp
TableVerbs() {
    awk '
        /constexpr auto VerbTable = std::to_array<VerbSpec>\(\{/ { inside = 1; next }
        inside && /^    \}\);$/                                  { inside = 0 }
        inside && match($0, /\.name = "[a-z][a-z0-9-]*"/) {
            piece = substr($0, RSTART, RLENGTH)
            sub(/^\.name = "/, "", piece)
            sub(/"$/, "", piece)
            print piece
        }
    ' "$1" | LC_ALL=C sort -u
}

# Verb names out of the doc's `## Commands` table.
#
# The SECOND cell of each row, because the first is the family label (`Read`,
# `Write (memcached)`) and the verbs are beside it. Only backtick spans, and only ones
# opening with a lower-case letter -- which is what excludes the modifiers (`--ttl`,
# `--raw`) that share the section.
#
# Rows are the only thing read: the `### The memcached-only verbs` prose below is
# inside this section and is full of backticked verbs being EXPLAINED rather than
# listed, and counting those would make every explained verb look listed.
#
# An escaped `\|` inside a cell is a literal pipe in markdown and is masked before the
# split, as the RESP check does -- without it a cell splits in half and the verbs after
# the pipe are lost from a row that documents them.
#
# @param 1 path to fastcache-cli.md
DocVerbs() {
    awk '
        /^## Commands/ { inside = 1; next }
        inside && /^## / { inside = 0 }
        !inside { next }
        /^\|/ {
            row = $0
            gsub(/\\\|/, "\001", row)          # mask escaped pipes
            n = split(row, cells, "|")
            if (n < 4) next
            cell = cells[3]
            gsub(/\001/, "|", cell)
            while (match(cell, /`[a-z][a-z0-9-]*`/)) {
                verb = substr(cell, RSTART + 1, RLENGTH - 2)
                print verb
                cell = substr(cell, RSTART + RLENGTH)
            }
        }
    ' "$1" | LC_ALL=C sort -u
}

# Compare one tree.
#
# @param 1 source directory
Verify() {
    local dir="$1"
    local impl="${dir}/src/apps/fastcache-cli/CliVerbs.cpp"
    local doc="${dir}/docs/tools/fastcache-cli.md"
    local tableFile docFile verb tableCount docCount

    if [ ! -r "$impl" ]; then
        refuse "cannot read ${impl}"
        return
    fi
    if [ ! -r "$doc" ]; then
        refuse "cannot read ${doc}"
        return
    fi

    tableFile="$(mktemp)"
    docFile="$(mktemp)"
    TableVerbs "$impl" > "$tableFile"
    DocVerbs "$doc" > "$docFile"

    tableCount="$(wc -l < "$tableFile" | tr -d ' ')"
    docCount="$(wc -l < "$docFile" | tr -d ' ')"

    # Fail CLOSED on either extraction coming back empty. **Two empty lists agree
    # perfectly**, and that agreement is exactly what a broken reader looks like -- a
    # renamed table literal or a renamed doc heading would otherwise report a clean
    # tree forever.
    if [ "$tableCount" -lt 1 ]; then
        refuse "read no verbs out of ${impl}: the VerbTable literal did not match"
        rm -f "$tableFile" "$docFile"
        return
    fi
    if [ "$docCount" -lt 1 ]; then
        refuse "read no verbs out of ${doc}: the '## Commands' table did not match"
        rm -f "$tableFile" "$docFile"
        return
    fi

    while IFS= read -r verb; do
        [ -n "$verb" ] || continue
        if ! grep -Fxq "$verb" "$docFile"; then
            refuse "\`${verb}\` is a VerbTable row and is in no Commands table row of ${doc##*/}"
        fi
    done < "$tableFile"

    while IFS= read -r verb; do
        [ -n "$verb" ] || continue
        if ! grep -Fxq "$verb" "$tableFile"; then
            refuse "${doc##*/} lists \`${verb}\` and there is no such VerbTable row"
        fi
    done < "$docFile"

    note "${self}: ${tableCount} verbs in the table, ${docCount} in the docs"
    rm -f "$tableFile" "$docFile"
}

# --- self-test ----------------------------------------------------------------------
#
# Both directions, and both ways the reader can fail to LOOK. A suite whose only case is
# a correct tree passes under every bug this check exists to catch -- and the correct
# case is not decoration either: a guard nobody has watched ACCEPT is not known to work,
# which is #1031, where a check refused every tree for two days behind a confidently
# worded false cause.

Stage() {
    local root="$1" verbs="$2" docVerbs="$3"
    mkdir -p "${root}/src/apps/fastcache-cli" "${root}/docs/tools"
    {
        echo '    // A decoy table above the real one, spelling `.name` -- this is what a'
        echo '    // file-wide grep would wrongly report as undocumented verbs.'
        echo '    constexpr auto McStatsSubs = std::to_array<McStatsSub>({'
        echo '        { .name = "settings", .summary = "..." },'
        echo '    });'
        echo ''
        echo '    constexpr auto VerbTable = std::to_array<VerbSpec>({'
        for verb in $verbs; do
            echo "        { .name = \"${verb}\","
            echo '          .wire = Wire::Resp,'
            echo '          .handler = &Something },'
        done
        echo '    });'
    } > "${root}/src/apps/fastcache-cli/CliVerbs.cpp"
    {
        echo '# `fastcache-cli`'
        echo ''
        echo '## Commands'
        echo ''
        echo '| | |'
        echo '|---|---|'
        printf '| Read |'
        for verb in $docVerbs; do
            printf ' `%s`,' "$verb"
        done
        echo ' |'
        echo ''
        echo 'Prose mentioning `get` and `--ttl`, which must not be read as a listing.'
        echo ''
        echo '## Output'
    } > "${root}/docs/tools/fastcache-cli.md"
}

RunCase() {
    local name="$1" want="$2" root="$3"
    local before="$failures"
    Verify "$root" > /dev/null 2>&1
    local got="pass"
    [ "$failures" -gt "$before" ] && got="fail"
    # The counter is reset per case: a self-test tallying its subject's refusals as its
    # own would report every deliberate refusal as a failure of the run.
    failures="$before"
    if [ "$got" = "$want" ]; then
        echo "  ok   ${name} (${got})"
        return 0
    fi
    echo "  FAIL ${name}: expected ${want}, got ${got}" >&2
    return 1
}

# The self-test's scratch directory, at FILE scope rather than `local` to SelfTest.
# The trap runs at EXIT, by which time a local is out of scope -- under `set -u` that
# is an unbound-variable error AFTER the verdict is printed, so the run reports success
# and leaves the directory behind, which is the quiet half.
selfTestTmp=""
CleanSelfTest() { [ -n "$selfTestTmp" ] && rm -rf "$selfTestTmp"; }

SelfTest() {
    local bad=0 ran=0 tmp
    selfTestTmp="$(mktemp -d)"
    tmp="$selfTestTmp"
    trap CleanSelfTest EXIT

    Stage "${tmp}/agree" "get set touch" "get set touch"
    RunCase "a tree whose table and docs agree" pass "${tmp}/agree" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/undocumented" "get set touch mc-stats" "get set touch"
    RunCase "a verb in the table and not in the docs" fail "${tmp}/undocumented" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/invented" "get set" "get set list"
    RunCase "a verb in the docs and not in the table" fail "${tmp}/invented" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/hyphen" "cache-memlimit get" "cache-memlimit get"
    RunCase "a hyphenated verb is read on both sides" pass "${tmp}/hyphen" || bad=1
    ran=$(( ran + 1 ))

    # The reader failing to LOOK, in both places. Each leaves one list empty, and two
    # empty lists agree -- so without these the check would report a clean tree for a
    # renamed literal or a renamed heading, forever.
    Stage "${tmp}/renamed-table" "get set" "get set"
    sed 's/constexpr auto VerbTable/constexpr auto RenamedTable/' \
        "${tmp}/renamed-table/src/apps/fastcache-cli/CliVerbs.cpp" > "${tmp}/renamed-table/x" \
        && mv "${tmp}/renamed-table/x" "${tmp}/renamed-table/src/apps/fastcache-cli/CliVerbs.cpp"
    RunCase "a renamed table literal refuses rather than reading nothing" fail "${tmp}/renamed-table" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/renamed-heading" "get set" "get set"
    sed 's/^## Commands/## Verbs/' \
        "${tmp}/renamed-heading/docs/tools/fastcache-cli.md" > "${tmp}/renamed-heading/x" \
        && mv "${tmp}/renamed-heading/x" "${tmp}/renamed-heading/docs/tools/fastcache-cli.md"
    RunCase "a renamed doc heading refuses rather than reading nothing" fail "${tmp}/renamed-heading" || bad=1
    ran=$(( ran + 1 ))

    Stage "${tmp}/missing-doc" "get set" "get set"
    rm -f "${tmp}/missing-doc/docs/tools/fastcache-cli.md"
    RunCase "a missing doc is a refusal, not a pass" fail "${tmp}/missing-doc" || bad=1
    ran=$(( ran + 1 ))

    # The decoy is what makes the bounded read worth having: staged trees all carry a
    # `.name`-spelling table above the real one, and this case says so out loud so the
    # property is not silently resting on the fixture.
    if grep -q 'McStatsSubs' "${tmp}/agree/src/apps/fastcache-cli/CliVerbs.cpp"; then
        echo "  ok   the fixtures carry a decoy .name table above VerbTable"
        ran=$(( ran + 1 ))
    else
        echo "  FAIL the decoy table is missing, so the bounded read is untested" >&2
        bad=1
    fi

    # A self-test that stopped early must not look like one that judged something.
    echo "${self}: ${ran} self-test cases ran"
    return "$bad"
}

if [ "$selfTest" = "yes" ]; then
    if SelfTest; then
        echo "${self}: self-test passed"
        exit 0
    fi
    echo "${self}: self-test FAILED" >&2
    exit 1
fi

if [ -z "$sourceDir" ]; then
    echo "${self}: a source directory is required" >&2
    exit 2
fi

Verify "$sourceDir"

if [ "$failures" -gt 0 ]; then
    echo "${self}: ${failures} problem(s)" >&2
    exit 1
fi
echo "${self}: OK"
exit 0
