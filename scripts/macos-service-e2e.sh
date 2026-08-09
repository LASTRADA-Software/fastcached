#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of the macOS launchd integration (user scope, no root needed).
#
# Asserts the properties the plist generator promises, against a real launchd
# rather than against the generated XML — each of these has a failure mode that
# leaves every unit test green:
#
#   1. Registers and RUNS        — bootstrap alone leaves the job loaded but
#                                  unstarted ("pended ... speculative"), so the
#                                  installer reported success while nothing
#                                  listened. Requires a live pid, not just a
#                                  registration.
#   2. Not a reaped fork         — a plist carrying --daemon double-forks and
#                                  launchd reaps it instantly as "exited".
#   3. Serves the protocol       — the job is reachable on its configured port.
#   4. Reinstall is idempotent   — bootout is asynchronous, so a second install
#                                  used to race it and fail with "Bootstrap
#                                  failed: 5: Input/output error".
#   5. Uninstall leaves nothing  — no job, no plist.
#
# Uses a non-default service name and port throughout, so it can never disturb
# a real fastcached installed on the same machine.
#
# Usage:
#   macos-service-e2e.sh --fastcached <path> [--port <n>]
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a runtime
# prerequisite was missing (skip).
set -euo pipefail

fastcached=""
port="21993"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) fastcached="$2"; shift 2 ;;
        --port)       port="$2";       shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

[[ "$(uname -s)" == "Darwin" ]] || { echo "not macOS; skipping"; exit "$SKIP"; }
[[ -n "$fastcached" && -x "$fastcached" ]] || { echo "fastcached not found: '$fastcached'; skipping"; exit "$SKIP"; }

readonly SERVICE_NAME="FastCachedE2E"
readonly LABEL="software.lastrada.fastcachede2e"
readonly PLIST="${HOME}/Library/LaunchAgents/${LABEL}.plist"

# GitHub's macOS runners have a logged-in user but not always a full Aqua
# session, and `gui/<uid>` then fails with "Bootstrap failed: 5". The `user/`
# domain always exists. Pick whichever is available, but keep gui/ first: it is
# what a real login uses, so that is the path worth exercising when it works.
uid="$(id -u)"
if launchctl print "gui/${uid}" >/dev/null 2>&1; then
    DOMAIN="gui/${uid}"
else
    echo "note: no Aqua session; asserting against the user/ domain"
    DOMAIN="user/${uid}"
fi
readonly DOMAIN

workdir="$(mktemp -d)"
cleanup() {
    "$fastcached" --uninstall-service --service-scope=user --service-name="$SERVICE_NAME" >/dev/null 2>&1 || true
    rm -rf "$workdir"
}
trap cleanup EXIT

fail() { echo "macOS service E2E FAILED: $*" >&2; exit 1; }

# Wait for the port to accept a connection rather than sleeping a fixed amount:
# a cold CI runner is much slower than a warm developer machine.
wait_for_port() {
    local p="$1"
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/${p}") 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

# --- 1. install -------------------------------------------------------------
echo "== installing the launchd agent"
"$fastcached" --install-service --service-scope=user \
    --service-name="$SERVICE_NAME" --port="$port" \
    --storage="${workdir}/cache" > "${workdir}/install.log" 2>&1 \
    || fail "--install-service exited non-zero: $(cat "${workdir}/install.log")"

[[ -f "$PLIST" ]] || fail "no plist at $PLIST"

# A malformed plist is accepted by our writer and rejected by launchd, which
# reports nothing useful; plutil names the actual syntax error.
plutil -lint "$PLIST" >/dev/null || fail "plutil rejected $PLIST"

# --- 2. the job must be RUNNING, with a real pid ---------------------------
# Polled, not sampled once: immediately after bootstrap launchd reports the
# transient `xpcproxy` state while the stub execs the real binary. The states
# that matter are settling to `running` (good) or to `not running` (the job
# started and died, which is what a --daemon plist looks like).
job_state() {
    launchctl print "${DOMAIN}/${LABEL}" 2>/dev/null | awk -F'= ' '/^\tstate = /{print $2; exit}'
}

state=""
for _ in $(seq 1 100); do
    state="$(job_state)"
    [[ "$state" == "running" || "$state" == "not running" ]] && break
    sleep 0.1
done
[[ "$state" == "running" ]] || fail "expected state = running, got '${state:-<absent>}'"

pid="$(launchctl print "${DOMAIN}/${LABEL}" 2>/dev/null | awk -F'= ' '/^\tpid = /{print $2; exit}')"
[[ -n "$pid" ]] || fail "job has no pid; launchd started nothing"

# A forking daemon is reaped immediately, so the pid launchd knows about would
# not be a live process.
kill -0 "$pid" 2>/dev/null || fail "pid $pid is not alive; the job forked and was reaped"

# --- 3. the plist must not carry --daemon ----------------------------------
! grep -q -- "--daemon" "$PLIST" || fail "plist contains --daemon; launchd would reap the forked process"

# --- 4. it actually serves --------------------------------------------------
wait_for_port "$port" || fail "nothing listening on 127.0.0.1:${port}"
reply="$(printf 'set k 0 0 5\r\nhello\r\nget k\r\nquit\r\n' | nc -w 5 127.0.0.1 "$port" || true)"
[[ "$reply" == *"STORED"* ]] || fail "no STORED in reply: $reply"
[[ "$reply" == *"hello"*  ]] || fail "value did not round-trip: $reply"

# --- 5. reinstall is idempotent --------------------------------------------
# Exercises the bootout/bootstrap race directly: the job is running right now,
# so the second bootstrap only succeeds if the teardown was waited for.
echo "== reinstalling over the running job"
newport=$((port + 1))
"$fastcached" --install-service --service-scope=user \
    --service-name="$SERVICE_NAME" --port="$newport" \
    --storage="${workdir}/cache" > "${workdir}/reinstall.log" 2>&1 \
    || fail "reinstall exited non-zero: $(cat "${workdir}/reinstall.log")"

wait_for_port "$newport" || fail "reinstall did not take effect; nothing on ${newport}"

# --- 6. uninstall leaves nothing behind ------------------------------------
echo "== uninstalling"
"$fastcached" --uninstall-service --service-scope=user --service-name="$SERVICE_NAME" \
    > "${workdir}/uninstall.log" 2>&1 \
    || fail "--uninstall-service exited non-zero: $(cat "${workdir}/uninstall.log")"

[[ ! -f "$PLIST" ]] || fail "plist still present after uninstall: $PLIST"
! launchctl print "${DOMAIN}/${LABEL}" >/dev/null 2>&1 || fail "job still registered after uninstall"

echo "macOS service E2E: all assertions held"
