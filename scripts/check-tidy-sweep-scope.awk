# SPDX-License-Identifier: Apache-2.0
#
# `check-tidy-sweep-scope.sh`'s reader, over the shared walk. Run as:
#
#   awk -v want=<kind> [-v sweepId=<id> -v sweepJob=<job>] \
#       -f scripts/lib/workflow-walk.awk -f scripts/check-tidy-sweep-scope.awk <file> <file>
#
# The file is passed TWICE, which is the walk's convention: the pass counter comes from `FNR == 1`
# and a consumer judges on the second.
#
# ## What this replaces, and the column counts that went with it
#
# Three awk programs, each carrying `build.yml`'s indentation as literal columns -- `/^        id:/`,
# `/^        env:/`, `/^        if:/` at eight spaces, `/^          /` at ten, `/^      - /` at six,
# `/^  [A-Za-z0-9_-]+:[ \t]*$/` at two. Every one is right for the file as written today and says
# nothing about a file whose jobs sit at another depth, which is legal YAML (#1456).
#
# ## The joining, which is the part that must not regress
#
# A `run:` and an `if:` are both JOINED across their continuation lines before being tested, and
# the script records why in its own words: the shipped expression is over a hundred characters,
# wrapping it as a folded scalar is the natural response to that, and read one line at a time a
# wrapped expression looks exactly like a DELETED reader -- which is a red build accusing an author
# of something they did not do.
#
# The walk hands a `run:` body over as `WfBody[]` and every other scalar's continuations as `text`
# events carrying `WfBlockKey`, the key they belong to. That field exists for this check: without
# it a folded `if:` cannot be told from any other scalar by anything except indentation, which is
# the column counting being removed.
#
# ## The records
#
#   want=sweep   SWEEP  \t <job> \t <step id> \t <joined run> \t <joined step env>
#   want=reader  READER \t <joined if>    (only for @p sweepJob, naming @p sweepId and master)
#   want=perms   PERMS  \t <one permissions row as `scope: level`>

function WorkflowOn(kind) {
    if (kind == "raw") return               # no fact of this check lives in an unplaced line
    if (kind == "text") { OnText(); return }
    if (kind == "step-line") return         # the key events below are finer and are what is wanted
    if (kind == "key") { OnKey(); return }
    if (kind == "step") { FlushStep(); return }
    if (kind == "job") return               # every rule here is per STEP, inside a job the walk names
    if (kind == "refusal") return           # a workflow this walk cannot read is #1175's subject
    if (kind == "end") return
}

# A continuation line of the step's own `if:`. `WfScope` is asked because `text` fires outside a
# step too, and a JOB-level `if:` folded across lines would otherwise land in this accumulator.
function OnText(   line) {
    if (WfPass != 2) return
    if (WfScope != "step" || WfBlockKey != "if") return
    line = WorkflowTrim(WfLine)
    if (line != "") stepIf = (stepIf == "" ? line : stepIf " " line)
}

function OnKey(   v) {
    if (WfPass != 2) return

    # The `permissions:` block of the job the sweep was found in. Emitted as it is read: the job is
    # named by the caller, so there is nothing to hold back for.
    if (want == "perms" && sweepJob != "" && WfPath == "jobs/" sweepJob "/permissions/" WfKey)
        printf "PERMS\t%s: %s\n", WfKey, WfValue

    # A row of this step's own `env:`, rebuilt as the source spells it. The walk says which rows
    # those are (`WfEnvRow`), so a comment inside the block is not one -- the site that used to
    # answer this was an indentation match, and a comment at that indent was a row.
    if (WfEnvRow) {
        v = WfKey ": " WfValue
        stepEnv = (stepEnv == "" ? v : stepEnv " " v)
        return
    }

    if (!WfStepKey) return
    if (WfKey == "id") stepId = WorkflowTrim(WfValue)
    else if (WfKey == "if") {
        # A block indicator carries no text of its own; anything else is the first line.
        stepIf = (WfValue ~ /^[|>]/) ? "" : WorkflowTrim(WfValue)
    }
}

# One step, complete. The `run:` is joined here rather than as it is read, because a step whose
# whole command sits on the `run:` key line was invisible to the line-by-line form: reflowing
# `run: >-` plus a continuation into the identical one-liner made this check report that the sweep
# had been deleted.
function FlushStep(   i, line, run) {
    if (WfPass != 2) { ResetStepFacts(); return }
    for (i = 0; i < WfBodyCount; i++) {
        line = WorkflowTrim(WfBody[i])
        if (line != "") run = (run == "" ? line : run " " line)
    }
    if (want == "sweep") {
        if (run ~ /tidy-sweep\.sh/ && run !~ /--self-test/)
            printf "SWEEP\t%s\t%s\t%s\t%s\n", WfJob, stepId, run, stepEnv
    } else if (want == "reader" && WfJob == sweepJob) {
        # Scoped to the job, and that is not tidiness: `steps.<id>.conclusion` only ever resolves
        # within its own job, so a reader in job A says nothing about a sweep in job B. Two sweep
        # steps both using `id: sweep` once made this check report the SECOND one as having a
        # reader -- it had found the FIRST one's.
        if (index(stepIf, "steps." sweepId ".conclusion") && index(stepIf, "refs/heads/master"))
            printf "READER\t%s\n", stepIf
    }
    ResetStepFacts()
}

function ResetStepFacts() { stepId = ""; stepIf = ""; stepEnv = "" }
