#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# A job gated on the scope classifier must FAIL SAFE when the classifier does not
# answer.
#
# ## The defect this exists to stop returning
#
# `scripts/ci-scope.sh` states its own rule at length: every way of not knowing —
# an unresolvable ref, a failed diff, an empty diff, a path matching no row —
# escalates to `code=true`, because *"we could not tell what changed" must never
# read as "nothing did"*.
#
# The workflow reading it inverted that principle one level up. Sixteen jobs were
# gated `if: needs.changes.outputs.code == 'true'`, and if the `changes` job
# itself FAILS there is no output at all: the comparison is false, the jobs are
# skipped, and — measured on the head of the docs-only #356 (`b4777aa`) — a
# skipped required context reports `conclusion: skipped` and the ruleset reads it
# as **passing**. `changes` is not itself a required context. So a classifier
# that died produced a green, mergeable pull request that nothing had compiled:
# the exact outcome `ci-scope.sh`'s own header warns about, reached through the
# workflow rather than through the script.
#
# The fix is one token — `!= 'false'` rather than `== 'true'` — which makes "did
# not answer" mean build-everything, agreeing with the script it reads. Plus
# `!cancelled()`, so a failed dependency does not skip the job before its
# condition is even consulted.
#
# ## Why it is a check and not sixteen careful edits
#
# Sixteen hand-edits with nothing asserting them is how this comes back. A
# reviewer cannot eyeball thirty-six conditions; what makes the tree safe is that
# the SEVENTEENTH job cannot be added wrong.
#
# Both rules are derived from the workflow rather than tabulated — a hand-written
# list of gated job names would not be a cross-check, it would be a second thing
# to be wrong, the same reasoning `check-tsan-scope.cmake` and
# `check-merge-queue-contexts.sh` record. `release` needs `changes` too and is
# correctly excluded by construction: it never reads the classifier's output, so
# rule B never applies to it, and a release must NOT run when jobs have failed.
#
# ## And the third rule: the checks whose SUBJECT is documentation are NOT gated
#
# The two rules above are about a gate failing safe. The third is about a gate
# that should never have been there. #687: every check whose subject is
# documentation was skipped on exactly the pull requests that change
# documentation, because `code=false` is right for a compiler and backwards for a
# check that reads prose. Prose drifts by being edited, and a prose-only edit is
# the change those checks never saw.
#
# So `scripts/doc-subject-checks.sh` runs from a step carrying no scope condition
# at all -- and that step lives in a job producing a REQUIRED context, because a
# doc check in an unrequired job is #684: a failure reported nowhere, on a pull
# request that merges green. Fixing #687 by reintroducing #684 one file over is
# not a fix, and the two properties are independent, so both are asserted.
#
# The required-context list is READ from `scripts/check-merge-queue-contexts.sh`,
# not restated. A second copy is not a cross-check, it is a second thing to be
# wrong.
#
# ## And the fourth rule: a job that gates the release survives a tag run
#
# #559 trimmed six jobs off the master push, because a push run buys no merge
# latency -- it happens after the merge -- and those six write no cache entry any
# other run can read. That is the first condition in this workflow keyed on the
# EVENT rather than on the scope classifier, and it introduces a failure the
# three rules above cannot see.
#
# `release` needs every other job in this file, and `check-release-gate` asserts
# that list stays complete. A `needs` job that is SKIPPED skips its dependent --
# `release` carries `if: startsWith(github.ref, 'refs/tags/v')`, which has no
# status function, so GitHub still ANDs `success()` onto it. So a condition that
# excludes a tag run does not fail the release: it makes the release job vanish,
# and a tag would be pushed, every check would be green, and no draft would ever
# appear. The `changes` job already states the invariant this depends on in a
# comment -- *"on a tag nothing below is ever skipped"* -- and nothing asserted
# it.
#
# A shell check cannot evaluate a GitHub expression, so this asserts the SPELLING
# instead: an event-keyed condition on a job that gates the release must contain
# the exact clause `github.ref_type != 'branch'`, which is the one clause that
# lets a tag through. Any other spelling is refused by name rather than analysed
# -- the same answer rule A gives a classifier read in a shape it does not
# recognise, and for the same reason: a check that cannot vouch for something
# must say so rather than pass it.
#
# The gating set is READ from `release.needs`, never restated, and an empty read
# is a refusal: with no gating jobs every condition would look unregulated and
# this rule would vouch for all of them, which is rule C's empty-table failure
# one file over.

# ## What is asserted rather than demonstrated
#
# The `changes` job cannot be made to fail on demand, so this proves the SHAPE of
# the conditions, not the behaviour of a run in which the classifier died. The
# behaviour it relies on is the measurement above (`b4777aa`), which is recorded
# in `.agent/rules/build-and-toolchain.md` rather than re-derived here.
#
# ## bash 3.2, and a self-test
#
# In the default ctest set, so it runs on macOS's 2007 `/bin/bash`: no `mapfile`,
# no `declare -A`, no `${var^^}`. And `--self-test` drives every rule against
# GENERATED workflows in both directions -- for two runs these three rules had
# only ever been seen to PASS, which is the state this whole file is about one
# level up. Generated rather than edited from a copy: an edit that matches no
# anchor changes nothing and reports success.
#
# Usage:  scripts/check-gated-jobs.sh [--workflow FILE] [--self-test]

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

DocSubjectRunner="scripts/doc-subject-checks.sh"

Main() {
workflow="$1"
problems=0
Fail() { echo "  FAIL: $*" >&2; problems=$((problems + 1)); }

[[ -f "$workflow" ]] || { echo "check-gated-jobs: $workflow does not exist" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Rule A: the classifier's output is only ever compared `!= 'false'`.
#
# Counted rather than grepped for a match, so that "none" and "some" are both
# visible and a new reference in a shape nobody anticipated fails rather than
# passing unnoticed.
total="$(grep -c 'needs\.changes\.outputs\.code' "$workflow" || true)"
safe="$(grep -co "needs\.changes\.outputs\.code[[:space:]]*!=[[:space:]]*'false'" "$workflow" || true)"
equality="$(grep -c "needs\.changes\.outputs\.code[[:space:]]*==" "$workflow" || true)"

if [[ "$equality" -ne 0 ]]; then
    Fail "$equality condition(s) compare the scope classifier with \`==\`. Use \`!= 'false'\`: when the \`changes\` job FAILS it publishes no output, so \`== 'true'\` is false, the job is SKIPPED, and a skipped required context is read as passing -- a green pull request that nothing compiled."
    grep -n "needs\.changes\.outputs\.code[[:space:]]*==" "$workflow" | sed 's/^/    /' >&2
fi

if [[ "$safe" -ne "$total" ]]; then
    Fail "$total reference(s) to the scope classifier but only $safe of them read \`!= 'false'\`; the rest are in a shape this check does not recognise and cannot vouch for."
fi

[[ "$problems" -eq 0 ]] && echo "ok: all $total scope-classifier conditions read != 'false'"

# ---------------------------------------------------------------------------
# Rule B: a job that consults the classifier -- in its own condition or in any
# step's -- carries a job-level `if:` containing `!cancelled()`.
#
# Without it, a FAILED `changes` skips the job before its condition is consulted,
# and rule A buys nothing. For a matrix job it is worse than a wrong-green: a
# skipped matrix job never expands, so its per-leg contexts never exist at all
# and the pull request cannot merge.
guarded="$(awk '
    function flush() {
        if (jobKey != "" && usesCode && index(jobIf, "!cancelled()") == 0)
            print "MISSING\t" jobKey "\t" (jobIf == "" ? "<no job-level if:>" : jobIf)
        jobKey = ""; jobIf = ""; usesCode = 0
    }
    /^jobs:[ \t]*$/             { inJobs = 1; next }
    inJobs && /^[^ \t]/         { flush(); inJobs = 0 }
    !inJobs                     { next }
    /^[ \t]*#/                  { next }
    /^  [A-Za-z0-9_-]+:[ \t]*$/ { flush()
                                  jobKey = $0
                                  sub(/^  /, "", jobKey)
                                  sub(/:[ \t]*$/, "", jobKey)
                                  next }
    /^    if:/                  { jobIf = $0; sub(/^    if:[ \t]*/, "", jobIf) }
    /needs\.changes\.outputs\.code/ { usesCode = 1 }
    END                         { flush() }
' "$workflow")"

if [[ -n "$guarded" ]]; then
    while IFS=$'\t' read -r _ jobKey jobIf; do
        Fail "job '$jobKey' consults the scope classifier but its job-level condition does not contain \`!cancelled()\`, so a FAILED \`changes\` skips it outright -- if: $jobIf"
    done <<< "$guarded"
else
    echo "ok: every job that consults the classifier survives a failed one"
fi

# ---------------------------------------------------------------------------
# Rule C: the doc-subject checks run UNGATED, from a job that gates.
#
# Two independent properties over one step, because failing either restores a
# different bug: gated, it is #687 (skipped on the only change it exists to
# catch); in an unrequired job, it is #684 (a failure nobody is shown, on a pull
# request that merges).
#
# The whole step block is read -- its own `if:`, and the job-level `if:` above it
# -- because either can carry the condition and only one of the two is where a
# reader would look.
docSubject="$(awk -v runner="$DocSubjectRunner" '
    function flushStep() {
        if (inStep && stepUsesRunner)
            print jobKey "\t" jobName "\t" jobIf "\t" stepIf
        inStep = 0; stepIf = ""; stepUsesRunner = 0
    }
    function flushJob() { flushStep(); jobKey = ""; jobName = ""; jobIf = "" }
    /^jobs:[ \t]*$/                { inJobs = 1; next }
    inJobs && /^[^ \t]/            { flushJob(); inJobs = 0 }
    !inJobs                        { next }
    # A comment naming the runner is not a call site. Without this the job\047s
    # own explanatory comment satisfied rule C, and the step it was attributed to
    # was whichever block happened to be open -- the check reported the same step
    # TWICE against the real workflow, which is the only reason it was noticed.
    # A rule satisfied by prose is a rule that passes with the code deleted.
    /^[ \t]*#/                     { next }
    /^  [A-Za-z0-9_-]+:[ \t]*$/    { flushJob()
                                     jobKey = $0
                                     sub(/^  /, "", jobKey); sub(/:[ \t]*$/, "", jobKey)
                                     next }
    /^    name:[ \t]*/             { jobName = $0
                                     sub(/^    name:[ \t]*/, "", jobName)
                                     gsub(/^"|"$/, "", jobName)
                                     next }
    /^    if:[ \t]*/               { jobIf = $0; sub(/^    if:[ \t]*/, "", jobIf); next }
    /^      - /                    { flushStep(); inStep = 1 }
    inStep && /^        if:[ \t]*/ { stepIf = $0; sub(/^        if:[ \t]*/, "", stepIf) }
    inStep && index($0, runner)    { stepUsesRunner = 1 }
    END                            { flushJob() }
' "$workflow")"

if [[ -z "$docSubject" ]]; then
    Fail "no step in $workflow runs \`$DocSubjectRunner\`. The checks whose SUBJECT is documentation would then run only when something OTHER than documentation changed, which is #687 -- and a doc-check step that is absent reports exactly as green as one that passed."
else
    # Read once, so a rename in the required table is a failure here rather than
    # a silent reclassification of every job as unrequired.
    requiredFile="${FASTCACHED_REQUIRED_CONTEXTS_FILE:-scripts/check-merge-queue-contexts.sh}"
    requiredNames=""
    if [[ -f "$requiredFile" ]]; then
        requiredNames="$(awk '
            /^RequiredContexts=\(/ { inTable = 1; next }
            inTable && /^\)/       { inTable = 0 }
            inTable                {
                line = $0
                sub(/^[ \t]*"/, "", line); sub(/"[ \t]*$/, "", line); sub(/\|.*$/, "", line)
                if (length(line)) print line
            }
        ' "$requiredFile")"
    fi
    if [[ -z "$requiredNames" ]]; then
        Fail "read 0 required contexts out of $requiredFile; with an empty list every job looks unrequired and rule C would vouch for a doc check nothing gates on"
    fi

    while IFS=$'\t' read -r jobKey jobName jobIf stepIf; do
        [[ -n "$jobKey" ]] || continue
        if [[ "$stepIf" == *needs.changes.outputs.code* ]]; then
            Fail "the \`$DocSubjectRunner\` step in job '$jobKey' is gated on the scope classifier -- if: $stepIf. A docs-only change answers code=false, so the checks whose subject is documentation would be skipped on exactly the change they exist to catch (#687)."
        elif [[ "$jobIf" == *needs.changes.outputs.code* ]]; then
            Fail "job '$jobKey' runs \`$DocSubjectRunner\` but the JOB is gated on the scope classifier -- if: $jobIf. The step's own condition is irrelevant when the job never starts (#687)."
        else
            echo "ok: '$DocSubjectRunner' runs ungated in job '$jobKey'"
        fi

        if printf '%s\n' "$requiredNames" | grep -Fxq -- "$jobName"; then
            echo "ok: job '$jobKey' produces the required context '$jobName', so a failing doc check blocks the merge"
        else
            Fail "job '$jobKey' runs \`$DocSubjectRunner\` but its name '$jobName' is not a required context. A doc check that reports and does not GATE is #684 -- measured on this repository, five of the last six failing merge-group runs failed an unrequired job and all five pull requests merged with nobody told."
        fi
    done <<< "$docSubject"
fi

# ---------------------------------------------------------------------------
# Rule D: an event-keyed condition on a job that gates the release names
# `github.ref_type != 'branch'`, so a tag run is never the run it skips.
releaseNeeds="$(awk '
    /^jobs:[ \t]*$/             { inJobs = 1; next }
    inJobs && /^[^ \t]/         { inJobs = 0 }
    !inJobs                     { next }
    /^[ \t]*#/                  { next }
    /^  [A-Za-z0-9_-]+:[ \t]*$/ { jobKey = $0
                                  sub(/^  /, "", jobKey); sub(/:[ \t]*$/, "", jobKey)
                                  inNeeds = 0
                                  next }
    jobKey == "release" && /^    needs:[ \t]*$/ { inNeeds = 1; next }
    inNeeds && /^      - [A-Za-z0-9_-]+[ \t]*$/ { need = $0
                                  sub(/^      - /, "", need); sub(/[ \t]*$/, "", need)
                                  print need
                                  next }
    inNeeds && /^    [A-Za-z]/  { inNeeds = 0 }
' "$workflow")"

if [[ -z "$releaseNeeds" ]]; then
    Fail "read 0 jobs out of \`release.needs\` in $workflow; with an empty gating set every event-keyed condition looks unregulated and rule D would vouch for all of them."
else
    # Every job's own `if:`, one `key<TAB>condition` row per job that has one.
    jobConditions="$(awk '
        /^jobs:[ \t]*$/             { inJobs = 1; next }
        inJobs && /^[^ \t]/         { inJobs = 0 }
        !inJobs                     { next }
        /^[ \t]*#/                  { next }
        /^  [A-Za-z0-9_-]+:[ \t]*$/ { jobKey = $0
                                      sub(/^  /, "", jobKey); sub(/:[ \t]*$/, "", jobKey)
                                      next }
        /^    if:/                  { cond = $0; sub(/^    if:[ \t]*/, "", cond)
                                      print jobKey "\t" cond }
    ' "$workflow")"

    trimmed=0
    while IFS=$'\t' read -r jobKey cond; do
        [[ -n "$jobKey" ]] || continue
        # Only jobs that gate the release, and only conditions keyed on the event.
        printf '%s\n' "$releaseNeeds" | grep -Fxq -- "$jobKey" || continue
        case "$cond" in
            *github.event_name*|*github.ref*) ;;
            *) continue ;;
        esac
        trimmed=$((trimmed + 1))
        if [[ "$cond" != *"github.ref_type != 'branch'"* ]]; then
            Fail "job '$jobKey' gates the release and carries an event-keyed condition that does not name \`github.ref_type != 'branch'\` -- if: $cond. A tag push IS a push: without that clause the job is skipped on the tag, a skipped \`needs\` skips \`release\`, and the draft release silently never appears while every check reports green."
        fi
    done <<< "$jobConditions"

    if [[ "$trimmed" -eq 0 ]]; then
        echo "ok: no job that gates the release keys its condition on the event (nothing for rule D to vouch for)"
    else
        echo "ok: all $trimmed release-gating job(s) with an event-keyed condition let a tag run through"
    fi
fi

if [[ $problems -gt 0 ]]; then
    echo "check-gated-jobs: $problems problem(s); a failed scope classifier would go green" >&2
    return 1
fi
echo "check-gated-jobs: the scope gate fails safe"
return 0
}

# ---------------------------------------------------------------------------
# Self-test. Every rule, both directions, against generated workflows.
#
# Each case asserts its generated workflow DIFFERS from the correct one, so a
# knob that no longer changes anything is a failure rather than a pass -- the
# trap `check-tidy-sweep-scope.sh` records, where a `sed` that BSD rejects left
# the fragment empty and the case printed `ok` for a break it never staged.
SelfTest() {
    local scratch status=0 cases=0
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT

    local requiredFile="${scratch}/required.sh"
    cat > "$requiredFile" <<'REQ'
RequiredContexts=(
    "Check C++ style|.github/workflows/build.yml"
    "clang-tidy|.github/workflows/build.yml"
)
REQ

    # Knobs, one per case, applied to a correct workflow by GENERATING it.
    #   $1 = output file
    #   $2 = classifier comparison for the linux job: safe | equality | odd
    #   $3 = whether the linux job carries !cancelled(): yes | no
    #   $4 = the doc-subject step: ungated | gated | absent
    #   $5 = the name of the job carrying it: required | unrequired
    #   $6 = the `coverage` job's push trim: none | canonical | tagless
    #   $7 = what `release` gates on: all | nothing
    Generate() {
        local out="$1" comparison="$2" cancelled="$3" docStep="$4" docJob="$5"
        local trim="${6:-none}" gate="${7:-all}"
        local compare="needs.changes.outputs.code != 'false'"
        [[ "$comparison" == "equality" ]] && compare="needs.changes.outputs.code == 'true'"
        [[ "$comparison" == "odd" ]] && compare="contains(needs.changes.outputs.code, 'true')"
        local jobIf="\${{ !cancelled() }}"
        [[ "$cancelled" == "no" ]] && jobIf="\${{ ${compare} }}"
        local docName="Check C++ style"
        [[ "$docJob" == "unrequired" ]] && docName="Some unrequired job"

        {
            echo "on:"
            echo "  pull_request:"
            echo "jobs:"
            echo "  style:"
            echo "    name: \"${docName}\""
            echo "    if: ${jobIf}"
            echo "    steps:"
            echo "      - uses: actions/checkout@v4"
            # A comment naming the runner, always. Rule C must be satisfied by
            # the STEP and never by prose describing it -- the `absent` case
            # below would otherwise pass on this comment alone, which is how the
            # check first reported a step it had never seen.
            echo "        # see scripts/doc-subject-checks.sh for what this job runs"
            if [[ "$docStep" != "absent" ]]; then
                echo "      - name: \"Doc-subject checks\""
                [[ "$docStep" == "gated" ]] && echo "        if: \${{ ${compare} }}"
                echo "        run: scripts/doc-subject-checks.sh"
            fi
            echo "      - name: \"Run clang-format\""
            echo "        if: \${{ ${compare} }}"
            echo "        run: clang-format --dry-run"

            # A second gating job, so rule D has a subject that is not also
            # rule C's. `tagless` is the defect: it reads "not a push", and a
            # tag push IS a push.
            local trimClause=""
            [[ "$trim" == "canonical" ]] && trimClause=" && (github.event_name != 'push' || github.ref_type != 'branch')"
            [[ "$trim" == "tagless" ]] && trimClause=" && github.event_name != 'push'"
            echo "  coverage:"
            echo "    name: \"Code coverage\""
            echo "    if: \${{ !cancelled() && ${compare}${trimClause} }}"
            echo "    steps:"
            echo "      - run: ctest"

            echo "  release:"
            echo "    if: startsWith(github.ref, 'refs/tags/v')"
            echo "    needs:"
            if [[ "$gate" == "all" ]]; then
                echo "      - style"
                echo "      - coverage"
            fi
            echo "    steps:"
            echo "      - run: gh release create"
        } > "$out"
    }

    local correct="${scratch}/correct.yml"
    Generate "$correct" safe yes ungated required

    Case() {
        local what="$1" want="$2" file="${scratch}/wf.yml" out got=0
        cases=$((cases + 1))
        if [[ "$file" != "$correct" ]] && diff -q "$correct" "$file" >/dev/null 2>&1; then
            echo "  FAIL  '$what' generated a workflow identical to the correct one; the case stages nothing" >&2
            status=1
            return
        fi
        out="$(FASTCACHED_REQUIRED_CONTEXTS_FILE="$requiredFile" bash "$0" --workflow "$file" 2>&1)" || got=$?
        if [[ "$want" == "want-pass" && "$got" -eq 0 ]] || [[ "$want" == "want-fail" && "$got" -ne 0 ]]; then
            echo "  ok    ($want) $what"
        else
            echo "  FAIL  ($want, exit $got) $what" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            status=1
        fi
    }

    # The baseline. Every negative case below is evidence only if this passes --
    # and it is not decoration: it is what caught the whole self-test running
    # nothing at all. See the note on `bash "$0"` above.
    # `bash "$0"` and never a bare `"$0"`. This file is mode 644 in git -- ctest
    # runs it as `bash <path>` and nothing here needs the executable bit -- so a
    # bare `"$0"` exits **126, Permission denied**, having run no check at all.
    # Every `want-fail` case then passes because the SHELL refused, not because
    # the rule fired: a self-test that is entirely green while testing nothing,
    # which is the exact defect this file exists to stop shipping. It was caught
    # only by the one case that expects a PASS, which is the argument for having
    # a baseline case at all rather than only negative ones.
    cp "$correct" "${scratch}/wf.yml"
    cases=$((cases + 1))
    if FASTCACHED_REQUIRED_CONTEXTS_FILE="$requiredFile" bash "$0" --workflow "${scratch}/wf.yml" >/dev/null 2>&1; then
        echo "  ok    (want-pass) the correct shape passes"
    else
        echo "  FAIL  the baseline workflow does not pass; every other case is evidence of nothing" >&2
        FASTCACHED_REQUIRED_CONTEXTS_FILE="$requiredFile" bash "$0" --workflow "${scratch}/wf.yml" 2>&1 | sed 's/^/        /' >&2 || true
        status=1
    fi

    Generate "${scratch}/wf.yml" equality yes ungated required
    Case "rule A: a classifier read with == 'true' (a failed classifier then SKIPS the job)" want-fail

    Generate "${scratch}/wf.yml" odd yes ungated required
    Case "rule A: a classifier read in a shape this check cannot vouch for" want-fail

    Generate "${scratch}/wf.yml" safe no ungated required
    Case "rule B: a job consulting the classifier without !cancelled()" want-fail

    Generate "${scratch}/wf.yml" safe yes gated required
    Case "rule C: the doc-subject step gated on the classifier (#687)" want-fail

    Generate "${scratch}/wf.yml" safe yes absent required
    Case "rule C: no doc-subject step at all (a COMMENT naming the runner does not count)" want-fail

    Generate "${scratch}/wf.yml" safe yes ungated unrequired
    Case "rule C: the doc-subject step in a job that is not a required context (#684)" want-fail

    # And the direction that rots silently: an empty required table would make
    # every job look unrequired, so rule C must refuse rather than vouch.
    : > "${scratch}/empty.sh"
    cp "$correct" "${scratch}/wf.yml"
    local out got=0
    out="$(FASTCACHED_REQUIRED_CONTEXTS_FILE="${scratch}/empty.sh" bash "$0" --workflow "${scratch}/wf.yml" 2>&1)" || got=$?
    if [[ "$got" -ne 0 ]]; then
        echo "  ok    (want-fail) rule C: an empty required-context table is REFUSED, not read as 'nothing is required'"
    else
        echo "  FAIL  (want-fail, exit 0) an empty required-context table vouched for the doc step" >&2
        printf '%s\n' "$out" | sed 's/^/        /' >&2
        status=1
    fi

    cases=$((cases + 1))

    # Rule D. The correct workflow above carries `trim=none`, so the baseline
    # already covers "nothing to vouch for"; these are the two states that
    # differ.
    Generate "${scratch}/wf.yml" safe yes ungated required canonical all
    Case "rule D: a release-gating job trimmed off branch pushes, spelled canonically" want-pass

    Generate "${scratch}/wf.yml" safe yes ungated required tagless all
    Case "rule D: a trim reading only \`event_name != 'push'\` -- a tag push IS a push, so the release job would silently never run" want-fail

    Generate "${scratch}/wf.yml" safe yes ungated required canonical nothing
    Case "rule D: an unreadable/empty \`release.needs\` is REFUSED, not read as 'no job gates the release'" want-fail

    if [[ "$status" -ne 0 ]]; then
        echo "check-gated-jobs: self-test FAILED after $cases case(s)" >&2
        exit 1
    fi
    echo "check-gated-jobs: self-test passed ($cases cases)"
}

# ---------------------------------------------------------------------------
workflowArg=".github/workflows/build.yml"
selfTest="no"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --workflow)  [[ $# -ge 2 ]] || { echo "--workflow needs a file" >&2; exit 2; }
                     workflowArg="$2"; shift 2 ;;
        --self-test) selfTest="yes"; shift ;;
        *)           echo "check-gated-jobs: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [[ "$selfTest" == "yes" ]]; then
    SelfTest
    exit 0
fi

Main "$workflowArg"
