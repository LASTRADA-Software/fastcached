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
# Since #570 the mapping that decides all of this is ONE table -- `CiScopeTable`
# in `scripts/tidy-sweep.sh`, driven by that script's own `--self-test`. It used
# to be two GitHub expressions on the sweep step composed with the shell inside
# the script, so the mapping could only be reconstructed across two languages.
# The reader's `if:` below is a separate per-event expression -- who is TOLD,
# not how wide the analysis is -- and rule C still checks it here. #570's
# acceptance counts it as a third expression of the same mapping; it is not, and
# moving it would have made one table describe two things, so the deviation was
# decided rather than overlooked.
# This file therefore no longer checks a SPELLING; it checks the WIRING: that the
# workflow reaches the table (`--ci`), that it has not started deciding again
# alongside it, and that the one fact only the workflow holds gets there.
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
SweepScript="scripts/tidy-sweep.sh"

# The env NAME `scripts/tidy-sweep.sh --ci` actually READS for the queue's base
# commit, taken from that script's own call rather than restated here.
#
# Rule B is about a value REACHING the table, and a value reaches it under a
# name. Matching only the `github.event.merge_group.base_sha` expression cannot
# see the one edit that breaks the wiring while leaving the expression in place
# -- renaming the key -- and that edit fails in the silent direction: the script
# reads an unset variable, `CiScopeFor` takes the `origin/master` fallback, and
# every queue entry diffs against master while this check prints `ok`. A second
# copy of the name here would be a second thing to be wrong, so it is read.
#
# @param 1 The sweep script.
FindQueueBaseEnvName() {
    local line
    line="$(grep -m1 'CiScopeFor "\${GITHUB_EVENT_NAME' "$1")" || return 1
    printf '%s\n' "$line" \
        | grep -o '\${[A-Za-z_][A-Za-z0-9_]*:-}' \
        | sed 's/^\${//; s/:-}$//' \
        | sed -n '3p'
}

# ---------------------------------------------------------------------------
# Find the sweep in a workflow: the step that invokes `scripts/tidy-sweep.sh` as
# the sweep rather than as its own self-test. Emits one
# `<job key>\t<step id>\t<run: block, joined>\t<env: block, joined>` row per such
# step.
#
# The whole `env:` block rather than one key: since #570 the step passes FACTS and
# the decision lives in `CiScopeTable`, so what matters is which facts reach the
# script, not what any one of them is called. Keyed on a single name, renaming the
# variable would empty the field and rule B would report the queue's base as
# deleted -- a red build accusing an author of something they did not do, which
# this file already carries two controls against.
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
# ---------------------------------------------------------------------------
# Every fact this check reads out of a workflow, from the SHARED walk.
#
# Three awk programs used to live here, each spelling `build.yml`'s indentation
# as literal columns -- `/^        id:/`, `/^        env:/`, `/^        if:/` at
# eight spaces, `/^          /` at ten, `/^      - /` at six. Every one was right
# for the file as written and said nothing about a file whose jobs sit at another
# depth, which is legal YAML. `scripts/check-tidy-sweep-scope.awk` holds this
# check's half over `scripts/lib/workflow-walk.awk` and documents the records
# (#1456).
#
# A missing awk program is refused BY NAME: a reader that ran over no program is
# not a clean tree, and awk's own complaint names a file rather than the check
# whose verdict just became meaningless.
FastCachedWalkAwk="$(dirname "${BASH_SOURCE[0]}")/lib/workflow-walk.awk"
FastCachedScopeAwk="$(dirname "${BASH_SOURCE[0]}")/check-tidy-sweep-scope.awk"
for awkProgram in "$FastCachedWalkAwk" "$FastCachedScopeAwk"; do
    if [ ! -f "$awkProgram" ]; then
        echo "check-tidy-sweep-scope: missing awk program ${awkProgram}; no workflow was read, so" >&2
        echo "  this is a refusal and not a clean run. It is expected beside this script." >&2
        exit 2
    fi
done

# @param 1 the workflow  @param 2 which records  @param 3 a step id  @param 4 a job key
WorkflowRead() {
    awk -v want="$2" -v sweepId="${3:-}" -v sweepJob="${4:-}" \
        -f "$FastCachedWalkAwk" -f "$FastCachedScopeAwk" "$1" "$1" | cut -f2-
}

FindSweep() {
    WorkflowRead "$1" sweep
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
# @param 1 workflow  @param 2 the sweep step's `id:`  @param 3 the JOB it sits in
#
# Scoped to the job, and that is not tidiness. `steps.<id>.conclusion` only ever
# resolves within its own job, so a reader in job A says nothing about a sweep in
# job B. Before this took a job, two sweep steps both using `id: sweep` made the
# check report the SECOND one as having a reader -- it had found the FIRST one's.
# The generalisation to N steps is what exposed it: with one sweep the bug was
# unreachable, and it would have shipped a leg whose failures reach nobody while
# the guard said otherwise.
FindReader() {
    WorkflowRead "$1" reader "$2" "$3"
}

# The `permissions:` block of one job, comments dropped.
# @param 1 The workflow file.
# @param 2 The job key.
FindJobPermissions() {
    WorkflowRead "$1" perms "" "$2"
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

    local sweep sweepCount sweepJob sweepId sweepRun sweepEnv
    sweep="$(FindSweep "$workflow")"
    if [[ -z "$sweep" ]]; then
        Fail "no step in $workflow runs scripts/tidy-sweep.sh as a sweep; this project's only clang-tidy coverage would be gone and every run would stay green"
        return 1
    fi

    sweepCount="$(printf '%s\n' "$sweep" | wc -l | tr -d ' ')"
    echo "  ok: $sweepCount step(s) run the sweep; every rule below is applied to each"

    # EVERY sweep step, not the first. This used to assert there was exactly one,
    # saying it "reasons about one and cannot vouch for the rest" -- which was
    # honest, and became a wall the moment a second analyser leg existed (#858).
    #
    # The rules were always per-STEP, so the generalisation is a loop rather than a
    # rewrite. What must NOT happen is the count assertion being replaced by a
    # count: rules A-D each close a way a sweep reports nothing, so a check that
    # merely tallied steps would pass while the reporting hole it exists to guard
    # reopened -- the fix wearing the defect's clothes.
    while IFS=$'\t' read -r sweepJob sweepId sweepRun sweepEnv; do
        [[ -z "${sweepJob:-}" ]] && continue
        echo "  -- sweep in job \`${sweepJob}\`"

    [[ -n "$sweepId" ]] \
        || Fail "the sweep step has no \`id:\`, so nothing downstream can read its conclusion apart from the whole job's -- which fires on every unrelated infrastructure failure in the job, and is the alarm people mute"

    # -----------------------------------------------------------------------
    # Rule A: the workflow CONSULTS the mapping and restates none of it.
    #
    # Until #570 the scope decision was two GitHub expressions on this step,
    # composed with the shell inside the script, and this rule asserted their
    # SPELLING: `--all` had to be guarded by EXCLUDING the two
    # diff-scoped events rather than by naming `push`, because the inclusive
    # spelling diff-scopes any trigger added to `on:` later, and on master that
    # diff is EMPTY, so the sweep exits 0 having analysed nothing.
    #
    # That property did not go away; it moved somewhere it cannot be spelled
    # backwards. `CiScopeTable` in `scripts/tidy-sweep.sh` lists the diff-scoped
    # events and everything else falls to a default row that sweeps everything,
    # and `scripts/tidy-sweep.sh --self-test` -- which the step above this one
    # already runs -- drives every row, `push`, a tag push and an unforeseen
    # trigger included. So what is left for THIS check is the wiring: that the
    # workflow reaches the table at all, and that it has not started deciding
    # again alongside it. Two decisions that can disagree are worse than one that
    # is coarse.
    # `--only=` is the second legitimate scope source: an explicit set named by a
    # file IN THE REPOSITORY, for a leg that covers a fixed subset rather than a
    # diff (#858's Windows leg reads the units `tidy-blind-spots.txt` says it
    # reaches). It satisfies the same property `--ci` does -- the workflow does not
    # DECIDE the scope, it passes a fact -- so it is accepted, and the branch-on-
    # event rules below still apply to it.
    if [[ "$sweepRun" == *"--only="* ]]; then
        if [[ "$sweepRun" != *"scripts/"* ]]; then
            Fail "the sweep passes \`--only=\` but the set does not come from a file under \`scripts/\`, so what this leg covers is decided in the workflow rather than somewhere reviewable -- run: $sweepRun"
        else
            echo "  ok: the sweep is explicitly scoped from a file under \`scripts/\`"
        fi
    elif [[ "$sweepRun" != *"--ci"* ]]; then
        Fail "the sweep does not pass \`--ci\`, so it does not consult \`CiScopeTable\` in scripts/tidy-sweep.sh and the event -> (scope, base) mapping is being decided somewhere else -- run: $sweepRun"
    elif [[ "$sweepRun" == *"--all"* ]]; then
        Fail "the sweep passes \`--all\` alongside \`--ci\`; every pull request and every queue entry would sweep the whole tree again, which is the cost #554 removed -- run: $sweepRun"
    elif [[ "$sweepRun" == *"github.event_name"* ]]; then
        Fail "the sweep's command line branches on \`github.event_name\`, so the workflow is deciding the scope as well as the table. Pass the FACTS and let \`CiScopeTable\` decide (#570): two mappings that can disagree are worse than one -- run: $sweepRun"
    elif [[ "$sweepEnv" == *"github.event_name"* ]]; then
        Fail "the sweep's \`env:\` branches on \`github.event_name\`, so the base is being decided in the workflow rather than by \`CiScopeTable\` (#570) -- env: $sweepEnv"
    else
        echo "  ok: the sweep consults CiScopeTable via \`--ci\` and restates none of the mapping"
    fi

    # -----------------------------------------------------------------------
    # Rule B: the queue's own base actually REACHES the table.
    #
    # `CiScopeTable` decides that a `merge_group` diff-scopes against the entry it
    # is stacked on, but the base commit itself is a value only the workflow
    # context holds -- `GITHUB_EVENT_NAME` and `GITHUB_BASE_REF` are in every
    # step's environment and this one is not. Drop it here and the table's
    # `origin/master` fallback takes over, silently.
    #
    # Losing it is not a hole: `origin/master` resolves and over-approximates. It
    # is a COST rule, asserted because the way it gets lost is somebody removing
    # an env var that looks unused from the workflow -- which it is, from here.
    local queueBaseEnv
    queueBaseEnv="$(FindQueueBaseEnvName "$SweepScript")"
    if [[ -z "$queueBaseEnv" ]]; then
        # Every way of not knowing escalates, as everywhere else here: a name this
        # check could not read is a rule it cannot apply, not a rule that passed.
        Fail "cannot read from $SweepScript which environment variable its \`--ci\` path takes the queue's base commit from, so this rule cannot be applied at all"
    elif [[ "$sweepEnv" != *"${queueBaseEnv}:"* ]]; then
        Fail "the sweep's \`env:\` sets no \`${queueBaseEnv}\`, which is the name $SweepScript reads the queue's base commit from, so a queue entry falls back to \`origin/master\` and diffs against something other than the entry it is stacked on -- env: ${sweepEnv:-<none>}"
    elif [[ "$sweepEnv" != *"merge_group.base_sha"* ]]; then
        Fail "the sweep's \`env:\` does not pass \`github.event.merge_group.base_sha\`, so a queue entry falls back to \`origin/master\` and diffs against something other than the entry it is stacked on -- env: ${sweepEnv:-<none>}"
    else
        echo "  ok: the queue's own base_sha reaches the table as \`${queueBaseEnv}\`"
    fi

    # -----------------------------------------------------------------------
    # Rule C: the only full sweep left has a reader, keyed on the sweep STEP.
    #
    # Never on the job: over the 18 failed master-push runs in the rulebook's
    # sample the job failed 18 times and the sweep step itself failed none, so a
    # job-keyed alarm is a thing people mute by the second week.
    local reader
    reader="$(FindReader "$workflow" "$sweepId" "$sweepJob")"
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
            Fail "job \`${sweepJob}\` does not grant \`issues: write\`, so its report step would be refused by the API and its reader would be decorative -- permissions: ${perms:-<none>}"
        else
            echo "  ok: job \`${sweepJob}\` grants the reader \`issues: write\`"
        fi
    fi
    done < <(printf '%s\n' "$sweep")

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
gPerms=""; gSweepId=""; gArgs=""; gEnvBase=""; gReaderIf=""; gReader=""; gSweepStep=""
Reset() {
    gPerms="yes"
    gSweepId="sweep"
    gArgs="--ci"
    gEnvBase="MERGE_GROUP_BASE_SHA: \${{ github.event.merge_group.base_sha }}"
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
        echo "          TIDY: clang-tidy-22"
        [[ -n "$gEnvBase" ]] && echo "          ${gEnvBase}"
        echo "        run: >-"
        echo "          scripts/tidy-sweep.sh"
        echo "          ${gArgs}"
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

    gArgs=""
    Case "the table not consulted at all (no --ci)" want-fail

    gArgs="--all"
    Case "--all unconditionally, bypassing the table" want-fail

    gArgs="--ci --all"
    Case "--all alongside --ci" want-fail

    # Deliberately spelled WITHOUT `--all`: with it, the case is caught by the
    # rule above and the `github.event_name` arm is never once seen to bite,
    # which is a guard that has told you nothing.
    gArgs="--ci --base \${{ github.event_name == 'merge_group' && github.event.merge_group.base_sha || 'origin/master' }}"
    Case "the workflow deciding the scope again beside the table" want-fail

    gEnvBase="BASE: \${{ github.event_name == 'merge_group' && github.event.merge_group.base_sha || 'origin/master' }}"
    Case "the workflow deciding the base again beside the table" want-fail

    # The rename, which is the one break that leaves the EXPRESSION in place: the
    # script reads `MERGE_GROUP_BASE_SHA` and gets nothing, so the queue silently
    # diffs against master. A rule matching only the expression reports `ok` here.
    gEnvBase="QUEUE_BASE_SHA: \${{ github.event.merge_group.base_sha }}"
    Case "the queue's base carried under a name the script does not read" want-fail

    gEnvBase=""
    Case "the queue's own base_sha never reaching the table" want-fail

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

    # The reader's `if:` written as a FOLDED scalar. Its own case below asserts the
    # check accepts one; the walk neuter reuses it, because `WfBlockKey` is reached
    # only by a CONTINUATION line and the shipped fragment writes that `if:` on one
    # line -- a neuter is evidence only over input that reaches the line it disabled.
    FoldedReaderIf() {
        gReaderIf=""
        Fragment | awk '
        /^        if: $/ { print "        if: >-"
                           print "          ${{ failure() && steps.sweep.conclusion == '\''failure'\''"
                           print "          && github.event_name == '\''push'\''"
                           print "          && github.ref == '\''refs/heads/master'\'' }}"
                           next }
        { print }
      '
    }

    Reset
    FoldedReaderIf > "$file"
    if CheckWorkflow "$file" >/dev/null 2>&1; then
        echo "  ok   a folded reader if: is not a deleted reader"
    else
        echo "  FAIL a folded reader if: is not a deleted reader: the check rejected a pure reflow"
        status=1
    fi
    Reset

    # The verdict must come from the SHARED walk, which is a different claim from
    # "the check refuses". Every case above would pass just as well if this script
    # had kept a private reader beside the shared one. So the library is copied,
    # one line disabled, and `FastCachedWalkAwk` pointed at the copy for one case.
    #
    # @param 1 what the line holds up  @param 2 a sed expression disabling it
    # @param 3 want-pass|want-fail  @param 4 which generator stages the workflow
    WalkNeuter() {
        local what="$1" expr="$2" want="$3" stage="${4:-Fragment}"
        local file="${scratch}/wf.yml" neutered got realWalk
        Reset
        "$stage" > "$file"
        neutered="${scratch}/neutered-walk.awk"
        sed "$expr" "$FastCachedWalkAwk" > "$neutered"
        if cmp -s "$neutered" "$FastCachedWalkAwk"; then
            echo "  FAIL ${what}: the sed expression neutered no line, so the case stages nothing"
            status=1
            return
        fi
        realWalk="$FastCachedWalkAwk"
        FastCachedWalkAwk="$neutered"
        if CheckWorkflow "$file" >/dev/null 2>&1; then got=want-pass; else got=want-fail; fi
        FastCachedWalkAwk="$realWalk"
        if [[ "$got" == "$want" ]]; then
            echo "  ok   ${what}"
        else
            echo "  FAIL ${what}: wanted ${want#want-}, got ${got#want-}"
            status=1
        fi
    }

    WalkNeuter "neutering the shared walk's step boundary makes this check REFUSE, so its verdict comes from that walk" \
        '/WorkflowFlushStep(); WorkflowResetStep(); stepItem = 1/s/^/#/' want-fail
    WalkNeuter "neutering the shared walk's WfBlockKey makes this check REFUSE over a FOLDED reader if:, so that if: is joined through that field" \
        's/^        WfBlockOwner = ind; WfBlockKey = key$/        WfBlockOwner = ind/' want-fail FoldedReaderIf
    # The same folded shape with the library untouched must PASS, or the case above
    # says only that a folded `if:` is refused for some other reason entirely.
    WalkNeuter "a FOLDED reader if: passes over an untouched copy of the shared walk, so the case above names its cause" \
        '1s|^# SPDX|# neutered-nothing\n# SPDX|' want-pass FoldedReaderIf
    # And the control, without which the two above prove only that a damaged
    # library breaks something: an UNTOUCHED copy at a different path must pass.
    WalkNeuter "a copy of the shared walk with only a comment changed still PASSES, so the two cases above name their cause" \
        '1s|^# SPDX|# neutered-nothing\n# SPDX|' want-pass
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
