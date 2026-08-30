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
# ## What is asserted rather than demonstrated
#
# The `changes` job cannot be made to fail on demand, so this proves the SHAPE of
# the conditions, not the behaviour of a run in which the classifier died. The
# behaviour it relies on is the measurement above (`b4777aa`), which is recorded
# in `.agent/rules/build-and-toolchain.md` rather than re-derived here.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

workflow=".github/workflows/build.yml"
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

if [[ $problems -gt 0 ]]; then
    echo "check-gated-jobs: $problems problem(s); a failed scope classifier would go green" >&2
    exit 1
fi
echo "check-gated-jobs: the scope gate fails safe"
