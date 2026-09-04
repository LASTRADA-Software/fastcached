#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Open an issue, or comment on the one that already carries this exact title.
#
# Usage:
#   scripts/ci-report-issue.sh <title> <bodyFile> [label ...]
#
# Reads `GH_TOKEN` and `GH_REPO` from the environment, like every other `gh` call
# in this repository's workflows. Prints what it did; exits non-zero when it
# could not tell whether the issue already existed, which is the one outcome that
# must never be quietly treated as "no match".
#
# ## Why the lookup is a LIST and not a SEARCH
#
# `gh issue list --search` goes through GitHub's issue search INDEX, which lags
# creation by seconds to minutes -- so two failures inside that window each find
# nothing and each open an issue, which is the noise a de-duplicating reporter
# exists to avoid. A plain `gh issue list` reads the repository's issue
# connection directly, so the second failure sees what the first just opened.
# That connection is GRAPHQL rather than REST, which matters only for which
# rate-limit budget a reader chasing a failure here should look at.
#
# The listing is deliberately NOT narrowed by the labels this script applies.
# Narrowing by them makes de-duplication depend on labels nobody promised to
# keep -- this script invites a human to touch them by applying
# `status/needs-triage`, and a triager re-areaing the report would make the next
# failure miss the open issue and open a second one. The exact title decides it;
# the labels are for the humans.
#
# ## The failure directions that are not symmetric
#
# A lookup that FAILED and a lookup that found NOTHING are different findings,
# and the obvious spelling renders them identically: `found="$(gh ... || true)"`
# turns an API error, a mangled `--jq` program and a real absence into one empty
# string, and the surrounding code then opens a duplicate issue every single run.
# `.agent/rules/build-and-toolchain.md` records two instruments that invented
# their subject exactly this way. So the exit status of the query is checked
# before its output is read, and a query that did not answer is fatal.
#
# A listing that came back AT its cap is a third state: it is a real answer about
# a set that is not the whole set. It warns rather than failing, because opening
# a second copy of a report is a smaller harm than never reporting -- but it says
# so, so a duplicate is explicable rather than mysterious.
#
# ## Not this repository's only copy, deliberately, for now
#
# `.github/workflows/build.yml`'s clang-tidy job carries an inline copy of this
# logic for the master-sweep report it opens. Folding it into this script is
# [#717](https://github.com/LASTRADA-Software/fastcached/issues/717) rather than
# part of #684: that step lives inside a REQUIRED context, it is reachable only
# on a master push that has already failed, and editing it to remove a
# duplication would put a required job's YAML at risk for no gain this change
# needs. The duplication is recorded rather than left to be discovered.

set -euo pipefail

Fatal() { echo "ci-report-issue: $*" >&2; exit 1; }

[[ $# -ge 2 ]] || Fatal "usage: $(basename "${BASH_SOURCE[0]}") <title> <bodyFile> [label ...]"

title="$1"
bodyFile="$2"
shift 2

[[ -n "$title" ]] || Fatal "empty title: the title is what de-duplicates this report, so an empty one would match every future report with an empty one"
[[ -f "$bodyFile" ]] || Fatal "no body at '$bodyFile'"
[[ -s "$bodyFile" ]] || Fatal "the body at '$bodyFile' is empty; an issue that says nothing is worse than none"

command -v gh >/dev/null || Fatal "gh is not on PATH; this reporter cannot silently do nothing"

# Expanded as `${labelArgs[@]+"${labelArgs[@]}"}` at the call below rather than
# bare: under `set -u`, bash 3.2 -- which macOS still ships and which every
# script this tree registers in the default ctest set has to survive -- treats an
# EMPTY array expanded with `[@]` as an unbound variable and dies. A reporter
# that dies when handed no labels would be a reporter nobody notices is broken,
# since the labelled call sites work.
labelArgs=()
for label in "$@"; do
    labelArgs+=(--label "$label")
done

# `$ENV.TITLE` rather than the title spliced into the jq program, so no quoting
# in a title anyone rewords later can turn the lookup into a syntax error -- and,
# more to the point, into an empty answer that reads as "no match".
limit=500
if ! found="$(TITLE="$title" gh issue list --state open --limit "$limit" --json number,title \
              --jq '(length | tostring) + "\t"
                    + ([.[] | select(.title == $ENV.TITLE) | .number] | first // "" | tostring)')"; then
    Fatal "the open-issue listing failed; a query that did not answer is not a query that found nothing, and treating it as one opens a duplicate on every run"
fi

# A well-formed answer always carries the tab. Checked before anything is read
# out of it, because a response whose SHAPE was never checked is a conclusion
# drawn from nothing.
case "$found" in
    *$'\t'*) ;;
    *) Fatal "the open-issue listing returned '${found}', which is not '<count><TAB><number>'; refusing to draw a conclusion from a shape this does not recognise" ;;
esac

seen="${found%%$'\t'*}"
existing="${found#*$'\t'}"

case "$seen" in
    ''|*[!0-9]*) Fatal "the open-issue listing reported '${seen}' issues, which is not a number" ;;
esac

if [[ "$seen" -ge "$limit" ]]; then
    echo "::warning::the open-issue listing hit its ${limit} cap; a duplicate report may be opened"
fi

if [[ -n "$existing" ]]; then
    echo "updating existing report #${existing} (searched ${seen} open issues)"
    gh issue comment "$existing" --body-file "$bodyFile"
else
    echo "opening a new report (searched ${seen} open issues, no title match)"
    gh issue create --title "$title" --body-file "$bodyFile" ${labelArgs[@]+"${labelArgs[@]}"}
fi
