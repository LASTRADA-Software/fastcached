# SPDX-License-Identifier: Apache-2.0
#
# `check-gated-jobs.sh`'s reader: every fact its six rules need, from ONE walk. Run as:
#
#   awk -v sep=<separator> -v runner=<path> -v action=<action> \
#       -f scripts/lib/workflow-walk.awk -f scripts/check-gated-jobs.awk <file> <file>
#
# The file is passed TWICE, which is the walk's convention: the pass counter comes from `FNR == 1`
# and a consumer judges on the second, so a `runs-on:` written after that job's `steps:` still
# reaches a step's record.
#
# ## What this replaces
#
# SIX awk programs in the shell script, every one of them spelling `build.yml`'s indentation as
# literal column counts -- the job key at two spaces, `name:`/`if:`/`runs-on:` at four, a step item
# at six, a step's own keys at eight, a `with:` row and a matrix axis at ten. Each was right for the
# file as written today and answered nothing about a file whose jobs sit at another depth, which is
# legal YAML (#1456). Here each is a key PATH.
#
# ## The separator is `sep`, and NOT a tab
#
# Tab is IFS *whitespace*, so `IFS=$'\t' read` treats runs of it as one delimiter and drops leading
# and trailing ones -- an empty field in the MIDDLE of a row collapses and shifts every field after
# it. Rule C reads four fields of which two can legitimately be empty, and when the first was, it
# read the second's value into the first's variable and reported the wrong half of a real defect
# (#791). The KIND column is tab-separated because the shell selects on it with `cut -f2-` before
# any `read` sees it.
#
# ## Two capabilities this gains, stated because neither is visible today
#
# A job-level and a step-level `if:` are now JOINED across their continuation lines. The old readers
# took the first line only, so a condition wrapped as a folded scalar read as a condition that says
# something else -- `check-tidy-sweep-scope.sh` records paying for exactly that, where a wrapped
# expression looked like a DELETED reader. No workflow here folds one, so nothing in the output
# moves; what changes is what happens the day somebody wraps a hundred-character condition.
#
# And a `runs-on:` written as a block sequence reaches rule F. The old reader matched only the
# inline form and left `runsOn` empty for a list, which rule F reports as a label it cannot map --
# a refusal, so not a silent wrong answer, but a refusal on a legal workflow.
#
# ## The records
#
#   GUARDED \t <job> <sep> <job if:>              a job consulting the classifier with no `!cancelled()`
#   DOCSUBJ \t <job> <sep> <name> <sep> <job if:> <sep> <step if:>   a step running @p runner
#   NEEDS   \t <job>                              an entry of `release.needs`
#   JOBIF   \t <job> <sep> <job if:>              every job that carries a condition
#   CCACHE  \t <job> <sep> <with.save:>           a step using @p action, its `save:` or empty
#   CACHE   \t <writer> <sep> <uses> <sep> <runs-on> <sep> <with.key:> <sep> <unresolved>
#   JOBS    \t <how many jobs the walk placed>
#
# `CACHE` is one row per COMBINATION of the job's matrix (#1432), with `runs-on` and the key
# already expanded for it by the shared walk -- so the `writer` is the job key, followed by the
# combination's label when the job has a matrix. Every leg is a writer, because legs of one job run
# concurrently exactly as two jobs do: a key the legs do NOT vary is one key written several times,
# which is #318 arriving through a matrix. `unresolved` is empty, or says why this row's key cannot
# be compared at all -- a matrix the walk cannot read, or a matrix reference it left unexpanded --
# and the shell refuses such a row rather than comparing its text.
#
# `JOBS` is a POSITIVE CONTROL, and it is the only record here that is not a finding. Every
# rule in this check reports on what it did NOT find -- an unguarded job, a missing reader, an
# empty gating set -- so over a file the walk could not read, four of the six report "nothing
# to vouch for" and the check passes. *Absence of the negative is not the positive*: a check
# concluding from a count of bad things needs a separate assertion that the good things exist.

function WorkflowOn(kind) {
    if (kind == "raw") return               # every fact below is attributed to a job or a step,
                                            # and `raw` fires before the walk has decided which
    if (kind == "text") { OnText(); return }
    if (kind == "step-line") return         # the `key` and `text` arms are finer and cover it
    if (kind == "key") { OnKey(); return }
    if (kind == "item") { OnItem(); return }
    if (kind == "step") { FlushStep(); return }
    if (kind == "job") { FlushJob(); return }
    if (kind == "refusal") return           # a workflow this walk cannot read is #1175's subject
    if (kind == "end") { printf "JOBS\t%d\n", jobs; return }
}

# Does @p s name the scope classifier? Rules A and B turn on it appearing ANYWHERE in the job.
function NamesClassifier(s) { return index(s, "needs.changes.outputs.code") > 0 }

# A continuation line: it extends whichever `if:` it belongs to, and it is scanned like any other
# text. `WfBlockKey` is what says which key owns it -- the fact the old readers had to guess from
# indentation, and the reason a folded condition used to read as a truncated one.
function OnText(   line) {
    if (WfPass != 2) return
    line = WorkflowTrim(WfLine)
    if (line == "") return
    if (NamesClassifier(line)) usesClassifier = 1
    if (index(line, runner)) stepUsesRunner = 1
    if (index(line, action)) stepUsesAction = 1
    if (WfBlockKey != "if") return
    if (WfScope == "step") stepIf = (stepIf == "" ? line : stepIf " " line)
    else if (WfScope == "job") jobIf = (jobIf == "" ? line : jobIf " " line)
}

function OnKey(   v) {
    if (WfPass != 2) return
    v = WfValue
    if (NamesClassifier(v)) usesClassifier = 1
    if (WfScope == "step") {
        if (index(v, runner)) stepUsesRunner = 1
        if (index(v, action)) stepUsesAction = 1
    }

    if (WfJob == "") return

    # The job's own keys, by path, so a step's `name:` or `if:` cannot be mistaken for one.
    if (WfPath == "jobs/" WfJob "/name") jobName = WorkflowUnquote(v)
    else if (WfPath == "jobs/" WfJob "/if") jobIf = (v ~ /^[|>]/) ? "" : v

    if (!WfInStepKeys()) return
    if (WfPath == "jobs/" WfJob "/steps/if") stepIf = (v ~ /^[|>]/) ? "" : v
    else if (WfPath == "jobs/" WfJob "/steps/with/save") stepSave = v
    else if (WfPath == "jobs/" WfJob "/steps/with/key") stepKey = v
}

# Whether the current line belongs to a step at all. `WfScope` answers it; a helper because the
# three paths above would otherwise each repeat the test.
function WfInStepKeys() { return WfScope == "step" }

# `release.needs`, which decides which jobs gate the release. With an empty gating set every
# event-keyed condition looks unregulated, which rule D refuses rather than vouching for.
function OnItem() {
    if (WfPass != 2) return
    if (WfPath == "jobs/release/needs") printf "NEEDS\t%s\n", WfItem
}

function FlushStep() {
    if (WfPass != 2) { ResetStepFacts(); return }
    # A `run:` body reaches `OnText` line by line already, except for a value written on the `run:`
    # key line itself -- which `OnKey` sees. Both are covered, so nothing is scanned twice here.
    if (stepUsesRunner)
        printf "DOCSUBJ\t%s%s%s%s%s%s%s\n", WfJob, sep, jobName, sep, jobIf, sep, stepIf
    if (stepUsesAction)
        printf "CCACHE\t%s%s%s\n", WfJob, sep, stepSave
    if (WfStepUses != "" && stepKey != "") EmitCacheRows()
    ResetStepFacts()
}

# One `CACHE` row per combination of this job's matrix, the key and the runner expanded for it. A
# matrix the walk cannot read has no combinations, so it gets one row saying so rather than none --
# a writer that vanished from the comparison would be the silent half.
function EmitCacheRows(   c, runsOn, key, writer, why) {
    if (WfMatrixUnread != "") {
        printf "CACHE\t%s%s%s%s%s%s%s%s%s\n", WfJob, sep, WfStepUses, sep, WorkflowTrim(WfRunsOn), sep,
               stepKey, sep, "its job's matrix cannot be read -- " WfMatrixUnread
        return
    }
    for (c = 1; c <= WfComboCount; c++) {
        runsOn = WorkflowTrim(WorkflowMatrixExpand(WfRunsOn, c))
        key = WorkflowMatrixExpand(stepKey, c)
        writer = WfJob (WfHasMatrix ? " " WorkflowComboLabel(c) : "")
        # Case-folded, because a reference the walk LEFT is one whose case differs from a key.
        why = (tolower(runsOn " " key) ~ /\$\{\{[^}]*matrix/) ? "a matrix reference is left unexpanded" : ""
        printf "CACHE\t%s%s%s%s%s%s%s%s%s\n", writer, sep, WfStepUses, sep, runsOn, sep, key, sep, why
    }
}

function FlushJob() {
    if (WfPass != 2) { ResetJobFacts(); return }
    jobs++
    # Rule A/B: a job that consults the classifier must survive a FAILED one. Without
    # `!cancelled()` a failed `changes` skips the job before its condition is consulted, and for a
    # MATRIX job that is worse than a wrong green -- a skipped matrix job never expands, so its
    # per-leg contexts never exist and the pull request cannot merge.
    if (usesClassifier && index(jobIf, "!cancelled()") == 0)
        printf "GUARDED\t%s%s%s\n", WfJob, sep, (jobIf == "" ? "<no job-level if:>" : jobIf)
    if (jobIf != "") printf "JOBIF\t%s%s%s\n", WfJob, sep, jobIf
    ResetJobFacts()
}

function ResetStepFacts() { stepIf = ""; stepSave = ""; stepKey = ""; stepUsesRunner = 0; stepUsesAction = 0 }
function ResetJobFacts() { ResetStepFacts(); jobName = ""; jobIf = ""; usesClassifier = 0 }
