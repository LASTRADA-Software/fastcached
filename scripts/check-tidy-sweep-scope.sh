#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# The `clang-tidy` sweep still sweeps EVERYTHING somewhere, and that somewhere
# still reaches a person.
#
# ## The property, and why nothing else holds it
#
# Since #554 a pull request and a merge-queue entry both diff-scope, and the
# `push` on master is the only full sweep left. What makes that sound is an
# argument with two halves, both of which live in
# `.agent/rules/build-and-toolchain.md` and neither of which is visible in a run:
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
# ## bash 3.2, and no `sed` in the self-test
#
# It is in the default ctest set and macOS ships a 2007 `/bin/bash`. No
# `mapfile`, no `declare -A`, no `${var^^}`.
#
# The self-test's broken workflows are GENERATED rather than produced by editing
# a correct one. Editing was the first shape and it was quietly wrong twice over:
# `sed` ranges like `/pat/,+2d` are a GNU extension that BSD sed rejects outright,
# and nothing checked sed's status -- so on macOS the edit failed, the fragment
# came out EMPTY, the check failed because there was no workflow at all, and the
# case printed `ok` for a break it had never staged. A generated fragment cannot
# fail that way, and every case additionally asserts its fragment DIFFERS from the
# correct one, so a knob that does nothing is a failure rather than a pass.
#
# Usage:  scripts/check-tidy-sweep-scope.sh [--self-test]
#
#   --self-test  drive every rule against generated workflows, in both
#                directions, and exit. Needs nothing but bash and awk.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

Workflow=".github/workflows/build.yml"

# ---------------------------------------------------------------------------
# Find the sweep in a workflow: the step that invokes `scripts/tidy-sweep.sh` as
# the sweep rather than as its own self-test. Emits one
# `<job key>\t<step id>\t<run: block, joined>\t<BASE value>` row per such step.
#
# By what it RUNS, never by its name -- a step found by name is a step a rename
# turns into "no step matched", which every rule below would then pass over
# silently. Comment lines are dropped first: build.yml explains itself at length
# and much of what it says names the script, so a scan that reads comments finds
# three sweeps and can vouch for none of them.
#
# The whole `run:` block is joined and only THEN tested for the two markers,
# rather than testing line by line as it is read. Line by line, a step whose
# command sits on the `run:` key line itself was invisible -- reflowing
# `run: >-` plus a continuation into the identical one-liner made this report
# that the sweep had been deleted, which is a red build accusing an author of
# something they did not do.
#
# @param 1 The workflow file.
FindSweep() {
    awk '
        function strip(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
        # A step begins at `      - `; more deeply indented lines belong to it.
        function flush() {
            if (run ~ /tidy-sweep\.sh/ && run !~ /--self-test/)
                print job "\t" id "\t" run "\t" base
            id = ""; run = ""; base = ""; inRun = 0
        }
        /^[ \t]*#/                { next }
        /^  [A-Za-z0-9_-]+:[ \t]*$/ { flush(); job = strip($0); sub(/:$/, "", job); next }
        /^      - /               { flush(); inRun = 0 }
        # Any step-level key closes a run: block that was open.
        /^        [A-Za-z_-]+:/   { inRun = 0 }
        /^        id:/            { id = strip(substr($0, index($0, ":") + 1)) }
        /^          BASE:/        { base = strip(substr($0, index($0, ":") + 1)) }
        /^        run:/           { inRun = 1
                                    rest = strip(substr($0, index($0, ":") + 1))
                                    # `run: >-` and `run: |` carry nothing here;
                                    # anything else is the command itself.
                                    if (rest != "" && rest !~ /^[>|]/) run = rest
                                    next }
        inRun                     { line = strip($0)
                                    if (line != "") run = (run == "" ? line : run " " line) }
        END                       { flush() }
    ' "$1"
}

# The `if:` of every step that reads a named step's conclusion AND names master.
#
# The `if:` is joined across continuation lines for the same reason the `run:` is:
# the shipped expression is over a hundred characters, wrapping it as a folded
# scalar is the natural response to that, and read one line at a time a wrapped
# expression looks exactly like a deleted reader.
#
# @param 1 The workflow file.
# @param 2 The sweep step's id.
FindReader() {
    awk -v id="$2" '
        function strip(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
        function flush() {
            if (index(ifExpr, "steps." id ".conclusion") && index(ifExpr, "refs/heads/master"))
                print ifExpr
            ifExpr = ""; inIf = 0
        }
        /^[ \t]*#/               { next }
        /^  [A-Za-z0-9_-]+:[ \t]*$/ { flush(); next }
        /^      - /              { flush(); inIf = 0 }
        /^        [A-Za-z_-]+:/  { inIf = 0 }
        /^        if:/           { inIf = 1
                                   rest = strip(substr($0, index($0, ":") + 1))
                                   if (rest != "" && rest !~ /^[>|]/) ifExpr = rest
                                   next }
        inIf                     { line = strip($0)
                                   if (line != "") ifExpr = (ifExpr == "" ? line : ifExpr " " line) }
        END                      { flush() }
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

    local sweep sweepCount sweepJob sweepId sweepRun sweepBase
    sweep="$(FindSweep "$workflow")"
    if [[ -z "$sweep" ]]; then
        Fail "no step in $workflow runs scripts/tidy-sweep.sh as a sweep; this project's only clang-tidy coverage would be gone and every run would stay green"
        return 1
    fi

    sweepCount="$(printf '%s\n' "$sweep" | wc -l | tr -d ' ')"
    [[ "$sweepCount" -eq 1 ]] \
        || Fail "$sweepCount steps run the sweep; this check reasons about one and cannot vouch for the rest"

    sweepJob="$(printf '%s\n' "$sweep" | head -1 | cut -f1)"
    sweepId="$(printf '%s\n' "$sweep" | head -1 | cut -f2)"
    sweepRun="$(printf '%s\n' "$sweep" | head -1 | cut -f3)"
    sweepBase="$(printf '%s\n' "$sweep" | head -1 | cut -f4)"

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
    #
    # The job key comes from where the SWEEP was found, never a literal: every
    # other rule follows the step wherever it moves, and a hard-coded
    # `clang-tidy` would leave this one reading the permissions of a job that no
    # longer contains the reporter.
    if [[ -n "$reader" ]]; then
        local perms
        perms="$(FindJobPermissions "$workflow" "$sweepJob")"
        if [[ "$perms" != *"issues:"*"write"* ]]; then
            Fail "job \`${sweepJob}\` does not grant \`issues: write\`, so its report step would be refused by the API and the only reader on the only full sweep would be decorative -- permissions: ${perms:-<none>}"
        else
            echo "  ok: job \`${sweepJob}\` grants the reader \`issues: write\`"
        fi
    fi

    return "$problems"
}

# ---------------------------------------------------------------------------
# Self-test
#
# Each rule is driven in BOTH directions over a GENERATED workflow: a check that
# has never been seen to fail is a check that has told you nothing, and one that
# only ever fails is a check that would fail on a correct tree too.
#
# Every knob below changes the generated text, and `Case` asserts that it did --
# a knob that silently does nothing would otherwise stage the correct workflow,
# watch the check pass it, and report `ok` for a break that was never made.

# Knobs. `Reset` restores the shipped shape; a case changes one thing.
gPerms=""; gSweepId=""; gGuard=""; gBase=""; gReaderIf=""; gReader=""; gSweepStep=""
Reset() {
    gPerms="yes"
    gSweepId="sweep"
    gGuard="\${{ (github.event_name != 'pull_request' && github.event_name != 'merge_group') && '--all' || '' }}"
    gBase="\${{ github.event_name == 'merge_group' && github.event.merge_group.base_sha || format('origin/{0}', github.base_ref || 'master') }}"
    gReaderIf="\${{ failure() && steps.sweep.conclusion == 'failure' && github.event_name == 'push' && github.ref == 'refs/heads/master' }}"
    gReader="yes"
    gSweepStep="yes"
}

# The synthetic workflow. Indentation matches build.yml's, because that is what
# the extractors key on.
Fragment() {
    echo "jobs:"
    echo "  clang-tidy:"
    echo "    name: \"clang-tidy\""
    if [[ -n "$gPerms" ]]; then
        echo "    permissions:"
        echo "      contents: read"
        echo "      issues: write"
    fi
    echo "    steps:"
    echo "      # A comment naming scripts/tidy-sweep.sh --all, which must not be read."
    echo "      - name: \"Self-test the sweep's scope computation\""
    echo "        run: scripts/tidy-sweep.sh --self-test"
    if [[ -n "$gSweepStep" ]]; then
        echo "      - name: \"Sweep\""
        [[ -n "$gSweepId" ]] && echo "        id: ${gSweepId}"
        echo "        env:"
        echo "          BASE: ${gBase}"
        echo "        run: >-"
        echo "          scripts/tidy-sweep.sh"
        echo "          ${gGuard}"
    fi
    if [[ -n "$gReader" ]]; then
        echo "      - name: \"Report a full sweep that only master saw\""
        echo "        if: ${gReaderIf}"
        echo "        run: gh issue create"
    fi
    echo "  other:"
    echo "    steps:"
    echo "      - run: true"
}

SelfTest() {
    local status=0 scratch correct
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    Reset
    correct="${scratch}/correct.yml"
    Fragment > "$correct"

    # @param 1 What is being staged. @param 2 want-pass|want-fail.
    # Reads the knobs as they stand; resets them afterwards.
    Case() {
        local what="$1" want="$2" file="${scratch}/wf.yml" got
        Fragment > "$file"
        if [[ "$want" == "want-fail" ]] && cmp -s "$file" "$correct"; then
            echo "  FAIL ${what}: the case generated the CORRECT workflow, so it staged no break at all"
            status=1
            Reset
            return
        fi
        if [[ "$want" == "want-pass" ]] && ! cmp -s "$file" "$correct"; then
            echo "  FAIL ${what}: expected the shipped shape and generated something else"
            status=1
            Reset
            return
        fi
        if CheckWorkflow "$file" >/dev/null 2>&1; then got=want-pass; else got=want-fail; fi
        if [[ "$got" == "$want" ]]; then
            echo "  ok   ${what}"
        else
            echo "  FAIL ${what}: wanted ${want#want-}, got ${got#want-}"
            status=1
        fi
        Reset
    }

    echo "TIDY SWEEP SCOPE SELF-TEST"
    Case "the shipped shape passes" want-pass

    gGuard=""
    Case "no --all anywhere" want-fail

    gGuard="--all"
    Case "--all unconditionally" want-fail

    gGuard="\${{ github.event_name == 'push' && '--all' || '' }}"
    Case "the inclusive spelling (a new trigger would diff-scope)" want-fail

    gGuard="\${{ github.event_name != 'pull_request' && '--all' || '' }}"
    Case "the queue no longer excluded" want-fail

    gGuard="\${{ github.event_name != 'merge_group' && '--all' || '' }}"
    Case "the pull request no longer excluded" want-fail

    gBase="\${{ format('origin/{0}', github.base_ref || 'master') }}"
    Case "the queue's own base dropped" want-fail

    gReader=""
    Case "the reader deleted" want-fail

    gReaderIf="\${{ failure() && steps.sweep.conclusion == 'failure' && github.event_name == 'push' }}"
    Case "the reader no longer scoped to master" want-fail

    gReaderIf="\${{ failure() && github.ref == 'refs/heads/master' }}"
    Case "the reader keyed on the job, not the step" want-fail

    gPerms=""
    Case "issues: write withdrawn" want-fail

    gSweepId=""
    Case "the sweep step's id removed" want-fail

    gSweepStep=""
    Case "the sweep step gone entirely" want-fail

    # And the two shapes that must NOT be mistaken for a break. Both are pure
    # reflows of the shipped expressions, and both used to fail: the run: block
    # collapsed to one line, and the reader's if: wrapped as a folded scalar.
    # A red build accusing an author of deleting the sweep they only reformatted
    # is worse than no check, because it is acted on.
    ReflowedRun() {
        Fragment | awk '
            /^        run: >-$/ { inRun = 1; line = "        run: scripts/tidy-sweep.sh"; next }
            inRun && /^          scripts\/tidy-sweep\.sh$/ { next }
            inRun && /^          / { sub(/^ +/, "", $0); print line " " $0; inRun = 0; next }
            { print }
        '
    }
    local file="${scratch}/wf.yml"
    ReflowedRun > "$file"
    if cmp -s "$file" "$correct"; then
        echo "  FAIL a one-line run: is not a deleted sweep: the reflow changed nothing"
        status=1
    elif CheckWorkflow "$file" >/dev/null 2>&1; then
        echo "  ok   a one-line run: is not a deleted sweep"
    else
        echo "  FAIL a one-line run: is not a deleted sweep: the check rejected a pure reflow"
        status=1
    fi

    Reset
    gReaderIf=""
    { Fragment | awk '
        /^        if: $/ { print "        if: >-"
                           print "          ${{ failure() && steps.sweep.conclusion == '\''failure'\''"
                           print "          && github.event_name == '\''push'\''"
                           print "          && github.ref == '\''refs/heads/master'\'' }}"
                           next }
        { print }
      '; } > "$file"
    if CheckWorkflow "$file" >/dev/null 2>&1; then
        echo "  ok   a folded reader if: is not a deleted reader"
    else
        echo "  FAIL a folded reader if: is not a deleted reader: the check rejected a pure reflow"
        status=1
    fi
    Reset

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
