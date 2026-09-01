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
# Not decoration: these nodes say `--fleet-open`, so their compile verbs admit every
# caller that can reach the port, and a node in that shape has to be able to check the
# lease a client presents it (#282). Giving them the key is what a real fleet in this
# shape does, and it means these fixtures exercise the SIGNING scheduler and the
# VERIFYING worker rather than the unchecked pair.
#
# The binds are loopback, which is the OTHER half of #282's rule and is why the key is
# a choice here rather than a requirement: either a loopback bind or a loopback-only
# policy closes the port on its own, and these nodes have the first and deliberately
# not the second. This paragraph used to say they left the bind at the wildcard, which
# was true of the flag it named (`--bind`, defaulting to 0.0.0.0) and became false when
# #290 stage 3 replaced it with `--listen-node=127.0.0.1:<port>` -- a mechanical flag
# rename carried a claim about behaviour with it.
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

# How long ONE cluster probe may take before it is abandoned.
#
# **5 s, and the number is a measurement with its conditions attached rather than
# a round figure.** All three readings below are from the ASan build, which is the
# configuration this fixture fails in, on an otherwise idle machine:
#
#   a live node that answers          16 ms   (74 probes, 1.18 s, one healthy run)
#   a port nothing listens on        ~900 ms
#   a port that accepts, never replies ~12.7 s (`ClusterAdminCli`'s own 10 s
#                                               `DialTimeout`, plus startup)
#
# The bound is chosen off the HEALTHY cost with a very wide margin, never off the
# pathological one. At 5 s a node taking three hundred times its measured time is
# still served, and the only case cut short is the one that would otherwise cost
# 12.7 s. A node that cannot answer a status query in five seconds is not a slow
# node, it is a finding.
#
# **Why bound it at all.** The waits below poll, and an unbounded probe lets ONE
# unhealthy node eat a whole wait's budget: 150 passes x 12.7 s is thirty-one
# minutes against this fixture's 300 s CTest timeout, so the run is killed from
# outside and the failure arrives as a status with no diagnosis. That is #457's
# actual mechanism -- not the probe COUNT, which is 74 on the healthy path and
# 11% of the run.
#
# A third row is what makes the second column dangerous: a node that is merely
# slow under a loaded runner presents exactly as "accepts, never replies", because
# its listener is bound -- `wait_for_port` has already passed -- while the process
# is too starved to answer inside the client's window. So the probe's cost rises
# by three orders of magnitude precisely when the machine is busy, and each slow
# probe is itself another sanitizer-instrumented process on the same cores.
readonly ClusterProbeSeconds=5

# What the waits below are allowed, in SECONDS of wall clock.
#
# Seconds and not passes, which is the other half of #457. `for _ in $(seq 1 150)`
# with a `sleep 0.2` reads as thirty seconds and enforces no such thing: each pass
# also pays for its probes, so the real bound was 30 s plus however long asking
# took -- unbounded, and largest exactly when the answers were slowest. A failing
# run measured 52.04 s against a loop whose message said 30 s, and the pathological
# case ran to thirty-one minutes. The number in the source is now the quantity
# enforced, which is the rule `.agent/rules/testing.md` states for #452.
#
# Generous rather than tight: these are deadlock bounds, not performance budgets. A
# healthy cluster forms in well under a second and every wait here exits on its
# first or second pass.
readonly FormationSeconds=60
readonly RedirectSeconds=30
readonly ReplicationSeconds=30
readonly JoinSeconds=60


# What a probe that did not finish says, in the output stream callers already read.
#
# Deliberately matches none of the patterns the callers test for, so a caller that
# only looks at the text treats it as "not the answer I wanted" -- the safe
# default -- while the sentence still appears verbatim in any failure dump.
readonly ProbeTimedOut="probe did not finish within ${ClusterProbeSeconds}s"

# Every probe's outcome, appended as it happens.
#
# A FILE and not shell variables, and that is not a style preference: every caller
# invokes `cluster` inside `$( ... )`, which is a subshell, so a counter
# incremented in the function is discarded at the closing paren. Silently -- the
# summary would report zero of everything, which is the exact shape of defect this
# fixture exists to catch in the product.
probe_log="${workdir}/probes"
: > "$probe_log"

# Ask one node a cluster question. Echoes its output; never fails the script.
#
# **THREE outcomes, and the third one is the point of #457.** A probe can answer,
# fail to reach anybody, or NOT FINISH -- and the last is not the cluster saying
# "no", it is this fixture failing to ask. Folded into a negative it would make
# the caller retry, exhaust its budget and report "the cluster never formed" about
# a cluster that was answering in six seconds. That is precisely the distinction
# `.agent/rules/testing.md` requires a wait to be able to make, and precisely the
# one every occurrence of this flake has turned on.
#
# `|| true` is gone rather than kept: a refusal is still an ordinary answer and
# still does not fail the run, but its status is now READ instead of discarded,
# which is what lets the three outcomes be told apart at all.
# @param 1 scheduler endpoint
# @param 2.. the cluster flag and its operand
cluster() {
    local endpoint="$1"; shift
    local out rc=0
    out="$(timeout "$ClusterProbeSeconds" "$node" --scheduler="$endpoint" "$@" 2>&1)" || rc=$?

    # 124 is `timeout` reporting that it killed the probe. Checked before anything
    # else, because every other non-zero status is the CLI having run and said
    # something.
    if [[ "$rc" -eq 124 ]]; then
        echo "timeout ${endpoint}" >> "$probe_log"
        printf '%s\n' "$ProbeTimedOut"
        return 0
    fi

    # A non-zero status is an ANSWER, not a failure to reach anybody, and calling
    # it one would put a confident wrong number in the summary below. Measured on a
    # healthy run: 47 of 74 probes exit non-zero, and every one of them is the
    # cluster talking -- 34 "this node does not lead the cluster", 12 "the cluster
    # has no leader right now", 1 "rejected (invalid-cluster-change)". All three are
    # states this fixture asserts.
    #
    # What the status cannot tell apart is a follower's redirect from a genuinely
    # refused connection: both are rc=2, and only the TEXT separates them. So this
    # counts what it can defend -- the probe finished, or it did not -- and does not
    # invent a reachability reading it has no evidence for.
    if [[ "$rc" -ne 0 ]]; then
        echo "declined ${endpoint}" >> "$probe_log"
    else
        echo "affirmed ${endpoint}" >> "$probe_log"
    fi
    printf '%s\n' "$out"
    return 0
}

# One line saying how the probes went, for a failure message to carry.
#
# The counts are what separate "this runner was too slow to ask" from "the cluster
# never formed": a wait that expired having timed out most of its probes has not
# observed the cluster at all, and saying "never formed" about it would be the
# confident wrong sentence this fixture has produced before.
#
# Read with one `awk` pass rather than three `grep -c` calls: `grep -c` exits 1 on
# no match, which under `set -e` and `pipefail` turns an honest zero into an
# aborted script.
probe_summary() {
    awk '
        { kind[$1]++; total++ }
        END {
            printf "%d probes: %d affirmed, %d declined, %d TIMED OUT",
                   total, kind["affirmed"], kind["declined"], kind["timeout"]
            if (kind["timeout"] > 0 && total > 0)
                printf " (%.0f%% never finished, so this wait spent its budget asking rather than observing)",
                       kind["timeout"] * 100 / total
        }
    ' "$probe_log"
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
    local deadline=$(( SECONDS + FormationSeconds ))
    while [[ "$SECONDS" -lt "$deadline" ]]; do
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

    echo "no live node answered as leader within ${FormationSeconds}s, waiting for ${what}."
    echo "  $(probe_summary)"
    echo "  What each was asked and said:"
    for index in "${!scheduler_ports[@]}"; do
        [[ -n "${scheduler_ports[$index]}" ]] || { echo "  slot ${index}: stopped by this fixture"; continue; }
        echo "  127.0.0.1:${scheduler_ports[$index]}: $(cluster "127.0.0.1:${scheduler_ports[$index]}" --cluster-status)"
    done
    fail "no live node answered as leader, waiting for ${what} ($(probe_summary))"
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

    # What the DECLINES said, which is the evidence this wait used to throw away.
    #
    # A node that is not leading answers `ask --scheduler=<endpoint>` -- it names
    # who it believes leads. Across a whole failing wait that is the difference
    # between three distinguishable states, and until #457's first CI run this
    # function reported only two of them:
    #
    #   nothing named, ever      nobody knew who led; a genuine non-formation
    #   one endpoint, named early a leader was stably known and never affirmed,
    #                             which is a finding about the PRODUCT rather than
    #                             about this fixture's budget
    #   one endpoint, named late  the election really did happen near the deadline,
    #                             and the budget is what was short
    #
    # The third state is the one a bare "never formed" hides, and it is the one
    # that decides whether raising a number is a fix or a cover-up. `at` is seconds
    # from the start of THIS wait, so the reader gets it without arithmetic.
    local firstNamed="" firstNamedAt="" lastNamed="" namedRuns=0
    local started="$SECONDS"

    local deadline=$(( SECONDS + FormationSeconds ))
    while [[ "$SECONDS" -lt "$deadline" ]]; do
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

        # Recorded before the exit test, so a wait that succeeds on its first pass
        # has still observed what it saw -- and recorded on CHANGE rather than every
        # pass, so `namedRuns` counts how often the answer moved rather than how
        # often it was asked.
        if [[ -n "$named" && "$named" != "$lastNamed" ]]; then
            if [[ -z "$firstNamed" ]]; then
                firstNamed="$named"
                firstNamedAt=$(( SECONDS - started ))
            fi
            namedRuns=$(( namedRuns + 1 ))
            lastNamed="$named"
        fi

        if [[ -n "$led" && "$followers" -eq $(( live - 1 )) && "$named" == "$led" ]]; then
            leader_endpoint="$led"
            return 0
        fi
        sleep 0.2
    done

    # What the declines named, as its own sentence, because it is the evidence that
    # separates the remaining possibilities and it is cheap to state.
    local naming
    if [[ -z "$firstNamed" ]]; then
        naming="no node ever named a leader, so none was known to any of them"
    elif [[ "$namedRuns" -eq 1 ]]; then
        naming="every decline named ${firstNamed}, from ${firstNamedAt}s in and unchanged for the rest of the wait -- so a leader WAS known throughout and never answered as one"
    else
        naming="the named leader moved ${namedRuns} times, first ${firstNamed} at ${firstNamedAt}s, last ${lastNamed} -- the cluster was still re-electing"
    fi

    # Faults, in the order that makes each one answerable.
    #
    # The probe accounting comes first: if the probes did not finish, this fixture
    # never observed the cluster and cannot say what it did, so reporting "never
    # elected" from a run that never asked is the confident wrong sentence #388
    # already cost half an hour to.
    #
    # Then the naming evidence, and it is not decoration. #457's first CI run on
    # macOS produced 570 probes, 0 affirmed, 0 timed out -- and the dumped node logs
    # showed the cluster HAD formed: n2 led term 2 and had committed its own member
    # record naming its scheduler port, with both others following it. So both
    # halves of the old sentence were false at once, and neither this fixture nor a
    # reader could tell. What it could not say, and now can, is whether that leader
    # was known early and silent or known only at the end.
    [[ "$everLed" -eq 1 ]] ||
        fail "no node ever answered as leader in ${FormationSeconds}s: ${naming} ($(probe_summary))"
    fail "the cluster elected but never formed in ${FormationSeconds}s: one leader and every other node naming it never held at once. ${naming} ($(probe_summary))"
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
    local deadline=$(( SECONDS + RedirectSeconds ))
    while [[ "$SECONDS" -lt "$deadline" ]]; do
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
    fail "a follower at ${endpoint} never named where to ask within ${RedirectSeconds}s: ${answer} ($(probe_summary))"
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
replication_deadline=$(( SECONDS + ReplicationSeconds ))
while [[ "$SECONDS" -lt "$replication_deadline" ]]; do
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
join_deadline=$(( SECONDS + JoinSeconds ))
while [[ "$SECONDS" -lt "$join_deadline" ]]; do
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
    fail "the admitted node never learned who leads within ${JoinSeconds}s, so it was never replicated to: ${answer} ($(probe_summary))"
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
adopted_deadline=$(( SECONDS + JoinSeconds ))
while [[ "$SECONDS" -lt "$adopted_deadline" ]]; do
    if grep -q "consensus: this node counts [0-9]* member(s)" "${workdir}/n4.log" 2>/dev/null; then
        adopted=1
        break
    fi
    sleep 0.2
done
if [[ "$adopted" -ne 1 ]]; then
    echo "n4 never adopted a configuration. What it last said about its own quorum:"
    grep -E "consensus: this node counts" "${workdir}/n4.log" 2>/dev/null || echo "  (nothing -- it never reported one at all)"
    fail "the admitted node never counted itself a member within ${JoinSeconds}s, so it can vote in no election"
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

# What the probing actually cost, on the SUCCESS path.
#
# `.agent/rules/testing.md` asks for this directly and nothing here recorded it:
# without it no budget in this file could be set from data, and every number was a
# guess that survived by not being tested. It is also the baseline that makes a
# future regression legible -- a run whose probe count has grown by an order of
# magnitude has changed behaviour even while it passes.
echo "cluster E2E: $(probe_summary)"
echo "cluster E2E: OK"
