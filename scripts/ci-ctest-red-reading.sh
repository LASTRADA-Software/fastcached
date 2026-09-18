#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# What a red Windows `Test` step can and cannot say about the tree (#1515).
#
# Usage:  scripts/ci-ctest-red-reading.sh --log <file> --spawn <reading>
#         scripts/ci-ctest-red-reading.sh --self-test
#
#   --log     the file `ctest --output-log` wrote in the `Test` step
#   --spawn   what the after-red spawn probe read: `ok`, `broken` or `unconcluded`.
#             Anything else -- including the EMPTY string a misspelt step id
#             substitutes -- is read as `unconcluded`, never as `ok`.
#
# Prints GitHub annotations and, last, exactly one `ctest-red-reading: <verdict>`
# line. Exits 0 on every verdict: this is a classifier on an already-failing job,
# and it must be able to explain the verdict and never to change it.
#
# ## Why this exists
#
# The spawn bracket around `ctest` (#966) used to end in "spawning still works, so
# this red is not #966's runner-state shape; read the failures as being about the
# tree". On #1514 it printed exactly that over ONE red: `mkdocs-validation`, a
# check measured at 170 ms, ***Timeout at 60.34 s on a branch whose inputs were
# disjoint from it. The runner could still start a process, so the probe was right
# about what it tested -- and wrong about the conclusion it licensed, because **a
# degradation guard that probes one capability cannot clear a runner that degraded
# in another**. It told the reader, in as many words, to blame the tree.
#
# ## A timeout is not a verdict about the tree
#
# A hang in the tree and a runner that stopped making progress both end in a
# ctest timeout, and nothing in the log separates them. So a timeout NEVER
# reaches the `tree` verdict, whatever the spawn probe read; it is named, with the
# tests it happened to, and the reader is sent to compare those against their cost
# on a green run of the same leg. `TIMEOUT` is not raised anywhere: a sub-second
# check that did not finish in 60 s did not run out of budget.
#
# ## Why the log, and why `--output-log`
#
# Because it is the only place ctest writes the distinction. `LastTest.log` records
# a timeout and a failed assertion identically -- `Test Failed.` -- while the
# console and `--output-log` carry `***Timeout` and, in the summary, `N - name
# (Timeout)`. Probed with ctest 4.2.3 against a two-test project, one sleeping past
# its TIMEOUT and one exiting 1.
#
# ## Every way of not being able to read is its own verdict
#
# `no-log`, `unconcluded` and `unparsed` are three different reasons nothing can
# be said, and each is a warning rather than a fall-through: the `*)` arm of a
# classifier is where the last state collapse in this family lived. And the
# FAILED list is checked against ctest's own count before anything is concluded
# from it -- absence of a `(Timeout)` row means nothing unless the rows that ARE
# there are all of them.
#
# ## bash 3.2
#
# Registered in the default ctest set, so it runs on macOS's 2007 `/bin/bash`, and
# it ships on Git Bash. No `mapfile`, no `declare -A`, no `${var^^}`; regexes live
# in variables, which is the one spelling 3.2 and 5.x read alike.

set -euo pipefail

# ctest's summary: `99% tests passed, 1 tests failed out of 4834`.
SummaryPattern='^[0-9]+% tests passed, ([0-9]+) tests? failed out of ([0-9]+)'
# A row of the FAILED list: `	  4673 - mkdocs-validation (Timeout)`. The LAST
# parenthesised group is ctest's classification, so a name that itself contains
# parentheses -- or the word Timeout -- is not misread.
RowPattern='^[[:space:]]+[0-9]+ - (.+) \(([^()]+)\)[[:space:]]*$'
FailedHeader='The following tests FAILED:'

# How many timed-out names an annotation lists before it says how many more.
NamesShown=10

# Read @p 1 and set the facts every verdict is drawn from. Pure over the file.
#
# Sets: logState (absent|present), summaryFailed, summaryTotal (empty when ctest
# printed no summary), rowCount, timeoutCount, timeoutNames.
ReadLog() {
    local log="$1" line inFailed=0
    logState=absent
    summaryFailed=""
    summaryTotal=""
    rowCount=0
    timeoutCount=0
    timeoutNames=""
    [[ -f "$log" && -r "$log" ]] || return 0
    logState=present
    while IFS= read -r line || [[ -n "$line" ]]; do
        # ctest writes this log in text mode, so on Windows every line ends in CR.
        line="${line%$'\r'}"
        if [[ "$line" == "$FailedHeader" ]]; then
            inFailed=1
            continue
        fi
        if [[ "$inFailed" -eq 1 ]]; then
            if [[ "$line" =~ $RowPattern ]]; then
                rowCount=$((rowCount + 1))
                if [[ "${BASH_REMATCH[2]}" == "Timeout" ]]; then
                    timeoutCount=$((timeoutCount + 1))
                    if [[ "$timeoutCount" -le "$NamesShown" ]]; then
                        timeoutNames="${timeoutNames:+$timeoutNames, }${BASH_REMATCH[1]}"
                    fi
                fi
                continue
            fi
            # The list ends at the first line that is not a row.
            inFailed=2
        fi
        if [[ "$line" =~ $SummaryPattern ]]; then
            summaryFailed="${BASH_REMATCH[1]}"
            summaryTotal="${BASH_REMATCH[2]}"
        fi
    done < "$log"
}

# The verdict, from the facts `ReadLog` set and the spawn reading @p 1.
#
# Order is precedence: what cannot be read first, then what the runner says, and
# `tree` only when every other reading has been asked and none objected.
Decide() {
    local spawn="$1"
    if [[ "$logState" != present ]]; then
        echo "no-log"
    elif [[ -z "$summaryTotal" ]]; then
        echo "unconcluded"
    elif [[ "$summaryFailed" -eq 0 ]]; then
        echo "ctest-green"
    elif [[ "$rowCount" -ne "$summaryFailed" ]]; then
        echo "unparsed"
    elif [[ "$spawn" == broken ]]; then
        echo "runner"
    elif [[ "$timeoutCount" -gt 0 ]]; then
        echo "timeouts"
    elif [[ "$spawn" != ok ]]; then
        echo "spawn-unconcluded"
    else
        echo "tree"
    fi
}

# The annotations for verdict @p 1, about log @p 2.
Render() {
    local verdict="$1" log="$2" more=""
    if [[ "$timeoutCount" -gt "$NamesShown" ]]; then
        more=" and $((timeoutCount - NamesShown)) more"
    fi
    case "$verdict" in
        no-log)
            echo "::warning::there is no ctest log at $log -- the Test step did not run, or did not get as far as writing one -- so nothing here classifies this red (#1515)"
            ;;
        unconcluded)
            echo "::warning::$log has no ctest summary, so ctest did not conclude -- killed, or the step itself ran out of time; this red is unclassified (#1515)"
            ;;
        ctest-green)
            echo "::notice::ctest passed all $summaryTotal tests, so this red came from another step, and nothing here classifies it"
            ;;
        unparsed)
            echo "::warning::ctest counted $summaryFailed failures but its FAILED list has $rowCount readable rows, so no conclusion is drawn from that list; this red is unclassified (#1515)"
            ;;
        runner)
            echo "::error::spawning worked before ctest and does not now, so the $summaryFailed failures above are about the runner and not the tree (#966)."
            if [[ "$timeoutCount" -gt 0 ]]; then
                echo "::notice::$timeoutCount of them timed out: $timeoutNames$more"
            fi
            ;;
        timeouts)
            echo "::warning::$timeoutCount of the $summaryFailed failures are TIMEOUTS: $timeoutNames$more."
            echo "::warning::A timeout is not a verdict about the tree: a hang in the tree and a runner that stopped making progress both end in one, and a spawn probe cannot clear a runner that stalled in something else (#1515). Compare each against its cost on a green run of this leg before reading it as a regression."
            ;;
        spawn-unconcluded)
            echo "::warning::every one of the $summaryFailed failures is a failed test rather than a timeout, but the spawn probe did not conclude, so this red is unclassified (#966)"
            ;;
        tree)
            echo "::notice::every one of the $summaryFailed failures is a failed test rather than a timeout, and spawning still works, so neither runner reading applies; read the failures as being about the tree."
            ;;
        *)
            echo "::warning::ci-ctest-red-reading decided '$verdict', which it has no rendering for; this red is unclassified"
            ;;
    esac
    echo "ctest-red-reading: $verdict"
}

Classify() {
    local log="$1" spawn="$2" verdict
    ReadLog "$log"
    verdict="$(Decide "$spawn")"
    Render "$verdict" "$log"
}

# ---------------------------------------------------------------------------
# --self-test: every verdict, from staged logs, in both line-ending spellings.

# Global rather than local: the EXIT trap runs after `SelfTest` has returned.
selfTestDir=""

SelfTest() {
    local dir ran=0 failed=0
    selfTestDir="$(mktemp -d "${TMPDIR:-/tmp}/ctest-red-reading.XXXXXX")"
    trap '[[ -z "$selfTestDir" ]] || rm -r "$selfTestDir"' EXIT
    dir="$selfTestDir"

    # Writes a log whose lines are the remaining arguments.
    Stage() {
        local name="$1"
        shift
        printf '%s\n' "$@" > "$dir/$name.log"
    }

    Expect() {
        local what="$1" want="$2" log="$3" spawn="$4" got
        ran=$((ran + 1))
        got="$(Classify "$dir/$log.log" "$spawn" | sed -n 's/^ctest-red-reading: //p')"
        if [[ "$got" == "$want" ]]; then
            echo "ok: $what -> $want"
        else
            echo "  FAIL: $what: wanted '$want', got '$got'" >&2
            failed=$((failed + 1))
        fi
    }

    # The shape #1515 was filed about, as the Windows runner printed it.
    Stage issue1515 \
        '4674/4834 Test #4673: mkdocs-validation ....................***Timeout  60.34 sec' \
        '' \
        '99% tests passed, 1 tests failed out of 4834' \
        '' \
        'Total Test time (real) = 900.10 sec' \
        '' \
        'The following tests FAILED:' \
        '	4673 - mkdocs-validation (Timeout)' \
        'Errors while running CTest'
    Stage failed \
        '98% tests passed, 2 tests failed out of 4834' \
        'The following tests FAILED:' \
        '	  12 - FleetView renders the forgotten section (Failed)' \
        '	  40 - cli-e2e (SEGFAULT)' \
        'Errors while running CTest'
    Stage mixed \
        '98% tests passed, 2 tests failed out of 4834' \
        'The following tests FAILED:' \
        '	  12 - slow one (Timeout)' \
        '	  40 - failing one (Failed)'
    # A name containing the word, and parentheses, is not a timeout.
    Stage named \
        '99% tests passed, 1 tests failed out of 10' \
        'The following tests FAILED:' \
        '	   7 - Timeout (bounded) handling is honoured (Failed)'
    # The did-not-run list comes FIRST and its rows have the same shape.
    Stage skipped \
        'The following tests did not run:' \
        '	   3 - e2e-helpers-selftest (Skipped)' \
        '	   4 - slow on purpose (Timeout)' \
        '99% tests passed, 1 tests failed out of 10' \
        'The following tests FAILED:' \
        '	   9 - a real failure (Failed)'
    Stage green '100% tests passed, 0 tests failed out of 4834'
    Stage killed \
        '1/4834 Test    #1: first ......   Passed    0.01 sec' \
        '2/4834 Test    #2: second .....   Passed    0.01 sec'
    # ctest counted two and only one row is readable: no conclusion from the list.
    Stage short \
        '98% tests passed, 2 tests failed out of 4834' \
        'The following tests FAILED:' \
        '	  12 - one (Failed)' \
        'something ctest never prints'
    Stage empty
    local many=()
    local i
    for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
        many+=("	  $i - hung $i (Timeout)")
    done
    Stage many '99% tests passed, 12 tests failed out of 4834' 'The following tests FAILED:' "${many[@]}"

    Expect "#1515's single timeout is never a tree verdict" timeouts issue1515 ok
    Expect "a timeout outranks an unconcluded spawn reading too" timeouts issue1515 unconcluded
    Expect "failed assertions with spawning intact read as the tree" tree failed ok
    Expect "one timeout among failures still withholds the tree verdict" timeouts mixed ok
    Expect "a name spelling Timeout is not a timeout" tree named ok
    Expect "the did-not-run list is not the FAILED list" tree skipped ok
    Expect "spawning broken after ctest is the runner" runner failed broken
    Expect "the runner outranks timeouts" runner issue1515 broken
    Expect "an unconcluded spawn probe clears nothing" spawn-unconcluded failed unconcluded
    # A misspelt step id substitutes EMPTY. That must not read as `ok`.
    Expect "an empty spawn reading clears nothing" spawn-unconcluded failed ""
    Expect "an unknown spawn reading clears nothing" spawn-unconcluded failed "OK"
    Expect "a green ctest is another step's red" ctest-green green ok
    Expect "no summary means ctest did not conclude" unconcluded killed ok
    Expect "an empty log is not a green one" unconcluded empty ok
    Expect "no log at all" no-log absent ok
    Expect "a FAILED list shorter than ctest's count is refused" unparsed short ok
    Expect "twelve timeouts are all counted" timeouts many ok

    # The log as Windows writes it: every line CR-terminated.
    local crlf
    for crlf in issue1515 failed skipped green; do
        sed 's/$/\r/' "$dir/$crlf.log" > "$dir/$crlf-crlf.log"
    done
    Expect "CRLF: the #1515 shape" timeouts issue1515-crlf ok
    Expect "CRLF: failed assertions" tree failed-crlf ok
    Expect "CRLF: the did-not-run list" tree skipped-crlf ok
    Expect "CRLF: green" ctest-green green-crlf ok

    # What the annotations SAY, not only which verdict: the three sentences #1515 is
    # about. `tree` must be the only verdict telling a reader to blame the tree.
    ran=$((ran + 1))
    local verdict leaked=""
    for verdict in no-log unconcluded ctest-green unparsed runner timeouts spawn-unconcluded; do
        # A herestring, never `Render | grep -q`: under `pipefail` the producer dies of
        # SIGPIPE on the MATCH and the pipeline reports that -- a leak read as clean.
        ReadLog "$dir/mixed.log"
        if grep -q "about the tree\." <<< "$(Render "$verdict" x)"; then
            leaked="${leaked:+$leaked, }$verdict"
        fi
    done
    if [[ -z "$leaked" ]]; then
        echo "ok: only the tree verdict tells a reader to blame the tree"
    else
        echo "  FAIL: these verdicts tell a reader to blame the tree: $leaked" >&2
        failed=$((failed + 1))
    fi
    ran=$((ran + 1))
    ReadLog "$dir/many.log"
    local rendered
    rendered="$(Render timeouts x)"
    if [[ "$rendered" == *"hung 10, "* || "$rendered" != *"hung 10 and 2 more"* ]]; then
        echo "  FAIL: twelve timeouts should name ten and say two more; rendered: $rendered" >&2
        failed=$((failed + 1))
    else
        echo "ok: a long timeout list names ten and counts the rest"
    fi

    echo "ci-ctest-red-reading --self-test: $ran checks ran, $failed failed"
    [[ "$failed" -eq 0 ]]
}

# ---------------------------------------------------------------------------

log=""
spawn=""
haveLog=0
case "${1:-}" in
    --self-test)
        SelfTest
        exit $?
        ;;
esac
Usage() {
    echo "usage: $0 --log <file> --spawn <ok|broken|unconcluded> | --self-test" >&2
    exit 2
}
while [[ $# -gt 0 ]]; do
    [[ $# -ge 2 ]] || Usage
    case "$1" in
        --log)
            log="${2:-}"
            haveLog=1
            shift 2
            ;;
        --spawn)
            spawn="${2:-}"
            shift 2
            ;;
        *)
            Usage
            ;;
    esac
done
[[ "$haveLog" -eq 1 && -n "$log" ]] || Usage
Classify "$log" "$spawn"
