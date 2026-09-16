# SPDX-License-Identifier: Apache-2.0
#
# `check-shipping-configure-guard.sh`'s reader: every SHIPPING configure in a workflow, and whether
# the instruction-set check follows it in the same job and before that job's build. Run as:
#
#   awk -v guard=<script path> -v shipping=<flag> \
#       -f scripts/lib/workflow-walk.awk -f scripts/check-shipping-configure-guard.awk <f> <f>
#
# The file is passed TWICE, which is the walk's convention.
#
# ## What a SHIPPING configure is, as a selector rather than a list
#
# A step whose joined `run:` body invokes `cmake` with `-DFASTCACHED_BUILD_TESTS=OFF`. That is the
# one property that distinguishes what a customer receives from what `ctest` measures, and it is
# DATA -- `shipping` is passed in -- rather than the three job names that happen to have it today.
# A fourth packaging job is then a row the selector finds, not a row somebody remembered to add.
#
# The body must be JOINED before it is tested, and that is not a nicety: the macOS configure spells
# `-DFASTCACHED_BUILD_TESTS=OFF \` on a continuation line of its own, so a line-wise selector finds
# two of the three and reports a clean tree.
#
# ## The four things asked of the guard step, and why each is its own answer
#
#   * it exists in the SAME JOB -- a check in another job says nothing, because the database it
#     reads is that job's working tree;
#   * it comes BEFORE that job's build -- after it, the objects it would have refused are already
#     built, and on a packaging job already signed;
#   * it carries no `if:` -- a condition is how a step reports without gating;
#   * it carries no `continue-on-error:` -- which is how a step gates without failing.
#
# The last two are different mechanisms with one outcome, so they are two records rather than one
# flag. A refusal that names the wrong one sends somebody to delete the wrong line.
#
# ## The records
#
#   SHIPPING \t <job> \t <step index> \t <step name> \t <binary dir>
#   GUARD    \t <job> \t <step index> \t <argument> \t <if:> \t <continue-on-error:>
#   BUILD    \t <job> \t <step index>
#   STEPS    \t <how many steps the walk placed>
#
# `STEPS` is a POSITIVE CONTROL and the only record that is not a finding. Every verdict here is
# about something MISSING, so over a file the walk could not read every one would answer *nothing
# to vouch for*: absence of the negative is not the positive.

function WorkflowOn(kind) {
    if (kind == "raw") return               # every fact is attributed to a step the walk placed
    if (kind == "text") { OnText(); return }
    if (kind == "step-line") return         # the `key` and `text` arms are finer and cover it
    if (kind == "key") { OnKey(); return }
    if (kind == "item") return              # no fact here is a sequence entry
    if (kind == "step") { FlushStep(); return }
    if (kind == "job") return               # the job is a COLUMN of each record, never a record
    if (kind == "refusal") return           # a workflow this walk cannot read is #1175's subject
    if (kind == "end") { printf "STEPS\t%d\n", steps; return }
}

function OnText(   line) {
    if (WfPass != 2) return
    if (WfScope != "step") return
    line = WorkflowTrim(WfLine)
    if (line == "") return
    # A step's `if:` may be folded, and a condition read one line at a time is a condition that
    # says something else. `WfBlockKey` is what says which scalar a continuation belongs to.
    if (WfBlockKey == "if") stepIf = (stepIf == "" ? line : stepIf " " line)
}

function OnKey() {
    if (WfPass != 2) return
    if (WfScope != "step") return
    if (WfKey == "if" && WfStepKey) stepIf = (WfValue ~ /^[|>]/) ? "" : WfValue
    else if (WfKey == "continue-on-error" && WfStepKey) stepContinue = WorkflowUnquote(WfValue)
}

# The build directory a `cmake` line in @p s configures into: its `-B` value, or `out/build/<preset>`
# for a `--preset`. The preset convention is this repository's own and is stated rather than
# resolved out of `CMakePresets.json`: there is no JSON reader in this tree, and a model of one
# would be MORE PERMISSIVE than the file it models, which produces wrong agreement rather than a
# refusal. A configure this cannot resolve yields "" and its caller refuses by name.
function BinaryDirOf(s,   rest) {
    if (match(s, /-B[ \t]+[^ \t]+/)) {
        rest = substr(s, RSTART, RLENGTH)
        sub(/^-B[ \t]+/, "", rest)
        return rest
    }
    if (match(s, /--preset[ \t=]+[^ \t]+/)) {
        rest = substr(s, RSTART, RLENGTH)
        sub(/^--preset[ \t=]+/, "", rest)
        return "out/build/" rest
    }
    return ""
}

# The argument a guard step hands the instruction-set check: everything after the script's own path.
function GuardArgumentOf(s,   rest) {
    rest = s
    if (!match(rest, guard "[ \t]+[^ \t]+")) return ""
    rest = substr(rest, RSTART, RLENGTH)
    sub(guard "[ \t]+", "", rest)
    return rest
}

function FlushStep(   i, line, body) {
    if (WfPass != 2) { ResetStepFacts(); return }
    steps++
    for (i = 0; i < WfBodyCount; i++) {
        line = WorkflowTrim(WfBody[i])
        if (line != "") body = (body == "" ? line : body " " line)
    }
    if (body != "") {
        if (index(body, "cmake") && index(body, shipping))
            printf "SHIPPING\t%s\t%d\t%s\t%s\n", WfJob, WfStepIndex, WfStepName, BinaryDirOf(body)
        if (index(body, guard))
            printf "GUARD\t%s\t%d\t%s\t%s\t%s\n", WfJob, WfStepIndex, GuardArgumentOf(body), stepIf, stepContinue
        # A BUILD is `cmake --build`, which is the moment after which a refusal costs the objects
        # it would have prevented. Recorded for every step that has one, because the guard has to
        # precede the FIRST.
        if (index(body, "cmake --build"))
            printf "BUILD\t%s\t%d\n", WfJob, WfStepIndex
    }
    ResetStepFacts()
}

function ResetStepFacts() { stepIf = ""; stepContinue = "" }
