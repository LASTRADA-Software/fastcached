#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# A rulebook table with a stated total has two sources of truth wearing one hat:
# the rows, and a hand-maintained number in the prose above them. Editing the
# table does not update the number, nothing checked that it did, and the failure
# is silent -- the sentence still reads correctly (#780).
#
# Measured, three consecutive commits, one table -- `.agent/rules/metrics-and-
# observability.md`'s four-states collapse table:
#
#   8ee9983a  "five separate times, in four different instruments"  7 instruments, 10 collapses
#   5ecb2537  "ten collapses across seven instruments"              right for the table; a further
#                                                                   instance was in PROSE and never
#                                                                   added to it
#   562eb0fe  "twelve collapses across eight instruments"           correct
#
# Each commit fixed the previous count and introduced the next. All three were
# made by the same author, in the same file, on the same day, with the rule about
# instruments that miscount open in the next tab. Prose did not prevent it and
# re-reading did not prevent it, which is the argument for a derivation -- the
# same argument `RowsInEnumeratorOrder` makes for C++ enum tables: the count comes
# from the table or it is not a count, it is a second claim.
#
# ## Two decisions, recorded here rather than in a commit message
#
# **(a) The multipliers are PARSED, not banned.** The table that motivated this
# carries them -- `a CI watcher (twice)`, `a merge-readiness checker (three
# times)` -- so 8 rows describe 12 collapses, and the prose states BOTH figures.
# A checker that counts rows is wrong for exactly the table that motivated it,
# and wrong in the direction that looks right: `8` beside a table of 8 rows is
# entirely plausible, so it would go green on a wrong number. The alternative --
# restating the rule as *state the row count only* -- was rejected because it
# deletes a true and load-bearing figure: twelve is the count that section's
# argument rests on, and eight is a different fact about the same rows. Two
# quantities, two modes (`rows` and `weighted`), both derived.
#
# A multiplier this script cannot read is a REFUSAL, never a silent 1. A new
# spelling defaulting to 1 would undercount in silence, which is the defect.
#
# **(b) Scope is every markdown table under `.agent/`, and the marker is
# MANDATORY.** Not opt-in. An opt-in marker is exact about the tables it knows
# and silent about the ones it does not, and silence reads identically to
# complete coverage -- #492, which is a rule in this repository already. So a
# table with no marker is refused, and a table that states no total says so
# explicitly with `none`.
#
# That is the `Refuse` / `RefuseWithoutCounter` idiom from
# `.agent/rules/metrics-and-observability.md`, one directory over: *deliberately
# uncounted must not be spelled like forgot*. `none` costs one comment line and
# buys the property that a table ARRIVING cannot join the tree unchecked.
# The one-time cost is one line per table with no total, and every run PRINTS the
# live census -- read THAT line rather than a figure restated here, which is this
# check's own rule applied to its own header. (The number that used to sit here
# said 10 tables: the anchored first draft could not see a table indented inside
# a bullet, and the real count is twice that. A restated figure drifts; a printed
# one cannot.) `docs/` is deliberately out: ~140 tables, reference material rather
# than argument, and no instance of this defect.
#
# **What is deliberately NOT checked:** `AGENT.md`'s tripwire restates *"five
# times in four instruments in one session"*. That figure is not derived from the
# table -- it describes the FIRST session, five of the twelve -- so asserting it
# against the table would assert a wrong thing. It agrees with the rule file's
# own opening sentence, and prose-agreement between two files is #534's
# mechanism, not this one. Recorded because picking silently is what the ticket
# refuses.
#
# ## The marker
#
#   <!-- table-total: none -->                      this table states no total
#   <!-- table-total: instruments=rows -->          "<N> instruments" in the prose
#                                                   above must equal the row count
#   <!-- table-total: collapses=weighted -->        ... must equal the sum of the
#                                                   rows' multipliers
#
# Comma-separated for a table stating more than one. The marker carries NO
# number: it names the noun to look for and how to count, and the figure is read
# from the PROSE and compared to the TABLE. A marker holding the number would be
# a third source of truth, which is the defect with an extra step.
#
# The prose is whitespace-normalised before the number is looked for, because a
# phrase WRAPPED ACROSS A LINE reading as absent is itself a row of the table
# this check exists to protect.
#
# Usage:
#   bash scripts/check-table-totals.sh [<repo-root>]
#   bash scripts/check-table-totals.sh --self-test
#
# Exit: 0 clean, 1 a rule was broken, 2 usage.

set -uo pipefail

# Number words this check can read, in prose and in a multiplier. Digits are read
# too. A figure spelled in a word this table lacks is a REFUSAL rather than a
# skipped assertion -- an unreadable number and a correct one must not look alike.
NumberWords="one=1 two=2 three=3 four=4 five=5 six=6 seven=7 eight=8 nine=9 ten=10
eleven=11 twelve=12 thirteen=13 fourteen=14 fifteen=15 sixteen=16 seventeen=17
eighteen=18 nineteen=19 twenty=20"

# How many words may sit between a stated figure and the noun the marker names.
# 3, because "the six remaining file(STRINGS) readers" is two, and a marker forced
# to spell the intervening words would be a copy of the sentence -- a third source
# of truth, which is the defect with an extra step.
GapWords=3

# The awk program is the whole check. One pass per file, no shell loop over lines:
# a markdown table is dense with `[#501](...)` and `|`, and every shell splitting
# idiom in this repository has been bitten by one or the other.
CheckAwk='
BEGIN {
    n = split(WORDS, parts, /[ \n]+/)
    for (i = 1; i <= n; i++) { split(parts[i], kv, "="); NUM[kv[1]] = kv[2] + 0 }
    # The bare multiplier words, a table rather than a branch each: the next
    # spelling is a row, and one this table lacks is refused rather than counted
    # as 1.
    MULT["twice"] = 2
    MULT["thrice"] = 3
}
function num(w) {
    if (w ~ /^[0-9]+$/) return w + 0
    return (w in NUM) ? NUM[w] : -1
}
# Every refusal is one printed line, which the CALLER counts. No second counter:
# an awk-side tally and the shell-side one were a single fact counted twice, and
# the shell-side one is the stronger -- it also catches anything awk writes that
# function did not.
function refuse(line, msg) {
    printf "%s:%d: %s\n", FILENAME, line, msg
}
# The multiplier a body row carries. 1 unless it says otherwise; -1 means the row
# says something this check cannot read, which is refused rather than defaulted.
function multiplier(row,   rest, openAt, closeAt, group, m, found, tok) {
    found = 0; m = 1; rest = row
    while ((openAt = index(rest, "(")) > 0) {
        rest = substr(rest, openAt + 1)
        closeAt = index(rest, ")")
        if (closeAt == 0) break
        group = substr(rest, 1, closeAt - 1)
        rest = substr(rest, closeAt + 1)
        if (group in MULT) { tok = MULT[group] }
        else if (group ~ /^[A-Za-z0-9]+ times$/) {
            sub(/ times$/, "", group)
            tok = num(group)
            if (tok < 0) return -1
        }
        else if (group ~ /times/) { return -1 }
        else continue
        # Two multipliers in one row is ambiguous, not additive.
        if (found++) return -1
        m = tok
    }
    return m
}
# Leading whitespace is allowed everywhere a table line is recognised. A table
# INSIDE a markdown bullet is indented, and an `^\|` anchor walks straight past
# it -- measured on this repository: the anchored first draft found 10 tables and
# was silent about 3 more, one of them the census table in
# `.agent/rules/build-and-toolchain.md` cited by this very ticket. NOTE: no
# apostrophes in this awk block -- it is a single-quoted shell string, and one
# ends it. A
# check that is exact about the tables it can see and silent about the rest is
# #492 rebuilt inside the fix for it.
{ lines[NR] = $0 }
END {
    # Fenced code blocks are skipped: a ``` block may legitimately SHOW a
    # markdown table, and demanding a marker inside one would be a refusal a
    # reader cannot act on.
    fence = 0
    for (i = 1; i <= NR; i++) {
        # A running toggle rather than an NR-sized array: the answer is only ever
        # read in the same order it is written.
        if (lines[i] ~ /^[ \t]*```/) { fence = !fence; continue }
        if (fence || i < 2) continue
        if (lines[i] !~ /^[ \t]*\|[ :|+-]+\|[ \t]*$/) continue
        if (lines[i - 1] !~ /^[ \t]*\|/) continue
        tables++
        # Body rows: contiguous `|` lines after the separator.
        rows = 0; weighted = 0; bailed = 0
        for (j = i + 1; j <= NR && lines[j] ~ /^[ \t]*\|/; j++) {
            rows++
            m = multiplier(lines[j])
            if (m < 0) {
                refuse(j, "a multiplier this check cannot read. Rows say `(twice)`, `(thrice)`, `(<number> times)` -- one per row. An unreadable one is refused rather than counted as 1, because a new spelling silently defaulting to 1 undercounts in exactly the direction #780 is about.")
                bailed = 1
                continue
            }
            weighted += m
        }
        # The marker, within the three lines above the header row.
        marker = ""; markerLine = 0
        for (j = i - 2; j >= 1 && j >= i - 4; j--) {
            if (lines[j] ~ /<!--[ ]*table-total:/) {
                marker = lines[j]; markerLine = j; break
            }
        }
        if (marker == "") {
            refuse(i - 1, "this table carries no `<!-- table-total: ... -->` marker. Every table under .agent/ must state whether its prose gives a total, because an opt-in marker is silent about a table that never opted in (#492). A table with no total says so: `<!-- table-total: none -->`.")
            continue
        }
        markers++
        sub(/^.*<!--[ ]*table-total:[ ]*/, "", marker)
        sub(/[ ]*-->.*$/, "", marker)
        if (marker == "none") { noneMarkers++; continue }
        if (bailed) continue

        # The prose above the marker: back to a heading, a previous table, or 25
        # lines, whichever comes first -- normalised to one space, because a phrase
        # wrapped across a line reading as absent is a row of the very table this
        # protects.
        prose = ""
        for (j = markerLine - 1; j >= 1 && j >= markerLine - 25; j--) {
            if (lines[j] ~ /^#/) break
            if (lines[j] ~ /^[ \t]*\|/) break
            prose = lines[j] " " prose
        }
        gsub(/[ \t]+/, " ", prose)
        gsub(/[*`_]/, "", prose)

        nClaims = split(marker, claims, /[ ]*,[ ]*/)
        for (c = 1; c <= nClaims; c++) {
            claim = claims[c]
            eq = index(claim, "=")
            if (eq == 0) {
                refuse(markerLine, "marker claim `" claim "` is not `<noun>=rows` or `<noun>=weighted`.")
                continue
            }
            noun = substr(claim, 1, eq - 1)
            mode = substr(claim, eq + 1)
            if (mode == "rows") want = rows
            else if (mode == "weighted") want = weighted
            else {
                refuse(markerLine, "marker claim `" claim "` names mode `" mode "`, which is neither `rows` nor `weighted`. An unrecognised mode is refused rather than guessed.")
                continue
            }
            # `<number> <noun>` in the prose. The noun may be several words.
            # Find the NOUN, then the nearest readable number in the GapWords
            # before it. Not "the word immediately before": the prose that
            # motivated the gap reads "the six remaining file(STRINGS) readers",
            # where the count and its noun are two words apart, and requiring
            # adjacency would have forced the marker to spell the intervening
            # words -- turning the marker into a copy of the sentence, which is a
            # third source of truth. The gap is small and the noun must match
            # exactly, so a number belonging to a different phrase cannot reach.
            # The LAST occurrence of the noun, not the first: a rulebook
            # paragraph legitimately states an OLDER figure with the same noun.
            # Measured on the table that motivated this ticket, whose paragraph
            # carries both "five separate times, in four different instruments"
            # (the first session, five of the twelve) and "twelve collapses
            # across eight instruments" (the table) -- and a first-match search
            # read the table as claiming 4. **The figure that describes a table is
            # the one nearest it**, which is also where a writer puts it.
            stated = -1
            nounSeen = 0
            n = split(prose, w, " ")
            nounWords = split(noun, nw, " ")
            for (q = 1; q <= nounWords; q++) gsub(/[^A-Za-z0-9-]/, "", nw[q])
            for (k = n; k >= 1 && stated < 0; k--) {
                ok = 1
                for (q = 1; q <= nounWords; q++) {
                    got = w[k + q - 1]
                    gsub(/[^A-Za-z0-9-]/, "", got)
                    if (got != nw[q]) { ok = 0; break }
                }
                if (!ok) continue
                nounSeen = 1
                for (g = 1; g <= GapWords && k - g >= 1; g++) {
                    cand = w[k - g]
                    gsub(/[^A-Za-z0-9]/, "", cand)
                    v = num(cand)
                    if (v >= 0) { stated = v; statedWord = cand; break }
                }
            }
            # Two faults, two messages. The phrase being GONE means the sentence
            # was reworded or the marker is stale; the phrase being PRESENT with
            # no readable number beside it means the figure is spelled in a word
            # this check has no row for. They are fixed in different places, and
            # a check about four states must not collapse its own two.
            if (stated < 0 && !nounSeen) {
                refuse(markerLine, "the phrase `" noun "` does not appear in the prose above this table, so there is no figure to check against it. Either the sentence was reworded, or the marker is stale. (Searched back to the previous heading or table, whitespace-normalised.)")
                continue
            }
            if (stated < 0) {
                refuse(markerLine, "`" noun "` appears above this table, but no number this check can read sits within " GapWords " words before it. A figure that cannot be READ is refused rather than skipped -- an unreadable one and a correct one must not look alike. Spell it in digits, or in a word the number table carries.")
                continue
            }
            if (stated != want) {
                refuse(markerLine, "the prose says " stated " " noun " (`" statedWord "`), the table has " want " (" mode "). The number is not maintained beside the table it describes -- it is derived from it, or it is a second claim (#780).")
                continue
            }
            checked++
        }
    }
    printf "SUMMARY %d %d %d %d\n", tables, markers, noneMarkers, checked
}
'

RunCheck() {
    local root="$1" f out tables=0 markers=0 none=0 checked=0 failures=0 line
    local files
    files="$(find "$root/.agent" -name '*.md' -type f 2>/dev/null | sort)"
    if [[ -z "$files" ]]; then
        echo "check-table-totals: no markdown files under $root/.agent -- refusing rather than reporting clean." >&2
        return 1
    fi
    while IFS= read -r f; do
        [[ -n "$f" ]] || continue
        # The status is CHECKED. An awk that refused to parse writes nothing to
        # stdout, and the `tables == 0` guard below would then report "failed to
        # look" -- fail-closed and correct, and naming the wrong cause. A reader
        # would go hunting for a missing table.
        local awkrc=0
        out="$(awk -v WORDS="$NumberWords" -v GapWords="$GapWords" "$CheckAwk" "$f" 2>&1)" || awkrc=$?
        if [[ "$awkrc" -ne 0 ]]; then
            echo "check-table-totals: awk failed on $f (status $awkrc): $out" >&2
            return 1
        fi
        while IFS= read -r line; do
            case "$line" in
                SUMMARY\ *)
                    set -- $line
                    tables=$((tables + $2)); markers=$((markers + $3))
                    none=$((none + $4)); checked=$((checked + $5))
                    ;;
                "") ;;
                *) echo "  $line" >&2; failures=$((failures + 1)) ;;
            esac
        done <<< "$out"
    done <<< "$files"

    # Zero rows is not a verdict, it is the absence of one. Both of these mean the
    # check failed to LOOK, and both read exactly like a clean tree if they pass.
    if [[ "$tables" -eq 0 ]]; then
        echo "check-table-totals: found no markdown table under $root/.agent. A census that finds nothing has not passed, it has failed to look." >&2
        return 1
    fi
    if [[ "$markers" -eq 0 ]]; then
        echo "check-table-totals: found $tables table(s) and not one marker. The marker syntax has stopped matching, which reads exactly like a tree with no totals to check." >&2
        return 1
    fi

    echo "table totals: $tables table(s) under .agent/, $markers marked ($none stating no total), $checked figure(s) asserted against their table"
    [[ "$failures" -eq 0 ]] || return 1
    return 0
}

# ---------------------------------------------------------------------------
# The self-test. Synthetic `.agent/` trees, one per verdict, driven in BOTH
# directions -- a suite with only a correct-count case passes under every bug
# this check exists to catch, which is the ticket's own acceptance clause.
#
# It prints how many cases ran. Without that, a run that died half way through is
# indistinguishable from one where everything passed, and "no failures printed"
# reads as "the guard did not fire" (#584, one file over).
SelfTest() {
    local scratch ran=0 failures=0
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT

    # @param 1 case name
    # @param 2 expected outcome: `clean` or `refused`
    # @param 3 file body under .agent/rules/t.md
    # @param 4.. text the output must contain; a leading `!` means must NOT
    Case() {
        local name="$1" want="$2" body="$3"; shift 3
        local dir out rc=0 pattern got
        ran=$((ran + 1))
        dir="$scratch/case-$ran"
        mkdir -p "$dir/.agent/rules"
        printf '%s\n' "$body" > "$dir/.agent/rules/t.md"
        out="$(RunCheck "$dir" 2>&1)" || rc=$?
        got="clean"; [[ "$rc" -eq 0 ]] || got="refused"
        if [[ "$got" != "$want" ]]; then
            echo "  FAIL ${name}: expected ${want}, got ${got}" >&2
            printf '%s\n' "$out" | sed 's/^/       | /' >&2
            failures=$((failures + 1))
            return
        fi
        # `${1+"$@"}`: before bash 4.4 an empty `$@` is an unbound expansion
        # under `set -u`, and this runs on macOS 3.2.
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

    # The shape that motivated the ticket: 3 rows, one carrying `(twice)` and one
    # `(three times)`, so 3 instruments and 6 collapses. `rows` and `weighted`
    # DISAGREE here on purpose -- a fixture where they coincide cannot tell a
    # checker that counts rows from one that parses multipliers, which is the one
    # distinction the ticket turns on.
    _tbl='| instrument | what |
|---|---|
| a watcher (twice) | x |
| a checker (three times) | y |
| a classifier | z |'

    Case "a correct pair of figures is clean" clean \
"So the table stands at **six collapses across three instruments**:

<!-- table-total: instruments=rows, collapses=weighted -->
$_tbl" \
        "2 figure(s) asserted"

    # The failure the ticket measured three times running: the table gained a row
    # and the sentence above it did not.
    Case "a stated total that no longer describes the table is refused" refused \
"So the table stands at **six collapses across two instruments**:

<!-- table-total: instruments=rows, collapses=weighted -->
$_tbl" \
        "the prose says 2 instruments" "the table has 3 (rows)"

    # The direction that looks right and is wrong: counting ROWS for a figure the
    # prose gives in COLLAPSES. 3 is a plausible number beside a table of 3 rows.
    Case "a weighted figure counted as rows is refused" refused \
"So the table stands at **three collapses across three instruments**:

<!-- table-total: collapses=weighted -->
$_tbl" \
        "the prose says 3 collapses" "the table has 6 (weighted)"

    # The #492 hole this design closes: a table that never opted in.
    Case "a table with no marker at all is refused" refused \
"So the table stands at **six collapses across three instruments**:

$_tbl" \
        "carries no" "table-total"

    # ... and its counterpart, which is what makes the mandatory marker bearable:
    # a table that genuinely states no total says so, and is clean.
    Case "a table stating no total says so and is clean" clean \
"Measured on clang-cl 22.1.3, one binary:

<!-- table-total: none -->
$_tbl" \
        "1 stating no total" "0 figure(s) asserted"

    # `(thrice)` and a digit multiplier are rows of the multiplier table just as
    # `(twice)` is, and a row nothing exercises is a row that can be deleted
    # without a test noticing. 3 + 4 + 1 = 8 against 3 rows, so the two modes
    # disagree here too.
    Case "thrice and a digit multiplier are read" clean \
"It stands at **eight collapses across three instruments**:

<!-- table-total: instruments=rows, collapses=weighted -->
| instrument | what |
|---|---|
| a watcher (thrice) | x |
| a checker (4 times) | y |
| a classifier | z |" \
        "2 figure(s) asserted"

    # A multiplier spelling this check cannot read must not quietly count as 1.
    Case "an unreadable multiplier is refused, never counted as one" refused \
"So the table stands at **six collapses across three instruments**:

<!-- table-total: instruments=rows, collapses=weighted -->
| instrument | what |
|---|---|
| a watcher (seventy-nine times) | x |
| a checker | y |" \
        "a multiplier this check cannot read"

    # The phrase WRAPPED ACROSS A LINE. Reading a present phrase as absent is
    # literally a row of the table this check protects, so it is a case here.
    Case "a total wrapped across a line is still found" clean \
"So the table stands at **six collapses across
three instruments**:

<!-- table-total: instruments=rows, collapses=weighted -->
$_tbl" \
        "2 figure(s) asserted"

    # An unrecognised mode renders as unrecognised rather than as the nearest
    # plausible one -- a `weighted` typo silently read as `rows` would pass here
    # and be wrong by exactly the multiplier.
    Case "an unrecognised counting mode is refused, not guessed" refused \
"So the table stands at **six collapses across three instruments**:

<!-- table-total: collapses=wieghted -->
$_tbl" \
        "neither" "rows" "weighted"

    # A marker whose phrase is not in the prose at all. Skipping it silently
    # would make every marker optional again, one level down.
    Case "a marker whose phrase is not in the prose is refused" refused \
"Nothing above states a count.

<!-- table-total: instruments=rows -->
$_tbl" \
        "does not appear in the prose" "!cannot read"

    # ... and the OTHER fault, which the same message used to swallow: the phrase
    # is there and the figure beside it is spelled in a word this check has no
    # row for. A reworded sentence and an unreadable figure are fixed in
    # different places.
    Case "a figure spelled in an unreadable word is refused, and says which fault" refused \
"It happened **seventy-nine instruments** times over:

<!-- table-total: instruments=rows -->
$_tbl" \
        "no number this check can read" "!does not appear"

    # A table INDENTED inside a bullet. The first draft anchored on `^|` and
    # found 10 of the 20 tables in this repository, silently -- which is the
    # #492 defect inside the fix for it. Measured while writing this, and only
    # because one missed table happened to catch the eye.
    Case "a table indented inside a bullet is found" refused \
"- A bullet whose table has **six collapses across three instruments**:

    $(printf '%s' "$_tbl" | sed 's/^/    /')" \
        "carries no" "table-total"

    # A ``` block may legitimately SHOW a markdown table. Demanding a marker
    # inside one is a refusal a reader cannot act on.
    Case "a table inside a fenced code block is not a table" refused \
'```
| instrument | what |
|---|---|
| a watcher | x |
```' \
        "failed to look"

    # The figure NEAREST the table wins. A rulebook paragraph legitimately
    # states an older figure with the same noun -- the table that motivated this
    # ticket carries both "four different instruments" (the first session) and
    # "eight instruments" (the table), and a first-match search read the table as
    # claiming 4. Found by this check refusing a CORRECT tree.
    Case "the figure nearest the table is the one that describes it" clean \
"It happened **five times, in two instruments**; the table now stands at
**six collapses across three instruments**:

<!-- table-total: instruments=rows, collapses=weighted -->
$_tbl" \
        "2 figure(s) asserted"

    # ... and the number need not be the word immediately before the noun.
    Case "a figure a few words from its noun is still read" clean \
"Measured across the **three remaining \`file(STRINGS)\` instruments**:

<!-- table-total: instruments=rows -->
$_tbl" \
        "1 figure(s) asserted"

    # Zero rows is not a verdict. Both of the ways this check can fail to LOOK
    # have to be seen refusing, because both read exactly like a clean tree: a
    # tree with no table, and a marker syntax that has stopped matching.
    Case "a tree with no table is refused, not reported clean" refused \
"Prose with no table at all." \
        "failed to look"

    Case "a tree where no table is marked is refused" refused \
"x

$_tbl" \
        "not one marker"

    echo "check-table-totals --self-test: ${ran} cases ran, ${failures} failed"
    [[ "$failures" -eq 0 ]] || exit 1
    exit 0
}

case "${1:-}" in
    --self-test) SelfTest ;;
    -h|--help)   echo "usage: $0 [<repo-root>] | --self-test"; exit 0 ;;
esac

Root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
RunCheck "$Root"
