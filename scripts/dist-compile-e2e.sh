#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of distributed compilation (POSIX). Starts a fastcached with a
# dispatch listener, one or more fastcache-compile-node workers, and drives real
# compiles through fastcache-cc.
#
# The properties asserted here are the ones no unit test can reach, because each
# needs three real processes and a real compiler:
#
#   1. Byte-identical      — an object compiled on a WORKER equals the one this
#                            machine's compiler produces locally. This is the whole
#                            soundness claim of the feature in one assertion: every
#                            other property is worthless if this one does not hold,
#                            because a wrong object is stored under a key other
#                            machines then fetch.
#   2. Still a cache       — a dispatched compile STOREs, so the next identical
#                            compile is served from the cache and is NOT dispatched
#                            again. The client stores the worker's result, so a
#                            break here presents as a fleet that recompiles
#                            everything forever while looking healthy.
#   3. Fingerprint isolate — a worker whose toolchain fingerprint differs is NEVER
#                            dispatched to, even when it is the only worker
#                            registered. An over-loose match does not fail, it
#                            produces an object built by the wrong compiler under a
#                            key that claims otherwise.
#   4. Failover            — with every worker dead, the build still succeeds
#                            locally. Distribution must be incapable of failing a
#                            build; this is the property that lets it be switched on
#                            by default in a fleet where nodes come and go.
#   5. Role gate           — pointing a client's scheduler at the CACHE listener is
#                            refused with a typed reply and falls back to a local
#                            compile, rather than hanging or serving.
#   6. Concurrency         — more concurrent clients than the fleet has slots must
#                            still all succeed, with correct objects and no hang.
#                            See the case for why it does not assert WHICH ones were
#                            refused.
#   7. Language             — a `.c` source compiled by a C++ driver comes back
#                            compiled as C++, because that is what this driver does
#                            with it locally. The client states the language when it
#                            hands over preprocessed text, and taking that from the
#                            extension alone is wrong for exactly this shape — a
#                            wrong object rather than a failed one.
#   8. Graceful stop        — a worker asked to stop does, promptly, rather than
#                            waiting for a supervisor to escalate.
#
# Ports are allocated per run rather than fixed. This fixture needs four of them
# (cache, dispatch, and two workers), and four more fixed ports is four more ways
# for a CI runner shared with the other smoke tests to collide — a failure that
# reads as "distribution is broken" when it means "something else was listening".
#
# Usage:
#   dist-compile-e2e.sh --fastcached <path> --node <path> --launcher <path>
#                       [--compiler <cxx>]
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a runtime prerequisite
# was missing (skip).
set -euo pipefail

fastcached=""
node=""
launcher=""
compiler="${CXX:-c++}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) fastcached="$2"; shift 2 ;;
        --node)       node="$2";       shift 2 ;;
        --launcher)   launcher="$2";   shift 2 ;;
        --compiler)   compiler="$2";   shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

[[ -n "$fastcached" && -x "$fastcached" ]] || { echo "fastcached not found: '$fastcached'; skipping"; exit "$SKIP"; }
[[ -n "$node"       && -x "$node"       ]] || { echo "fastcache-compile-node not found: '$node'; skipping"; exit "$SKIP"; }
[[ -n "$launcher"   && -x "$launcher"   ]] || { echo "fastcache-cc not found: '$launcher'; skipping"; exit "$SKIP"; }
command -v "$compiler" >/dev/null 2>&1 || { echo "compiler not found: '$compiler'; skipping"; exit "$SKIP"; }

workdir="$(mktemp -d)"
pids=()
cleanup() {
    # Every spawned process, not just the ones a happy path reaps: a `fail`
    # anywhere exits the script, and a daemon or worker left holding a port makes
    # the NEXT run fail at startup for a reason unrelated to what actually broke.
    #
    # SIGTERM first, then SIGKILL after a grace period -- never a bare `wait`.
    # These processes handle SIGTERM, so a bug that stops one from finishing its
    # shutdown would hang cleanup forever, and cleanup runs on EVERY exit path
    # including the failing ones. That turns "one assertion failed" into "the
    # suite timed out with no output", which is how this fixture's own first
    # version reported a real worker-shutdown bug: ***Timeout 900.10 sec, in
    # three CI jobs, naming nothing.
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

fail() { echo "dist-compile E2E FAILED: $*" >&2; exit 1; }

# Statistics are per-user state; keep this run out of the developer's real log.
export XDG_STATE_HOME="${workdir}/state"
export FASTCACHE_VERBOSE=1

# --- helpers ----------------------------------------------------------------

# Find a port nothing is listening on.
#
# A connect probe, not a bind probe: bind-then-close leaves the port in TIME_WAIT
# on some systems, and the caller is about to hand it to a *different* process
# anyway, so the only question this can honestly answer is "is anything answering
# here right now". Racy in principle; the test is RUN_SERIAL and the range is
# wide, and the alternative — four hard-coded ports — races with every other
# smoke test in the suite rather than only with itself.
free_port() {
    local port
    for _ in $(seq 1 200); do
        port=$(( 20000 + RANDOM % 20000 ))
        if ! (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then
            echo "$port"
            return 0
        fi
    done
    fail "could not find a free port"
}

# GET one path off a worker's admin endpoint and echo the whole response.
#
# `/dev/tcp` rather than curl, because a fixture that skips when curl is absent
# tests nothing on the machine that lacks it, and this needs no more than one
# request. Every read is bounded with `read -t`: the endpoint closes the
# connection itself (`Connection: close`), so a healthy server ends the loop on
# its own -- and a WEDGED one, which is exactly the state this probe exists to
# detect, would otherwise hang the suite instead of failing it.
# @param 1 port
# @param 2 path
http_get() {
    local port="$1" path="$2" line="" body=""
    exec 3<>"/dev/tcp/127.0.0.1/${port}" || return 1
    printf 'GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' "$path" >&3
    while IFS= read -r -t 5 line <&3; do body+="${line}"$'\n'; done
    exec 3<&-
    printf '%s' "$body"
}

# Block until something answers on a port, or the process behind it dies.
#
# Waiting on the listener rather than sleeping a fixed amount: a cold CI runner
# takes noticeably longer to get there than a warm developer machine, and a fixed
# sleep is either flaky or slow.
wait_for_port() {
    local port="$1" pid="$2" what="$3" logfile="$4"
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$logfile" >&2
            fail "${what} exited before it started listening"
        fi
        sleep 0.2
    done
    cat "$logfile" >&2
    fail "${what} never listened on port ${port}"
}

# Stop a process and require it to actually exit, within a bound.
#
# `kill` then a bare `wait` is the obvious spelling and it HANGS when the signal
# is handled but the process never finishes stopping -- which is a real failure
# mode and was a real bug: the worker installs a SIGTERM handler, and if its
# accept loop cannot be woken the handler sets a flag nobody comes back to read.
# A test that hangs reports less than a test that fails, so this bounds the wait
# and says what it was waiting for.
#
# @param 1 pid
# @param 2 what it is, for the message
# @param 3 seconds to allow
stop_and_require_exit() {
    local pid="$1" what="$2" seconds="$3"
    kill "$pid" >/dev/null 2>&1 || true
    local deadline=$(( seconds * 5 ))
    for _ in $(seq 1 "$deadline"); do
        kill -0 "$pid" 2>/dev/null || { wait "$pid" 2>/dev/null || true; return 0; }
        sleep 0.2
    done
    kill -9 "$pid" >/dev/null 2>&1 || true
    wait "$pid" 2>/dev/null || true
    fail "${what} did not exit within ${seconds}s of being asked to stop"
}

# Write a translation unit whose content is unique to the caller.
#
# Every case gets its own source text. Sharing one would let a case pass by
# hitting an entry an EARLIER case stored -- a spurious pass that survives the
# property under test being broken outright, which is exactly how a Windows
# cross-depth case passed while cross-depth sharing was not working at all.
write_source() {
    local path="$1" token="$2"
    cat > "$path" <<EOF
// unique: ${token}
#include <string>
#include <vector>

namespace ${token}
{
struct Widget
{
    std::string name;
    std::vector<int> values;
};

int Total(Widget const& w)
{
    int sum = 0;
    for (int v: w.values)
        sum += v;
    return sum + static_cast<int>(w.name.size());
}
} // namespace ${token}

int Entry_${token}()
{
    ${token}::Widget w { "${token}", { 1, 2, 3 } };
    return ${token}::Total(w);
}
EOF
}

# The toolchain fingerprint the launcher will send.
#
# Asked of the launcher rather than derived here. It used to be the first line of
# `<compiler> --version`, which a script could reproduce; it is now a digest over
# the compiler's whole include tree, and a fixture that recomputed it by hand would
# be asserting its own reimplementation rather than the launcher's.
#
# This matters more than it looks: if the fixture and the launcher disagreed, every
# case would degrade to a local compile and still exit 0 -- the fixture would pass
# while testing nothing at all. Case 1 catches that by requiring a DISPATCH, and
# this keeps it from arising.
#
# The workers below are given a bare --toolchain and compute the same digest
# themselves, so this value is used only to assert that all three agree.
fingerprint="$("$launcher" --print-toolchain-fingerprint "$compiler")" \
    || fail "the launcher could not compute a toolchain fingerprint"
[[ -n "$fingerprint" ]] || fail "the launcher reported an empty toolchain fingerprint"

# --- start the daemon --------------------------------------------------------
# Two listeners: the cache surface and the dispatch surface. They are separate
# because they have different trust postures -- the cache may reasonably be open
# on a build LAN, while the surface that makes a compiler RUN on another machine
# must be switched on deliberately and firewalled separately. `--bind` is
# deliberately not used: it is mutually exclusive with the --listen family.
cache_port="$(free_port)"
dispatch_port="$(free_port)"

"$fastcached" --listen="127.0.0.1:${cache_port}" --listen-dispatch="127.0.0.1:${dispatch_port}" \
    --storage-max-value=64M --log-level=info \
    > "${workdir}/daemon.log" 2>&1 &
daemon_pid=$!
pids+=("$daemon_pid")
wait_for_port "$cache_port" "$daemon_pid" "daemon" "${workdir}/daemon.log"
wait_for_port "$dispatch_port" "$daemon_pid" "daemon (dispatch listener)" "${workdir}/daemon.log"

grep -q "distributed execution enabled" "${workdir}/daemon.log" \
    || { cat "${workdir}/daemon.log" >&2; fail "daemon did not enable distributed execution"; }

export FASTCACHE_ADDR="127.0.0.1:${cache_port}"

# --- start a worker ----------------------------------------------------------
worker_port="$(free_port)"
worker_admin_port="$(free_port)"
"$node" --scheduler="127.0.0.1:${dispatch_port}" \
    --bind=127.0.0.1 --port="$worker_port" --advertise="127.0.0.1:${worker_port}" \
    --admin-listen="$worker_admin_port" \
    --toolchain="${compiler}" --slots=2 --log-level=debug \
    > "${workdir}/worker.log" 2>&1 &
worker_pid=$!
pids+=("$worker_pid")
wait_for_port "$worker_port" "$worker_pid" "worker" "${workdir}/worker.log"

# Registration is the worker's own outbound step and completes after it listens,
# so the port being up is not enough to start dispatching against.
# The worker computed its own fingerprint from a bare --toolchain. If it derived
# a different digest from the launcher's, everything below still "works": it
# registers, it heartbeats, and the scheduler simply never matches it -- so every
# case falls back to a local compile and exits 0. Asserting the agreement here is
# what keeps a fingerprint regression from presenting as a fixture that passes
# while testing nothing.
worker_fingerprint=""
for _ in $(seq 1 150); do
    worker_fingerprint="$(sed -n 's/.*serving .* as //p' "${workdir}/worker.log" | head -1)"
    [[ -n "$worker_fingerprint" ]] && break
    if ! kill -0 "$worker_pid" 2>/dev/null; then
        cat "${workdir}/worker.log" >&2
        fail "worker exited before reporting its toolchain fingerprint"
    fi
    sleep 0.2
done
[[ -n "$worker_fingerprint" ]] || { cat "${workdir}/worker.log" >&2; fail "worker never reported a toolchain fingerprint"; }
[[ "$worker_fingerprint" == "$fingerprint" ]] \
    || fail "worker and launcher disagree on the toolchain fingerprint: '${worker_fingerprint}' vs '${fingerprint}'"
echo "== toolchain fingerprint agreed by launcher and worker"

registered=""
for _ in $(seq 1 100); do
    if grep -q "registered" "${workdir}/worker.log"; then registered=1; break; fi
    if ! kill -0 "$worker_pid" 2>/dev/null; then
        cat "${workdir}/worker.log" >&2
        fail "worker exited before registering"
    fi
    sleep 0.2
done
[[ -n "$registered" ]] || { cat "${workdir}/worker.log" >&2; fail "worker never registered with the scheduler"; }

# --- the worker's own admin endpoint -----------------------------------------
#
# Asserted in a real process rather than only in a unit test, because what the
# unit tests cannot reach is whether the endpoint is actually SERVED by a worker
# that is simultaneously doing its job -- it runs on its own thread beside the
# accept loop, and "it constructs" and "it answers while the worker is busy" are
# different claims.
wait_for_port "$worker_admin_port" "$worker_pid" "worker admin endpoint" "${workdir}/worker.log"

health="$(http_get "$worker_admin_port" /healthz)" \
    || fail "worker admin endpoint refused a connection on ${worker_admin_port}"
[[ "$health" == *"200 OK"* ]] \
    || { printf '%s\n' "$health" >&2; fail "worker /healthz did not answer 200"; }

# A worker has no cache, so the cache series must be ABSENT rather than present
# and zero -- `fastcached_items 0` states an empty unbounded cache as a fact, and
# a dashboard reads it as one. This is the assertion that would fail if
# `MetricsSnapshot::storage` stopped being optional.
before="$(http_get "$worker_admin_port" /metrics)"     || fail "worker admin endpoint refused a /metrics request"
[[ "$before" == *"fastcache_worker_jobs_completed_total"* ]] \
    || { printf '%s\n' "$before" >&2; fail "worker /metrics carries no worker counters"; }
[[ "$before" == *"fastcache_node_logical_cores"* ]] \
    || { printf '%s\n' "$before" >&2; fail "worker /metrics does not report the machine's size"; }
[[ "$before" != *"fastcached_items"* ]] \
    || { printf '%s\n' "$before" >&2; fail "worker /metrics reports cache series for a process with no cache"; }
echo "== worker serves /healthz and /metrics"

# --- the project layout every case compiles in -------------------------------
proj="${workdir}/proj"
mkdir -p "${proj}/build"
export FASTCACHE_SOURCE_DIR="${proj}"
export FASTCACHE_BINARY_DIR="${proj}/build"

# Run one compile through the launcher, capturing its notes.
# @param 1 log file to write the launcher's own output to
# @param 2.. the compile command, launcher excluded
run_launcher() {
    local logfile="$1"; shift
    "$launcher" "$compiler" "$@" > "$logfile" 2>&1
}

# --- 1 + 2: a worker's object is this machine's object, and it caches ---------
echo "== case 1+2: byte-identical remote object, then a cache hit"
write_source "${proj}/one.cpp" "caseone"

# The reference: this machine's own compiler, no launcher, no cache.
"$compiler" -std=c++17 -O1 -c "${proj}/one.cpp" -o "${proj}/build/reference.o" \
    || fail "the reference compile failed"

export FASTCACHE_SCHEDULER="127.0.0.1:${dispatch_port}"

run_launcher "${workdir}/case1.log" -std=c++17 -O1 -c "${proj}/one.cpp" -o "${proj}/build/one.o" \
    || { cat "${workdir}/case1.log" >&2; fail "the dispatched compile failed"; }

grep -q "DISPATCHED to " "${workdir}/case1.log" \
    || {
        cat "${workdir}/case1.log" >&2
        # The worker's side as well. A refusal reaches the client as one line
        # naming a wire error code; WHY it happened is only visible on the worker.
        echo "--- worker log ---" >&2
        cat "${workdir}/worker.log" >&2
        fail "the compile was not dispatched to a worker"
    }

[[ -f "${proj}/build/one.o" ]] || fail "no object was written by the dispatched compile"
cmp -s "${proj}/build/reference.o" "${proj}/build/one.o" || {
    # How they differ, not just that they do: equal sizes with different bytes
    # means something environment-specific was embedded, which is a different
    # investigation from a size mismatch.
    echo "  reference: $(wc -c < "${proj}/build/reference.o") bytes" >&2
    echo "  produced:  $(wc -c < "${proj}/build/one.o") bytes" >&2
    cmp -l "${proj}/build/reference.o" "${proj}/build/one.o" 2>/dev/null | head -5 >&2
    fail "the worker's object differs from the locally compiled one"
}
echo "   byte-identical to the local object"

# The same compile again must come from the cache, not from a worker. A
# dispatched result is STORED by the client, so this is what proves the two
# halves are joined: distribution that never populates the cache would make a
# fleet recompile every translation unit forever while looking perfectly healthy.
rm -f "${proj}/build/one.o"
run_launcher "${workdir}/case2.log" -std=c++17 -O1 -c "${proj}/one.cpp" -o "${proj}/build/one.o" \
    || { cat "${workdir}/case2.log" >&2; fail "the second compile failed"; }
grep -q "fastcache-cc: HIT" "${workdir}/case2.log" \
    || { cat "${workdir}/case2.log" >&2; fail "a dispatched result was not served from the cache afterwards"; }
grep -q "DISPATCHED to " "${workdir}/case2.log" \
    && { cat "${workdir}/case2.log" >&2; fail "a cached compile was dispatched again"; }
cmp -s "${proj}/build/reference.o" "${proj}/build/one.o" \
    || fail "the cached object differs from the locally compiled one"
echo "   served from the cache on the second compile"

# The counters moved, which is the half no unit test can reach. A counter that is
# incremented in a unit test and by nothing on the real path is the defect the
# catalog exists to prevent, one layer up: it exports a permanent zero, which
# reads as "distribution is not happening" rather than as "nobody wired this".
after="$(http_get "$worker_admin_port" /metrics)"     || fail "worker admin endpoint stopped answering after serving a compile"
completed="$(sed -n 's/^fastcache_worker_jobs_completed_total \([0-9][0-9]*\)$/\1/p' <<< "$after" | tail -1)"
[[ -n "$completed" && "$completed" -ge 1 ]] \
    || { printf '%s\n' "$after" >&2; fail "worker completed a compile but its jobs counter did not move"; }
millis="$(sed -n 's/^fastcache_worker_compile_milliseconds_total \([0-9][0-9]*\)$/\1/p' <<< "$after" | tail -1)"
[[ -n "$millis" ]] \
    || { printf '%s\n' "$after" >&2; fail "worker reports no compile wall time"; }
bytes="$(sed -n 's/^fastcache_worker_bytes_received_total \([0-9][0-9]*\)$/\1/p' <<< "$after" | tail -1)"
[[ -n "$bytes" && "$bytes" -ge 1 ]] \
    || { printf '%s\n' "$after" >&2; fail "worker received a job but its byte counter did not move"; }
echo "   worker metrics moved: ${completed} job(s), ${millis}ms, ${bytes} bytes in"

# --- 3: a worker for a different toolchain is never chosen -------------------
echo "== case 3: fingerprint isolation"
# A second daemon and a second worker, so the mismatched worker is the ONLY one
# registered. Reusing the first scheduler would leave the matching worker
# available and the case would pass without testing anything.
iso_cache_port="$(free_port)"
iso_dispatch_port="$(free_port)"
"$fastcached" --listen="127.0.0.1:${iso_cache_port}" --listen-dispatch="127.0.0.1:${iso_dispatch_port}" \
    --log-level=info > "${workdir}/iso-daemon.log" 2>&1 &
iso_daemon_pid=$!
pids+=("$iso_daemon_pid")
wait_for_port "$iso_dispatch_port" "$iso_daemon_pid" "isolation daemon" "${workdir}/iso-daemon.log"

iso_worker_port="$(free_port)"
"$node" --scheduler="127.0.0.1:${iso_dispatch_port}" \
    --bind=127.0.0.1 --port="$iso_worker_port" --advertise="127.0.0.1:${iso_worker_port}" \
    --toolchain="not-the-compiler-this-client-uses=${compiler}" --slots=2 --log-level=debug \
    > "${workdir}/iso-worker.log" 2>&1 &
iso_worker_pid=$!
pids+=("$iso_worker_pid")
wait_for_port "$iso_worker_port" "$iso_worker_pid" "isolation worker" "${workdir}/iso-worker.log"
for _ in $(seq 1 100); do
    grep -q "registered" "${workdir}/iso-worker.log" && break
    sleep 0.2
done
grep -q "registered" "${workdir}/iso-worker.log" \
    || { cat "${workdir}/iso-worker.log" >&2; fail "the isolation worker never registered"; }

write_source "${proj}/three.cpp" "casethree"
"$compiler" -std=c++17 -O1 -c "${proj}/three.cpp" -o "${proj}/build/three-ref.o" \
    || fail "the case 3 reference compile failed"

(
    export FASTCACHE_ADDR="127.0.0.1:${iso_cache_port}"
    export FASTCACHE_SCHEDULER="127.0.0.1:${iso_dispatch_port}"
    run_launcher "${workdir}/case3.log" -std=c++17 -O1 -c "${proj}/three.cpp" -o "${proj}/build/three.o"
) || { cat "${workdir}/case3.log" >&2; fail "the compile failed when only a mismatched worker was registered"; }

grep -q "DISPATCHED to " "${workdir}/case3.log" \
    && { cat "${workdir}/case3.log" >&2; fail "a job was dispatched to a worker with a different toolchain"; }
grep -q "not dispatched (rejected (no-worker)" "${workdir}/case3.log" \
    || { cat "${workdir}/case3.log" >&2; fail "expected a no-worker refusal naming the missing toolchain"; }
cmp -s "${proj}/build/three-ref.o" "${proj}/build/three.o" \
    || fail "the locally compiled fallback object is wrong"
echo "   a mismatched worker was refused, and the build compiled locally"

# --- 4: with every worker dead the build still succeeds ----------------------
echo "== case 4: failover to a local compile"
stop_and_require_exit "$iso_worker_pid" "the isolation worker" 15

write_source "${proj}/four.cpp" "casefour"
"$compiler" -std=c++17 -O1 -c "${proj}/four.cpp" -o "${proj}/build/four-ref.o" \
    || fail "the case 4 reference compile failed"

(
    export FASTCACHE_ADDR="127.0.0.1:${iso_cache_port}"
    export FASTCACHE_SCHEDULER="127.0.0.1:${iso_dispatch_port}"
    run_launcher "${workdir}/case4.log" -std=c++17 -O1 -c "${proj}/four.cpp" -o "${proj}/build/four.o"
) || { cat "${workdir}/case4.log" >&2; fail "the build did not survive every worker being dead"; }

# Either refusal is correct and which one fires is a race with heartbeat expiry:
# the scheduler may still believe the worker is alive and lease it (the client
# then finds it unreachable), or may already have expired it (no-worker). Both
# end at a local compile, which is the property; asserting one of the two would
# be asserting the timing.
grep -qE "not dispatched \((rejected \(no-worker\)|worker .* unreachable)" "${workdir}/case4.log" \
    || { cat "${workdir}/case4.log" >&2; fail "expected a refusal naming the unavailable worker"; }
cmp -s "${proj}/build/four-ref.o" "${proj}/build/four.o" \
    || fail "the failover object is wrong"
echo "   the build succeeded locally with no worker alive"

# --- 5: the cache listener does not serve dispatch ---------------------------
echo "== case 5: dispatch refused on a cache-only listener"
write_source "${proj}/five.cpp" "casefive"
"$compiler" -std=c++17 -O1 -c "${proj}/five.cpp" -o "${proj}/build/five-ref.o" \
    || fail "the case 5 reference compile failed"

(
    # The scheduler address deliberately points at the CACHE port. An operator
    # who mixes these up must get a typed refusal and a working build, not a hang
    # and not a served job.
    export FASTCACHE_SCHEDULER="127.0.0.1:${cache_port}"
    run_launcher "${workdir}/case5.log" -std=c++17 -O1 -c "${proj}/five.cpp" -o "${proj}/build/five.o"
) || { cat "${workdir}/case5.log" >&2; fail "a build pointed at the wrong listener did not survive"; }

grep -q "not dispatched (rejected (dispatch-not-permitted)" "${workdir}/case5.log" \
    || { cat "${workdir}/case5.log" >&2; fail "expected a dispatch-not-permitted refusal from the cache listener"; }
cmp -s "${proj}/build/five-ref.o" "${proj}/build/five.o" \
    || fail "the object built after a role refusal is wrong"
echo "   the cache listener refused dispatch, and the build compiled locally"

# --- 6: more concurrent clients than slots ------------------------------------
echo "== case 6: concurrency beyond the fleet's slot count"
# A single worker with ONE slot, and four clients at once.
#
# What this asserts is that every client succeeds with a correct object and the
# run terminates. It deliberately does NOT assert which clients were refused: with
# one slot and four clients, whether a given client sees NoCapacity depends on
# whether the others' jobs happen to overlap it, and a fixture that asserts a race
# is a fixture that fails for reasons unrelated to the code. The capacity RULE --
# a worker at its cap is not picked -- is pinned deterministically in
# WorkerRegistry_test against a ManualClock; what needs three processes to observe
# is that pressure produces neither a hang nor a wrong object.
cap_cache_port="$(free_port)"
cap_dispatch_port="$(free_port)"
"$fastcached" --listen="127.0.0.1:${cap_cache_port}" --listen-dispatch="127.0.0.1:${cap_dispatch_port}" \
    --log-level=info > "${workdir}/cap-daemon.log" 2>&1 &
cap_daemon_pid=$!
pids+=("$cap_daemon_pid")
wait_for_port "$cap_dispatch_port" "$cap_daemon_pid" "capacity daemon" "${workdir}/cap-daemon.log"

cap_worker_port="$(free_port)"
"$node" --scheduler="127.0.0.1:${cap_dispatch_port}" \
    --bind=127.0.0.1 --port="$cap_worker_port" --advertise="127.0.0.1:${cap_worker_port}" \
    --toolchain="${compiler}" --slots=1 --log-level=debug \
    > "${workdir}/cap-worker.log" 2>&1 &
cap_worker_pid=$!
pids+=("$cap_worker_pid")
wait_for_port "$cap_worker_port" "$cap_worker_pid" "capacity worker" "${workdir}/cap-worker.log"
for _ in $(seq 1 100); do
    grep -q "registered" "${workdir}/cap-worker.log" && break
    sleep 0.2
done
grep -q "registered" "${workdir}/cap-worker.log" \
    || { cat "${workdir}/cap-worker.log" >&2; fail "the capacity worker never registered"; }

cap_pids=()
for i in 1 2 3 4; do
    write_source "${proj}/cap${i}.cpp" "casecap${i}"
    "$compiler" -std=c++17 -O1 -c "${proj}/cap${i}.cpp" -o "${proj}/build/cap${i}-ref.o" \
        || fail "the case 6 reference compile ${i} failed"
done
for i in 1 2 3 4; do
    (
        export FASTCACHE_ADDR="127.0.0.1:${cap_cache_port}"
        export FASTCACHE_SCHEDULER="127.0.0.1:${cap_dispatch_port}"
        run_launcher "${workdir}/case6-${i}.log" -std=c++17 -O1 -c "${proj}/cap${i}.cpp" -o "${proj}/build/cap${i}.o"
    ) &
    cap_pids+=($!)
done
for i in 1 2 3 4; do
    if ! wait "${cap_pids[$((i - 1))]}"; then
        cat "${workdir}/case6-${i}.log" >&2
        fail "concurrent compile ${i} failed under capacity pressure"
    fi
done
for i in 1 2 3 4; do
    cmp -s "${proj}/build/cap${i}-ref.o" "${proj}/build/cap${i}.o" \
        || fail "concurrent compile ${i} produced a wrong object"
done
dispatched_count=0
for i in 1 2 3 4; do
    grep -q "DISPATCHED to " "${workdir}/case6-${i}.log" && dispatched_count=$((dispatched_count + 1))
done
echo "   4 concurrent compiles all correct (${dispatched_count} dispatched, $((4 - dispatched_count)) local)"

# --- 7: the language is stated, and the driver has the last word on it -------
echo "== case 7: a .c source through a C++ driver is compiled as C++"
# The worker names its own scratch file and its compiler reads the language off
# that name, so the client has to say which language the text is -- and taking
# that from the source's EXTENSION alone is wrong for exactly this shape. "g++
# treats .c, .h and .i files as C++ source files instead of C source files", so a
# `.c` compiled by this project's C++ driver is C++, and telling a worker
# `-x cpp-output` would have it compile as C what this machine compiles as C++.
#
# That is a WRONG object rather than a failed one -- it is stored under the key
# and served to everybody -- which is why it is asserted end to end rather than
# left to the unit test that pins the same rule.
cat > "${proj}/seven.c" <<'EOF'
// unique: caseseven
struct Widget { int a; int b; };
static int Total(struct Widget const* w) { return w->a + w->b; }
int caseseven_entry(void)
{
    struct Widget w = { 2, 3 };
    return Total(&w);
}
EOF

"$compiler" -O1 -c "${proj}/seven.c" -o "${proj}/build/seven-ref.o"     || fail "the case 7 reference compile failed"

run_launcher "${workdir}/case7.log" -O1 -c "${proj}/seven.c" -o "${proj}/build/seven.o"     || { cat "${workdir}/case7.log" >&2; fail "the .c compile failed"; }
grep -q "DISPATCHED to " "${workdir}/case7.log"     || { cat "${workdir}/case7.log" >&2; fail "the .c compile was not dispatched"; }
cmp -s "${proj}/build/seven-ref.o" "${proj}/build/seven.o"     || {
        # C compiled as C++ differs in far more than a byte: this source has
        # external linkage, so the symbol names themselves are mangled.
        cmp -l "${proj}/build/seven-ref.o" "${proj}/build/seven.o" 2>/dev/null | head -5 >&2
        fail "a .c source did not come back compiled the way this driver compiles it"
    }
echo "   a .c source came back matching what this driver produces locally"

# --- 8: a worker stops when it is asked to --------------------------------------
echo "== case 8: a worker exits on SIGTERM"
# The property a supervisor depends on, and one that fails in the worst possible
# shape: the worker handles SIGTERM, so the signal no longer kills it outright --
# if the accept loop cannot then be woken, the process hangs and `systemctl stop`
# waits out its timeout before escalating to SIGKILL. It needs a real socket to
# catch, because the mechanism IS the socket: POSIX does not unblock a parked
# accept() when the listener is closed, so the loop only comes back through the
# SO_RCVTIMEO poll. macOS wakes the accept anyway and hides the whole thing, which
# is why this is asserted here and not left to a developer machine.
stop_port="$(free_port)"
"$node" --scheduler="127.0.0.1:${dispatch_port}" \
    --bind=127.0.0.1 --port="$stop_port" --advertise="127.0.0.1:${stop_port}" \
    --toolchain="${compiler}" --slots=1 --log-level=info \
    > "${workdir}/stop-worker.log" 2>&1 &
stop_worker_pid=$!
pids+=("$stop_worker_pid")
wait_for_port "$stop_port" "$stop_worker_pid" "shutdown worker" "${workdir}/stop-worker.log"

stop_and_require_exit "$stop_worker_pid" "the worker under test" 15

# Exiting is necessary but not sufficient: a worker that died of the signal also
# "exits". These lines are what distinguish a graceful stop from a death, and
# their absence would mean the handler never ran.
grep -q "stop requested; no longer accepting compiles" "${workdir}/stop-worker.log" \
    || { cat "${workdir}/stop-worker.log" >&2; fail "the worker did not report a graceful stop"; }
grep -q "compile node stopped" "${workdir}/stop-worker.log" \
    || { cat "${workdir}/stop-worker.log" >&2; fail "the worker did not run its shutdown path to completion"; }
echo "   the worker stopped gracefully on SIGTERM"

echo
echo "dist-compile E2E PASSED"
