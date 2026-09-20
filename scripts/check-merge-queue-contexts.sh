#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Every REQUIRED status check must be able to report a conclusion inside a
# `merge_group` event.
#
# ## Why this is a test and not a review note
#
# A merge queue dispatches `merge_group`. A workflow that does not listen for it
# produces no check run at all for that event, so a required context never
# reports — and a queued pull request does not fail, it **sits there**. That is
# the identical never-arrives failure as a `paths-ignore` filter on a required
# check (`.agent/rules/build-and-toolchain.md`), reached through a different
# door, and it presents as the feature working right up until the first pull
# request enters the queue.
#
# It is also invisible to every other check in this repository: the workflows are
# valid YAML, every job is correct, and nothing anywhere disagrees. The only
# observable is a queue that stalls.
#
# So the acceptance criterion for #351 is asserted rather than demonstrated once:
# for each required context, the workflow that produces it triggers on
# `merge_group`, the job still exists under that name, and its job-level `if:`
# does not exclude the event.
#
# ## What this cannot check, stated rather than left as an apparent omission
#
# The required-context LIST below is a copy of the `default-master` ruleset's
# `required_status_checks`, and nothing offline can verify a copy of a server-side
# setting. Read the live list with:
#
#   gh api repos/LASTRADA-Software/fastcached/rulesets \
#     --jq '.[] | select(.name == "default-master") | .id'
#   gh api repos/LASTRADA-Software/fastcached/rulesets/<id> \
#     --jq '.rules[] | select(.type == "required_status_checks")
#           | .parameters.required_status_checks[].context'
#
# A context added to the ruleset and not added here is not caught. A context here
# that no longer resolves to a job IS caught, which is the direction a rename
# breaks in. And a context added HERE and not in the ruleset is not caught
# either -- the direction a PROMOTION breaks in, and the costly one: this table
# is what `ci-merge-group-report.sh` reads to decide whether a merge-group
# failure still needs reporting, so while it leads the ruleset that leg's
# failure is classified *already surfaced* by a queue that never held on it.
# The trade-off is argued at `RequiredContexts` below; re-read the live list
# above before concluding a promotion is in force.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# ---------------------------------------------------------------------------
# The two helpers the FOURTH door needs, defined HERE rather than beside their
# three siblings further down, because `--self-test` selects its own subject with
# them and a self-test cannot call a function defined below it. Finding that out
# cost nothing only because the case refuses when it can stage no defect; a
# selector that had silently matched nothing would have reported a passing tree.
#
# The FOURTH door, and the only one where the context REPORTS and is then taken
# back: a run cancelled by its own successor at the SAME head SHA (#761, #820).
#
# The first three doors are a context that never arrives -- no `merge_group:`
# trigger, a `branches:` filter, a job whose `if:` excludes the event. This one
# arrives, gets computed, and is discarded: the required context reads
# `completed/cancelled`, which is neither pass nor fail, so the pull request is
# blocked until a later run reports. Measured on `pr-labels.yml`, the 100 most
# recent runs to 2026-09-10T20:36Z: **14 cancelled**, each superseded by a
# same-branch run seconds to minutes later, and in three of four inspected by hand
# the labelling job had already SUCCEEDED before the verdict was thrown away.
#
# ## The rule is not "never cancel", and getting that wrong would refuse a
# ## correct workflow
#
# `build.yml` sets `cancel-in-progress: true` and is right to. It triggers on
# `push` and a bare `pull_request:` (`opened`, `synchronize`, `reopened`), so a
# superseding run carries a NEW head SHA and the contexts it cancels belong to a
# commit no ruleset gates on any more.
#
# What makes it a defect is a trigger that fires WITHOUT changing the head SHA --
# `edited`, `labeled`, `unlabeled`. Then both runs answer for one commit, and
# killing the first destroys that commit's only verdict. So the two halves are
# read out of the file and ANDed; nothing here names a workflow, and `build.yml`
# is exempt because the rule does not reach it rather than because a list says so.
#
# ## What this does NOT cover
#
# A `concurrency:` block on a JOB rather than on the workflow. The refusal is
# therefore worded as "this workflow", and a job-level setting would pass -- said
# plainly, because a check silent about a case reads exactly like one that
# cleared it. No workflow here has one today.

# ---------------------------------------------------------------------------
# Every fact this check reads out of a workflow file, from ONE walk.
#
# Five awk programs used to live here, four of them structure readers spelling
# `build.yml`'s two-space indentation as literal column counts -- so each was
# right for the file as written today and answered nothing about a file whose
# jobs sit at another depth, which is legal YAML. They are now key PATHS over
# `scripts/lib/workflow-walk.awk` (#1456), which makes the depth the walk's
# problem; `scripts/check-merge-queue-contexts.awk` holds this check's half and
# documents the records.
#
# A missing awk program is refused BY NAME. A reader that ran over no program is
# not a clean tree, and awk's own complaint names a file rather than the check
# whose verdict just became meaningless.
FastCachedTab="$(printf '\t')"
FastCachedWalkAwk="$(dirname "${BASH_SOURCE[0]}")/lib/workflow-walk.awk"
FastCachedFactsAwk="$(dirname "${BASH_SOURCE[0]}")/check-merge-queue-contexts.awk"
for awkProgram in "$FastCachedWalkAwk" "$FastCachedFactsAwk"; do
    if [ ! -f "$awkProgram" ]; then
        echo "check-merge-queue-contexts: missing awk program ${awkProgram}; no workflow was read," >&2
        echo "  so this is a refusal and not a clean run. It is expected beside this script, and a" >&2
        echo "  staged copy of this check must stage it too." >&2
        exit 2
    fi
done

# Every record the walk yields for the workflow @p 1. The file is passed TWICE,
# which is the walk's convention.
WorkflowRecords() {
    awk -f "$FastCachedWalkAwk" -f "$FastCachedFactsAwk" "$1" "$1"
}

# The rows of kind @p 2 for the workflow @p 1, with the kind column dropped.
# `grep | cut` and never `grep -q` or `| head`: a pipe into a consumer that exits
# early is a false negative under `pipefail` on its SUCCESS path, and `cut` exits
# early for nothing.
WorkflowFacts() {
    local records
    records="$(WorkflowRecords "$1")"
    grep -F -- "$2${FastCachedTab}" <<< "$records" | cut -f2- || true
}

# Whether the workflow @p 1 carries the key at path @p 2. A HERESTRING, which is
# not a pipe, so `grep -q` is safe here where `producer | grep -q` would not be.
WorkflowHasKey() {
    local records
    records="$(WorkflowRecords "$1")"
    grep -Fq -- "ON${FastCachedTab}$2${FastCachedTab}" <<< "$records"
}

# Which same-SHA re-trigger types this workflow's `on:` block names, if any.
#
# Comments are stripped before matching: a COMMENT is not a call site, and both
# workflows this rule fires on discuss these very words in prose beside the
# settings it reads.
SameShaRetriggerTypes() {
    local hits
    hits="$(WorkflowFacts "$1" RETRIGGER)"
    [ -n "${hits}" ] || return 1
    printf '%s\n' "${hits}"
}

# Does this workflow cancel a run that is already in progress?
# @return 0 and the line number(s) when it does.
CancelsRunsInProgress() {
    local hits
    hits="$(WorkflowFacts "$1" CANCEL)"
    [ -n "${hits}" ] || return 1
    printf '%s\n' "${hits}"
}

# ---------------------------------------------------------------------------
# `--self-test` drives the verdict rules against SYNTHESISED trees, in both
# directions. A guard nobody has watched refuse is not a guard, and the arm that
# matters most -- a leg added to a workflow with no row -- had never been seen to
# fire, because on a clean tree there is no such leg to fire on.
#
# It runs a COPY of this script against a copy of the workflow tree rather than
# reimplementing anything: the script cds to its own parent's parent, so a copy
# at `$scratch/scripts/` reads `$scratch/.github/workflows/`, and there is no
# second implementation to drift. Each case asserts its tree DIFFERS from the
# baseline, so an injection that stopped injecting is a failure rather than a
# pass, and `bash <path>` is never a bare path. Not because of the mode -- every
# tracked script with a shebang is 100755 and `ctest -R script-modes` enforces it
# (#720, #1033) -- but because a bare invocation that fails to START for any
# reason runs nothing, and every negative case then passes because the SHELL
# refused rather than because the rule fired.
if [[ "${1:-}" == "--self-test" ]]; then
    scratch="$(mktemp -d)" || { echo "cannot create a scratch directory" >&2; exit 2; }
    # shellcheck disable=SC2064  # expand $scratch now, not at trap time
    trap "rm -rf '$scratch'" EXIT
    selfTestStatus=0
    selfTestCases=0
    me="$(pwd)/scripts/$(basename "${BASH_SOURCE[0]}")"

    # A pristine copy of the tree this check reads, remade for every case.
    Stage() {
        rm -rf "$scratch/tree"
        mkdir -p "$scratch/tree/scripts/lib" "$scratch/tree/.github/workflows"
        cp "$me" "$scratch/tree/scripts/"
        # The awk programs as well, or the copy refuses by name. That refusal is
        # the point: without it the staged check would read no program at all.
        cp "$FastCachedWalkAwk" "$scratch/tree/scripts/lib/"
        cp "$FastCachedFactsAwk" "$scratch/tree/scripts/"
        cp .github/workflows/*.yml "$scratch/tree/.github/workflows/"
    }

    # @param 3 optional text the output must hold. A `want-fail` alone cannot tell
    # the rule a case is about from any other refusal the same tree trips.
    SelfTestCase() {
        local what="$1" want="$2" mustSay="${3:-}" out got=0
        selfTestCases=$((selfTestCases + 1))
        out="$(bash "$scratch/tree/scripts/$(basename "$me")" 2>&1)" || got=$?
        if { [[ "$want" == "want-pass" && "$got" -eq 0 ]] || [[ "$want" == "want-fail" && "$got" -ne 0 ]]; } \
            && [[ -z "$mustSay" || "$out" == *"$mustSay"* ]]; then
            echo "  ok    ($want) $what"
        else
            echo "  FAIL  ($want, exit $got${mustSay:+, must say '$mustSay'}) $what" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            selfTestStatus=1
        fi
    }

    # A synthetic workflow holding one matrix job, staged beside the real ones,
    # and NotBinding rows for exactly the contexts named -- so a case passes only
    # when the check derives THAT set: a context the model produced and no row
    # names is refused as having no verdict, and a row naming a context the model
    # did not produce is refused as stale. Both directions of one set, in one run.
    #
    # The rows go in at the opening line of `NonBindingContexts`, and through
    # ENVIRON rather than `-v`, which may not carry a newline on BSD awk.
    # @param 1 the job's `strategy:` block and everything after `runs-on:`, as YAML
    # @param 2... the contexts to give rows
    StageMatrix() {
        local body="$1" rows="" context
        shift
        {
            printf 'on: push\njobs:\n  fixture:\n'
            printf '%s\n' "$body"
        } > "$scratch/tree/.github/workflows/matrix-fixture.yml"
        for context in ${1+"$@"}; do
            rows="${rows}    \"${context}|NotBinding|a matrix fixture row\""$'\n'
        done
        cp "$scratch/tree/scripts/$(basename "$me")" "$scratch/before"
        MatrixRows="$rows" awk '
            { print }
            /^NonBindingContexts=\($/ && !done { printf "%s", ENVIRON["MatrixRows"]; done = 1 }
            END { if (!done) exit 3 }
        ' "$scratch/before" > "$scratch/tree/scripts/$(basename "$me")" \
            || { echo "  FAIL  the NonBindingContexts anchor is missing; no matrix case can stage its rows" >&2; selfTestStatus=1; return 1; }
        if [[ -n "$rows" ]]; then
            Injected "matrix fixture rows" "$scratch/tree/scripts/$(basename "$me")" "$scratch/before" || return 1
        fi
    }

    # Assert an injection actually injected. An edit that matches no anchor
    # changes nothing and reports success, which is how three tools in this tree
    # reported work they did not do.
    Injected() {
        local what="$1" file="$2" before="$3"
        if cmp -s "$before" "$file"; then
            echo "  FAIL  '$what' changed nothing; the case stages no defect" >&2
            selfTestStatus=1
            return 1
        fi
    }

    # The baseline, and it is not decoration: every negative case below is
    # evidence only if the unmodified tree passes.
    Stage
    SelfTestCase "the real tree, copied unmodified, passes" want-pass

    # The arm this whole table exists for.
    Stage
    cp "$scratch/tree/.github/workflows/build.yml" "$scratch/before"
    printf '  a-newly-added-leg:\n    name: "A newly added leg"\n    steps:\n      - run: true\n' \
        >> "$scratch/tree/.github/workflows/build.yml"
    Injected "an extra leg" "$scratch/tree/.github/workflows/build.yml" "$scratch/before" \
        && SelfTestCase "a leg added to a workflow with no verdict row is REFUSED (#408/#629 arriving again)" want-fail

    # The fourth door: a same-SHA re-trigger plus `cancel-in-progress: true`.
    #
    # Anchored on the SETTING and not on a workflow name, and the tree is chosen
    # by the RULE rather than named: whichever workflow already carries a
    # same-SHA trigger type is the one where flipping the setting stages the
    # defect. Naming `pr-labels.yml` here would be a scheduled failure the day
    # that file's triggers change, which is the anchoring mistake the case below
    # this one records paying for twice.
    Stage
    victim=""
    for candidate in "$scratch"/tree/.github/workflows/*.yml; do
        SameShaRetriggerTypes "$candidate" >/dev/null || continue
        CancelsRunsInProgress "$candidate" >/dev/null && continue
        # AND it must carry the line this case is about to flip. The two conditions
        # above are satisfied by a workflow with NO concurrency block at all, and the
        # `sed` below then changes nothing -- the case stages no defect and reports
        # that, which is honest but is a red over a tree with nothing wrong with it.
        #
        # Reached for real: `issue-triage.yml` grew a `labeled` trigger (#1563), which
        # IS a same-SHA re-trigger type, and it sorts before `pr-labels.yml`. It has no
        # concurrency group and needs none -- it produces no check run on a pull
        # request at all, which is what its `NonBindingContexts` row already says -- so
        # the fourth door cannot open there and there was nothing to fix in the
        # workflow. What was incomplete was this predicate: it selected on the RULE and
        # not on what the mutation needs.
        grep -q '^  cancel-in-progress: false$' "$candidate" || continue
        victim="$candidate"
        break
    done
    if [[ -z "$victim" ]]; then
        echo "  FAIL  no workflow names a same-SHA re-trigger type without already cancelling; the fourth-door case can stage nothing" >&2
        selfTestStatus=1
    else
        cp "$victim" "$scratch/before"
        sed -i.bak 's/^  cancel-in-progress: false$/  cancel-in-progress: true/' "$victim"
        rm -f "$victim.bak"
        Injected "cancel-in-progress flipped on" "$victim" "$scratch/before" \
            && SelfTestCase "cancelling a run in progress on a same-SHA trigger is REFUSED (#761/#820)" want-fail
    fi

    # And the direction that keeps the rule from being "never cancel": a workflow
    # that cancels but names no same-SHA trigger type must still PASS. Without
    # this, a check that refused every `cancel-in-progress: true` would satisfy
    # the case above for the wrong reason and take `build.yml` red.
    Stage
    control=""
    for candidate in "$scratch"/tree/.github/workflows/*.yml; do
        CancelsRunsInProgress "$candidate" >/dev/null || continue
        SameShaRetriggerTypes "$candidate" >/dev/null && continue
        control="$candidate"
        break
    done
    if [[ -z "$control" ]]; then
        echo "  FAIL  no workflow cancels without a same-SHA trigger, so the rule's exempt direction is untested" >&2
        selfTestStatus=1
    else
        echo "  ok    (control) $(basename "$control") cancels in progress and is correctly NOT refused"
        selfTestCases=$((selfTestCases + 1))
        SelfTestCase "a workflow that cancels but has no same-SHA trigger still passes" want-pass
    fi

    # A row naming a context nothing produces any more.
    Stage
    cp "$scratch/tree/.github/workflows/build.yml" "$scratch/before"
    sed -i.bak 's/^    name: "Docker image"$/    name: "Container image"/' \
        "$scratch/tree/.github/workflows/build.yml"
    rm -f "$scratch/tree/.github/workflows/build.yml.bak"
    Injected "a renamed job" "$scratch/tree/.github/workflows/build.yml" "$scratch/before" \
        && SelfTestCase "a NonBindingContexts row naming a context no job produces is REFUSED" want-fail

    # One context claiming both verdicts at once.
    #
    # ## Why nothing here names a context
    #
    # This case used to anchor on the literal row `"clang-tsan|Undecided|#408"`,
    # and promoting that context to `RequiredContexts` DELETED the anchor -- so
    # the case died on its own `assert` instead of reporting anything about the
    # rule. The assertion was working exactly as designed; the defect is which
    # line it was anchored on. A row's membership in these tables is the one
    # thing this file exists to change, so ANY named row is a scheduled failure
    # and re-anchoring on a different row only relocates it.
    #
    # So both ends are DERIVED. The contradictory row takes its context from
    # whatever `RequiredContexts` lists first, and is inserted at the opening
    # line of `NonBindingContexts`. Neither depends on a value; both fail loudly
    # if a table is renamed or emptied, which is a change that SHOULD stop the
    # self-test rather than one that should quietly disarm it.
    Stage
    cp "$scratch/tree/scripts/$(basename "$me")" "$scratch/before"
    python3 - "$scratch/tree/scripts/$(basename "$me")" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
first = re.search(r'^RequiredContexts=\(\n    "([^"|]+)\|', s, re.M)
assert first, "self-test anchor is missing: no first row of RequiredContexts to contradict"
anchor = 'NonBindingContexts=(\n'
assert s.count(anchor) == 1, "self-test anchor is missing or not unique"
open(p, 'w').write(s.replace(
    anchor, anchor + '    "%s|NotBinding|a deliberately contradictory row"\n' % first.group(1)))
PY
    Injected "a doubly-classified context" "$scratch/tree/scripts/$(basename "$me")" "$scratch/before" \
        && SelfTestCase "a context in BOTH tables is REFUSED; required and non-binding are opposite claims" want-fail

    # A verdict spelling this check does not recognise, and an Undecided row
    # with no issue -- the two ways the three spellings collapse back into one.
    #
    # Anchored on the SHAPE of an `Undecided` row rather than on a named one,
    # for the reason the case above records: this one anchored on
    # `"Docker image|Undecided|#408"`, and that row's issue number moved in the
    # same change that promoted the sanitizer legs. The corrupted row keeps its
    # own context, so it stays a context some job produces and the case fails
    # for the VERDICT rather than for naming a context nothing emits -- inside a
    # `want-fail` assertion those two are indistinguishable.
    #
    # The two defects are the two arms, and they have to differ in the VERDICT
    # field or the second one is a duplicate of the first: both used to begin
    # `Undecided|`, so both landed on *names no issue* and the `*)` arm -- the
    # one that refuses a spelling this check does not recognise -- had never
    # been seen to fire. Verified by reading the refusal each case produces, not
    # by the exit status, because `want-fail` cannot tell two arms apart.
    for defect in "Probably fine|it is slow" "Undecided|soon"; do
        Stage
        cp "$scratch/tree/scripts/$(basename "$me")" "$scratch/before"
        python3 - "$scratch/tree/scripts/$(basename "$me")" "$defect" <<'PY'
import re, sys
p, defect = sys.argv[1], sys.argv[2]
s = open(p).read()
row = re.compile(r'^    "([^"|]+)\|Undecided\|#[0-9]+"$', re.M)
found = row.search(s)
assert found, "self-test anchor is missing: no `Undecided` row carrying an issue to corrupt"
# A lambda and not a template string: `re.sub` reads backslashes and `\1` in a
# replacement, and the defect text comes from the loop above.
open(p, 'w').write(row.sub(lambda m: '    "%s|%s"' % (m.group(1), defect), s, count=1))
PY
        Injected "verdict '$defect'" "$scratch/tree/scripts/$(basename "$me")" "$scratch/before" \
            && SelfTestCase "a row spelled '$defect' is REFUSED rather than read as a decision" want-fail
    done

    # A job MATRIX, read through the shared walk's model of one (#1432). Each case
    # names the exact set of contexts it expects, so a model that produced one too
    # many or one too few fails it -- see `StageMatrix`.
    #
    # #1432's shape: the include row would overwrite the ORIGINAL `runner`, so it
    # fits no base combination and is a third leg; `suffix`, which only that row
    # carries, expands EMPTY on the other two, so their names do not move.
    ArmShape='    name: "Fixture-${{ matrix.preset }}${{ matrix.suffix }}"
    runs-on: ${{ matrix.runner }}
    strategy:
      matrix:
        preset: [clang-release, gcc-release]
        runner: [ubuntu-24.04]
        include:
          - preset: gcc-release
            runner: ubuntu-24.04-arm
            suffix: "-arm64"
    steps:
      - run: true'
    Stage
    StageMatrix "$ArmShape" Fixture-clang-release Fixture-gcc-release Fixture-gcc-release-arm64 \
        && SelfTestCase "an include row that fits no base combination is a context of its own, and a key only it carries expands EMPTY on the others" want-pass

    Stage
    StageMatrix "$ArmShape" Fixture-clang-release Fixture-gcc-release \
        && SelfTestCase "the same matrix with no row for its third leg is REFUSED -- the leg is produced, not merged away" \
            want-fail "'Fixture-gcc-release-arm64' (from .github/workflows/matrix-fixture.yml) carries no verdict"

    # The other arm of the rule: a row that overwrites no ORIGINAL value MERGES.
    # Appended instead, the first row would add `Fixture-gcc-release` beside
    # `Fixture-gcc-release-merged`, and the second a context named `Fixture-`.
    Stage
    StageMatrix '    name: "Fixture-${{ matrix.preset }}${{ matrix.suffix }}"
    runs-on: ubuntu-24.04
    strategy:
      matrix:
        preset: [clang-release, gcc-release]
        include:
          - preset: gcc-release
            suffix: "-merged"
          - note: every leg
    steps:
      - run: true' Fixture-clang-release Fixture-gcc-release-merged \
        && SelfTestCase "an include row matching an axis value, and one naming no axis at all, MERGE into the combinations they fit" want-pass

    Stage
    StageMatrix '    name: "Fixture-${{ matrix.preset }}"
    runs-on: ubuntu-24.04
    strategy:
      matrix: ${{ fromJSON(needs.plan.outputs.matrix) }}
    steps:
      - run: true' \
        && SelfTestCase "a matrix the walk cannot read is REFUSED by name, never read as the job producing nothing" \
            want-fail "produces contexts this check cannot tell: its matrix cannot be read"

    Stage
    StageMatrix '    name: "Fixture"
    runs-on: ubuntu-24.04
    strategy:
      matrix:
        preset: [clang-release, gcc-release]
    steps:
      - run: true' \
        && SelfTestCase "a matrix job whose name carries no expression is REFUSED -- GitHub decorates it with each leg's values, so the literal name is no context" \
            want-fail "carries no expression"

    # And the empty read, which is the failure every table in this tree shares:
    # with no workflows nothing produces a context, so every row would look
    # accounted for and the check would vouch for a tree it never read.
    Stage
    rm -f "$scratch/tree/.github/workflows/"*.yml
    SelfTestCase "an empty workflow glob is REFUSED, not read as 'every context is accounted for'" want-fail

    # The verdict must come from the SHARED walk, and that is a different claim
    # from "the check refuses". Every case above would still pass if this script
    # had kept a private reader beside the shared one -- they assert the verdict,
    # not its source. So the NEUTER goes at `scripts/lib/workflow-walk.awk`: with
    # the job boundary disabled, no job record is ever completed, and a check
    # whose whole subject is which job produces which context must go red.
    #
    # Asserted as an injection too, because a sed expression that matches nothing
    # leaves a working library in place and the case then passes for the opposite
    # reason.
    Stage
    cp "$scratch/tree/scripts/lib/workflow-walk.awk" "$scratch/before"
    sed '/WorkflowFlushStep(); WorkflowFlushJob(); WorkflowResetJob(); WfJob = key/s/^/#/' \
        "$scratch/before" > "$scratch/tree/scripts/lib/workflow-walk.awk"
    Injected "a neutered job boundary in the shared walk" \
        "$scratch/tree/scripts/lib/workflow-walk.awk" "$scratch/before" \
        && SelfTestCase "neutering the shared walk's job boundary makes this check REFUSE, so its verdict comes from that walk" want-fail

    # And a staged tree that forgot the walk is a REFUSAL, never a clean run. The
    # window this closes is the one the previous increment met for real: a
    # self-test that stages a copy of a check and not the programs it reads runs a
    # reader over nothing, which produces no findings and reads exactly like a
    # clean tree.
    Stage
    rm -f "$scratch/tree/scripts/lib/workflow-walk.awk"
    SelfTestCase "a staged check with no shared walk beside it is REFUSED, not read as clean" want-fail

    if [[ "$selfTestStatus" -ne 0 ]]; then
        echo "check-merge-queue-contexts: self-test FAILED after $selfTestCases case(s)" >&2
        exit 1
    fi
    echo "check-merge-queue-contexts: self-test passed ($selfTestCases cases)"
    exit 0
fi

# ---------------------------------------------------------------------------
# The table: one row per required context, naming the workflow that produces it.
# A context whose job lives in a matrix is written as the EXPANDED name, because
# that is what GitHub reports and what the ruleset names.
#
# ## This table is the one place the required-context COUNT lives
#
# The run prints `all <n> required contexts ...` from `${#RequiredContexts[@]}`,
# and every other surface that needs the list READS it from here -- see
# `scripts/ci-merge-group-report.sh`, `scripts/check-gated-jobs.sh` and
# `scripts/check-merge-group-report.sh`. Nothing restates the set or the number
# in prose: point at this table, or at the line the run prints, and do not write
# either down again. `.agent/rules/build-and-toolchain.md` carries that rule and
# the sites it was written from.
#
# ## The docs-only `skipped` hazard is INHERITED here, not introduced
#
# On a docs-only pull request the scope classifier answers `code=false` and the
# compiler legs report `skipped`, which a required context is read as PASSING.
# That is already true of every required context whose JOB carries the scope gate
# rather than gating its STEPS -- derive the set rather than reading one from here,
# with `awk '/^  [A-Za-z0-9_-]+:$/{j=$0} /^    if:/{print j, $0}' .github/workflows/build.yml`
# and intersect it with the table below; naming one member would read as complete,
# which is the failure this whole header exists to refuse. It is deliberate: a
# matrix job must NOT be job-gated (its per-leg contexts would never exist), and a
# non-matrix one has nothing to gain from starting a runner to do nothing.
# Promoting `clang-asan-ubsan` and `clang-tsan` adds no new KIND of instance
# of it -- said out loud so the next reader does not meet it here and file it as
# a regression this change introduced (#629's acceptance, clause 4).
#
# THE MARKER BELOW IS LOAD-BEARING and `check-required-context-table.sh` enforces
# it. Four files in this tree declare `RequiredContexts=(` and three of them are
# self-test fixtures, so a reader that finds the array by searching for its name
# has a one-in-four chance of landing here -- and the 11-row fixture in
# `check-merge-group-report.sh` is a strict SUBSET whose every row is a real
# context, which is the shape that reports "11 of 11 passed" and trips nothing
# (#1360). This is the copy everything reads; the marker says so at the
# declaration, where a searcher arrives.
# required-context-table: live
RequiredContexts=(
    "Windows-cl-release|.github/workflows/build.yml"
    "Windows-clangcl-release|.github/workflows/build.yml"
    "Linux-clang-release|.github/workflows/build.yml"
    "Linux-gcc-release|.github/workflows/build.yml"
    "macOS-clang-release|.github/workflows/build.yml"
    "clang-tidy|.github/workflows/build.yml"
    "clang-asan-ubsan|.github/workflows/build.yml"
    "clang-tsan|.github/workflows/build.yml"
    "sccache smoke (memcached text)|.github/workflows/build.yml"
    "sccache smoke (memcached binary)|.github/workflows/build.yml"
    "sccache smoke (redis RESP2)|.github/workflows/build.yml"
    "Check C++ style|.github/workflows/build.yml"
    "Require a type label|.github/workflows/pr-labels.yml"
    # Split out of `Check C++ style` by #1045. It reads the pull request BODY, so its
    # only remedy is a body edit -- and `build.yml` does not fire on `edited`, which
    # made the refusal unclearable by the one action it advised. Its own workflow can
    # carry `edited` because re-running it costs a runner start rather than the matrix.
    #
    # Required for the reason it was a step in a required job before: reporting without
    # gating is #684. Its `merge_group:` row is what keeps this promotion from being the
    # never-arrives failure -- a new required context with no queue leg leaves a queued
    # pull request waiting on a check that never reports.
    "Check the PR body's closing keywords|.github/workflows/pr-body.yml"
)

# ---------------------------------------------------------------------------
# The binding table: every OTHER context these workflows can produce, and why it
# does not gate a merge.
#
# ## The gap this closes
#
# #408 recorded that `clang-tsan` runs, is proven live by its own canary, and
# could not block a merge -- and said the answer was not to add one row to the
# ruleset, because "the same question applies to every other non-required leg"
# and deciding one in isolation leaves the gap in seven places. #629 was the
# same ticket for `clang-asan-ubsan`, the project's ENTIRE sanitizer test run.
#
# Both are now `Binding`. What settled them was a MEASUREMENT rather than the
# cost argument their own bodies carried, which the measurement FALSIFIED:
# neither leg is on the merge queue's critical path, and the slowest leg in the
# workflow was already required. #629 carries the numbers, the window they were
# taken over and the contention they were taken under -- read them there.
#
# What this table records without a ruleset decision is which legs are binding
# and why -- so that a leg nobody has decided about is DISTINGUISHABLE from one
# somebody deliberately left unrequired. Without it they look identical: absent
# from `RequiredContexts` and absent from everywhere else.
#
# The `Binding` column remains a copy of a server-side setting, per this file's
# header, and the ruleset is still the authority. A promotion is not in force
# until an administrator adds the row there.
#
# ## The two copies were out of step, in the measured direction
#
# The ruleset went FIRST: an administrator added both rows on 2026-09-05, while
# the branch carrying this table change was still open. So the blind spot this
# file's header states -- *a context added to the ruleset and not added here is
# not caught* -- happened for real rather than hypothetically, and it is worth
# recording which cost that bought, because the two orderings have OPPOSITE ones
# and neither is free.
#
#   Ruleset first (what happened). `ci-merge-group-report.sh` READS this table.
#   Between the flip and this landing, a failing sanitizer leg inside a merge
#   group would have been classified unrequired-and-unreported and had a #684
#   issue filed for a failure the queue had already ejected the pull request
#   over -- the reporter creating the bug it was written to prevent. #629's
#   acceptance predicted exactly this, which is why it asks for the ruleset row
#   and the table row in ONE change.
#
#   Table first. A failure of either leg inside a merge group would be
#   classified *already surfaced* by a queue that never held on it: #684's
#   silence, narrowed to two jobs and to the window.
#
# Both windows are one administrator action wide, and #629 measured 132
# executions of the two jobs across the 69 most recent `Build` runs with zero
# failures -- so neither was likely to fire. That is luck about the exposure,
# not an argument that the ordering does not matter. The standing instruction is
# the `gh api` call in this file's header: **re-read the live list rather than
# inferring it from this table**, in either direction.
#
# ## Three spellings, because two collapse the distinction
#
# This is `Protocol/SurfaceRefusal.hpp`'s idiom, one layer out. There a refusal
# is `Refuse` (a rise means something), `RefuseWithoutCounter` (a rise would
# mean nothing, and why) or `RefuseUntriaged` (nobody has decided, and which
# issue will) -- because "deliberately uncounted" must not be spelled like
# "forgot". The same three states exist here:
#
#   Binding      membership in `RequiredContexts` above; the ruleset gates it.
#   NotBinding   a considered decision that this must not block a merge, with
#                the reason and, where one exists, the issue that recorded it.
#   Undecided    nobody has decided. Carries the issue that will, and the run
#                TALLIES these per issue -- which is the only thing that keeps
#                `Undecided` from becoming a synonym for `forgot`.
#
# ## And the completeness assertion, which is the load-bearing half
#
# Every context every workflow produces must appear in exactly ONE of the two
# tables. A leg added to a workflow with no row is REFUSED, because today it
# joins as unrequired with nobody told -- exactly #492's shape, a list that is
# exact about what it knows and silent about what it does not. The workflow set
# is a GLOB and never a file list, for the same reason.
#
# Note this asserts nothing about whether a verdict is RIGHT; it asserts that
# one was reached and written down. The ruleset remains the authority on
# `Binding`, and the caveat above about a copy of a server-side setting applies
# to that column unchanged.
NonBindingContexts=(
    # Decided, with the record that decided it.
    "clang-tidy-windows|Undecided|#874: it ships UNREQUIRED and CONDITIONALLY. The leg works -- it parsed 514 units on Windows and found real diagnostics -- but the tree has findings it cannot pass today, and a gate that is reliably red teaches everyone to ignore it, which disarms it as thoroughly as deleting it (the argument in tsan-canary-rate.sh, harder at 100% than at a few percent). It becomes REQUIRED when it has been green on master twice consecutively AND #874 is decided: #874 is the MSVC-STL category, which unlike the other two does not converge, so requiring this leg before that decision is requiring a gate that may never settle"
    "Package (Linux .deb/.rpm)|NotBinding|#684: the packaging jobs are slow and a packaging failure should not block ordinary work. That ticket calls the reasoning sound and proposes REPORTING the failure instead, which merge-group-report.yml now does"
    "Package (macOS .pkg)|NotBinding|#684, as above -- and this is the job whose silent failure #684 was filed about"
    "Package (Windows .msi)|NotBinding|#684, as above"
    "Report a failure the merge queue did not gate on|NotBinding|#684 is explicit that this must gate NOTHING: a notifier that turns unrequired jobs into gates by the back door defeats the reason they are unrequired"
    "Decide what this change can affect|NotBinding|check-gated-jobs.sh rule A exists BECAUSE this job can fail without gating -- the fix for a dead classifier is that every reader compares != 'false', not that the classifier becomes a gate"
    "Draft GitHub release|NotBinding|tag-only (if: startsWith(github.ref, 'refs/tags/v')). A pull request can never produce this context, so requiring it would leave every branch waiting on a check that never reports"
    "Maintain status/needs-triage|NotBinding|triggered by an issues event, so it produces no check run on a pull request at all. Renamed from 'Mark as needing triage' when the job grew a remover (#1563) -- a workflow job's name IS the context string, so renaming one without moving its row here leaves the new context with no verdict AND a stale row excusing nothing"
    "Apply derivable labels|NotBinding|a pull_request_target job, and it MUTATES rather than checks. Its outcome is enforced by the required Require a type label"
    "Deploy to Pages|NotBinding|a deployment, not a check, and it runs only after a push to master has already merged"
    "Build site (strict)|NotBinding|docs.yml is not reachable from a code change; a docs break is caught on the push that lands it and blocks no unrelated work"
    "Check the release gate covers every job|NotBinding|it guards a TAG run, and a tag is not a merge. A release that lost a gating job is caught by this job on the tag, which is the event that can act on it"
    "Linux-gcc-release-arm64|NotBinding|#1432: the arm64 legs exist to compile and run the ARMv8 SHA-256 engine and its detection on GitHub's partner arm64 images. A required context there would make every merge depend on a runner pool this repository cannot fix, and an unrequired failure is still reported by the notifier (merge-group-report.yml)"
    "Windows-cl-release-arm64|NotBinding|#1432, as above"

    # Nobody has decided. Tallied per issue on every run.
    #
    # These six used to cite #408, which the promotion above settles and the
    # pull request carrying it closes on merge.
    # An `Undecided` row naming a CLOSED issue is exactly the "forgot" state the
    # three spellings exist to keep distinguishable from a decision -- it still
    # reads as *somebody will settle this*, and nobody will. #829 is that
    # somebody. (The check asserts a row NAMES an issue; it cannot tell an open
    # one from a closed one, which is why this had to be caught by hand.)
    "Windows-cl-debug|Undecided|#829"
    "Code coverage|Undecided|#829"
    "compile-cache E2E (Linux)|Undecided|#829"
    "compile-cache E2E (Windows)|Undecided|#829"
    "fastcache-cc smoke (compile-cache 0xFC)|Undecided|#829"
    "Docker image|Undecided|#829"
)

problems=0
Fail() { echo "  FAIL: $*" >&2; problems=$((problems + 1)); }

# ---------------------------------------------------------------------------
# Does this workflow's `on:` block name `merge_group`?
TriggersOnMergeGroup() {
    WorkflowHasKey "$1" on/merge_group
}

# Every context this workflow can produce, one per line, as
# `context<TAB>jobKey<TAB>job-level if:`.
#
# The mapping is DERIVED rather than tabulated. A second hand-written list of
# which job produces which context would not be a cross-check, it would be a
# second thing to be wrong -- the same reasoning `check-tsan-scope.cmake` records
# for reading the gate's tag expression instead of restating it.
#
# One row per COMBINATION of the job's matrix, computed by the shared walk with
# GitHub's semantics (#1432): a matrix `include` row that fits no base
# combination is a leg of its own, and a key a leg lacks expands to nothing.
EmitJobContexts() {
    WorkflowFacts "$1" CONTEXT
}

# ---------------------------------------------------------------------------
# Does this workflow's `on:` block filter `pull_request` by base branch?
#
# The THIRD door to the never-arrives failure, after `paths-ignore` and a missing
# `merge_group:` row. A `branches:` filter under `pull_request:` means a pull
# request whose base is anything else produces no check run at all -- so every
# required context stays pending and the pull request sits at BLOCKED. That is
# every layer of a STACKED pull request but the bottom one, and it is invisible:
# the workflow is valid, the jobs are correct, and the only observable is a pull
# request waiting on CI nobody asked to run.
FiltersPullRequestBranches() {
    WorkflowHasKey "$1" on/pull_request/branches
}


# ---------------------------------------------------------------------------
# Every workflow named in the table must listen for the event.
#
# A read loop and not `mapfile`: that is bash 4+, and macOS still ships 3.2 as
# /bin/bash. This script is registered in the DEFAULT ctest set, so it runs on
# every platform CI builds -- and the constraint is invisible from the Linux box
# it was written on, where `local-gate.sh` would have run it. `scripts/coverage.sh`
# carries the same note for the same reason. Still process substitution rather than
# a pipeline, so a producer whose `grep` matches nothing cannot take the script
# down under `pipefail`.
workflows=()
while IFS= read -r line; do
    workflows+=("$line")
done < <(printf '%s\n' "${RequiredContexts[@]}" | cut -d'|' -f2 | sort -u)
for workflow in "${workflows[@]}"; do
    if [[ ! -f "$workflow" ]]; then
        Fail "$workflow does not exist, but the table says a required context comes from it"
        continue
    fi
    if TriggersOnMergeGroup "$workflow"; then
        echo "ok: $workflow triggers on merge_group"
    else
        Fail "$workflow has no \`merge_group:\` trigger, so every required context it produces would NEVER REPORT inside a merge queue -- a queued pull request would sit there rather than fail"
    fi

    if FiltersPullRequestBranches "$workflow"; then
        Fail "$workflow filters \`pull_request\` by base branch, so every required context it produces would NEVER REPORT on a pull request based on anything else -- which is every layer of a stacked pull request but the bottom one, and it presents as CI that has not started rather than as a failure"
    else
        echo "ok: $workflow does not filter pull_request by base branch"
    fi

    # Both halves, ANDed. Either alone is ordinary: cancelling is right when the
    # successor carries a new SHA, and a same-SHA trigger is fine when nothing
    # kills the run holding the verdict.
    if retrigger="$(SameShaRetriggerTypes "$workflow")"; then
        if cancels="$(CancelsRunsInProgress "$workflow")"; then
            Fail "$workflow triggers on $retrigger -- which fire WITHOUT changing the head SHA -- and sets \`cancel-in-progress: true\` (line $cancels). The superseding run then answers for the same commit as the run it kills, so until it finishes every required context this workflow produces reads \`cancelled\` on that commit: neither pass nor fail, and a blocked pull request. Measured on pr-labels.yml: 14 of the 100 most recent runs, with the work already done in three of four inspected. Set \`cancel-in-progress: false\`. This is NOT an argument against cancelling in general -- build.yml triggers on push and a bare \`pull_request:\`, so its superseding runs carry a new SHA and it is untouched by this rule."
        else
            echo "ok: $workflow triggers on $retrigger but does not cancel a run in progress, so a same-SHA verdict survives"
        fi
    else
        echo "ok: $workflow names no same-SHA re-trigger type, so a superseding run carries a new SHA"
    fi
done

# ---------------------------------------------------------------------------
# Every required context must still resolve to a job, and that job's own `if:`
# must not exclude the event.
for row in "${RequiredContexts[@]}"; do
    context="${row%%|*}"
    workflow="${row##*|}"
    [[ -f "$workflow" ]] || continue

    # A HERESTRING, not a pipeline: under `pipefail` any consumer that exits at its
    # first match kills the producer with SIGPIPE and the pipeline reports the
    # PRODUCER's status -- a false negative, and one that fires on the SUCCESS path,
    # because the earlier the match the more data is left queued.
    #
    # This comment used to cite that rule and vouch for the line below it, which was
    # `printf ... | awk '$1 == want { print; exit }'`. Swapping `grep -q` for an awk
    # `exit` changes the tool and keeps the defect: same early exit, same signal, same
    # false negative. The rule was known, quoted, and reintroduced on the next line --
    # so the remedy is the one #970 actually prescribes, which is not piping at all.
    #
    # Measured on this check, N=80 each, conditions stated because the rate is not a
    # property of the script alone -- scheduling decides the race:
    #
    #     piped, interleaved with a second copy      48 of 80 failed, all exit 141
    #     piped, under the herestring run's load       9 of 80 failed, all exit 141
    #     herestring, interleaved with a second copy   0 of 80
    #
    # The middle row is the control, and it is why the zero means anything: the same
    # unpatched script under the SAME load the fixed one saw still failed, so the zero
    # is the fix rather than a quiet machine. It is also why the conditions are stated
    # rather than a single percentage being quoted -- 48 and 9 are the same script, and
    # the ticket that filed this measured 29 of 80 again. The rate belongs to the
    # scheduling, and only the ZERO belongs to the code.
    #
    # 141 is 128+13, and neither 0 nor 1: the shape this repository already reads as
    # the instrument failing rather than the tree being bad. It died mid-loop with no
    # diagnostic, having printed its `ok:` lines up to that point, so the output read
    # like a check that had reached a verdict.
    produced="$(EmitJobContexts "$workflow")"
    match="$(awk -F'\t' -v want="$context" '$1 == want { print; exit }' <<< "$produced")"

    if [[ -z "$match" ]]; then
        Fail "no job in $workflow produces the required context '$context' -- a rename or a deleted job leaves it unreportable and the branch unmergeable"
        continue
    fi

    jobKey="$(printf '%s' "$match" | cut -f2)"
    ifExpr="$(printf '%s' "$match" | cut -f3)"

    # A heuristic, and stated as one: a job-level condition that names the
    # pull-request event without naming the queue's is almost certainly gating
    # itself off inside the queue. `needs.changes.outputs.code` names neither and
    # is correct -- the `changes` job answers `code=true` on `merge_group`.
    if [[ "$ifExpr" == *pull_request* && "$ifExpr" != *merge_group* ]]; then
        Fail "'$context' (job '$jobKey' in $workflow) has a job-level condition that names pull_request and not merge_group, so it would be skipped-by-omission inside a queue: if: $ifExpr"
        continue
    fi
    echo "ok: '$context' <- job '$jobKey' in $workflow"
done

# ---------------------------------------------------------------------------
# And the scope classifier must state the event rather than reach the right
# answer through its non-pull-request fallback. That fallback is correct today by
# ACCIDENT, and an accidentally-correct behaviour is one refactor away from being
# an accidentally-wrong one -- at which point every job gated on `code` would be
# skipped inside the queue, which is the same never-reports failure wearing a
# different hat.
if grep -q '^ *merge_group)' .github/workflows/build.yml; then
    echo "ok: the changes job classifies merge_group deliberately"
else
    Fail "the \`changes\` job in build.yml has no explicit \`merge_group)\` arm; it would fall through to the non-pull-request default and nothing would say so"
fi

# ---------------------------------------------------------------------------
# Every context every workflow produces carries a verdict, and exactly one.
#
# The workflow set is a GLOB, never a file list: a list is exact about the files
# it knows and silent about the ones it does not, and silence reads identically
# to complete coverage (#492). An empty glob is a REFUSAL rather than a vacuous
# pass, for the same reason the two empty-table reads elsewhere in this tree are.
allWorkflows=()
for candidate in .github/workflows/*.yml .github/workflows/*.yaml; do
    [[ -f "$candidate" ]] && allWorkflows+=("$candidate")
done
if [[ "${#allWorkflows[@]}" -eq 0 ]]; then
    # Refuse and STOP, rather than falling through to a loop over an empty
    # array. On bash before 4.4 -- which is macOS's /bin/bash, and this check is
    # in the default ctest set -- `"${arr[@]}"` on an empty array is an unbound
    # variable under `set -u`, so the fall-through would die with a shell error
    # instead of this sentence. The self-test's empty-glob case would still be
    # red, and red for a reason that has nothing to do with the rule: a
    # `want-fail` case cannot tell the rule firing from the shell refusing.
    #
    # That is #723 arriving through a different door. There the `want-fail` case
    # was satisfied by a MODE BIT refusing the script; here by `set -u` refusing
    # an expansion. The general form has two independent instances now: inside a
    # `want-fail` assertion, ANY failure to run is indistinguishable from the
    # rule firing.
    #
    # And the second half, which has to be written down because it is invisible:
    # NO TEST ON A MODERN BASH CAN CATCH A REGRESSION OF THIS. `BASH_COMPAT=3.2`
    # does not restore the old behaviour, so the guard is closed by construction
    # and a refactor that removes it would go green everywhere but macOS. Do not
    # collapse this arm back into the loop below.
    Fail "found no workflow files under .github/workflows/; with none, every context would look accounted for and this check would vouch for a tree it never read"
    echo "check-merge-queue-contexts: $problems problem(s); a merge queue would stall on these" >&2
    exit 1
fi

# `context<TAB>workflow` for every context the tree can produce.
#
# And every job whose contexts cannot be TOLD, which is refused rather than
# skipped: a job that produced no row here would be exactly the leg that joins
# unrequired with nobody told, arriving through the reader instead of the
# workflow. `scripts/check-merge-queue-contexts.awk` lists the ways it happens --
# a matrix the shared walk cannot read, a matrix job whose name GitHub decorates,
# a name still carrying an expression.
producedContexts=""
for workflow in "${allWorkflows[@]}"; do
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        producedContexts="${producedContexts}${line}	${workflow}
"
    done < <(EmitJobContexts "$workflow" | cut -f1)
    while IFS= read -r unknown; do
        [[ -n "$unknown" ]] || continue
        Fail "job '${unknown%%	*}' in $workflow produces contexts this check cannot tell: ${unknown#*	}. Refused rather than guessed, because both wrong guesses are silent -- a leg under a name no table holds joins with no verdict, and a required name nothing really produces reads as reported. Name the job so each combination's context is spelled with \`\${{ matrix.* }}\`, or write its matrix as block mappings of plain scalars; if the construct is needed, teach scripts/lib/workflow-walk.awk to read it, with a case."
    done < <(WorkflowFacts "$workflow" UNKNOWN)
done

# `${arr[@]+"${arr[@]}"}` and not a bare `"${arr[@]}"`, in both places the
# non-binding table is expanded. It cannot legitimately be empty here -- these
# workflows produce far more contexts than the ruleset requires, so most of them
# must carry a row -- and the completeness check below would catch an empty one
# anyway, since every non-required context would then fail for want of a row.
# (This sentence used to state both figures, and they went stale the first time a
# context was promoted -- which is why the counts are printed at the end of the
# run and written down nowhere.)
#
# The guard is for bash 3.2, which is macOS's /bin/bash and a platform
# this check runs on from the default ctest set: there `"${arr[@]}"` on an empty
# array is an UNBOUND VARIABLE under `set -u`, so the check would die with a
# shell error rather than print the sentence it exists to print.
#
# Same caveat as the empty-glob arm above, and for the same reason it is stated
# rather than left implicit: no test on a modern bash can catch a regression of
# this, because `BASH_COMPAT=3.2` does not restore the behaviour. A reviewer who
# reads `${arr[@]+"${arr[@]}"}` as noise and simplifies it back would see every
# check stay green here and only macOS go red.
requiredNames="$(printf '%s\n' "${RequiredContexts[@]}" | cut -d'|' -f1)"
nonBindingNames="$(printf '%s\n' ${NonBindingContexts[@]+"${NonBindingContexts[@]}"} | cut -d'|' -f1)"

# Direction one: a context the tree produces and neither table names. This is
# the arm that fires when a leg is ADDED, which is the whole point -- an
# unrequired leg joining silently is what #408 and #629 are both instances of.
accounted=0
while IFS=$'\t' read -r context workflow; do
    [[ -n "$context" ]] || continue
    inRequired=no
    inNonBinding=no
    grep -Fxq -- "$context" <<< "$requiredNames" && inRequired=yes
    grep -Fxq -- "$context" <<< "$nonBindingNames" && inNonBinding=yes

    if [[ "$inRequired" == "yes" && "$inNonBinding" == "yes" ]]; then
        Fail "'$context' is in BOTH tables. Required and non-binding are opposite claims; a context carrying both has no verdict at all."
    elif [[ "$inRequired" == "no" && "$inNonBinding" == "no" ]]; then
        Fail "'$context' (from $workflow) carries no verdict: it is neither in \`RequiredContexts\` nor in \`NonBindingContexts\`. A leg added with no row joins as UNREQUIRED with nobody told, which is #408 and #629 arriving again. Add a row: \`NotBinding\` with the reason, or \`Undecided\` with the issue that will settle it."
    else
        accounted=$((accounted + 1))
    fi
done <<< "$producedContexts"

# Direction two: a non-binding row naming a context nothing produces any more --
# the same rename failure the required table is already checked for above.
allProduced="$(printf '%s\n' "$producedContexts" | cut -f1)"
undecidedIssues=""
for row in ${NonBindingContexts[@]+"${NonBindingContexts[@]}"}; do
    context="${row%%|*}"
    rest="${row#*|}"
    verdict="${rest%%|*}"
    reason="${rest#*|}"

    grep -Fxq -- "$context" <<< "$allProduced" \
        || Fail "\`NonBindingContexts\` names '$context', which no job in any workflow produces. A stale row silently excuses nothing and hides the context that replaced it."

    case "$verdict" in
        NotBinding)
            [[ -n "$reason" && "$reason" != "$verdict" ]] \
                || Fail "'$context' is \`NotBinding\` with no reason. A verdict with no reason is indistinguishable from having forgotten, which is the distinction these three spellings exist to keep."
            ;;
        Undecided)
            case "$reason" in
                '#'[0-9]*) undecidedIssues="${undecidedIssues}${reason%% *}
" ;;
                *) Fail "'$context' is \`Undecided\` but names no issue ('$reason'). An undecided row with no issue IS the forgotten row it is meant to be distinguishable from." ;;
            esac
            ;;
        *)
            Fail "'$context' carries the verdict '$verdict', which is not one of \`NotBinding\` or \`Undecided\`. A spelling this check does not recognise is refused rather than read as a decision."
            ;;
    esac
done

# The tally. Printed on EVERY run, not only when it changes: an `Undecided` row
# is only safe because the total is visible, exactly as `RefuseUntriaged`'s is.
if [[ -n "$undecidedIssues" ]]; then
    echo "check-merge-queue-contexts: contexts nobody has decided about, by issue:"
    printf '%s' "$undecidedIssues" | sort | uniq -c | sed 's/^/  /'
fi

if [[ $problems -gt 0 ]]; then
    echo "check-merge-queue-contexts: $problems problem(s); a merge queue would stall on these" >&2
    exit 1
fi
echo "check-merge-queue-contexts: all ${#RequiredContexts[@]} required contexts can report inside a merge_group event"
echo "check-merge-queue-contexts: all $accounted context(s) across ${#allWorkflows[@]} workflow(s) carry exactly one verdict"
