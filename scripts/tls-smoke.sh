#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# TLS smoke test (POSIX). Start fastcached with TLS terminated using the
# checked-in self-signed cert, connect with `openssl s_client`, send a RESP
# PING, and assert the daemon answers +PONG over the encrypted channel.
#
# Usage:
#   tls-smoke.sh --fastcached <path> --cert <pem> --key <pem> [--port <n>]
#
# Exit codes: 0 = +PONG over TLS; 1 = ran but no PONG; 77 = a runtime
# prerequisite (openssl / fastcached / cert) was missing — treated as a skip.
#
# The port is DRAWN per run unless one is passed. It used to be the constant
# `11811` — the same constant `tls-smoke.ps1` carried, so the two halves of one
# fixture could not even run side by side — and `RUN_SERIAL` is no defence
# against a second worktree or a daemon left behind by a run that missed its
# cleanup (#1284). This is the arm Linux CI runs.
set -euo pipefail

fastcached=""
# Empty, never a number: an empty default is what makes the draw below reachable
# from the only caller there is. #183 records the shape of getting this half
# wrong — a fixture whose script drew a port while its registration passed
# `--port 11611` unconditionally reads as converted in the diff and binds one
# constant on every run.
port=""
cert=""
key=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) fastcached="$2"; shift 2 ;;
        --port)       port="$2";       shift 2 ;;
        --cert)       cert="$2";       shift 2 ;;
        --key)        key="$2";        shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77
command -v openssl >/dev/null 2>&1 || { echo "openssl not found; skipping"; exit "$SKIP"; }
[[ -n "$fastcached" && -x "$fastcached" ]] || { echo "fastcached not found: '$fastcached'; skipping"; exit "$SKIP"; }
[[ -f "$cert" && -f "$key" ]] || { echo "cert/key not found; skipping"; exit "$SKIP"; }

workdir="$(mktemp -d)"
server_pid=""
cleanup() {
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" >/dev/null 2>&1 || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$workdir"
}
trap cleanup EXIT

# After the EXIT trap, which is this library's stated precondition: a `fail`
# raised in a subshell arrives here as SIGTERM, and `e2e_begin` turns it back
# into an ordinary exit so the cleanup above still runs.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "TLS smoke" "$workdir"

# Drawn from below the kernel's ephemeral range, where a connect probe can
# answer: a port above the floor may be an outbound connection's local endpoint
# with nothing listening, so the probe says free and the bind still fails.
# `free_port`'s comment carries that argument in full.
[[ -n "$port" ]] || port="$(free_port)"

"$fastcached" --bind 127.0.0.1 --port "$port" --tls --tls-cert "$cert" --tls-key "$key" \
    > "${workdir}/fastcached.log" 2>&1 &
server_pid=$!

# Bounded, and it says what it waited for and which KIND of failure a timeout
# was. The counted `for _ in $(seq 1 50)` this replaces was neither: it reported
# nothing when five seconds were not enough, so a cold runner surfaced as
# `openssl` failing to connect — a diagnosis of the wrong thing — and its
# "5 seconds" was a product of the loop shape that no run ever observed.
wait_for_port 127.0.0.1 "$port" "$server_pid" "fastcached with TLS" "${workdir}/fastcached.log"

# Send PING then QUIT: the QUIT makes the daemon close the connection, so
# `openssl s_client` exits cleanly (it otherwise keeps the socket open —
# `-quiet` implies `-ign_eof` — and a fixed-size read would block forever).
raw="$(printf '*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nQUIT\r\n' \
    | openssl s_client -connect "127.0.0.1:${port}" -quiet 2>/dev/null || true)"
# The first 128 characters, in bash. `| head -c 128` would leave openssl writing
# into a closed pipe, and `pipefail` under the `set -e` above then reports
# OPENSSL's status for a perfectly good response (#1181).
resp="${raw:0:128}"
echo "response: ${resp}"
case "$resp" in
    *PONG*) echo "TLS smoke OK"; exit 0 ;;
    *) echo "no PONG over TLS"; exit 1 ;;
esac
