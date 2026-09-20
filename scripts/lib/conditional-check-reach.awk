# SPDX-License-Identifier: Apache-2.0
#
# A consumer of `scripts/lib/workflow-walk.awk`: which jobs exist, and what text each one
# carries. Loaded as
#
#     awk -f scripts/lib/workflow-walk.awk -f scripts/lib/conditional-check-reach.awk <workflow>
#
# It emits two record kinds on stdout, tab-separated:
#
#     JOB   <jobKey>                a job record completed
#     TEXT  <jobKey>  <line>        a line of text belonging to that job
#
# The consumer of THOSE is `check-conditional-check-reach.sh`, which asks whether any one
# job both configures the options a conditionally-registered check needs and runs `ctest`.
#
# Every kind gets an arm, including the ones this consumer does not care about -- the
# library's rule, and it is about omission rather than tidiness: a kind with no arm is a
# decision nobody wrote down. `raw` is deliberately ignored here because `text`, `key`,
# `item` and `step-line` between them carry everything this check reads, and `raw` would
# duplicate all of it.
function WorkflowOn(kind)
{
    if (kind == "raw")       return
    if (kind == "text")      { if (WfJob != "") print "TEXT\t" WfJob "\t" WfLine; return }
    if (kind == "step-line") { if (WfJob != "") print "TEXT\t" WfJob "\t" WfLine; return }
    if (kind == "key")       { if (WfJob != "") print "TEXT\t" WfJob "\t" WfKey ": " WfValue; return }
    if (kind == "item")      { if (WfJob != "") print "TEXT\t" WfJob "\t" WfItem; return }
    if (kind == "step")      return
    if (kind == "job")       { print "JOB\t" WfJob; return }
    if (kind == "refusal")   { print "REFUSAL\t" WfRefuseKind "\t" WfRefuseDetail "\tline " WfAt; return }
    if (kind == "end")       return
    return
}
