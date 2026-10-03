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
#   7. Relative paths     — a build whose driver reports relative dependency paths
#                           must still record a manifest naming them. They lie under
#                           no root, so a classifier that asked "is this toolchain
#                           content?" before "is this anchored anywhere?" dropped
#                           every one of them, and an empty manifest validates
#                           against anything: an edited header served the previous
#                           object under a zero exit code, permanently.
#  10. Root-bound object — a translation unit whose object names its checkout
#                           (`__FILE__`, `source_location`) is not served into a
#                           second checkout in either direct mode, one naming no
#                           path still is, the first checkout still hits its own
#                           copy, and a hit FASTCACHE_VERIFY rejects is logged
#                           VERIFY-MISMATCH rather than HIT.
#   2c. Dropped object    — the operator's repair after 2b names a wrong object: the
#                           key is dropped over CACHE-DROP while its direct-mode
#                           manifest stands. The next compile must MISS, recompile
#                           and store, and the one after must HIT (#1276). Runs with
#                           2b, so it needs --testclient.
#
# The PowerShell counterpart (src/apps/fastcache-cc/run-launcher-e2e.ps1) asserts
# the same contract against cl / clang-cl on Windows — except case 7, which has no
# Windows twin on purpose: `cl` resolves every `/showIncludes` note through the
# filesystem and reports an absolute path whatever the `/I` spelling, so the case's
# own premise ("the driver reported a relative path") cannot hold there, and a case
# that silently degrades to a tautology is worse than an absent one. clang-cl does
# echo the spelling it was handed, so a clang-cl-only variant would be meaningful;
# the classification itself is pinned by DirectManifest_test on every platform.
#
# Usage:
#   compile-cache-e2e.sh --fastcached <path> --launcher <path>
#                        [--port <n>] [--compiler <cxx>]
#                        [--testclient <path>]
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a runtime
# prerequisite was missing (skip).
set -euo pipefail

fastcached=""
launcher=""
testclient=""
port=""
compiler="${CXX:-c++}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) fastcached="$2"; shift 2 ;;
        --launcher)   launcher="$2";   shift 2 ;;
        --port)       port="$2";       shift 2 ;;
        --compiler)   compiler="$2";   shift 2 ;;
        --testclient) testclient="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

[[ -n "$fastcached" && -x "$fastcached" ]] || { echo "fastcached not found: '$fastcached'; skipping"; exit "$SKIP"; }
command -v "$compiler" >/dev/null 2>&1 || { echo "compiler not found: '$compiler'; skipping"; exit "$SKIP"; }

workdir="$(mktemp -d)"
server_pid=""
auth_pid=""
# Every background job this shell started -- both daemons, and the launcher the
# large-object case backgrounds -- is reaped on every path out, BOUNDED:
# `reap_background_jobs` escalates to SIGKILL and names a job that outlives even
# that rather than waiting on it. The `kill; wait` pair this replaced waited
# unboundedly, and a local gate hung in it for 80 minutes on a daemon the kernel
# could not finish killing. A daemon left holding a port also makes the NEXT run
# fail at startup for a reason that has nothing to do with the run that broke.
cleanup() {
    reap_background_jobs
    rm -rf "$workdir"
    e2e_exit_if_reap_left_survivors
}
trap cleanup EXIT

# The shared helpers: `fail`, `free_port`, `wait_for_port` -- one copy for every
# POSIX fixture (#449). This file's own `free_port` was a near-copy of
# dist-compile-e2e.sh's WITHOUT the issued-port ledger, and it draws two ports
# (the plain daemon and the authenticating one), which is exactly the shape the
# ledger exists for: nothing is listening on a port issued a moment ago whose
# server has not bound yet, so both draws could return it and the second daemon
# would die of EADDRINUSE.
#
# The reasoning that used to live here -- why a connect probe and not a bind
# probe, and why the range stops below the kernel's ephemeral range -- is above
# `free_port` in `lib/e2e-common.sh`, in full. The part that is about THIS
# fixture stays below, at the check that the daemon still listening is the one it
# started: this test's readiness probe cannot tell its own daemon from a
# stranger's, and both halves of that mistake show up as a claim about caching --
# a first compile reported as a HIT, or a second one that was not served -- with
# nothing anywhere naming the port.
#
# Sourcing it also clears every FASTCACHE_* this shell inherited (an operator's
# FASTCACHE_SCHEDULER would dispatch each "local" compile to their fleet), which is
# why this fixture's own exports come AFTER it.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "compile-cache E2E" "$workdir"

# After `e2e_begin`, whose snapshot of the caller's statistics must precede every use of
# the launcher variable -- `launcher-state-isolation` refuses one above it.
[[ -n "$launcher"   && -x "$launcher"   ]] || { echo "fastcache-cc not found: '$launcher'; skipping"; exit "$SKIP"; }

# Every launcher this fixture runs records into a state directory of the run's own,
# through the shim `e2e_launcher_state_enter` (scripts/lib/e2e-common.sh) puts in
# front of it. Before any launcher runs, and after `e2e_begin`, whose workdir holds it.
e2e_launcher_state_enter launcher
export FASTCACHE_VERBOSE=1
export FASTCACHE_PREFETCH_GROUP="e2e"

[[ -n "$port" ]] || port="$(free_port)"

# --- start the daemon -------------------------------------------------------
# The value cap must exceed the object size; the default 16 MiB is ample for
# these tiny fixtures, but pass it explicitly so the flag stays exercised.
"$fastcached" --bind=127.0.0.1 --port="$port" --storage-max-value=64M --log-level=info \
    > "${workdir}/daemon.log" 2>&1 &
server_pid=$!

wait_for_port 127.0.0.1 "$port" "$server_pid" "daemon" "${workdir}/daemon.log"

# Something answered; make sure it is OURS. The loop above breaks the moment the
# port is connectable, which a daemon somebody else left behind satisfies just as
# well -- and this script would then spend its whole run asserting things about a
# cache it does not control.
kill -0 "$server_pid" 2>/dev/null || {
    cat "${workdir}/daemon.log" >&2
    fail "something else is listening on port ${port}: our daemon is gone"
}

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

# --- 2b: a wrong object under a correct key must be detected -----------------
# The one failure this whole cache exists not to have, and until #423 the one
# nothing here could observe: #368 was noticed only because the stale object
# happened to crash, and one that linked and passed would have left no trace.
#
# It is PLANTED rather than provoked, because no way of driving the launcher can
# produce it -- the launcher derives the key from the source, so it cannot
# disagree with itself. The test client is the only thing that stores a chosen
# value under a chosen key.
#
# The plant compiles a DIFFERENT source that includes the SAME header, and both
# halves of that are load-bearing. Different source, or the object might come out
# byte-identical and the case would prove nothing. Same header, or the value's
# dependency record names paths this checkout does not have, `MaterializeHit`
# declares the hit stale, the launcher recompiles for an ordinary reason, and the
# case would pass while never exercising verification at all.
if [[ -n "$testclient" && -x "$testclient" ]]; then
    echo "== a planted wrong object must be named, and the fresh one used =="

    # The key the launcher actually used, read off its own trace rather than
    # recomputed here: a second implementation of the key would be a second thing
    # to be wrong, and the key is precisely what this case is about.
    planted_keys="$(sed -n 's/^fastcache-cc: MISS key=//p' "${workdir}/miss.log")"
    planted_key="${planted_keys%%$'\n'*}"
    [[ -n "$planted_key" ]] || fail "could not read the object key out of the MISS trace"

    cat > "${proj}/wrong.cpp" <<'EOF'
#include "hdr.hpp"
#include <string>
int main() { return (helper() * 7) + static_cast<int>(std::string{"a different translation unit"}.size()); }
EOF

    "$testclient" store \
        --host 127.0.0.1 --port "$port" \
        --key "$planted_key" \
        --prefetch-group "e2e" \
        --srcroot "$proj" --buildtree "${proj}/build" \
        --compiler "$compiler" --source "${proj}/wrong.cpp" \
        --out "${workdir}/planted.o" > "${workdir}/plant.log" 2>&1 \
        || { cat "${workdir}/plant.log" >&2; fail "could not plant an object under the launcher's key"; }

    # The plant has to actually differ, or this case passes for the wrong reason.
    # A comparison that never fired and a comparison that fired and matched are
    # indistinguishable from outside, which is the shape the whole feature exists
    # to refuse.
    if cmp -s "${workdir}/expected.o" "${workdir}/planted.o"; then
        fail "the planted object is identical to the real one, so this case would prove nothing"
    fi

    rm -f "${proj}/build/a.o" "${proj}/build/a.d"
    FASTCACHE_VERIFY=1 "$launcher" "$compiler" -std=c++23 -MD -MF "${proj}/build/a.d" \
        -c "${proj}/a.cpp" -o "${proj}/build/a.o" 2> "${workdir}/verify.log" \
        || { cat "${workdir}/verify.log" >&2; fail "the verifying compile returned non-zero"; }
    # Only the launcher's own lines: the rest is the PLANTED value's replayed
    # streams. The plant stores the launcher's layout -- its depfile in the depfile
    # region, not on stdout -- because since #1531 a value with no depfile region is
    # not served to a compile that names a depfile, and this case would then verify
    # nothing.
    grep '^fastcache-cc:' "${workdir}/verify.log" || true

    # A hit the verifier REJECTS is traced `VERIFY-MISMATCH`, never `HIT` -- the outcome the
    # build actually used -- so that word, on this key, is the proof the plant was served.
    grep -q "fastcache-cc: VERIFY-MISMATCH key=${planted_key}" "${workdir}/verify.log" \
        || fail "the planted object was not served and rejected, so nothing was verified (a hit FASTCACHE_VERIFY rejects is VERIFY-MISMATCH)"
    grep -q "WRONG OBJECT" "${workdir}/verify.log" \
        || fail "a wrong object was served and nothing said so"
    grep -q "$planted_key" "${workdir}/verify.log" \
        || fail "the mismatch was reported without naming the key, so the entry cannot be looked at"

    # And the half that matters more than the message: what the build is left
    # holding. A launcher that named the mismatch and then linked the cache's
    # object would be worse than one that said nothing, because it would have been
    # believed.
    cmp "${workdir}/expected.o" "${proj}/build/a.o" \
        || fail "the wrong object was left on disk after being detected"
    [[ ! -e "${proj}/build/a.o.fastcache-verify" ]] \
        || fail "verification left its scratch copy in the build tree"
    echo "   the wrong object was named, and the freshly compiled one was used"

    # --- 2c: the operator removes the wrong object; its manifest stays (#1276) ---
    # The repair the case above leaves to a person: `FASTCACHE_VERIFY` named the key,
    # and CACHE-DROP takes that key away. What must follow is a MISS and a recompile,
    # never a wrong or a failed build -- and the load-bearing part is that the
    # launcher's direct-mode MANIFEST still stands, pointing at the key that is now
    # gone. `ValidateManifest` asserts the dependencies and never that the object
    # exists, so a launcher that trusted a validated manifest to have an object behind
    # it is exactly what this state would catch.
    #
    # The manifest's presence is ASSERTED before the recompile, because without it the
    # case would exercise the other direction -- no manifest, an ordinary miss -- and
    # pass while proving nothing about this one.
    echo "== a dropped object whose manifest stands must recompile, then hit again =="
    manifest_keys="$(sed -n 's/^fastcache-cc: MANIFEST stored key=\([^ ]*\).*/\1/p' "${workdir}/miss.log")"
    manifest_key="${manifest_keys%%$'\n'*}"
    [[ -n "$manifest_key" ]] || fail "could not read the manifest key out of the first compile's trace"

    # `|| drop_rc=$?`, never a bare `$?` on the next line: this script runs under `set -e`,
    # so the miss below -- exit 4, an ANSWER -- would end the whole run before it was read.
    drop_rc=0
    "$testclient" drop --host 127.0.0.1 --port "$port" --key "$planted_key" > "${workdir}/drop-1.log" 2>&1 || drop_rc=$?
    [[ "$drop_rc" -eq 0 ]] || { cat "${workdir}/drop-1.log" >&2; fail "dropping the planted key exited ${drop_rc}, not 0"; }
    grep -q "DROP ok key=${planted_key}" "${workdir}/drop-1.log" || fail "the drop did not report removing the key"

    # A second drop is a MISS, which is an answer: exit 4, never a refusal.
    drop_rc=0
    "$testclient" drop --host 127.0.0.1 --port "$port" --key "$planted_key" > "${workdir}/drop-2.log" 2>&1 || drop_rc=$?
    [[ "$drop_rc" -eq 4 ]] || { cat "${workdir}/drop-2.log" >&2; fail "a second drop exited ${drop_rc}, not 4 (a miss)"; }

    manifest_rc=0
    "$testclient" fetch --host 127.0.0.1 --port "$port" --key "$manifest_key" \
        --srcroot "$proj" --buildtree "${proj}/build" > "${workdir}/manifest-fetch.log" 2>&1 || manifest_rc=$?
    # Present means SERVED -- exit 0 and its own line -- rather than "not a miss": a socket
    # failure also exits non-4, and would otherwise read as a manifest that is there.
    [[ "$manifest_rc" -eq 0 ]] && grep -q "FETCH ok key=${manifest_key}" "${workdir}/manifest-fetch.log" \
        || { cat "${workdir}/manifest-fetch.log" >&2; fail "the manifest was not served (exit ${manifest_rc}), so the recompile below would not exercise a manifest without its object"; }

    rm -f "${proj}/build/a.o" "${proj}/build/a.d"
    "$launcher" "$compiler" -std=c++23 -MD -MF "${proj}/build/a.d" -c "${proj}/a.cpp" -o "${proj}/build/a.o" \
        2> "${workdir}/dropped.log" \
        || { cat "${workdir}/dropped.log" >&2; fail "the compile after the drop returned non-zero"; }
    grep '^fastcache-cc:' "${workdir}/dropped.log" || true
    if grep -q "fastcache-cc: HIT" "${workdir}/dropped.log"; then
        fail "the compile after the drop was served from the cache, so the drop removed nothing a fetch reads"
    fi
    grep -q "STORED key=${planted_key}" "${workdir}/dropped.log" \
        || fail "the compile after the drop did not store a fresh object under the dropped key"
    cmp "${workdir}/expected.o" "${proj}/build/a.o" || fail "the recompiled object differs from the one compiled first"

    rm -f "${proj}/build/a.o" "${proj}/build/a.d"
    "$launcher" "$compiler" -std=c++23 -MD -MF "${proj}/build/a.d" -c "${proj}/a.cpp" -o "${proj}/build/a.o" \
        2> "${workdir}/redropped-hit.log" \
        || { cat "${workdir}/redropped-hit.log" >&2; fail "the compile after the repair returned non-zero"; }
    grep -q "fastcache-cc: HIT" "${workdir}/redropped-hit.log" \
        || fail "the compile after the repair was not served, so the repair left the cache cold"
    cmp "${workdir}/expected.o" "${proj}/build/a.o" || fail "the repaired entry serves a different object"
    echo "   the manifest outlived its object, the launcher recompiled, and the repaired entry hits"

    # The entry is deliberately NOT repaired by the LAUNCHER: which side of the
    # disagreement is wrong is not knowable from one machine, and a launcher that
    # overwrote the fleet's entry from a host whose own environment is the anomaly
    # would turn one bad build into everyone's. A person decides, and 2c above is what
    # that person's repair does. Nothing below fetches this key -- the two later
    # compiles of the same source run with an unreachable daemon and with `-coverage`,
    # which keys differently.
    rm -f "${proj}/build/a.o" "${proj}/build/a.d" "${proj}/wrong.cpp"
else
    # Said out loud rather than skipped in silence: FASTCACHED_BUILD_TESTCLIENT is
    # off by default and on for the linux and clang-tidy jobs, so a reader of a
    # local run must be told which of those they are looking at.
    echo "== SKIPPED: no --testclient built, so the planted-wrong-object case did not run =="
fi

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

# --- 4b: one root nested inside another --------------------------------------
# Split out of #108 and filed as #116. Both existing root cases cover ALIASING --
# two spellings naming one location, where the right answer is "these are the
# same tree". Nesting is the opposite: two genuinely different trees where one
# root is a strict string PREFIX of the other.
#
# It matters because every root test in `PathCanon` is a prefix comparison, so
# from the outer layout's point of view every file in the inner tree is in-tree
# source, and both trees canonicalize their own `src/inc/h.hpp` to the identical
# token `<SRCROOT>/src/inc/h.hpp`. Two different files, one token.
#
# Sharing is nonetheless CORRECT, because the preprocessed text is part of the
# key -- but that is an argument, and arguments are what regression tests are
# for. Anyone running `git worktree add .worktrees/foo` inside their checkout
# produces exactly this shape, which makes it a good deal more ordinary than a
# symlinked or substituted root.
#
# `FASTCACHE_NO_DIRECT=1` throughout, so what is exercised is the OBJECT key
# rather than the manifest -- the manifest path would answer from recorded
# dependencies and never reach the prefix comparison this is about.
outer="${workdir}/nest/outer"
inner="${outer}/nested/inner"
mkdir -p "${outer}/src/inc" "${outer}/build" "${inner}/src/inc" "${inner}/build"

write_nest_tree() {
    local root="$1" value="$2"
    cat > "${root}/src/inc/h.hpp" <<EOF
#pragma once
inline int nested() { return ${value}; }
EOF
    cat > "${root}/src/t.cpp" <<'EOF'
#include "inc/h.hpp"
int main() { return nested(); }
EOF
}

compile_nest() {
    local root="$1" tag="$2"
    export FASTCACHE_SOURCE_DIR="$root" FASTCACHE_BINARY_DIR="${root}/build"
    FASTCACHE_NO_DIRECT=1 "$launcher" "$compiler" -std=c++23 -MD -MF "${root}/build/t.d"         -c "${root}/src/t.cpp" -o "${root}/build/t.o" 2> "${workdir}/${tag}.log"         || fail "nested-root compile ${tag} returned non-zero"
    cat "${workdir}/${tag}.log"
}

write_nest_tree "$outer" 41
write_nest_tree "$inner" 41

echo "== nested roots: store from the OUTER tree =="
compile_nest "$outer" nest-outer
grep -q "STORED" "${workdir}/nest-outer.log" || fail "the outer tree did not store its result"

echo "== nested roots: identical content in the INNER tree (expect HIT) =="
compile_nest "$inner" nest-inner
grep -q "fastcache-cc: HIT" "${workdir}/nest-inner.log"     || fail "a nested root did not share with its parent: identical content missed"
cmp "${outer}/build/t.o" "${inner}/build/t.o" || fail "nested-root hit did not reproduce the object"
echo "   a root nested inside another shares with it, and the object is identical"

# The depfile must name the CONSUMER's tree. This is the assertion nesting makes
# sharp: the producer's paths are a strict prefix of the consumer's, so a depfile
# replayed verbatim would still "contain the consumer root" by accident when
# tested loosely. Assert the inner path is present AND that no line names the
# outer tree's own `src/` -- which only the producer's spelling can.
[[ -f "${inner}/build/t.d" ]] || fail "nested-root hit did not restore the depfile"
grep -q "${inner}" "${inner}/build/t.d" || fail "restored depfile was not localized to the nested tree"
if grep -qE "${outer}/src/" "${inner}/build/t.d"; then
    fail "restored depfile names the OUTER tree's sources from inside the nested one"
fi
echo "   the depfile is localized to the nested tree, not the enclosing one"

echo "== nested roots: a changed HEADER in the inner tree must not be served =="
write_nest_tree "$inner" 41
cat > "${inner}/src/inc/h.hpp" <<'EOF'
#pragma once
inline int nested() { return 99; }
EOF
compile_nest "$inner" nest-hdr
# BOTH directions, deliberately. `STORED` present is not by itself proof that
# nothing was served -- that would be reading the absence of a HIT out of the
# presence of a STORE, and these two lines being mutually exclusive is an
# assumption about the launcher's logging rather than about its behaviour. The
# mis-serve this case exists to catch shows up as a HIT, so say so.
grep -q "STORED" "${workdir}/nest-hdr.log"     || fail "a changed header in the nested tree did not store a new object"
if grep -q "fastcache-cc: HIT" "${workdir}/nest-hdr.log"; then
    fail "a changed header in the nested tree was SERVED the enclosing tree's object"
fi
echo "   a changed header misses, so one token for two files is not a mis-serve"

echo "== nested roots: a changed SOURCE in the inner tree must not be served =="
write_nest_tree "$inner" 41
cat > "${inner}/src/t.cpp" <<'EOF'
#include "inc/h.hpp"
int main() { return nested() + 1; }
EOF
compile_nest "$inner" nest-src
grep -q "STORED" "${workdir}/nest-src.log"     || fail "a changed source in the nested tree did not store a new object"
if grep -q "fastcache-cc: HIT" "${workdir}/nest-src.log"; then
    fail "a changed source in the nested tree was SERVED the enclosing tree's object"
fi
echo "   a changed source misses too"

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

# --- 7: a relatively-spelled build must still populate a usable manifest -----
# Direct mode revalidates a translation unit by re-hashing the files its manifest
# names, and BuildManifest decided what to name by asking ClassifyAgainstRoots —
# which calls every path outside both roots toolchain, and a RELATIVE path lies under
# no root at all. So a build whose driver reported relative paths (a relative `-I`,
# or a compile run from the source directory, which is how the CMake Ninja generator
# spells its sources) recorded a manifest with those entries silently missing, and an
# empty manifest validates against anything. Edit a dropped header, leave the .cpp
# alone, and the direct hit fires on a stale object with a zero exit code — forever,
# because a direct hit never re-records the manifest that let it through.
#
# Unit tests pin the classification; only this can show that the driver really does
# report relative paths and that the whole flow survives them, which is the same
# reason the three key properties above are asserted here rather than in isolation.
reldir="${workdir}/relative"
mkdir -p "${reldir}/inc" "${reldir}/src" "${reldir}/build"
cat > "${reldir}/inc/h.hpp" <<'HDR'
#pragma once
inline int answer() { return 42; }
HDR
# The tag keeps this sequence off every other section's key: paths under
# SOURCE_DIR are tokenized before hashing, so two trees holding the same bytes
# key identically and this would open on another case's entry.
cat > "${reldir}/src/r.cpp" <<'SRC'
#include <h.hpp>
inline char const* variant() { return "relative-paths"; }
int main() { return answer() - 42; }
SRC

export FASTCACHE_SOURCE_DIR="$reldir" FASTCACHE_BINARY_DIR="${reldir}/build"

# Absolute before the cd, because either may have been passed as a relative path.
rel_launcher="$(cd "$(dirname "$launcher")" && pwd)/$(basename "$launcher")"
rel_compiler="$compiler"
[[ "$rel_compiler" == */* ]] \
    && rel_compiler="$(cd "$(dirname "$compiler")" && pwd)/$(basename "$compiler")"

# Everything relative: the source, the include directory, the object and the
# depfile. Run from the source root, as a hand-driven or in-source build does.
run_relative() {
    local log="$1"
    ( cd "$reldir" \
        && "$rel_launcher" "$rel_compiler" -std=c++23 -Iinc \
               -MD -MF build/r.d -c src/r.cpp -o build/r.o ) 2> "$log"
}

echo "== relative paths: populate (expect MISS) =="
run_relative "${workdir}/relative-1.log" || fail "relative: first compile returned non-zero"
cat "${workdir}/relative-1.log"
grep -q "MISS" "${workdir}/relative-1.log" || fail "relative: first compile was not a MISS"
grep -q "STORED" "${workdir}/relative-1.log" \
    || fail "relative: first compile did not store, so nothing below proves anything"
# The premise, asserted rather than assumed: if this driver reported ABSOLUTE
# paths the whole section would pass without ever exercising the defect.
grep -qE '(^|[[:space:]])inc/h\.hpp([[:space:]]|$)' "${reldir}/build/r.d" \
    || fail "relative: the driver reported no relative dependency path, so this case proves nothing"
# The manifest must name the TU and its header, not just whatever was absolute.
grep -q "manifest: 2 entries" "${workdir}/relative-1.log" \
    || fail "relative: the manifest did not record both the source and its header"
cp "${reldir}/build/r.o" "${workdir}/relative-1.o"

echo "== relative paths: unchanged tree must HIT =="
rm -f "${reldir}/build/r.o" "${reldir}/build/r.d"
run_relative "${workdir}/relative-2.log" || fail "relative: second compile returned non-zero"
cat "${workdir}/relative-2.log"
# Anchored, for the reason check_header_move records: the launcher prints
# "STALE HIT (...); recompiling" on its way to a MISS, so a bare grep for HIT is
# satisfied by exactly the collapse these assertions exist to reject.
grep -q "fastcache-cc: HIT" "${workdir}/relative-2.log" \
    || fail "relative: an unchanged tree did not hit, so direct mode never populated"

# The property this case exists for. Only the header changes; the .cpp is not
# touched. Before the fix the manifest named neither, so it validated and the
# previous revision's object was replayed.
cat > "${reldir}/inc/h.hpp" <<'HDR'
#pragma once
inline int answer() { return 43; }
HDR
rm -f "${reldir}/build/r.o" "${reldir}/build/r.d"
echo "== relative paths: an edited header must not be served the old object =="
run_relative "${workdir}/relative-3.log" || fail "relative: third compile returned non-zero"
cat "${workdir}/relative-3.log"
grep -q "MISS" "${workdir}/relative-3.log" \
    || fail "relative: an edited header was served from a manifest that never named it"
if cmp -s "${workdir}/relative-1.o" "${reldir}/build/r.o"; then
    fail "relative: the edited header produced the previous revision's object"
fi
echo "   relative paths reach the manifest, and an edit to one re-keys"

# --- 8: the cache is never load-bearing -------------------------------------
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

# --- 8b: every way a dead cache can be silent, inside a bound -----------------
# The leg above is one shape of a dead cache: a port that refuses. A peer that is LISTENING
# but never accepts, and one that accepts and then resets, are the other two, and a
# regression specific to one of them -- a launcher that waits forever on the silent one --
# would pass the leg above. So each shape compiles here against a peer of its own, must exit
# 0 with the object a working cache would have left, and must do it inside a BOUND: a hang is
# then a named failure rather than the test's own timeout.
#
# The bound is DERIVED, never picked, and it is per SHAPE. The baseline is the same compile
# against the live daemon, on the same monotonic clock: what a compile costs when the cache
# works. One compile makes at most `dead_exchanges` exchanges before it gives up on the cache --
# direct mode's manifest round trip, then the object fetch; never the STORE, which a fetch the
# cache did not serve skips (`fastcache-cc --help` says the same under FASTCACHE_TIMEOUT). No
# scheduler is configured, so there is no dispatch budget to add.
#
# What ONE exchange can cost is the deadline that ends it, which differs by shape -- the
# `dead_shapes` table below:
#   never-accepting    the connect completes into the backlog, so the exchange runs out TOTAL
#   refused            the dial fails: at once here, after SYN retries on Windows, never past CONNECT
#   accept-then-reset  the RST arrives at once; at most CONNECT
# A shape's bound is baseline + N x cost + cost / 2: N exchanges fit with half an exchange to
# spare, and ONE exchange more overshoots it by half an exchange of that shape. The shared bound
# this replaces, baseline + N x (connect + total), left never-accepting -- which never spends a
# connect -- exactly one whole exchange of slack, so a launcher retrying a failed fetch once
# passed it on Windows and failed it here by 6 ms (review I-1). A shape whose exchanges cost
# NOTHING in practice -- the reset, and a refusal on this platform -- cannot be held to a count by
# time at all; the reset leg COUNTS its exchanges instead.
#
# The peers and the clock are perl, because bash can neither listen nor read a monotonic
# clock (`SECONDS` is the wall clock, which this host steps). A missing perl, or a missing core
# module, fails the run -- see below.
dead_connect_ms=1000
dead_total_ms=2000
dead_exchanges=2
# shape, then the NAME of the deadline one exchange against it can spend. The silent shape
# LAST: this fixture stops at its first failure, and a hang there is the regression the bound
# exists for, so the other two have reported by the time it can fire.
dead_shapes="refused dead_connect_ms
accept-then-reset dead_connect_ms
never-accepting dead_total_ms"
dead_peer_pid=""
dead_launch_pid=""
dead_ended_ms=0
dead_accepts=""

# Milliseconds on a monotonic clock.
dead_now_ms() {
    perl -MTime::HiRes=clock_gettime,CLOCK_MONOTONIC -e 'printf "%d\n", clock_gettime(CLOCK_MONOTONIC) * 1000' # perl-lifetime: a foreground clock read; it exits at once and is never backgrounded
}

# A loopback peer that writes its port to @2 and then, for @1 `silent`, never accepts --
# the kernel still completes every handshake into its backlog, so a client connects, writes
# its request and waits for a reply nobody sends -- or, for `reset`, accepts each connection
# and closes it with a zero linger, which is an RST, appending one line per accept to
# `@2.accepts` BEFORE the reset, so the line exists by the time the launcher sees it. It lives
# at most @3 seconds.
dead_peer() {
    # Run as `dead_peer ... &`, a SUBSHELL, which inherits this fixture's EXIT trap -- the run's
    # cleanup. Cleared first thing, which closes the window from here until the `exec` into perl
    # below replaces the shell (#1084). It cannot close the window BEFORE this line runs; the
    # `kill -KILL` that ends the peer is what covers that one.
    trap - EXIT TERM INT HUP
    local program='
        use IO::Socket::INET; use Socket qw(SOL_SOCKET SO_LINGER);
        my ($mode, $portfile) = @ARGV;
        my $listener = IO::Socket::INET->new(LocalAddr => "127.0.0.1", LocalPort => 0, Listen => 16)
            or die "listen: $!";
        open(my $out, ">", "$portfile.tmp") or die "$portfile: $!";
        print $out $listener->sockport, "\n";
        close $out;
        rename("$portfile.tmp", $portfile) or die "$portfile: $!";
        if ($mode eq "silent") { select(undef, undef, undef, 3600) while 1; }
        while (my $client = $listener->accept) {
            open(my $log, ">>", "$portfile.accepts") or die "$portfile.accepts: $!";
            print $log "accept\n";
            close $log;
            setsockopt($client, SOL_SOCKET, SO_LINGER, pack("ii", 1, 0));
            close $client;
        }'
    e2e_bounded_perl "$3" "$program" "$1" "$2"
}

dead_peer_ready() { [[ -s "${workdir}/dead-peer.port" ]]; }

# Readiness of the launcher run below: it has exited. Records when it was noticed.
dead_launch_done() {
    kill -0 "$dead_launch_pid" 2>/dev/null && return 1
    dead_ended_ms="$(dead_now_ms)"
}

# One compile against @1, in the background, bounded by @2 ms. Leaves the exit status in
# `dead_status` and the elapsed monotonic time in `dead_elapsed_ms`.
dead_compile() {
    local addr="$1" bound_ms="$2" what="$3" started
    rm -f "${dead}/build/d.o"
    started="$(dead_now_ms)"
    FASTCACHE_ADDR="$addr" FASTCACHE_CONNECT_TIMEOUT="${dead_connect_ms}ms" FASTCACHE_TIMEOUT="${dead_total_ms}ms" \
        "$launcher" "$compiler" -std=c++23 -c "${dead}/d.cpp" -o "${dead}/build/d.o" 2> "${workdir}/dead.log" &
    dead_launch_pid=$!
    # Whole seconds for the wait, rounded UP; the millisecond bound is judged below.
    wait_until dead_launch_done "$what, within the ${bound_ms} ms bound" "-" "${workdir}/dead.log" \
        $(( (bound_ms + 999) / 1000 ))
    dead_status=0
    wait "$dead_launch_pid" || dead_status=$?
    dead_launch_pid=""
    dead_elapsed_ms=$(( dead_ended_ms - started ))
}

# perl and its two modules are REQUIRED, not optional: a missing one FAILS the run, naming what
# is missing. `IO::Socket::INET` and `Time::HiRes` ship with perl itself, so their absence is a
# broken environment rather than a platform without them, and a skip here printed a line nobody
# reads while the run still ended in `compile-cache E2E OK` -- a skip that read as a pass, over
# the three shapes this section exists for (review M-2). Every host this fixture runs on ships
# perl.
command -v perl >/dev/null 2>&1 \
    || fail "dead peers: no perl on PATH, so the never-accepting and reset peers and the elapsed bounds cannot run -- every host this fixture runs on ships perl"
for dead_module in IO::Socket::INET Time::HiRes; do
    perl -M"$dead_module" -e1 >/dev/null 2>&1 || fail "dead peers: perl cannot load ${dead_module}, which ships with perl itself -- a broken perl installation, not a platform without it" # perl-lifetime: a foreground availability probe; it exits at once and is never backgrounded
done
echo "== dead peers: a cache that cannot answer leaves the compile to go on =="
dead="${workdir}/deadproj"
mkdir -p "${dead}/build"
echo 'char const* tag() { return "dead-peer"; } int main() { return 0; }' > "${dead}/d.cpp"
export FASTCACHE_SOURCE_DIR="$dead" FASTCACHE_BINARY_DIR="${dead}/build"

dead_compile "127.0.0.1:${port}" $(( 300 * 1000 )) "the baseline compile against the live daemon"
[[ "$dead_status" -eq 0 && -f "${dead}/build/d.o" ]] \
    || { cat "${workdir}/dead.log" >&2; fail "dead peers: the live baseline did not compile (exit ${dead_status})"; }
cp "${dead}/build/d.o" "${workdir}/dead-expected.o"
dead_baseline_ms="$dead_elapsed_ms"
echo "   baseline ${dead_baseline_ms} ms against the live daemon"

# fd 3, so nothing the body runs can read the table as its stdin.
while read -r shape dead_cost_name <&3; do
    dead_cost_ms="${!dead_cost_name}"
    dead_bound_ms=$(( dead_baseline_ms + dead_exchanges * dead_cost_ms + dead_cost_ms / 2 ))
    echo "   ${shape}: bound ${dead_bound_ms} ms = baseline + ${dead_exchanges} x ${dead_cost_ms} + ${dead_cost_ms} / 2 ms"
    rm -f "${workdir}/dead-peer.port" "${workdir}/dead-peer.port.accepts"
    case "$shape" in
        refused)
            addr="127.0.0.1:$(free_port)" ;;
        accept-then-reset|never-accepting)
            mode=reset; [[ "$shape" = never-accepting ]] && mode=silent
            dead_peer "$mode" "${workdir}/dead-peer.port" $(( 2 * ((dead_bound_ms + 999) / 1000) )) &
            dead_peer_pid=$!
            wait_until dead_peer_ready "the ${shape} peer to listen" "$dead_peer_pid" - 10
            addr="127.0.0.1:$(cat "${workdir}/dead-peer.port")" ;;
        *)
            fail "dead peers: no peer for the shape '${shape}'" ;;
    esac
    dead_compile "$addr" "$dead_bound_ms" "the launcher against the ${shape} peer to finish"
    if [[ -n "$dead_peer_pid" ]]; then
        # KILL, never TERM: it covers the window the `trap -` inside `dead_peer` cannot -- a
        # subshell signalled between its fork and its first command still holds the fixture's
        # EXIT trap, and a catchable signal would run the run's cleanup from inside it (#1084).
        # KILL runs no trap, whichever side of that line the peer is on.
        kill -KILL "$dead_peer_pid" 2>/dev/null || true
        wait "$dead_peer_pid" 2>/dev/null || true
        dead_peer_pid=""
    fi
    [[ "$dead_status" -eq 0 ]] \
        || { cat "${workdir}/dead.log" >&2; fail "dead peers: ${shape}: the compile exited ${dead_status}"; }
    [[ "$dead_elapsed_ms" -le "$dead_bound_ms" ]] \
        || { cat "${workdir}/dead.log" >&2; fail "dead peers: ${shape}: ${dead_elapsed_ms} ms, over the ${dead_bound_ms} ms bound"; }
    # What tells this leg from one that never reached the peer: the launcher says it fell back.
    grep -qF "(fetch exchange failed)" "${workdir}/dead.log" \
        || { cat "${workdir}/dead.log" >&2; fail "dead peers: ${shape}: no 'fetch exchange failed' fall-back, so the peer was never asked"; }
    # And on the reset leg, where every exchange is a connection the peer accepts, the COUNT:
    # exactly `dead_exchanges`. Its exchanges cost nothing, so no time bound can see one too
    # many -- a launcher that retried a failed fetch once passes every bound and fails this.
    if [[ "$shape" = accept-then-reset ]]; then
        # No file is no accept at all -- a count of 0, not a fixture fault.
        dead_accepts=0
        [[ ! -e "${workdir}/dead-peer.port.accepts" ]] \
            || dead_accepts="$(count_lines "${workdir}/dead-peer.port.accepts")"
        [[ "$dead_accepts" -eq "$dead_exchanges" ]] \
            || { cat "${workdir}/dead.log" >&2; fail "dead peers: ${shape}: the peer accepted ${dead_accepts} connection(s), want exactly ${dead_exchanges} -- one per exchange"; }
    fi
    cmp -s "${workdir}/dead-expected.o" "${dead}/build/d.o" \
        || fail "dead peers: ${shape}: no object, or not the one the live baseline compiled"
    echo "   ${shape}: compiled locally in ${dead_elapsed_ms} ms (bound ${dead_bound_ms}), object matches the baseline${dead_accepts:+, ${dead_accepts} exchange(s) counted}"
    dead_accepts=""
done 3<<< "$dead_shapes"

# --- 9: forms the launcher must decline to cache ----------------------------
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

# --- 9: a root spelled differently from what the driver emits ----------------
# Every root test in the launcher is a string prefix comparison, so a root whose
# spelling differs from the one the compiler echoes back matches NOTHING it emits.
# Three mechanisms then fail at once and hide each other: the keyed dependency set
# is empty (a moved header keys identically), the replay guard classifies every
# path as toolchain and probes none of them, and the stored value keeps this
# machine's absolute paths. The launcher reports ordinary hits throughout
# (issue #66).
#
# A symlinked directory is the portable stand-in for the 8.3 short name measured
# on a Windows runner: one directory, two spellings, and a compiler that echoes
# whichever one it was handed.
#
# Two distinct properties are asserted here, and the second is easy to break while
# fixing the first:
#
#   a) The two spellings must key TOGETHER, so a compile driven through the link
#      shares its entry with one driven through the real path.
#   b) A replayed depfile must keep the spelling THIS BUILD uses. Its rule target
#      has to be byte-identical to the `-o` path the build passed, or Ninja fails
#      outright ("expected depfile ... to mention ...") and make matches no rule at
#      all and silently drops every header dependency. Reconciling the ROOTS to
#      their resolved form satisfies (a) and breaks (b); reconciling the emitted
#      paths INTO the build's own spelling satisfies both.
#
# Direct mode is off because this is about the PREPROCESSED key: `KeyDependencySet`
# and the "dependency set: N of M reported path(s) keyed" line only exist on that
# path, and a direct hit would reach the object without ever computing them.
echo "== an aliased root must canonicalize, and must not respell the depfile =="
aliasroot="${workdir}/aliasroot"
mkdir -p "${aliasroot}/real/src/inc" "${aliasroot}/real/build"
cat > "${aliasroot}/real/src/inc/h1.h" <<'HDR'
#pragma once
inline int h1() { return 9; }
HDR
cat > "${aliasroot}/real/src/a.cpp" <<'SRC'
#include "inc/h1.h"
char const* alias_marker = "aliased-root-case";
int main() { return h1(); }
SRC
if ! ln -s "${aliasroot}/real" "${aliasroot}/link" 2>/dev/null; then
    echo "   this filesystem does not support symlinks; nothing to compare"
else
    # The roots and the OUTPUT paths are the link spelling; the source and include
    # paths are the real one. That split is the whole point, and it mimics what
    # `cl` does: a build system spells everything one way, and the compiler reports
    # its dependencies resolved through the filesystem the other way. Spelling the
    # output paths the same way as the roots is not a convenience — it is what a
    # real build does, since `-o` and FASTCACHE_BINARY_DIR come from one generator,
    # and it is what makes property (b) below testable at all.
    linkobj="${aliasroot}/link/build/alias.o"
    linkdep="${aliasroot}/link/build/alias.d"
    realsrc="${aliasroot}/real/src/a.cpp"
    realinc="${aliasroot}/real/src/inc"
    export FASTCACHE_SOURCE_DIR="${aliasroot}/link/src" FASTCACHE_BINARY_DIR="${aliasroot}/link/build"

    run_alias_leg() {
        local log="$1" src="$2" inc="$3" obj="$4" dep="$5"
        # -MP as well as -MD: it emits a phony `header:` rule per dependency, whose
        # TARGET is a path the compiler reported rather than one the build system
        # named. Those must be reconciled like any other dependency, so the legs
        # below can tell a rule keyed on position from one keyed on value.
        FASTCACHE_NO_DIRECT=1 "$launcher" "$compiler" -std=c++23 -MD -MP -MF "$dep" \
            -I"$inc" -c "$src" -o "$obj" 2> "${workdir}/${log}" \
            || { cat "${workdir}/${log}" >&2; fail "aliased-root: compile (${log}) returned non-zero"; }
        cat "${workdir}/${log}"
    }

    # Every phony rule target (`<path>:` with nothing after it) must name a file
    # that exists here. A reconciler that exempted them by position would leave the
    # producing machine's spelling in the stored value, and a consumer's replayed
    # depfile would point -MP's deleted-header protection at paths it cannot stat.
    require_phony_targets_resolve() {
        local label="$1" depfile="$2" line target found=0
        while IFS= read -r line; do
            case "$line" in
                *:) target="${line%:}" ;;
                *) continue ;;
            esac
            [[ -n "$target" ]] || continue
            found=$((found + 1))
            [[ -e "$target" ]] \
                || fail "${label}: a phony depfile rule names a file that does not exist: ${target}"
        done < "$depfile"
        [[ "$found" -gt 0 ]] || fail "${label}: expected -MP phony rules in the depfile, found none"
    }

    # Leg A — populate through the aliased spelling. This is the leg that fails
    # before the fix, and it fails silently.
    run_alias_leg "alias-a.log" "$realsrc" "$realinc" "$linkobj" "$linkdep"
    grep -q "fastcache-cc: MISS" "${workdir}/alias-a.log" \
        || fail "aliased-root: first compile was not a MISS"

    # The signature of the bug, and it is a distinct failure from a driver that
    # reports nothing: "0 of M" means every reported path was filtered out.
    if grep -qE "dependency set: 0 of [1-9]" "${workdir}/alias-a.log"; then
        fail "aliased-root: the dependency set is empty; the root did not reconcile (issue #66)"
    fi
    grep -qE "dependency set: [1-9][0-9]* of [1-9]" "${workdir}/alias-a.log" \
        || fail "aliased-root: no dependency path reached the key"

    # The depfile the COMPILER wrote, for comparison with the one the cache
    # reproduces. Its rule target is what the build system will look for.
    compiled_target="$(head -n 1 "$linkdep" | sed 's/:.*//')"
    [[ -n "$compiled_target" ]] || fail "aliased-root: the compile wrote no depfile rule"

    # Leg B — the same compile again, so the cache has to reproduce both artifacts.
    rm -f "$linkobj" "$linkdep"
    run_alias_leg "alias-b.log" "$realsrc" "$realinc" "$linkobj" "$linkdep"
    grep -q "fastcache-cc: HIT" "${workdir}/alias-b.log" \
        || fail "aliased-root: the repeated compile was not a HIT"
    [[ -f "$linkdep" ]] || fail "aliased-root: the hit reproduced no depfile"

    replayed_target="$(head -n 1 "$linkdep" | sed 's/:.*//')"
    if [[ "$replayed_target" != "$compiled_target" ]]; then
        fail "aliased-root: the replayed depfile respelled its rule target;" \
             "the build asked for '${compiled_target}' and the cache produced '${replayed_target}'" \
             "-- Ninja rejects this outright and make silently drops every header dependency"
    fi
    require_depfile_resolves "aliased-root" "$linkdep"

    # Leg C — through the REAL spelling, with roots to match: same content, same
    # relative layout, so it must reach leg A's entry. Before the fix the two
    # spellings keyed apart and this MISSed.
    rm -f "${aliasroot}/real/build/alias.o"
    export FASTCACHE_SOURCE_DIR="${aliasroot}/real/src" FASTCACHE_BINARY_DIR="${aliasroot}/real/build"
    run_alias_leg "alias-c.log" "$realsrc" "$realinc" \
        "${aliasroot}/real/build/alias.o" "${aliasroot}/real/build/alias.d"
    # Anchored: the launcher prints "STALE HIT (...); recompiling" on its way to a
    # MISS, so a bare `grep HIT` is satisfied by exactly the collapse this case
    # exists to reject -- the same trap the moved-header case records above.
    grep -q "fastcache-cc: HIT" "${workdir}/alias-c.log" \
        || fail "aliased-root: the two spellings of one tree did not share a cache entry"
    [[ -f "${aliasroot}/real/build/alias.o" ]] || fail "aliased-root: the hit produced no object"
    echo "   two spellings share a key, and the replayed depfile kept the build's own: OK"

    # Leg D — the same tree with roots that carry a TRAILING SEPARATOR, which a
    # build system is free to export and `PathCanon::Layout` does not accept:
    # IsSegmentPrefix wants a separator AFTER the root, so `/x/build/` matches
    # nothing under `/x/build`. Untrimmed, the path gets a second chance through
    # the resolved root (which comes back without the separator) and JoinLocalized
    # then adds one of its own, so the replayed rule target reads `/x/build//a.o`
    # -- rejected by Ninja, matched against no rule by make.
    rm -f "$linkobj" "$linkdep"
    export FASTCACHE_SOURCE_DIR="${aliasroot}/link/src/" FASTCACHE_BINARY_DIR="${aliasroot}/link/build/"
    run_alias_leg "alias-d.log" "$realsrc" "$realinc" "$linkobj" "$linkdep"
    # Asserted BEFORE the target comparison, and not merely for completeness: a
    # trimmed root keys identically to leg A, so this must HIT. If it ever misses,
    # the real compiler writes the depfile and the comparison below passes without
    # the cache having reproduced anything at all.
    grep -q "fastcache-cc: HIT" "${workdir}/alias-d.log" \
        || fail "aliased-root: a trailing separator on a root changed the key"
    trailing_target="$(head -n 1 "$linkdep" | sed 's/:.*//')"
    if [[ "$trailing_target" != "$compiled_target" ]]; then
        fail "aliased-root: a trailing separator on a root respelled the depfile rule target;" \
             "the build asked for '${compiled_target}' and the cache produced '${trailing_target}'"
    fi
    require_depfile_resolves "aliased-root (trailing separator)" "$linkdep"
    echo "   a trailing separator on a root left the depfile alone: OK"

    # Leg E — the OUTPUT paths spelled differently from the roots, which is the
    # one shape where the depfile's rule target and its dependencies have
    # different authors: the target is the `-o` path the build system named, the
    # dependencies are what the compiler reported. Reconciliation must translate
    # the second and not the first. Every other leg deliberately spells the
    # outputs the same way as the roots (a real build does), which makes this the
    # only leg that can catch a reconciler reaching into the target.
    realobj="${aliasroot}/real/build/alias-e.o"
    realdep="${aliasroot}/real/build/alias-e.d"
    rm -f "$realobj" "$realdep"
    export FASTCACHE_SOURCE_DIR="${aliasroot}/link/src" FASTCACHE_BINARY_DIR="${aliasroot}/link/build"
    run_alias_leg "alias-e1.log" "$realsrc" "$realinc" "$realobj" "$realdep"
    e_compiled_target="$(head -n 1 "$realdep" | sed 's/:.*//')"
    [[ -n "$e_compiled_target" ]] || fail "aliased-root: leg E compile wrote no depfile rule"

    rm -f "$realobj" "$realdep"
    run_alias_leg "alias-e2.log" "$realsrc" "$realinc" "$realobj" "$realdep"
    grep -q "fastcache-cc: HIT" "${workdir}/alias-e2.log" \
        || fail "aliased-root: leg E did not reach its own entry"
    e_replayed_target="$(head -n 1 "$realdep" | sed 's/:.*//')"
    if [[ "$e_replayed_target" != "$e_compiled_target" ]]; then
        fail "aliased-root: an output path spelled unlike the roots was respelled in the replayed depfile;" \
             "the build asked for '${e_compiled_target}' and the cache produced '${e_replayed_target}'"
    fi
    require_depfile_resolves "aliased-root (output outside the build root spelling)" "$realdep"
    require_phony_targets_resolve "aliased-root (phony rules)" "$realdep"
    echo "   an output spelled unlike the roots kept its own rule target: OK"
    echo "   -MP phony rule targets were reconciled like dependencies: OK"

    # Leg F — DIRECT MODE, which every leg above turns off because it reaches the
    # object without computing the dependency set they assert on. It has to be
    # covered here too: direct mode is on by default, and its manifest names the
    # translation unit by a token derived from the source path. If the reconciled
    # spelling reaches the manifest KEY but the raw one reaches the manifest
    # BUILDER, the builder refuses (the source lies under neither root as written)
    # and direct mode is permanently, silently dead on exactly the hosts this
    # section exists for -- reported only under FASTCACHE_VERBOSE, which a build
    # does not set.
    directsrc="${aliasroot}/real/src/a.cpp"
    directobj="${aliasroot}/link/build/direct.o"
    directdep="${aliasroot}/link/build/direct.d"
    rm -f "$directobj" "$directdep"
    export FASTCACHE_SOURCE_DIR="${aliasroot}/link/src" FASTCACHE_BINARY_DIR="${aliasroot}/link/build"

    run_direct_leg() {
        local log="$1"
        "$launcher" "$compiler" -std=c++23 -MD -MP -MF "$directdep" \
            -I"$realinc" -c "$directsrc" -o "$directobj" 2> "${workdir}/${log}" \
            || { cat "${workdir}/${log}" >&2; fail "aliased-root: direct-mode compile (${log}) returned non-zero"; }
        cat "${workdir}/${log}"
    }

    run_direct_leg "alias-f1.log"
    grep -q "fastcache-cc: MANIFEST stored" "${workdir}/alias-f1.log" \
        || fail "aliased-root: no manifest was recorded, so direct mode is dead on an aliased root"

    rm -f "$directobj" "$directdep"
    run_direct_leg "alias-f2.log"
    grep -q "fastcache-cc: HIT" "${workdir}/alias-f2.log" \
        || fail "aliased-root: the manifest recorded on an aliased root did not serve the next compile"
    [[ -f "$directobj" ]] || fail "aliased-root: the direct hit produced no object"
    echo "   direct mode records and serves a manifest on an aliased root: OK"
fi

# --- 10: an object naming its checkout is not served into another -----------
# A translation unit that bakes its own path into program data -- `__FILE__`, or the
# builtin `std::source_location` is made of -- must not be served into a second
# checkout, in either direct mode, while one that bakes none still is; the checkout
# that stored a root-bound copy must still hit it; and a hit FASTCACHE_VERIFY rejects
# is logged VERIFY-MISMATCH, never HIT. No key can see those paths: the direct-mode
# key never sees the `__FILE__` expansion and `source_location` reaches no key at
# all, so the launcher reads the OBJECT (apps/fastcache-cc/RootBinding.hpp). Before
# that, `__FILE__` with direct mode on and `source_location` in both modes printed
# the first checkout's path from the second.
#
# The outcome of the second checkout is per kind, and that is the discrimination:
# `srcloc` shares a portable key with the first (the builtin reaches no key), so it
# meets the first checkout's marker and is a BOUND-MISS; `file` does not share one on
# the preprocessed path, so once direct mode declines it is an ordinary MISS; `none`
# is a HIT of the first checkout's very bytes.
# The tree includes a PROJECT header, and that is load-bearing: a translation unit
# reporting no dependency records no direct-mode manifest, so a "direct on" leg would
# never reach direct mode -- where `__FILE__` was mis-served -- and would pass with the
# fix removed. check_root_bound asserts the manifest was stored.
write_bound_tree() {
    local root="$1" kind="$2" tag="$3" body
    mkdir -p "${root}/src/inc" "${root}/build"
    printf '#pragma once\ninline int one() { return 1; }\n' > "${root}/src/inc/h1.hpp"
    case "$kind" in
        file)   body='char const* Where() { return __FILE__; }' ;;
        srcloc) body='char const* Where() { return __builtin_FILE(); }' ;;
        none)   body='char const* Where() { return "nowhere"; }' ;;
        *)      fail "unknown root-bound kind: ${kind}" ;;
    esac
    printf '#include "inc/h1.hpp"\nchar const* Tag() { return "%s"; }\n%s\nint G() { return one(); }\n' \
        "$tag" "$body" > "${root}/src/u.cpp"
}

# Compile one tree through the launcher. The environment assignments come first and
# there is always at least one, so `"$@"` is never empty under `set -u`.
bound_compile() {
    local root="$1" log="$2" flag="$3"
    shift 3
    env "$@" FASTCACHE_SOURCE_DIR="$root" FASTCACHE_BINARY_DIR="${root}/build" \
        "$launcher" "$compiler" -std=c++20 ${flag:+"$flag"} -MD -MF "${root}/build/u.d" \
        -c "${root}/src/u.cpp" -o "${root}/build/u.o" 2> "$log"
}

# The trace line's word, with the root-binding qualifier the launcher adds.
bound_outcome() {
    if grep -q "fastcache-cc: VERIFY-MISMATCH key=" "$1"; then echo VERIFY-MISMATCH
    elif grep -qE "fastcache-cc: HIT key=[^ ]+ \(root-bound:" "$1"; then echo BOUND-HIT
    elif grep -qE "fastcache-cc: MISS key=[^ ]+ \(root-bound:" "$1"; then echo BOUND-MISS
    elif grep -q "fastcache-cc: HIT" "$1"; then echo HIT
    elif grep -q "fastcache-cc: MISS" "$1"; then echo MISS
    else echo UNKNOWN
    fi
}

check_root_bound() {
    local kind="$1" direct="$2" want_b="$3" dir a b no_direct oa ob oa2 want_a2
    dir="${workdir}/bound/${kind}-${direct}"
    a="${dir}/checkout-a"
    b="${dir}/checkout-b"
    write_bound_tree "$a" "$kind" "rootbound-${kind}-${direct}"
    write_bound_tree "$b" "$kind" "rootbound-${kind}-${direct}"
    no_direct="FASTCACHE_E2E_CASE=rootbound"
    [[ "$direct" == off ]] && no_direct="FASTCACHE_NO_DIRECT=1"

    bound_compile "$a" "${dir}/a.log" "" "$no_direct" || fail "root-bound ${kind}/${direct}: checkout a failed to compile"
    bound_compile "$b" "${dir}/b.log" "" "$no_direct" || fail "root-bound ${kind}/${direct}: checkout b failed to compile"
    cp "${a}/build/u.o" "${dir}/a.o"
    # And a again, from a deleted object: the checkout that stored a root-bound copy
    # must still be served it, or the fix bought correctness by giving up the cache
    # for these translation units altogether.
    rm -f "${a}/build/u.o"
    bound_compile "$a" "${dir}/a2.log" "" "$no_direct" || fail "root-bound ${kind}/${direct}: checkout a failed again"

    if [[ "$direct" == on ]] && ! grep -q "fastcache-cc: MANIFEST stored" "${dir}/a.log"; then
        cat "${dir}/a.log"
        fail "root-bound ${kind}/direct on: checkout a recorded no manifest, so the leg never reaches direct mode"
    fi
    oa="$(bound_outcome "${dir}/a.log")"
    ob="$(bound_outcome "${dir}/b.log")"
    oa2="$(bound_outcome "${dir}/a2.log")"
    want_a2=BOUND-HIT
    [[ "$kind" == none ]] && want_a2=HIT
    if [[ "$oa" != MISS || "$ob" != "$want_b" || "$oa2" != "$want_a2" ]]; then
        cat "${dir}/a.log" "${dir}/b.log" "${dir}/a2.log"
        fail "root-bound ${kind}/direct ${direct}: a=${oa} b=${ob} (want ${want_b}) a-again=${oa2} (want ${want_a2})"
    fi
    # And b must have READ that manifest: one a stored and b never validated would turn this
    # leg back into a direct-off one without a single outcome moving. For a bound object, b's
    # direct mode follows the manifest to a's marker and says so -- and with direct mode off
    # it never can.
    if [[ "$kind" != none ]]; then
        followed=no
        grep -q "the direct-mode object is root-bound and this checkout has no copy" "${dir}/b.log" && followed=yes
        want_followed=no
        [[ "$direct" == on ]] && want_followed=yes
        if [[ "$followed" != "$want_followed" ]]; then
            cat "${dir}/b.log"
            fail "root-bound ${kind}/direct ${direct}: checkout b followed a's manifest=${followed} (want ${want_followed})"
        fi
    fi
    if [[ "$kind" == none ]]; then
        cmp "${dir}/a.o" "${b}/build/u.o" \
            || fail "root-bound ${kind}/direct ${direct}: a path-free object was not served into the other checkout"
    elif grep -qaF "$a" "${b}/build/u.o"; then
        # `if` rather than `grep && fail`: under `set -e` a non-matching grep would
        # end the script on the SUCCESS path.
        fail "root-bound ${kind}/direct ${direct}: checkout b's object names checkout a"
    fi
    echo "   ${kind}, direct ${direct}: a=${oa} b=${ob} a-again=${oa2}"
}

echo "== root-bound objects =="
for direct in on off; do
    check_root_bound file "$direct" MISS
    check_root_bound srcloc "$direct" BOUND-MISS
    check_root_bound none "$direct" HIT
done

# A verified hit is logged as what the build USED. An ELF object keeps a strict byte
# comparison, so a path-free object served across checkouts verifies clean without
# debug info and is rejected with it (its DWARF names the source) -- both directions,
# which is what keeps this from passing under a launcher that logged every verified
# hit one way.
echo "== a verified cross-checkout hit is logged as what the build used =="
for debug in off on; do
    dir="${workdir}/bound/verify-${debug}"
    write_bound_tree "${dir}/checkout-a" none "rootbound-verify-${debug}"
    write_bound_tree "${dir}/checkout-b" none "rootbound-verify-${debug}"
    flag=""
    want=HIT
    if [[ "$debug" == on ]]; then
        flag="-g"
        want=VERIFY-MISMATCH
    fi
    bound_compile "${dir}/checkout-a" "${dir}/a.log" "$flag" FASTCACHE_NO_DIRECT=1 || fail "verify ${debug}: a failed"
    bound_compile "${dir}/checkout-b" "${dir}/b.log" "$flag" FASTCACHE_NO_DIRECT=1 FASTCACHE_VERIFY=1 \
        || fail "verify ${debug}: b failed"
    ob="$(bound_outcome "${dir}/b.log")"
    logged="$(tail -n 1 "$(e2e_launcher_state_log)" | e2e_launcher_log_field outcome)"
    wrong=no
    if grep -q "WRONG OBJECT served" "${dir}/b.log"; then wrong=yes; fi
    want_wrong=no
    [[ "$want" == VERIFY-MISMATCH ]] && want_wrong=yes
    if [[ "$ob" != "$want" || "$logged" != "$want" || "$wrong" != "$want_wrong" ]]; then
        cat "${dir}/b.log"
        fail "verify with debug info ${debug}: trace=${ob} logged=${logged} (want ${want}), WRONG OBJECT line=${wrong}"
    fi
    echo "   debug info ${debug}: logged ${logged}, WRONG OBJECT line=${wrong}"
done

# --- authentication ---------------------------------------------------------
# The compile-cache protocol was the only one in the tree that never checked
# SessionContext::CurrentAuth(), so a daemon started with --requirepass gated
# memcached and RESP while serving this port to anyone. Three properties, and
# the third is the one that would rot silently:
#
#   a) the wrong credential (or none) is REFUSED, so the gate is real;
#   b) the build still SUCCEEDS anyway, because a cache that cannot be reached
#      must never be load-bearing -- a gate that broke builds would be reverted
#      by the first person it bit;
#   c) the right credential still HITs, i.e. authenticating did not cost the
#      cache its function. A gate that refused everybody would pass (a) and (b).
echo "== authentication =="
authport="$(free_port)"
authdir="${workdir}/authproj"
mkdir -p "${authdir}/build"
cat > "${authdir}/a.cpp" <<'EOF'
#include <string>
int main() { return static_cast<int>(std::string{"authenticated"}.size()); }
EOF

"$fastcached" --bind=127.0.0.1 --port="$authport" --requirepass=e2e-s3cret --log-level=info \
    > "${workdir}/auth-daemon.log" 2>&1 &
auth_pid=$!
wait_for_port 127.0.0.1 "$authport" "$auth_pid" "authenticating daemon" "${workdir}/auth-daemon.log"
kill -0 "$auth_pid" 2>/dev/null || {
    cat "${workdir}/auth-daemon.log" >&2
    fail "something else is listening on port ${authport}: our authenticating daemon is gone"
}

(
    export FASTCACHE_ADDR="127.0.0.1:${authport}"
    export FASTCACHE_SOURCE_DIR="$authdir"
    export FASTCACHE_BINARY_DIR="${authdir}/build"

    # (a) + (b): no credential at all.
    unset FASTCACHE_TOKEN
    "$launcher" "$compiler" -c "${authdir}/a.cpp" -o "${authdir}/build/none.o" \
        > "${workdir}/auth-none.log" 2>&1 \
        || fail "a refused credential broke the build; the cache must never be load-bearing"
    grep -q "unauthenticated" "${workdir}/auth-none.log" \
        || { cat "${workdir}/auth-none.log" >&2; fail "an uncredentialed compile was not refused as unauthenticated"; }
    [[ -f "${authdir}/build/none.o" ]] || fail "no object produced when the cache refused the launcher"

    # (a): a WRONG credential must be refused too, and distinguishably. "failed"
    # and "required" are different operator problems -- a bad token versus no
    # token -- and a single message for both is a support ticket either way.
    FASTCACHE_TOKEN=not-the-secret "$launcher" "$compiler" -c "${authdir}/a.cpp" -o "${authdir}/build/wrong.o" \
        > "${workdir}/auth-wrong.log" 2>&1 \
        || fail "a wrong credential broke the build"
    grep -q "authentication failed" "${workdir}/auth-wrong.log" \
        || { cat "${workdir}/auth-wrong.log" >&2; fail "a wrong credential was not reported as a failed authentication"; }

    # (c): the right credential caches as usual -- miss, then hit.
    export FASTCACHE_TOKEN=e2e-s3cret
    "$launcher" "$compiler" -c "${authdir}/a.cpp" -o "${authdir}/build/ok.o" \
        > "${workdir}/auth-miss.log" 2>&1 \
        || fail "an authenticated compile failed"
    grep -q "MISS" "${workdir}/auth-miss.log" \
        || { cat "${workdir}/auth-miss.log" >&2; fail "the first authenticated compile was not a miss"; }

    rm -f "${authdir}/build/ok.o"
    "$launcher" "$compiler" -c "${authdir}/a.cpp" -o "${authdir}/build/ok.o" \
        > "${workdir}/auth-hit.log" 2>&1 \
        || fail "the repeat authenticated compile failed"
    # Anchored on the launcher's own prefix rather than a bare `grep HIT`: the
    # launcher prints "STALE HIT (...); recompiling" on its way to a MISS, so a
    # loose match is satisfied by exactly the collapse this case exists to reject.
    grep -q "fastcache-cc: HIT" "${workdir}/auth-hit.log" \
        || { cat "${workdir}/auth-hit.log" >&2; fail "the repeat authenticated compile did not HIT"; }
    [[ -f "${authdir}/build/ok.o" ]] || fail "no object reproduced on the authenticated hit"
) || exit 1

# Stopped and REQUIRED to go, within `dist-compile-e2e.sh`'s `daemon_stop_seconds`
# and for its reason: `fastcached` has nothing to drain, so it stops as soon as its
# loop is woken. A bare `kill; wait` here was unbounded.
stop_and_require_exit "$auth_pid" "the authenticating daemon" 5
auth_pid=""
echo "   a credential is required, refusals never break the build, and the right one still HITs"

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

# The positive control, BEFORE `-z` clears it; and after it, the caller's logs intact --
# `-z` being the call that deleted a developer's statistics on the Windows twin.
e2e_launcher_state_assert_used
"$launcher" -z >/dev/null || fail "-z returned non-zero"
"$launcher" --zero-stats >/dev/null || fail "--zero-stats returned non-zero"
[ ! -f "$(e2e_launcher_state_log)" ] || fail "-z left this run's state log in place ($(e2e_launcher_state_log)), so it cleared some other one"
e2e_launcher_state_assert_caller_untouched

echo "compile-cache E2E OK: miss/hit, byte-identical, >1 MiB values, store ceiling, cross-depth, nested roots," \
     "moved-header convergence (both layouts keyed apart), an edit re-keying, root-bound objects keyed apart," \
     "authentication (refused without a credential, cached with one), and safe fallback"
exit 0
