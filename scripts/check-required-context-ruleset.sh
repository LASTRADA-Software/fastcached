#!/usr/bin/env bash
#
# `RequiredContexts` agrees with the ruleset the SERVER actually enforces (#1562).
#
# THE GAP. `scripts/check-merge-queue-contexts.sh` carries the tree's statement of which
# contexts gate a merge, and every check around it reconciles that table against the
# WORKFLOWS -- can each context be produced, does each have a `merge_group` leg, does a
# skipped job still report. Nothing reconciled it against the RULESET, which is the only
# thing that actually blocks a merge. The table and the server could disagree
# indefinitely and every check would stay green, because each was correct about its own
# side.
#
# They do disagree today, which is what makes this worth having rather than a hypothesis.
#
# BOTH DIRECTIONS, and they are different failures:
#
#   in the TABLE, not in the RULESET   the tree believes a context gates and it does
#                                      not. Everything downstream -- the queue-leg
#                                      check, `ci-pr-required.sh`'s blocking list --
#                                      reasons about a gate that is not there. This is
#                                      the UNSAFE direction and it is the one that is
#                                      live.
#   in the RULESET, not in the TABLE   the server blocks on something the tree has
#                                      never heard of, so no check asserts it can
#                                      report. That is the never-arrives failure: a
#                                      required context nothing produces leaves a
#                                      queued pull request waiting forever.
#
# FAIL CLOSED, because an unreachable API and an agreeing set are not the same green.
# Measured, so the arms are drawn from the tool rather than from a guess about it:
#
#   normal                       exit 0, a non-empty context list
#   --jq matched nothing         exit 0, EMPTY stdout  -- reachable on a HEALTHY API
#                                (wrong branch, rule removed). REFUSED, not passed.
#   HTTP error (401, 404, 403)   exit 1, and the raw JSON error lands on STDOUT even
#                                though `--jq` was given, so a caller that does not
#                                check `$?` gets a string that is neither empty nor a
#                                context list. REFUSED.
#   not authenticated            exit 4, empty stdout. UNAVAILABLE -> SKIP.
#   `gh` not installed           exit 127. UNAVAILABLE -> SKIP.
#
# The last two are the instrument being absent rather than a verdict about the tree, so
# they exit 77 and SAY so. Anything neither 0 nor 1 is the instrument, not the subject.
#
# WHICH SIDE EACH NAME CAME FROM is printed for every finding, so a future divergence
# names a DIRECTION rather than a count -- "13 vs 14" tells nobody which way to look.
set -o errexit
set -o nounset
set -o pipefail

SKIP=77
Root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
Mode="scan"
Repo="${FASTCACHED_RULESET_REPO:-LASTRADA-Software/fastcached}"
Branch="${FASTCACHED_RULESET_BRANCH:-master}"
StagedTable=""
StagedRuleset=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --self-test)      Mode="selftest"; shift ;;
        --root)           Root="$2"; shift 2 ;;
        --staged-table)   StagedTable="$2"; shift 2 ;;
        --staged-ruleset) StagedRuleset="$2"; shift 2 ;;
        *) echo "usage: $0 [--root <dir>] | --self-test" >&2; exit 2 ;;
    esac
done

Failed=0
Fail() { echo "REQUIRED CONTEXT RULESET FAILED: $*" >&2; Failed=$(( Failed + 1 )); }
# STDERR, and that is the whole of it. Every `Skip` call site sits inside
# `ReadRuleset`, whose stdout `Scan` redirects into the ruleset file so the refusal
# counter survives (a pipeline would run it in a subshell) -- so a skip written to
# stdout lands in a temp file the EXIT trap then deletes. Measured with `gh` off
# PATH: exit 77 and not one byte of output, which is a skip that does not say which
# way it was, in the file whose header promises exactly that it does.
Skip() { echo "SKIP: $*" >&2; exit "$SKIP"; }

# --- the pending divergence -------------------------------------------------
#
# Rows are `<context>|<direction>|<issue>|<reason>`, `direction` being `table-only` or
# `ruleset-only`.
#
# An exemption, NOT a softening. The one live divergence is a decision nobody has taken
# yet -- whether `Check the PR body's closing keywords` should be promoted on the server
# or dropped from the table -- and it is the repository owner's to take. This check
# cannot take it by refusing, and it must not silently accept it either, so the
# divergence is recorded WITH its issue and every OTHER divergence is refused.
#
# A row that has stopped describing a real divergence is refused as STALE, which is what
# makes this expire rather than accumulate: resolving #1562 in either direction turns
# this row red until it is deleted, so deleting it is part of closing the ticket.
#
# TOTAL: 1 row.
PendingDivergence=(
    "Check the PR body's closing keywords|table-only|1562|Promoted to a required context by #1045 in the TREE and never added to the default-master ruleset. The table's own comment asserts it IS required; the server disagrees. Which side is wrong is the owner's call and is pending -- do not resolve it by editing either side to match the other."
)

# --- the table --------------------------------------------------------------
ReadTable() {
    # The staged file goes through the SAME awk reader, never around it: a fixture
    # that bypasses the parser tests a different object, and the parser is half of
    # what can be wrong here.
    local file="${StagedTable:-${Root}/scripts/check-merge-queue-contexts.sh}"
    # Rows are `<context>|<workflow>`; the context is everything before the first `|`.
    # The region is delimited by the declaration and its closing paren, so a second
    # `RequiredContexts=(` elsewhere in the file cannot bleed in.
    awk '
        /^RequiredContexts=\(/ { inTable = 1; next }
        inTable && /^\)/       { inTable = 0 }
        inTable {
            line = $0
            sub(/^[ \t]+/, "", line)
            if (line ~ /^#/ || line == "") next
            if (substr(line, 1, 1) != "\"") next
            line = substr(line, 2)
            p = index(line, "|")
            if (p > 0) print substr(line, 1, p - 1)
        }
    ' "$file"
}

# --- the ruleset ------------------------------------------------------------
#
# ONE call. The ruleset LIST endpoint does not carry `rules` (measured: `has("rules")`
# answers false), so it cannot answer this on its own, and the per-ruleset GET needs an
# id resolved by NAME -- a name typo there yields exit 0 and an empty list, which is the
# silent-zero this check refuses anyway but would rather not create for itself.
# `rules/branches/<branch>` returns the EFFECTIVE rules for the branch as a flat array,
# needs no id, and picks up an org-level ruleset automatically.
ReadRuleset() {
    if [ -n "$StagedRuleset" ]; then cat "$StagedRuleset"; return 0; fi
    local out rc
    if ! command -v gh >/dev/null 2>&1; then
        Skip "gh is not on PATH, so the live ruleset could not be read. This is the instrument being absent, not a verdict about the table."
    fi
    local err
    err="$(mktemp)"
    out="$(gh api "repos/${Repo}/rules/branches/${Branch}" \
             --jq '.[] | select(.type == "required_status_checks") | .parameters.required_status_checks[].context' 2>"$err")" && rc=0 || rc=$?
    local stderrText
    stderrText="$(cat "$err")"
    rm -f "$err"
    case "$rc" in
        0) ;;
        4)   Skip "gh is not authenticated (exit 4), so the live ruleset could not be read. Run \`gh auth login\`. This is the instrument being absent, not a verdict." ;;
        1)   # gh spends exit 1 on BOTH an HTTP error and a transport failure, so the
             # status alone cannot separate "GitHub said no" from "we never reached
             # GitHub". They are different answers: the first is about this repository,
             # the second about the machine. Read the message.
             case "$(printf '%s' "$stderrText" | tr '[:upper:]' '[:lower:]')" in
                 *"dial tcp"*|*"no such host"*|*"connection refused"*|*"network is unreachable"*|*"i/o timeout"*|*"temporary failure in name resolution"*|*"tls handshake timeout"*|*"error connecting to"*|*"check your internet connection"*)
                     Skip "the GitHub API could not be reached (${stderrText}). No network is not a verdict about the table -- reported as a skip rather than as a pass or a failure." ;;
             esac
             Fail "the ruleset API answered with an HTTP error (gh exit 1): ${stderrText}. Re-run \`gh api repos/${Repo}/rules/branches/${Branch}\` to see it. An unreachable API and an agreeing set are NOT the same answer, so this is a refusal rather than a pass."
             return 1 ;;
        127) Skip "gh could not be executed (exit 127)." ;;
        *)   Fail "gh exited ${rc}, which is neither 0 nor 1 and is therefore the instrument failing rather than a finding about the ruleset."
             return 1 ;;
    esac
    printf '%s\n' "$out"
}

IsPending() {
    local ctx="$1" dir="$2" row
    for row in "${PendingDivergence[@]}"; do
        [ "${row%%|*}" = "$ctx" ] || continue
        local rest="${row#*|}"
        [ "${rest%%|*}" = "$dir" ] && return 0
    done
    return 1
}

Scan() {
    local scratch table ruleset ctx
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT

    table="${scratch}/table"
    ruleset="${scratch}/ruleset"
    ReadTable | sort -u > "$table"
    # NOT `ReadRuleset | sort -u`. A pipeline runs its left-hand side in a SUBSHELL, so
    # `Fail`'s increment of `Failed` would be lost and the refusal would print while the
    # check exited 0 -- which is the precise shape this whole file exists to refuse.
    # Measured: with a bad token it printed REQUIRED CONTEXT RULESET FAILED and reported
    # "0 unreconciled difference(s)", exit 0.
    local raw="${scratch}/raw"
    ReadRuleset > "$raw" || { rm -rf "$scratch"; trap - EXIT; return 1; }
    sort -u < "$raw" > "$ruleset"

    local nTable nRuleset
    nTable="$(grep -c . < "$table" || true)"
    nRuleset="$(grep -c . < "$ruleset" || true)"

    # Both empties are the silent zero, and each has its own cause and its own message.
    if [ "${nTable:-0}" -eq 0 ]; then
        Fail "no context was read out of check-merge-queue-contexts.sh's RequiredContexts table. Two empty lists agree perfectly, so this cannot be a pass -- the declaration has most likely been renamed or reformatted."
        rm -rf "$scratch"; trap - EXIT; return 1
    fi
    if [ "${nRuleset:-0}" -eq 0 ]; then
        Fail "the ruleset API answered (exit 0) and named NO required status check. That is reachable on a healthy API -- a wrong branch, a renamed ruleset, or the rule genuinely removed -- and all three mean the reconciliation below would compare against nothing and pass. Refused."
        rm -rf "$scratch"; trap - EXIT; return 1
    fi
    echo "reconciling ${nTable} table context(s) against ${nRuleset} live on ${Repo}@${Branch}"

    local pendingSeen=""
    while IFS= read -r ctx; do
        [ -n "$ctx" ] || continue
        if ! grep -Fxq "$ctx" "$ruleset"; then
            if IsPending "$ctx" "table-only"; then
                pendingSeen="${pendingSeen}${ctx}|table-only "
                echo "pending: '${ctx}' is in the TABLE and not in the RULESET -- known, recorded against #1562"
            else
                Fail "'${ctx}' is in RequiredContexts (read from scripts/check-merge-queue-contexts.sh) and NOT in the ${Branch} ruleset (read from the GitHub API). The tree believes this context gates a merge and the server does not, so every check reasoning off that table is reasoning about a gate that is not there. Fix by promoting it on the server, or by removing the row -- and if the decision is pending, add a PendingDivergence row naming the issue."
            fi
        fi
    done < "$table"

    while IFS= read -r ctx; do
        [ -n "$ctx" ] || continue
        if ! grep -Fxq "$ctx" "$table"; then
            if IsPending "$ctx" "ruleset-only"; then
                pendingSeen="${pendingSeen}${ctx}|ruleset-only "
                echo "pending: '${ctx}' is in the RULESET and not in the TABLE -- known, recorded"
            else
                Fail "'${ctx}' is required by the ${Branch} ruleset (read from the GitHub API) and is NOT in RequiredContexts (read from scripts/check-merge-queue-contexts.sh). Nothing in this tree asserts it can report, which is the never-arrives failure: a required context no workflow produces leaves a queued pull request waiting on a check that never comes. Add the row, with the workflow that publishes it."
            fi
        fi
    done < "$ruleset"

    # A pending row that no longer describes a divergence is STALE. This is what makes
    # the exemption expire instead of accumulating: resolving the question in either
    # direction turns the row red, so deleting it is part of closing the ticket.
    local row ctx2 dir issue
    for row in "${PendingDivergence[@]}"; do
        ctx2="${row%%|*}"
        dir="$(echo "$row" | cut -d'|' -f2)"
        issue="$(echo "$row" | cut -d'|' -f3)"
        case "$pendingSeen" in
            *"${ctx2}|${dir} "*) ;;
            *) Fail "the PendingDivergence row for '${ctx2}' (${dir}, #${issue}) no longer describes a real divergence -- the two sides agree about it now. Delete the row; that is part of closing #${issue}, not a separate chore." ;;
        esac
    done

    rm -rf "$scratch"
    trap - EXIT
    return 0
}

SelfTest() {
    local scratch ran=0 out rc
    scratch="$(mktemp -d)"
    trap 'rm -rf "$scratch"' EXIT

    # A staged table is a real `RequiredContexts=(` block, so the awk reader is what
    # parses it -- including the leading quote and the `|workflow` suffix. A fixture
    # that handed the check a pre-parsed list would be testing a different object, and
    # the parser is half of what can be wrong here. The first spelling of this did
    # exactly that, and two cases failed reporting context names with their quotes
    # still attached.
    Stage() {
        printf 'RequiredContexts=(\n%s\n)\n' "$2" > "${scratch}/$1"
    }
    StageRuleset() {
        printf '%s\n' "$2" > "${scratch}/$1"
    }
    Drive() {
        ( bash "${Root}/scripts/check-required-context-ruleset.sh" --root "$Root" \
              --staged-table "${scratch}/$1" --staged-ruleset "${scratch}/$2" ) 2>&1
    }

    # Table rows in the file's own shape, so the reader is exercised rather than
    # bypassed. The pending row's context is included where the real table has it.
    local pend="Check the PR body's closing keywords"

    # 1. ACCEPTING arm first, and it is not decoration: a check that refused every
    #    tree would pass every refusing case below.
    Stage t1 "\"Alpha|w.yml\"
\"${pend}|w.yml\""
    StageRuleset r1 "Alpha"
    ran=$(( ran + 1 ))
    out="$(Drive t1 r1)" && rc=0 || rc=$?
    if [ "${rc:-0}" -eq 0 ] && grep -q "pending: '${pend}'" <<< "$out"; then
        echo "self-test 1/6 ok: agreement plus the one recorded divergence is accepted"
    else
        Fail "self-test 1: expected acceptance, got rc=${rc} <<${out}>>"
    fi

    # 2. RED, direction one: a table row the ruleset does not carry.
    Stage t2 "\"Alpha|w.yml\"
\"Beta|w.yml\"
\"${pend}|w.yml\""
    StageRuleset r2 "Alpha"
    ran=$(( ran + 1 ))
    out="$(Drive t2 r2)" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "'Beta' is in RequiredContexts" <<< "$out"; then
        echo "self-test 2/6 ok: a table row absent from the ruleset is refused, naming the direction"
    else
        Fail "self-test 2: expected the table-only refusal, got rc=${rc} <<${out}>>"
    fi

    # 3. RED, direction two: a ruleset context the table does not carry.
    Stage t3 "\"Alpha|w.yml\"
\"${pend}|w.yml\""
    StageRuleset r3 "Alpha
Gamma"
    ran=$(( ran + 1 ))
    out="$(Drive t3 r3)" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "'Gamma' is required by the" <<< "$out"; then
        echo "self-test 3/6 ok: a ruleset context absent from the table is refused, naming the direction"
    else
        Fail "self-test 3: expected the ruleset-only refusal, got rc=${rc} <<${out}>>"
    fi

    # 4. The exemption EXPIRES. With the pending divergence resolved, the row is stale.
    Stage t4 "\"Alpha|w.yml\"
\"${pend}|w.yml\""
    StageRuleset r4 "Alpha
${pend}"
    ran=$(( ran + 1 ))
    out="$(Drive t4 r4)" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "no longer describes a real divergence" <<< "$out"; then
        echo "self-test 4/6 ok: a PendingDivergence row that has stopped describing one is refused as stale"
    else
        Fail "self-test 4: expected the stale-row refusal, got rc=${rc} <<${out}>>"
    fi

    # 5. The silent zero, ruleset side. An API that answers and names nothing is the
    #    shape that would make every comparison above pass over nothing.
    Stage t5 "\"Alpha|w.yml\""
    StageRuleset r5 ""
    ran=$(( ran + 1 ))
    out="$(Drive t5 r5)" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "named NO required status check" <<< "$out"; then
        echo "self-test 5/6 ok: an empty ruleset answer is refused, not passed"
    else
        Fail "self-test 5: expected the empty-ruleset refusal, got rc=${rc} <<${out}>>"
    fi

    # 6. The silent zero, table side. Two empty lists agree perfectly.
    Stage t6 ""
    StageRuleset r6 "Alpha"
    ran=$(( ran + 1 ))
    out="$(Drive t6 r6)" && rc=0 || rc=$?
    if [ "${rc:-0}" -ne 0 ] && grep -q "no context was read out of" <<< "$out"; then
        echo "self-test 6/6 ok: an empty table read is refused, not passed"
    else
        Fail "self-test 6: expected the empty-table refusal, got rc=${rc} <<${out}>>"
    fi

    rm -rf "$scratch"
    trap - EXIT
    echo "required-context-ruleset --self-test: ${ran} case(s) ran, ${Failed} failed"
    [ "$Failed" -eq 0 ]
}

if [ "$Mode" = "selftest" ]; then
    SelfTest
    exit $?
fi

Scan || true
echo "required-context-ruleset: ${Failed} unreconciled difference(s)"
[ "$Failed" -eq 0 ]
