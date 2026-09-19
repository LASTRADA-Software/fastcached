#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# `AGENT.md` stays within the budget in `scripts/agent-md-budget.txt`.
#
# ## Why
#
# `AGENT.md` is loaded in FULL into every session -- `CLAUDE.md` is one line,
# `@AGENT.md` -- so every line is a line every session pays for whether or not it
# needs the rule. It reached 2,411 lines, 1,878 of them its own rulebook index,
# with 286 of 379 tripwire bullets running to three lines or more and the longest
# to 36, while stating the rule that growth broke: *a tripwire that has acquired
# measurements, ticket archaeology or a counter-argument has stopped being one.*
#
# Prose did not hold it and re-reading did not hold it, which is the argument for
# a derivation rather than a better sentence -- the same argument
# `RowsInEnumeratorOrder` makes for enum tables and `check-table-totals.sh` makes
# for a figure beside a table.
#
# ## Two measures, because either alone is gameable
#
# **`max-bullet-lines` is primary, because it cannot be satisfied by deleting a
# rule.** A file-size limit can always be met by dropping a bullet, and that is
# the one thing this budget must never encourage: *a rule landed in
# `.agent/rules/` without a bullet here fires in no session that does not open its
# file.* A per-bullet ceiling can only be met by moving the DERIVATION to the
# matching rules file, where length is explicitly fine. A check with only the
# total would score deleting a rule as an improvement.
#
# **`total-lines` is an EQUALITY, not a ceiling.** More is growth and is refused.
# FEWER is a number that has stopped describing the file, refused as stale, so a
# compression is recorded in the change that makes it and the slack it created is
# not room for the next arrival. A ceiling banks slack; an equality tightens. That
# is `check-test-loops-backlog.txt`'s ratchet on a different subject.
#
# ## The number lives in ONE file, and AGENT.md may not quote its own size
#
# `check-test-loops-backlog.txt` records this defect by name against itself: *"The
# count was spelled out here as `148 sites` and went stale the first time a batch
# landed."* So the figures live in `agent-md-budget.txt` alone, every run PRINTS
# the measured ones, and this check REFUSES `total-lines` appearing in AGENT.md
# other than behind a `#` -- a size claim in the file whose size it describes is a
# second source of truth, and `#1605` is an issue reference rather than a claim.
#
# ## What this does NOT cover, stated because a reader will over-apply it
#
#   * It does not budget `.agent/rules/*.md`, deliberately and permanently.
#     AGENT.md says reasoning there can be as long as it needs to be, so
#     budgeting them would contradict the rule this enforces and push derivations
#     back into the file this keeps small.
#   * It does not judge whether a bullet is a good tripwire, only how long it is.
#     `ctest -R rulebook-tripwires` is the check that asks whether a rule has one
#     at all.
#   * A bullet can satisfy the ceiling and still be derivation, by being wrapped
#     tighter. Nothing mechanical catches that.
#
# Usage:
#   bash scripts/check-agent-md-budget.sh [<repo-root>]
#   bash scripts/check-agent-md-budget.sh --self-test
#
# Exit: 0 clean, 1 a rule was broken, 2 usage.

set -uo pipefail

# The measuring pass. One awk over AGENT.md, no shell loop over lines: this file
# is dense with `|` inside tables and `[#876](...)`, and every shell splitting
# idiom in this repository has been bitten by one or the other.
#
# A bullet's LINES are its own -- the `- ` line plus the blank and indented lines
# under it, stopping at the first line that is neither. That boundary is not
# cosmetic: a standalone PARAGRAPH after a bullet (AGENT.md has one, the
# backwards-compatibility policy) belongs to no bullet, and a rule that ran to the
# next bullet attributed 12 of its lines to the tripwire above it and reported a
# 14-line bullet that is two lines long.
#
# NOTE: no apostrophes anywhere in this block -- it is a single-quoted shell
# string, and one ends it.
MeasureAwk='
{ lines[NR] = $0 }
function extent(j,   n, k, l) {
    n = 1
    for (k = j + 1; k <= NR; k++) {
        l = lines[k]
        if (l ~ /^- / || l ~ /^ +- /) break
        if (l ~ /^[ \t]*$/) { n++; continue }
        if (l ~ /^ /) { n++; continue }
        break
    }
    while (n > 1 && lines[j + n - 1] ~ /^[ \t]*$/) n--
    return n
}
END {
    fence = 0
    inRulebook = 0
    for (i = 1; i <= NR; i++) {
        if (lines[i] ~ /^[ \t]*(```|~~~)/) { fence = !fence; continue }
        if (fence) continue
        # The budget is about the RULEBOOK index, which is the part that grows.
        if (lines[i] ~ /^## /) { inRulebook = (lines[i] == "## The rulebook") }
        if (!inRulebook) continue
        if (lines[i] ~ /^- /)   { top++;    n = extent(i) }
        else if (lines[i] ~ /^ +- /) { nest++; n = extent(i) }
        else continue
        if (n > MaxLines) printf "OVER %d %d %s\n", i, n, substr(lines[i], 1, 72)
        if (n > worst) worst = n
        hist[n]++
    }
    printf "COUNTS %d %d %d\n", top, nest, worst
    # Formatted HERE rather than reassembled by the shell: an awk array has no
    # order, and the first version paired the lines up with `paste - -`, which
    # silently drops the last entry when the count is odd.
    line = ""
    for (k = 1; k <= worst; k++)
        if (hist[k]) line = line sprintf(" %dx%d", hist[k], k)
    printf "HIST%s\n", line
}
'

NotCovered='  This check measures HOW LONG a rulebook bullet is, nothing else.
  It does NOT budget .agent/rules/*.md -- deliberately and permanently, because
  AGENT.md says reasoning there can be as long as it needs to be, and budgeting
  those files would push derivations back into the file this keeps small.
  It does NOT judge whether a bullet is a good tripwire (that is
  `ctest -R rulebook-tripwires`), and a bullet wrapped tighter can satisfy the
  ceiling and still be derivation.'

RunCheck() {
    local root="$1" budget agent out line rest
    local overLine="" overCount="" overText=""
    local maxLines="" totalLines="" key value
    local top=0 nest=0 worst=0 measured=0 failures=0

    budget="$root/scripts/agent-md-budget.txt"
    agent="$root/AGENT.md"
    if [[ ! -f "$budget" ]]; then
        echo "check-agent-md-budget: no $budget -- refusing rather than reporting clean." >&2
        return 1
    fi
    if [[ ! -f "$agent" ]]; then
        echo "check-agent-md-budget: no $agent -- refusing rather than reporting clean. The subject being absent and the budget being met are not the same green." >&2
        return 1
    fi

    # A row this reader cannot split is REFUSED, never skipped: a key read as
    # empty waves the measure it governs straight through.
    while IFS= read -r line; do
        case "$line" in
            ''|'#'*) continue ;;
        esac
        key="${line%%|*}"
        value="${line#*|}"
        if [[ "$key" == "$line" || -z "$key" || -z "$value" ]]; then
            echo "check-agent-md-budget: cannot read budget row: $line" >&2
            return 1
        fi
        case "$key" in
            max-bullet-lines) maxLines="$value" ;;
            total-lines)      totalLines="$value" ;;
            *) echo "check-agent-md-budget: unknown budget key \`$key\`. An unrecognised key is refused rather than ignored -- a row nothing reads is a budget nobody enforces." >&2
               return 1 ;;
        esac
    done < "$budget"

    if [[ -z "$maxLines" || -z "$totalLines" ]]; then
        echo "check-agent-md-budget: the budget must set both \`max-bullet-lines\` and \`total-lines\`." >&2
        return 1
    fi
    case "$maxLines$totalLines" in
        *[!0-9]*) echo "check-agent-md-budget: budget values must be numbers, got max=$maxLines total=$totalLines" >&2; return 1 ;;
    esac

    local awkrc=0
    out="$(awk -v MaxLines="$maxLines" "$MeasureAwk" "$agent" 2>&1)" || awkrc=$?
    if [[ "$awkrc" -ne 0 ]]; then
        echo "check-agent-md-budget: awk failed on $agent (status $awkrc): $out" >&2
        return 1
    fi

    local hist=""
    while IFS= read -r line; do
        case "$line" in
            COUNTS\ *) set -- $line; top="$2"; nest="$3"; worst="$4" ;;
            HIST*)     hist="${line#HIST}" ;;
            OVER\ *)
                # Parameter expansion, NOT `set -- $line`: the trailing field is
                # the bullet text and these bullets open with `**`, which an
                # unquoted expansion would glob against the working directory.
                rest="${line#OVER }"
                overLine="${rest%% *}"; rest="${rest#* }"
                overCount="${rest%% *}"; overText="${rest#* }"
                echo "  AGENT.md:${overLine}: this bullet is ${overCount} lines, over the $maxLines-line ceiling: ${overText}" >&2
                echo "      Move the derivation to the matching .agent/rules/*.md, where length is fine. Do NOT delete the bullet and do NOT raise the ceiling: a rule with no bullet here fires in no session that does not open its file." >&2
                failures=$((failures + 1))
                ;;
            '') ;;
            *) echo "  $line" >&2; failures=$((failures + 1)) ;;
        esac
    done <<< "$out"

    measured="$(wc -l < "$agent" | tr -d ' ')"

    # Zero is not a verdict, it is the absence of one, and both of these read
    # exactly like a tree that is within budget.
    if [[ "$top" -eq 0 ]]; then
        echo "check-agent-md-budget: found no rulebook bullet in $agent. A census that finds nothing has not passed, it has failed to look -- the \`## The rulebook\` heading or the \`- \` bullet shape has changed." >&2
        return 1
    fi

    echo "AGENT.md budget: $measured line(s) against $totalLines; $top top-level and $nest nested rulebook bullet(s), longest $worst against a ceiling of $maxLines"
    echo "  bullet lines:${hist:- none}"

    # The ratchet. More is growth; fewer is a number that has stopped describing
    # the file. Both are refused, and the two messages differ because they are
    # fixed in different places.
    if [[ "$measured" -gt "$totalLines" ]]; then
        echo "  AGENT.md has grown to $measured lines, over the recorded $totalLines." >&2
        echo "      Compress a bullet -- move its derivation into the matching .agent/rules/*.md. Raising the number in scripts/agent-md-budget.txt is not the answer; the ratchet turns one way." >&2
        failures=$((failures + 1))
    elif [[ "$measured" -lt "$totalLines" ]]; then
        echo "  AGENT.md is $measured lines and scripts/agent-md-budget.txt still records $totalLines." >&2
        echo "      That number has stopped describing the file. Record $measured in the same change that made the file smaller, or the slack becomes room for the next arrival." >&2
        failures=$((failures + 1))
    fi

    # The number lives in ONE file. `#1605` is an issue reference; a bare 1605 in
    # a file whose length it describes is a second source of truth.
    # Anchored at BOTH ends: `[^#0-9]N[^0-9]` cannot match the number at the start
    # or the end of a line, so a line reading `1610 lines of rules` would have
    # walked straight past -- a guard failing toward *nothing unusual here*.
    local selfSize="(^|[^#0-9])${totalLines}([^0-9]|$)"
    if grep -nE "$selfSize" "$agent" > /dev/null 2>&1; then
        echo "  AGENT.md contains the figure $totalLines outside an issue reference:" >&2
        grep -nE "$selfSize" "$agent" | sed 's/^/        /' >&2
        echo "      AGENT.md must never quote its own size. The budget lives in scripts/agent-md-budget.txt and this check PRINTS the measured figures on every run; a number restated in prose goes stale the first time anybody edits the file." >&2
        failures=$((failures + 1))
    fi

    # Not a refusal: lowering the ceiling is a decision somebody makes, and a
    # second equality would fail the build on every unrelated edit to the longest
    # bullet. But it must be SAID, or the ceiling banks slack in silence.
    if [[ "$worst" -lt "$maxLines" ]]; then
        echo "  note: the longest bullet is now $worst lines; the ceiling could be lowered to $worst in scripts/agent-md-budget.txt."
    fi

    if [[ "$failures" -ne 0 ]]; then
        printf '%s\n' "$NotCovered" >&2
        return 1
    fi
    return 0
}

# ---------------------------------------------------------------------------
# The self-test. Synthetic trees, one per verdict, driven in BOTH directions --
# a guard nobody has watched ACCEPT is not known to work either, and a suite
# whose only case is a refusal passes under a check that refuses everything.
#
# It prints how many cases ran, because a run that died half way through is
# otherwise indistinguishable from one where everything passed.
SelfTest() {
    local scratch ran=0 failures=0
    scratch="$(mktemp -d)" || { echo "check-agent-md-budget: mktemp -d failed" >&2; exit 2; }
    trap 'rm -rf "$scratch"' EXIT

    # @param 1 case name
    # @param 2 expected outcome: `clean` or `refused`
    # @param 3 budget file body
    # @param 4 AGENT.md body
    # @param 5.. text the output must contain; a leading `!` means must NOT
    Case() {
        local name="$1" want="$2" budget="$3" agent="$4"; shift 4
        local dir out rc=0 pattern got
        ran=$((ran + 1))
        dir="$scratch/case-$ran"
        mkdir -p "$dir/scripts"
        printf '%s\n' "$budget" > "$dir/scripts/agent-md-budget.txt"
        printf '%s' "$agent" > "$dir/AGENT.md"
        out="$(RunCheck "$dir" 2>&1)" || rc=$?
        got="clean"; [[ "$rc" -eq 0 ]] || got="refused"
        if [[ "$got" != "$want" ]]; then
            echo "  FAIL ${name}: expected ${want}, got ${got}" >&2
            printf '%s\n' "$out" | sed 's/^/       | /' >&2
            failures=$((failures + 1))
            return
        fi
        for pattern in ${1+"$@"}; do
            if [[ "${pattern:0:1}" == "!" ]]; then
                if [[ "$out" == *"${pattern:1}"* ]]; then
                    echo "  FAIL ${name}: output contains '${pattern:1}' and must not" >&2
                    printf '%s\n' "$out" | sed 's/^/       | /' >&2
                    failures=$((failures + 1)); return
                fi
            elif [[ "$out" != *"$pattern"* ]]; then
                echo "  FAIL ${name}: output lacks '${pattern}'" >&2
                printf '%s\n' "$out" | sed 's/^/       | /' >&2
                failures=$((failures + 1)); return
            fi
        done
        echo "  ok   ${name}"
    }

    # 7 lines, two 2-line bullets.
    local _ok='# T

## The rulebook

- A short rule, stated once.
- Another short rule, also stated
  in two lines.
'
    local _budget='max-bullet-lines|3
total-lines|7'

    # The ACCEPT direction first, deliberately.
    Case "a file inside both measures is clean" clean "$_budget" "$_ok" \
        "7 line(s) against 7" "2 top-level" "longest 2"

    # The PRIMARY measure: a bullet over the ceiling. Ceiling 1, because `_ok`'s
    # longest bullet is 2 -- a ceiling of 2 refuses nothing here, which is a case
    # that passes for the wrong reason.
    Case "a bullet over the per-bullet ceiling is refused" refused \
'max-bullet-lines|1
total-lines|7' "$_ok" \
        "over the 1-line ceiling" "Do NOT delete the bullet and do NOT raise the ceiling"

    # The ratchet, upward: growth.
    Case "a file that has grown is refused" refused \
'max-bullet-lines|3
total-lines|5' "$_ok" \
        "has grown to 7 lines, over the recorded 5" "the ratchet turns one way"

    # ... and downward, which is what makes it tighten instead of bank slack.
    Case "a file that shrank without recording it is refused as stale" refused \
'max-bullet-lines|3
total-lines|9' "$_ok" \
        "still records 9" "stopped describing the file"

    # AGENT.md may never quote its own size.
    Case "AGENT.md quoting its own size is refused" refused \
'max-bullet-lines|3
total-lines|8' '# T

## The rulebook

This file is 8 lines long.
- A short rule, stated once.
- Another one.
' \
        "must never quote its own size"

    # ... at the START of a line too. The first version anchored on a character
    # either side, so a size claim opening a line was invisible to it.
    Case "a size claim at the start of a line is refused" refused \
'max-bullet-lines|3
total-lines|8' '# T

## The rulebook

8 lines, and counting.
- A short rule, stated once.
' \
        "must never quote its own size"

    # ... but an ISSUE reference carrying the same digits is not a size claim.
    Case "an issue reference with the same digits is not a size claim" clean \
'max-bullet-lines|3
total-lines|8' '# T

## The rulebook

- A short rule, stated once (#8).
- Another one, also short.

x
' \
        "!must never quote its own size"

    # Zero is not a verdict. Both ways this can fail to LOOK read exactly like a
    # tree within budget, so both are watched refusing.
    Case "a rulebook with no bullet is refused, not reported clean" refused \
'max-bullet-lines|3
total-lines|4' '# T

## The rulebook
' \
        "failed to look"

    # An EMPTY AGENT.md and a MISSING one are different faults with different
    # messages, and conflating them is the state collapse this repository keeps
    # writing rules about. Both are watched, and the empty one goes first because
    # it is what the fixture naturally produces.
    Case "an empty AGENT.md is refused as a failure to look" refused "$_budget" "" \
        "failed to look"
    rm -f "$scratch/case-$ran/AGENT.md"
    local out2 rc2=0
    out2="$(RunCheck "$scratch/case-$ran" 2>&1)" || rc2=$?
    ran=$((ran + 1))
    if [[ "$rc2" -eq 0 ]]; then
        echo "  FAIL a tree with no AGENT.md is refused: reported clean" >&2
        failures=$((failures + 1))
    elif [[ "$out2" != *"no "*"AGENT.md"* ]]; then
        echo "  FAIL a tree with no AGENT.md: wrong reason: $out2" >&2
        failures=$((failures + 1))
    else
        echo "  ok   a tree with no AGENT.md names that as the reason"
    fi

    # A bullet is measured by ITS OWN lines. A standalone paragraph after it
    # belongs to no bullet -- the first version of this check ran a bullet to the
    # next bullet and reported a two-line tripwire as fourteen lines.
    Case "a standalone paragraph after a bullet is not part of it" clean \
'max-bullet-lines|2
total-lines|10' '# T

## The rulebook

- A short rule, stated once.

A standalone policy paragraph that
belongs to no bullet at all, and
runs for several lines without
being anybody constraint.
' \
        "longest 1"

    # Nested sub-bullets are measured separately and against the same ceiling, so
    # a bullet carrying genuine sub-rules is several rules, not one long one.
    Case "a nested sub-bullet is measured on its own" clean \
'max-bullet-lines|2
total-lines|7' '# T

## The rulebook

- A parent rule, stated once.
  - A sub-rule, stated once.
  - Another sub-rule.
' \
        "1 top-level and 2 nested"

    # A budget row this reader cannot split waves its measure through.
    Case "an unreadable budget row is refused" refused \
'max-bullet-lines 3
total-lines|7' "$_ok" \
        "cannot read budget row"

    Case "an unknown budget key is refused, not ignored" refused \
'max-bullet-lines|3
total-lines|7
max-words|900' "$_ok" \
        "unknown budget key"

    # Only the rulebook section is budgeted; a bullet elsewhere is not a tripwire.
    Case "a long bullet outside the rulebook section is not measured" clean \
'max-bullet-lines|2
total-lines|12' '# T

## Something else

- A bullet here may be long,
  running on for several
  lines, because it is not
  a rulebook tripwire.

## The rulebook

- A short rule.
' \
        "1 top-level" "longest 1"

    echo "check-agent-md-budget --self-test: ${ran} cases ran, ${failures} failed"
    [[ "$failures" -eq 0 ]] || exit 1
    exit 0
}

case "${1:-}" in
    --self-test) SelfTest ;;
    -h|--help)   echo "usage: $0 [<repo-root>] | --self-test"; exit 0 ;;
esac

Root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
RunCheck "$Root"
