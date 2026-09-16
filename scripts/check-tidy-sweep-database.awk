# SPDX-License-Identifier: Apache-2.0
#
# `check-tidy-sweep-database.sh`'s workflow reader, over the shared walk. Run as:
#
#   awk -v job=<job key> -v step=<step name> \
#       -f scripts/lib/workflow-walk.awk -f scripts/check-tidy-sweep-database.awk <file> <file>
#
# The file is passed TWICE, which is the walk's convention.
#
# It prints the named step's `run:` body FOLDED to one line -- each line stripped and followed by
# one space, with no terminating newline -- because the caller compares that against the same
# command as a documentation file spells it, and the two must be one string.
#
# ## What this replaces
#
# One awk program spelling `build.yml`'s indentation as literal columns: the job key at two spaces,
# a step item at six, `run:` at eight, its body at ten (#1456). The walk answers all four, and the
# step's name is the record's rather than a re-derived `- name:` match -- which is the site that
# needed a comment explaining why it must NOT `next`, since a `- name:` line is both a new step and
# possibly the wanted one.
#
# ## The two body rules kept verbatim, because each was a bug
#
# A BLANK line inside the block is skipped rather than ending collection. It separates the body's
# paragraphs; ending there dropped every option after it, and a truncated command still compares as
# a command -- so the failure was a silent pass.
#
# And a `run:` whose command sits on the key line is the body: `>-` and `|` introduce a block and
# carry no text, anything else is the command itself. The walk already makes that distinction, so
# here it is simply `WfBody[]`.
#
# The other extractor in that script reads MARKDOWN and shell files for the same command, and is
# not a workflow reader at all. It stays where it is.

function WorkflowOn(kind) {
    if (kind == "raw") return
    if (kind == "text") return              # a body line is already in the step record
    if (kind == "step-line") return
    if (kind == "key") return
    if (kind == "item") return              # the body of one named step is all this reads
    if (kind == "step") { OnStep(); return }
    if (kind == "job") return
    if (kind == "refusal") return           # a workflow this walk cannot read is #1175's subject
    if (kind == "end") return
}

function OnStep(   i, line) {
    if (WfPass != 2) return
    if (WfJob != job || WfStepName != step) return
    for (i = 0; i < WfBodyCount; i++) {
        line = WorkflowTrim(WfBody[i])
        if (line != "") printf "%s ", line
    }
}
