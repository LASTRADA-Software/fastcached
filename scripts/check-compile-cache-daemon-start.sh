#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of FASTCACHE_AUTO_START (POSIX) — the other half of issue
# #90 from scripts/check-compile-cache-daemon-staging.cmake, which proves the
# module can STAGE a fastcached daemon out of a mirrored release archive with
# fake, --version-only stand-ins. What that mirror test cannot reach is
# whether _fc_auto_start_fastcached actually spawns a REAL daemon that
# answers real connections — a fake shell script has no socket to accept on
# — so this drives the real, freshly built fastcached and fastcache-cc
# through a real configure instead.
#
# What is asserted:
#   1. Daemon appears     — with FASTCACHE_AUTO_START=ON, FASTCACHE_CC already
#                           pointed at the real launcher, and FASTCACHE_ADDR
#                           naming a port nothing is listening on, the
#                           configure leaves a fastcached process listening
#                           on that port when it exits — not merely staged,
#                           actually running.
#   2. Cache actually works — a real translation unit compiled through the
#                           configured launcher against that daemon reports a
#                           MISS then a HIT, i.e. this is not just "a process
#                           exists" but "the cache the module exists to
#                           provide is functioning" end to end.
#   3. No double-start     — a SECOND configure against the same address finds
#                           the daemon the first one started and does not
#                           spawn a second one (see the race-safety design in
#                           _fc_daemon_answering's own comment): the daemon's
#                           pidfile names the same PID after both configures.
#   4. Never fails a configure — every prerequisite this script itself cannot
#                           meet (no POSIX daemon path, no compiler) is a skip,
#                           not a failure, mirroring the module's own
#                           constraint.
#
# This does NOT go through FASTCACHE_AUTO_INSTALL's download path at all —
# the real fastcached built by this project's own build is placed exactly
# where the module would have staged one, so the test exercises the SPAWN and
# PROBE logic without needing network access or a published release, which
# check-compile-cache-daemon-staging.cmake already covers separately with a
# synthetic mirror.
#
# Windows has no --daemon double-fork equivalent and this script's process
# bookkeeping (pidfile-based) is POSIX-specific; a Windows smoke test is left
# as follow-up work rather than attempted here, the same scoping
# check-compile-cache-install.cmake and check-compile-cache-autoinstall.cmake
# already use for their own sandboxes.
#
# Usage:
#   check-compile-cache-daemon-start.sh --fastcached <path> --launcher <path>
#                                        --compiler <cxx> --source-dir <repo>
#
# Exit codes: 0 = every assertion held; 1 = a failure; 77 = a runtime
# prerequisite was missing (skip).
set -euo pipefail

fastcached=""
launcher=""
compiler="${CXX:-c++}"
sourceDir=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) fastcached="$2"; shift 2 ;;
        --launcher)   launcher="$2";   shift 2 ;;
        --compiler)   compiler="$2";   shift 2 ;;
        --source-dir) sourceDir="$2";  shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

[[ -n "$fastcached" && -x "$fastcached" ]] || { echo "fastcached not found: '$fastcached'; skipping"; exit "$SKIP"; }
[[ -n "$launcher"   && -x "$launcher"   ]] || { echo "fastcache-cc not found: '$launcher'; skipping"; exit "$SKIP"; }
[[ -n "$sourceDir"  && -d "$sourceDir"  ]] || { echo "source dir not found: '$sourceDir'; skipping"; exit "$SKIP"; }
command -v "$compiler" >/dev/null 2>&1 || { echo "compiler not found: '$compiler'; skipping"; exit "$SKIP"; }
command -v cmake >/dev/null 2>&1 || { echo "cmake not found; skipping"; exit "$SKIP"; }

fixtureDir="${sourceDir}/src/tests/compile-cache-fixture"
[[ -d "$fixtureDir" ]] || { echo "no fixture project at ${fixtureDir}; skipping"; exit "$SKIP"; }

workdir="$(mktemp -d)"
pids=()
cleanup() {
    # Same reasoning as dist-compile-e2e.sh's own cleanup: SIGTERM then a
    # bounded grace period, never a bare `wait`, because a fixture that hangs
    # on cleanup reports a suite timeout naming nothing instead of the actual
    # failure. The daemon here is additionally found via its PIDFILE rather
    # than a PID execute_process would hand back, because --daemon
    # double-forks and detaches — the parent this script could have waited on
    # is long gone by the time the configure that started it returns.
    if [[ -n "${daemonPidfile:-}" && -f "$daemonPidfile" ]]; then
        daemonPid="$(cat "$daemonPidfile" 2>/dev/null || true)"
        [[ -n "$daemonPid" ]] && pids+=("$daemonPid")
    fi
    for pid in ${pids+"${pids[@]}"}; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    for pid in ${pids+"${pids[@]}"}; do
        for _ in $(seq 1 25); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.2
        done
        kill -9 "$pid" >/dev/null 2>&1 || true
        wait "$pid" 2>/dev/null || true
    done
    rm -rf "$workdir"
}
trap cleanup EXIT

# The shared helpers: `fail`, `free_port` and `wait_for_port`, one copy for every
# POSIX fixture (#449). The three this file had were near-copies of
# dist-compile-e2e.sh's, and two of them had drifted:
#
#   * `free_port` had no issued-port ledger. Only one port is drawn here today,
#     so it was latent rather than wrong -- and latent-because-of-the-caller is
#     how the next caller inherits a bug.
#   * `wait_for_port` checked NO liveness at all, so a daemon that died at
#     `bind()` was reported as `never answered on port N` after the full bound
#     had burned. A slow machine and a dead process are fixed in different
#     places and that message cannot tell them apart, which is the thing
#     `.agent/rules/testing.md` asks a bounded wait to do.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "compile-cache daemon-start" "$workdir"

port="$(free_port)"
addr="127.0.0.1:${port}"
stageDir="${workdir}/stage"
storageDir="${stageDir}/daemon-storage"
buildDir1="${workdir}/build1"
buildDir2="${workdir}/build2"
daemonPidfile="${storageDir}/fastcached.pid"

# Stage the real, freshly built binaries exactly where the module would have
# put them after auto-installing from a real release — same directory shape
# (<dir>/<version>/<platform>/<exe>) — so FASTCACHE_AUTO_INSTALL never has
# anything to do and this test needs no network access. The platform token
# MUST match the asset-table row the module itself resolves for this host
# (_fc_asset_<row>_platform in CompileCache.cmake) — _fc_auto_start_fastcached
# builds its staging path from that row, not from anything this script
# invents, so a made-up label here would make the module look in a directory
# this script never populated and fall through to "no binary staged". This
# oracle is independent of the module for the same reason
# check-compile-cache-install.cmake's own oracle is: a rename on one side
# must be caught, not agreed to by both sides at once.
hostSystem="$(uname -s)"
hostMachine="$(uname -m)"
case "${hostSystem}-${hostMachine}" in
    Linux-x86_64)  platform="Linux-x86_64" ;;
    Darwin-arm64)  platform="Darwin-arm64" ;;
    Darwin-aarch64) platform="Darwin-arm64" ;;
    *) echo "no published-asset row for ${hostSystem}-${hostMachine}; skipping"; exit "$SKIP" ;;
esac
# Must match CompileCache.cmake's own numeric X.Y.Z regex (the same shape
# cmake/Version.cmake insists on) — a suffixed value like "0.0.0-test" would
# be refused as "not a numeric X.Y.Z version" before ever reaching the
# staged-binary EXISTS check this test relies on.
version="0.0.0"
binDir="${stageDir}/${version}/${platform}"
mkdir -p "$binDir"
cp "$fastcached" "${binDir}/fastcached"
cp "$launcher" "${binDir}/fastcache-cc"
chmod +x "${binDir}/fastcached" "${binDir}/fastcache-cc"

commonArgs=(
    -S "$fixtureDir"
    "-DFASTCACHED_MODULE_DIR=${sourceDir}/cmake/portable"
    "-DCMAKE_CXX_COMPILER=${compiler}"
    "-DFASTCACHE_CC=${binDir}/fastcache-cc"
    "-DFASTCACHE_ADDR=${addr}"
    "-DFASTCACHE_AUTO_START=ON"
    "-DFASTCACHE_AUTO_START_STORAGE_DIR=${storageDir}"
    # AUTO_INSTALL stays OFF: FASTCACHE_CC is already supplied above, so the
    # only thing left for auto-start to do is find/stage/spawn the DAEMON,
    # which _fc_auto_start_fastcached still needs a resolvable release row
    # for. Pinning AUTO_INSTALL_DIR at the same stage directory and a version
    # this test controls means it finds the daemon already sitting there
    # (see the EXISTS check ahead of any fetch in _fc_auto_start_fastcached)
    # and never reaches the network.
    "-DFASTCACHE_AUTO_INSTALL_DIR=${stageDir}"
    "-DFASTCACHE_AUTO_INSTALL_VERSION=${version}"
)

echo "--- first configure: expect a daemon to be spawned ---"
cmake "${commonArgs[@]}" -B "$buildDir1" >"${workdir}/configure1.log" 2>&1 \
    || { cat "${workdir}/configure1.log" >&2; fail "first configure failed"; }
cat "${workdir}/configure1.log"

grep -q "Auto-started fastcached" "${workdir}/configure1.log" \
    || fail "first configure never reported starting a daemon"

# The daemon is double-forked and detached by the configure, so this script never
# held its pid -- but the configure has returned by now and the pidfile is how
# anything here finds it. Read if it is there, `-` if it is not: a wait told `-`
# says INCONCLUSIVE on expiry rather than implying it watched something.
daemonPid="-"
[[ -f "$daemonPidfile" ]] && daemonPid="$(cat "$daemonPidfile" 2>/dev/null || echo -)"
wait_for_port 127.0.0.1 "$port" "$daemonPid" "fastcached" "${workdir}/configure1.log"

[[ -f "$daemonPidfile" ]] || fail "no pidfile at ${daemonPidfile} after the daemon was reported started"
firstPid="$(cat "$daemonPidfile")"
kill -0 "$firstPid" 2>/dev/null || fail "pidfile names PID ${firstPid}, which is not running"

# --- assertion 2: the cache actually works end to end ------------------------
echo "--- compiling a real translation unit through the launcher ---"
src="${workdir}/probe.cpp"
cat > "$src" <<'EOF'
int daemonStartProbe() { return 42; }
EOF

compile_once() {
    FASTCACHE_ADDR="$addr" \
    FASTCACHE_SOURCE_DIR="$sourceDir" \
    FASTCACHE_BINARY_DIR="$buildDir1" \
    FASTCACHE_VERBOSE=1 \
    FASTCACHE_NO_STATS=1 \
    "${binDir}/fastcache-cc" "$compiler" -c "$src" -o "${workdir}/probe.o" 2>&1
}

# Same output path both times, deliberately: the object path is one of the
# arguments the key is computed over (relativized when it falls under
# FASTCACHE_SOURCE_DIR/FASTCACHE_BINARY_DIR, left absolute and hashed verbatim
# otherwise — which is exactly this probe's situation, compiling out of a
# mktemp workdir outside both roots). Two different output filenames would
# therefore key two different entries and the second compile would MISS
# forever, not because the daemon or the cache is broken but because the two
# calls were never asking for the same cache entry.
firstOutput="$(compile_once)"
echo "$firstOutput"
echo "$firstOutput" | grep -q "fastcache-cc: MISS key=" || fail "first compile against the auto-started daemon was not a MISS"

secondOutput="$(compile_once)"
echo "$secondOutput"
echo "$secondOutput" | grep -q "fastcache-cc: HIT key=" || fail "second compile against the auto-started daemon was not a HIT"

# --- assertion 3: a second configure must not start a second daemon ---------
echo "--- second configure: expect the SAME daemon to be found, not a new one ---"
cmake "${commonArgs[@]}" -B "$buildDir2" >"${workdir}/configure2.log" 2>&1 \
    || { cat "${workdir}/configure2.log" >&2; fail "second configure failed"; }
cat "${workdir}/configure2.log"

grep -q "Auto-started fastcached" "${workdir}/configure2.log" \
    && fail "second configure started ANOTHER daemon instead of finding the first"

secondPid="$(cat "$daemonPidfile")"
[[ "$firstPid" == "$secondPid" ]] || fail "pidfile changed from ${firstPid} to ${secondPid}; a second daemon replaced the first"
kill -0 "$secondPid" 2>/dev/null || fail "the original daemon (PID ${secondPid}) is no longer running"

echo "compile-cache daemon-start: spawned once, cache verified end to end, second configure found the same daemon"
