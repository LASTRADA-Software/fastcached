#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of the compile cache (POSIX). Starts fastcached, drives real
# compiles through fastcache-cc, and asserts the properties the launcher actually
# promises:
#
#   1. MISS then HIT      — a repeated compile is served from the cache.
#   2. Byte-identical     — the cached object equals the compiled one.
#   3. Large values       — an object past the daemon's 1 MiB socket send buffer
#                           still round-trips. Only such a reply reaches the
#                           reactor's park-and-resume path, and a bug there wedged
#                           a real build while every small fixture kept passing.
#   4. Cross-depth        — content stored from a DEEP checkout path HITs from a
#                           SHALLOW one, which is the whole reason this launcher
#                           exists instead of ccache/sccache.
#   5. Convergence        — after a header MOVES with its contents unchanged, the
#                           replayed depfile names the new path. Preprocessing
#                           suppresses line markers, so the object is invariant
#                           under such a move; a hit would otherwise replay a
#                           depfile naming a file that no longer exists, and Ninja
#                           would rebuild that TU on every build, forever
#                           (issue #53). The dependency set is part of the key, so
#                           the two layouts are two keys and the pre-move entry
#                           survives the move (issue #56).
#   6. Content in the key — an edited source must MISS. The preprocessed text is
#                           the only key input carrying the source's content, so a
#                           probe that captures none of it answers an edit with the
#                           previous revision's object: a wrong build, silently.
#                           Asserted with direct mode OFF, since its manifest hashes
#                           the source's own bytes and would mask exactly that.
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
export FASTCACHE_PREFETCH_GROUP="e2e"

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

export FASTCACHE_SOURCE_DIR="$proj"
export FASTCACHE_BINARY_DIR="${proj}/build"

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
grep -q "fastcache-cc: HIT" "${workdir}/hit.log" || fail "second compile was not served from the cache"
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

# --- 3: a value larger than the socket send buffer ---------------------------
# Every other fixture here compiles to a few KB, which never fills a send buffer
# and so never exercises the reactor's park-and-resume path. A reply that does
# is how a real build wedged: the daemon parked mid-reply and the launcher waited
# forever for bytes the header had already promised. The object must clear 1 MiB
# (the daemon's SO_SNDBUF) for this to mean anything, so assert that it does
# rather than assuming the compiler emitted what we expected.
echo "== large object (> 1 MiB) must round-trip =="
big="${workdir}/bigproj"
mkdir -p "${big}/build"
{
    echo "extern const int data[300000];"
    echo "const int data[300000] = {"
    awk 'BEGIN { for (i = 0; i < 300000; i++) printf "%d,\n", (i * 2654435761) % 2147483647 }'
    echo "};"
    echo "int main() { return data[0]; }"
} > "${big}/big.cpp"

export FASTCACHE_SOURCE_DIR="$big" FASTCACHE_BINARY_DIR="${big}/build"

"$launcher" "$compiler" -std=c++23 -O0 -c "${big}/big.cpp" -o "${big}/build/big.o" \
    2> "${workdir}/big-miss.log" || fail "large-object compile returned non-zero"
cat "${workdir}/big-miss.log"
grep -q "STORED" "${workdir}/big-miss.log" || fail "large object was not stored"

objbytes=$(wc -c < "${big}/build/big.o" | tr -d ' ')
[[ "$objbytes" -gt 1048576 ]] \
    || fail "large-object fixture is only ${objbytes} bytes; it must exceed 1 MiB to exercise the park path"
echo "   object is ${objbytes} bytes"

cp "${big}/build/big.o" "${workdir}/big-expected.o"
rm -f "${big}/build/big.o"

# A hung FETCH is the failure this guards, so bound the wait: without a cap a
# regression would hang CI until the job timed out instead of reporting.
"$launcher" "$compiler" -std=c++23 -O0 -c "${big}/big.cpp" -o "${big}/build/big.o" \
    2> "${workdir}/big-hit.log" &
big_pid=$!
big_waited=0
while kill -0 "$big_pid" 2>/dev/null; do
    if [[ "$big_waited" -ge 600 ]]; then
        kill -9 "$big_pid" 2>/dev/null || true
        fail "large-object FETCH did not complete within 60s (the daemon stalled mid-reply)"
    fi
    sleep 0.1
    big_waited=$((big_waited + 1))
done
wait "$big_pid" || fail "large-object second compile returned non-zero"
cat "${workdir}/big-hit.log"
grep -q "fastcache-cc: HIT" "${workdir}/big-hit.log" || fail "large object was not served from the cache"
cmp "${workdir}/big-expected.o" "${big}/build/big.o" || fail "large cached object differs from the compiled one"
echo "   large object reproduced byte-identically"

# --- 4: cross-depth portability ---------------------------------------------
# Same content, different checkout depth. The key must match, because paths
# under SOURCE_DIR/BINARY_DIR are tokenized before hashing.
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
export FASTCACHE_SOURCE_DIR="$deep" FASTCACHE_BINARY_DIR="${deep}/build"
"$launcher" "$compiler" -std=c++23 -MD -MF "${deep}/build/t.d" -c "${deep}/t.cpp" -o "${deep}/build/t.o" \
    2> "${workdir}/deep.log" || fail "deep compile returned non-zero"
cat "${workdir}/deep.log"
grep -q "STORED" "${workdir}/deep.log" || fail "deep compile did not store its result"

echo "== fetch from a SHALLOW checkout (expect HIT) =="
export FASTCACHE_SOURCE_DIR="$shallow" FASTCACHE_BINARY_DIR="${shallow}/build"
"$launcher" "$compiler" -std=c++23 -MD -MF "${shallow}/build/t.d" -c "${shallow}/t.cpp" -o "${shallow}/build/t.o" \
    2> "${workdir}/shallow.log" || fail "shallow compile returned non-zero"
cat "${workdir}/shallow.log"
grep -q "fastcache-cc: HIT" "${workdir}/shallow.log" \
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

# --- 5: a moved header must not replay a depfile naming its old path ---------
# The header's CONTENTS do not change, so the preprocessed text is byte-identical
# (line markers are suppressed). The depfile is nothing but paths, and the one on
# record names a file that no longer exists. Ninja records that dependency, cannot
# stat it, rebuilds the TU, hits the cache again, and never converges.
#
# The dependency path set is folded into the key, so the two layouts are two
# different keys and the move is a MISS by construction rather than a hit some
# guard had to catch and discard. Both properties are asserted below, because they
# are distinguishable and only the second one holds: the move must produce no
# "STALE HIT", and the PRE-MOVE entry must still be there afterwards.

# The property Ninja actually needs: every dependency a depfile lists must exist.
# Splices `\`-continuations, drops each rule's target (an output, and here one we
# deliberately deleted), and stats what remains. The field separator is a string
# rather than a /regex/ literal: as split()'s third argument a literal evaluates
# to 0 or 1 in a strictly-POSIX awk, and this runs on the BSD awk macOS ships.
require_depfile_resolves() {
    local label="$1" depfile="$2"
    local dep
    while read -r dep; do
        [[ -e "$dep" ]] || fail "${label}: depfile lists a dependency that does not exist: ${dep}"
    done < <(awk '
        { line = line $0 }
        sub(/\\$/, "", line) { next }
        {
            sub(/^[^:]*:/, "", line)
            n = split(line, parts, "[ \t]+")
            for (i = 1; i <= n; i++)
                if (parts[i] != "")
                    print parts[i]
            line = ""
        }
    ' "$depfile")
}

# Run twice: once in the default configuration and once with direct mode off,
# because the two reach the value by different routes and only the preprocessed
# one produced the reported failure.
#
# Each variant gets its own project directory AND its own content. The directory
# alone is not enough: paths under SOURCE_DIR are tokenized before hashing, so two
# trees holding the same bytes key identically and the second variant would open on
# a HIT against the first variant's entry rather than populating. That HIT is
# correct — it is the cross-checkout sharing test 4 exists for — but it is not what
# this test is about, and the string literal below (which survives preprocessing,
# unlike a comment) is what keeps the two sequences independent.
#
# The tag lives in the SOURCE, never in the header: the header's bytes must stay
# identical across the move, since a move that changed them would prove nothing.
write_move_source() {
    local root="$1" name="$2" include="$3"
    cat > "${root}/t.cpp" <<SRC
#include <${include}>
inline char const* variant() { return "${name}"; }
int main() { return answer() - 42; }
SRC
}

check_header_move() {
    local label="$1" name="$2"
    shift 2

    local root="${workdir}/${name}"
    mkdir -p "${root}/inc/old" "${root}/build"
    cat > "${root}/inc/old/Hdr.hpp" <<'HDR'
#pragma once
inline int answer() { return 42; }
HDR
    write_move_source "$root" "$name" "inc/old/Hdr.hpp"

    export FASTCACHE_SOURCE_DIR="$root" FASTCACHE_BINARY_DIR="${root}/build"

    # "MISS", not "not a HIT": a discarded stale hit reports MISS as well, and the
    # variants share a key, so this asserts the outcome rather than the route.
    echo "== ${label}: populate (expect MISS) =="
    "$@" "$launcher" "$compiler" -std=c++23 -I"$root" \
        -MD -MF "${root}/build/t.d" -c "${root}/t.cpp" -o "${root}/build/t.o" \
        2> "${workdir}/${name}-1.log" || fail "${label}: first compile returned non-zero"
    cat "${workdir}/${name}-1.log"
    grep -q "MISS" "${workdir}/${name}-1.log" || fail "${label}: first compile was not a MISS"
    grep -q "inc/old/Hdr.hpp" "${root}/build/t.d" || fail "${label}: depfile does not name the header"

    # Move it. Same bytes, new path — and update the include that finds it. The
    # source is rewritten wholesale rather than edited in place: `sed -i` takes a
    # backup suffix on BSD sed and none on GNU sed, so no single spelling works on
    # both macOS and Linux, and this script runs on both.
    mkdir -p "${root}/inc/new"
    mv "${root}/inc/old/Hdr.hpp" "${root}/inc/new/Hdr.hpp"
    rmdir "${root}/inc/old"
    write_move_source "$root" "$name" "inc/new/Hdr.hpp"
    rm -f "${root}/build/t.o" "${root}/build/t.d"

    echo "== ${label}: after the move (expect MISS, not a stale HIT) =="
    "$@" "$launcher" "$compiler" -std=c++23 -I"$root" \
        -MD -MF "${root}/build/t.d" -c "${root}/t.cpp" -o "${root}/build/t.o" \
        2> "${workdir}/${name}-2.log" || fail "${label}: second compile returned non-zero"
    cat "${workdir}/${name}-2.log"
    grep -q "MISS" "${workdir}/${name}-2.log" \
        || fail "${label}: a moved header still served a HIT, so the depfile is the producer's"
    # The dependency set is part of the key, so the move is a different key and the
    # value under the old one is never fetched at all. A "STALE HIT" here would mean
    # the two layouts still collide and the replay guard is carrying the property on
    # its own — true today, and exactly what issue #56 removed.
    # `if grep`, not `grep && fail`: under `set -e` an AND-list whose left side
    # fails takes the whole list's non-zero status, so the passing case would abort
    # the script.
    if grep -q "STALE HIT" "${workdir}/${name}-2.log"; then
        fail "${label}: the moved header still keyed identically and had to be discarded on replay"
    fi
    require_depfile_resolves "$label" "${root}/build/t.d"
    grep -q "inc/new/Hdr.hpp" "${root}/build/t.d" || fail "${label}: depfile does not name the moved header"

    # Third compile: the repaired entry must now hit, and keep naming the new
    # path. Without this the guard could "pass" by simply never hitting again.
    rm -f "${root}/build/t.o" "${root}/build/t.d"
    echo "== ${label}: repaired entry must HIT (convergence) =="
    "$@" "$launcher" "$compiler" -std=c++23 -I"$root" \
        -MD -MF "${root}/build/t.d" -c "${root}/t.cpp" -o "${root}/build/t.o" \
        2> "${workdir}/${name}-3.log" || fail "${label}: third compile returned non-zero"
    cat "${workdir}/${name}-3.log"
    # Anchored: the launcher also prints "fastcache-cc: STALE HIT (...); recompiling"
    # immediately BEFORE falling through to a MISS, so a bare `grep "HIT"` is
    # satisfied by the very state these assertions exist to reject.
    grep -q "fastcache-cc: HIT" "${workdir}/${name}-3.log" \
        || fail "${label}: the repaired entry did not hit, so every build recompiles this TU"
    require_depfile_resolves "$label" "${root}/build/t.d"
    grep -q "inc/new/Hdr.hpp" "${root}/build/t.d" || fail "${label}: the hit replayed the old path again"

    # Fourth compile: move the header BACK and the ORIGINAL entry must still be
    # there. This is what separates "a different key" from "a hit that was caught
    # and discarded": a guard-only fix re-stores the moved layout under the one
    # shared key, destroying the entry the old layout needs, so this compile would
    # MISS. Two keys means both layouts keep their own value.
    mkdir -p "${root}/inc/old"
    mv "${root}/inc/new/Hdr.hpp" "${root}/inc/old/Hdr.hpp"
    rmdir "${root}/inc/new"
    write_move_source "$root" "$name" "inc/old/Hdr.hpp"
    rm -f "${root}/build/t.o" "${root}/build/t.d"
    echo "== ${label}: moved back (the pre-move entry must have survived) =="
    "$@" "$launcher" "$compiler" -std=c++23 -I"$root" \
        -MD -MF "${root}/build/t.d" -c "${root}/t.cpp" -o "${root}/build/t.o" \
        2> "${workdir}/${name}-4.log" || fail "${label}: fourth compile returned non-zero"
    cat "${workdir}/${name}-4.log"
    grep -q "fastcache-cc: HIT" "${workdir}/${name}-4.log" \
        || fail "${label}: the pre-move entry was destroyed, so the two layouts share one key"
    if grep -q "STALE HIT" "${workdir}/${name}-4.log"; then
        fail "${label}: the restored layout still keyed onto the moved entry"
    fi
    require_depfile_resolves "$label" "${root}/build/t.d"
    grep -q "inc/old/Hdr.hpp" "${root}/build/t.d" || fail "${label}: the restored hit names the wrong path"

    # Nothing of the probe's own may outlive it. The dependency capture writes a
    # depfile beside the object, and a stray one would be an artefact no build
    # system asked for — in a directory a build system does clean and compare.
    [[ -z "$(find "${root}/build" -name '*.fcdep' -print -quit)" ]] \
        || fail "${label}: the dependency probe left its depfile behind"

    echo "   moved header: MISS, repaired, HIT, and the pre-move entry survived"
}

check_header_move "moved header" "movedhdr"
check_header_move "moved header (no direct mode)" "movedhdr-nodirect" env FASTCACHE_NO_DIRECT=1

# --- 6: an edited source must not be served the old object -------------------
# The preprocessed text is the only key input that carries the source's CONTENT,
# so a probe that fails to capture it produces a key that cannot tell two
# revisions of a file apart — and the cache then answers an edited source with
# the object built from the previous one. That is not a hit-rate problem, it is a
# WRONG BUILD, and it is silent: the compile succeeds every time.
#
# Direct mode is switched off deliberately. Its manifest hashes the source file's
# own bytes, so it catches an edit regardless of what the preprocessed text
# contains — which is exactly how a probe capturing nothing stayed invisible.
edited="${workdir}/edited"
mkdir -p "${edited}/build"
export FASTCACHE_SOURCE_DIR="$edited" FASTCACHE_BINARY_DIR="${edited}/build"
cat > "${edited}/e.cpp" <<'SRC'
int value() { return 1; }
SRC
echo "== edited source: first revision =="
FASTCACHE_NO_DIRECT=1 "$launcher" "$compiler" -std=c++23 -c "${edited}/e.cpp" -o "${edited}/build/e.o" \
    2> "${workdir}/edited-1.log" || fail "first revision returned non-zero"
cat "${workdir}/edited-1.log"
# The first revision must actually POPULATE. Without this the second revision's
# MISS is satisfied by an empty cache rather than by a changed key, and the whole
# section — the one written for the class of bug where the key carries no content
# from the source at all — proves nothing.
grep -q "STORED" "${workdir}/edited-1.log" || fail "first revision did not store, so the next MISS proves nothing"
[[ -f "${edited}/build/e.o" ]] || fail "first revision produced no object"
cp "${edited}/build/e.o" "${workdir}/edited-1.o"

cat > "${edited}/e.cpp" <<'SRC'
int value() { return 2; }
SRC
echo "== edited source: second revision (expect MISS and a different object) =="
FASTCACHE_NO_DIRECT=1 "$launcher" "$compiler" -std=c++23 -c "${edited}/e.cpp" -o "${edited}/build/e.o" \
    2> "${workdir}/edited-2.log" || fail "second revision returned non-zero"
cat "${workdir}/edited-2.log"
grep -q "MISS" "${workdir}/edited-2.log" \
    || fail "an edited source keyed identically to its previous revision"
if cmp -s "${workdir}/edited-1.o" "${edited}/build/e.o"; then
    fail "the edited source produced the previous revision's object"
fi
echo "   an edit re-keys, and the object follows the source"

# --- 7: the cache is never load-bearing -------------------------------------
# With no daemon reachable the build must still succeed, uncached.
#
# The layout is re-exported first: every section above exports its own, so
# without this the compile below runs `${proj}/a.cpp` under roots pointing at an
# unrelated tree and passes for reasons that have nothing to do with the daemon
# being unreachable.
export FASTCACHE_SOURCE_DIR="$proj" FASTCACHE_BINARY_DIR="${proj}/build"
echo "== unreachable daemon must still compile =="
FASTCACHE_ADDR="127.0.0.1:1" "$launcher" "$compiler" -std=c++23 -c "${proj}/a.cpp" -o "${proj}/build/fb.o" \
    2> "${workdir}/fallback.log" || fail "compile failed when the cache was unreachable"
cat "${workdir}/fallback.log"
[[ -f "${proj}/build/fb.o" ]] || fail "fallback compile produced no object"

# --- 8: forms the launcher must decline to cache ----------------------------
# A compile with no -o defaults its output to ./a.o, a path the launcher cannot
# reconstruct. It must pass straight through rather than claim the compile and
# then fail to store it on every single invocation.
echo "== a compile with no -o must pass through and still build =="
nooutdir="${workdir}/noout"
mkdir -p "$nooutdir"
cp "${proj}/a.cpp" "${proj}/hdr.hpp" "$nooutdir/"
export FASTCACHE_SOURCE_DIR="$nooutdir" FASTCACHE_BINARY_DIR="$nooutdir"
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

# A result too large to be worth caching is another form the launcher declines,
# and the one that proved a decline could be fatal: it used to stream the object
# at a daemon that refuses an over-cap frame and closes, and die of SIGPIPE
# mid-store -- so the build saw a command killed by signal 13 while the object
# file it asked for sat complete and correct on disk (issue #68). The ceiling
# declines before a byte moves; what must survive is the compile.
#
# Driven through FASTCACHE_MAX_STORE_BYTES rather than a genuinely over-cap
# object, which would mean generating and pushing 64+ MiB of fixture on every CI
# run to assert what the ceiling asserts here in milliseconds. The socket-level
# half -- a write to a peer that hung up reports an error instead of raising a
# signal -- is pinned in TcpClient_test.
echo "== a value over FASTCACHE_MAX_STORE_BYTES is skipped, and the build still succeeds =="
ceiling="${workdir}/ceilproj"
mkdir -p "${ceiling}/build"
cat > "${ceiling}/c.cpp" <<'EOF'
#include <string>
int main() { return static_cast<int>(std::string{"ceiling"}.size()); }
EOF

export FASTCACHE_SOURCE_DIR="$ceiling" FASTCACHE_BINARY_DIR="${ceiling}/build"

# 1 byte: every real object clears it, so this needs no assumption about what
# the compiler emitted.
FASTCACHE_MAX_STORE_BYTES=1 "$launcher" "$compiler" -std=c++23 -c "${ceiling}/c.cpp" -o "${ceiling}/build/c.o" \
    2> "${workdir}/ceiling-1.log" \
    || fail "compile past the store ceiling returned non-zero (the cache broke the build)"
cat "${workdir}/ceiling-1.log"
[[ -f "${ceiling}/build/c.o" ]] || fail "compile past the store ceiling produced no object"
grep -q "MISS" "${workdir}/ceiling-1.log" || fail "compile past the store ceiling was not reported as a MISS"
grep -q "FASTCACHE_MAX_STORE_BYTES" "${workdir}/ceiling-1.log" \
    || fail "the skipped store was not explained; an operator cannot act on a silent one"
grep -q "STORED" "${workdir}/ceiling-1.log" \
    && fail "a value over the ceiling was stored anyway"

# Nothing was written, so the next compile must MISS again rather than HIT. This
# is what separates "declined the store" from "stored it and said otherwise".
rm -f "${ceiling}/build/c.o"
FASTCACHE_MAX_STORE_BYTES=1 "$launcher" "$compiler" -std=c++23 -c "${ceiling}/c.cpp" -o "${ceiling}/build/c.o" \
    2> "${workdir}/ceiling-2.log" || fail "second compile past the store ceiling returned non-zero"
grep -q "MISS" "${workdir}/ceiling-2.log" || fail "a value over the ceiling was cached after all"
echo "   store declined, compile succeeded, nothing cached"

# The ceiling is opt-out: with it disabled the same TU caches normally, so a
# regression that left the check permanently on would surface here.
rm -f "${ceiling}/build/c.o"
FASTCACHE_MAX_STORE_BYTES=0 "$launcher" "$compiler" -std=c++23 -c "${ceiling}/c.cpp" -o "${ceiling}/build/c.o" \
    2> "${workdir}/ceiling-3.log" || fail "compile with the store ceiling disabled returned non-zero"
grep -q "STORED" "${workdir}/ceiling-3.log" \
    || fail "FASTCACHE_MAX_STORE_BYTES=0 did not disable the ceiling"
echo "   ceiling disabled by 0, as documented"

# A flag that merely starts like a dropped one must not be dropped: -coverage
# begins with -c, and eating it used to break preprocessing and force a
# permanent, silent fallback to uncached compiles.
echo "== a flag prefixed like a dropped flag must not break caching =="
export FASTCACHE_SOURCE_DIR="$proj" FASTCACHE_BINARY_DIR="${proj}/build"
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
"$launcher" --show-stats || fail "--show-stats returned non-zero"
"$launcher" -s >/dev/null || fail "-s returned non-zero"
"$launcher" --show-stats --prefetch-group "e2e" >/dev/null || fail "--show-stats --prefetch-group returned non-zero"

# The launcher's help must describe the flags it actually accepts; a drift here
# is exactly what the unit-level guard in LauncherCli_test.cpp protects, and this
# repeats it against the shipped binary.
help="$("$launcher" --help)" || fail "--help returned non-zero"
for flag in --show-stats -s --zero-stats -z --help -h --version --prefetch-group; do
    case "$help" in
        *"$flag"*) ;;
        *) fail "--help does not document ${flag}" ;;
    esac
done
echo "   --help documents every accepted flag"

# Retired spellings must be diagnosed, not spawned as if they were a compiler.
if "$launcher" --stats >/dev/null 2>&1; then
    fail "the retired --stats flag still succeeds"
fi
"$launcher" --stats >/dev/null 2>&1 || rc=$?
[ "${rc:-0}" -eq 2 ] || fail "retired flag should exit 2, got ${rc:-0}"
echo "   retired flags exit 2 with a diagnostic"

"$launcher" -z >/dev/null || fail "-z returned non-zero"
"$launcher" --zero-stats >/dev/null || fail "--zero-stats returned non-zero"

echo "compile-cache E2E OK: miss/hit, byte-identical, >1 MiB values, store ceiling, cross-depth," \
     "moved-header convergence (both layouts keyed apart), an edit re-keying," \
     "and safe fallback"
exit 0
