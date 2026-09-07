#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Decide which jobs of a finished `merge_group` run nobody was told about.
#
# Usage:
#   scripts/ci-merge-group-report.sh <event> <runUrl> <jobsTsv> [headBranch]
#
# `<jobsTsv>` is one job per line, `name<TAB>status<TAB>conclusion<TAB>url`,
# which is what `gh api .../actions/runs/<id>/jobs --jq` produces. Prints one
# reportable failure per line on stdout as `context<TAB>conclusion<TAB>url`;
# narrates on stderr; exits 0 when it DECIDED and 1 when it could not.
#
# ## The failure this exists for, measured rather than argued
#
# A merge group whose only failing job is not one of the REQUIRED contexts is
# invisible to every surface anybody looks at. The pull request page is green
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
# ## A push to master is the SECOND door, and it needed no new trigger
#
# [#774](https://github.com/LASTRADA-Software/fastcached/issues/774): a job failing
# on a push to master reaches nobody by the same mechanism. Master `dd4633b2` had
# `Package (Linux .deb/.rpm)` and `Package (macOS .pkg)` red and it was found by a
# human looking.
#
# The `workflow_run` trigger already fires for those runs -- it is unfiltered, and
# this script has always narrated "event is 'push', not merge_group" for them -- so
# the change is in the DECISION and nowhere else. Three questions #774 leaves open,
# answered here rather than in a pull request body:
#
#   * **A second trigger?** No trigger was needed. What differs is the RULE, which
#     is a row of `EventPolicy` below.
#   * **Does the release gate change instead?** No. `check-release-gate` already
#     asserts every job in `build.yml` appears in `release.needs`, so a red
#     packaging job cannot ship a release -- it would fail again on the tag. What
#     was missing is not a gate, it is being TOLD, weeks before the tag.
#   * **And `pull_request`?** Out of scope, deliberately and permanently. Such a
#     failure is on the pull request page, which somebody is looking at; a report
#     per red pull-request job is noise, and noise is how a notifier gets ignored.
#     It is a row here saying `none` so the decision is visible rather than absent.
#
# **On master every failure is reportable, where in a queue only the unrequired
# ones are.** In a queue the required contexts eject the pull request and name the
# check, so reporting them again is duplicate noise. A push is gated by nothing at
# all, so that distinction has no meaning there -- and a wholly broken master
# opening several reports once is proportionate, because master is broken.
#
# The comment storm #774 fears is answered at the OTHER end: the workflow passes
# `FASTCACHED_REPORT_ONLY_IF_NEW` for a push, so a context that goes on failing on
# every subsequent push updates nothing. A push report is a TRANSITION -- this
# context started failing -- and the second failure is not a new fact.
#
# ## What it must not do
#
# It must not fail the merge group. It runs from a separate workflow on
# `workflow_run`, after the queue has already concluded, so it has no way to --
# and it opens an issue rather than returning a status anybody waits on. The same
# is true of a push: nothing waits on this.
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
# Which failures of a run are unreported, per triggering event. One row per event
# this reporter has an opinion about; anything else is answered "not applicable"
# out loud, because "nothing to report" and "this is not my question" are two
# states and a notifier that renders them identically is the defect it exists for.
#
#   unrequired -- report only contexts the `default-master` ruleset does not
#                 require. The required ones ejected the pull request and named
#                 the check, so a second report is duplicate noise.
#   all        -- report every failure. Nothing gates a push, so there is no
#                 already-surfaced set to subtract.
#   none       -- report nothing, and say why. `pull_request` is here rather than
#                 absent so the decision is visible: such a failure is on the
#                 pull request page, and a report per red job would be noise.
EventPolicy=(
    "merge_group|unrequired|the queue reports success and the pull request merges, so an unrequired failure reaches nobody"
    "push|all|nothing gates a push, so no failure here is surfaced by anything a human is shown"
    "pull_request|none|the failure is on the pull request page, where somebody is already looking"
)

PolicyFor() {
    local event="$1" row
    for row in "${EventPolicy[@]}"; do
        [[ "${row%%|*}" == "$event" ]] && { row="${row#*|}"; echo "${row%%|*}"; return 0; }
    done
    return 1
}

ReasonFor() {
    local event="$1" row
    for row in "${EventPolicy[@]}"; do
        [[ "${row%%|*}" == "$event" ]] && { echo "${row##*|}"; return 0; }
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

[[ $# -eq 3 || $# -eq 4 ]] \
    || Fatal "usage: $(basename "${BASH_SOURCE[0]}") <event> <runUrl> <jobsTsv> [headBranch]"
event="$1"
runUrl="$2"
jobsTsv="$3"
headBranch="${4:-}"

# An event with no row is not a defect and not a silence -- it is a question this
# reporter does not answer. Said out loud rather than left as an empty result,
# because "nothing to report" and "not applicable" are two states.
if ! policy="$(PolicyFor "$event")"; then
    Say "event is '$event', which is not in this script's event table; it answers nothing about it"
    exit 0
fi
if [[ "$policy" == "none" ]]; then
    Say "event is '$event': $(ReasonFor "$event") -- deliberately reporting nothing"
    exit 0
fi

# A push to a branch that is not the default one is somebody's own branch, and its
# red is theirs to see. `master` is named rather than inferred from the run,
# because the workflow's own `push:` filter is `[master, fix-ci]` and `fix-ci` is a
# scratch branch for exactly the CI experiments that are expected to fail.
#
# Refused rather than skipped when the branch was not passed: a push policy that
# cannot tell WHICH branch would report every scratch push, and inferring "master"
# from an absent value is the guess this file exists to refuse.
if [[ "$event" == "push" ]]; then
    [[ -n "$headBranch" ]] \
        || Fatal "a push run was passed no head branch, so this cannot tell master from a scratch branch; refusing to guess"
    if [[ "$headBranch" != "master" ]]; then
        Say "push to '$headBranch' rather than master; a scratch branch's red belongs to whoever pushed it"
        exit 0
    fi
fi

Say "event is '$event', policy '$policy': $(ReasonFor "$event")"

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
            # Under the `all` policy the required/unrequired split has no meaning:
            # nothing gates a push, so there is no already-surfaced set to subtract.
            # Counted anyway, so the narration still says how many of the failures
            # were contexts a queue WOULD have held on.
            if IsRequired "$name"; then
                badRequired=$((badRequired + 1))
                if [[ "$policy" == "unrequired" ]]; then
                    Say "'$name' concluded $conclusion and IS a required context: the queue ejected the pull request and named the check, so it is already reported"
                    continue
                fi
            fi
            reportable="${reportable}${name}"$'\t'"${conclusion}"$'\t'"${url}"$'\n'
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
    if [[ "$policy" == "unrequired" ]]; then
        Say "nothing to report: $good job(s) succeeded and every failure was a required context the queue already surfaced"
    else
        Say "nothing to report: $good job(s) succeeded and nothing failed"
    fi
    exit 0
fi

printf '%s' "$reportable"
exit 0
