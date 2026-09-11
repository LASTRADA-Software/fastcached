#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# sccache smoke test (POSIX). Start fastcached, point sccache at it over the
# chosen wire protocol, compile a one-file program twice, and assert the
# second compile is served from cache. Shared by the CI smoke jobs and the
# CTest `smoke` tests so both exercise exactly the same path.
#
# Usage:
#   sccache-smoke.sh --fastcached <path> [--protocol memcached|redis]
#                    [--port <n>] [--compiler <cxx>]
#
# Exit codes: 0 = cache hit observed; 1 = ran but no cache hit; 77 = a runtime
# prerequisite (sccache / fastcached / compiler) was missing — treated as a
# skip by CTest's SKIP_RETURN_CODE.
set -euo pipefail

fastcached=""
protocol="memcached"
# Empty means DRAW one; `--port` still overrides, which is what CI and a human
# debugging a run need. A FIXED port needs a reaper and this fixture never had one,
# so a run that missed its cleanup left a daemon holding 11611 for the life of the
# machine and every later run failed against it, naming neither the port nor the
# process (#220 is that same defect one fixture over).
#
# The reason offered for the constant was TRUE and did not support it: the backend
# URL must be decided before sccache starts, which is an argument for deciding the
# port EARLY -- not for fixing it. Drawing it early is what this file now does.
port=""
compiler="${CXX:-c++}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) fastcached="$2"; shift 2 ;;
        --protocol)   protocol="$2";   shift 2 ;;
        --port)       port="$2";       shift 2 ;;
        --compiler)   compiler="$2";   shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

command -v sccache >/dev/null 2>&1 || { echo "sccache not found; skipping"; exit "$SKIP"; }
[[ -n "$fastcached" && -x "$fastcached" ]] || { echo "fastcached not found: '$fastcached'; skipping"; exit "$SKIP"; }
command -v "$compiler" >/dev/null 2>&1 || { echo "compiler not found: '$compiler'; skipping"; exit "$SKIP"; }

case "$protocol" in
    memcached|redis) ;;
    *) echo "unknown protocol: '$protocol'" >&2; exit 2 ;;
esac
export SCCACHE_NO_DAEMON=0

# The shared fixture library: `free_port`, `port_answers` and the bounded waits.
# Sourced AFTER the prerequisite checks above, which exit 77 for a skip -- the
# library's `fail` exits 1, and a missing sccache is not a failure.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"

workdir="$(mktemp -d)"
src="${workdir}/hello.cpp"
obj="${workdir}/hello.o"
daemon_log="${workdir}/fastcached.log"
cat > "$src" <<'EOF'
#include <string>
int main() { return static_cast<int>(std::string{"hi"}.size()); }
EOF

server_pid=""
cleanup() {
    sccache --stop-server >/dev/null 2>&1 || true
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" >/dev/null 2>&1 || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$workdir"
}
trap cleanup EXIT

# After the EXIT trap, which is this library's stated precondition: a `fail` raised
# in a subshell arrives here as SIGTERM, and `e2e_begin` turns it back into an
# ordinary exit so the cleanup above still runs.
e2e_begin "sccache smoke (${protocol})" "$workdir"

# Drawn from below the kernel's ephemeral range, where a connect probe can answer:
# a port above the floor may be an outbound connection's local endpoint with nothing
# listening, so the probe says free and the bind still fails.
#
# What keeps the two protocols apart is `RUN_SERIAL`, NOT `free_port`'s ledger. That
# ledger lives at `${_e2e_workdir}/.issued-ports` and `_e2e_workdir` is the `mktemp -d`
# above, so it is per PROCESS -- and the two smokes are two ctest processes with two
# workdirs. It covers a fixture drawing several ports before binding any of them,
# which is one draw here. Stated because the tempting reading is that the ledger
# makes the pair safe, and a reason that reaches further than the fact behind it is
# what gets a `RUN_SERIAL` deleted later as redundant.
[ -n "$port" ] || port="$(free_port)"

case "$protocol" in
    memcached) export SCCACHE_MEMCACHED="tcp://127.0.0.1:${port}" ;;
    redis)     export SCCACHE_REDIS="redis://127.0.0.1:${port}" ;;
esac

"$fastcached" --port="$port" --log-level=info > "$daemon_log" 2>&1 &
server_pid=$!
# Waits on the LISTENER, bounded, and says which kind of failure it was: a flat
# `sleep 1` is either flaky on a cold runner or slow on a warm one, and when it is
# too short the failure surfaces later as sccache being unable to reach a backend
# that was merely not up yet.
wait_for_port 127.0.0.1 "$port" "$server_pid" "fastcached" "$daemon_log"

sccache --stop-server >/dev/null 2>&1 || true
# Starting the server is where sccache validates that the requested backend is
# compiled in. A binary built without the chosen feature (e.g. no redis support)
# fails here with "Cache type not supported with current feature configuration".
# That is a missing runtime prerequisite, not a fastcached fault, so map it to a
# skip rather than a hard failure.
if ! start_err="$(sccache --start-server 2>&1)"; then
    echo "$start_err" >&2
    if grep -qi "feature configuration" <<< "$start_err"; then
        echo "sccache lacks ${protocol} backend support; skipping"
        exit "$SKIP"
    fi
    exit 1
fi
sccache --zero-stats >/dev/null

# First compile: cache miss (populates the cache via fastcached).
sccache "$compiler" -std=c++23 -c "$src" -o "$obj"
rm -f "$obj"
# Second compile: must be a cache hit.
sccache "$compiler" -std=c++23 -c "$src" -o "$obj"

stats="$(sccache --show-stats)"
echo "$stats"
if echo "$stats" | grep -E "Cache hits[[:space:]]+[1-9]" >/dev/null; then
    echo "sccache smoke (${protocol}) OK: cache hit observed"
    exit 0
fi
echo "sccache smoke (${protocol}) FAILED: no cache hit" >&2
exit 1
