#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Run a Catch2 binary and say which of FIVE things happened to it.
#
#   scripts/run-suite.sh -- out/build/gcc-debug/target/FastCacheTest
#   scripts/run-suite.sh --retries 2 -- <binary> "[frame]"
#   scripts/run-suite.sh --self-test
#
# ## Why this exists (#1212)
#
# **A test binary that CRASHES reports zero failures in every summary format
# there is**, and a mutation harness asks exactly one question -- was this
# mutation caught? -- which it answers from a failure count. So a run that died
# partway through arrives as "0 of 482 cases failed" and is scored CAUGHT:
# indistinguishable from a real result, and pointing the wrong way, toward
# believing the guard works.
#
# Crashed, not-caught and caught are three states, and the harness that surfaced
# this had two. Measured on this repository:
#
#   * a truncated JUnit document still carries failures="0" errors="0" on the
#     elements it did emit;
#   * the console reporter prints no summary line at all;
#   * ctest is the one consumer that already distinguishes them -- a crash is
#     ***Exception, not ***Failed -- which is exactly why nobody has met this
#     through ctest, and why a harness running the binary directly does.
#
# It was caught by a DENOMINATOR THAT MOVED (482 / 496 / 530), not by a verdict
# that looked wrong. A crash dying at a stable point has no tell at all.
#
# ## Neither input is sufficient alone, and the reason is arithmetic
#
# **Catch2 spends its exit status on the failure count**, clamped at 255. So a
# run killed by SIGSEGV (128 + 11 = 139) and a run with 139 failing assertions
# produce the SAME status, and no reading of the status alone separates them.
# Completeness does: a killed run's report stops mid-document. And #1211 measured
# the other direction too -- exits 0 / 139 / 0 / 0 / 139 / 139 across six runs
# whose element counts were 551 / 507 / 551 / 551 / 518 / 518, so a SHORT run
# exited 0 at least once.
#
# Completeness is therefore asked FIRST, and the status only refines it.
#
# ## A FIFTH state the ticket does not name, measured here
#
# A filter matching nothing exits 2 and prints "No tests ran" -- and to a
# count-reader that is another zero. It is the mutation harness's own likeliest
# failure: an anchor that has stopped matching, a renamed tag, a typo in a spec.
# So selected-nothing is its own outcome and never a pass.
#
# ## The retry, and why it is named
#
# A crash is retried, because a harness that scores one has measured nothing --
# and the retry is REPORTED with its count, since a retry that hides an
# instrument's own failures is how this class of defect stays invisible. Only
# crashed is retried, never failed.
#
# ## Not a stderr grep
#
# Deliberately no malloc() pattern. That is the signature the crash in #1211
# happens to have, and the next one aborts somewhere else. The verdict comes from
# the three inputs below and from nothing the subject chooses to print.
set -uo pipefail

Reporter="junit"
Retries=1
Out=""
SelfTest="no"

Usage() {
    echo "usage: $(basename "$0") [--reporter junit|console] [--out FILE] [--retries N] -- <binary> [args...]" >&2
    echo "       $(basename "$0") --self-test" >&2
    echo "  exit 0 passed, 1 failed, 3 crashed after retries, 4 selected nothing, 2 usage or unknown" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --reporter) Reporter="${2:-}"; shift 2 ;;
        --out) Out="${2:-}"; shift 2 ;;
        --retries) Retries="${2:-}"; shift 2 ;;
        --self-test) SelfTest="yes"; shift ;;
        --) shift; break ;;
        -h|--help) Usage ;;
        *) echo "unknown option: $1" >&2; Usage ;;
    esac
done

# ---------------------------------------------------------------------------
# THE DECISION. Three readings in, one word out, nothing else.
#
# Pure, and driven by --self-test, because the acquisition needs a built binary
# and a crash that happens a few times in twelve -- which is exactly the
# population a verdict table must not be reachable only from.
#
# @param 1 the child's wait status
# @param 2 whether the report is complete: yes or no
# @param 3 how many cases the report carries
# @return prints one of: crashed | selected-nothing | passed | failed | unknown
SuiteVerdict() {
    local status="$1" complete="$2" cases="$3"
    case "$status" in ""|*[!0-9]*) echo "unknown"; return ;; esac
    case "$cases" in ""|*[!0-9]*) echo "unknown"; return ;; esac
    case "$complete" in
        yes|no) ;;
        *) echo "unknown"; return ;;
    esac
    # COMPLETENESS FIRST. A killed run and a run with 139 failures share a status,
    # and this is the only thing that tells them apart.
    if [ "$complete" != "yes" ]; then
        echo "crashed"
        return
    fi
    if [ "$cases" -eq 0 ]; then
        echo "selected-nothing"
        return
    fi
    if [ "$status" -eq 0 ]; then
        echo "passed"
        return
    fi
    echo "failed"
}

# What a status means beyond its number, when it means anything more.
# @param 1 the wait status
StatusDetail() {
    local status="$1"
    if [ "$status" -gt 128 ] && [ "$status" -lt 192 ]; then
        echo " (killed by signal $((status - 128)))"
    else
        echo ""
    fi
}

# The exit status this script answers with, per verdict. Named, because the same
# integer at three sites is three chances to disagree.
# @param 1 the verdict word
VerdictStatus() {
    case "$1" in
        passed) echo 0 ;;
        failed) echo 1 ;;
        crashed) echo 3 ;;
        selected-nothing) echo 4 ;;
        *) echo 2 ;;
    esac
}

if [ "$SelfTest" = "yes" ]; then
    cases=0
    failures=0
    Case() {
        local what="$1" status="$2" complete="$3" seen="$4" want="$5" got
        cases=$((cases + 1))
        got="$(SuiteVerdict "$status" "$complete" "$seen")"
        if [ "$got" = "$want" ]; then
            echo "  ok    ${what} -> ${got}"
        else
            failures=$((failures + 1))
            echo "  FAIL  ${what} -> ${got}, wanted ${want}" >&2
        fi
    }

    # The POSITIVE direction first: every refusal below is evidence only if an
    # ordinary clean run is reported as one.
    Case "a clean complete run" 0 yes 436 passed
    Case "a complete run with failures" 7 yes 436 failed

    # The collision this file exists for. Same status, opposite verdicts, and only
    # completeness separates them.
    Case "139 failing assertions, report complete" 139 yes 436 failed
    Case "killed by SIGSEGV, report truncated" 139 no 507 crashed

    # And the direction the status cannot see at all: #1211 measured a SHORT run
    # that exited 0.
    Case "a short run that still exited 0" 0 no 507 crashed

    # The fifth state, measured rather than assumed: Catch2 exits 2 and prints
    # "No tests ran" for a filter that matches nothing, and a count-reader sees
    # another zero.
    Case "a filter that matched nothing" 2 yes 0 selected-nothing
    Case "and a zero status does not rescue it" 0 yes 0 selected-nothing

    # Readings that are not answers. A verdict function that coerces them invents
    # one, silently.
    Case "a non-numeric status" "" yes 436 unknown
    Case "a non-numeric case count" 0 yes "" unknown
    Case "a completeness nobody enumerated" 0 maybe 436 unknown

    echo "run-suite: self-test ran ${cases} case(s), ${failures} failure(s)"
    if [ "$failures" -ne 0 ]; then
        echo "run-suite: self-test FAILED" >&2
        exit 1
    fi
    echo "run-suite: self-test passed (${cases} cases)"
    exit 0
fi

[ "$#" -ge 1 ] || Usage
binary="$1"
shift
[ -x "$binary" ] || { echo "run-suite: no executable at ${binary}" >&2; exit 2; }
case "$Retries" in ""|*[!0-9]*) echo "run-suite: --retries takes a number" >&2; exit 2 ;; esac
case "$Reporter" in junit|console) ;; *) echo "run-suite: --reporter takes junit or console" >&2; exit 2 ;; esac

report="$Out"
if [ -z "$report" ]; then
    report="$(mktemp)" || { echo "run-suite: cannot create a scratch file" >&2; exit 2; }
fi

attempt=0
verdict="unknown"
status=0
complete="no"
seen=0
while [ "$attempt" -le "$Retries" ]; do
    attempt=$((attempt + 1))
    if [ "$Reporter" = junit ]; then
        "$binary" ${1+"$@"} -r junit -o "$report" > /dev/null 2>&1
        status=$?
        # The document's own terminator, never a phrase the subject chooses. A
        # truncated report ends mid-attribute, so this is absent exactly when the
        # run did not finish.
        complete="no"
        grep -aq "</testsuites>" "$report" 2>/dev/null && complete="yes"
        seen="$(grep -ac "<testcase " "$report" 2>/dev/null || true)"
    else
        "$binary" ${1+"$@"} > "$report" 2>&1
        status=$?
        # Measured on this tree: a completed console run ends with
        # "All tests passed (...)", or a "test cases:" summary block when anything
        # failed, or "No tests ran" when the filter matched nothing. A killed run
        # prints none of the three.
        complete="no"
        if grep -aqE "^(All tests passed|No tests ran|test cases:)" "$report" 2>/dev/null; then
            complete="yes"
        fi
        seen="$(sed -n "s/.*in \([0-9][0-9]*\) test case.*/\1/p" "$report" | tail -1)"
        [ -n "$seen" ] || seen=0
        grep -aq "^No tests ran" "$report" 2>/dev/null && seen=0
    fi
    verdict="$(SuiteVerdict "$status" "$complete" "$seen")"
    [ "$verdict" = "crashed" ] || break
    if [ "$attempt" -le "$Retries" ]; then
        # SAID, not swallowed. A retry that hides an instrument's own failures is
        # the defect this file removes, one level up.
        echo "run-suite: attempt ${attempt} CRASHED (exit ${status}$(StatusDetail "$status"), ${seen} case(s), report incomplete) -- retrying"
    fi
done

reportState="complete"
[ "$complete" = "yes" ] || reportState="INCOMPLETE"
echo "== SUITE CONCLUDED: ${verdict} -- ${binary} ${*:-} (exit ${status}$(StatusDetail "$status"), ${seen} case(s), report ${reportState}, ${attempt} attempt(s))"
case "$verdict" in
    crashed)
        echo "   A crashed run is NOT zero failures. Nothing about the subject was measured;"
        echo "   score it as the instrument failing, never as a mutation being caught."
        ;;
    selected-nothing)
        echo "   The filter matched no test case. To a count-reader that is another zero --"
        echo "   check the spec before believing any verdict drawn from it."
        ;;
esac
exit "$(VerdictStatus "$verdict")"
