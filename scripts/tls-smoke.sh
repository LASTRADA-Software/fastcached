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
set -euo pipefail

fastcached=""
port="11811"
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

server_pid=""
cleanup() {
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" >/dev/null 2>&1 || true
        wait "$server_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT

"$fastcached" --bind 127.0.0.1 --port "$port" --tls --tls-cert "$cert" --tls-key "$key" &
server_pid=$!

# Wait until the daemon is accepting connections.
for _ in $(seq 1 50); do
    if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then
        exec 3>&- 3<&-
        break
    fi
    sleep 0.1
done

# Send PING then QUIT: the QUIT makes the daemon close the connection, so
# `openssl s_client` exits cleanly (it otherwise keeps the socket open —
# `-quiet` implies `-ign_eof` — and a fixed-size read would block forever).
resp="$(printf '*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nQUIT\r\n' \
    | openssl s_client -connect "127.0.0.1:${port}" -quiet 2>/dev/null \
    | head -c 128 || true)"
echo "response: ${resp}"
case "$resp" in
    *PONG*) echo "TLS smoke OK"; exit 0 ;;
    *) echo "no PONG over TLS"; exit 1 ;;
esac
