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
port="11611"
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
    memcached) export SCCACHE_MEMCACHED="tcp://127.0.0.1:${port}" ;;
    redis)     export SCCACHE_REDIS="redis://127.0.0.1:${port}" ;;
    *) echo "unknown protocol: '$protocol'" >&2; exit 2 ;;
esac
export SCCACHE_NO_DAEMON=0

workdir="$(mktemp -d)"
src="${workdir}/hello.cpp"
obj="${workdir}/hello.o"
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

"$fastcached" --port="$port" --log-level=info &
server_pid=$!
sleep 1

sccache --stop-server >/dev/null 2>&1 || true
sccache --start-server
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
