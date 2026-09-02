#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The `clang-tidy` sweep still sweeps EVERYTHING somewhere, and that somewhere
# still reaches a person.
#
# ## The property, and why nothing else holds it
#
# Since #554 the merge queue diff-scopes its sweep, exactly as a pull request
# does. What makes that sound is an argument with two halves, both of which live
# in `.agent/rules/build-and-toolchain.md` and neither of which is visible in a
# run:
#
#   * master@N was proved fully clean by the `push` that landed it, so a scope
#     that over-approximates everything the next change can reach covers
#     master@N+1; and
#   * that `push` is the only place the full sweep still happens, so a finding
#     outside a change's include closure is seen there and nowhere else.
#
# Take the `--all` off that push and every run in this repository stays green
# while the analysis is permanently narrower than the rulebook says it is. There
# is no failing check, no slower job and no log line -- the sweep prints a
# confident count of a smaller number of translation units, which is the exact
# failure `scripts/tidy-sweep.sh` exists to make impossible one level down.
#
# The second half is the same shape one step further out. A master push blocks
# nothing and notifies nobody -- 18 of 18 failed master-push runs were never once
# re-run, measured and recorded in the rulebook -- so the report step is what
# turns that run into a signal. Delete it and the full sweep still runs, still
# fails, and still reaches no one. A check nobody reads is a check that does not
# exist.
#
# ## What is asserted rather than demonstrated
#
# This proves the SHAPE of the workflow, not the behaviour of a run: neither a
# master push nor a queue entry can be staged here. The behaviours it rests on
# are measurements, recorded in the rulebook rather than re-derived. What it can
# do is make the wiring impossible to remove silently, which is the half with no
# other guard -- `tidy-sweep.sh --self-test` covers the scope COMPUTATION, and
# nothing at all covered which events reach it with which scope.
#
# ## bash 3.2
#
# It is in the default ctest set and macOS ships a 2007 `/bin/bash`. No
# `mapfile`, no `declare -A`, no `${var^^}`.
#
# Usage:  scripts/check-tidy-sweep-scope.sh [--self-test]
#
#   --self-test  drive every rule against synthetic workflows, in both
#                directions, and exit. Needs nothing but bash and awk.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

Workflow=".github/workflows/build.yml"

# ---------------------------------------------------------------------------
# Find the sweep in a workflow: the step that invokes `scripts/tidy-sweep.sh` as
# the sweep rather than as its own self-test. Emits one
# `<id>\t<run: block, joined>\t<BASE value>` row per such step.
#
# By what it RUNS, never by its name -- a step found by name is a step a rename
# turns into "no step matched", which every rule below would then pass over
# silently. Comment lines are dropped first: build.yml explains itself at length
# and much of what it says names the script, so a scan that reads comments finds
# three sweeps and can vouch for none of them.
#
# @param 1 The workflow file.
FindSweep() {
    awk '
        function strip(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
        # A step begins at `      - `; more deeply indented lines belong to it.
        function flush() {
            if (runsSweep && !selfTest) print id "\t" run "\t" base
            id = ""; run = ""; base = ""; runsSweep = 0; selfTest = 0; inRun = 0
        }
        /^[ \t]*#/                { next }
        /^      - /               { flush(); inRun = 0 }
        # Any step-level key closes a run: block that was open.
        /^        [A-Za-z_-]+:/   { inRun = 0 }
        /^        id:/            { id = strip(substr($0, index($0, ":") + 1)) }
        /^          BASE:/        { base = strip(substr($0, index($0, ":") + 1)) }
        /^        run:/           { inRun = 1
                                    rest = strip(substr($0, index($0, ":") + 1))
                                    # `run: >-` and `run: |` carry nothing here.
                                    if (rest != "" && rest !~ /^[>|]/) run = rest
                                    next }
        inRun                     { line = strip($0)
                                    if (line != "") run = (run == "" ? line : run " " line) }
        inRun && /tidy-sweep\.sh/ { runsSweep = 1 }
        inRun && /--self-test/    { selfTest = 1 }
        END                       { flush() }
    ' "$1"
}

# The `if:` of every step that reads a named step's conclusion AND names master.
# @param 1 The workflow file.
# @param 2 The sweep step's id.
FindReader() {
    awk -v id="$2" '
        function flush() {
            if (readsSweep && onMaster) print ifExpr
            ifExpr = ""; readsSweep = 0; onMaster = 0
        }
        /^[ \t]*#/     { next }
        /^      - /    { flush() }
        /^        if:/ { ifExpr = substr($0, index($0, ":") + 1)
                         if (index(ifExpr, "steps." id ".conclusion")) readsSweep = 1
                         if (index(ifExpr, "refs/heads/master")) onMaster = 1 }
        END            { flush() }
    ' "$1"
}

# The `permissions:` block of one job, comments dropped.
# @param 1 The workflow file.
# @param 2 The job key.
FindJobPermissions() {
    awk -v job="  $2:" '
        index($0, job) == 1                  { inJob = 1; next }
        inJob && /^  [A-Za-z0-9_-]+:[ \t]*$/ { inJob = 0 }
        inJob && /^    permissions:/         { inPerms = 1; next }
        inPerms && /^    [^ ]/               { inPerms = 0 }
        inPerms && !/^[ \t]*#/               { print }
    ' "$1"
}

# ---------------------------------------------------------------------------
# Every rule, over one workflow. Prints its findings; returns non-zero on any.
# @param 1 The workflow file.
CheckWorkflow() {
    local workflow="$1"
    local problems=0
    Fail() { echo "  FAIL: $*" >&2; problems=$((problems + 1)); }

    if [[ ! -f "$workflow" ]]; then
        echo "  FAIL: $workflow does not exist" >&2
        return 1
    fi

    local sweep sweepCount sweepId sweepRun sweepBase
    sweep="$(FindSweep "$workflow")"
    if [[ -z "$sweep" ]]; then
        Fail "no step in $workflow runs scripts/tidy-sweep.sh as a sweep; this project's only clang-tidy coverage would be gone and every run would stay green"
        return 1
    fi

    sweepCount="$(printf '%s\n' "$sweep" | wc -l | tr -d ' ')"
    [[ "$sweepCount" -eq 1 ]] \
        || Fail "$sweepCount steps run the sweep; this check reasons about one and cannot vouch for the rest"

    sweepId="$(printf '%s\n' "$sweep" | head -1 | cut -f1)"
    sweepRun="$(printf '%s\n' "$sweep" | head -1 | cut -f2)"
    sweepBase="$(printf '%s\n' "$sweep" | head -1 | cut -f3)"

    [[ -n "$sweepId" ]] \
        || Fail "the sweep step has no \`id:\`, so nothing downstream can read its conclusion apart from the whole job's -- which fires on every unrelated infrastructure failure in the job, and is the alarm people mute"

    # -----------------------------------------------------------------------
    # Rule A: the full sweep exists, and an event nobody thought of GETS it.
    #
    # `--all` on nothing is silent and permanent: no failing check, no slower
    # job, just a smaller confident count. `--all` on everything is the cost #554
    # removed and is at least loud.
    #
    # The third way is the one worth a check rather than a comment, and it is a
    # SPELLING. `github.event_name == 'push' && '--all'` and
    # `github.event_name != 'pull_request' && github.event_name != 'merge_group'
    # && '--all'` pick out the same events today and fall opposite ways tomorrow.
    # `on:` is contemplated growing -- build.yml says a nightly is *deliberately*
    # not a third trigger -- and under the inclusive spelling a `schedule:` added
    # later diff-scopes against `origin/master`, whose diff on master is EMPTY, so
    # `tidy-sweep.sh` prints `no source changed` and exits 0 having analysed
    # nothing. Under the exclusive spelling it sweeps everything: slow, never
    # wrong, and `ci-scope.sh`'s own principle that every way of not knowing
    # escalates. So the guard must EXCLUDE the events that have a base to diff
    # against, never INCLUDE the one that does not.
    if [[ "$sweepRun" != *"--all"* ]]; then
        Fail "the sweep never passes \`--all\`, so NO event sweeps every translation unit. The queue's diff-scoping rests on master having been proved fully clean; with no full sweep on the push there is nothing for it to rest on, and every run stays green -- run: $sweepRun"
    elif [[ "$sweepRun" != *"github.event_name"* ]]; then
        Fail "the sweep passes \`--all\` unconditionally; every pull request and every queue entry would sweep the whole tree again -- run: $sweepRun"
    elif [[ "$sweepRun" == *"github.event_name == "* ]]; then
        Fail "the \`--all\` guard NAMES the event that gets the full sweep. Exclude the diff-scoped ones instead (\`github.event_name != 'pull_request' && github.event_name != 'merge_group'\`): an inclusive guard diff-scopes every trigger added to \`on:\` later, and on master that diff is empty, so the sweep exits 0 having analysed nothing -- run: $sweepRun"
    else
        local missing=""
        case "$sweepRun" in
            *"github.event_name != 'pull_request'"*) ;;
            *) missing="pull_request" ;;
        esac
        case "$sweepRun" in
            *"github.event_name != 'merge_group'"*) ;;
            *) missing="${missing:+$missing and }merge_group" ;;
        esac
        if [[ -n "$missing" ]]; then
            Fail "the \`--all\` guard does not exclude ${missing}, so that event sweeps every translation unit and the saving #554 measured is not there -- run: $sweepRun"
        else
            echo "  ok: only \`pull_request\` and \`merge_group\` diff-scope; any other event sweeps everything"
        fi
    fi

    # -----------------------------------------------------------------------
    # Rule B: a merge-queue entry diffs against its OWN base.
    #
    # Losing this is not a hole -- `github.base_ref` is empty outside a pull
    # request, so the fallback is `origin/master`, which resolves and
    # over-approximates. It is a COST rule, asserted because the way it gets lost
    # is somebody simplifying an expression they read as redundant.
    if [[ "$sweepBase" != *"merge_group.base_sha"* ]]; then
        Fail "the sweep's BASE does not name \`github.event.merge_group.base_sha\`, so a queue entry diffs against something other than the entry it is stacked on -- BASE: ${sweepBase:-<none>}"
    else
        echo "  ok: a merge-queue entry diff-scopes against its own base_sha"
    fi

    # -----------------------------------------------------------------------
    # Rule C: the only full sweep left has a reader, keyed on the sweep STEP.
    #
    # Never on the job: over the 18 failed master-push runs in the rulebook's
    # sample the job failed 18 times and the sweep step itself failed none, so a
    # job-keyed alarm is a thing people mute by the second week.
    local reader
    reader="$(FindReader "$workflow" "$sweepId")"
    if [[ -z "$reader" ]]; then
        Fail "no step reads \`steps.${sweepId}.conclusion\` for a push to \`refs/heads/master\`. That push is the only full sweep left, it blocks nothing and notifies nobody -- 18 of 18 failed master-push runs were never once re-run -- so with no reader the check runs, fails, and reaches no one."
    else
        echo "  ok: the master push's full sweep has a reader keyed on the sweep step"
    fi

    # -----------------------------------------------------------------------
    # Rule D: and that reader can actually write.
    #
    # A job-level `permissions:` block is exhaustive -- every scope not named
    # drops to none -- so this fails in the direction where the block exists and
    # does not name the scope, which is what happens when somebody adds a scope
    # for a new step and rewrites the list.
    if [[ -n "$reader" ]]; then
        local perms
        perms="$(FindJobPermissions "$workflow" clang-tidy)"
        if [[ "$perms" != *"issues:"*"write"* ]]; then
            Fail "the clang-tidy job does not grant \`issues: write\`, so its report step would be refused by the API and the only reader on the only full sweep would be decorative -- permissions: ${perms:-<none>}"
        else
            echo "  ok: the clang-tidy job grants the reader \`issues: write\`"
        fi
    fi

    return "$problems"
}

# ---------------------------------------------------------------------------
# Self-test
#
# Each rule is driven in BOTH directions over a synthetic workflow: a check that
# has never been seen to fail is a check that has told you nothing, and one that
# only ever fails is a check that would fail on a correct tree too. The correct
# fragment below is what every break is made FROM, so no break can pass by being
# malformed in some second way.
SelfTest() {
    local status=0 scratch
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    # A minimal workflow with every property in place. Indentation matches
    # build.yml's, because that is what the extractors key on.
    Correct() {
        cat <<'FRAGMENT'
jobs:
  clang-tidy:
    name: "clang-tidy"
    permissions:
      contents: read
      issues: write
    steps:
      # A comment naming scripts/tidy-sweep.sh --all, which must not be read.
      - name: "Self-test the sweep's scope computation"
        run: scripts/tidy-sweep.sh --self-test
      - name: "Sweep"
        id: sweep
        env:
          BASE: ${{ github.event_name == 'merge_group' && github.event.merge_group.base_sha || format('origin/{0}', github.base_ref || 'master') }}
        run: >-
          scripts/tidy-sweep.sh
          ${{ (github.event_name != 'pull_request' && github.event_name != 'merge_group') && '--all' || '' }}
      - name: "Report a full sweep that only master saw"
        if: ${{ failure() && steps.sweep.conclusion == 'failure' && github.event_name == 'push' && github.ref == 'refs/heads/master' }}
        run: gh issue create
  other:
    steps:
      - run: true
FRAGMENT
    }

    # @param 1 What is being staged. @param 2 want-pass|want-fail.
    # @param 3 A sed program applied to the correct fragment ("" for none).
    Case() {
        local what="$1" want="$2" program="$3" file="${scratch}/wf.yml" got
        if [[ -z "$program" ]]; then Correct > "$file"; else Correct | sed "$program" > "$file"; fi
        if CheckWorkflow "$file" >/dev/null 2>&1; then got=pass; else got=fail; fi
        if [[ "$got" == "${want#want-}" ]]; then
            echo "  ok   ${what}"
        else
            echo "  FAIL ${what}: wanted ${want#want-}, got ${got}"
            status=1
        fi
    }

    echo "TIDY SWEEP SCOPE SELF-TEST"
    Case "the shipped shape passes"                want-pass ''
    Case "no --all anywhere"                       want-fail "s/ *\\\${{ (github.event_name.*//"
    Case "--all unconditionally"                   want-fail "s/\\\${{ (github.event_name.*}}/--all/"
    Case "the inclusive spelling (a new trigger would diff-scope)" \
                                                   want-fail "s/(github.event_name != 'pull_request' && github.event_name != 'merge_group')/github.event_name == 'push'/"
    Case "the queue no longer excluded"            want-fail "s/ && github.event_name != 'merge_group'//"
    Case "the pull request no longer excluded"     want-fail "s/github.event_name != 'pull_request' && //"
    Case "the queue's own base dropped"            want-fail "s/github.event_name == 'merge_group' && github.event.merge_group.base_sha || //"
    Case "the reader deleted"                      want-fail "/Report a full sweep/,+2d"
    Case "the reader no longer scoped to master"   want-fail "s|github.ref == 'refs/heads/master'|true|"
    Case "the reader keyed on the job, not the step" want-fail "s/steps.sweep.conclusion == 'failure' && //"
    Case "issues: write withdrawn"                 want-fail "/^      issues: write$/d"
    Case "the sweep step's id removed"             want-fail "/^        id: sweep$/d"
    Case "the sweep step gone entirely"            want-fail "/name: \"Sweep\"/,+6d"

    [[ "$status" -eq 0 ]] && echo "TIDY SWEEP SCOPE SELF-TEST PASSED"
    return "$status"
}

# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--self-test" ]]; then
    SelfTest
    exit $?
elif [[ $# -gt 0 ]]; then
    echo "usage: $0 [--self-test]" >&2
    exit 2
fi

echo "check-tidy-sweep-scope: ${Workflow}"
if ! CheckWorkflow "$Workflow"; then
    echo "check-tidy-sweep-scope: the tidy sweep's coverage would narrow silently" >&2
    exit 1
fi
echo "check-tidy-sweep-scope: a full sweep still happens on the master push, and it has a reader"
