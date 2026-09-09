#!/usr/bin/env bash
# Both guards against a background helper running the fixture's cleanup (#1084).
#
# A background subshell inherits the shell's traps. In these fixtures the EXIT trap IS the
# run's cleanup, so a helper signalled with a CATCHABLE signal runs `rm -rf "$scratch"` on
# its way out and deletes the workdir of the run still using it. It presented as
# `node-ready-waits-for-marker` failing about a third of the time with `cannot arm a 15s
# deadline: '/tmp/tmp.XXXX' is not a directory` -- the SECOND wait's arm refusing because
# the FIRST wait's timer had already removed the directory. Nothing in that sentence names
# a trap, a timer or a cleanup.
#
# ## Why this is a scan and not a comment
#
# The fix is TWO guards closing TWO windows, and only the second is load-bearing:
#
#   * `trap - EXIT TERM INT HUP` first inside the subshell covers a subshell that has
#     STARTED;
#   * `kill -KILL` in the disarm covers the window BEFORE that -- the one that actually
#     fires, because the disarm follows the arm by microseconds on a wait that returns at
#     once, so the helper is usually signalled before it runs a single command.
#
# Measured, interleaved and order-alternated: `trap -` ALONE is 10 pass / 2 fail against an
# unfixed 9 / 3, which is indistinguishable; `kill -KILL` is 12 / 0 against 7 / 5. So the
# tempting guard looks sufficient, is not, and is the one a reader deletes as redundant --
# which is exactly the shape a comment cannot prevent and a scan can.
#
# It is also VERSION-DEPENDENT, and that is why the behavioural half cannot be a test here:
# measured on bash 5.2.21, a background subshell runs no inherited EXIT trap and the shape
# does not reproduce at all. It lives on macOS's bash 3.2, which no development host in
# this project can measure, so a green Linux run is evidence of nothing about it. What CAN
# be asserted everywhere is that neither guard has been removed, and that is what this does.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
#
# Usage:
#   bash scripts/check-fixture-cleanup-guards.sh
#   bash scripts/check-fixture-cleanup-guards.sh --self-test
#   bash scripts/check-fixture-cleanup-guards.sh --root <dir>
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
selfTest=0
while [ $# -gt 0 ]; do
    case "${1:-}" in
        --self-test) selfTest=1; shift ;;
        --root)      root="${2:?--root needs a tree}"; shift 2 ;;
        *) printf 'check-fixture-cleanup-guards: unknown argument: %s\n' "${1:-}" >&2; exit 2 ;;
    esac
done

failures=0
Fail() { printf 'FAIL %s\n' "$1" >&2; failures=$((failures + 1)); }

# Every `( ... ) &` SUBSHELL fork, with its line number, in one file.
#
# A command fork (`"$@" &`, `cmd args &`) is deliberately NOT matched: it execs a new
# program, which cannot inherit a shell trap, so the hazard is a property of the SUBSHELL
# form alone. Matching both would demand `trap -` where it means nothing.
# $1: file
SubshellForks() {
    grep -nE '^[[:space:]]*\([^)]*\).*&[[:space:]]*$' "$1" 2>/dev/null || true
}

# Does a `( ... ) &` line disarm its inherited traps FIRST?
# $1: the line
ForkDisarmsTraps() {
    case "$1" in
        *'( trap - '*) return 0 ;;
        *'(trap - '*)  return 0 ;;
    esac
    return 1
}

CheckFile() {
    local file rel forks line lineNo body
    file="$1"
    rel="${file#"$root"/}"
    forks="$(SubshellForks "$file")"
    [ -n "$forks" ] || return 0
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        lineNo="${line%%:*}"
        body="${line#*:}"
        forkCount=$((forkCount + 1))
        if ! ForkDisarmsTraps "$body"; then
            Fail "${rel}:${lineNo} forks a subshell into the background without disarming the
     inherited traps first. It inherits this fixture's EXIT trap, which is the run's
     cleanup, so signalling it deletes the workdir the caller is still using.
     Put \`trap - EXIT TERM INT HUP\` first inside the subshell -- and note that alone
     is NOT sufficient; whatever signals it must use an uncatchable signal too (#1084).
     Line: ${body}"
        fi
    done <<EOF
$forks
EOF
}

# The DEADLINE DISARM must use an uncatchable signal.
#
# Narrow on purpose, and the first version was not. Scanning every `kill -TERM` in the
# tree produced nine findings on a healthy tree, none of them this defect:
#
#   * `run_bounded` signals a bounded COMMAND with TERM, a grace, then KILL -- correct,
#     and you WANT a command given the chance to exit;
#   * `fail` signals the TOP-LEVEL shell with TERM on purpose, which is how it works;
#   * two hits were `printf` STRING LITERALS staging a fixture, and a string literal is
#     no more a call site than a comment is -- the same false positive shape twice.
#
# An over-broad guard that fires on correct code is deleted by whoever meets it. What the
# ticket is about is the TIMER subshell, which is armed by `_e2e_deadline_arm` and
# signalled by `_e2e_deadline_disarm`, so that function is what is asserted.
CheckDisarms() {
    local file rel body
    file="$1"
    rel="${file#"$root"/}"
    body="$(awk '/^_e2e_deadline_disarm\(\)/{ inside=1 } inside { print } inside && /^}/{ exit }' "$file" 2>/dev/null || true)"
    [ -n "$body" ] || return 0
    disarmSeen=$((disarmSeen + 1))
    case "$body" in
        *"kill -KILL"*|*"kill -9"*) : ;;
        *)
            Fail "${rel}: _e2e_deadline_disarm does not signal the timer with an UNCATCHABLE
     signal. The fixture's \`trap 'exit 1' TERM\` turns a catchable one into an ordinary
     exit, which runs the EXIT trap -- the run's cleanup -- inside the timer subshell.
     This is the load-bearing half: measured interleaved and order-alternated, \`trap -\`
     alone is 10 pass / 2 fail against an unfixed 9 / 3, while \`kill -KILL\` is 12 / 0
     against 7 / 5 (#1084)."
            ;;
    esac
}

RunScan() {
    local library
    forkCount=0
    disarmSeen=0
    library="${root}/scripts/lib/e2e-common.sh"
    if [ ! -f "$library" ]; then
        Fail "the shared fixture library is missing: ${library}"
        return
    fi

    # The LIBRARY only, and that is a scope choice stated rather than an oversight. The
    # fixture scripts deliberately stage unguarded `( ... ) &` forks as TEST SUBJECTS --
    # `check-e2e-helpers.sh` alone has five -- so scanning them refuses correct code. The
    # timers this rule is about live here, and this is the file every fixture sources.
    CheckFile "$library"
    CheckDisarms "$library"

    if [ "$forkCount" -lt 1 ]; then
        Fail "no \`( ... ) &\` subshell fork was found in ${library} -- either the shape
     moved or this scan stopped matching it, and in both cases the check is vouching
     for nothing."
    fi
    if [ "$disarmSeen" -lt 1 ]; then
        Fail "no \`_e2e_deadline_disarm\` was found in ${library}, so the load-bearing half
     of #1084 is asserted by nothing. If the timer moved, this scan must move with it."
    fi
    printf 'fixture-cleanup-guards: %d subshell fork(s) examined, %d disarm(s) checked\n' \
        "$forkCount" "$disarmSeen"
}

# ---------------------------------------------------------------------------
# Self-test. Every case drives the REAL scan over a tree it owns, through `--root`.
# ---------------------------------------------------------------------------
SelfTestCases=0
Ok() { SelfTestCases=$((SelfTestCases + 1)); printf 'ok   %s\n' "$1"; }
Bad() { printf 'FAIL %s\n' "$1"; [ -z "${2:-}" ] || printf '%s\n' "$2" | sed 's/^/     /'; exit 1; }

StageTree() {
    mkdir -p "$1/scripts/lib"
    printf '%s\n' 'x=1' > "$1/scripts/lib/e2e-common.sh"
}

# Append a `_e2e_deadline_disarm` signalling with $2. Every tree that is meant to be
# ACCEPTED needs one, because a missing disarm is itself a refusal -- the scan must not be
# satisfiable by deleting the thing it asserts about.
# $1: tree  $2: signal name, e.g. KILL or TERM
StageDisarm() {
    {
        printf '%s\n' '_e2e_deadline_disarm() {'
        printf '    kill -%s "$1" 2>/dev/null || true\n' "$2"
        printf '%s\n' '}'
    } >> "$1/scripts/lib/e2e-common.sh"
}

RunSelfTest() {
    # NOT `local`: the EXIT trap below runs after this function has returned, so a
    # scratch path scoped to it is an unbound variable by then -- which under `set -u`
    # turns a passing run into a failure after every case has already held.
    local out rc
    tmp="$(mktemp -d)" || { printf 'mktemp failed\n' >&2; exit 1; }
    trap 'rm -rf "$tmp"' EXIT

    # Case 1 -- a guarded fork and an uncatchable disarm is ACCEPTED. The passing
    # direction, because a guard nobody has watched accept is not known to work (#1031).
    StageTree "$tmp/a"
    printf '%s\n' '    ( trap - EXIT TERM INT HUP; sleep "$s"; : > "$m" ) >/dev/null 2>&1 &' \
        >> "$tmp/a/scripts/lib/e2e-common.sh"
    StageDisarm "$tmp/a" KILL
    out=$(bash "$0" --root "$tmp/a" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        Ok "case 1: a disarmed fork with an uncatchable signal is accepted"
    else
        Bad "case 1: rc=$rc" "$out"
    fi

    # Case 2 -- GUARD ONE removed. This is the arm that looks redundant and is deleted.
    StageTree "$tmp/b"
    printf '%s\n' '    ( sleep "$s"; : > "$m" ) >/dev/null 2>&1 &' \
        >> "$tmp/b/scripts/lib/e2e-common.sh"
    StageDisarm "$tmp/b" KILL
    out=$(bash "$0" --root "$tmp/b" 2>&1) && rc=0 || rc=$?
    case "$out" in
        *"without disarming the"*) : ;;
        *) Bad "case 2: an undisarmed fork was not named (rc=$rc)" "$out" ;;
    esac
    [ "$rc" -ne 0 ] || Bad "case 2: complained and exited 0" "$out"
    Ok "case 2: a fork that does not disarm its inherited traps is refused"

    # Case 3 -- GUARD TWO removed. The load-bearing one: `trap -` present, signal
    # catchable. Measured indistinguishable from no fix at all, so it must be refused
    # even though the tempting guard is there.
    StageTree "$tmp/c"
    printf '%s\n' '    ( trap - EXIT TERM INT HUP; sleep "$s" ) >/dev/null 2>&1 &' \
        >> "$tmp/c/scripts/lib/e2e-common.sh"
    StageDisarm "$tmp/c" TERM
    out=$(bash "$0" --root "$tmp/c" 2>&1) && rc=0 || rc=$?
    # Matched on a phrase that cannot straddle a line break. The refusal's own text wraps
    # between "UNCATCHABLE" and "signal", so the obvious pattern `CATCHABLE signal` exists
    # in the output and in no single line of it -- and the case then reports a refutation
    # of a check that was refusing correctly.
    case "$out" in
        *"UNCATCHABLE"*) : ;;
        *) Bad "case 3: a catchable signal was not named (rc=$rc)" "$out" ;;
    esac
    [ "$rc" -ne 0 ] || Bad "case 3: complained and exited 0" "$out"
    Ok "case 3: signalling a helper catchably is refused even when the fork disarms"

    # Case 4 -- a COMMAND fork is not a subshell fork. It execs, so it cannot inherit a
    # shell trap, and demanding `trap -` there would be a false refusal on correct code.
    StageTree "$tmp/d"
    printf '%s\n' '    ( trap - EXIT TERM INT HUP; sleep 1 ) &' >> "$tmp/d/scripts/lib/e2e-common.sh"
    printf '%s\n' '    "$@" > "$capture" 2>&1 &' >> "$tmp/d/scripts/lib/e2e-common.sh"
    StageDisarm "$tmp/d" KILL
    out=$(bash "$0" --root "$tmp/d" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        Ok "case 4: a command fork is not required to disarm traps it cannot inherit"
    else
        Bad "case 4: rc=$rc" "$out"
    fi

    # Case 5 -- the vacuity floor. A tree with no subshell fork at all is REFUSED, not
    # reported clean: that is what this scan looks like once the shape moves.
    StageTree "$tmp/e"
    StageDisarm "$tmp/e" KILL
    out=$(bash "$0" --root "$tmp/e" 2>&1) && rc=0 || rc=$?
    # Single-word again -- "vouching for nothing" wraps too. Every pattern in this file is
    # chosen to fit inside one line of a message this script wraps by hand.
    case "$out" in
        *"vouching"*) : ;;
        *) Bad "case 5: an empty scan was not refused (rc=$rc)" "$out" ;;
    esac
    [ "$rc" -ne 0 ] || Bad "case 5: complained and exited 0" "$out"
    Ok "case 5: a scan that matches no fork is refused, not read as clean"

    printf '\nself-test: %d case(s) ran, all passed\n' "$SelfTestCases"
}

if [ "$selfTest" -eq 1 ]; then
    RunSelfTest
    exit 0
fi

RunScan
if [ "$failures" -gt 0 ]; then
    printf 'check-fixture-cleanup-guards: %d finding(s)\n' "$failures" >&2
    exit 1
fi
exit 0
