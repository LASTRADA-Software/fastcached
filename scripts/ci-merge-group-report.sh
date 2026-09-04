#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Decide which jobs of a finished `merge_group` run nobody was told about.
#
# Usage:
#   scripts/ci-merge-group-report.sh <event> <runUrl> <jobsTsv>
#
# `<jobsTsv>` is one job per line, `name<TAB>status<TAB>conclusion<TAB>url`,
# which is what `gh api .../actions/runs/<id>/jobs --jq` produces. Prints one
# reportable failure per line on stdout as `context<TAB>conclusion<TAB>url`;
# narrates on stderr; exits 0 when it DECIDED and 1 when it could not.
#
# ## The failure this exists for, measured rather than argued
#
# A merge group whose only failing job is not one of the eleven REQUIRED contexts
# is invisible to every surface anybody looks at. The pull request page is green
# (its own run passed on the head), the queue reports success (every required
# context passed), master is green afterwards (the same job passes on the merge
# commit), and the only trace is an `event=merge_group` run that no part of the
# normal flow ever lists.
#
# Measured on 2026-09-04, over every failing `merge_group` `Build` run the API
# still held -- six of them:
#
#   | run         | failing job          | required? | pull request                |
#   |-------------|----------------------|-----------|-----------------------------|
#   | 33783939363 | Code coverage        | no        | #689 merged                 |
#   | 33782559943 | macOS-clang-release  | YES       | #686 ejected, fixed, merged |
#   | 33760836218 | Windows-cl-debug     | no        | #667 merged                 |
#   | 33749317968 | Package (macOS .pkg) | no        | #669 merged                 |
#   | 33665118078 | Package (macOS .pkg) | no        | #546 merged                 |
#   | 33650121718 | Windows-cl-debug     | no        | #539 merged                 |
#
# Five of six, four distinct jobs, five pull requests merged with nobody told.
# The one required failure behaved correctly -- ejected, fixed, re-queued -- and
# that contrast is what makes the other five a REPORTING gap rather than a gating
# one. Nothing here proposes changing which contexts are required; #684 is
# explicit that the packaging jobs are unrequired for good reasons.
#
# `Windows-cl-debug` is why this classifies by CONTEXT and never by job key: it
# is a leg of the same matrix job as `Windows-cl-release`, which IS required. The
# unit that is required or not is the expanded name, so that is the unit here.
#
# ## What it must not do
#
# It must not fail the merge group. It runs from a separate workflow on
# `workflow_run`, after the queue has already concluded, so it has no way to --
# and it opens an issue rather than returning a status anybody waits on.
#
# ## Skipped, absent, unstarted and failed are four states
#
# A notifier that says "no failures" because the run never started is #684 one
# level further out, and this repository has now made that mistake in four
# separate instruments. So every conclusion is looked up in a TABLE and an
# unenumerated one is a hard failure rather than whichever bucket a negation
# happens to catch -- `gh` renders a RUNNING job's conclusion as the empty
# string, not `null`, and a `!= null` test once classified every in-progress job
# here as FAILED (`.agent/rules/build-and-toolchain.md`). And "nothing to report"
# is asserted POSITIVELY: a record in which nothing failed and nothing SUCCEEDED
# is not a clean run, it is a run that did not happen, so it is refused.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

Fatal() { echo "ci-merge-group-report: $*" >&2; exit 1; }
Say()   { echo "ci-merge-group-report: $*" >&2; }

# ---------------------------------------------------------------------------
# Every conclusion GitHub can put on a job, and what this reporter does with it.
# One row per value; anything not here stops the run rather than being guessed
# at, because "we could not tell" must never read as "nothing was wrong".
#
#   bad    -- a failure a human has to be told about
#   good   -- evidence the run actually did something
#   inert  -- neither; counted and named so it cannot be mistaken for either
ConclusionClasses=(
    "success|good"
    "neutral|good"
    "skipped|inert"
    "cancelled|inert"
    "stale|inert"
    "failure|bad"
    "timed_out|bad"
    "action_required|bad"
)

ClassOf() {
    local conclusion="$1" row
    for row in "${ConclusionClasses[@]}"; do
        [[ "${row%%|*}" == "$conclusion" ]] && { echo "${row##*|}"; return 0; }
    done
    return 1
}

# ---------------------------------------------------------------------------
# The required-context list is READ from `scripts/check-merge-queue-contexts.sh`
# rather than restated. A second copy of that table is not a cross-check, it is a
# second thing to be wrong -- and here it is the copy that decides whether a
# failure gets reported, so a drift between the two would make this reporter
# silent about exactly the jobs it exists for. That script's header carries the
# provenance of the list and the `gh api` call that reads the live one.
#
# Overridable only so the selftest can stage a table; production passes nothing.
RequiredContextsFile="${FASTCACHED_REQUIRED_CONTEXTS_FILE:-scripts/check-merge-queue-contexts.sh}"

ReadRequiredContexts() {
    [[ -f "$RequiredContextsFile" ]] \
        || Fatal "$RequiredContextsFile does not exist; the required-context list has no source"
    awk '
        /^RequiredContexts=\(/ { inTable = 1; next }
        inTable && /^\)/       { inTable = 0 }
        inTable                {
            line = $0
            sub(/^[ \t]*"/, "", line)
            sub(/"[ \t]*$/, "", line)
            sub(/\|.*$/, "", line)
            if (length(line)) print line
        }
    ' "$RequiredContextsFile"
}

# ---------------------------------------------------------------------------

[[ $# -eq 3 ]] || Fatal "usage: $(basename "${BASH_SOURCE[0]}") <event> <runUrl> <jobsTsv>"
event="$1"
runUrl="$2"
jobsTsv="$3"

# A run that is not a merge-group run is not a defect and not a silence -- it is
# a question this reporter does not answer. Said out loud rather than left as an
# empty result, because "nothing to report" and "not applicable" are two states.
if [[ "$event" != "merge_group" ]]; then
    Say "event is '$event', not merge_group; this reporter answers nothing about it"
    exit 0
fi

[[ -f "$jobsTsv" ]] || Fatal "no job record at '$jobsTsv': a listing that could not be taken is not a listing of nothing"
[[ -s "$jobsTsv" ]] || Fatal "the job record at '$jobsTsv' is EMPTY; zero rows is the absence of a verdict, not a verdict"

# A read loop rather than `mapfile`, and process substitution rather than a
# pipeline: `mapfile` is bash 4 and macOS ships 3.2, and a pipeline here would
# reintroduce the `pipefail` trap this tree keeps records about. Both constraints
# apply because the selftest driving this script is in the default ctest set.
required=()
while IFS= read -r line; do
    required+=("$line")
done < <(ReadRequiredContexts)
[[ ${#required[@]} -gt 0 ]] \
    || Fatal "read 0 required contexts out of $RequiredContextsFile; with an empty list EVERY failure would look unrequired and this reporter would be loud about the ones the queue already surfaced"
Say "read ${#required[@]} required context(s) from $RequiredContextsFile"

IsRequired() {
    local want="$1" context
    for context in "${required[@]}"; do
        [[ "$context" == "$want" ]] && return 0
    done
    return 1
}

good=0
inert=0
bad=0
badRequired=0
rows=0
reportable=""

# Read whole lines and split on tabs BY HAND. `IFS=$'\t' read -r a b c d` looks
# like the obvious spelling and is wrong here: tab is one of bash's three IFS
# WHITESPACE characters, so a run of tabs collapses to one delimiter and leading
# ones are dropped. A record with an empty field therefore silently SHIFTS every
# field after it -- and the empty field is exactly the case that matters, since
# `gh` renders an unfinished job's conclusion as the empty string. Caught by the
# self-test case for a row with no job name, which could not fire at all: the
# empty name vanished and the URL was read as the conclusion.
tabsOnly() { local t="${1//[!$'\t']/}"; echo "${#t}"; }

while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "$line" ]] && continue
    rows=$((rows + 1))

    count="$(tabsOnly "$line")"
    [[ "$count" -eq 3 ]] \
        || Fatal "row $rows has $count tab(s) where the record shape is name<TAB>status<TAB>conclusion<TAB>url; refusing to read fields out of a line this does not recognise"

    name="${line%%$'\t'*}"
    rest="${line#*$'\t'}"
    status="${rest%%$'\t'*}"
    rest="${rest#*$'\t'}"
    conclusion="${rest%%$'\t'*}"
    url="${rest#*$'\t'}"

    [[ -n "$name" ]] || Fatal "row $rows has no job name; the record is not the shape this reads"
    [[ -n "$status" ]] || Fatal "row $rows ('$name') has no status; the record is not the shape this reads"

    # A job that has not finished is not a job that concluded nothing. `gh`
    # renders its conclusion as the EMPTY STRING, which a negation sorts into
    # whichever bucket it happens to catch -- once into FAILED, on a
    # byte-identical tree, which is how this repository learnt to enumerate.
    if [[ "$status" != "completed" ]]; then
        Fatal "job '$name' is still '$status' in a run reported as completed; this reporter cannot say what happened and will not guess"
    fi

    if ! class="$(ClassOf "$conclusion")"; then
        Fatal "job '$name' concluded '${conclusion:-<empty>}', which is not in this script's conclusion table; add a row rather than letting an unknown value fall into a bucket"
    fi

    case "$class" in
        good)  good=$((good + 1)) ;;
        inert) inert=$((inert + 1)) ;;
        bad)
            bad=$((bad + 1))
            if IsRequired "$name"; then
                badRequired=$((badRequired + 1))
                Say "'$name' concluded $conclusion and IS a required context: the queue ejected the pull request and named the check, so it is already reported"
            else
                reportable="${reportable}${name}"$'\t'"${conclusion}"$'\t'"${url}"$'\n'
            fi
            ;;
    esac
done < "$jobsTsv"

[[ "$rows" -gt 0 ]] || Fatal "the job record parsed to 0 rows; zero rows is the absence of a verdict, not a verdict"

reportableCount=0
if [[ -n "$reportable" ]]; then
    reportableCount="$(printf '%s' "$reportable" | grep -c '' || true)"
fi

Say "$rows job(s) in $runUrl: good=$good inert=$inert failing=$bad (of which required=$badRequired, unrequired-and-unreported=$reportableCount)"

# Absence of the negative is not the positive. A record in which nothing failed
# is only good news if something SUCCEEDED; a run where every job was skipped or
# cancelled reports no failures for the same reason a run that never started
# does, and reading that as "all clear" is the four-states mistake this whole
# file is about.
if [[ "$bad" -eq 0 && "$good" -eq 0 ]]; then
    Fatal "no job failed and no job succeeded either ($rows row(s), inert=$inert); that is not a clean run, it is a run that did nothing"
fi

if [[ "$reportableCount" -eq 0 ]]; then
    Say "nothing to report: $good job(s) succeeded and every failure was a required context the queue already surfaced"
    exit 0
fi

printf '%s' "$reportable"
exit 0
