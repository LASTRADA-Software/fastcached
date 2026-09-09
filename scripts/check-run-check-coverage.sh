#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every hygiene check ctest runs as `scripts/check-<name>.sh` goes through
# `run-check.sh`.
#
# That scope is stated in the first sentence rather than left to the pattern
# below, because a scan is silent about what it cannot reach and silence reads
# identically to complete coverage (#492).
#
# What is OUTSIDE it is DERIVED and printed on every run, never listed here. A
# hand-kept exclusion list is #492 one level along -- exact about what its author
# remembered, silent about the rest -- and the first draft of this paragraph
# proved it: it named six scripts plus "the e2e fixtures", while
# `reactor-teardown-gate-selftest.sh`, `sccache-smoke.sh` and `tls-smoke.sh` were
# neither. `Audit` counts the uncovered invocations from the registrations
# themselves, so the number moves when the tree does.
#
# ## The gap this leaves, stated rather than implied
#
# The `cmake -P` checks are NOT covered, and that is a decision with a residual
# rather than a clean boundary. The wrapper runs `"$interpreter" "$check"` -- it
# invokes a SHELL on a SCRIPT -- so covering a `cmake -P` registration means a
# wrapper that takes an arbitrary COMMAND, which is a different tool and not a
# wider pattern.
#
# The hazard does not stop at the boundary, and the tempting argument that it
# does is wrong. It runs: a `cmake -P` check's verdict is read from its OUTPUT
# (`FAIL_REGULAR_EXPRESSION`, because a `message(WARNING)` exits 0), so a KILLED
# one prints no `CMake Error` where a FAILING one does, and the two are already
# distinguishable. That is true and it is not sufficient -- the tell is an
# ABSENCE, and "the only tell is the absence of output nobody is looking for" is
# the sentence at the top of `run-check.sh`. What the `cmake -P` half has is a
# positive signal for the failing case; what it lacks is one for the killed case.
# Weaker than the `.sh` collapse, where the two are identical in every respect,
# and still the same shape.
#
# So this branch closes the total collapse and leaves the partial one open. That
# residual is a ticket, not a sentence here.
#
# ## Why this is a check and not a convention
#
# `run-check.sh` is what tells a check that was KILLED from one that FOUND A
# PROBLEM (#1079). A check registered without it reports the two identically, and
# nothing anywhere says so: the registration is valid CMake, the check is correct,
# and the only observable is a `***Failed` nobody can read. That is the state
# collapse the wrapper exists to remove, arriving through omission.
#
# ## The set is DERIVED, never listed
#
# From the ctest registrations themselves, because a hand-kept list is exact about
# the files it knows and silent about the ones it does not, and silence reads
# identically to complete coverage (#492). A script nobody registers is not in the
# set this ticket is about, so registration is the right source rather than a glob
# of `scripts/`.
#
# The rule is per LINE: a line naming `scripts/check-<name>.sh` must also name
# `scripts/run-check.sh`. That works because every registration here puts the
# script on its `COMMAND` line, and it is checked rather than assumed — a check
# whose path moved to a continuation line would show up as a violation rather than
# be skipped, which is the safe direction.
#
# **A COMMENT is not a call site**, so full-line comments are stripped first: two
# checks in this tree have matched their own headers and reported a step twice.
#
# `run-check.sh` itself does not match `check-<name>.sh` — after `check` comes a
# `.`, not a `-` — so the wrapper's own registration is not asked to wrap itself.
# That is a property of the two names, which is why the scan states its pattern
# rather than only its count: a pattern is broader than its author reads it as.
#
# ## bash 3.2
#
# macOS ships a 2007 `/bin/bash`. No `mapfile`, no `declare -A`, no `local -n`.
#
# Usage:  check-run-check-coverage.sh [--self-test]

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
interpreter="${BASH:-bash}"

# The pattern that decides the set. Stated once, and printed with every count, so
# a figure never travels without the pattern that produced it.
CheckPattern='scripts/check-[a-z0-9-]*\.sh'
WrapperName='scripts/run-check.sh'

# Report which registered checks do not go through the wrapper.
#
# @param 1 The CMakeLists to read.
# @return 0 when every registered check is wrapped, 1 otherwise; prints its
#         findings and always prints what it scanned.
Audit() {
    local list="$1"
    if [ ! -f "$list" ]; then
        echo "FAIL: no such registration file: ${list}" >&2
        return 1
    fi

    # Full-line comments stripped: a comment is not a call site.
    local body
    body="$(grep -v '^[[:space:]]*#' "$list")"

    local naming
    naming="$(printf '%s\n' "$body" | grep -E "$CheckPattern")"

    local total=0
    if [ -n "$naming" ]; then
        total="$(printf '%s\n' "$naming" | grep -c .)"
    fi

    echo "run-check-coverage: pattern ${CheckPattern} matched ${total} line(s) in ${list##*/}"

    # Two empty lists agree perfectly. A derivation that matched nothing has not
    # found every check wrapped; it has found nothing at all, and that is the one
    # failure a coverage check must never report as success.
    if [ "$total" -lt 1 ]; then
        echo "FAIL: the derivation matched no registered check at all, so 'all wrapped' would be vacuous" >&2
        return 1
    fi

    local unwrapped
    unwrapped="$(printf '%s\n' "$naming" | grep -vF "$WrapperName")"

    local bad=0
    if [ -n "$unwrapped" ]; then
        bad="$(printf '%s\n' "$unwrapped" | grep -c .)"
    fi

    if [ "$bad" -gt 0 ]; then
        echo "FAIL: ${bad} registration(s) invoke a check without ${WrapperName}:" >&2
        printf '%s\n' "$unwrapped" | sed 's/^[[:space:]]*/    /' >&2
        echo "  A check registered directly reports a KILLED run and a FAILING one identically (#1079)." >&2
        return 1
    fi

    echo "run-check-coverage: all ${total} invocation(s) matching ${CheckPattern} go through ${WrapperName}"

    # What this check does NOT reach, derived from the same file and printed
    # beside the pass. A count nobody can derive from the thing it counts is a
    # second source of truth (#780); a scope stated only in a header is one that
    # goes stale silently. Printed on the SUCCESS path deliberately -- an
    # uncovered set is not a failure, and a reader who only ever sees this line
    # when something breaks learns nothing about the boundary.
    local outsideCMake outsideScripts
    outsideCMake="$(printf '%s\n' "$body" | grep -cE '\-P .*scripts/[a-z0-9./-]*\.cmake')" || outsideCMake=0
    outsideScripts="$(printf '%s\n' "$body" \
        | grep -oE 'scripts/[a-z0-9./-]*\.sh' \
        | grep -v 'scripts/check-' \
        | grep -vF "$WrapperName" \
        | sort -u | grep -c .)" || outsideScripts=0
    echo "run-check-coverage: outside this check: ${outsideCMake} 'cmake -P' invocation(s), ${outsideScripts} registered script(s) not named check-*.sh"

    return 0
}

# Assert no marker `run-check.sh` prints can match a live `FAIL_REGULAR_EXPRESSION`.
#
# The wrapper writes its verdict on the same stream ctest scans, and the terminal
# marker contains the words `failed` and `did-not-conclude`. A registration whose
# fail-pattern matched one of those would turn EVERY wrapped check red — including
# the ones that passed — and it would read as a real regression across the whole
# hygiene set rather than as a pattern that grew.
#
# BOTH sides are derived: the markers out of `run-check.sh`, the patterns out of the
# registrations. Restating either would make this a second source of truth that
# agrees with nothing, and the whole point is that the pattern set GROWS.
#
# @param 1 The registration file to read patterns from.
# @param 2 The wrapper to read markers from.
# @return 0 when no marker matches any live pattern.
MarkersAreSafe() {
    local list="$1" wrapper="$2"
    [ -f "$list" ] || { echo "FAIL: no such registration file: ${list}" >&2; return 1; }
    [ -f "$wrapper" ] || { echo "FAIL: no such wrapper: ${wrapper}" >&2; return 1; }

    # The marker text, read from the wrapper's own assignments.
    local start terminal
    start="$(sed -n 's/^StartMarker="\(.*\)"$/\1/p' "$wrapper" | head -1)"
    terminal="$(sed -n 's/^TerminalMarker="\(.*\)"$/\1/p' "$wrapper" | head -1)"
    if [ -z "$start" ] || [ -z "$terminal" ]; then
        echo "FAIL: could not read the markers out of ${wrapper##*/}; this check cannot vouch for anything" >&2
        return 1
    fi

    # Every line the wrapper can emit, outcome words included -- those are the
    # substrings a fail-pattern is most likely to collide with.
    local lines outcome
    lines="$start"
    for outcome in passed skipped failed did-not-conclude; do
        lines="${lines}
${terminal}: ${outcome}"
    done

    # The patterns, taken as the VALUES those registrations actually use: a literal
    # pattern, or the variable's own definition when they name it.
    local patterns
    # Full-line comments stripped, for the reason `Audit` strips them: a
    # commented-out registration is not a live pattern, and refusing a correct tree
    # over one is this tree's oldest instrument bug.
    patterns="$(grep -v '^[[:space:]]*#' "$list" | sed -n 's/.*FAIL_REGULAR_EXPRESSION "\([^"]*\)".*/\1/p' | sort -u)"
    local resolved="" p name
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        # `FAIL_REGULAR_EXPRESSION "${SOME_VAR}"` is the spelling this file uses, so
        # the variable is followed to its `set(...)` rather than the literal `${…}`
        # being matched against the markers -- which would never match anything and
        # would make this check quietly vacuous.
        case "$p" in
            '${'*'}')
                name="${p#\$\{}"
                name="${name%\}}"
                p="$(grep -v '^[[:space:]]*#' "$list" | sed -n 's/^set('"${name}"' "\(.*\)")$/\1/p' | head -1)"
                # REFUSED, never dropped. A variable this cannot follow takes its
                # pattern out of the scan silently, and the patterns that remain then
                # report "no collision" for a set no longer containing the one that
                # mattered -- the vacuous pass the count guard below exists to refuse,
                # arriving one pattern at a time instead of all at once.
                if [ -z "$p" ]; then
                    echo "FAIL: a fail-pattern names \${${name}}, and no 'set(${name} \"...\")' in ${list##*/} resolves it" >&2
                    echo "  Dropping it would leave this check vouching for a pattern set it could not read." >&2
                    return 1
                fi
                ;;
        esac
        [ -n "$p" ] || continue
        resolved="${resolved}${p}
"
    done <<< "$patterns"

    local count=0
    if [ -n "$resolved" ]; then
        count="$(printf '%s' "$resolved" | grep -c .)"
    fi
    echo "run-check-coverage: ${count} distinct fail-pattern(s) resolved from ${list##*/}"
    if [ "$count" -lt 1 ]; then
        echo "FAIL: no fail-pattern resolved, so 'no marker collides' would be vacuous" >&2
        return 1
    fi

    local bad=0
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        # A HERESTRING, not a pipe: `grep -q` exits at its first match, the producer
        # takes SIGPIPE, and `pipefail` then reports the PRODUCER's status -- a false
        # negative on the SUCCESS path (#970). Which is to say: a collision would go
        # unreported precisely when there IS one. The scan in `check-e2e-helpers.sh`
        # caught this here, in a file written for a ticket about instruments that
        # misreport, by someone who had read that rule the same evening.
        if grep -Eq -- "$p" <<< "$lines"; then
            echo "FAIL: a marker run-check.sh prints matches the live fail-pattern '${p}'" >&2
            echo "  Every wrapped check would go red, passing ones included, and it would read as a regression." >&2
            bad=1
        fi
    done <<< "$resolved"

    [ "$bad" -eq 0 ] || return 1
    echo "run-check-coverage: no marker collides with any live fail-pattern"
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT
    selfTestStatus=0
    selfTestCases=0
    me="${repo_root}/scripts/$(basename "${BASH_SOURCE[0]}")"

    # Drive the audit against a synthesised registration file.
    # @param 1 What is being checked.
    # @param 2 want-pass or want-fail.
    # @param 3 The file contents.
    Case() {
        local what="$1" want="$2" body="$3" got=0 out=""
        selfTestCases=$((selfTestCases + 1))
        printf '%s\n' "$body" > "${scratch}/CMakeLists.txt"
        out="$("$interpreter" "$me" --audit "${scratch}/CMakeLists.txt" 2>&1)" || got=$?
        if { [ "$want" = "want-pass" ] && [ "$got" -eq 0 ]; } || { [ "$want" = "want-fail" ] && [ "$got" -ne 0 ]; }; then
            echo "  ok    (${want}) ${what}"
        else
            echo "  FAIL  (${want}, exit ${got}) ${what}" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            selfTestStatus=1
        fi
    }

    Case "a wrapped registration passes" want-pass \
'add_test(NAME "foo" COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" "${CMAKE_SOURCE_DIR}/scripts/check-foo.sh")'

    # The arm this whole file exists for.
    Case "an UNWRAPPED registration is refused" want-fail \
'add_test(NAME "foo" COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/check-foo.sh")'

    Case "one unwrapped among several wrapped is refused" want-fail \
'add_test(NAME "a" COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" "${CMAKE_SOURCE_DIR}/scripts/check-a.sh")
add_test(NAME "b" COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/check-b.sh")'

    # A file naming no check at all is a derivation that found nothing, which must
    # refuse rather than report every check wrapped.
    Case "a registration file naming no check is REFUSED, not passed vacuously" want-fail \
'add_test(NAME "unrelated" COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/something-else.sh")'

    # A comment is not a call site.
    Case "an unwrapped check inside a COMMENT does not refuse a good file" want-pass \
'# COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/check-old.sh"
add_test(NAME "foo" COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" "${CMAKE_SOURCE_DIR}/scripts/check-foo.sh")'

    # And the wrapper is not asked to wrap itself.
    Case "the wrapper's own self-test registration is not a violation" want-pass \
'add_test(NAME "run-check-selftest" COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" --self-test)
add_test(NAME "foo" COMMAND ${FASTCACHED_BASH} "${CMAKE_SOURCE_DIR}/scripts/run-check.sh" "${CMAKE_SOURCE_DIR}/scripts/check-foo.sh")'

    # --- the marker-safety half ---------------------------------------------
    #
    # Driven against synthesised registration files, and BOTH directions matter
    # more than usual here: a resolution that silently yielded the literal
    # `${SOME_VAR}` would match no marker and report "no collision" forever, which
    # is the vacuous pass this whole file exists to refuse.
    #
    # @param 1 What is being checked.
    # @param 2 want-pass or want-fail.
    # @param 3 The registration file contents.
    MarkerCase() {
        local what="$1" want="$2" body="$3" got=0 out=""
        selfTestCases=$((selfTestCases + 1))
        printf '%s\n' "$body" > "${scratch}/markers.txt"
        out="$("$interpreter" "$me" --audit-markers "${scratch}/markers.txt" "${repo_root}/scripts/run-check.sh" 2>&1)" || got=$?
        if { [ "$want" = "want-pass" ] && [ "$got" -eq 0 ]; } || { [ "$want" = "want-fail" ] && [ "$got" -ne 0 ]; }; then
            echo "  ok    (${want}) ${what}"
        else
            echo "  FAIL  (${want}, exit ${got}) ${what}" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            selfTestStatus=1
        fi
    }

    MarkerCase "a pattern that cannot match a marker is fine" want-pass \
'set_tests_properties("x" PROPERTIES FAIL_REGULAR_EXPRESSION "CMake Error|CMake Warning")'

    # The hazard itself: a pattern matching the word the terminal marker carries.
    MarkerCase "a pattern matching the terminal marker is REFUSED" want-fail \
'set_tests_properties("x" PROPERTIES FAIL_REGULAR_EXPRESSION "did-not-conclude")'

    MarkerCase "a pattern matching 'failed' is REFUSED" want-fail \
'set_tests_properties("x" PROPERTIES FAIL_REGULAR_EXPRESSION "CMake Error|failed")'

    # Proves the variable is FOLLOWED rather than matched as the literal `${…}`.
    # Without resolution this passes, and so would every real collision behind a
    # variable -- which is how this check would go quietly vacuous.
    MarkerCase "a colliding pattern behind a VARIABLE is still refused" want-fail \
'set(FASTCACHED_SCRIPT_CHECK_FAILED "CMake Error|CHECK CONCLUDED")
set_tests_properties("x" PROPERTIES FAIL_REGULAR_EXPRESSION "${FASTCACHED_SCRIPT_CHECK_FAILED}")'

    MarkerCase "a registration file with NO fail-pattern is REFUSED, not passed vacuously" want-fail \
'add_test(NAME "x" COMMAND true)'

    # A commented-out registration is not a live pattern. Without the comment strip
    # this refuses a correct tree over a pattern nothing enforces -- the mirror of
    # the audit half's own rule, met on the other side of the file.
    MarkerCase "a colliding pattern inside a COMMENT does not refuse a good file" want-pass \
'# set_tests_properties("old" PROPERTIES FAIL_REGULAR_EXPRESSION "did-not-conclude")
set_tests_properties("x" PROPERTIES FAIL_REGULAR_EXPRESSION "CMake Error")'

    # And a variable that resolves to nothing is REFUSED rather than dropped. The
    # other pattern here IS resolvable, so the count guard stays satisfied and
    # cannot answer this case for it -- without the refusal the unreadable one
    # simply vanishes and the run reports 'no collision' over a set it could not
    # read.
    MarkerCase "a fail-pattern naming an UNRESOLVABLE variable is REFUSED, not dropped" want-fail \
'set(FASTCACHED_KNOWN "CMake Error")
set_tests_properties("a" PROPERTIES FAIL_REGULAR_EXPRESSION "${FASTCACHED_KNOWN}")
set_tests_properties("b" PROPERTIES FAIL_REGULAR_EXPRESSION "${FASTCACHED_ELSEWHERE}")'

    # An argument this does not know is REFUSED rather than ignored. Ignored, the
    # fall-through audits `src/tests/CMakeLists.txt` and the run reads as an answer
    # about whatever was passed -- which is how this case came to exist rather than
    # a hazard somebody imagined.
    #
    # `case`, never `printf | grep -q`: `grep -q` exits at its first match, the
    # producer dies of SIGPIPE and `pipefail` reports the PRODUCER -- a false
    # answer on the SUCCESS path, which this file has already been caught by once.
    selfTestCases=$((selfTestCases + 1))
    bogusStatus=0
    bogusOut="$("$interpreter" "$me" --registrations "${scratch}/CMakeLists.txt" 2>&1)" || bogusStatus=$?
    bogusOk=0
    case "$bogusOut" in *"unrecognised argument"*) bogusOk=1 ;; esac
    if [ "$bogusStatus" -ne 0 ] && [ "$bogusOk" -eq 1 ]; then
        echo "  ok    (want-fail) an unrecognised argument is REFUSED, not silently ignored"
    else
        echo "  FAIL  (want-fail, exit ${bogusStatus}) an unrecognised argument must be refused by name" >&2
        printf '%s\n' "$bogusOut" | sed 's/^/        /' >&2
        selfTestStatus=1
    fi

    echo "check-run-check-coverage: self-test ran ${selfTestCases} case(s)"
    if [ "$selfTestStatus" -ne 0 ]; then
        echo "check-run-check-coverage: self-test FAILED" >&2
        exit 1
    fi
    echo "check-run-check-coverage: self-test passed"
    exit 0
fi

# `--audit <file>` is how the self-test drives the rule against a synthesised
# tree, so there is one implementation rather than two that can drift.
if [ "${1:-}" = "--audit" ]; then
    Audit "${2:-}"
    exit $?
fi

if [ "${1:-}" = "--audit-markers" ]; then
    MarkersAreSafe "${2:-}" "${3:-${repo_root}/scripts/run-check.sh}"
    exit $?
fi

# An UNRECOGNISED argument is REFUSED, never ignored -- and this one is not
# hypothetical. Ignored, the fall-through below audits `src/tests/CMakeLists.txt`
# whatever it was asked about, so the run answers a question nobody put. Measured
# while writing this branch: `--registrations <master's file>`, a flag that does
# not exist, reported `all 44 invocation(s) ... go through scripts/run-check.sh`
# for a file it had never opened -- confident, well-formed, and about the wrong
# subject. Asked correctly it refuses 42 registrations by name. That is this
# ticket's own thesis reaching its own instrument, which is where these land:
# the author is thinking about the SUBJECT's states, not the TOOL's.
if [ "$#" -gt 0 ]; then
    echo "FAIL: unrecognised argument '$1'" >&2
    echo "  usage: ${0##*/} [--self-test | --audit <file> | --audit-markers <file> [wrapper]]" >&2
    echo "  Refused rather than ignored: ignoring it reports on src/tests/CMakeLists.txt" >&2
    echo "  while appearing to answer about '$1'." >&2
    exit 2
fi

status=0
Audit "${repo_root}/src/tests/CMakeLists.txt" || status=1
MarkersAreSafe "${repo_root}/src/tests/CMakeLists.txt" "${repo_root}/scripts/run-check.sh" || status=1
exit "$status"
