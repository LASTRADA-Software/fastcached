#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Every RESP verb fastcached serves is documented, and every verb the docs claim is
# served exists.
#
# ## Why this exists
#
# `RedisResp.hpp` has said for a long time that `CommandTable` is the source of truth
# "if this prose drifts" -- which is a sentence, not a reader, so the prose drifted.
# Measured at the commit that added this check, against a live daemon:
#
#   * `docs/commands/redis/unsupported.md` said pub/sub, `MULTI`/`EXEC`/`WATCH`,
#     sets and streams were unimplemented. Pub/sub (5 verbs), transactions (5) and
#     streams (15) are all served, and so are 7 of the set verbs -- including
#     `SADD` and `SMEMBERS`, which that page named as examples of the absence.
#   * `docs/protocols/redis-resp.md` said `HELLO 3` answers `-NOPROTO` and `COMMAND`
#     always answers `*0`. A probe returns a RESP3 map with `proto:3`, and
#     `COMMAND COUNT` returns `:66`.
#   * Six pages said RESP3 was not supported. One -- the architecture diagram --
#     said it was. The docs disagreed with each other for months.
#
# None of that is a coding mistake and none of it could fail: a claim with no reader
# cannot. So the supported list is DERIVED from the table and checked against it.
#
# ## What it does NOT cover, stated rather than left to be discovered
#
#   * It says nothing about the per-command pages under `docs/commands/redis/`.
#     Only 17 of the verbs have one, and demanding a page per verb would refuse the
#     tree as it stands rather than describing a rule anybody agreed to. A missing
#     page is a gap in coverage; a missing TABLE ROW is a false statement about
#     what the server answers, which is the thing worth failing a build over.
#   * It reads the SUPPORTED table only. `unsupported.md` is prose about families
#     and is deliberately not machine-checked -- a scan over it could only compare
#     verb spellings inside sentences, and a sentence is not a list.
#   * It does not check semantics. A row saying `GET` returns an integer would pass.
#
# ## Constraints
#
# bash 3.2 (macOS ships a 2007 `/bin/bash` and this runs in the default ctest set):
# no `mapfile`, `declare -A`, `${var^^}` or `local -n`. `fail` is unconditional
# rather than guarded on `BASHPID`, which is bash 4.0+.
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

# The verbs `Dispatch` answers BEFORE consulting the table, with the reason each is
# there. They carry side-effects beyond a plain reply -- a credential decision, a
# close, a session reset -- so they are not table rows and a check comparing only
# against the table would report them as documented-but-absent.
#
# A named list rather than a derivation: recognising a fast-track in C++ needs to
# read control flow, and a regex that tried would be wrong in the quiet direction.
# The cost of the list is that a FOURTH fast-track verb would have to be added here
# by hand -- which is a visible edit in review, where a missed derivation is not.
FastTrackVerbs="AUTH QUIT RESET"

failures=0
note() { echo "$*"; }
refuse() {
    echo "FAIL ${self}: $*" >&2
    failures=$(( failures + 1 ))
}

# Verb names out of `CommandTable`'s array literal.
#
# Bounded to the literal rather than grepping the file: `.name = "..."` appears in
# other tables in the same translation unit, and a match from one of those would be
# reported as an undocumented RESP verb.
#
# @param 1 path to RedisResp.cpp
TableVerbs() {
    awk '
        /constexpr auto CommandTable = std::array \{/ { inside = 1; next }
        inside && /^    \};$/                        { inside = 0 }
        inside && match($0, /\.name = "[A-Z][A-Z0-9._]*"/) {
            piece = substr($0, RSTART, RLENGTH)
            sub(/^\.name = "/, "", piece)
            sub(/"$/, "", piece)
            print piece
        }
    ' "$1" | LC_ALL=C sort -u
}

# Verb names out of the doc's `## Supported commands` section.
#
# Only the FIRST cell of a table row, and within it only a token that OPENS a
# backtick span. Both restrictions are load-bearing:
#
#   * the Notes column is full of backticked replies (`+OK`, `:1`) and of verbs
#     being mentioned rather than defined;
#   * a span like `SET <key> <value> [EX|PX <ttl>]` carries the option keywords
#     `EX`, `PX`, `NX`, `XX` in capitals, and reporting those as undocumented verbs
#     would make the check noise. A verb is what a span STARTS with.
#
# An escaped `\|` inside a cell is a literal pipe in markdown, so it is masked
# before the row is split -- without that, `[EX\|PX ...]` splits the first cell in
# half and the verb is lost from a row that documents it.
#
# @param 1 path to redis-resp.md
DocVerbs() {
    awk '
        /^## Supported commands/ { inside = 1; next }
        inside && /^## /         { inside = 0 }
        !inside                  { next }
        /^\|/ {
            row = $0
            gsub(/\\\|/, "\001", row)          # mask escaped pipes
            n = split(row, cells, "|")
            if (n < 2) next
            cell = cells[2]
            gsub(/\001/, "|", cell)
            # Every token that OPENS a backtick span.
            while (match(cell, /`[A-Z][A-Z0-9]*/)) {
                verb = substr(cell, RSTART + 1, RLENGTH - 1)
                print verb
                cell = substr(cell, RSTART + RLENGTH)
                # Skip to the end of this span, so a capitalised word further
                # inside it is not read as a second verb.
                if (match(cell, /`/))
                    cell = substr(cell, RSTART + 1)
                else
                    cell = ""
            }
        }
    ' "$1" | LC_ALL=C sort -u
}

# The row count the doc states in prose, or empty when it states none.
#
# A total written beside a derived list is a second source of truth: it drifts while
# looking current, which `ctest -R table-totals` exists to stop in the rulebook and
# nothing stopped here. So the number is checked too.
#
# The phrase is fixed -- `**<n> CommandTable rows**` -- rather than matched loosely
# near the word `CommandTable`. The first attempt did the loose thing and the number
# sat on a different LINE from the symbol, so a line-based extractor found nothing
# and refused a correct tree. A required phrase is also what makes the number
# findable: a reader searching for the count has one spelling to grep.
#
# Not `sed ... | head -1`: `head` leaves after the first line, `sed` takes SIGPIPE,
# and under `pipefail` the pipeline reports SED's status -- a false negative on the
# SUCCESS path. `${all%%$'\n'*}` takes the first line and forks nothing.
#
# @param 1 path to redis-resp.md
DocStatedCount() {
    local all
    all="$(sed -n 's/.*\*\*\([0-9][0-9]*\) CommandTable rows\*\*.*/\1/p' "$1")"
    printf '%s' "${all%%$'\n'*}"
}

# Compare one tree.
#
# @param 1 source directory
Verify() {
    local dir="$1"
    local impl="${dir}/src/FastCache/Protocol/RedisResp.cpp"
    local doc="${dir}/docs/protocols/redis-resp.md"
    local tableFile docFile verb stated tableCount docCount

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

    # Fail CLOSED on either extraction coming back empty. Two empty lists agree
    # perfectly, and that agreement is what a broken reader looks like.
    if [ "$tableCount" -lt 1 ]; then
        refuse "extracted no verbs from CommandTable -- the array literal's shape changed,
     so every documented verb 'matched' without anything being read"
        rm -f "$tableFile" "$docFile"
        return
    fi
    if [ "$docCount" -lt 1 ]; then
        refuse "extracted no verbs from the doc's '## Supported commands' section --
     the heading or the table shape changed, so nothing was compared"
        rm -f "$tableFile" "$docFile"
        return
    fi

    # A served verb the docs do not name.
    while IFS= read -r verb; do
        [ -n "$verb" ] || continue
        if ! grep -Fx -- "$verb" "$docFile" > /dev/null 2>&1; then
            refuse "${verb} is a CommandTable row and is not in
     docs/protocols/redis-resp.md's supported list. A verb the server answers and
     the docs do not name is how the whole RESP surface came to be described wrong."
        fi
    done < "$tableFile"

    # A documented verb that is neither a row nor a declared fast-track.
    while IFS= read -r verb; do
        [ -n "$verb" ] || continue
        if grep -Fx -- "$verb" "$tableFile" > /dev/null 2>&1; then continue; fi
        case " ${FastTrackVerbs} " in *" ${verb} "*) continue ;; esac
        refuse "${verb} is documented as supported and is neither a CommandTable row
     nor one of the fast-track verbs (${FastTrackVerbs}). Either it was removed and
     the docs kept it, or it needs adding to FastTrackVerbs in this check with a
     reason."
    done < "$docFile"

    # Every fast-track verb IS documented. Without this the list above is an
    # allowance with no obligation: drop `RESET` from the docs and nothing objects.
    for verb in $FastTrackVerbs; do
        if ! grep -Fx -- "$verb" "$docFile" > /dev/null 2>&1; then
            refuse "${verb} is answered by Dispatch ahead of the table and is not
     documented as supported."
        fi
    done

    # The stated count, against the derived one.
    stated="$(DocStatedCount "$doc")"
    if [ -z "$stated" ]; then
        refuse "the doc states no CommandTable row count. It must carry the phrase
     '**<n> CommandTable rows**', so the number is checked rather than remembered."
    elif [ "$stated" != "$tableCount" ]; then
        refuse "the doc says CommandTable has ${stated} rows; it has ${tableCount}."
    fi

    note "resp-command-docs: ${tableCount} CommandTable row(s), ${docCount} documented,"
    note "                   ${FastTrackVerbs} answered ahead of the table"

    rm -f "$tableFile" "$docFile"
}

# --- the self-test ---------------------------------------------------------
#
# Each verdict against a synthetic tree. A suite with only a correct case passes
# under every bug this check exists to catch, and the two ways it can fail to LOOK
# -- no table, no doc section -- are watched refusing alongside the drift cases.

StageTree() {
    local tree="$1" impl="$2" doc="$3"
    mkdir -p "${tree}/src/FastCache/Protocol" "${tree}/docs/protocols"
    printf '%s' "$impl" > "${tree}/src/FastCache/Protocol/RedisResp.cpp"
    printf '%s' "$doc" > "${tree}/docs/protocols/redis-resp.md"
}

GoodImpl() {
    cat <<'IMPL'
namespace
{
    constexpr auto CommandTable = std::array {
        CommandEntry { .name = "GET", .arity = 2 },
        CommandEntry { .name = "SET", .arity = -3 },
        CommandEntry { .name = "PING", .arity = -1 },
    };
}
// A DIFFERENT table in the same file, whose rows must not be read as RESP verbs.
constexpr auto OtherTable = std::array {
    Row { .name = "NOTAVERB" },
};
IMPL
}

GoodDoc() {
    cat <<'DOC'
# Redis RESP

The authoritative list is `CommandTable` in
`src/FastCache/Protocol/RedisResp.cpp` (**3 CommandTable rows**) plus `AUTH`,
`QUIT` and `RESET`.

## Supported commands

### Strings

| Command | Notes |
|---------|-------|
| `GET <key>` | Bulk string, or nil on a miss |
| `SET <key> <value> [EX\|PX <ttl>] [NX\|XX]` | Full options |

### Connection

| Command | Notes |
|---------|-------|
| `PING [msg]` | `+PONG` or bulk echo |
| `AUTH [user] <pass>` | Against `--requirepass` |
| `QUIT` | `+OK` and close |
| `RESET` | `+RESET` |

## What is not supported

`SINTER` and `ZADD` are not implemented.
DOC
}

RunSelfTest() {
    local ran=0 bad=0 tree out
    local caseName want

    Expect() {
        caseName="$1"; want="$2"
        ran=$(( ran + 1 ))
        out="$( failures=0; Verify "$tree" 2>&1; echo "__failures=$failures" )"
        local got
        got="$(printf '%s\n' "$out" | sed -n 's/^__failures=//p')"
        if [ "$want" = "pass" ] && [ "$got" != "0" ]; then
            echo "SELFTEST FAIL [${caseName}]: expected a pass, got ${got} refusal(s)" >&2
            printf '%s\n' "$out" | sed 's/^/    | /' >&2
            bad=$(( bad + 1 ))
        elif [ "$want" = "fail" ] && [ "$got" = "0" ]; then
            echo "SELFTEST FAIL [${caseName}]: expected a refusal, got none" >&2
            printf '%s\n' "$out" | sed 's/^/    | /' >&2
            bad=$(( bad + 1 ))
        fi
    }

    # 1. The correct tree. A check nobody has watched ACCEPT is not known to work,
    #    and its `none` arm is the one that had never been observed answering in the
    #    guard this repository filed as #1031.
    tree="$(mktemp -d)"
    StageTree "$tree" "$(GoodImpl)" "$(GoodDoc)"
    Expect "a correct tree passes" pass
    rm -rf "$tree"

    # 2. A served verb the docs do not name -- the drift that motivated this check.
    tree="$(mktemp -d)"
    StageTree "$tree" "$(GoodImpl | sed 's/"PING"/"SUBSCRIBE"/')" "$(GoodDoc)"
    Expect "an undocumented served verb is refused" fail
    rm -rf "$tree"

    # 3. A documented verb nothing serves.
    tree="$(mktemp -d)"
    StageTree "$tree" "$(GoodImpl)" "$(GoodDoc | sed 's/| `PING \[msg\]`/| `SINTER <key>`/')"
    Expect "a documented verb nothing serves is refused" fail
    rm -rf "$tree"

    # 4. A fast-track verb dropped from the docs. Without this case the fast-track
    #    list is an allowance carrying no obligation.
    tree="$(mktemp -d)"
    StageTree "$tree" "$(GoodImpl)" "$(GoodDoc | sed '/| `RESET`/d')"
    Expect "a fast-track verb missing from the docs is refused" fail
    rm -rf "$tree"

    # 5. The stated count drifting from the derived one.
    tree="$(mktemp -d)"
    StageTree "$tree" "$(GoodImpl)" "$(GoodDoc | sed 's/3 CommandTable rows/9 CommandTable rows/')"
    Expect "a wrong stated row count is refused" fail
    rm -rf "$tree"

    # 6. No stated count at all.
    tree="$(mktemp -d)"
    StageTree "$tree" "$(GoodImpl)" "$(GoodDoc | sed 's/(\*\*3 CommandTable rows\*\*) //')"
    Expect "a missing stated row count is refused" fail
    rm -rf "$tree"

    # 7. The array literal's shape changed, so the reader finds nothing. This must
    #    REFUSE and not pass vacuously: two empty lists agree perfectly.
    tree="$(mktemp -d)"
    StageTree "$tree" "$(GoodImpl | sed 's/constexpr auto CommandTable = std::array {/constexpr auto CommandTable = MakeTable(/')" "$(GoodDoc)"
    Expect "an unreadable CommandTable is refused, not passed" fail
    rm -rf "$tree"

    # 8. The doc's heading changed, so the doc reader finds nothing.
    tree="$(mktemp -d)"
    StageTree "$tree" "$(GoodImpl)" "$(GoodDoc | sed 's/^## Supported commands/## The commands/')"
    Expect "an unreadable supported section is refused, not passed" fail
    rm -rf "$tree"

    # 9. A row of ANOTHER table in the same file must not be read as a RESP verb.
    #    Case 1 covers this by construction -- `GoodImpl` carries `OtherTable` --
    #    so this case removes the bound and asserts the check then objects, which is
    #    what proves the bound is doing something.
    tree="$(mktemp -d)"
    StageTree "$tree" "$(GoodImpl | sed 's/^    };$/    };XX/')" "$(GoodDoc)"
    Expect "an unbounded read picks up a neighbouring table and is refused" fail
    rm -rf "$tree"

    echo "resp-command-docs-selftest: ${ran} case(s) ran, ${bad} failed"
    [ "$bad" -eq 0 ] || return 1
    return 0
}

if [ "$selfTest" = "yes" ]; then
    RunSelfTest || exit 1
    exit 0
fi

if [ -z "$sourceDir" ]; then
    echo "${self}: a source directory is required (or --self-test)" >&2
    exit 2
fi

Verify "$sourceDir"
[ "$failures" -eq 0 ] || exit 1
exit 0
