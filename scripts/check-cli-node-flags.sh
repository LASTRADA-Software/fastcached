#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Every `fastcache-compile-node` flag that `fastcache-cli --help` sends an operator to
# is an option that binary actually has.
#
# ## Why this exists
#
# `fastcache-cli --help` carries a NOTES paragraph naming the operator actions that live
# only in the other binary -- enrollment and `--print-surfaces` -- because an operator
# who has learned one tool cannot otherwise discover the other half of the surface
# (#1307). That paragraph is prose in one executable about another executable's flag
# table, and nothing else connects the two: rename `--enroll-open` in `NodeConfig.cpp`
# and every test in both binaries stays green while the help sends people to a flag that
# does not exist. A sentence is not a reader.
#
# So this asks the BINARIES, which are the subject, rather than grepping either source
# tree: the flags a node accepts are whatever its `--help` lists as option rows, however
# many tables they are assembled from, and a source scan would have to know that set.
#
# ## What it checks, precisely
#
#   * Only NOTES paragraphs that name `fastcache-compile-node`. A flag cited anywhere
#     else in `fastcache-cli --help` is `fastcache-cli`'s own and is not this check's.
#   * A cited flag must be an option ROW of the node's help: a line that starts with that
#     flag at the row indent. A flag mentioned only inside another row's description
#     (`--scheduler` appears in a dozen, some of them opening a wrapped line) is not an
#     option, and whole-token matching is what stops `--enroll` passing because
#     `--enroll-open` exists.
#
# ## What it does NOT cover, stated rather than left to be discovered
#
#   * Whether the paragraph's WORDS about each flag are true. That is a judgement about
#     prose; this check only refuses a citation of something that is not there.
#   * Flags the node has and the paragraph does not mention. Naming every node flag is
#     not the paragraph's job.
#
# ## Constraints
#
# bash 3.2 (macOS ships a 2007 `/bin/bash` and this runs in the default ctest set): no
# `mapfile`, `declare -A`, `${var^^}` or `local -n`. No `producer | grep -q` under
# `pipefail`. Input is read with CRs stripped, because a Windows executable writes its
# help in text mode.
set -uo pipefail

self="${0##*/}"
selfTest="no"
cli=""
node=""

while [ $# -gt 0 ]; do
    case "$1" in
        --self-test) selfTest="yes"; shift ;;
        --cli)
            [ $# -ge 2 ] || { echo "${self}: --cli needs a path" >&2; exit 2; }
            cli="$2"; shift 2
            ;;
        --node)
            [ $# -ge 2 ] || { echo "${self}: --node needs a path" >&2; exit 2; }
            node="$2"; shift 2
            ;;
        -h|--help)
            echo "usage: ${self} --cli <fastcache-cli> --node <fastcache-compile-node>"
            echo "       ${self} --self-test"
            exit 0
            ;;
        *) echo "${self}: unexpected argument: $1" >&2; exit 2 ;;
    esac
done

failures=0
note() { echo "$*"; }
refuse() {
    echo "FAIL ${self}: $*" >&2
    failures=$(( failures + 1 ))
}

# The flags cited by NOTES paragraphs that name `fastcache-compile-node`, one per line.
#
# The NOTES section runs from a line that is exactly `NOTES` to the next line that
# starts in column one; paragraphs inside it are separated by blank lines. A flag is
# `--` then lower-case words joined by single hyphens, so `cluster-*` in prose is not
# one and a trailing hyphen is not part of one.
#
# @param 1 a file holding `fastcache-cli --help`
CitedNodeFlags() {
    tr -d '\r' < "$1" | awk '
        function flush() {
            if (paragraph ~ /fastcache-compile-node/) {
                text = paragraph
                while (match(text, /--[a-z][a-z0-9]*(-[a-z0-9]+)*/)) {
                    print substr(text, RSTART, RLENGTH)
                    text = substr(text, RSTART + RLENGTH)
                }
                cited = 1
            }
            paragraph = ""
        }
        $0 == "NOTES"                { inside = 1; next }
        inside && /^[^ ]/ && $0 != "" { flush(); inside = 0 }
        !inside                      { next }
        /^[ ]*$/                     { flush(); next }
        { paragraph = paragraph " " $0 }
        END {
            if (inside) flush()
            # A marker line the caller tells apart from a flag, so "no paragraph named
            # the node" and "a paragraph named it and cited nothing" stay two refusals.
            if (cited) print "#cited"
        }
    ' | LC_ALL=C sort -u
}

# The flags a `fastcache-compile-node --help` lists as option rows, one per line.
#
# **A row is a flag at the ROW indent, not any line that starts with a flag.** A wrapped
# description continues in the description column, and in the real help a dozen of those
# continuation lines open with a flag -- `--scheduler` inside `--enroll-open`'s text,
# `--cluster-forget` inside `--enroll-reject`'s. Counting them would let a removed row
# pass as long as another row's prose still mentions it, which is the stale-prose case
# this check exists for. The row indent is the SHALLOWEST indent any flag-led line has,
# measured rather than written down, so a renderer that changes its indent does not turn
# every row into a non-row.
#
# @param 1 a file holding `fastcache-compile-node --help`
NodeOptionRows() {
    tr -d '\r' < "$1" | awk '
        match($0, /^[ ]+--[a-z][a-z0-9]*(-[a-z0-9]+)*/) {
            line = substr($0, RSTART, RLENGTH)
            flag = line
            sub(/^[ ]+/, "", flag)
            indent = length(line) - length(flag)
            count++
            indents[count] = indent
            flags[count] = flag
            if (least == "" || indent < least) least = indent
        }
        END {
            for (i = 1; i <= count; i++)
                if (indents[i] == least) print flags[i]
        }
    ' | LC_ALL=C sort -u
}

# Judge two help texts. The DECISION, separate from running any binary, so the self-test
# drives it over staged texts and the registered run reaches it through the real ones.
#
# @param 1 a file holding `fastcache-cli --help`
# @param 2 a file holding `fastcache-compile-node --help`
Judge() {
    local cliHelp="$1" nodeHelp="$2"
    local citedFile rowsFile flag citedCount rowCount

    citedFile="$(mktemp)"
    rowsFile="$(mktemp)"
    CitedNodeFlags "$cliHelp" > "$citedFile"
    NodeOptionRows "$nodeHelp" > "$rowsFile"

    # Fail CLOSED on every way of reading nothing. An empty citation list against any
    # node passes perfectly, and that is exactly what a moved or renamed NOTES heading
    # would otherwise report forever.
    if ! grep -Fxq "#cited" "$citedFile"; then
        refuse "no NOTES paragraph of fastcache-cli --help names fastcache-compile-node, so there is nothing to check"
        rm -f "$citedFile" "$rowsFile"
        return
    fi
    citedCount="$(grep -cv '^#cited$' "$citedFile" | tr -d ' ')"
    rowCount="$(grep -c . "$rowsFile" | tr -d ' ')"
    if [ "$citedCount" -lt 1 ]; then
        refuse "the NOTES paragraph naming fastcache-compile-node cites no flag"
        rm -f "$citedFile" "$rowsFile"
        return
    fi
    if [ "$rowCount" -lt 1 ]; then
        refuse "fastcache-compile-node --help lists no option rows, so no citation can be judged"
        rm -f "$citedFile" "$rowsFile"
        return
    fi

    while IFS= read -r flag; do
        [ -n "$flag" ] || continue
        [ "$flag" = "#cited" ] && continue
        if grep -Fxq -- "$flag" "$rowsFile"; then
            note "  ok   ${flag}"
        else
            refuse "fastcache-cli --help sends operators to fastcache-compile-node ${flag}, which is not an option row of its --help"
        fi
    done < "$citedFile"

    note "${self}: ${citedCount} node flag(s) cited, ${rowCount} node option row(s)"
    rm -f "$citedFile" "$rowsFile"
}

# Run one binary's --help into a file, refusing when it does not answer.
#
# @param 1 the executable
# @param 2 the output file
Capture() {
    local exe="$1" out="$2" status
    if [ ! -x "$exe" ]; then
        refuse "cannot execute ${exe}"
        return 1
    fi
    "$exe" --help > "$out" 2>&1
    status=$?
    if [ "$status" -ne 0 ]; then
        refuse "${exe} --help exited ${status}"
        return 1
    fi
    return 0
}

# --- self-test ----------------------------------------------------------------------
#
# Both directions, and every way the reader can fail to LOOK. A suite whose only case
# is a correct pair passes under every bug this check exists to catch; and the correct
# case is not decoration either, because a guard nobody has watched ACCEPT is not known
# to work.

StageCli() {
    local file="$1" paragraph="$2"
    {
        echo "fastcache-cli - operate a fastcached cache and a compile fleet from a terminal."
        echo ""
        echo "OPTIONS"
        echo "  --quiet        no remarks"
        echo ""
        echo "NOTES"
        echo "  Remarks go to stderr in every format. Redirect them, or pass --quiet."
        echo ""
        printf '%s\n' "$paragraph"
    } > "$file"
}

StageNode() {
    local file="$1" rows="$2" row
    {
        echo "fastcache-compile-node - a compile worker for fastcached distributed builds."
        echo ""
        echo "OPTIONS"
        for row in $rows; do
            echo "  ${row}                   something it does, naming --scheduler in passing"
            # A wrapped description line that OPENS with a flag, as the real help's do.
            echo "                             --wrapped-only starts this continuation line"
        done
    } > "$file"
}

RunCase() {
    local name="$1" want="$2" cliHelp="$3" nodeHelp="$4"
    local before="$failures"
    Judge "$cliHelp" "$nodeHelp" > /dev/null 2>&1
    local got="pass"
    [ "$failures" -gt "$before" ] && got="fail"
    # Reset per case: a self-test tallying its subject's refusals as its own would report
    # every deliberate refusal as a failure of the run.
    failures="$before"
    if [ "$got" = "$want" ]; then
        echo "  ok   ${name} (${got})"
        return 0
    fi
    echo "  FAIL ${name}: expected ${want}, got ${got}" >&2
    return 1
}

# At FILE scope rather than `local`: the EXIT trap runs after SelfTest has returned, and
# a local out of scope is an unbound variable under `set -u` -- after the verdict.
selfTestTmp=""
CleanSelfTest() { [ -n "$selfTestTmp" ] && rm -rf "$selfTestTmp"; }

SelfTest() {
    local bad=0 ran=0 tmp
    selfTestTmp="$(mktemp -d)"
    tmp="$selfTestTmp"
    trap CleanSelfTest EXIT

    local cited="  Some actions are fastcache-compile-node flags: --enroll-open and
  --print-surfaces, which dials nothing."

    StageCli "${tmp}/cli-agree" "$cited"
    StageNode "${tmp}/node-agree" "--enroll-open --print-surfaces --scheduler=<host:port>"
    RunCase "every cited flag is a node option row" pass "${tmp}/cli-agree" "${tmp}/node-agree" || bad=1
    ran=$(( ran + 1 ))

    StageNode "${tmp}/node-renamed" "--enroll-window-open --print-surfaces"
    RunCase "a cited flag the node no longer has" fail "${tmp}/cli-agree" "${tmp}/node-renamed" || bad=1
    ran=$(( ran + 1 ))

    # `--scheduler` is in every staged row's DESCRIPTION and in no row's first column.
    StageCli "${tmp}/cli-prose" "  A fastcache-compile-node flag: --scheduler."
    StageNode "${tmp}/node-prose" "--enroll-open"
    RunCase "a flag named only inside another row's description is not an option" fail "${tmp}/cli-prose" "${tmp}/node-prose" || bad=1
    ran=$(( ran + 1 ))

    # `--wrapped-only` opens a continuation line under every staged row and is no row.
    StageCli "${tmp}/cli-wrapped" "  A fastcache-compile-node flag: --wrapped-only."
    RunCase "a flag opening a wrapped description line is not an option" fail "${tmp}/cli-wrapped" "${tmp}/node-agree" || bad=1
    ran=$(( ran + 1 ))

    StageCli "${tmp}/cli-prefix" "  A fastcache-compile-node flag: --enroll."
    RunCase "a prefix of a real flag is not that flag" fail "${tmp}/cli-prefix" "${tmp}/node-agree" || bad=1
    ran=$(( ran + 1 ))

    # `--quiet` is cited in a NOTES paragraph that does not name the node, and the staged
    # node has no such row: it must not be judged.
    RunCase "a flag in a paragraph that does not name the node is not checked" pass "${tmp}/cli-agree" "${tmp}/node-agree" || bad=1
    ran=$(( ran + 1 ))

    # The reader failing to LOOK, three ways.
    StageCli "${tmp}/cli-absent" "  Nothing here names the other binary, only --quiet."
    RunCase "no paragraph naming the node refuses rather than passing" fail "${tmp}/cli-absent" "${tmp}/node-agree" || bad=1
    ran=$(( ran + 1 ))

    StageCli "${tmp}/cli-noflags" "  See fastcache-compile-node for enrollment."
    RunCase "a paragraph naming the node and citing nothing refuses" fail "${tmp}/cli-noflags" "${tmp}/node-agree" || bad=1
    ran=$(( ran + 1 ))

    printf 'fastcache-compile-node\n\nno options here\n' > "${tmp}/node-empty"
    RunCase "a node help with no option rows refuses" fail "${tmp}/cli-agree" "${tmp}/node-empty" || bad=1
    ran=$(( ran + 1 ))

    # Text-mode output from a Windows executable.
    sed 's/$/\r/' "${tmp}/cli-agree" > "${tmp}/cli-crlf"
    sed 's/$/\r/' "${tmp}/node-agree" > "${tmp}/node-crlf"
    RunCase "CRLF help texts are read as their LF selves" pass "${tmp}/cli-crlf" "${tmp}/node-crlf" || bad=1
    ran=$(( ran + 1 ))

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

if [ -z "$cli" ] || [ -z "$node" ]; then
    echo "${self}: --cli and --node are both required" >&2
    exit 2
fi

scratch="$(mktemp -d)"
CleanScratch() { rm -rf "$scratch"; }
trap CleanScratch EXIT

if Capture "$cli" "${scratch}/cli.txt" && Capture "$node" "${scratch}/node.txt"; then
    Judge "${scratch}/cli.txt" "${scratch}/node.txt"
fi

if [ "$failures" -gt 0 ]; then
    echo "${self}: ${failures} problem(s)" >&2
    exit 1
fi
echo "${self}: OK"
exit 0
