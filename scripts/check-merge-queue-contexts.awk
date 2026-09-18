# SPDX-License-Identifier: Apache-2.0
#
# `check-merge-queue-contexts.sh`'s reader: every fact it needs from one workflow file, from ONE
# walk. Run as:
#
#   awk -f scripts/lib/workflow-walk.awk -f scripts/check-merge-queue-contexts.awk <file> <file>
#
# The file is passed TWICE because that is the walk's convention -- the pass counter comes from
# `FNR == 1` and a consumer judges on the second, so a fact stated after the block that needs it is
# still available. This check needs no such fact today and pays one extra read of a text file for
# uniformity, which is cheaper than a second convention.
#
# It loads `scripts/lib/workflow-walk.awk` and NOT `scripts/lib/shell-lex.awk`: this check reads no
# shell, and the two libraries are separate exactly so that is possible.
#
# ## What this replaces
#
# Five awk programs in the shell script, four of them structure readers with `build.yml`'s
# two-space indentation spelled as literal column counts -- `/^  [A-Za-z0-9_-]+:$/`, `/^    if:/`,
# `/^    types:[ \t]*\[/`, `/^    branches:/`. Each was right for the file as written and answered
# nothing about a file whose jobs sit at another depth, which is legal YAML. Here every one is a
# KEY PATH, so the depth is the walk's problem and not this check's.
#
# ## The one deliberate narrowing
#
# `cancel-in-progress: true` was a raw line grep at any indentation, so it would have matched the
# text inside a `run:` script. It is now a mapping KEY, which cannot. No workflow here has that
# shape, so the records are unchanged; the narrowing is stated because a check that got narrower
# without saying so reads as one that always meant this.
#
# ## The records
#
#   ON        \t <key path> \t <value>   every key under `on:`, the block itself included
#   RETRIGGER \t <types>                 the same-SHA re-trigger types this file names, joined
#   CANCEL    \t <line numbers>          where `cancel-in-progress: true` is set, joined
#   CONTEXT   \t <context> \t <job key> \t <job if:>   one per context the file can produce
#   UNKNOWN   \t <job key> \t <why>      a job whose contexts cannot be told
#
# `CONTEXT` is the job's `name:` or, absent one, its key -- expanded once per COMBINATION of the
# job's matrix, which the shared walk computes with GitHub's semantics (#1432). That expansion is
# what makes one job key several required contexts, and a key a combination lacks expands to
# nothing, which is how a `suffix` only an `include` row carries leaves the other legs' names --
# the REQUIRED ones -- where they were.
#
# `UNKNOWN` is every way of not knowing, and each is a REFUSAL rather than a guess, because both
# wrong guesses are silent: a context produced under a name the table does not hold joins with no
# verdict, and one the table holds that nothing produces reads as a rename.
#
#   * a matrix the walk cannot read, which leaves the job NO combinations to name;
#   * a matrix job whose name carries no expression -- GitHub then APPENDS the combination's values
#     to the name, in a form this check does not model, so the literal name is not a context;
#   * a name still carrying an expression once the matrix is expanded -- `${{ github.* }}`, or a
#     reference to the matrix the walk left unexpanded, which it does rather than guess.

function WorkflowOn(kind) {
    if (kind == "raw") return               # nothing here reads a line before the walk has placed it
    if (kind == "text") return              # a scalar's body is text; no fact of this check lives there
    if (kind == "step-line") return         # and no fact lives at a step's own lines either
    if (kind == "key") { OnKey(); return }
    if (kind == "item") return              # every fact here is a key or a job, never a sequence entry
    if (kind == "step") return              # this check counts contexts, which are per JOB
    if (kind == "job") { FlushJob(); return }
    if (kind == "refusal") { Refused(); return }
    if (kind == "end") { Report(); return }
}

function OnKey(   v, i, n, part) {
    if (WfPass != 2) return
    if (WfPath == "on" || WfPath ~ /^on\//) printf "ON\t%s\t%s\n", WfPath, WfValue

    # A same-SHA re-trigger type, which is an event's own `types:` flow sequence. Three of them
    # re-run a workflow on a commit it has already run on, so together with a cancelling
    # concurrency group they are the fourth door to a required context that never reports.
    if (WfPath ~ /^on\/[^\/]+\/types$/ && substr(WfValue, 1, 1) == "[") {
        v = WfValue
        sub(/^\[/, "", v); sub(/\].*$/, "", v)
        n = split(v, part, ",")
        for (i = 1; i <= n; i++) {
            gsub(/^[ \t]+|[ \t]+$/, "", part[i])
            part[i] = WorkflowUnquote(part[i])
            if (part[i] == "edited" || part[i] == "labeled" || part[i] == "unlabeled")
                retrigger = retrigger (retrigger == "" ? "" : ", ") part[i]
        }
    }

    if (WfKey == "cancel-in-progress" && WorkflowUnquote(WfValue) == "true")
        cancel = cancel (cancel == "" ? "" : ", ") WfAt

    # The job-level facts. Matched by PATH rather than by column, and against this job's own key,
    # so a step's `name:` -- whose path is `jobs/<job>/steps/name` -- cannot be mistaken for it.
    if (WfJob == "") return
    if (WfPath == "jobs/" WfJob "/name") jobName = WorkflowUnquote(WfValue)
    else if (WfPath == "jobs/" WfJob "/if") jobIf = WfValue
}

# One job's contexts: one per combination of its matrix, because the context is the EXPANDED name
# -- which is why a second hand-written list of job-to-context would not be a cross-check but a
# second thing to be wrong. A job with no matrix has one combination, with no keys.
function FlushJob(   name, c, expanded) {
    if (WfPass != 2) { ResetJobFacts(); return }
    name = (jobName != "" ? jobName : WfJob)
    if (WfMatrixUnread != "")
        printf "UNKNOWN\t%s\t%s\n", WfJob, "its matrix cannot be read -- " WfMatrixUnread
    else if (WfHasMatrix && index(name, "${{") == 0)
        printf "UNKNOWN\t%s\t%s\n", WfJob, "it is a matrix job whose name `" name "` carries no expression, and GitHub then appends each combination's values to it in a form this check does not model"
    else
        for (c = 1; c <= WfComboCount; c++) {
            expanded = WorkflowMatrixExpand(name, c)
            if (index(expanded, "${{") > 0)
                printf "UNKNOWN\t%s\t%s\n", WfJob, "its name expands to `" expanded "`" (WfHasMatrix ? " for " WorkflowComboLabel(c) : "") ", which still carries an expression this check cannot evaluate"
            else
                printf "CONTEXT\t%s\t%s\t%s\n", expanded, WfJob, jobIf
        }
    ResetJobFacts()
}

function ResetJobFacts() { jobName = ""; jobIf = "" }

# A line the walk cannot place is COUNTED and not silently dropped. This check's verdict does not
# turn on it -- a workflow it cannot read is `check-workflow-step-env.sh`'s subject, and two checks
# refusing the same line twice would send whoever met it to fix it in the wrong place -- but the
# total is printed, so a file that stops being readable is visible here rather than invisible.
function Refused() { if (WfPass == 2) refusals++ }

function Report() {
    if (retrigger != "") printf "RETRIGGER\t%s\n", retrigger
    if (cancel != "") printf "CANCEL\t%s\n", cancel
    if (refusals > 0) printf "UNREADABLE\t%d\n", refusals
}
