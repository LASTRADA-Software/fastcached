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
#  12. Cache independence   — a cache the launcher cannot reach does not stop the
#                            compile being dispatched. The cache and the scheduler
#                            are separate services on separate machines, so a
#                            failure of one is not a failure of the other; joining
#                            them turned a mistyped FASTCACHE_ADDR into a
#                            fleet-wide outage behind an entirely green build.
#
# Cases 9 to 11 — the node's own cache tier, a worker that sizes itself, and a
# black-holed upstream — carry their reasoning at the case rather than here,
# because each is about a node's internals rather than about the client contract
# this list describes.
#
# Ports are allocated per run rather than fixed. This fixture needs four of them
# (cache, dispatch, and two workers), and four more fixed ports is four more ways
# for a CI runner shared with the other smoke tests to collide — a failure that
# reads as "distribution is broken" when it means "something else was listening".
#
# All twelve of those are loopback end to end, and `--case membership` is the one
# body of assertions that is not. Its own block below carries the whole argument;
# `.agent/rules/testing.md` carries the rule it came from.
#
# Usage:
#   dist-compile-e2e.sh --fastcached <path> --node <path> --launcher <path>
#                       [--compiler <cxx>] [--case suite|membership]
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a runtime prerequisite
# was missing (skip).
set -euo pipefail

fastcached=""
node=""
launcher=""
compiler="${CXX:-c++}"

# Which body of assertions to run. `suite` is the twelve cases above; `membership`
# is the pair that needs a non-loopback address and is registered as its own ctest
# test. See the block that runs it for why it is a MODE rather than a thirteenth
# case in the same run.
mode="suite"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) fastcached="$2"; shift 2 ;;
        --node)       node="$2";       shift 2 ;;
        --launcher)   launcher="$2";   shift 2 ;;
        --compiler)   compiler="$2";   shift 2 ;;
        --case)       mode="$2";       shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$mode" in
    suite|membership) ;;
    *) echo "unknown --case: ${mode} (expected 'suite' or 'membership')" >&2; exit 2 ;;
esac

readonly SKIP=77

[[ -n "$fastcached" && -x "$fastcached" ]] || { echo "fastcached not found: '$fastcached'; skipping"; exit "$SKIP"; }
[[ -n "$node"       && -x "$node"       ]] || { echo "fastcache-compile-node not found: '$node'; skipping"; exit "$SKIP"; }

# Every node below except the cache-tier case turns its own cache port OFF.
# `--listen-node` defaults to 127.0.0.1:6674 -- where `fastcache-cc` looks --
# which is right for the one node per machine a real deployment runs and wrong
# here, where several share a host and would race for it. Said explicitly rather
# than left to the default's warn-and-continue, so a node that failed to bind for
# some OTHER reason still shows up as the fault it is.
no_local_cache="--cache-memory=0"
[[ -n "$launcher"   && -x "$launcher"   ]] || { echo "fastcache-cc not found: '$launcher'; skipping"; exit "$SKIP"; }
command -v "$compiler" >/dev/null 2>&1 || { echo "compiler not found: '$compiler'; skipping"; exit "$SKIP"; }

workdir="$(mktemp -d)"

# The cluster key every node in this fixture shares.
#
# It is what makes the dispatch path here the SIGNED one. Without it the scheduler
# hands out bare serials and every worker runs `UncheckedLeaseValidator`, so a fixture
# that dispatches hundreds of compiles would exercise none of the lease check (#282) --
# and the two fixtures that DO carry a key, cluster-e2e and fleet-dashboard-e2e, never
# dispatch a compile, so between them they proved only that a keyed node starts.
#
# With it, every case below is a real client presenting a real signed grant to a real
# worker over a real socket, and the grant's MAC covers the endpoint that worker
# advertised. A worker advertising an address the scheduler did not grant fails every
# case rather than none, which is the property no in-process test can show: the unit
# tests mint and verify inside one process.
#
# Fixed text rather than /dev/urandom: nothing here turns on its value, and a per-run
# secret would make a failure look like a flake. Sixteen bytes is the minimum.
cluster_key="${workdir}/cluster.key"
printf 'e2e-fixture-cluster-key-not-a-secret\n' > "$cluster_key"
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
#
# Ports already handed out THIS RUN are remembered and skipped. Without that, the
# only question asked is "is anything listening", and nothing is listening on a
# port issued a moment ago whose server has not bound yet -- so two calls could
# return the same number and the second process to start died with
# `bind(...) failed: 98`. This fixture draws every port it needs before binding
# any of them, which is exactly the window that makes it reachable: rare enough
# to read as an unrelated flake, and it did.
#
# The ledger is a FILE rather than a variable because every call site is a command
# substitution, and a subshell's assignment is gone the moment it exits -- which
# is how a first attempt at this fixed nothing at all. It lives under `workdir`,
# so the existing cleanup takes it away.
#
# The range stops BELOW the kernel's ephemeral port range, which is the half a
# connect probe cannot cover. A port can be the local endpoint of an OUTBOUND
# connection -- ESTABLISHED or TIME_WAIT -- with nothing listening on it, so the
# probe says "free" and the `bind()` that follows still fails with EADDRINUSE.
# The ledger does not help either: that port was never issued by this fixture.
#
# CI caught exactly that: `bind(127.0.0.1:33174) failed: 98` for case 6's daemon,
# started after five cases' worth of launcher and probe connections had consumed
# ephemeral ports. 33174 is inside Linux's default `ip_local_port_range` of
# 32768-60999, which the old draw of 20000-39999 overlapped by its top 7232
# numbers -- worse than one draw in three.
#
# 20000-31999 is below that floor and below macOS's 49152, and 12000 numbers is
# ample for a fixture that draws a dozen. A machine that lowered the sysctl below
# 32000 would need this to move with it, and
# `cat /proc/sys/net/ipv4/ip_local_port_range` is the check.
free_port() {
    local port ledger="${workdir}/.issued-ports"
    local floor=20000 ceiling=32000
    for _ in $(seq 1 200); do
        port=$(( floor + RANDOM % (ceiling - floor) ))
        if grep -qx "$port" "$ledger" 2>/dev/null; then
            continue
        fi
        if ! (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then
            echo "$port" >> "$ledger"
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
#
# The host is a parameter with a loopback default rather than the constant it used
# to be, because the membership mode binds its workers on an address that is NOT
# loopback -- and probing 127.0.0.1 for a listener bound to 192.168.x.y reports
# "never listened" about a process that is listening perfectly well somewhere else.
# A default keeps every existing caller reading as it did.
#
# @param 1 port
# @param 2 pid
# @param 3 what it is, for the message
# @param 4 the log to dump when it does not come up
# @param 5 host to probe; defaults to 127.0.0.1
wait_for_port() {
    local port="$1" pid="$2" what="$3" logfile="$4" host="${5:-127.0.0.1}"
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/${host}/${port}") 2>/dev/null; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$logfile" >&2
            fail "${what} exited before it started listening"
        fi
        sleep 0.2
    done
    cat "$logfile" >&2
    fail "${what} never listened on ${host}:${port}"
}

# Wait for a line to appear in a log, the way `wait_for_port` waits for a listener.
#
# A bound port does NOT mean a process has finished announcing itself. Every tier
# here binds its listener and logs what it bound *afterwards*, because the message
# names the endpoint and the endpoint is not known until the bind returns. So a
# `grep` run straight after `wait_for_port` is a race -- and one that widens
# exactly where it is least welcome: under a sanitizer, or with the log on a slow
# filesystem, the gap between the two stops being instant. It reported "node did
# not start its scheduler" about a node that started its scheduler perfectly well
# a millisecond later, and it did so in roughly half of full-suite runs while
# passing every time the test was run alone.
#
# Liveness is checked as `wait_for_port` checks it, so a process that dies is
# reported as having died rather than as never having got round to it. Two of the
# three hand-written poll loops this replaces did not check that at all.
#
# @param 1 the text to wait for
# @param 2 pid
# @param 3 what it is, for the message
# @param 4 the log to watch
wait_for_log() {
    local marker="$1" pid="$2" what="$3" logfile="$4"
    for _ in $(seq 1 100); do
        if grep -q "$marker" "$logfile"; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$logfile" >&2
            fail "${what} exited before logging: ${marker}"
        fi
        sleep 0.2
    done
    cat "$logfile" >&2
    fail "${what} never logged: ${marker}"
}

# What every worker this fixture starts is told its drain may take, and what every
# wait for one to stop is bounded by. ONE number, which is the whole of #380.
#
# The bound was 15 s while the node's own default drain is 30 s, so the fixture's
# ceiling was HALF the process's configured maximum: a worker that drained for 20 s
# and then exited was behaving exactly as configured and this fixture called it a
# failure. It was also not the EXPECTED time -- a worker signalled with nothing in
# flight wakes its accept loop and stops in well under a second -- so 15 sat between
# the two numbers that mean something, catching neither case cleanly and deciding
# itself on how loaded the runner was. It ejected #378 from the merge queue on a base
# that had passed the same leg, and it failed #418, a change that removes a dead
# config field.
#
# So the fixture STATES the drain rather than inheriting it. Every worker is started
# with `--drain-timeout`, and the wait is that number plus a margin for the runner.
# The contradiction is gone because both halves now read the same variable, and the
# assertion is STRICTER than before rather than looser: ten seconds against a
# sub-second expectation, where the old bound allowed fifteen and disagreed with the
# process about what was legal.
#
# Raising the bound to 30 to stop this recurring is the one thing that must not
# happen: it widens the window in which a genuinely wedged worker still passes, which
# is exactly what `stop_and_require_exit` exists to catch.
worker_drain_seconds=5
stated_drain="--drain-timeout=${worker_drain_seconds}"
stop_bound_seconds=$(( worker_drain_seconds + 5 ))

# And the daemon's own, which is NOT derived from the above. `fastcached` takes no
# `--drain-timeout` and runs no compiles, so it has nothing to drain: it stops as
# soon as its loop is woken. Tying its bound to a worker's drain would be arithmetic
# that reads as a shared rule and is a category error -- and it is the shape that
# made the old single 15 wrong in the first place.
daemon_stop_seconds=5

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
# @param 3 seconds to allow; every caller passes `$stop_bound_seconds`, which is
#          derived from the drain the process was actually started with
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

# Run one compile through the launcher, capturing its notes.
# @param 1 log file to write the launcher's own output to
# @param 2.. the compile command, launcher excluded
run_launcher() {
    local logfile="$1"; shift
    "$launcher" "$compiler" "$@" > "$logfile" 2>&1
}

# Pull one counter out of a Prometheus body.
#
# The grammar lives HERE and nowhere else. It was written out at three call sites
# before this function existed, and the day the exporter grows a label or renders
# `1` as `1.0` a copy that still matches nothing does not say "the series changed
# shape" -- it says "the counter did not move", which is a statement about the
# subject rather than about the instrument.
#
# Nothing on stdout means the series was ABSENT, which is a different fact from a
# reading of zero and must not be folded into one: a counter is a tally, so zero is
# the truth about events that never happened, while an absent series is a counter
# nothing exports. Every caller checks for the empty string separately.
#
# @param 1 a whole /metrics body
# @param 2 the Prometheus series name
metric_value() {
    local body="$1" name="$2"
    sed -n "s/^${name} \([0-9][0-9]*\)\$/\1/p" <<< "$body" | tail -1
}

# Read one counter off a worker's admin endpoint.
#
# Separate from `metric_value` because the suite's assertions deliberately fetch
# ONE body and read three series out of it -- three requests would be three
# different instants, and a fixture comparing series taken a round trip apart is
# asserting something nobody meant.
#
# @param 1 admin port
# @param 2 the Prometheus series name
worker_counter() {
    local port="$1" name="$2" body=""
    body="$(http_get "$port" /metrics)" || return 1
    metric_value "$body" "$name"
}

# Wait until a worker's counter reaches a floor, the way `wait_for_port` waits for
# a listener, and echo the reading.
#
# The third member of the bounded-wait family rather than a fourth hand-written
# poll loop, for exactly the reason `wait_for_log`'s header gives: two of the three
# loops it replaced never checked that the process was still alive. A counter makes
# that worse, not better -- a worker that DIED simply stops changing its reading,
# so an open-coded poll runs out its bound and then explains the silence with a
# confident sentence about what the counter means.
#
# @param 1 admin port
# @param 2 the Prometheus series name
# @param 3 the floor the reading must reach
# @param 4 pid
# @param 5 what it is, for the message
# @param 6 the log to dump when it does not get there
# A failed scrape does not end the wait, because ending it is what a retry loop
# exists to avoid -- but it is remembered, so a bound that expires having never
# had an answer says THAT rather than blaming the counter. Three outcomes, three
# sentences: nothing ever answered, the series is absent, the reading never rose.
wait_for_counter() {
    local port="$1" name="$2" floor="$3" pid="$4" what="$5" logfile="$6"
    local seconds=10 value="" answered=0
    for _ in $(seq 1 $(( seconds * 5 ))); do
        if value="$(worker_counter "$port" "$name")"; then
            answered=1
            if [[ -n "$value" && "$value" -ge "$floor" ]]; then
                printf '%s\n' "$value"
                return 0
            fi
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$logfile" >&2
            fail "${what} exited while ${name} was still below ${floor}"
        fi
        sleep 0.2
    done
    cat "$logfile" >&2
    [[ "$answered" == "1" ]] || fail "${what} never answered a /metrics request within ${seconds}s"
    [[ -n "$value" ]] || fail "${what} exports no ${name} series"
    fail "${what} never reached ${name} >= ${floor} within ${seconds}s; last reading ${value}"
}

# Wait until a node's heartbeat round was ACCEPTED by its scheduler.
#
# Never the bare word `registered`: the summary line is logged after every round
# whatever the outcome, so `0 of 1 toolchain(s) registered` matches it too and a
# wait built on it returns for a worker the scheduler turned away. That matters
# most where scheduler-side admission is itself under test, and it is a silent
# pass everywhere else -- the failure surfaces two steps later as a dispatch fault.
#
# Every node this fixture starts serves exactly one toolchain, so the accepted
# count is always `1 of 1`.
#
# @param 1 pid
# @param 2 what it is, for the message
# @param 3 the log to watch
wait_for_registration() {
    wait_for_log "1 of 1 toolchain(s) registered" "$1" "$2" "$3"
}

# Slots enough that background CPU cannot withdraw all of them.
#
# `AvailableSlots` reduces a worker's ceiling by the cores its machine is busy
# with OUTSIDE this fleet -- `cpuBusyPermille * logicalCores / 1000`, less this
# fleet's own in-flight jobs -- so a worker offering two slots on a many-core
# machine withdraws both as soon as a few percent of that machine is doing
# something else. This fixture IS that something else: it runs local reference
# compiles on the same box, and on CI the rest of the suite runs beside it. The
# dispatch then comes back `rejected (withdrawn)` and the case fails as "the
# compile was not dispatched to a worker", which reads as a fault in dispatch and
# is a fault in the fixture's sizing.
#
# Offering the whole machine puts the ceiling at cores-minus-external, which
# reaches zero only when the host really is saturated -- and is what a node
# dedicating this machine to the fleet would advertise anyway. The `--slots=1`
# workers elsewhere in this file are deliberate and stay: their cases are ABOUT
# a worker having exactly one.
#
# Above the mode split rather than beside the first worker that uses it, because
# the membership mode below starts workers too and both want the same number.
worker_slots="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"

# --- mode: a compile arriving from an address that is NOT loopback -------------
#
# Every case in the suite below this block is loopback end to end, and that is why
# #235 -- a worker that admitted only its own machine and refused every dispatched
# compile -- survived all of them. `ClusterMembership::Classify` admits loopback
# unconditionally and deliberately, so a client that is always local never reaches
# the branch underneath:
#
#     if (IsLoopbackHost(peerAddress))
#         return Membership::Member;                            <- every case below
#     ...
#     return any_of(_hosts, SameHost) ? Member : Outsider;      <- this mode
#
# A second loopback address does not reach it either, and that was checked rather
# than assumed: `IsLoopbackHost` matches the whole of `127.0.0.0/8` rather than
# `127.0.0.1` alone, and says so in its own comment. What does reach it is this
# host's OWN non-loopback address. Bind the worker there, let the scheduler grant
# that endpoint, and the connection the worker accepts arrives from an address
# outside 127/8 -- with no second machine, no container and no alias involved.
#
# ## Two legs, and only the pair proves anything
#
#   admitted   the address IS in the worker's `--fleet-member`: the compile is
#              dispatched and that worker's job counter moves.
#   refused    the address is NOT in the worker's policy: the worker answers
#              `not-a-member`, the launcher compiles locally, and
#              `fastcache_worker_jobs_refused_not_a_member_total` moves. That
#              counter is the assertion #235 needed and did not have.
#
# The admitted leg on its own would pass over loopback too -- loopback is admitted
# by the branch ABOVE the list -- so it cannot show that the peer address ever
# reached the list. The refused leg is what shows it: a loopback peer would have
# been admitted there as well, and the leg would fail. Neither is worth running
# without the other, which is why they are one mode rather than two.
#
# In BOTH legs the scheduler admits the client. #235's shape is a lease that is
# granted and then a worker that refuses it, so no scheduler counter moves and the
# fleet looks healthy from the only side anybody watches; a refusal coming from the
# scheduler instead would be a different test passing under the same name.
#
# ## What binding on that address does and does not expose
#
# Both workers are started with the cluster key, so every grant is signed and
# checked. Leg 1's worker admits exactly one host -- the address the machine
# already answers on -- and leg 2's admits none but its own loopback, so for the
# few seconds these ports are open the set of callers either would serve is
# {this machine}. A peer address is the kernel's, not a claim in a frame, so
# nothing on the network can present that address without being this machine.
#
# ## Why this is a mode and not a thirteenth case
#
# It is the only assertion here with a prerequisite the machine may not have, and a
# host with no non-loopback address has to report SKIPPED rather than passed. A
# script exits once, so a thirteenth case could do no more than print a line and
# let the run go green -- a pass reported for a case that never ran, which is
# precisely the defect #252 is about. As its own ctest test it has its own state,
# while the helpers, the bounded waits and the port ledger stay shared rather than
# copied.
if [[ "$mode" == "membership" ]]; then
    # Every way this host might name a non-loopback address of its own. A list of
    # probes rather than one command because none of them is portable: `ip` is
    # Linux-only, `hostname -I` is Linux-only and absent from busybox, and
    # `ifconfig` prints three layouts across macOS and two vintages of net-tools.
    # Each row is allowed to fail and the first usable answer wins; the loop below
    # word-splits, so a row may print one candidate per line (`ip`, `ifconfig`) or
    # all of them on one line (`hostname -I`) and neither has to be normalised.
    #
    # This asks the SHELL a question the tree already answers in C++:
    # `Platform/LocalAddresses.hpp`'s `QueryLocalAddresses()` is the authority, and
    # is what the node's own locality oracle consults. Nothing exposes it to a
    # script -- `--print-surfaces` prints configured surfaces, not the machine's
    # addresses -- so this is a second definition of one question, and the two could
    # disagree about a host neither of them was written for. Naming the authority
    # here is what makes that visible when it happens.
    #
    # `up` is part of the `ip` filter, and it is not tidiness: an address on an
    # administratively-down interface is still enumerated, and binding it succeeds
    # while the local route is gone -- so the fixture's own `wait_for_port` would
    # never connect and the run would FAIL where its contract says SKIP. A laptop on
    # Wi-Fi with a configured wired NIC unplugged is the ordinary way to meet that.
    #
    # The other two rows cannot all be filtered the same way, and saying they can
    # would be a comment describing something the code does not do: `hostname -I`
    # reports only up interfaces, but macOS's `ifconfig` with no arguments lists
    # every interface rather than only the up ones. That residual is why a chosen
    # address that will not carry a connection is a named FAIL naming the address --
    # `${tag} never listened on <addr>:<port>` -- rather than anything silent.
    probe_ip_addr() { ip -4 -o addr show scope global up 2>/dev/null | awk '{ print $4 }' | cut -d/ -f1; }
    probe_ifconfig() { ifconfig 2>/dev/null | awk '$1 == "inet" { print $2 }' | sed 's/^addr://'; }
    probe_hostname() { hostname -I 2>/dev/null; }

    # The first address that is a bare IPv4 literal and is neither loopback nor
    # link-local. Link-local is excluded because a 169.254 address means DHCP did
    # not answer -- it is routable by nothing and would fail the bind or the dial
    # for a reason that has nothing to do with membership.
    first_non_loopback_address() {
        local probe out addr
        for probe in probe_ip_addr probe_ifconfig probe_hostname; do
            out="$("$probe" || true)"
            for addr in $out; do
                case "$addr" in
                    127.*|169.254.*|0.0.0.0) continue ;;
                    *[!0-9.]*) continue ;;
                    *.*.*.*) printf '%s\n' "$addr"; return 0 ;;
                esac
            done
        done
        return 1
    }

    lan_address="$(first_non_loopback_address || true)"
    if [[ -z "$lan_address" ]]; then
        # Loudly, and never a quiet fall back to 127.0.0.1. A run over loopback
        # here would exercise the branch ABOVE the member list, pass, and report a
        # result for a property it did not test -- this ticket's own failure mode
        # wearing a different hat.
        echo "dist-compile membership E2E SKIPPED: this host reports no non-loopback IPv4 address"
        echo "  probed: 'ip -4 -o addr show scope global up', 'ifconfig', 'hostname -I'"
        echo "  Without one, every connection to this machine's own worker arrives from 127.0.0.0/8,"
        echo "  which ClusterMembership::Classify admits before it ever consults the member list. There"
        echo "  is no arrangement of loopback addresses that reaches that list (127.0.0.2 included), so"
        echo "  running anyway would report a pass for a case that never ran (#252)."
        exit "$SKIP"
    fi

    echo "== membership: a dispatched compile arriving from ${lan_address}, which is not loopback"

    # A cache of this mode's own, and a real one rather than a drawn port nothing
    # binds.
    #
    # Nothing here asserts anything about the cache, so an unreachable one would
    # also do -- case 12 proves a compile still dispatches around one. What that
    # would give up is the control: with a daemon answering, the admitted leg walks
    # exactly the path the suite's case 1 walks (fetch, miss, dispatch, store), so
    # the refusing leg differs from it in ONE flag rather than in a flag and a
    # cache. Leaving `FASTCACHE_ADDR` unset is the option that is simply wrong: it
    # sends the launcher to 127.0.0.1:6674, which on a developer machine is very
    # likely a real node serving real builds.
    mem_cache_port="$(free_port)"
    "$fastcached" --listen="127.0.0.1:${mem_cache_port}" \
        --storage-max-value=64M --log-level=info \
        > "${workdir}/mem-daemon.log" 2>&1 &
    mem_daemon_pid=$!
    pids+=("$mem_daemon_pid")
    wait_for_port "$mem_cache_port" "$mem_daemon_pid" "membership daemon" "${workdir}/mem-daemon.log"
    export FASTCACHE_ADDR="127.0.0.1:${mem_cache_port}"

    proj="${workdir}/memproj"
    mkdir -p "${proj}/build"
    export FASTCACHE_SOURCE_DIR="${proj}"
    export FASTCACHE_BINARY_DIR="${proj}/build"

    # Start a scheduler that admits this host's non-loopback address.
    #
    # Both legs get one of their own, so that each leg's worker is the ONLY match
    # its scheduler has: sharing one would leave the admitting worker available to
    # the refusing leg and turn a refusal into a second dispatch. The scheduler's
    # own worker surface names a toolchain nothing here compiles with, for the same
    # reason the suite's does.
    #
    # `--fleet-member` rather than `--fleet-open`, so the scheduler's own gate is
    # reached from a non-loopback address too. The client dials it at $lan_address,
    # and so does the worker when it registers.
    #
    # The pid of the process the last `start_membership_*` call started, and the
    # only way it is handed back. A function that PRINTED its pid would have to be
    # called in a command substitution, and `fail` inside one of those ends the
    # subshell rather than the run -- so a start that failed would hand its caller
    # an empty pid and carry on.
    started_pid=""

    # @param 1 tag, for the log file and the messages
    # @param 2 dispatch (`--listen-node`) port
    start_membership_scheduler() {
        local tag="$1" dispatch="$2" own_port="" pid=""
        # Its own worker surface's port is drawn here rather than by the caller: no
        # assertion in either leg names it, and a port nothing reads is scaffolding
        # the reader has to carry to the end of the block to find out.
        own_port="$(free_port)"
        "$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" \
            --serve-scheduler --listen-node="${lan_address}:${dispatch}" \
            --fleet-member="$lan_address" \
            --scheduler="${lan_address}:${dispatch}" \
            --advertise="${lan_address}:${dispatch}" \
            --toolchain="scheduler-only=${compiler}" --slots=1 --log-level=debug \
            > "${workdir}/${tag}.log" 2>&1 &
        pid=$!
        started_pid="$pid"
        pids+=("$pid")
        wait_for_port "$dispatch" "$pid" "${tag} scheduler" "${workdir}/${tag}.log" "$lan_address"
        wait_for_log "scheduling for the fleet" "$pid" "${tag} scheduler" "${workdir}/${tag}.log"
    }

    # Start a worker bound on this host's non-loopback address.
    #
    # The toolchain fingerprint is PINNED to the one the launcher reported rather
    # than probed, which is the whole of `--toolchain=<fingerprint>=<compiler>`. It
    # matches for the same reason the suite's bare `--toolchain` does -- the suite
    # asserts that agreement, once, and this mode is not about it -- and it skips an
    # include-tree walk per worker, which is the expensive part of starting one.
    #
    # That also takes away the one stall these waits could not diagnose. A walk logs
    # nothing while it runs, so a slow one and a wedge produce identical logs and
    # need the CPU classifier `node-scratch-isolation-e2e` carries; with the
    # fingerprint pinned there is no walk here, and a worker that has not logged
    # `compile node ready` within the bound has not started.
    #
    # @param 1 tag, for the log file and the messages
    # @param 2 dispatch port to register with
    # @param 3 admin port
    # @param 4.. the membership flags under test, if any
    start_membership_worker() {
        local tag="$1" dispatch="$2" admin="$3" port="" pid=""
        shift 3
        # As above: the compile port is the worker's own business, since the client
        # is told where to dial by the lease rather than by this fixture.
        port="$(free_port)"
        "$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" \
            --scheduler="${lan_address}:${dispatch}" \
            --listen-node="${lan_address}:${port}" --advertise="${lan_address}:${port}" \
            --admin-listen="$admin" \
            --toolchain="${fingerprint}=${compiler}" --slots="$worker_slots" --log-level=debug \
            ${@+"$@"} > "${workdir}/${tag}.log" 2>&1 &
        pid=$!
        started_pid="$pid"
        pids+=("$pid")
        # Bind and registration are waited for separately, because a stall in one is
        # a different fault from a stall in the other and a fixture that folds them
        # cannot say which happened.
        wait_for_port "$port" "$pid" "${tag} worker" "${workdir}/${tag}.log" "$lan_address"
        wait_for_log "compile node ready" "$pid" "${tag} worker" "${workdir}/${tag}.log"
        wait_for_registration "$pid" "${tag} worker" "${workdir}/${tag}.log"
        wait_for_port "$admin" "$pid" "${tag} worker admin endpoint" "${workdir}/${tag}.log"
    }

    # --- leg 1: the address is a member, and the compile is served ---------------
    echo "== membership leg 1: ${lan_address} listed as a member"
    admit_dispatch_port="$(free_port)"
    admit_admin_port="$(free_port)"

    start_membership_scheduler "mem-admit-scheduler" "$admit_dispatch_port"
    start_membership_worker "mem-admit-worker" "$admit_dispatch_port" "$admit_admin_port" \
        --fleet-member="$lan_address"
    admit_worker_pid="$started_pid"

    # The policy the worker actually adopted, from its own ready line. Asserted
    # because the two legs differ in exactly one flag, and a leg that silently
    # started with the OTHER leg's policy would still produce a plausible result.
    grep -q "this machine plus 1 member host(s)" "${workdir}/mem-admit-worker.log" \
        || { cat "${workdir}/mem-admit-worker.log" >&2; fail "the admitting worker did not report a member list"; }

    write_source "${proj}/admitted.cpp" "memberadmitted"
    "$compiler" -std=c++17 -O1 -c "${proj}/admitted.cpp" -o "${proj}/build/admitted-ref.o" \
        || fail "the membership reference compile failed"

    export FASTCACHE_SCHEDULER="${lan_address}:${admit_dispatch_port}"
    run_launcher "${workdir}/mem-admitted.log" -std=c++17 -O1 -c "${proj}/admitted.cpp" -o "${proj}/build/admitted.o" \
        || { cat "${workdir}/mem-admitted.log" >&2; fail "the compile from a member address failed"; }

    grep -q "DISPATCHED to " "${workdir}/mem-admitted.log" \
        || {
            cat "${workdir}/mem-admitted.log" >&2
            echo "--- worker log ---" >&2
            cat "${workdir}/mem-admit-worker.log" >&2
            fail "a compile from a listed member address was not dispatched"
        }
    cmp -s "${proj}/build/admitted-ref.o" "${proj}/build/admitted.o" \
        || fail "the object built for a member address differs from the local one"

    # The job counter is polled and the refusal counter is read from the same
    # instant -- one scrape, two series. The worker completes a job on the thread
    # that ran it, after the client already has its object, so a single read of
    # `jobs_completed_total` races the reply; and the two readings must come from
    # ONE body, because "it served a job and refused nobody" is a statement about a
    # moment rather than about two scrapes a round trip apart.
    #
    # This is also where the pair earns its keep from the other side: the fixture's
    # own `wait_for_port` dialled this worker from $lan_address, and here that is
    # ADMITTED -- the flat zero below is that probe passing the member list, against
    # leg 2 where the identical probe is refused.
    admit_completed="$(wait_for_counter "$admit_admin_port" fastcache_worker_jobs_completed_total 1 \
        "$admit_worker_pid" "the admitting worker" "${workdir}/mem-admit-worker.log")"
    admit_metrics="$(http_get "$admit_admin_port" /metrics)" \
        || fail "the admitting worker's admin endpoint refused a /metrics request"
    admit_refused="$(metric_value "$admit_metrics" fastcache_worker_jobs_refused_not_a_member_total)"
    # Absent is not zero, and here it would read as one: an empty string compares
    # unequal to "0" and the failure would name a count nobody exported.
    [[ -n "$admit_refused" ]] \
        || fail "the admitting worker exports no fastcache_worker_jobs_refused_not_a_member_total series"
    [[ "$admit_refused" == "0" ]] \
        || fail "the admitting worker refused ${admit_refused} caller(s) as not-a-member"
    echo "   dispatched and served: ${admit_completed} job(s), 0 refused"

    # --- leg 2: the same address, not a member, and the worker says so -----------
    #
    # The worker is started with NO membership flags at all, which is the default
    # and admits this machine's loopback and nothing else. Its scheduler still lists
    # $lan_address, so the lease IS granted -- #235's shape exactly -- and the
    # refusal has to come from the worker.
    echo "== membership leg 2: ${lan_address} absent from the worker's policy"
    refuse_dispatch_port="$(free_port)"
    refuse_admin_port="$(free_port)"

    start_membership_scheduler "mem-refuse-scheduler" "$refuse_dispatch_port"
    start_membership_worker "mem-refuse-worker" "$refuse_dispatch_port" "$refuse_admin_port"
    refuse_worker_pid="$started_pid"

    grep -q "this machine only" "${workdir}/mem-refuse-worker.log" \
        || { cat "${workdir}/mem-refuse-worker.log" >&2; fail "the refusing worker did not report a loopback-only policy"; }

    # The reading BEFORE the compile, and it is not expected to be zero.
    #
    # `wait_for_port` dials the compile port from this same non-loopback address to
    # decide the worker is up, and that probe is a caller like any other: leg 1
    # admits it -- which is what the flat zero up there says -- and this leg refuses
    # it. So the branch under test is already observable here, the floor of 1 is an
    # assertion rather than a formality, and what the compile has to add is a
    # FURTHER refusal, measured as a delta from this reading.
    refuse_before="$(wait_for_counter "$refuse_admin_port" fastcache_worker_jobs_refused_not_a_member_total 1 \
        "$refuse_worker_pid" \
        "the refusing worker (it never refused this fixture's own probe from ${lan_address}, so that probe reached it as an admitted caller and this leg would prove nothing)" \
        "${workdir}/mem-refuse-worker.log")"

    write_source "${proj}/refused.cpp" "memberrefused"

    export FASTCACHE_SCHEDULER="${lan_address}:${refuse_dispatch_port}"
    run_launcher "${workdir}/mem-refused.log" -std=c++17 -O1 -c "${proj}/refused.cpp" -o "${proj}/build/refused.o" \
        || { cat "${workdir}/mem-refused.log" >&2; fail "a build refused by a worker did not survive"; }

    # The client's half: a typed refusal naming the reason, and a local compile.
    # `refused the job:` is part of the match rather than `rejected (not-a-member)`
    # alone, because that phrase is what says the refusal came from the WORKER the
    # lease named -- a scheduler declining the lease would be a different failure
    # reported in a different sentence, and this leg would then be asserting the
    # gate it deliberately arranged NOT to test.
    grep -q "refused the job: rejected (not-a-member)" "${workdir}/mem-refused.log" \
        || {
            cat "${workdir}/mem-refused.log" >&2
            echo "--- worker log ---" >&2
            cat "${workdir}/mem-refuse-worker.log" >&2
            fail "a compile from an unlisted address was not refused as not-a-member by the worker"
        }
    grep -q "; compiling locally" "${workdir}/mem-refused.log" \
        || { cat "${workdir}/mem-refused.log" >&2; fail "a refused compile did not fall back to a local one"; }
    # An object exists and is not empty, and deliberately no `cmp` against a
    # reference. Both sides of such a comparison would be this machine's own
    # compiler on the same source with the same flags, so it asserts compiler
    # determinism at the price of a second full compile; that the local fallback
    # produces a CORRECT object is case 4's, which owns it.
    [[ -s "${proj}/build/refused.o" ]] \
        || fail "no object was written after a membership refusal"

    # The worker's half, and the reason this leg exists. #235 was invisible from
    # every other vantage point -- the lease was granted, so no scheduler counter
    # moved and the build went green -- and this counter was the one signal that
    # would have named it. Bounded rather than read once, because the refusal is
    # counted on the worker's accept loop and the client has its answer first.
    refuse_after="$(wait_for_counter "$refuse_admin_port" fastcache_worker_jobs_refused_not_a_member_total \
        $(( refuse_before + 1 )) "$refuse_worker_pid" \
        "the refusing worker (it refused the compile and the counter never moved past ${refuse_before})" \
        "${workdir}/mem-refuse-worker.log")"
    refuse_metrics="$(http_get "$refuse_admin_port" /metrics)" \
        || fail "the refusing worker's admin endpoint refused a /metrics request"
    refuse_completed="$(metric_value "$refuse_metrics" fastcache_worker_jobs_completed_total)"
    [[ -n "$refuse_completed" ]] \
        || fail "the refusing worker exports no fastcache_worker_jobs_completed_total series"
    [[ "$refuse_completed" == "0" ]] \
        || fail "a worker that refused the caller still completed ${refuse_completed} job(s)"
    echo "   refused as not-a-member (counter ${refuse_before} -> ${refuse_after}), and the build compiled locally"

    echo
    echo "dist-compile membership E2E PASSED"
    exit 0
fi

# --- start the cache ---------------------------------------------------------
# One listener now, and only the cache. `fastcached` used to carry the scheduler
# as well, on a second `--listen-dispatch` endpoint; that flag is gone. The two
# have opposite deployment shapes -- a cache is shared infrastructure somebody
# operates, while handing out capacity is a decision only ONE node may make at a
# time -- and nothing in the cache daemon can establish which node that is. So
# the scheduler moved to where leadership lives, which is the compile node.
#
# One 0xFC surface since #290 stage 3: --listen-node is the bind, and compiles
# arrive on it beside the cache and scheduler verbs.
cache_port="$(free_port)"

"$fastcached" --listen="127.0.0.1:${cache_port}" \
    --storage-max-value=64M --log-level=info \
    > "${workdir}/daemon.log" 2>&1 &
daemon_pid=$!
pids+=("$daemon_pid")
wait_for_port "$cache_port" "$daemon_pid" "daemon" "${workdir}/daemon.log"

export FASTCACHE_ADDR="127.0.0.1:${cache_port}"

# --- start the scheduler -----------------------------------------------------
# A compile node running the fleet's scheduler. --fleet-open because every peer
# here is loopback -- and because the policy has to be STATED: a node with no
# member list refuses everybody, which is the right default and not a working
# configuration, so it is refused at startup rather than discovered later as a
# fleet that silently distributes nothing.
#
# It names a toolchain nothing here compiles with, deliberately. Every node is
# both a peer and a possible scheduler, so it always registers as a worker too --
# and a second MATCHING worker would make "which worker ran this job" a race,
# which the cases below assert against by reading one worker's counters. That a
# scheduler CAN also take work is the point of the architecture; it is simply not
# what these cases are measuring.
dispatch_port="$(free_port)"
sched_worker_port="$(free_port)"

"$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" --serve-scheduler --listen-node="127.0.0.1:${dispatch_port}" --fleet-open \
    --scheduler="127.0.0.1:${dispatch_port}" \
    --advertise="127.0.0.1:${dispatch_port}" \
    --toolchain="scheduler-only=${compiler}" --slots=1 --log-level=debug \
    > "${workdir}/scheduler.log" 2>&1 &
scheduler_pid=$!
pids+=("$scheduler_pid")
wait_for_port "$dispatch_port" "$scheduler_pid" "scheduler" "${workdir}/scheduler.log"

wait_for_log "scheduling for the fleet" "$scheduler_pid" "scheduler" "${workdir}/scheduler.log"

# --- start a worker ----------------------------------------------------------

worker_port="$(free_port)"
worker_admin_port="$(free_port)"
"$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" --scheduler="127.0.0.1:${dispatch_port}" \
    --listen-node="127.0.0.1:${worker_port}" --advertise="127.0.0.1:${worker_port}" \
    --admin-listen="$worker_admin_port" \
    --toolchain="${compiler}" --slots="$worker_slots" --log-level=debug \
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

wait_for_registration "$worker_pid" "worker" "${workdir}/worker.log"

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
completed="$(metric_value "$after" fastcache_worker_jobs_completed_total)"
[[ -n "$completed" && "$completed" -ge 1 ]] \
    || { printf '%s\n' "$after" >&2; fail "worker completed a compile but its jobs counter did not move"; }
millis="$(metric_value "$after" fastcache_worker_compile_milliseconds_total)"
[[ -n "$millis" ]] \
    || { printf '%s\n' "$after" >&2; fail "worker reports no compile wall time"; }
bytes="$(metric_value "$after" fastcache_worker_bytes_received_total)"
[[ -n "$bytes" && "$bytes" -ge 1 ]] \
    || { printf '%s\n' "$after" >&2; fail "worker received a job but its byte counter did not move"; }
echo "   worker metrics moved: ${completed} job(s), ${millis}ms, ${bytes} bytes in"

# --- 3: a worker for a different toolchain is never chosen -------------------
echo "== case 3: fingerprint isolation"
# A second cache and a second SCHEDULER, so the mismatched worker is the ONLY
# one registered with it. Reusing the first scheduler would leave the matching
# worker available and the case would pass without testing anything.
#
# The scheduler node names a toolchain nothing here uses, for the same reason:
# a scheduler that also served the real compiler would be a second matching
# worker, which is exactly what this case must not have.
iso_cache_port="$(free_port)"
iso_dispatch_port="$(free_port)"
iso_sched_worker_port="$(free_port)"
"$fastcached" --listen="127.0.0.1:${iso_cache_port}" \
    --log-level=info > "${workdir}/iso-daemon.log" 2>&1 &
iso_daemon_pid=$!
pids+=("$iso_daemon_pid")
wait_for_port "$iso_cache_port" "$iso_daemon_pid" "isolation daemon" "${workdir}/iso-daemon.log"

"$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" --serve-scheduler --listen-node="127.0.0.1:${iso_dispatch_port}" --fleet-open \
    --scheduler="127.0.0.1:${iso_dispatch_port}" \
    --advertise="127.0.0.1:${iso_dispatch_port}" \
    --toolchain="also-not-the-compiler-this-client-uses=${compiler}" \
    --slots=1 --log-level=debug > "${workdir}/iso-scheduler.log" 2>&1 &
iso_scheduler_pid=$!
pids+=("$iso_scheduler_pid")
wait_for_port "$iso_dispatch_port" "$iso_scheduler_pid" "isolation scheduler" "${workdir}/iso-scheduler.log"

iso_worker_port="$(free_port)"
"$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" --scheduler="127.0.0.1:${iso_dispatch_port}" \
    --listen-node="127.0.0.1:${iso_worker_port}" --advertise="127.0.0.1:${iso_worker_port}" \
    --toolchain="not-the-compiler-this-client-uses=${compiler}" --slots=2 --log-level=debug \
    > "${workdir}/iso-worker.log" 2>&1 &
iso_worker_pid=$!
pids+=("$iso_worker_pid")
wait_for_port "$iso_worker_port" "$iso_worker_pid" "isolation worker" "${workdir}/iso-worker.log"
wait_for_registration "$iso_worker_pid" "isolation worker" "${workdir}/iso-worker.log"

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
stop_and_require_exit "$iso_worker_pid" "the isolation worker" "$stop_bound_seconds"

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
cap_sched_worker_port="$(free_port)"
"$fastcached" --listen="127.0.0.1:${cap_cache_port}" \
    --log-level=info > "${workdir}/cap-daemon.log" 2>&1 &
cap_daemon_pid=$!
pids+=("$cap_daemon_pid")
wait_for_port "$cap_cache_port" "$cap_daemon_pid" "capacity daemon" "${workdir}/cap-daemon.log"

# The scheduler node names a toolchain nothing here compiles with, deliberately.
# Every node is both a peer and a possible scheduler, so it always registers as a
# worker too -- and a second MATCHING worker would give this fleet two slots when
# the whole point of the case is that it has one.
"$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" --serve-scheduler --listen-node="127.0.0.1:${cap_dispatch_port}" --fleet-open \
    --scheduler="127.0.0.1:${cap_dispatch_port}" \
    --advertise="127.0.0.1:${cap_dispatch_port}" \
    --toolchain="not-the-compiler-under-test=${compiler}" \
    --slots=1 --log-level=debug > "${workdir}/cap-scheduler.log" 2>&1 &
cap_scheduler_pid=$!
pids+=("$cap_scheduler_pid")
wait_for_port "$cap_dispatch_port" "$cap_scheduler_pid" "capacity scheduler" "${workdir}/cap-scheduler.log"

cap_worker_port="$(free_port)"
"$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" --scheduler="127.0.0.1:${cap_dispatch_port}" \
    --listen-node="127.0.0.1:${cap_worker_port}" --advertise="127.0.0.1:${cap_worker_port}" \
    --toolchain="${compiler}" --slots=1 --log-level=debug \
    > "${workdir}/cap-worker.log" 2>&1 &
cap_worker_pid=$!
pids+=("$cap_worker_pid")
wait_for_port "$cap_worker_port" "$cap_worker_pid" "capacity worker" "${workdir}/cap-worker.log"
wait_for_registration "$cap_worker_pid" "capacity worker" "${workdir}/cap-worker.log"

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
#
# Its fingerprint is PINNED and unique, like every other single-purpose worker here,
# and that is not cosmetic. This worker registers with the shared scheduler and is
# then deliberately killed -- but the scheduler only learns a worker is gone when its
# heartbeat lapses, which is by design: a polite goodbye would be a second path to
# "this worker is alive", exercised on exactly the shutdowns that are already
# harmless and never on the crash that matters (the reasoning is at the end of
# `WorkerBody`). So for the rest of the heartbeat window this endpoint is still
# leasable.
#
# On the DEFAULT fingerprint that made this corpse a candidate for case 12, which
# asks the same scheduler for the same toolchain the launcher uses. The scheduler
# hands out the dead endpoint, the dispatch fails at the transport, the client
# correctly falls back to a local compile -- and case 12, which asserts DISPATCHED,
# fails for a reason that has nothing to do with what it guards. Seen twice on
# macOS, once ejecting a pull request from the merge queue (#427).
#
# Pinning removes the overlap rather than papering over it: nothing leases
# `graceful-stop-only`, so a corpse under that name is inert. Loosening case 12 to
# accept a local fallback would have deleted the #236 assertion it exists for.
stop_port="$(free_port)"
"$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" --scheduler="127.0.0.1:${dispatch_port}" \
    --listen-node="127.0.0.1:${stop_port}" --advertise="127.0.0.1:${stop_port}" \
    --toolchain="graceful-stop-only=${compiler}" --slots=1 --log-level=info \
    > "${workdir}/stop-worker.log" 2>&1 &
stop_worker_pid=$!
pids+=("$stop_worker_pid")
wait_for_port "$stop_port" "$stop_worker_pid" "shutdown worker" "${workdir}/stop-worker.log"

stop_and_require_exit "$stop_worker_pid" "the worker under test" "$stop_bound_seconds"

# Exiting is necessary but not sufficient: a worker that died of the signal also
# "exits". These lines are what distinguish a graceful stop from a death, and
# their absence would mean the handler never ran.
grep -q "stop requested; no longer accepting compiles" "${workdir}/stop-worker.log" \
    || { cat "${workdir}/stop-worker.log" >&2; fail "the worker did not report a graceful stop"; }
grep -q "compile node stopped" "${workdir}/stop-worker.log" \
    || { cat "${workdir}/stop-worker.log" >&2; fail "the worker did not run its shutdown path to completion"; }
echo "   the worker stopped gracefully on SIGTERM"

# --- 9: a node's own cache serves a hit without the network --------------------
echo "== case 9: a local hit never reaches the shared cache"
# The reason the node has a cache tier at all: a rebuild on a slow or bad network
# should not go to the wire. The shared cache holds every object, so a second copy
# looks redundant -- what is saved is the ROUND TRIP, not the compile.
#
# Proved by taking the shared cache away. A tier that merely proxied, or that
# revalidated a hit against the upstream, would fail the third compile; one that
# genuinely holds the object serves it with nothing listening upstream.
cache_node_port="$(free_port)"
cache_node_worker="$(free_port)"
cache_upstream_port="$(free_port)"

"$fastcached" --listen="127.0.0.1:${cache_upstream_port}" --log-level=info     > "${workdir}/tier-upstream.log" 2>&1 &
tier_upstream_pid=$!
pids+=("$tier_upstream_pid")
wait_for_port "$cache_upstream_port" "$tier_upstream_pid" "tier upstream" "${workdir}/tier-upstream.log"

"$node" "$stated_drain" --cluster-key-file="$cluster_key" --listen-node="127.0.0.1:${cache_node_port}" --cache-memory=64m     --upstream="127.0.0.1:${cache_upstream_port}"     --scheduler="127.0.0.1:${dispatch_port}"     --advertise="127.0.0.1:${cache_node_port}"     --toolchain="cache-node-only=${compiler}" --slots=1 --log-level=debug     > "${workdir}/tier-node.log" 2>&1 &
tier_node_pid=$!
pids+=("$tier_node_pid")
wait_for_port "$cache_node_port" "$tier_node_pid" "cache node" "${workdir}/tier-node.log"

write_source "${proj}/nine.cpp" "casenine"
"$compiler" -std=c++17 -O1 -c "${proj}/nine.cpp" -o "${proj}/build/nine-ref.o"     || fail "the case 9 reference compile failed"

(
    # Pointed at the NODE, not at the shared cache. That is the whole deployment
    # this tier exists for.
    export FASTCACHE_ADDR="127.0.0.1:${cache_node_port}"
    unset FASTCACHE_SCHEDULER
    run_launcher "${workdir}/case9-store.log" -std=c++17 -O1 -c "${proj}/nine.cpp" -o "${proj}/build/nine.o"
) || { cat "${workdir}/case9-store.log" >&2; fail "the first compile through the node failed"; }

rm -f "${proj}/build/nine.o"
(
    export FASTCACHE_ADDR="127.0.0.1:${cache_node_port}"
    unset FASTCACHE_SCHEDULER
    run_launcher "${workdir}/case9-hit.log" -std=c++17 -O1 -c "${proj}/nine.cpp" -o "${proj}/build/nine.o"
) || { cat "${workdir}/case9-hit.log" >&2; fail "the second compile through the node failed"; }
grep -q "fastcache-cc: HIT" "${workdir}/case9-hit.log"     || { cat "${workdir}/case9-hit.log" >&2; fail "the node did not serve a hit"; }

# Now take the shared cache away and ask again. This is the assertion: a hit is
# answered from the node's own tier, so it must survive an upstream that is gone.
stop_and_require_exit "$tier_upstream_pid" "the shared cache" "$daemon_stop_seconds"

rm -f "${proj}/build/nine.o"
(
    export FASTCACHE_ADDR="127.0.0.1:${cache_node_port}"
    unset FASTCACHE_SCHEDULER
    run_launcher "${workdir}/case9-offline.log" -std=c++17 -O1 -c "${proj}/nine.cpp" -o "${proj}/build/nine.o"
) || { cat "${workdir}/case9-offline.log" >&2; fail "a build survived neither the hit nor the fallback"; }
grep -q "fastcache-cc: HIT" "${workdir}/case9-offline.log"     || { cat "${workdir}/case9-offline.log" >&2; fail "the node went to the network for an object it held"; }

cmp -s "${proj}/build/nine-ref.o" "${proj}/build/nine.o"     || fail "the object served from the node's tier is wrong"
echo "   a hit was served with the shared cache stopped, and the object is right"

# --- case 10: a worker that names no slot count sizes itself ------------------
#
# Every other case passes --slots explicitly, so the DERIVED path -- which is what
# an operator who reads the docs and sets only --node-class actually runs -- would
# otherwise never be exercised outside unit tests. The failure it guards against is
# quiet in the usual way: a worker that derived zero, or that refused the flag,
# registers and is simply never picked, which reads at the client as a fleet that is
# permanently busy.
echo "== case 10: a worker sizes itself from its node class"

sizing_port="$(free_port)"
"$node" "$stated_drain" "$no_local_cache" --cluster-key-file="$cluster_key" --scheduler="127.0.0.1:${dispatch_port}" \
    --listen-node="127.0.0.1:${sizing_port}" --advertise="127.0.0.1:${sizing_port}" \
    --toolchain="self-sizing=${compiler}" \
    --node-class=dedicated --reserve-cores=0 --log-level=debug \
    > "${workdir}/sizing.log" 2>&1 &
sizing_pid=$!
pids+=("$sizing_pid")
wait_for_port "$sizing_port" "$sizing_pid" "self-sizing worker" "${workdir}/sizing.log"

# The count itself depends on the runner, so what is asserted is that it is a
# positive number and that the class reached the worker -- not a fixed value, which
# would make this case a report about the CI machine rather than about the code.
sizing_slots="$(sed -n "s/.*, \([0-9][0-9]*\) slot(s) as a dedicated node.*/\1/p" "${workdir}/sizing.log" | head -1)"
[[ -n "$sizing_slots" ]] \
    || { cat "${workdir}/sizing.log" >&2; fail "the worker did not report a derived slot count for its node class"; }
[[ "$sizing_slots" -ge 1 ]] \
    || { cat "${workdir}/sizing.log" >&2; fail "a self-sizing worker offered no slots at all"; }
echo "   a dedicated worker sized itself to ${sizing_slots} slot(s) with no --slots given"

stop_and_require_exit "$sizing_pid" "the self-sizing worker" "$stop_bound_seconds"

# --- case 11: a black-holed upstream does not stall the node's own clients -----
#
# THE regression case for the reason the node's cache tier moved onto a reactor.
# `RemoteUpstream` dials from inside a cache answer, and answering used to be
# serialized -- so one upstream that never responded held every local
# `fastcache-cc` behind it for the full per-operation ceiling, one after another.
# A node with an unreachable shared cache had an unusable port of its own.
#
# 192.0.2.1 is RFC 5737 documentation space: it BLACK-HOLES rather than refusing,
# which is the distinction that matters. A refused connect returns at once and
# would not reproduce this at all.
#
# What is ASSERTED here is correctness -- both objects are produced, and the node
# survives an upstream that never answers -- and the elapsed time is only REPORTED.
# That split is deliberate. Measured, two concurrent clients take ~3s and two
# serialized ones would take ~4s: the gap is one connect ceiling, which is thinner
# than the variance of a shared CI runner, so a threshold there would be a test that
# fails for reasons about the machine. This repository has already paid for that
# class of failure.
#
# The concurrency itself is proven deterministically instead, by
# `FrameEndpoint_test`'s held-answer case -- which was verified by serving inline
# again and watching only it fail. What this case adds is the integration: a real
# node, a real black hole, and two real launchers.
echo "== case 11: a black-holed upstream does not stall a second client"

blackhole_node_port="$(free_port)"
blackhole_worker_port="$(free_port)"

# `--scheduler` is required whenever a worker surface is configured -- a worker
# nothing knows about serves nobody, and the node refuses to start rather than
# looking healthy. It points at the scheduler this run already has.
"$node" "$stated_drain" --cluster-key-file="$cluster_key" --listen-node="127.0.0.1:${blackhole_node_port}" --cache-memory=64m     --upstream="192.0.2.1:6674"     --scheduler="127.0.0.1:${dispatch_port}"     --advertise="127.0.0.1:${blackhole_node_port}"     --toolchain="blackhole-node=${compiler}" --slots=1 --log-level=info     > "${workdir}/blackhole-node.log" 2>&1 &
blackhole_node_pid=$!
pids+=("$blackhole_node_pid")
wait_for_port "$blackhole_node_port" "$blackhole_node_pid" "black-hole node" "${workdir}/blackhole-node.log"

write_source "${proj}/eleven_a.cpp" "caseelevena"
write_source "${proj}/eleven_b.cpp" "caseelevenb"

blackhole_started=$(date +%s)
(
    export FASTCACHE_ADDR="127.0.0.1:${blackhole_node_port}"
    unset FASTCACHE_SCHEDULER
    run_launcher "${workdir}/case11-a.log" -std=c++17 -O1 -c "${proj}/eleven_a.cpp" -o "${proj}/build/eleven_a.o"
) &
blackhole_a=$!
(
    export FASTCACHE_ADDR="127.0.0.1:${blackhole_node_port}"
    unset FASTCACHE_SCHEDULER
    run_launcher "${workdir}/case11-b.log" -std=c++17 -O1 -c "${proj}/eleven_b.cpp" -o "${proj}/build/eleven_b.o"
) &
blackhole_b=$!

wait "$blackhole_a" || { cat "${workdir}/case11-a.log" >&2; fail "the first compile through the black-holed node failed"; }
wait "$blackhole_b" || { cat "${workdir}/case11-b.log" >&2; fail "the second compile through the black-holed node failed"; }
blackhole_elapsed=$(( $(date +%s) - blackhole_started ))

# Both objects exist, which is the part that must hold whatever the timing: an
# unreachable shared cache costs a miss, never a failed build.
[[ -s "${proj}/build/eleven_a.o" ]] || fail "the first object was not produced"
[[ -s "${proj}/build/eleven_b.o" ]] || fail "the second object was not produced"

# A ceiling loose enough to be about hanging rather than about speed: this is here
# so a node that wedges outright on an unreachable upstream fails the case instead
# of running until ctest's own timeout with nothing to say why.
if (( blackhole_elapsed > 60 )); then
    cat "${workdir}/blackhole-node.log" >&2
    fail "two clients behind a black-holed upstream took ${blackhole_elapsed}s; the node appears wedged"
fi
echo "   both clients were served behind an unreachable upstream (${blackhole_elapsed}s)"

# --- case 12: an unreachable cache does not take the fleet with it -------------
#
# THE regression case for issue #236. `RunCached` returned on a fetch that failed
# at the transport, above the call site that would dispatch -- so a cache the
# launcher could not reach turned off distribution as well. The docs put the
# shared cache on a different machine from the scheduler, which makes those two
# independent failure domains, and the less important one was load-bearing for the
# more important one: on an estate of forty machines a mistyped FASTCACHE_ADDR
# sent every build local while the fleet sat idle and healthy, with a green build
# and nothing in the log but one line about the cache.
#
# A port drawn and never bound, so the connect is REFUSED rather than black-holed:
# what is under test is the control flow after a transport failure, not how long
# one takes to notice. Case 11 above covers the black hole, on the node's side.
echo "== case 12: an unreachable cache still dispatches"

write_source "${proj}/twelve.cpp" "casetwelve"
"$compiler" -std=c++17 -O1 -c "${proj}/twelve.cpp" -o "${proj}/build/twelve-ref.o" \
    || fail "the case 12 reference compile failed"

dead_cache_port="$(free_port)"
(
    export FASTCACHE_ADDR="127.0.0.1:${dead_cache_port}"
    export FASTCACHE_SCHEDULER="127.0.0.1:${dispatch_port}"
    run_launcher "${workdir}/case12.log" -std=c++17 -O1 -c "${proj}/twelve.cpp" -o "${proj}/build/twelve.o"
) || { cat "${workdir}/case12.log" >&2; fail "the build did not survive an unreachable cache"; }

grep -q "DISPATCHED to " "${workdir}/case12.log" \
    || {
        cat "${workdir}/case12.log" >&2
        echo "--- worker log ---" >&2
        cat "${workdir}/worker.log" >&2
        fail "an unreachable cache stopped the compile from being dispatched"
    }

# The cache failure is still SAID, and said as a cache failure. Reaching the
# dispatch path must not turn an unreachable daemon into something an operator
# cannot see: `--show-stats` ranks this reason, and a build that quietly stopped
# caching is the defect this line exists to prevent.
grep -q "cache unavailable (fetch exchange failed)" "${workdir}/case12.log" \
    || { cat "${workdir}/case12.log" >&2; fail "an unreachable cache was not reported as one"; }

# And it is not reported as a miss. A MISS trace would clear the reason above and
# make a broken cache read as a cold one.
grep -q "fastcache-cc: MISS" "${workdir}/case12.log" \
    && { cat "${workdir}/case12.log" >&2; fail "a cache that never answered was traced as a miss"; }

# Nothing is pushed at a daemon that did not answer the fetch. Before the fix a
# failed fetch returned, so nothing was ever offered to a daemon that had just
# failed to answer; carrying on had to leave that true.
grep -q "STORED key=" "${workdir}/case12.log" \
    && { cat "${workdir}/case12.log" >&2; fail "an object was offered to a cache that never answered"; }

cmp -s "${proj}/build/twelve-ref.o" "${proj}/build/twelve.o" \
    || fail "the object dispatched around an unreachable cache is wrong"
echo "   the fleet compiled it with the cache unreachable, and said so"

echo
echo "dist-compile E2E PASSED"
