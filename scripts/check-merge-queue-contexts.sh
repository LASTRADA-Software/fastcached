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
# breaks in.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# ---------------------------------------------------------------------------
# The table: one row per required context, naming the workflow that produces it.
# A context whose job lives in a matrix is written as the EXPANDED name, because
# that is what GitHub reports and what the ruleset names.
RequiredContexts=(
    "Windows-cl-release|.github/workflows/build.yml"
    "Windows-clangcl-release|.github/workflows/build.yml"
    "Linux-clang-release|.github/workflows/build.yml"
    "Linux-gcc-release|.github/workflows/build.yml"
    "macOS-clang-release|.github/workflows/build.yml"
    "clang-tidy|.github/workflows/build.yml"
    "sccache smoke (memcached text)|.github/workflows/build.yml"
    "sccache smoke (memcached binary)|.github/workflows/build.yml"
    "sccache smoke (redis RESP2)|.github/workflows/build.yml"
    "Check C++ style|.github/workflows/build.yml"
    "Require a type label|.github/workflows/pr-labels.yml"
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
# Every workflow named in the table must listen for the event.
mapfile -t workflows < <(printf '%s\n' "${RequiredContexts[@]}" | cut -d'|' -f2 | sort -u)
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

if [[ $problems -gt 0 ]]; then
    echo "check-merge-queue-contexts: $problems problem(s); a merge queue would stall on these" >&2
    exit 1
fi
echo "check-merge-queue-contexts: all ${#RequiredContexts[@]} required contexts can report inside a merge_group event"
