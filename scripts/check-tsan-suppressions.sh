#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# `.tsan-suppressions` says what it is for, and nothing checked it (#1266).
#
# The file's own header states two rules. Every entry is a known bug that is still
# open and must name the issue tracking it. Removing an entry is part of fixing its
# issue -- because if the fix lands and the entry stays, the next regression of that
# exact race is invisible, which is the failure the whole gate exists to prevent,
# rebuilt one layer up.
#
# Both rules were prose. `scripts/tsan-gate.sh` prints ThreadSanitizer's own
# `Matched N suppressions` header and, since it learned to, the per-entry lines under
# it -- so the DATA to check them against has been on the gate's stdout, unread, with
# no reader anywhere. The only two references to the file in this tree are the gate
# setting `TSAN_OPTIONS` and a comment in `tsan-canary-rate.sh`.
#
# ## A dead entry is the finding, and it is silent by construction
#
# A suppression that matches nothing is a rule that has stopped applying. It costs
# nothing today and it is a standing licence: the day that race comes back, it is
# suppressed and the gate is green. Nothing about a dead entry is visible in any
# output -- a total of 5 and a total of 5 with one entry contributing 0 are the same
# number, which is why the per-entry lines had to exist before this could be written.
#
# ## Four outcomes, not two
#
# `NOTHING-TO-RECONCILE`, `NO-DATA`, `RECONCILED` and a refusal are four states and
# this reports them apart, because three of them are ways of not having checked and
# collapsing any two makes a run that verified nothing read like one that did.
#
#   NOTHING-TO-RECONCILE  the file holds no entries. This is master's resting state
#                         -- the file is 34 lines of header and no rules -- so it is
#                         the answer this check gives most often, and it is the one
#                         most easily mistaken for a pass. It says so in as many
#                         words instead.
#   NO-DATA               entries exist and the log carries no suppression output, so
#                         nothing is known about them. Not a verdict either way.
#   RECONCILED            every entry matched at least once.
#   refusal               an entry matched nothing, the log names one the file does
#                         not have, or the header disagrees with the per-entry sum.
#
# ## Usage
#
#   check-tsan-suppressions.sh <source-dir> [--log <tsan-gate-log>]
#   check-tsan-suppressions.sh --self-test <source-dir>
#
# Without `--log` the entry rules are checked and the reconciliation is reported as
# NOT RUN, which is the third state again: an entry that names no issue is a defect
# in the file whether or not a sanitizer ran this week.
#
# Exit: 0 when nothing is wrong, 1 on a finding. The verdict is also in the OUTPUT --
# every terminal line starts with one of the words above -- because a wrapper reading
# only a status cannot tell NO-DATA from RECONCILED.

set -u

selfTest=0
root=""
log=""

while [ "$#" -gt 0 ]; do
    case "${1:-}" in
        --self-test) selfTest=1 ;;
        --log)
            shift
            [ "$#" -gt 0 ] || { printf 'check-tsan-suppressions: --log needs a path\n' >&2; exit 2; }
            log="$1"
            ;;
        --log=*) log="${1#--log=}" ;;
        -*)
            printf 'check-tsan-suppressions: unknown option %s\n' "$1" >&2
            exit 2
            ;;
        *) root="$1" ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# The entries of a suppressions file, one per line, as `type:pattern`.
#
# TSan's format is one rule per line with `#` comments on their own lines. Read with
# a `while read` rather than `mapfile`, which is bash 4: macOS ships a 2007 /bin/bash
# and this runs there.
SuppressionEntries() {
    local file="$1" line
    while IFS= read -r line || [ -n "$line" ]; do
        # Trim leading blanks FIRST, then decide. A rule is not indented, but an
        # indented one is tolerated rather than silently dropped -- and testing
        # before the trim as well was two tests for one question, the first of them
        # wholly covered by the second.
        line="${line#"${line%%[![:space:]]*}"}"
        case "$line" in
            '#'*|'') continue ;;
        esac
        printf '%s\n' "$line"
    done < "$file"
}

# The issue an entry names, taken from the comment block immediately ABOVE it.
#
# Not from the entry's own line: TSan reads the whole line as the pattern, so a
# trailing `# 1234` would silently become part of the symbol being matched and the
# entry would stop suppressing anything. The convention the file documents is a
# comment, and a comment is where this has to look.
IssueForEntry() {
    local file="$1" wanted="$2" line trimmed pending=""
    while IFS= read -r line || [ -n "$line" ]; do
        trimmed="${line#"${line%%[![:space:]]*}"}"
        case "$trimmed" in
            # A blank line ENDS a comment block: the convention is the comment
            # immediately above the rule, and a paragraph two blank lines up is
            # about something else.
            '') pending=""; continue ;;
            '#'*) pending="${pending} ${trimmed}"; continue ;;
        esac
        if [ "$trimmed" = "$wanted" ]; then
            printf '%s\n' "$pending"
            return 0
        fi
        pending=""
    done < "$file"
    printf '\n'
}

# ---------------------------------------------------------------------------
# The reconciliation. PURE with respect to everything but the two files it is handed,
# so the self-test drives every outcome over staged text in milliseconds.
#
# @param 1 the suppressions file. @param 2 the log, or the empty string.
# Prints the report. Returns 0 when nothing is wrong.
Reconcile() {
    local file="$1" logFile="$2"
    local entries entry count matched total sum=0 findings=0 seen entryCount=0
    local line pattern

    if [ ! -f "$file" ]; then
        printf 'FAIL check-tsan-suppressions: no suppressions file at %s\n' "$file" >&2
        printf '     The gate passes this path to ThreadSanitizer, so a run without it is\n' >&2
        printf '     one whose suppressions are whatever TSan defaults to. That is not a\n' >&2
        printf '     clean tree and must not report as one.\n' >&2
        return 1
    fi

    entries="$(SuppressionEntries "$file")"

    # Every entry names its issue. The file says so and nothing enforced it.
    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        entryCount=$((entryCount + 1))
        case "$(IssueForEntry "$file" "$entry")" in
            *'#'[0-9]*) : ;;
            *)
                printf 'FAIL %s names no issue in the comment above it\n' "$entry" >&2
                findings=$((findings + 1))
                ;;
        esac
    done <<EOF
$entries
EOF

    if [ "$entryCount" -eq 0 ]; then
        # The state master is in, and the one that most looks like a pass. Named,
        # because "0 dead entries" and "no entries" are the same number.
        printf 'NOTHING-TO-RECONCILE check-tsan-suppressions: %s holds no entries, so there is\n' "$file"
        printf '     no rule here to be dead and nothing was reconciled. This is not a statement\n'
        printf '     about any sanitizer run.\n'
        return 0
    fi

    if [ "$findings" -ne 0 ]; then
        printf 'FAIL check-tsan-suppressions: %d entry/entries name no issue.\n' "$findings" >&2
        printf '     Every entry is a known bug that is still OPEN, and the issue number is how\n' >&2
        printf '     anybody finds out whether it still is. Put it in a comment on the line\n' >&2
        printf '     ABOVE the rule -- never on the rule itself, which TSan reads as part of the\n' >&2
        printf '     pattern, so a trailing comment stops the entry matching anything at all.\n' >&2
        return 1
    fi

    if [ -z "$logFile" ]; then
        printf 'NO-DATA check-tsan-suppressions: %d entry/entries, each naming an issue. No log\n' "$entryCount"
        printf '     was given, so whether any of them still MATCHES is unknown and was not\n'
        printf '     checked. Pass --log <gate log> to reconcile.\n'
        return 0
    fi

    if [ ! -r "$logFile" ]; then
        printf 'FAIL check-tsan-suppressions: cannot read the log %s\n' "$logFile" >&2
        return 1
    fi

    # ThreadSanitizer prints
    #     ThreadSanitizer: Matched 5 suppressions (pid=NNN):
    #     2 race_top:FastCache::BlockingListener::Close
    # and `tsan-gate.sh` greps both shapes onto its own stdout, so either the gate log
    # or a raw sanitizer log can be handed here.
    # EVERY header, summed -- not the first one.
    #
    # The log handed here is several target runs CONCATENATED: `tsan-gate.sh` loops
    # `TARGETS` (four rows) and appends each target's log, and every one of them is a
    # separate process printing its own `Matched N suppressions` header. Reading one
    # header and summing every per-entry line compares a number from the first run
    # against a total over all four, so an entry matching in two targets made
    # `sum -ne total` fire -- a FAIL saying "this is a defect in how they are read
    # here", correctly, about this reader, turning a CLEAN gate red.
    #
    # It was latent only because `.tsan-suppressions` is empty: `entryCount == 0`
    # returns NOTHING-TO-RECONCILE first. The first entry anybody added -- the
    # documented way to record a known race -- would have hit it.
    #
    # The invariant that does hold across a concatenation: each run's header equals
    # the sum of ITS OWN per-entry lines, so the sum of headers equals the sum of all
    # per-entry lines. Summing also restores the other direction the first-header read
    # lost -- a parse defect in targets two through four was invisible, because their
    # headers were never looked at.
    total=""
    while IFS= read -r headerCount; do
        [ -n "$headerCount" ] || continue
        total=$((${total:-0} + headerCount))
    done <<EOF
$(sed -n 's/.*Matched \([0-9][0-9]*\) suppressions.*/\1/p' "$logFile")
EOF
    matched="$(sed -n 's/^\([0-9][0-9]*\) \([a-z_][a-z_]*:.*\)$/\1 \2/p' "$logFile")"

    if [ -z "$total" ] && [ -z "$matched" ]; then
        printf 'NO-DATA check-tsan-suppressions: %s carries no ThreadSanitizer suppression output,\n' "$logFile"
        printf '     so nothing is known about the %d entry/entries. A run that reported none is\n' "$entryCount"
        printf '     not a run in which none matched -- print_suppressions=1 has to be on, and the\n'
        printf '     gate sets it.\n'
        return 0
    fi

    # Dead entries: in the file, absent from what TSan matched.
    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        seen=0
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            pattern="${line#* }"
            [ "$pattern" = "$entry" ] && seen=1
        done <<EOF
$matched
EOF
        if [ "$seen" -eq 0 ]; then
            printf 'FAIL %s matched NOTHING in this run\n' "$entry" >&2
            findings=$((findings + 1))
        fi
    done <<EOF
$entries
EOF

    # ... and the other direction: matched, and not in the file. TSan cannot report a
    # suppression it was not given, so this means the gate ran against a DIFFERENT
    # file -- which makes every verdict above a statement about the wrong rules.
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        count="${line%% *}"
        pattern="${line#* }"
        sum=$((sum + count))
        seen=0
        while IFS= read -r entry; do
            [ -n "$entry" ] || continue
            [ "$pattern" = "$entry" ] && seen=1
        done <<EOF
$entries
EOF
        if [ "$seen" -eq 0 ]; then
            printf 'FAIL %s was matched and is not in %s\n' "$pattern" "$file" >&2
            findings=$((findings + 1))
        fi
    done <<EOF
$matched
EOF

    # The headers against the per-entry sum. Every run in this log accounts for its own
    # matches twice -- once in its header, once in its entry lines -- so the two totals
    # must agree however many runs were concatenated. A disagreement means one of the
    # two was parsed wrongly here: a reader's defect, reported as one rather than
    # resolved by preferring whichever number is bigger.
    if [ -n "$total" ] && [ -n "$matched" ] && [ "$sum" -ne "$total" ]; then
        printf 'FAIL check-tsan-suppressions: the headers total %s matches and the per-entry lines sum to %d\n' "$total" "$sum" >&2
        printf '     Every run in this log states its own total and then lists what made it up,\n' >&2
        printf '     so these two count the same events. A disagreement is a defect in how they\n' >&2
        printf '     are read here, not a finding about the tree. Do not pick the larger number.\n' >&2
        findings=$((findings + 1))
    fi

    if [ "$findings" -ne 0 ]; then
        printf 'FAIL check-tsan-suppressions: %d finding(s) reconciling %s against %s.\n' "$findings" "$file" "$logFile" >&2
        printf '     A suppression that matches NOTHING is a rule that has stopped applying, and\n' >&2
        printf '     it costs nothing until the day that race comes back -- when it is suppressed\n' >&2
        printf '     and the gate is green. Removing an entry is part of fixing its issue.\n' >&2
        printf '     If the entry is still needed and has merely stopped matching, the accessing\n' >&2
        printf '     frame has been inlined: name the inlining frame. Do NOT widen `race_top:` to\n' >&2
        printf '     `race:`, which silences every future race passing through that function.\n' >&2
        return 1
    fi

    printf 'RECONCILED check-tsan-suppressions: %d entry/entries, each naming an issue and each\n' "$entryCount"
    printf '     matched at least once (%d match(es) in %s).\n' "$sum" "$logFile"
    return 0
}

# ---------------------------------------------------------------------------
# Self-test. Every outcome, over staged input -- which is not a convenience here: the
# tree supplies NONE of them. `.tsan-suppressions` is empty, so the only state a real
# run can reach is NOTHING-TO-RECONCILE, and a self-test that ran against the
# repository would exercise one arm of four and pass.
# ---------------------------------------------------------------------------
Cases=0
Failures=0
Ok() { Cases=$((Cases + 1)); printf 'ok   %s\n' "$1"; }
Bad() {
    Cases=$((Cases + 1))
    Failures=$((Failures + 1))
    printf 'FAIL %s\n' "$1"
    [ -z "${2:-}" ] || printf '%s\n' "$2" | sed 's/^/     /'
}

# @param 1 case name. @param 2 expected leading word. @param 3 expected exit status.
# @param 4 a phrase the output must contain. @param 5 suppressions text. @param 6 log text.
Case() {
    local name="$1" wantWord="$2" wantRc="$3" phrase="$4" supp="$5" logText="$6"
    local dir out rc word logArg
    dir="$(mktemp -d)" || { printf 'mktemp failed\n' >&2; exit 1; }
    printf '%s' "$supp" > "$dir/.tsan-suppressions"
    logArg=""
    if [ "$logText" != "-" ]; then
        printf '%s' "$logText" > "$dir/gate.log"
        logArg="$dir/gate.log"
    fi
    out="$(Reconcile "$dir/.tsan-suppressions" "$logArg" 2>&1)" && rc=0 || rc=$?
    rm -rf "$dir"

    word="${out%%[ $'\n']*}"
    # FLATTENED before the phrase is looked for. These reports wrap, so a phrase can
    # exist in the output and in no single LINE of it -- which is how case 2 first
    # failed against a correct reconciler, on a sentence broken across two lines by
    # the very formatting this file chose.
    out="$(printf '%s' "$out" | tr '\n' ' ' | tr -s ' ')"
    if [ "$word" != "$wantWord" ]; then
        Bad "$name: expected the report to start with '$wantWord', got '$word'" "$out"
        return
    fi
    if [ "$rc" -ne "$wantRc" ]; then
        Bad "$name: expected exit $wantRc, got $rc" "$out"
        return
    fi
    case "$out" in
        *"$phrase"*) Ok "$name" ;;
        *) Bad "$name: the $word verdict was right but the words were not (wanted '$phrase')" "$out" ;;
    esac
}

RunSelfTest() {
    local live dead twoDead

    # An entry that names its issue, and one that does not.
    live='# a known race, tracked as #1207
race_top:FastCache::Alpha::Close
'
    dead='# a known race, tracked as #1208
race_top:FastCache::Beta::Detach
'
    twoDead="${live}${dead}"

    # 1 -- master's own state, and the one that reads most like a pass.
    Case "case 1: an empty suppressions file is NOTHING-TO-RECONCILE, not clean" \
        "NOTHING-TO-RECONCILE" 0 "no rule here to be dead" \
        "# only a header
# and no rules at all
" "-"

    # 2 -- entries and no log. Not a verdict about them, and it says which.
    Case "case 2: entries with no log are NO-DATA, not reconciled" \
        "NO-DATA" 0 "was not checked" "$live" "-"

    # 3 -- a log with no suppression output at all. The SAME word as case 2 and a
    # different sentence: a run that reported none is not a run in which none matched.
    Case "case 3: a log carrying no suppression output is NO-DATA" \
        "NO-DATA" 0 "carries no ThreadSanitizer suppression output" "$live" \
        "All tests passed (12 assertions in 3 test cases)
"

    # 4 -- the accepting direction. Without it a check that refused everything would
    # pass every case below.
    Case "case 4: an entry that matched is RECONCILED" \
        "RECONCILED" 0 "each naming an issue" "$live" \
        "ThreadSanitizer: Matched 2 suppressions (pid=41):
2 race_top:FastCache::Alpha::Close
"

    # 5 -- THE ticket: an entry that matched nothing. Invisible in every number the
    # gate prints, because a total of 2 and a total of 2 with a dead entry beside it
    # are the same total.
    Case "case 5: an entry matching nothing is refused and NAMED" \
        "FAIL" 1 "race_top:FastCache::Beta::Detach matched NOTHING" "$twoDead" \
        "ThreadSanitizer: Matched 2 suppressions (pid=41):
2 race_top:FastCache::Alpha::Close
"

    # 6 -- and the refusal says what to do instead of widening the rule, which is the
    # repair somebody reaches for when an entry stops matching.
    Case "case 6: the refusal warns against widening race_top to race" \
        "FAIL" 1 "Do NOT widen" "$twoDead" \
        "ThreadSanitizer: Matched 2 suppressions (pid=41):
2 race_top:FastCache::Alpha::Close
"

    # 7 -- the other direction: TSan matched something this file does not contain, so
    # the run used a different suppressions file and every other verdict here is about
    # the wrong rules.
    Case "case 7: a matched suppression absent from the file is refused" \
        "FAIL" 1 "is not in" "$live" \
        "ThreadSanitizer: Matched 3 suppressions (pid=41):
2 race_top:FastCache::Alpha::Close
1 race_top:FastCache::Gamma::Run
"

    # 8 -- the file's own rule, which was prose until now: an entry with no issue.
    Case "case 8: an entry naming no issue is refused" \
        "FAIL" 1 "names no issue" \
        "# no issue number here
race_top:FastCache::Alpha::Close
" "-"

    # 9 -- the issue must be in a comment ABOVE the rule and not on it. A trailing
    # comment becomes part of the pattern, so an entry spelled that way suppresses
    # nothing -- accepting it would be this check endorsing a dead rule.
    Case "case 9: an issue number on the rule's own line does not count" \
        "FAIL" 1 "names no issue" \
        "race_top:FastCache::Alpha::Close # 1207
" "-"

    # 10 -- the header against the per-entry sum. Both come from one run, so a
    # disagreement is this reader's defect and is reported as one.
    Case "case 10: a header disagreeing with the per-entry sum is refused" \
        "FAIL" 1 "defect in how they are read here" "$live" \
        "ThreadSanitizer: Matched 9 suppressions (pid=41):
2 race_top:FastCache::Alpha::Close
"

    # 12-14 -- SEVERAL runs concatenated, which is the only input the gate ever
    # produces and the one no case here could represent.
    #
    # `tsan-gate.sh` appends one log per `TARGETS` row -- four of them -- and each is a
    # separate process with its own `Matched N suppressions` header. Every case above
    # carries exactly ONE header, which is why reading the first header and summing all
    # the entry lines passed eleven cases and would have turned the first real
    # suppressions file's gate red. The fixture could not express the input; that is
    # the finding behind the finding.
    Case "case 12: an entry matching in two targets is RECONCILED, not a false mismatch" \
        "RECONCILED" 0 "each naming an issue" "$live" \
        "ThreadSanitizer: Matched 2 suppressions (pid=41):
2 race_top:FastCache::Alpha::Close
ThreadSanitizer: Matched 3 suppressions (pid=77):
3 race_top:FastCache::Alpha::Close
"

    # And the arm still BITES across a concatenation -- a sum fix that stopped
    # detecting anything would pass case 12 perfectly.
    Case "case 13: a genuine header/entry mismatch is still caught across two runs" \
        "FAIL" 1 "defect in how they are read here" "$live" \
        "ThreadSanitizer: Matched 2 suppressions (pid=41):
2 race_top:FastCache::Alpha::Close
ThreadSanitizer: Matched 9 suppressions (pid=77):
3 race_top:FastCache::Alpha::Close
"

    # A defect in a LATER run's header -- targets two through four were never looked
    # at before. NOT a discriminator for that defect, and saying so is the point:
    # measured, the first-header read also FAILS this input, by different arithmetic
    # that happens to land on a refusal. What discriminates are cases 12 and 15, which
    # the defect turns from RECONCILED into FAIL. This one asserts the arm still bites
    # on a later run at all.
    Case "case 14: a mismatch in the SECOND run alone is caught" \
        "FAIL" 1 "defect in how they are read here" "$live" \
        "ThreadSanitizer: Matched 2 suppressions (pid=41):
2 race_top:FastCache::Alpha::Close
ThreadSanitizer: Matched 4 suppressions (pid=77):
2 race_top:FastCache::Alpha::Close
"

    # And an entry alive in only ONE of the targets is alive: a dead entry is one that
    # matched in NONE of them, which is what makes the concatenation the right unit.
    Case "case 15: an entry matching in only one target is not dead" \
        "RECONCILED" 0 "each naming an issue" "$twoDead" \
        "ThreadSanitizer: Matched 1 suppressions (pid=41):
1 race_top:FastCache::Alpha::Close
ThreadSanitizer: Matched 1 suppressions (pid=77):
1 race_top:FastCache::Beta::Detach
"

    # 11 -- a missing file. The gate hands this path to TSan, so a run without it is a
    # run with whatever TSan defaults to.
    Cases=$((Cases + 1))
    local out rc
    out="$(Reconcile "/nonexistent/path/.tsan-suppressions" "" 2>&1)" && rc=0 || rc=$?
    if [ "$rc" -eq 1 ] && [ "${out#FAIL}" != "$out" ]; then
        printf 'ok   case 11: a missing suppressions file is refused\n'
    else
        Failures=$((Failures + 1))
        printf 'FAIL case 11: a missing suppressions file was not refused (rc=%s)\n' "$rc"
    fi

    printf '\nself-test: %d case(s) ran, %d failed\n' "$Cases" "$Failures"
    [ "$Failures" -eq 0 ]
}

if [ "$selfTest" -eq 1 ]; then
    RunSelfTest
    exit $?
fi

[ -n "$root" ] || { printf 'check-tsan-suppressions: a source directory is required\n' >&2; exit 2; }
Reconcile "${root}/.tsan-suppressions" "$log"
