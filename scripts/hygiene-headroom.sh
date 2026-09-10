#!/usr/bin/env bash
# Report how close each `hygiene` test ran to its own timeout, as a DISTRIBUTION.
#
# ## Why a distribution rather than a list of failures
#
# The `hygiene` label fails under contention in ways that have nothing to do with
# the tree under test, and the individual failures each look like a defect in
# whatever branch happens to be running ([#1143]). Measured across two contended
# runs, the five checks that failed for one lane were all in the TOP SEVEN of a
# run where **none of them failed** -- so the failures are just the part of the
# distribution that crossed the line on the day, and the tests sitting at 95% of
# budget are the next five.
#
# A test at 95% of its budget PASSES, so nobody looks at it, and it is one slow
# runner from red. That is the condition this script exists to make visible, and
# it is invisible to every instrument that reports only what failed.
#
# It is deliberately a REPORT and never a verdict: it exits 0 whatever it finds,
# for the same reason `target_set_report` does. A number that can fail a build is
# a number people tune; a number beside the run is one they read. Nothing here
# should ever be the reason a branch is refused.
#
# ## What it does NOT do
#
# It proposes no budget changes. Raising a per-test timeout is the fix that
# suggests itself from any single failure and it fails on its own terms: it moves
# one row and leaves the shape unchanged, and it makes the class invisible rather
# than absent, because a check whose budget has been raised until it stops failing
# is a check that no longer reports the condition.
#
# ## Usage
#
#   scripts/hygiene-headroom.sh <build-dir> [--parallel N] [--label L] [--top N]
#   scripts/hygiene-headroom.sh --self-test
#
# ## Reading the output
#
# A figure here is a quantity UNDER CONDITIONS and both halves travel: the header
# states the parallelism, the load average before and after, the build directory
# and the commit, because a run at `--parallel 32` on a shared box measures the
# box and not the branch. A table without that header is not comparable to
# another table, and comparing two anyway is how a per-test budget gets argued
# from a number nobody can reproduce.
set -uo pipefail

source_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

build_dir=""
parallel="1"
label="hygiene"
top="20"
self_test=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --parallel)  parallel="$2"; shift 2 ;;
        --label)     label="$2";    shift 2 ;;
        --top)       top="$2";      shift 2 ;;
        --self-test) self_test="1"; shift ;;
        -h|--help)   sed -n '2,40p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *)           build_dir="$1"; shift ;;
    esac
done

# ---------------------------------------------------------------------------
# The decision, as a pure function over readings.
#
# Split out for the reason this repository splits out every verdict it cares
# about: driven from staged records, both directions are reachable on any host,
# where a run of the real suite exercises whichever rows the day produces. The
# acquisition below is the part that needs a build directory; this part needs
# nothing and is what `--self-test` drives.
# ---------------------------------------------------------------------------

# Percent of budget used, in whole percent, from an elapsed time and a timeout.
#
# Both arrive as decimal seconds from ctest. bash 3.2 has no floating point, so
# they are scaled to milliseconds as TEXT before any arithmetic -- `${v%.*}` and a
# zero-padded fraction, the same shape `_e2e_ms_of` uses, rather than `bc`, which
# is not present on every runner this has to work on.
#
# A budget of zero means NO timeout was set. That is not 100% and it is not 0%: it
# is a test the ranking cannot place, and it is reported as its own outcome rather
# than folded into either end.
#
# @param 1 elapsed seconds, as ctest prints it.
# @param 2 budget seconds, or empty/0 when no timeout is registered.
# @return prints whole percent, or `no-budget`.
headroom_percent() {
    local elapsed="$1" budget="$2" e_ms b_ms
    e_ms="$(_ms_of "$elapsed")"
    b_ms="$(_ms_of "${budget:-0}")"
    if [[ "$b_ms" -le 0 ]]; then
        printf 'no-budget'
        return 0
    fi
    printf '%d' $(( (e_ms * 100) / b_ms ))
}

# Decimal seconds -> whole milliseconds, without floating point.
# One ctest result line -> `name<TAB>elapsed`, or nothing when the line is not one.
#
# ctest pads the name out to a fixed column with dots:
#
#     3/8 Test #49: repository-hygiene ...............   Passed    0.12 sec
#
# so the name and its padding are separated by nothing but a change of character.
# The obvious capture, `\(.*[^ ]\)`, is GREEDY and takes the dots with it -- and it
# does so INVISIBLY, because the pattern still matches and still yields a name. The
# only tests that survived it were the ones with the LONGEST names, whose padding is
# down to the three dots the pattern requires, so there was nothing left to swallow:
# 2 of 142 matched, and the other 140 reported as carrying no budget.
#
# That is why the capture forbids a trailing DOT as well as a trailing space. It is
# also why this is a function with cases below rather than a regex inline: the
# failure produced a well-formed answer, so nothing but a fixture comparing names
# would have caught it.
#
# @param 1 one line of ctest output.
# @return prints `name<TAB>elapsed`, or nothing.
ctest_row_fields() {
    printf '%s\n' "$1" | sed -n \
        's/^ *[0-9]*\/[0-9]* Test *#[0-9]*: \(.*[^ .]\) *\.\{3,\}.* \([0-9][0-9.]*\) sec$/\1\t\2/p'
}

_ms_of() {
    local v="${1:-0}" int frac
    int="${v%%.*}"
    if [[ "$v" == "$int" ]]; then frac=""; else frac="${v#*.}"; fi
    frac="${frac}000"
    frac="${frac:0:3}"
    # `10#` so a leading zero is not read as octal -- `08` is the reading that
    # would abort the arithmetic, and it is an ordinary fraction.
    printf '%d' $(( ${int:-0} * 1000 + 10#${frac} ))
}

# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------
if [[ -n "$self_test" ]]; then
    cases=0
    failed=0
    expect() {
        cases=$(( cases + 1 ))
        if [[ "$2" != "$3" ]]; then
            echo "  FAIL: $1 -- expected '$2', got '$3'" >&2
            failed=$(( failed + 1 ))
        fi
    }

    expect "half a 60s budget"            "50"         "$(headroom_percent 30 60)"
    expect "the #1143 psk-signing-seam row" "59"       "$(headroom_percent 35.9 60)"
    expect "the #1143 script-check-signals row" "57"   "$(headroom_percent 103.9 180)"
    expect "exactly at budget"            "100"        "$(headroom_percent 60 60)"
    expect "over budget still reports"    "116"        "$(headroom_percent 70 60)"
    expect "a fast test"                  "0"          "$(headroom_percent 0.24 60)"
    # No budget is its own outcome, never 0 and never 100 -- a test the ranking
    # cannot place must not sort as the safest thing in the table.
    expect "no timeout registered"        "no-budget"  "$(headroom_percent 12.5 0)"
    expect "an empty budget field"        "no-budget"  "$(headroom_percent 12.5 '')"
    # A fraction with a leading zero is ordinary and must not be read as octal.
    expect "a leading-zero fraction"      "13"         "$(headroom_percent 8.08 60)"

    # --- the ROW PARSE, which is where the real defect was --------------------
    #
    # Both dot-run lengths, because the bug was invisible at one of them: a long
    # name pads to three dots and parses correctly under the greedy capture, so a
    # fixture using only long names reports the parser working. The short name is
    # the one that carried its padding into the name and lost its budget.
    expect "a short name, long dot run" \
        "$(printf 'repository-hygiene\t0.12')" \
        "$(ctest_row_fields '   3/8 Test  #49: repository-hygiene ...............   Passed    0.12 sec')"
    expect "a long name, minimum dot run" \
        "$(printf 'workflow-script-invocations-selftest\t0.35')" \
        "$(ctest_row_fields '  12/142 Test #3402: workflow-script-invocations-selftest ...   Passed    0.35 sec')"
    # A Catch2 case name contains spaces, and several carry punctuation. The name
    # must survive whole, or a whole binary's cases drop out of the ranking.
    expect "a name with spaces" \
        "$(printf 'A missing search root is skipped, not fatal\t0.07')" \
        "$(ctest_row_fields ' 654/3608 Test  #654: A missing search root is skipped, not fatal .....   Passed    0.07 sec')"
    # A failing row is still a reading -- it consumed its budget and belongs in the
    # distribution. Dropping it would bias the ranking toward the healthy end.
    expect "a failed row still parses" \
        "$(printf 'e2e-helpers-selftest\t57.27')" \
        "$(ctest_row_fields '3596/3608 Test #3513: e2e-helpers-selftest .......***Failed   57.27 sec')"
    expect "a non-result line yields nothing" \
        "" "$(ctest_row_fields 'Total Test time (real) =  53.98 sec')"

    echo "hygiene-headroom self-test: ${cases} case(s) ran, ${failed} failed"
    [[ "$failed" -eq 0 ]] || exit 1
    exit 0
fi

# ---------------------------------------------------------------------------
# Acquisition
# ---------------------------------------------------------------------------
if [[ -z "$build_dir" || ! -d "$build_dir" ]]; then
    echo "usage: $(basename "${BASH_SOURCE[0]}") <build-dir> [--parallel N] [--label L] [--top N]" >&2
    exit 2
fi

# The budgets, read from ctest itself rather than from `src/tests/CMakeLists.txt`.
# The registration is what ctest enforces, and a second reading of the source is a
# second thing to be wrong -- the same argument `check-tsan-scope` makes for
# reading its table out of the script that owns it.
budgets="$(ctest --test-dir "$build_dir" -L "$label" --show-only=json-v1 2>/dev/null \
    | python3 -c '
import json, sys
try:
    doc = json.load(sys.stdin)
except Exception:
    sys.exit(0)
# The local below is `budget` and NOT `timeout`, deliberately, and renaming it back
# breaks the build. `check-e2e-helpers.sh` scans every script for `timeout(1)` -- macOS
# has no such binary -- by matching that word in COMMAND POSITION, and a line-based scan
# cannot tell this heredoc from shell: `    timeout = ""` matched, and the check failed
# on a correct script. Teaching the scan about heredocs is the WRONG repair. It would
# make the model more permissive than the thing it stands for, in the check whose whole
# job is that narrowness, and a real `timeout` inside a shell heredoc would then go
# unseen. So the name moves instead.
for t in doc.get("tests", []):
    budget = ""
    for p in t.get("properties", []):
        if p.get("name") == "TIMEOUT":
            budget = str(p.get("value", ""))
    print("%s\t%s" % (t.get("name", ""), budget))
')"

if [[ -z "$budgets" ]]; then
    echo "REFUSED: no tests carrying label '${label}' were found in ${build_dir}." >&2
    echo "  An empty set is not a clean report. Either the label matched nothing," >&2
    echo "  the build directory was configured with a narrower target set, or" >&2
    echo "  python3 is absent and the budgets could not be parsed." >&2
    exit 2
fi

load_before="$(uptime 2>/dev/null | sed 's/.*load average[s]*: //')"
commit="$(git -C "$source_dir" rev-parse --short HEAD 2>/dev/null || echo unknown)"

log="$(mktemp)"
trap 'rm -f "$log"' EXIT
ctest --test-dir "$build_dir" -L "$label" --parallel "$parallel" > "$log" 2>&1
ctest_status=$?

load_after="$(uptime 2>/dev/null | sed 's/.*load average[s]*: //')"

echo "== hygiene headroom, ${label} label"
echo "   build dir     ${build_dir}"
echo "   commit        ${commit}"
echo "   parallelism   ${parallel}"
echo "   load average  before: ${load_before:-unknown} / after: ${load_after:-unknown}"
echo "   ctest status  ${ctest_status}"
echo "   NOTE: a run on a shared box measures the box. State what else was running."
echo

# ctest prints `  N/M Test  #K: <name> ....  Passed  12.34 sec`. The name may
# contain spaces, so it is taken between the `: ` and the run of dots rather than
# by field position.
rows=""
while IFS= read -r line; do
    parsed="$(ctest_row_fields "$line")"
    [[ -n "$parsed" ]] && rows="${rows}${parsed}"$'\n'
done < "$log"
rows="${rows%$'\n'}"

if [[ -z "$rows" ]]; then
    echo "REFUSED: the run produced no readable per-test timings." >&2
    echo "  This is the derivation failing, not a suite with no tests -- ctest" >&2
    echo "  exited ${ctest_status} and its output is ${#log} bytes." >&2
    exit 2
fi

placed=0
unplaced=0
over_half=0
total=0
table=""
while IFS=$'\t' read -r name elapsed; do
    [[ -n "$name" ]] || continue
    total=$(( total + 1 ))
    budget=""
    while IFS=$'\t' read -r bname bvalue; do
        if [[ "$bname" == "$name" ]]; then budget="$bvalue"; break; fi
    done <<< "$budgets"
    pct="$(headroom_percent "$elapsed" "$budget")"
    if [[ "$pct" == "no-budget" ]]; then
        unplaced=$(( unplaced + 1 ))
        continue
    fi
    placed=$(( placed + 1 ))
    [[ "$pct" -ge 50 ]] && over_half=$(( over_half + 1 ))
    table="${table}$(printf '%6s%%%11ss%9ss   %s' "$pct" "$elapsed" "$budget" "$name")"$'\n'
done <<< "$rows"

printf '%7s%12s%10s   %s\n' "used" "elapsed" "budget" "name"
# `sort -rn` on the leading percent, then the top N. A herestring, never a pipe
# into `head`: a producer writing into a consumer that leaves early takes SIGPIPE
# and `pipefail` reports the producer's status (#1181).
sorted="$(printf '%s' "$table" | sort -rn)"
printf '%s\n' "$(head -n "$top" <<< "$sorted")"
echo
echo "  at or above 50% of budget: ${over_half} of ${placed}"
[[ "$unplaced" -gt 0 ]] && echo "  carrying no TIMEOUT, so unplaceable: ${unplaced} of ${total}"
echo
echo "  Reported, never failed: this script exits 0 whatever the distribution is."
exit 0
