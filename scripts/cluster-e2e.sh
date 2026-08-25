#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of a three-node cluster (POSIX). Starts three
# fastcache-compile-node processes that know each other, waits for them to elect a
# leader, and drives the cluster-administration verbs against every one of them.
#
# The properties asserted here are the ones no unit test can reach, because each
# needs three real processes, a real election and a real socket between them:
#
#   1. One leader, and it — of three nodes that started together, exactly ONE
#      keeps leading         answers a cluster verb, the other two refuse, and that
#                            is still true three seconds later. Every unit test in
#                            the consensus library asserts the first half about a
#                            simulated cluster; this asserts it about three
#                            processes and a network stack, which is where the wire,
#                            the transport and the timers all get their first say.
#                            The second half is what catches leadership that never
#                            settles, which a single poll cannot see.
#   2. Redirect is usable — a follower's refusal names the leader's SCHEDULER port,
#                          and dialling that endpoint works. This is the defect the
#                          two-endpoint member record exists to close: while one
#                          address was recorded it was the consensus port, so every
#                          redirect pointed a client at a listener that has never
#                          heard of the scheduler protocol. The assertion is not
#                          "a message was produced" but "the address in it answers".
#   3. Settings replicate — a setting changed through the leader is visible from
#                          every member afterwards, which is the whole point of the
#                          log carrying configuration at all.
#   4. Membership replicates — a member removed through the leader is gone from
#                          every member's view.
#   5. Leadership survives — the cluster elects a new leader after the first one is
#                          stopped, and the survivors agree on who it is. A cluster
#                          that formed once and could not re-form is one that works
#                          until the first reboot.
#
# Ports are allocated per run rather than fixed: this fixture needs six of them,
# and six more fixed ports is six more ways to collide with whatever else a CI
# runner is doing — a failure that reads as "consensus is broken" when it means
# "something else was listening".
#
# Usage:
#   cluster-e2e.sh --node <path>
#
# Exit codes: 0 = all assertions held; 1 = a failure; 77 = a prerequisite was
# missing (skip).
set -euo pipefail

node=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --node) node="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

[[ -n "$node" && -x "$node" ]] || { echo "fastcache-compile-node not found: '$node'; skipping"; exit "$SKIP"; }

workdir="$(mktemp -d)"
pids=()
cleanup() {
    # SIGTERM first, then SIGKILL after a grace period -- never a bare `wait`.
    # These processes handle SIGTERM, so a bug that stops one from finishing its
    # shutdown would hang cleanup forever, and cleanup runs on EVERY exit path
    # including the failing ones. That turns "one assertion failed" into "the suite
    # timed out with no output", which this repository has already paid for once.
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

# Every node's log, on the way out of any failure.
#
# The alternative is what this fixture shipped with: one line naming the
# assertion, and nothing at all about what the three processes were doing. A
# consensus defect that reproduces intermittently is diagnosable from the logs or
# it is not diagnosable at all, and the logs are gone the moment cleanup runs.
dump_logs() {
    for log in ${node_logs+"${node_logs[@]}"}; do
        [[ -n "$log" && -r "$log" ]] || continue
        { echo "--- $log"; cat "$log"; } >&2
    done
}

fail() { dump_logs; echo "cluster E2E FAILED: $*" >&2; exit 1; }

export XDG_STATE_HOME="${workdir}/state"

# --- helpers ----------------------------------------------------------------

# Find a port nothing is listening on.
#
# A connect probe, not a bind probe: bind-then-close leaves the port in TIME_WAIT
# on some systems, and the caller is about to hand it to a *different* process
# anyway, so the only question this can honestly answer is "is anything answering
# here right now".
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

# Block until something answers on a port, or the process behind it dies.
wait_for_port() {
    local port="$1" pid="$2" what="$3"
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then
            fail "${what} exited before it started listening"
        fi
        sleep 0.2
    done
    fail "${what} never listened on port ${port}"
}

# Ask one node a cluster question. Echoes its output; never fails the script.
#
# `|| true` because a refusal is an ordinary answer here -- a follower saying
# "ask somebody else" is a case this fixture asserts, not an error.
# @param 1 scheduler endpoint
# @param 2.. the cluster flag and its operand
cluster() {
    local endpoint="$1"; shift
    "$node" --scheduler="$endpoint" "$@" 2>&1 || true
}

# --- the cluster ------------------------------------------------------------

raft_ports=()
scheduler_ports=()
node_logs=()

for index in 0 1 2; do
    raft_ports+=("$(free_port)")
    scheduler_ports+=("$(free_port)")
done

peers=()
for index in 0 1 2; do
    peers+=("--raft-peer=n$((index + 1))=127.0.0.1:${raft_ports[$index]}")
done

# `--listen-cache=` turns each node's own cache port OFF. It defaults to
# 127.0.0.1:6674, which is right for the one node per machine a real deployment
# runs and wrong here, where three share a host and would race for it.
start_node() {
    local index="$1"
    local id="n$((index + 1))"
    local log="${workdir}/${id}.log"
    node_logs[index]="$log"

    "$node" \
        --node-id="$id" \
        --listen-raft="127.0.0.1:${raft_ports[$index]}" \
        "${peers[@]}" \
        --cluster-dir="${workdir}/${id}" \
        --listen-scheduler="127.0.0.1:${scheduler_ports[$index]}" \
        --fleet-open \
        --listen-cache= \
        --scheduler="127.0.0.1:${scheduler_ports[$index]}" \
        --toolchain="/bin/sh" \
        --port="$(free_port)" \
        --advertise="127.0.0.1:1" \
        --log-level=info \
        > "$log" 2>&1 &
    pids+=("$!")
    wait_for_port "${scheduler_ports[$index]}" "$!" "${id}"
}

for index in 0 1 2; do
    start_node "$index"
done

# The leader's scheduler endpoint, as this fixture will use it.
#
# Discovered by ASKING rather than by reading a log line: what a client can act on
# is what a node answers, and a fixture that scraped a log would keep passing if
# the answer stopped matching it.
#
# Bounded. An election takes a few hundred milliseconds and a cold CI runner takes
# longer; a cluster that has not settled inside ~30s has not settled.
leader_endpoint=""
find_leader() {
    leader_endpoint=""
    local index answer
    for _ in $(seq 1 150); do
        for index in "${!scheduler_ports[@]}"; do
            [[ -n "${scheduler_ports[$index]}" ]] || continue
            answer="$(cluster "127.0.0.1:${scheduler_ports[$index]}" --cluster-status)"
            if [[ "$answer" == *"known settings:"* ]]; then
                leader_endpoint="127.0.0.1:${scheduler_ports[$index]}"
                return 0
            fi
        done
        sleep 0.2
    done
    fail "no node ever answered a cluster question; the cluster never elected a leader"
}

find_leader
echo "cluster E2E: leader answers at ${leader_endpoint}"

# --- 1. exactly one leader, and it stays -------------------------------------

# Asked repeatedly over a few seconds rather than once, and the difference is the
# whole assertion. A cluster that re-elects on a timer has exactly one leader at
# almost every instant -- two only in the window where a deposed leader has not
# yet heard from its successor -- so a single poll passes against leadership that
# never settles, and the two-leader window shows up later as some other assertion
# failing for no visible reason. That is exactly how the driver's stale sleep
# reached CI: `find_leader` and one count both passed, and the follower check two
# steps later found a second leader. With all three processes alive and nothing
# else wrong, a healthy cluster elects once and never moves.
for round in $(seq 1 15); do
    answered=0
    who=""
    for index in 0 1 2; do
        [[ -n "${scheduler_ports[$index]}" ]] || continue
        endpoint="127.0.0.1:${scheduler_ports[$index]}"
        if [[ "$(cluster "$endpoint" --cluster-status)" == *"known settings:"* ]]; then
            answered=$(( answered + 1 ))
            who="$endpoint"
        fi
    done
    [[ "$answered" -eq 1 ]] ||
        fail "round ${round}: expected exactly one node to answer, got ${answered}"
    [[ "$who" == "$leader_endpoint" ]] ||
        fail "round ${round}: leadership moved from ${leader_endpoint} to ${who}, all three alive"
    sleep 0.2
done
echo "cluster E2E: exactly one node answers, and keeps answering"

# --- 2. a follower's redirect names an endpoint that works -------------------

# The defect the two-endpoint member record closes. While one address was
# recorded it was the CONSENSUS port, so a client that followed the advice spoke
# the scheduler protocol at a listener that has never heard of it. Asserting the
# message alone would have passed throughout.
#
# Bounded, not immediate: a leader announces its own record once it is elected, so
# the entry carrying its scheduler endpoint commits strictly after the election
# that provoked it, and a follower asked in between has nowhere to send anybody.
# That window is a real state — it is what `NotLeader` with no endpoint means —
# and waiting it out is what tells "not yet" apart from "never".
redirect_from() {
    local endpoint="$1" answer=""
    for _ in $(seq 1 100); do
        answer="$(cluster "$endpoint" --cluster-status)"
        case "$answer" in
            *"ask --scheduler="*)
                printf '%s' "$answer" | sed -n 's/.*--scheduler=\([^ ]*\) instead.*/\1/p'
                return 0
                ;;
            *"known settings:"*)
                fail "a follower at ${endpoint} answered as though it led"
                ;;
        esac
        sleep 0.2
    done
    fail "a follower at ${endpoint} never named where to ask: ${answer}"
}

for index in 0 1 2; do
    endpoint="127.0.0.1:${scheduler_ports[$index]}"
    [[ "$endpoint" == "$leader_endpoint" ]] && continue

    redirect="$(redirect_from "$endpoint")"
    [[ "$redirect" == "$leader_endpoint" ]] \
        || fail "a follower named ${redirect}, but the leader answers at ${leader_endpoint}"

    # And the named endpoint actually answers, which is the half a message check
    # cannot reach: while one address was recorded it was the consensus port, and a
    # message assertion would have been satisfied by it.
    answer="$(cluster "$redirect" --cluster-status)"
    [[ "$answer" == *"known settings:"* ]] || fail "the endpoint a follower named did not answer: ${answer}"
done
echo "cluster E2E: a follower redirects to an endpoint that answers"

# --- 3. a setting reaches every member ---------------------------------------

answer="$(cluster "$leader_endpoint" --cluster-set=upstream=cache.example:6674)"
[[ "$answer" == *"accepted"* ]] || fail "the leader refused a legitimate setting: ${answer}"

# Replication is asynchronous, so this is a bounded wait rather than an immediate
# assertion -- and bounded rather than unbounded, so a cluster that never
# replicates fails saying so instead of timing out the suite.
settled=0
for _ in $(seq 1 100); do
    answer="$(cluster "$leader_endpoint" --cluster-status)"
    [[ "$answer" == *"cache.example:6674"* ]] && { settled=1; break; }
    sleep 0.2
done
[[ "$settled" -eq 1 ]] || fail "a setting the leader accepted never appeared in its own state"
echo "cluster E2E: a setting replicates"

# A setting nobody has heard of is refused where the operator is watching, rather
# than replicated to every node and quietly doing nothing.
answer="$(cluster "$leader_endpoint" --cluster-set=upsteam=typo)"
[[ "$answer" == *"upsteam"* ]] || fail "a typo'd setting was not refused by name: ${answer}"
echo "cluster E2E: an unknown setting is refused by name"

# --- 4. a machine joins the running cluster ----------------------------------

# The property issue #97 is about, and it is only true end to end: the harness can
# admit a node into a simulated cluster, and what it cannot reach is the wire, the
# transport, the peer table and the operator's command line all having their first
# say at once.
#
# n4 starts with `--raft-join`, which changes what its `--raft-peer` list MEANS:
# these are nodes it can reach, not a cluster it belongs to. That is the only
# shape a cluster can admit -- without it a node bootstraps a cluster of itself,
# elects itself, and afterwards refuses `AppendEntries` from every leader its own
# configuration does not name, so the cluster that admitted it would count towards
# quorum a node that answers nobody.
#
# It is given the cluster's addresses as well as its own, and that is load-bearing
# rather than convenient: the leader starts replicating at its own last index, an
# empty log refuses that, and the leader only walks back to the beginning when the
# refusal reaches it. A joiner that could not send one was admitted, dialled, and
# permanently silent -- which is what this case found the first time it was run.
raft_ports+=("$(free_port)")
scheduler_ports+=("$(free_port)")

"$node" \
    --node-id=n4 \
    --raft-join \
    --listen-raft="127.0.0.1:${raft_ports[3]}" \
    --raft-peer="n4=127.0.0.1:${raft_ports[3]}" "${peers[@]}" \
    --cluster-dir="${workdir}/n4" \
    --listen-scheduler="127.0.0.1:${scheduler_ports[3]}" \
    --fleet-open \
    --listen-cache= \
    --scheduler="127.0.0.1:${scheduler_ports[3]}" \
    --toolchain="/bin/sh" \
    --port="$(free_port)" \
    --advertise="127.0.0.1:1" \
    --log-level=info \
    > "${workdir}/n4.log" 2>&1 &
pids+=("$!")
node_logs[3]="${workdir}/n4.log"
wait_for_port "${scheduler_ports[3]}" "$!" "n4"

# It is running and it leads nothing, which is the first half of the property: a
# node waiting to be admitted must not have formed a cluster of its own. Asked of
# the node itself, because "no cluster" is exactly what it should answer.
answer="$(cluster "127.0.0.1:${scheduler_ports[3]}" --cluster-status)"
[[ "$answer" != *"known settings:"* ]] || fail "a joining node answered as a leader; it bootstrapped its own cluster"
echo "cluster E2E: a joining node leads nothing"

answer="$(cluster "$leader_endpoint" --cluster-admit="n4=127.0.0.1:${raft_ports[3]}")"
[[ "$answer" == *"accepted"* ]] || fail "the leader refused to admit a member: ${answer}"

# Admission is two steps and this waits for the second. The record commits first,
# which is what teaches every node where n4 answers; only then does the leader
# propose counting it, because a member counted towards a quorum before anything
# can dial it is a cluster that stops forming one.
#
# Asserted on n4 rather than on the leader, and by ASKING rather than by reading a
# log: the leader accepting a command proves nothing about the machine it names.
#
# The question put to n4 is `--cluster-status`, and its REFUSAL is the assertion --
# a follower answers "ask that endpoint instead", and the endpoint comes from the
# replicated state. Which makes one answer carry the whole property:
#
#   * n4 knows who leads and where its scheduler answers, and neither is on its
#     command line, so it can only have been replicated to;
#   * a leader replicates only to members of its CONFIGURATION, so n4 receiving
#     anything at all is n4 being counted -- which is the half issue #97 is about
#     and the half `--cluster-status` on the leader could never show, since that
#     reports the fleet's member set rather than the quorum.
joined=0
for _ in $(seq 1 150); do
    answer="$(cluster "127.0.0.1:${scheduler_ports[3]}" --cluster-status)"
    [[ "$answer" == *"$leader_endpoint"* ]] && { joined=1; break; }
    sleep 0.2
done
[[ "$joined" -eq 1 ]] ||
    fail "the admitted node never learned who leads, so it was never replicated to: ${answer}"
echo "cluster E2E: an admitted node is replicated to, which is being counted"

# --- 5. a member can be removed ----------------------------------------------

answer="$(cluster "$leader_endpoint" --cluster-forget=n3)"
[[ "$answer" == *"accepted"* ]] || fail "the leader refused to forget a member: ${answer}"
echo "cluster E2E: a member can be removed"

# --- 6. leadership survives losing the leader --------------------------------

# A cluster that formed once and could not re-form is one that works until the
# first reboot. Three of four remain, which is still a majority.
for index in 0 1 2 3; do
    if [[ "127.0.0.1:${scheduler_ports[$index]}" == "$leader_endpoint" ]]; then
        kill "${pids[$index]}" >/dev/null 2>&1 || true
        for _ in $(seq 1 75); do
            kill -0 "${pids[$index]}" 2>/dev/null || break
            sleep 0.2
        done
        kill -0 "${pids[$index]}" 2>/dev/null && fail "the leader did not exit within 15s of being asked to stop"

        # The exit STATUS, not merely the exit. It used to be discarded with
        # `|| true`, which threw away the one signal that makes a whole class of
        # defect visible: under a sanitizer build a coroutine frame nobody freed --
        # the failure mode every part of the reactor migration is shaped to avoid --
        # is reported by a non-zero exit and by nothing else. A node asked to stop
        # must also stop CLEANLY.
        leader_status=0
        wait "${pids[$index]}" || leader_status=$?
        # 143 is SIGTERM, which is how it was asked to stop.
        if [[ $leader_status -ne 0 && $leader_status -ne 143 ]]; then
            fail "the leader exited with status $leader_status after SIGTERM"
        fi
        scheduler_ports[$index]=""
        break
    fi
done

find_leader
echo "cluster E2E: a new leader is elected at ${leader_endpoint}"

echo "cluster E2E: OK"
