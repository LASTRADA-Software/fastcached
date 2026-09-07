#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Every ticket a pull request's COMMITS say it closes must also be named by a closing
# keyword in its BODY.
#
# ## Why both, rather than one instead of the other
#
# They do different jobs and neither substitutes for the other:
#
#   * the COMMIT trailer, one per ticket, is what makes a ticket findable and EXCISABLE
#     on a batch branch -- `git log --grep` re-derives the boundaries after a rebase,
#     which is the only reliable way once every SHA has moved;
#   * the BODY keyword is what the merge acts on.
#
# Measured across one day's merges (#974): #955, #953 and #961 carried the keyword in
# both and closed #893, #901 and #208; #959 carried it only in its commits and left #826
# and #866 OPEN. Every close event that worked has NO `commit_id` in the issue timeline,
# which is what a body-keyword close looks like -- nothing in the sample was closed by a
# commit trailer.
#
# A delivered ticket that stays open is indistinguishable from one nobody has started.
# #826 sat inside the `type/bug` count for a day after merging; a batch of eight does
# that eight times at once.
#
# ## The direction it reports and does not refuse
#
# Requiring the body keyword creates a second hazard, and this check is where it has to
# be visible: excising a ticket from a batch removes its commits and leaves the body
# promising to close it. That closes something undelivered, which is worse than one
# staying open -- an open ticket gets re-triaged, a wrongly closed one does not. But an
# unbacked body keyword is ORDINARY in the common case (a change with no trailer), so a
# refusal would be wrong. It is named on every run instead.
#
# ## The DECISION is a pure function, and that is deliberate
#
# Acquisition (a `gh` call) and judgement are split, so the self-test drives staged
# records rather than a live pull request -- the shape `node-scratch-isolation-e2e`
# arrived at after its own timing measurements could not be staged.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
set -uo pipefail

REPO="${FASTCACHED_REPO:-LASTRADA-Software/fastcached}"
SkipUnavailable=77

usage() {
    cat >&2 <<'USAGE'
usage: check-pr-closing-keywords.sh --pr <number>
       check-pr-closing-keywords.sh --list-closing <number>
       check-pr-closing-keywords.sh --self-test

  --pr <n>            read the pull request from the API and judge it
  --list-closing <n>  print the tickets it closes, one per line, and judge nothing
  --self-test         drive the decision against staged records

exit 0 every ticket the commits name is in the body
     1 one or more are missing, or the body could not be read
    77 the API could not be reached, so NOTHING was verified

`--list-closing` exists so `check-rulebook-open-work.sh` can ask which tickets a pull
request will close WITHOUT restating GitHub's keyword pattern. A second copy of that
pattern is not a cross-check, it is a second thing to be wrong (#1016).
USAGE
}

# Tickets a text closes, one per line, sorted and unique.
#
# The keyword must precede EACH number: `Closes #A and #B` closes only #A, which is
# already a scar in `.agent/guides/team-run.md`. So this looks for the keyword-number
# PAIR and never for a bare `#N`, and it is the same reading GitHub performs -- a
# checker more permissive than the thing it stands for would pass a body that closes
# half of what it claims.
#
# `grep -oE ... <<<`, never `printf | grep`: `grep -q` is not used here, but the
# pipeline shape is the one #970 bans and the herestring costs nothing.
# @param 1 the text
ClosingKeywords() {
    grep -oiE '\b(close[sd]?|fix(e[sd])?|resolve[sd]?)[[:space:]]+#[0-9]+' <<< "$1" \
        | grep -oE '[0-9]+' | sort -un
}

# Judge one record.
# @param 1 the pull request body
# @param 2 every commit message, concatenated
# @return 0 when the body names every ticket the commits do
Judge() {
    local body="$1" commits="$2" inCommits="" inBody="" missing="" n=""
    inCommits="$(ClosingKeywords "$commits")"
    inBody="$(ClosingKeywords "$body")"

    # No trailer anywhere is a pull request that closes nothing -- ordinary, and not
    # this check's business. It must not be an error: a docs-only or chore pull request
    # legitimately names no ticket, and refusing it would teach people to write a
    # keyword to satisfy a checker, which closes the wrong issue.
    [ -n "$inCommits" ] || { echo "  no closing trailer in any commit; nothing to check"; return 0; }

    while IFS= read -r n; do
        [ -n "$n" ] || continue
        grep -qx -- "$n" <<< "$inBody" || missing="$missing $n"
    done <<< "$inCommits"

    if [ -n "$missing" ]; then
        echo "FAILED: the commits say this closes${missing}, and the body does not." >&2
        echo "        Two causes. They are indistinguishable in the text, so YOU have to" >&2
        echo "        pick -- neither GitHub nor this check can:" >&2
        echo >&2
        echo "        (1) You MEANT to close it and the body is missing the line." >&2
        echo "            Add a line per ticket to the body; KEEP the commit trailers," >&2
        echo "            which are what makes a ticket excisable from a batch branch." >&2
        echo "            Under the merge queue only the BODY keyword closes an issue," >&2
        echo "            so without it those tickets stay OPEN after this merged --" >&2
        echo "            delivered, counted as outstanding, and indistinguishable from" >&2
        echo "            work nobody started (#974)." >&2
        echo >&2
        echo "        (2) You did NOT mean to close it: a commit message NARRATES a" >&2
        echo "            closing -- \"the earlier PR closed #NNNN\" -- and a keyword" >&2
        echo "            sitting in front of a number is a directive to GitHub whatever" >&2
        echo "            the sentence around it means. REWORD the prose and add nothing." >&2
        echo "            Adding the body line here closes a ticket this branch never" >&2
        echo "            delivered, which is the worse of the two failures: an open" >&2
        echo "            ticket gets re-triaged, a wrongly closed one does not." >&2
        echo "            Note that QUOTING the phrase to explain it is still issuing it," >&2
        echo "            so discuss it with a placeholder rather than a real number." >&2
        return 1
    fi
    # The OTHER direction is reported and never refused, and the asymmetry is the
    # point rather than an omission.
    #
    # A ticket in the body that no commit names is legitimate in the ordinary case: a
    # docs or chore pull request closes something without a trailer, and refusing that
    # would teach people to write a trailer to satisfy a checker.
    #
    # It is DANGEROUS in exactly one case, and that case is created by this very check:
    # once the body carries a keyword per ticket, excising a ticket from a batch --
    # `rebase --onto`, which the batch workflow treats as routine -- removes its commits
    # and leaves the body promising to close it. That closes a ticket nobody delivered,
    # which is strictly worse than one staying open: an open ticket gets re-triaged, a
    # wrongly closed one does not.
    #
    # So it is named on every run. A refusal would be wrong (the ordinary case is
    # common) and silence would be worse (the dangerous case is invisible), which leaves
    # saying it out loud -- the same shape as `RefuseWithoutCounter`.
    local orphans="" b=""
    while IFS= read -r b; do
        [ -n "$b" ] || continue
        grep -qx -- "$b" <<< "$inCommits" || orphans="$orphans $b"
    done <<< "$inBody"
    if [ -n "$orphans" ]; then
        echo "  note: the body also closes${orphans}, which no commit names."
        echo "        Ordinary for a change with no trailer -- and what a ticket EXCISED from"
        echo "        this branch looks like, where it would close something undelivered."
    fi

    echo "  every ticket the commits name ($(tr '\n' ' ' <<< "$inCommits")) is in the body"
    return 0
}

if [ "${1:-}" = "--self-test" ]; then
    cases=0
    failures=0
    # @param 1 name  @param 2 expect pass|refuse  @param 3 body  @param 4 commits
    #        @param 5.. optional: substrings the OUTPUT must all carry
    #
    # Those trailing parameters are what makes an advice change testable at all. A case
    # can otherwise only assert the verdict, and the verdict does not move when the
    # advice is wrong -- so a case written for the narrating shape would pass just as
    # happily against the single-cause message #1013 replaced.
    #
    # There is deliberately no case asserting that the NARRATING shape gets one remedy
    # and the forgot-the-body shape gets the other. The check cannot tell them apart --
    # that is the finding -- so a per-shape assertion would be pinning a discrimination
    # the code does not make and must not claim to. One message names both, and the
    # case below asserts exactly that.
    run_case() {
        local name="$1" want="$2" body="$3" commits="$4" out="" got=pass need=""
        shift 4
        cases=$((cases + 1))
        out="$(Judge "$body" "$commits" 2>&1)" || got=refuse
        if [ "$got" != "$want" ]; then
            echo "  FAIL $name: expected $want, got $got" >&2
            printf '%s\n' "$out" | sed 's/^/       | /' >&2
            failures=$((failures + 1))
            return
        fi
        for need in "$@"; do
            if ! grep -qF -- "$need" <<< "$out"; then
                echo "  FAIL $name: the output does not carry '$need'" >&2
                printf '%s\n' "$out" | sed 's/^/       | /' >&2
                failures=$((failures + 1))
                return
            fi
        done
        echo "  ok   $name ($got)"
    }

    # selftest-data: begin
    run_case "body carries the one trailer" pass \
        "Prose about the change.

Fixes #883" \
        "fix(x): a thing

Fixes #883"
    run_case "body carries neither of two" refuse \
        "Prose about the change." \
        "fix(a): one

Fixes #826

fix(b): two

Fixes #866"
    run_case "body carries one of two -- the batch case" refuse \
        "Prose.

Fixes #826" \
        "fix(a): one

Fixes #826

fix(b): two

Fixes #866"
    run_case "no trailer anywhere is not this check's business" pass \
        "A docs-only change." \
        "docs: reword a paragraph"
    run_case "a body keyword with no commit trailer is allowed, and is NAMED" pass \
        "Fixes #100" \
        "chore: tidy"
    # The excision shape: the body still promises a ticket whose commits are gone. It
    # PASSES -- refusing would break the ordinary case above -- but the note has to be
    # there, because this is the direction that closes something undelivered.
    run_case "an excised ticket leaves the body promising it" pass \
        "Fixes #12
Fixes #13" \
        "feat: a

Fixes #12"
    run_case "Closes and Fixes are both keywords" pass \
        "Closes #12
Fixes #13" \
        "feat: a

Closes #12

fix: b

Fixes #13"
    # `Closes #A and #B` closes only #A, which is the scar in team-run.md. The checker
    # must read it the way GitHub does or it is more permissive than the thing it
    # stands for: here the BODY names only #12, so #13 is missing and this refuses.
    run_case "'Closes #A and #B' in the body covers only #A" refuse \
        "Closes #12 and #13" \
        "feat: a

Closes #12

fix: b

Closes #13"
    # A commit that NARRATES a closing rather than ordering one (#1013). It still
    # refuses, and must: the phrase is a directive to GitHub whatever the sentence
    # around it means, and no reading of the text can separate the two intents.
    #
    # What #1013 changed is the ADVICE. The old message named one cause and told this
    # author to add the ticket to the body -- which would close a ticket the branch
    # never delivered, the worse of the two failures. So the assertion is that ONE
    # refusal carries BOTH remedies; against the old message the second substring is
    # absent and this case goes red.
    run_case "the refusal names both causes, because it cannot tell them apart" refuse \
        "Prose about the change, naming no ticket." \
        "docs(rules): repoint an entry that went stale

The earlier pull request closed #904, so the entry describing it went stale." \
        "Add a line per ticket to the body" \
        "REWORD the prose and add nothing."
    # Case sensitivity: GitHub accepts `fixes`, `FIXES`, `Fixed`.
    run_case "keyword case and tense do not matter" pass \
        "fixes #42" \
        "fix: x

FIXED #42"
    # selftest-data: end

    echo "check-pr-closing-keywords self-test: $cases case(s) ran, $failures failure(s)"
    [ "$failures" -eq 0 ] || exit 1
    exit 0
fi

mode=""
case "${1:-}" in
    --pr) mode="judge" ;;
    --list-closing) mode="list" ;;
    *) usage; exit 2 ;;
esac
[ -n "${2:-}" ] || { usage; exit 2; }
pr="$2"

command -v gh >/dev/null 2>&1 || {
    echo "check-pr-closing-keywords: no gh on PATH, so NOTHING was verified" >&2
    exit "$SkipUnavailable"
}

# A LIVENESS anchor before the question, so "the API said no trailers" and "the API did
# not answer" are two outcomes rather than one silent pass. `rate_limit` is the cheapest
# authenticated call there is and needs no repository scope.
gh api rate_limit >/dev/null 2>&1 || {
    echo "check-pr-closing-keywords: the GitHub API did not answer, so NOTHING was verified" >&2
    exit "$SkipUnavailable"
}

record="$(gh pr view "$pr" --repo "$REPO" --json body,commits 2>/dev/null)" || record=""
[ -n "$record" ] || {
    echo "check-pr-closing-keywords: could not read pull request #${pr} from ${REPO}" >&2
    exit "$SkipUnavailable"
}

body="$(jq -r '.body // ""' <<< "$record")"
commits="$(jq -r '[.commits[] | ((.messageHeadline // "") + "\n" + (.messageBody // ""))] | join("\n")' <<< "$record")"

# An empty commit list is the API answering something this check cannot judge -- a pull
# request always has at least one commit -- so it is a refusal to conclude rather than a
# pass over nothing. Two empty lists agree perfectly.
[ -n "$commits" ] || {
    echo "check-pr-closing-keywords: #${pr} reported no commits at all, which cannot be true" >&2
    exit "$SkipUnavailable"
}

# The UNION of both sides, deliberately. `Judge` already refuses a pull request whose
# body omits what its commits name, so by the time both checks pass the body is a
# superset -- but a caller asking "what will this close" must not depend on the other
# check having run first.
if [ "$mode" = "list" ]; then
    { ClosingKeywords "$commits"; ClosingKeywords "$body"; } | sort -un
    exit 0
fi

echo "check-pr-closing-keywords: ${REPO}#${pr}"
Judge "$body" "$commits"
