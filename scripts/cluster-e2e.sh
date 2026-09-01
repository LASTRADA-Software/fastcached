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
#
#                            Asserted only once the cluster is FORMED, which is a
#                            stronger fact than "a leader exists" and the one this
#                            property is actually true of. A cluster still waiting
#                            for its last member re-elects on any hiccup by design
#                            — see `wait_for_formation`, and issue #117 for the
#                            three CI runs spent proving the algorithm right.
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

# The cluster's pre-shared key, which every node here shares.
#
# Not decoration and not a workaround for the startup rule: these nodes leave
# `--listen-node` at the wildcard and say `--fleet-open`, so another machine genuinely
# could dial their compile ports, and a node in that shape has to be able to check
# the lease a client presents it (#282). Giving them the key is what a real fleet in
# this shape does, and it means these fixtures exercise the SIGNING scheduler and
# the VERIFYING worker rather than the unchecked pair.
#
# Fixed text rather than /dev/urandom: what these scripts assert has nothing to do
# with the key's value, and a per-run secret would make a failure look like a flake.
# Sixteen bytes is the minimum `ReadClusterKey` accepts.
cluster_key="${workdir}/cluster.key"
printf 'e2e-fixture-cluster-key-not-a-secret\n' > "$cluster_key"
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

# The shared helpers: `fail`, `free_port` and `wait_for_port`, one copy for every
# POSIX fixture (#449). The reasoning that used to be repeated here -- why a
# connect probe rather than a bind probe, why the issued-port ledger is a FILE,
# and why the range stops below the kernel's ephemeral range -- is above
# `free_port` in `lib/e2e-common.sh`, in full rather than in each fixture's own
# abbreviation of it.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "cluster E2E" "$workdir"

# Every failure dumps every node's log first. A consensus defect that reproduces
# intermittently is diagnosable from the logs or it is not diagnosable at all,
# and cleanup takes them away -- so this has to run before the run ends rather
# than at the call site that noticed.
e2e_on_fail dump_logs

export XDG_STATE_HOME="${workdir}/state"

# --- helpers ----------------------------------------------------------------

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

# `--cache-memory=0` turns each node's own cache tier OFF -- the node port stays
# open, because the scheduler verbs are answered on it too since #290. It defaults to
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
        --serve-scheduler \
        --listen-node="127.0.0.1:${scheduler_ports[$index]}" \
        --fleet-open \
        --cluster-key-file="$cluster_key" \
        --cache-memory=0 \
        --scheduler="127.0.0.1:${scheduler_ports[$index]}" \
        --toolchain="/bin/sh" \
        --advertise="127.0.0.1:1" \
        --log-level=info \
        > "$log" 2>&1 &
    pids+=("$!")
    wait_for_port 127.0.0.1 "${scheduler_ports[$index]}" "$!" "${id}" "$log"
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

# @param $1 What was being waited for, named by the caller.
#
# The description is a parameter because this is called in two situations that
# fail for different reasons, and a shared sentence describing only the first one
# sends the next reader to the wrong place. #388's phase 6 failure reported "the
# cluster never elected a leader" -- after eight assertions had passed, one of
# which printed the endpoint it was led from. The cluster had elected perfectly
# well; what it could not do was elect AGAIN, and that is a different defect with
# a different cause. Half an hour went into re-reading logs against a sentence
# that was false.
#
# It also prints who was asked and what they said. "Nobody answered" is not a
# finding, it is the absence of one -- the finding is in the refusals, which name
# the endpoint each node believes leads.
find_leader() {
    local what="${1:-a leader}"
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

    echo "no live node answered as leader within 30s, waiting for ${what}. What each was asked and said:"
    for index in "${!scheduler_ports[@]}"; do
        [[ -n "${scheduler_ports[$index]}" ]] || { echo "  slot ${index}: stopped by this fixture"; continue; }
        echo "  127.0.0.1:${scheduler_ports[$index]}: $(cluster "127.0.0.1:${scheduler_ports[$index]}" --cluster-status)"
    done
    fail "no live node answered as leader, waiting for ${what}"
}

# The endpoint a refusal tells a client to ask instead, or empty when it names
# none.
#
# One spelling of the grammar rather than three. The three callers below ask the
# same question of the same answer -- is the cluster formed, does a follower's
# redirect work, has the admitted node been replicated to -- and a parse that
# drifted in one of them would make that caller quietly stop matching while still
# looking like it asserted something.
#
# Empty is a real answer rather than a parse failure: `NotLeader` with no address
# is what an election in progress looks like, and every caller has to tell that
# apart from an address it can dial.
# @param 1 the answer to read
named_endpoint() {
    printf '%s' "$1" | sed -n 's/.*--scheduler=\([^ ]*\) instead.*/\1/p'
}

# Block until the cluster is FORMED, which is a stronger fact than "a leader
# exists" and the one every assertion below actually rests on (issue #117).
#
# A three-node cluster whose third process is still connecting is running on a
# bare two-of-three quorum, and there the leader's CheckQuorum test degenerates
# into "has that ONE follower answered inside electionTimeoutMin". A follower
# only campaigns after its own randomized timeout -- drawn from
# [electionTimeoutMin, electionTimeoutMax] -- has elapsed with no contact, so by
# the time it asks, the leader's evidence about it is necessarily at least that
# old and therefore always stale. The leader grants, and the cluster re-elects.
# Pre-vote refuses nothing in that window, structurally rather than occasionally,
# and no fix belongs in the algorithm: a leader that refused anyway would be a
# partitioned leader vetoing its own replacement forever.
#
# So "elects once and never moves" is true of a formed cluster and false of one
# that has merely elected -- and `find_leader` returns the instant ANY node
# answers, which is the weaker fact. Asserting stability from there is what put
# `round 5: expected exactly one node to answer, got 0` into CI against an
# algorithm that was behaving exactly as specified.
#
# Formed is asked for, not scraped, for the reason `find_leader` gives: one node
# answers as leader and the other two REDIRECT to that same endpoint. A follower
# can only name it once it has taken an AppendEntries from that leader and the
# leader's own record has committed -- which also means the leader is holding
# that follower's answer, so its quorum has slack again. Waiting for a log line
# would prove less and would keep passing if the answer stopped matching it.
#
# Leadership may legitimately move while this waits, so the endpoint is
# re-derived on every pass rather than checked against the one `find_leader`
# happened to see.
# Over the LIVE slots, honouring the blanked-slot convention `find_leader` uses,
# and counting how many there are rather than assuming three. A stopped node's
# port is blanked, so a hard-coded `0 1 2` would dial `127.0.0.1:` and report a
# cluster that had in fact formed as one that never did -- which is the whole
# failure mode this function exists to stop misdiagnosing.
#
# It subsumes `find_leader`, so it is called INSTEAD of one rather than after it:
# both poll the same nodes for ~30s, and spending two budgets on the failing path
# is 450 extra client spawns and a real chance of hitting the fixture's own
# CTest timeout, which would replace the diagnosis below with an opaque kill.
# The two outcomes it has to tell apart are kept by remembering whether anything
# ever answered as leader at all.
wait_for_formation() {
    local index endpoint answer named led followers live everLed=0
    for _ in $(seq 1 150); do
        led=""
        named=""
        followers=0
        live=0
        for index in "${!scheduler_ports[@]}"; do
            [[ -n "${scheduler_ports[$index]}" ]] || continue
            live=$(( live + 1 ))
            endpoint="127.0.0.1:${scheduler_ports[$index]}"
            answer="$(cluster "$endpoint" --cluster-status)"
            case "$answer" in
                *"known settings:"*)
                    everLed=1
                    # Two at once is a cluster mid-handover rather than a formed
                    # one, so neither is adopted and the pass is abandoned.
                    [[ -z "$led" ]] || { led=""; break; }
                    led="$endpoint"
                    ;;
                *"ask --scheduler="*)
                    followers=$(( followers + 1 ))
                    endpoint="$(named_endpoint "$answer")"
                    if [[ -z "$named" ]]; then
                        named="$endpoint"
                    elif [[ "$named" != "$endpoint" ]]; then
                        named="disagreed"
                    fi
                    ;;
            esac
        done

        if [[ -n "$led" && "$followers" -eq $(( live - 1 )) && "$named" == "$led" ]]; then
            leader_endpoint="$led"
            return 0
        fi
        sleep 0.2
    done

    # Two different faults, and telling them apart is most of the value: nothing
    # ever led at all, or something led and the rest never came to name it.
    [[ "$everLed" -eq 1 ]] ||
        fail "no node ever answered a cluster question; the cluster never elected a leader"
    fail "the cluster elected but never formed: one leader and every other node naming it never held at once"
}

wait_for_formation
echo "cluster E2E: the cluster is formed, and led from ${leader_endpoint}"

# --- 1. exactly one leader, and it stays -------------------------------------

# Asked repeatedly over a few seconds rather than once, and the difference is the
# whole assertion. A cluster that re-elects on a timer has exactly one leader at
# almost every instant -- two only in the window where a deposed leader has not
# yet heard from its successor -- so a single poll passes against leadership that
# never settles, and the two-leader window shows up later as some other assertion
# failing for no visible reason. That is exactly how the driver's stale sleep
# reached CI: `find_leader` and one count both passed, and the follower check two
# steps later found a second leader.
#
# The precondition above is what makes this legitimate to assert at all: a FORMED
# cluster with all three processes alive and nothing else wrong elects once and
# never moves. One that has merely elected re-elects on any hiccup, by design --
# see `wait_for_formation`.
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
                named_endpoint "$answer"
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

# Submit a setting to whoever leads NOW, rather than to whoever led when section 1
# ran.
#
# `$leader_endpoint` was pinned there, and leadership may legitimately move before
# this section finishes -- a slow enough runner blows any election timeout. Pinned,
# BOTH halves of the old assertion were wrong: the submission went to a node that
# answers "ask somebody else", and the poll below then waited out its full 20s to
# report that a setting the leader accepted never appeared in its own state -- a
# sentence about replication, produced by an election. That is the message issue
# #117's second occurrence actually carried.
#
# Retried on ANY answer that is not the one the caller is asserting, rather than on
# a recognised "not the leader" refusal: that refusal has two spellings, one for
# "somebody else leads" and one for "an election is in progress", and a fixture
# that matched them would stop retrying the day either sentence is reworded --
# silently, and back to failing the way this section used to.
# @param 1 the `--cluster-set` argument to submit
# @param 2 the substring an answer carries when the submission worked
# @param 3 what to report when it never does
submit_setting() {
    local answer
    answer="$(cluster "$leader_endpoint" --cluster-set="$1")"
    if [[ "$answer" != *"$2"* ]]; then
        find_leader "whoever leads now, to re-offer a setting the previous leader did not take"
        answer="$(cluster "$leader_endpoint" --cluster-set="$1")"
    fi
    [[ "$answer" == *"$2"* ]] || fail "$3 (asked ${leader_endpoint}): ${answer}"
}

# The one submission this section makes, named once so the poll below re-offers
# the same setting rather than a second one that drifted from it.
set_upstream() {
    submit_setting "upstream=cache.example:6674" "accepted" \
        "the leader refused a legitimate setting"
}

set_upstream

# Replication is asynchronous, so the setting becomes visible a moment after it is
# accepted. Bounded rather than unbounded, so a cluster that never replicates
# fails saying so instead of timing out the suite -- and re-submitted rather than
# only waited on.
#
# Re-deriving the leader would not be enough on its own. A proposal accepted by a
# leader that is then deposed may legitimately never commit -- Raft promises
# nothing about an uncommitted entry across a term change -- so polling forever
# for it asserts something the algorithm does not offer. The honest property is
# that a setting a client SUCCESSFULLY sets becomes visible, and a client gets
# that by asking again. Which is what any real operator tool would do, so it is
# also the behaviour worth having under test.
settled=0
for _ in $(seq 1 100); do
    answer="$(cluster "$leader_endpoint" --cluster-status)"
    if [[ "$answer" == *"cache.example:6674"* ]]; then
        settled=1
        break
    fi

    # Not visible yet. Either replication is still in flight -- ordinary, wait --
    # or this node no longer leads, in which case the setting may have died with
    # its term and has to be offered to whoever leads now.
    if [[ "$answer" != *"known settings:"* ]]; then
        find_leader "whoever leads now, to re-offer a setting that may have died with its term"
        set_upstream
    fi
    sleep 0.2
done
[[ "$settled" -eq 1 ]] || fail "a setting accepted by the leader never became visible on it"
echo "cluster E2E: a setting replicates"

# A setting nobody has heard of is refused where the operator is watching, rather
# than replicated to every node and quietly doing nothing.
#
# Against whoever leads NOW for the same reason: a follower refuses this with "ask
# somebody else" and never names the typo, so an election here would fail it on a
# point it does not test.
submit_setting "upsteam=typo" "upsteam" "a typo'd setting was not refused by name"
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
    --serve-scheduler \
    --listen-node="127.0.0.1:${scheduler_ports[3]}" \
    --fleet-open \
    --cluster-key-file="$cluster_key" \
    --cache-memory=0 \
    --scheduler="127.0.0.1:${scheduler_ports[3]}" \
    --toolchain="/bin/sh" \
    --advertise="127.0.0.1:1" \
    --log-level=info \
    > "${workdir}/n4.log" 2>&1 &
pids+=("$!")
node_logs[3]="${workdir}/n4.log"
wait_for_port 127.0.0.1 "${scheduler_ports[3]}" "$!" "n4" "${workdir}/n4.log"

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
#
# What is NOT asserted is *which* node leads, and that is deliberate for the
# reason `wait_for_formation` exists at all: admission puts the cluster back into
# the state that has no slack -- the quorum grows to three of four while n4 is
# still attaching -- so a re-election here is legitimate. Pinning the endpoint
# recorded before admission would fail this bounded wait after 30s with "never
# learned who leads" for a node that had learned perfectly well, which is issue
# #117 one section later. So the endpoint n4 names is taken from n4, and then
# checked to be a leader by asking it.
#
# And n4 WINNING that election proves the same property more directly, so it is
# accepted too: leading means a quorum of the configuration voted for it, which a
# node outside the configuration cannot obtain and a node that bootstrapped alone
# could not have won against the others. Reading only its redirect would tolerate
# exactly one of the two legitimate outcomes and hang out the full 30s on the
# other -- the same shape of precondition error, and it would have been a hard one
# to see, because n4 winning here is rare.
joined=0
for _ in $(seq 1 150); do
    answer="$(cluster "127.0.0.1:${scheduler_ports[3]}" --cluster-status)"
    named="$(named_endpoint "$answer")"
    if [[ "$answer" == *"known settings:"* ]]; then
        leader_endpoint="127.0.0.1:${scheduler_ports[3]}"
        joined=1
        break
    fi
    if [[ -n "$named" && "$(cluster "$named" --cluster-status)" == *"known settings:"* ]]; then
        leader_endpoint="$named"
        joined=1
        break
    fi
    sleep 0.2
done
[[ "$joined" -eq 1 ]] ||
    fail "the admitted node never learned who leads, so it was never replicated to: ${answer}"
echo "cluster E2E: an admitted node is replicated to, which is being counted, and it names ${leader_endpoint}"

# And that n4 counts ITSELF a member, which is a different fact from either half
# above and is the one #388 turns on.
#
# Admission is two steps: the ClusterState record commits first, then the leader
# proposes counting the node. Everything asserted above is satisfied by the FIRST
# step alone -- a node that received the record knows who leads and will redirect
# to it, and the leader replicates to it either way. A node that never adopted the
# CONFIGURATION looks identical from outside, and is fatal: `HasCluster()` is false,
# so `NextDeadline()` answers `TimePoint::max()`, so it campaigns in no election and
# grants no pre-vote. The cluster then cannot re-elect once one more member goes.
#
# Asked of n4's log rather than over the wire, and that is a compromise rather than
# a preference -- `wait_for_formation` explains why asking beats scraping. There is
# no surface that answers it: `--cluster-status` reports the FLEET's member record,
# not the quorum, and only a leader answers it at all, so the one node whose view is
# needed is the one that redirects. #435 is that surface; until it exists this line
# is what an operator has too.
adopted=0
for _ in $(seq 1 150); do
    if grep -q "consensus: this node counts [0-9]* member(s)" "${workdir}/n4.log" 2>/dev/null; then
        adopted=1
        break
    fi
    sleep 0.2
done
if [[ "$adopted" -ne 1 ]]; then
    echo "n4 never adopted a configuration. What it last said about its own quorum:"
    grep -E "consensus: this node counts" "${workdir}/n4.log" 2>/dev/null || echo "  (nothing -- it never reported one at all)"
    fail "the admitted node never counted itself a member, so it can vote in no election"
fi
echo "cluster E2E: the admitted node counts itself a member: $(grep -o "counts [0-9]* member(s)" "${workdir}/n4.log" | tail -1)"

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

find_leader "a new leader after the old one was stopped"
echo "cluster E2E: a new leader is elected at ${leader_endpoint}"

echo "cluster E2E: OK"
