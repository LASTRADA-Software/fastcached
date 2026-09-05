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
# pass, and `bash <path>` is never a bare path -- this file is mode 644, so a
# bare invocation would exit 126 and every negative case would pass because the
# SHELL refused.
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
        mkdir -p "$scratch/tree/scripts" "$scratch/tree/.github/workflows"
        cp "$me" "$scratch/tree/scripts/"
        cp .github/workflows/*.yml "$scratch/tree/.github/workflows/"
    }

    SelfTestCase() {
        local what="$1" want="$2" out got=0
        selfTestCases=$((selfTestCases + 1))
        out="$(bash "$scratch/tree/scripts/$(basename "$me")" 2>&1)" || got=$?
        if [[ "$want" == "want-pass" && "$got" -eq 0 ]] || [[ "$want" == "want-fail" && "$got" -ne 0 ]]; then
            echo "  ok    ($want) $what"
        else
            echo "  FAIL  ($want, exit $got) $what" >&2
            printf '%s\n' "$out" | sed 's/^/        /' >&2
            selfTestStatus=1
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

    # And the empty read, which is the failure every table in this tree shares:
    # with no workflows nothing produces a context, so every row would look
    # accounted for and the check would vouch for a tree it never read.
    Stage
    rm -f "$scratch/tree/.github/workflows/"*.yml
    SelfTestCase "an empty workflow glob is REFUSED, not read as 'every context is accounted for'" want-fail

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
    "Package (Linux .deb/.rpm)|NotBinding|#684: the packaging jobs are slow and a packaging failure should not block ordinary work. That ticket calls the reasoning sound and proposes REPORTING the failure instead, which merge-group-report.yml now does"
    "Package (macOS .pkg)|NotBinding|#684, as above -- and this is the job whose silent failure #684 was filed about"
    "Package (Windows .msi)|NotBinding|#684, as above"
    "Report a failure the merge queue did not gate on|NotBinding|#684 is explicit that this must gate NOTHING: a notifier that turns unrequired jobs into gates by the back door defeats the reason they are unrequired"
    "Decide what this change can affect|NotBinding|check-gated-jobs.sh rule A exists BECAUSE this job can fail without gating -- the fix for a dead classifier is that every reader compares != 'false', not that the classifier becomes a gate"
    "Draft GitHub release|NotBinding|tag-only (if: startsWith(github.ref, 'refs/tags/v')). A pull request can never produce this context, so requiring it would leave every branch waiting on a check that never reports"
    "Mark as needing triage|NotBinding|triggered by an issues event, so it produces no check run on a pull request at all"
    "Apply derivable labels|NotBinding|a pull_request_target job, and it MUTATES rather than checks. Its outcome is enforced by the required Require a type label"
    "Deploy to Pages|NotBinding|a deployment, not a check, and it runs only after a push to master has already merged"
    "Build site (strict)|NotBinding|docs.yml is not reachable from a code change; a docs break is caught on the push that lands it and blocks no unrelated work"
    "Check the release gate covers every job|NotBinding|it guards a TAG run, and a tag is not a merge. A release that lost a gating job is caught by this job on the tag, which is the event that can act on it"

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
    awk '
        /^on:[ \t]*$/            { inOn = 1; next }
        inOn && /^[^ \t]/        { inOn = 0 }
        inOn && /^[ \t]*#/       { next }
        inOn && /^  merge_group:/ { found = 1 }
        END                      { exit(found ? 0 : 1) }
    ' "$1"
}

# Every context this workflow can produce, one per line, as
# `context<TAB>jobKey<TAB>job-level if:`.
#
# The mapping is DERIVED rather than tabulated. A second hand-written list of
# which job produces which context would not be a cross-check, it would be a
# second thing to be wrong -- the same reasoning `check-tsan-scope.cmake` records
# for reading the gate's tag expression instead of restating it.
#
# Indentation is the discriminator throughout, and it is reliable here because
# these are the repository's own files: a job key is two spaces, a job's `name:`
# and `if:` are four, and a step's are six and eight.
EmitJobContexts() {
    awk '
        function flush() {
            if (jobKey == "") return
            if (name == "") name = jobKey
            if (presets != "") {
                n = split(presets, values, ",")
                for (i = 1; i <= n; i++) {
                    value = values[i]
                    gsub(/^[ \t]+|[ \t]+$/, "", value)
                    expanded = name
                    gsub(/\$\{\{ *matrix\.preset *\}\}/, value, expanded)
                    print expanded "\t" jobKey "\t" ifExpr
                }
            } else {
                print name "\t" jobKey "\t" ifExpr
            }
            jobKey = ""; name = ""; ifExpr = ""; presets = ""
        }
        /^jobs:[ \t]*$/                  { inJobs = 1; next }
        inJobs && /^[^ \t]/              { flush(); inJobs = 0 }
        !inJobs                          { next }
        /^[ \t]*#/                       { next }
        /^  [A-Za-z0-9_-]+:[ \t]*$/      { flush(); jobKey = $0
                                           sub(/^  /, "", jobKey)
                                           sub(/:[ \t]*$/, "", jobKey)
                                           next }
        /^    name:[ \t]*/               { name = $0
                                           sub(/^    name:[ \t]*/, "", name)
                                           gsub(/^"|"$/, "", name)
                                           next }
        /^    if:[ \t]*/                 { ifExpr = $0
                                           sub(/^    if:[ \t]*/, "", ifExpr)
                                           next }
        /^[ \t]*preset:[ \t]*\[/         { presets = $0
                                           sub(/^[ \t]*preset:[ \t]*\[/, "", presets)
                                           sub(/\].*$/, "", presets)
                                           next }
        END                              { flush() }
    ' "$1"
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
    awk '
        /^on:[ \t]*$/             { inOn = 1; next }
        inOn && /^[^ \t]/         { inOn = 0 }
        inOn && /^[ \t]*#/        { next }
        inOn && /^  [a-z_]+:/     { inPr = ($0 ~ /^  pull_request:/) }
        inOn && inPr && /^    branches:/ { found = 1 }
        END                       { exit(found ? 0 : 1) }
    ' "$1"
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
done

# ---------------------------------------------------------------------------
# Every required context must still resolve to a job, and that job's own `if:`
# must not exclude the event.
for row in "${RequiredContexts[@]}"; do
    context="${row%%|*}"
    workflow="${row##*|}"
    [[ -f "$workflow" ]] || continue

    # `grep -F` on the derived list rather than a pipeline into `grep -q`: under
    # `pipefail` a `grep -q` that exits at its first match kills the producer with
    # SIGPIPE and the pipeline then reports the PRODUCER's status -- a false
    # negative on the success path. That trap is in the rulebook and this is the
    # shape that avoids it.
    produced="$(EmitJobContexts "$workflow")"
    match="$(printf '%s\n' "$produced" | awk -F'\t' -v want="$context" '$1 == want { print; exit }')"

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
producedContexts=""
for workflow in "${allWorkflows[@]}"; do
    while IFS= read -r line; do
        [[ -n "$line" ]] || continue
        producedContexts="${producedContexts}${line}	${workflow}
"
    done < <(EmitJobContexts "$workflow" | cut -f1)
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
    printf '%s\n' "$requiredNames" | grep -Fxq -- "$context" && inRequired=yes
    printf '%s\n' "$nonBindingNames" | grep -Fxq -- "$context" && inNonBinding=yes

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

    printf '%s\n' "$allProduced" | grep -Fxq -- "$context" \
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
