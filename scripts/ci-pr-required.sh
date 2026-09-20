#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# What is the state of every REQUIRED context on one pull request, by name.
#
# ## Why this exists
#
# Two situations present identically in `gh pr checks` and need opposite
# responses (#542): a REQUIRED context that is skipped or absent is a merge
# blocker that will never clear on its own, and a NON-REQUIRED context that is
# failing frequently blocks nothing. Told apart by eye they collapse, and
# collapsing them is wrong in both directions -- it either blocks a mergeable
# pull request on a cosmetic failure, or it queues one whose required set will
# never complete. `gh pr checks` renders required and non-required
# typographically identically and has no idea which is which.
#
# ## Four states, and a count is not a verdict
#
# `AGENT.md`: skipped, absent, unstarted and failed are four states, and tooling
# collapses them. "10 of 11" does not say whether the eleventh is RUNNING or
# ABSENT, and those are a wait and a blocker. So every required context is
# printed by name with its own state, and the totals are evidence rather than the
# verdict.
#
# The enumeration is closed and RECONCILED: the per-state counts must sum to the
# number of required contexts, asserted on every run. A state this script does
# not know is `UNKNOWN` and refuses -- an unguarded default arm is where the last
# three collapses in this repository were introduced, each while fixing the one
# before it.
#
# ## The withdrawn case (#903)
#
# #903 describes a required context that reports and then leaves "an absence
# where a verdict was". **That state does not exist**, and this script is written
# against what does. Measured on the pair the ticket names (runs 34009668478 and
# 34009671900, SHA a2f8359): GitHub deletes nothing, so that commit still lists
# THREE `Require a type label` check runs -- one `cancelled` and two `success`.
# The observable is a cancelled verdict that was superseded, not a missing one.
#
# So the hazard is real under a different name, and it is a predicate rather than
# a state: a required context whose LATEST check run is `cancelled` with no newer
# run for it queued or in progress. A benign supersession fails that predicate by
# construction -- the replacement is either in flight or already the latest -- so
# this does not fire on every multi-label edit, which is the failure mode #903's
# third clause warns about and the one that gets a check disabled within a week.
#
# ## Acquisition and decision are separate
#
# `--record` takes the check-run record and calls no API, which is what lets the
# self-test drive every verdict without a network or a pull request. `--pr`
# fetches and then calls the same decision. Nothing in the decision reads a clock
# or a socket. This is the shape `ci-merge-group-report.sh` uses and is checked
# the same way.
#
# ## And a FIFTH state: the instrument could not ask
#
# The four above are states of a check run. This one is a state of the tool, and
# it is the one that gets folded in: an exhausted API budget returns an error,
# and a caller that reads the RESULT of a `--jq` pipeline gets an empty answer.
# Empty reads as *no required context is failing*. So "could not ask" is its own
# outcome with its own exit status (3) and its own wording, and it is never
# reported as ABSENT. The details, the measurement behind them and why there is
# no retry are on `Unavailable` below.
#
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

Fatal() { echo "ci-pr-required: $*" >&2; exit 2; }

# ---------------------------------------------------------------------------
# THE INSTRUMENT COULD NOT ASK -- its own outcome, exit 3, never folded into a
# verdict about the pull request.
#
# An exhausted API budget returns an ERROR, and a caller that pipes it through
# `--jq` and reads the RESULT gets an EMPTY ANSWER. An empty answer to *which
# required contexts are failing* reads as NONE ARE. That is this file's own
# four-states rule arriving through a new door: a query that could not RUN,
# presented as a query that found NOTHING.
#
# Measured 2026-09-10 on this account: `gh project item-list` and every other
# GraphQL call refused with `API rate limit exceeded for user ID 56763` while
# `gh api rate_limit` reported `graphql: 5000/5000`. So `rate_limit` is NOT a
# usable precondition check -- it answers for a different limit than the one that
# fired -- and the only honest readings are a real call's response headers or the
# refusal itself. This tool takes the second: it asks, and says so when what came
# back is not an answer.
#
# **There is no retry.** A retry makes an instrument's own failures disappear
# without fixing them, and this is the class of failure whose entire cost is
# being invisible. If one is ever added it must SAY that it retried and how many
# times, or the same silence comes back wearing a fix.
Unavailable() {
    echo "VERDICT: COULD NOT ASK -- $*"
    echo "  Nothing above was measured. This is a statement about the INSTRUMENT,"
    echo "  not about the pull request, and it is exit 3 so a caller cannot read it"
    echo "  as either a pass or a block."
    exit 3
}

# THE HEAD MOVED UNDER THE WAIT -- its own outcome, exit 4 (#1289).
#
# A fifth answer beside settled, running, absent and failed, and it is not a
# failure of the pull request. Under `--wait` every poll re-reads the head SHA
# from the SAME response that yields `mergeable_state`, so the two cannot
# describe different moments. If it has moved, every verdict this tool could
# print is about a commit that is no longer the head -- and the contexts of the
# NEW head have their own states nobody has looked at.
#
# It REFUSES rather than silently re-basing onto the new head. A waiter that
# follows the head is answering a question nobody asked: the caller asked about
# the tree it pushed, and a force-push during the wait means somebody else's
# tree is now under the same number. Reporting the new head's verdict under the
# old head's question is how a green is attributed to the wrong commit.
#
# @param 1 the SHA the wait started on  @param 2 the SHA it found
HeadMoved() {
    echo "VERDICT: HEAD MOVED -- the wait started on $1 and #${PrNumber:-?} now points at $2."
    echo "  Nothing above describes the current head. The contexts that were"
    echo "  running belong to $1, and $2 has its own, which nothing here has read."
    echo "  This is exit 4 so a caller cannot read it as a pass, a block, or an"
    echo "  instrument failure. Re-run against the new head deliberately."
    exit 4
}

# Has the head moved under the wait? Pure, and reachable from `--head-guard` for
# the same reason `ListingVerdict` is reachable from `--listing-verdict`: a
# decision reachable only through the network is one nobody has driven in either
# direction. A real force-push mid-poll cannot be arranged in a fixture, and this
# is the decision that force-push exists to trip.
#
# An EMPTY first SHA is the first poll, not a move -- there is nothing to have
# moved from. An empty CURRENT SHA is not a comparison at all and refuses, rather
# than reading as `same` and letting a wait continue against nothing.
#
# @param 1 the SHA the wait started on, empty on the first poll
# @param 2 the SHA this poll read
# @return prints `same`, `moved` or `refuse`
HeadGuard() {
    local first="$1" current="$2"
    [[ -n "$current" ]] || { echo "refuse"; return; }
    if [[ -z "$first" || "$first" == "$current" ]]; then echo "same"; else echo "moved"; fi
}

# Should the wait poll again? Pure, and reachable from `--wait-decision`.
#
# `stop-decided` is the clause the ticket names: a head that CONFLICTS does not
# clean itself, so it exits on the refusal it already has rather than polling to
# the timeout and printing the same thing. Only `merges` and `uncomputed` are
# still open -- `behind`, `conflicts`, `history` and `refuse` have all decided
# something waiting cannot change.
#
# **ABSENT IS NOT SETTLED, AND THAT IS WHY THIS TAKES FIVE INPUTS.** A required
# context that has not been dispatched is RUNNING=0 and ABSENT=1, and keying
# `stop-settled` on RUNNING alone read that as *everything concluded* -- so a
# `--wait` on a pull request whose checks had not started yet returned on poll 1
# announcing a settled verdict over contexts nothing had run. That is the
# four-state collapse this file exists to refuse, in the wait loop rather than in
# the tally. `absent` therefore holds the answer open exactly as `running` does.
#
# The three readings of ABSENT are three different facts, and the merge kind is
# what separates them:
#
#   * absent while the head MERGES -- CI has not dispatched yet, or a matrix leg
#     has not expanded. Open: `poll`, and `stop-timeout` once the ceiling is up.
#   * absent while the head CONFLICTS -- GitHub computes no merge ref for a
#     conflicting pull request and dispatches NOTHING, so these contexts will
#     never arrive and no ceiling is long enough. `stop-decided`, and the caller
#     says which of the two silences this was.
#   * nothing absent and nothing running -- every required context reported.
#     `stop-settled`, which is now a claim about both counts.
#
# @param 1 the merge kind from `MergeVerdict`
# @param 2 how many required contexts are RUNNING
# @param 3 how many required contexts are ABSENT
# @param 4 seconds already spent  @param 5 the ceiling
# @return prints `poll`, `stop-decided`, `stop-settled` or `stop-timeout`
WaitDecision() {
    local kind="$1" running="$2" absent="$3" waited="$4" ceiling="$5"
    case "$kind" in
        merges|uncomputed) ;;
        *) echo "stop-decided"; return ;;
    esac
    if [[ "$running" -le 0 && "$absent" -le 0 ]]; then echo "stop-settled"; return; fi
    [[ "$waited" -lt "$ceiling" ]] || { echo "stop-timeout"; return; }
    echo "poll"
}

# How many required contexts are in a given state, read from the decision's own
# TALLY line rather than recounted -- a second count of one thing is a second
# thing that can be wrong.
#
# One reader taking the STATE as a parameter, not one function per state: the
# tally prints seven and a second copy of this awk per state is seven chances to
# spell a name that is never emitted, which reads back as `-1` and refuses on a
# healthy tree. A state the tally does not carry is `-1`, the same answer as no
# TALLY at all, because both mean *this file does not say*.
#
# @param 1 the decision file  @param 2 the state name, as the TALLY spells it
TallyCount() {
    awk -F'\t' -v want="$2" '
        /^TALLY/ {
            for (i = 2; i <= NF; i++)
                if ($i ~ "^" want "=") { sub("^" want "=", "", $i); print $i + 0; found = 1 }
        }
        END { if (!found) print -1 }
    ' "$1"
}

# Did the listing answer the question that was asked?
#
# Pure, and reachable from `--listing-verdict` so the self-test drives it without
# a network: acquisition is where an instrument lies, and a decision reachable
# only through the API is one nobody has watched.
#
# `total` is taken as TEXT on purpose. An error body, an empty string and a
# number all arrive the same way, and the EMPTY case was a live hole here: an
# unset `total` made `[[ "$total" -gt "$got" ]]` compare 0 against 0, so a
# response that carried no total at all passed the cap guard and every required
# context then read ABSENT.
#
# @param 1 the `total_count` the API reported, as text
# @param 2 the number of rows actually read back
# @return prints one of: ok | unreadable-total | capped
ListingVerdict() {
    local total="$1" got="$2"
    case "$total" in
        ''|*[!0-9]*) echo "unreadable-total"; return ;;
    esac
    case "$got" in
        ''|*[!0-9]*) echo "unreadable-total"; return ;;
    esac
    if [ "$total" -gt "$got" ]; then
        echo "capped"
        return
    fi
    echo "ok"
}

# The positive control on the reader, guarded on the RAW input rather than on the
# result of the step it is checking.
#
# A control guarded on the parse cannot fire when the parse produces nothing --
# which is precisely when it is needed. A sibling instrument built the same
# evening reported `RED=0, absent=14` across seven pull requests, one of which
# was already red, because its rows never parsed; its control was guarded on the
# parsed set being non-empty and so was silent throughout.
#
# @param 1 how many check-run rows the response carried
# @param 2 how many of those rows carry a name from the required list
# @return prints one of: ok | no-rows | matched-nothing
ReaderControl() {
    local rows="$1" matched="$2"
    if [ "$rows" -eq 0 ]; then
        echo "no-rows"
        return
    fi
    if [ "$matched" -eq 0 ]; then
        echo "matched-nothing"
        return
    fi
    echo "ok"
}

# How many rows of a record carry a name from the required list.
# @param 1 the record file
# @param 2 the required-context list file
CountRequiredRows() {
    awk -F'\t' '
        NR == FNR { required[$0] = 1; next }
        ($1 in required) { n++ }
        END { print n + 0 }
    ' "$2" "$1"
}

# Run the control and refuse when the READER is the thing that is broken.
#
# An empty required list is `Decide`'s refusal to make, not this one's -- it has
# its own wording and its own exit -- so this returns without an opinion there.
#
# @param 1 the record file
# @param 2 the required-context list file
ApplyReaderControl() {
    local rows matched
    [ -s "$2" ] || return 0
    rows="$(awk 'END { print NR }' "$1")"
    matched="$(CountRequiredRows "$1" "$2")"
    case "$(ReaderControl "$rows" "$matched")" in
        ok)
            echo "control: ${matched} of ${rows} check run(s) carry a required context name"
            ;;
        no-rows)
            echo "control: no check runs at all at this SHA, so ABSENT below is genuinely absent rather than unread"
            ;;
        matched-nothing)
            Unavailable "the response carried ${rows} check run(s) and not ONE of them matched a required context name -- the reader and \`RequiredContexts\` have parted company. Reporting every context ABSENT from here would read as 'CI has not started yet'."
            ;;
        *)
            # `ReaderControl` answers from a closed set of three, so a fourth
            # value is a code defect rather than a state of the world. The arm
            # that swallows one silently is where the last three collapses in
            # this repository's CI tooling were introduced, each while fixing
            # the one before it -- so this arm refuses instead of falling out of
            # the `case` with nothing said.
            Unavailable "the reader control answered a word this script does not enumerate"
            ;;
    esac
}

# ---------------------------------------------------------------------------
# The required-context list is READ from `scripts/check-merge-queue-contexts.sh`
# rather than restated, for the reason that file's header gives: a second copy of
# the table is not a cross-check, it is a second thing to be wrong. Here it
# decides which contexts are allowed to block, so a drift would make this tool
# confidently wrong about the only question it answers.
#
# Overridable only so the self-test can stage a table; production passes nothing.
RequiredContextsFile="${FASTCACHED_REQUIRED_CONTEXTS_FILE:-scripts/check-merge-queue-contexts.sh}"

ReadRequiredContexts() {
    [[ -f "$RequiredContextsFile" ]] \
        || Fatal "$RequiredContextsFile does not exist; the required-context list has no source"
    awk '
        /^RequiredContexts=\(/ { inTable = 1; next }
        inTable && /^\)/       { inTable = 0 }
        # A ROW is a quoted line. Without this guard every comment inside the
        # table is read as a context name -- measured, 8 phantoms from the
        # comment above the last row, each then reported as a required context
        # that no job produces. A COMMENT is not a call site.
        inTable && /^[ \t]*"/ {
            line = $0
            sub(/^[ \t]*"/, "", line)
            sub(/"[ \t]*$/, "", line)
            sub(/\|.*$/, "", line)
            if (length(line)) print line
        }
    ' "$RequiredContextsFile"
}

# ---------------------------------------------------------------------------
# THE DECISION. A record in, lines out, nothing else.
#
# The record is one check run per line, `name<TAB>status<TAB>conclusion<TAB>started_at`,
# which is exactly what `--pr` asks the API for. Several rows may share a name:
# that is the ordinary state after any re-run, and picking among them is the
# whole of what this has to get right.
#
# `started_at` orders them. An empty conclusion is NOT a failure -- an unfinished
# job reports `""` rather than `null`, and reading that as FAILED already
# misclassified a context once (#542's own body).
Decide() {
    local record="$1" required="$2"
    awk -F'\t' -v recordFile="$record" -v requiredFile="$required" '
        function stateOf(name,   i, best, bestAt, inFlight, concl, st) {
            best = ""; bestAt = ""; inFlight = 0
            for (i = 1; i <= rows; i++) {
                if (rowName[i] != name) continue
                st = rowStatus[i]
                if (st == "queued" || st == "in_progress" || st == "pending" || st == "waiting")
                    inFlight = 1
                # Ties on an empty `started_at` keep the LAST row seen, which is
                # the API order; a queued run has no start time and must not be
                # allowed to outrank a concluded one by sorting empty high.
                if (bestAt == "" || (rowAt[i] != "" && rowAt[i] >= bestAt)) {
                    bestAt = rowAt[i]; best = i
                }
            }
            if (best == "") return "ABSENT"
            st = rowStatus[best]; concl = rowConcl[best]
            if (st != "completed") return "RUNNING"
            if (concl == "success")                       return "SUCCESS"
            if (concl == "skipped")                       return "SKIPPED"
            if (concl == "cancelled")
                # #903: superseded is a wait, never replaced is a blocker.
                return inFlight ? "RUNNING" : "WITHDRAWN"
            if (concl == "failure" || concl == "timed_out" ||
                concl == "action_required" || concl == "startup_failure" ||
                concl == "stale" || concl == "neutral")   return "FAILED"
            if (concl == "")                              return "RUNNING"
            return "UNKNOWN"
        }
        BEGIN {
            rows = 0
            while ((getline line < recordFile) > 0) {
                if (line == "") continue
                n = split(line, f, "\t")
                if (n < 2) { print "MALFORMED\t" line; malformed++; continue }
                rows++
                rowName[rows] = f[1]; rowStatus[rows] = f[2]
                rowConcl[rows] = (n >= 3 ? f[3] : "")
                rowAt[rows]    = (n >= 4 ? f[4] : "")
                seen[f[1]] = 1
            }
            close(recordFile)

            requiredCount = 0
            while ((getline name < requiredFile) > 0) {
                if (name == "") continue
                requiredCount++
                requiredName[requiredCount] = name
                isRequired[name] = 1
            }
            close(requiredFile)
            if (requiredCount == 0) {
                print "REFUSE\tthe required-context list is empty, so every verdict below would be vacuous"
                exit 3
            }

            for (i = 1; i <= requiredCount; i++) {
                s = stateOf(requiredName[i])
                print "REQUIRED\t" s "\t" requiredName[i]
                tally[s]++
            }

            # Non-required FAILURES, their own section. Omitted, a real regression
            # in an unrequired job goes unnoticed for a week (#684 is that failure
            # with nobody watching at all).
            for (i = 1; i <= rows; i++) {
                if (isRequired[rowName[i]]) continue
                if (rowStatus[i] != "completed") continue
                c = rowConcl[i]
                if (c == "failure" || c == "timed_out" || c == "action_required" ||
                    c == "startup_failure" || c == "stale")
                    if (!reported[rowName[i]]++) print "NONBLOCKING\t" c "\t" rowName[i]
            }

            # The reconciliation. Not a summary line: an enumeration that does not
            # add up is this script having lost a context, and it must say so
            # rather than print a plausible tally.
            #
            # **No RECORD can trigger this**, and that is stated rather than left
            # for somebody to discover: stateOf only ever returns a state the
            # print order below lists, so the assertion fires on a code defect and
            # on nothing else. That makes it unreachable from the self-test, which
            # drives records -- so it was watched refusing by hand instead, by
            # changing the ABSENT return in stateOf to an unlisted PENDING:
            #
            #   REFUSING: the printed states sum to 1 where 2 were counted, so a
            #   state is missing from the fixed print order
            #
            # A guard nobody has watched refuse is not a guard, and one whose
            # self-test cannot reach it is exactly where that goes unnoticed.
            #
            # (No apostrophes in this block, and that is not a style choice: the
            # whole program is a single-quoted shell string, so one apostrophe
            # ends it. Writing this comment with two closed the quote and took all
            # 12 self-test cases red at once, which is the loud direction.)
            #
            # Printed in a FIXED order from a closed list rather than by iterating
            # the tally, whose order awk does not define -- a diagnostic that
            # reorders itself between runs is one nobody can diff, and a state
            # missing from this list would silently print nothing while still
            # counting toward the sum, which the assertion below then catches.
            split("SUCCESS RUNNING SKIPPED ABSENT FAILED WITHDRAWN UNKNOWN", order, " ")
            printf "TALLY"
            listed = 0
            for (i = 1; i <= 7; i++) {
                s = order[i]
                printf "\t%s=%d", s, (s in tally ? tally[s] : 0)
                listed += (s in tally ? tally[s] : 0)
            }
            printf "\n"
            sum = 0
            for (s in tally) sum += tally[s]
            if (sum != requiredCount)
                print "REFUSE\tthe states sum to " sum " over " requiredCount " required contexts, so one was lost"
            else if (listed != sum)
                print "REFUSE\tthe printed states sum to " listed " where " sum " were counted, so a state is missing from the fixed print order"
            print "COUNT\t" requiredCount
        }
    ' /dev/null
}

# ---------------------------------------------------------------------------
# WHETHER THE HEAD MERGES -- a second question, answered on its own line (#1352).
#
# Every verdict above is about check runs AT THE HEAD SHA, and a check run is never taken
# back. A pull request that BECAME conflicting after its runs finished keeps every one of
# them, values and all, while nothing further dispatches -- so on such a head this tool
# used to print `every required context reports SUCCESS` about a commit that cannot merge.
# That is the dangerous green: a red invites investigation, a green invites merging, and
# this is the script every lane is told to verify CI with. So it asks the pull request's
# merge state as well, and says it beside the context verdict rather than folding it in.
#
# From REST's `mergeable_state` and `mergeable`, never GraphQL's `mergeStateStatus`, for
# the budget reason `--pr` gives. REST spells the states in lowercase.
#
# A TABLE, and a state it does not name is REFUSED by name: the next word GitHub adds must
# not land in a default arm and read as clean. `unknown` is not clean either -- GitHub
# computes mergeability lazily, so the first query after a push routinely answers it.
# `draft` hides the merge state, so a draft's kind is read from `mergeable` instead: our
# pull requests are opened as drafts, and a draft that conflicts must not read as fine.
# `merged` and `closed` are not GitHub merge states at all -- `--pr` substitutes them from
# the same response -- because a pull request that is no longer open answers `unknown`
# FOREVER, and telling a lane to ask again about a question nobody will compute is the
# collapse in the other direction. Measured on #1383, merged: `unknown` with `null`.
#
#   state|mergeable values it is consistent with|kind|what it means for the verdict
MergeStates=(
    "clean|true|merges|the head merges into its base as it stands."
    "unstable|true|merges|the head merges; a NON-required check is failing or still running."
    "blocked|true|merges|the head merges once a repository rule is met, such as a review."
    "has_hooks|true|merges|the head merges, subject to the repository's pre-receive hooks."
    "behind|true|behind|the head is BEHIND its base, so the contexts describe a tree that is not what merging would produce. Rebase before merging."
    "dirty|false|conflicts|the head CONFLICTS with its base: nothing has dispatched since it began to, so every context above is a verdict about a commit that cannot merge."
    "unknown|null|uncomputed|NOT YET COMPUTED. GitHub computes this lazily and answers it for a while after every push; ask again. It is not clean."
    "draft|true false null|derive|a draft, whose merge state GitHub does not report, so whether it merges is read from its separate mergeable answer."
    "merged|true false null|history|the pull request is already MERGED: nothing is left to merge, and the contexts above are a record of its head, not a gate."
    "closed|true false null|history|the pull request is CLOSED without merging: the contexts above are a record of its head, not a gate."
)

# The merge half of the verdict. Pure: two words in, one line out.
#
# @param 1 `mergeable_state` as GitHub answered it
# @param 2 `mergeable`: true | false | null
# @return prints `kind|sentence`, kind one of merges | behind | conflicts | uncomputed |
#         history | refuse
MergeVerdict() {
    local state="$1" mergeable="$2" row word allowed kind sentence
    case "$mergeable" in
        true|false|null) ;;
        *)
            echo "refuse|the mergeable answer '$mergeable' is none of true, false or null."
            return
            ;;
    esac
    for row in "${MergeStates[@]}"; do
        word="${row%%|*}"
        row="${row#*|}"
        allowed="${row%%|*}"
        row="${row#*|}"
        kind="${row%%|*}"
        sentence="${row#*|}"
        [[ "$word" == "$state" ]] || continue
        case " $allowed " in
            *" $mergeable "*) ;;
            *)
                echo "refuse|GitHub's two answers disagree -- mergeable_state '$state' with mergeable '$mergeable' -- so neither can be believed."
                return
                ;;
        esac
        if [[ "$kind" == "derive" ]]; then
            # `mergeable` was checked above to be exactly one of these three.
            case "$mergeable" in
                true) kind="merges" ;;
                false) kind="conflicts" ;;
                null) kind="uncomputed" ;;
            esac
        fi
        echo "$kind|$sentence"
        return
    done
    echo "refuse|a merge state this script does not name: '$state'. Add it to MergeStates deliberately -- a word nobody enumerated is not a clean head."
}

# ---------------------------------------------------------------------------
# Rendering and the verdict. Kept apart from `Decide` so the self-test can assert
# on states rather than on prose.
# @param 1 the decision file  @param 2 `mergeable_state`  @param 3 `mergeable`
Render() {
    local decided="$1" mergeState="$2" mergeable="$3" blocked=0 unknown=0 line state name
    local merge kind sentence qualifier contexts
    echo "required contexts:"
    while IFS=$'\t' read -r kind state name; do
        case "$kind" in
            REQUIRED)
                printf '  %-10s %s\n' "$state" "$name"
                case "$state" in
                    SUCCESS)   ;;
                    SKIPPED)   blocked=$((blocked + 1)) ;;
                    RUNNING)   blocked=$((blocked + 1)) ;;
                    ABSENT)    blocked=$((blocked + 1)) ;;
                    FAILED)    blocked=$((blocked + 1)) ;;
                    WITHDRAWN) blocked=$((blocked + 1)) ;;
                    *)         blocked=$((blocked + 1)); unknown=$((unknown + 1)) ;;
                esac
                ;;
        esac
    done < "$decided"

    if grep -q '^NONBLOCKING' "$decided"; then
        echo
        echo "non-required failures (these block NOTHING; they are here so a real"
        echo "regression in an unrequired job is not invisible):"
        while IFS=$'\t' read -r kind state name; do
            [[ "$kind" == "NONBLOCKING" ]] && printf '  %-10s %s\n' "$state" "$name"
        done < "$decided"
    fi

    # CAPTURED, never `grep ... | sed` in a bare pipeline. Under `set -e` with
    # `pipefail` a grep that legitimately matches nothing exits 1, the pipeline
    # takes its status, and the script dies HERE -- silently, before the refusal
    # below could be printed. That is `.agent/rules/build-and-toolchain.md`'s
    # `producer | grep -q` trap arriving from the consumer end, and it cost this
    # file's own empty-table case: the decision file held the refusal and the
    # renderer never reached the line that prints it.
    local tally refusals
    tally="$(grep '^TALLY' "$decided" || true)"
    refusals="$(grep '^REFUSE' "$decided" || true)"

    echo
    [[ -n "$tally" ]] && printf '%s\n' "$tally" | sed 's/^TALLY/  states:/'
    merge="$(MergeVerdict "$mergeState" "$mergeable")"
    kind="${merge%%|*}"
    sentence="${merge#*|}"
    printf '  merge:\t%s\tmergeable=%s\t%s\n' "$mergeState" "$mergeable" "$kind"

    if [[ -n "$refusals" ]]; then
        echo
        printf '%s\n' "$refusals" | sed 's/^REFUSE\t/  REFUSING: /'
        return 2
    fi
    if [[ "$unknown" -gt 0 ]]; then
        echo
        echo "VERDICT: REFUSING -- $unknown required context(s) are in a state this script does not know."
        echo "  A conclusion nobody has enumerated is not a pass; add it to \`stateOf\` deliberately."
        return 2
    fi
    # The context verdict says what the merge state makes of it, on its own line, so a
    # reader of that line alone cannot take a verdict about a stale head for a current one.
    case "$kind" in
        conflicts)  qualifier=" -- about a head that CANNOT MERGE, so these are verdicts about a commit that no longer merges" ;;
        behind)     qualifier=" -- about a head BEHIND its base, not about what merging would produce" ;;
        uncomputed) qualifier=" -- and whether the head merges is NOT YET COMPUTED" ;;
        history)    qualifier=" -- about a pull request that is no longer open, as a record rather than a gate" ;;
        merges|refuse) qualifier="" ;;
        *)
            echo
            echo "VERDICT: REFUSING -- MergeVerdict answered a kind this script does not enumerate: '$kind'."
            return 2
            ;;
    esac
    echo
    if [[ "$blocked" -eq 0 ]]; then
        echo "VERDICT: every required context reports SUCCESS${qualifier}."
        contexts=0
    else
        echo "VERDICT: $blocked required context(s) are not SUCCESS${qualifier} -- read the list above by NAME."
        echo "  RUNNING   a wait."
        echo "  ABSENT    no check run of that name exists at this SHA. On a pull request whose"
        echo "            workflows are still queued that is a wait too, and it becomes a blocker"
        echo "            only if it persists -- then the workflow does not fire for this event,"
        echo "            which is the never-arrives failure check-merge-queue-contexts guards."
        echo "  SKIPPED   reports, and a required context that skips reads as PASSING to the"
        echo "            ruleset. It will not clear on its own."
        echo "  FAILED    will not clear on its own."
        echo "  WITHDRAWN a verdict was cancelled and no replacement is in flight (#903)."
        echo "            Re-run that workflow rather than waiting."
        contexts=1
    fi
    if [[ "$kind" == "refuse" ]]; then
        echo "MERGE VERDICT: REFUSING -- $sentence"
        return 2
    fi
    echo "MERGE VERDICT: $mergeState -- $sentence"
    # A context that is not SUCCESS blocks whatever the merge state says. Only a fully
    # green set asks the merge half: a head that is behind or conflicts is not ready, and
    # one whose state is not yet computed is not known to be.
    [[ "$contexts" -eq 0 ]] || return 1
    case "$kind" in
        merges|history) return 0 ;;
        behind|conflicts) return 1 ;;
        uncomputed) return 2 ;;
    esac
    echo "VERDICT: REFUSING -- no exit status is enumerated for merge kind '$kind'."
    return 2
}

# ---------------------------------------------------------------------------
# selftest-offer: this script offers no self-test of its own. The token below names
# `check-pr-required.sh --self-test`, which is where every verdict in this file is
# driven from -- a reference to ANOTHER script's mode, not an offer of one here.
usage() {
    echo "usage: $(basename "${BASH_SOURCE[0]}") --pr <number>" >&2
    echo "       $(basename "${BASH_SOURCE[0]}") --record <file> --merge-state <mergeable_state> --mergeable <true|false|null>" >&2
    echo "       $(basename "${BASH_SOURCE[0]}") --pr <number> --wait [--wait-interval <s>] [--wait-timeout <s>]" >&2
    echo "       $(basename "${BASH_SOURCE[0]}") --listing-verdict <total_count> <rows-read>" >&2
    echo "       $(basename "${BASH_SOURCE[0]}") --head-guard <first-sha> <current-sha>" >&2
    echo "       $(basename "${BASH_SOURCE[0]}") --wait-decision <merge-kind> <running> <absent> <waited> <ceiling>" >&2
    echo "  the verdicts are driven by scripts/check-pr-required.sh --self-test" >&2
    echo "  exit 0 every required context is SUCCESS, and the head is neither behind nor conflicting" >&2
    echo "         (or the pull request is no longer open, and the verdict is a record)" >&2
    echo "  exit 1 at least one is not, named above -- or the head is behind or conflicts" >&2
    echo "  exit 2 refusing to give a verdict about the pull request, including a merge state" >&2
    echo "         not yet computed or not named by MergeStates" >&2
    echo "  exit 3 the instrument could not ask -- nothing was measured" >&2
    echo "  exit 4 the head moved under --wait, so no verdict describes the current head" >&2
    exit 2
}

[[ $# -ge 1 ]] || usage

case "$1" in
    # The merge state is REQUIRED with a record. Optional, a staged record with none would
    # have to mean something, and the only thing it could mean is clean -- the collapse.
    --record)
        [[ $# -eq 6 && "$3" == "--merge-state" && "$5" == "--mergeable" ]] || usage
        [[ -f "$2" ]] || Fatal "no such record: $2"
        req="$(mktemp)"; dec="$(mktemp)"
        # shellcheck disable=SC2064
        trap "rm -f '$req' '$dec'" EXIT
        ReadRequiredContexts > "$req"
        ApplyReaderControl "$2" "$req"
        Decide "$2" "$req" > "$dec" || true
        Render "$dec" "$4" "$6"
        ;;
    # Not a hidden mode. `ListingVerdict` is a DECISION made during acquisition,
    # and a decision reachable only through the network is one nobody has driven
    # in either direction -- which is how its empty-`total_count` arm shipped
    # broken. Documented in `usage` so it is a door rather than a secret.
    --listing-verdict)
        [[ $# -eq 3 ]] || usage
        ListingVerdict "$2" "$3"
        ;;
    # Doors for the same reason `--listing-verdict` is one: these two decide what
    # `--wait` does, and a real force-push mid-poll cannot be staged in a fixture.
    --head-guard)
        [[ $# -eq 3 ]] || usage
        HeadGuard "$2" "$3"
        ;;
    --wait-decision)
        [[ $# -eq 6 ]] || usage
        WaitDecision "$2" "$3" "$4" "$5" "$6"
        ;;
    --pr)
        [[ $# -ge 2 ]] || usage
        PrNumber="$2"
        shift 2
        # `--wait` wraps the decision below; it does not replace it. Every poll
        # runs the SAME acquisition and the SAME `Decide`/`Render`, so a waiting
        # run and a one-shot run cannot disagree about a settled head (#1289).
        #
        # It exists because this tool was one-shot and every lane hand-rolled its
        # own waiter in a scratchpad -- each one a fresh chance to get the
        # head-moved case wrong, and none of them ever did get it right.
        Waiting=0
        WaitInterval=20
        WaitTimeout=1800
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --wait)          Waiting=1; shift ;;
                --wait-interval) [[ $# -ge 2 ]] || usage; WaitInterval="$2"; shift 2 ;;
                --wait-timeout)  [[ $# -ge 2 ]] || usage; WaitTimeout="$2"; shift 2 ;;
                *) usage ;;
            esac
        done
        case "$WaitInterval" in ''|*[!0-9]*) Fatal "--wait-interval takes whole seconds, not '$WaitInterval'" ;; esac
        case "$WaitTimeout"  in ''|*[!0-9]*) Fatal "--wait-timeout takes whole seconds, not '$WaitTimeout'" ;; esac
        [[ "$WaitInterval" -gt 0 ]] || Fatal "--wait-interval must be above zero, or the poll is a spin"
        command -v gh >/dev/null 2>&1 || Unavailable "gh is not on PATH"
        repo="${FASTCACHED_REPO:-LASTRADA-Software/fastcached}"
        rec="$(mktemp)"; req="$(mktemp)"; dec="$(mktemp)"
        # shellcheck disable=SC2064
        trap "rm -f '$rec' '$req' '$dec'" EXIT
        ReadRequiredContexts > "$req"
        FirstSha=""
        # The bound MEASURES itself by counting the sleep it ASKED for, never a
        # wall clock: `SECONDS` and `date` read CLOCK_REALTIME, which a VM host
        # steps in both directions, and a forward step SHORTENS the bound so a
        # healthy wait gives up early and reads as a slow runner.
        Waited=0
        while :; do
        # REST throughout, deliberately, and it is not a preference.
        #
        # The GraphQL budget is per USER and every lane on this machine shares
        # it. `gh pr view`, `gh pr list`, `gh issue view` and every `gh project`
        # call are GraphQL; `repos/.../pulls/N` and `.../check-runs` are REST.
        # Measured 2026-09-10: GraphQL refusing every call while REST answered
        # normally. Reading the head SHA through `gh pr view` would have made
        # this tool unusable exactly when several lanes are working, which is
        # exactly when somebody asks it which contexts are blocking.
        #
        # gh's own stderr is left alone rather than swallowed: the API's wording
        # is what tells a rate limit from an auth failure from a 404.
        #
        # The head SHA and the merge state come from ONE response, so the two cannot describe
        # different moments. Split by expansion rather than `IFS=$'\t' read`, which collapses
        # an empty field and would shift `mergeable` into `mergeable_state`.
        head="$(gh api "repos/$repo/pulls/$PrNumber" \
            --jq '[.head.sha, (.mergeable_state // ""), (if .mergeable == null then "null" else (.mergeable | tostring) end), (if .merged then "merged" elif .state == "closed" then "closed" else "open" end)] | @tsv')" \
            || Unavailable "the API would not answer the head SHA of #$PrNumber -- read gh's message above"
        case "$head" in
            *$'\t'*$'\t'*$'\t'*) ;;
            *) Unavailable "the pull request response for #$PrNumber did not carry four fields: '$head'" ;;
        esac
        sha="${head%%$'\t'*}"
        head="${head#*$'\t'}"
        mergeState="${head%%$'\t'*}"
        head="${head#*$'\t'}"
        mergeable="${head%%$'\t'*}"
        openness="${head#*$'\t'}"
        # A pull request that is no longer open has no merge state to compute, whatever
        # `mergeable_state` says -- so its openness names the row instead.
        case "$openness" in
            open) ;;
            merged|closed) mergeState="$openness" ;;
            *) Unavailable "#$PrNumber came back neither open, merged nor closed: '$openness'" ;;
        esac
        [[ -n "$sha" ]] || Unavailable "#$PrNumber came back with no head SHA"
        [[ -n "$mergeState" ]] || Unavailable "#$PrNumber came back with no mergeable_state, so nothing says whether its head merges"
        # The head, compared against the one this wait started on. Read from the
        # SAME response as `mergeable_state` above, so the two cannot describe
        # different moments -- which is the whole reason the guard can be trusted.
        case "$(HeadGuard "$FirstSha" "$sha")" in
            same)   [[ -n "$FirstSha" ]] || FirstSha="$sha" ;;
            moved)  HeadMoved "$FirstSha" "$sha" ;;
            *)      Unavailable "the head guard could not compare '$FirstSha' with '$sha'" ;;
        esac
        # One page of 100, and the total is read back rather than assumed.
        #
        # Deliberately NOT `--paginate`: it emits one complete JSON document per
        # page, back to back, and the tempting `}{` split cuts inside strings --
        # measured on this very endpoint, an 82 KB single-page response holding
        # 23 runs split into 24 fragments, every one unparseable, yielding zero
        # records and therefore every context ABSENT. Asking for one page and
        # REFUSING when it is not the whole set is immune to that by
        # construction, where handling it correctly would be one more thing to
        # get right.
        total="$(gh api "repos/$repo/commits/$sha/check-runs?per_page=100" --jq '.total_count')" \
            || Unavailable "the API would not answer the check runs of $sha -- read gh's message above"
        gh api "repos/$repo/commits/$sha/check-runs?per_page=100" \
           --jq '.check_runs[] | [.name, .status, (.conclusion // ""), (.started_at // "")] | @tsv' > "$rec" \
            || Unavailable "the API would not answer the check runs of $sha -- read gh's message above"
        got="$(awk 'END { print NR }' "$rec")"
        case "$(ListingVerdict "$total" "$got")" in
            unreadable-total)
                Unavailable "the check-run response carried no readable total_count (it said '${total}'), so nothing says whether these ${got} row(s) are the whole set"
                ;;
            capped)
                Unavailable "GitHub reports ${total} check run(s) for ${sha} and this read ${got}; the rest are beyond one page, so every verdict below would be about a set that is not the whole set"
                ;;
            ok) ;;
            *)
                # Same closed-set argument as `ApplyReaderControl`'s default arm:
                # a word nobody enumerated is a code defect, and the silent
                # fall-through is what makes it survive.
                Unavailable "ListingVerdict answered a word this script does not enumerate"
                ;;
        esac
        echo "#$PrNumber at $sha -- $got check run(s)"
        ApplyReaderControl "$rec" "$req"
        Decide "$rec" "$req" > "$dec" || true

        status=0
        Render "$dec" "$mergeState" "$mergeable" || status=$?

        # One-shot is the default and behaves exactly as before.
        [[ "$Waiting" -eq 1 ]] || exit "$status"

        # Whether waiting could still change the answer is the MERGE half's
        # question, and only two of its six kinds are still open. `conflicts`
        # is the one the ticket names: a dirty head does not clean itself, so
        # it exits on poll 1 on the refusal it already had rather than
        # polling until the timeout and then printing the same thing.
        mergeKind="$(MergeVerdict "$mergeState" "$mergeable")"
        mergeKind="${mergeKind%%|*}"

        running="$(TallyCount "$dec" RUNNING)"
        absent="$(TallyCount "$dec" ABSENT)"
        if [[ "$running" -lt 0 || "$absent" -lt 0 ]]; then
            # The decision carried no TALLY, so nothing says how many contexts
            # are in flight. Waiting on an unreadable count is waiting on
            # nothing; this is the instrument, not the pull request.
            Unavailable "the decision carried no TALLY line, so nothing says how many contexts are still running or absent"
        fi

        case "$(WaitDecision "$mergeKind" "$running" "$absent" "$Waited" "$WaitTimeout")" in
            stop-settled) exit "$status" ;;
            stop-decided)
                # Absent contexts on a decided head are worth a sentence, because
                # the two silences look identical in the tally and are opposite
                # diagnoses. A conflicting pull request has no merge ref, so the
                # workflows never dispatched; reading that as *CI is slow* is how
                # a queued branch sits for an afternoon (#1352).
                if [[ "$absent" -gt 0 && "$mergeKind" == "conflicts" ]]; then
                    echo "  ${absent} required context(s) are ABSENT and will stay absent: a conflicting"
                    echo "  pull request has no merge ref, so nothing was dispatched to report them."
                    echo "  Waiting cannot produce them. Resolve the conflict and push."
                fi
                exit "$status"
                ;;
            stop-timeout)
                echo "  the wait gave up after ${Waited}s with ${running} context(s) running and ${absent} absent."
                echo "  That is the verdict above, not a separate outcome: they had not"
                echo "  reported yet, which is what RUNNING and ABSENT already say."
                exit "$status"
                ;;
            poll) ;;
            *) Unavailable "WaitDecision answered a word this script does not enumerate" ;;
        esac

        echo "  waiting ${WaitInterval}s for ${running} running and ${absent} absent context(s) (${Waited}s of ${WaitTimeout}s spent)"
        sleep "$WaitInterval"
        Waited=$(( Waited + WaitInterval ))
        done
        ;;
    *) usage ;;
esac
