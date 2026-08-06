#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of the compile cache (POSIX). Starts fastcached, drives real
# compiles through fastcache-cc, and asserts the three properties the launcher
# actually promises:
#
#   1. MISS then HIT      — a repeated compile is served from the cache.
#   2. Byte-identical     — the cached object equals the compiled one.
#   3. Cross-depth        — content stored from a DEEP checkout path HITs from a
#                           SHALLOW one, which is the whole reason this launcher
#                           exists instead of ccache/sccache.
#
# The PowerShell counterpart (src/apps/fastcache-cc/run-launcher-e2e.ps1) asserts
# the same contract against cl / clang-cl on Windows.
#
# Usage:
#   compile-cache-e2e.sh --fastcached <path> --launcher <path>
#                        [--port <n>] [--compiler <cxx>]
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a runtime
# prerequisite was missing (skip).
set -euo pipefail

fastcached=""
launcher=""
port="21713"
compiler="${CXX:-c++}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) fastcached="$2"; shift 2 ;;
        --launcher)   launcher="$2";   shift 2 ;;
        --port)       port="$2";       shift 2 ;;
        --compiler)   compiler="$2";   shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

[[ -n "$fastcached" && -x "$fastcached" ]] || { echo "fastcached not found: '$fastcached'; skipping"; exit "$SKIP"; }
[[ -n "$launcher"   && -x "$launcher"   ]] || { echo "fastcache-cc not found: '$launcher'; skipping"; exit "$SKIP"; }
command -v "$compiler" >/dev/null 2>&1 || { echo "compiler not found: '$compiler'; skipping"; exit "$SKIP"; }

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

# Statistics are per-user state; keep this run out of the developer's real log.
export XDG_STATE_HOME="${workdir}/state"
export FASTCACHE_VERBOSE=1
export FASTCACHE_COHORT="e2e"

fail() { echo "compile-cache E2E FAILED: $*" >&2; exit 1; }

# --- start the daemon -------------------------------------------------------
# The value cap must exceed the object size; the default 16 MiB is ample for
# these tiny fixtures, but pass it explicitly so the flag stays exercised.
"$fastcached" --bind=127.0.0.1 --port="$port" --storage-max-value=64M --log-level=info \
    > "${workdir}/daemon.log" 2>&1 &
server_pid=$!

# Wait for the listener rather than sleeping a fixed amount: a cold CI runner
# can take noticeably longer than a warm developer machine.
ready=""
for _ in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then ready=1; break; fi
    if ! kill -0 "$server_pid" 2>/dev/null; then
        cat "${workdir}/daemon.log" >&2
        fail "daemon exited before it started listening"
    fi
    sleep 0.2
done
[[ -n "$ready" ]] || { cat "${workdir}/daemon.log" >&2; fail "daemon never listened on port ${port}"; }

export FASTCACHE_ADDR="127.0.0.1:${port}"

# --- 1 + 2: miss, then hit reproducing the object byte-identically ----------
proj="${workdir}/proj"
mkdir -p "${proj}/build"
cat > "${proj}/hdr.hpp" <<'EOF'
#pragma once
inline int helper() { return 7; }
EOF
cat > "${proj}/a.cpp" <<'EOF'
#include "hdr.hpp"
#include <string>
int main() { return helper() + static_cast<int>(std::string{"hi"}.size()); }
EOF

export FASTCACHE_SRCROOT="$proj"
export FASTCACHE_BUILDTREE="${proj}/build"

echo "== compile 1 (expect MISS) =="
"$launcher" "$compiler" -std=c++23 -MD -MF "${proj}/build/a.d" -c "${proj}/a.cpp" -o "${proj}/build/a.o" \
    2> "${workdir}/miss.log" || fail "first compile returned non-zero"
cat "${workdir}/miss.log"
[[ -f "${proj}/build/a.o" ]] || fail "first compile produced no object"
grep -q "MISS" "${workdir}/miss.log" || fail "first compile was not reported as a MISS"
grep -q "STORED" "${workdir}/miss.log" || fail "first compile did not store its result"
[[ -f "${proj}/build/a.d" ]] || fail "first compile produced no depfile"

cp "${proj}/build/a.o" "${workdir}/expected.o"
cp "${proj}/build/a.d" "${workdir}/expected.d"
# Remove BOTH: a hit must reproduce the depfile as well as the object. Leaving
# the depfile in place would let a launcher that never restores it still pass.
rm -f "${proj}/build/a.o" "${proj}/build/a.d"

echo "== compile 2 (expect HIT) =="
"$launcher" "$compiler" -std=c++23 -MD -MF "${proj}/build/a.d" -c "${proj}/a.cpp" -o "${proj}/build/a.o" \
    2> "${workdir}/hit.log" || fail "second compile returned non-zero"
cat "${workdir}/hit.log"
grep -q "HIT" "${workdir}/hit.log" || fail "second compile was not served from the cache"
[[ -f "${proj}/build/a.o" ]] || fail "cache hit did not write the object"
cmp "${workdir}/expected.o" "${proj}/build/a.o" || fail "cached object differs from the compiled one"
echo "   object reproduced byte-identically"

# The depfile is the build system's header-dependency record. A hit that omits
# it leaves Ninja/Make believing the TU depends on nothing, so it silently stops
# rebuilding when its headers change — a correctness bug no object comparison
# catches, because the object itself is perfect.
[[ -f "${proj}/build/a.d" ]] || fail "cache hit did not restore the depfile"
grep -q "hdr.hpp" "${proj}/build/a.d" \
    || fail "restored depfile does not list the header the TU includes"
echo "   depfile restored on the hit"

# --- 3: cross-depth portability ---------------------------------------------
# Same content, different checkout depth. The key must match, because paths
# under SRCROOT/BUILDTREE are tokenized before hashing.
deep="${workdir}/a/b/c/d/e/deepproj"
shallow="${workdir}/s"
mkdir -p "${deep}/build" "${shallow}/build"
for root in "$deep" "$shallow"; do
    cat > "${root}/hdr.hpp" <<'EOF'
#pragma once
inline int depth() { return 3; }
EOF
    cat > "${root}/t.cpp" <<'EOF'
#include "hdr.hpp"
int main() { return depth(); }
EOF
done

echo "== store from a DEEP checkout =="
export FASTCACHE_SRCROOT="$deep" FASTCACHE_BUILDTREE="${deep}/build"
"$launcher" "$compiler" -std=c++23 -MD -MF "${deep}/build/t.d" -c "${deep}/t.cpp" -o "${deep}/build/t.o" \
    2> "${workdir}/deep.log" || fail "deep compile returned non-zero"
cat "${workdir}/deep.log"
grep -q "STORED" "${workdir}/deep.log" || fail "deep compile did not store its result"

echo "== fetch from a SHALLOW checkout (expect HIT) =="
export FASTCACHE_SRCROOT="$shallow" FASTCACHE_BUILDTREE="${shallow}/build"
"$launcher" "$compiler" -std=c++23 -MD -MF "${shallow}/build/t.d" -c "${shallow}/t.cpp" -o "${shallow}/build/t.o" \
    2> "${workdir}/shallow.log" || fail "shallow compile returned non-zero"
cat "${workdir}/shallow.log"
grep -q "HIT" "${workdir}/shallow.log" \
    || fail "cross-depth portability broken: content stored from a deep checkout did not hit from a shallow one"
cmp "${deep}/build/t.o" "${shallow}/build/t.o" || fail "cross-depth object differs"
echo "   cross-depth hit reproduced the object byte-identically"

# The depfile restored from a hit must name THIS checkout's paths. If the stored
# depfile were replayed verbatim, the shallow checkout would get a file full of
# the deep checkout's absolute paths — pointing at another tree, or at nothing.
[[ -f "${shallow}/build/t.d" ]] || fail "cross-depth hit did not restore the depfile"
grep -q "${shallow}" "${shallow}/build/t.d" \
    || fail "restored depfile was not localized to the consuming checkout"
# `if !` rather than `grep && fail`: under `set -e` a non-matching grep exits 1
# and would abort the script on the SUCCESS path.
if grep -q "${deep}" "${shallow}/build/t.d"; then
    fail "restored depfile still carries the producing checkout's paths"
fi
echo "   depfile localized to the consuming checkout"

# --- 4: the cache is never load-bearing -------------------------------------
# With no daemon reachable the build must still succeed, uncached.
echo "== unreachable daemon must still compile =="
FASTCACHE_ADDR="127.0.0.1:1" "$launcher" "$compiler" -std=c++23 -c "${proj}/a.cpp" -o "${proj}/build/fb.o" \
    2> "${workdir}/fallback.log" || fail "compile failed when the cache was unreachable"
cat "${workdir}/fallback.log"
[[ -f "${proj}/build/fb.o" ]] || fail "fallback compile produced no object"

# --- 5: forms the launcher must decline to cache ----------------------------
# A compile with no -o defaults its output to ./a.o, a path the launcher cannot
# reconstruct. It must pass straight through rather than claim the compile and
# then fail to store it on every single invocation.
echo "== a compile with no -o must pass through and still build =="
nooutdir="${workdir}/noout"
mkdir -p "$nooutdir"
cp "${proj}/a.cpp" "${proj}/hdr.hpp" "$nooutdir/"
export FASTCACHE_SRCROOT="$nooutdir" FASTCACHE_BUILDTREE="$nooutdir"
# The compiler defaults its output to ./a.o, so this must run FROM that
# directory; `$launcher` may be a relative path, so resolve it before the cd.
launcher_abs="$(cd "$(dirname "$launcher")" && pwd)/$(basename "$launcher")"
( cd "$nooutdir" && "$launcher_abs" "$compiler" -std=c++23 -c a.cpp 2> "${workdir}/noout.log" ) \
    || { cat "${workdir}/noout.log" >&2; fail "compile without -o returned non-zero"; }
cat "${workdir}/noout.log"
[[ -f "${nooutdir}/a.o" ]] || fail "compile without -o produced no object"
# It is not a cache candidate at all, so it must report neither outcome.
if grep -qE "MISS|HIT" "${workdir}/noout.log"; then
    fail "compile without -o was treated as cacheable"
fi
echo "   passed through uncached, object still produced"

# A flag that merely starts like a dropped one must not be dropped: -coverage
# begins with -c, and eating it used to break preprocessing and force a
# permanent, silent fallback to uncached compiles.
echo "== a flag prefixed like a dropped flag must not break caching =="
export FASTCACHE_SRCROOT="$proj" FASTCACHE_BUILDTREE="${proj}/build"
"$launcher" "$compiler" -std=c++23 -coverage -c "${proj}/a.cpp" -o "${proj}/build/cov.o" \
    2> "${workdir}/coverage.log" || fail "compile with -coverage returned non-zero"
cat "${workdir}/coverage.log"
[[ -f "${proj}/build/cov.o" ]] || fail "compile with -coverage produced no object"
if grep -q "preprocess failed" "${workdir}/coverage.log"; then
    fail "-coverage was mistaken for -c and broke the preprocess probe"
fi
grep -qE "MISS|HIT" "${workdir}/coverage.log" \
    || fail "compile with -coverage was not a cache candidate"
echo "   -coverage survived the preprocess line"

# --- statistics -------------------------------------------------------------
echo "== statistics =="
"$launcher" --stats || fail "--stats returned non-zero"

echo "compile-cache E2E OK: miss/hit, byte-identical, cross-depth, and safe fallback"
exit 0
