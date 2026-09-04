#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Socket-activate the compile node for real, on this machine, in seconds.
#
# ## Why this exists
#
# #770 took both packaging jobs red on master: #754's advertise rule compared the
# advertised port against `--listen-node`, and under socket activation the UNIT owns
# the address, so that flag holds a default (6674) which is not the port the worker
# serves. The packaged worker exited 2, the socket accepted the activating connection,
# and nothing came up.
#
# **The gap was not that activation is untested. It is that the two things which ARE
# tested cannot fail in the same way.** `ParseSocketActivation` is unit-tested and
# passes under that bug -- it parses `LISTEN_FDS` correctly and knows nothing about
# startup policy. The packaging job that did catch it runs twenty minutes in, on a
# GitHub runner, and is **not a required context** -- so the failure reached master
# through a door that reports to nobody (#684). Between a parser unit test and a
# packaging job there was nothing at all, and that is the space this fills.
#
# ## What it proves that neither of those can
#
#   1. The worker COMES UP under activation      -- the assertion #770 failed. Under
#                                                   #754's shipped rule this fixture
#                                                   goes red in seconds with the exact
#                                                   error CI reported; it was run that
#                                                   way before being committed, because
#                                                   a guard nobody has watched refuse is
#                                                   not a guard.
#   2. Activation is REAL, not a plain spawn     -- nothing runs until a connection
#                                                   arrives. Without this the fixture
#                                                   would pass against a node that
#                                                   ignored the handover entirely.
#   3. The socket is ADOPTED, not re-bound       -- the node says so, AND the default
#                                                   port it would otherwise have taken
#                                                   did not become occupied. The log
#                                                   line alone is a claim. Sampled
#                                                   before and after, because a bare
#                                                   reading fails on any machine already
#                                                   running a daemon on 6674 -- which
#                                                   this project's own dogfood host is.
#   4. It advertises the ACTIVATED port          -- the half #770 was about: the port
#                                                   clients are sent to is the one the
#                                                   supervisor opened.
#
# ## The configuration is the packaged one
#
# `scheduler:` and `advertise:` and no `listen_node:` -- byte-for-byte the shape
# `.github/workflows/build.yml` writes into the file the unit's ExecStart names. Taken
# from the workflow rather than from a shape that seemed representative, because #770
# is precisely a configuration nobody thought to write down.
#
# Registered in `src/tests/CMakeLists.txt` rather than beside the node, for the reason
# `fleet-dashboard-e2e` states: a script-driven test naming an executable belongs where
# every target already exists.

set -uo pipefail

node=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --node) node="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

[[ -n "$node" && -x "$node" ]] \
    || { echo "SKIP: fastcache-compile-node not found: '$node'"; exit "$SKIP"; }

# **A runner without systemd SKIPS, loudly and as its own outcome.** Never a quiet
# fall back to a plain spawn: that would be a pass for the one case this fixture
# exists to exercise, which is the defect it is guarding against wearing a different
# hat (#499's `SKIP_RETURN_CODE`, #685's `SUCCEED` standing in for a skip).
#
# `systemd-socket-activate` rather than a hand-rolled fd handover, because it is what
# systemd itself does: it opens the listener, sets `LISTEN_FDS` and `LISTEN_PID` to
# the child's pid, and execs on the first connection. A bash imitation could not set
# `LISTEN_PID` correctly -- that check is the security-relevant half of
# `ParseSocketActivation` and the one a fake would quietly skip.
activator="$(command -v systemd-socket-activate 2>/dev/null || true)"
[[ -n "$activator" ]] \
    || { echo "SKIP: systemd-socket-activate not found; this host cannot socket-activate"; exit "$SKIP"; }

workdir="$(mktemp -d)"
activator_pid=""

cleanup() {
    [[ -n "$activator_pid" ]] && kill "$activator_pid" 2>/dev/null
    [[ -n "$activator_pid" ]] && wait "$activator_pid" 2>/dev/null
    rm -rf "$workdir"
}
trap cleanup EXIT

. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "node socket-activation E2E" "$workdir"

# **States which mode it exercised**, so a green run is not silently the skip. A
# fixture that cannot say what it did is one whose pass means nothing.
e2e_note "mode: REAL socket activation via ${activator}"

log="${workdir}/node.log"
dump_node_log() {
    [[ -f "$log" ]] && { echo "--- node log ---" >&2; cat "$log" >&2; }
    return 0
}
e2e_on_fail dump_node_log

# The port the SUPERVISOR opens. The node never asks for it, which is the whole point.
port="$(free_port)"

# Sampled BEFORE anything starts, so the adoption check below can be about a CHANGE
# rather than about a state this fixture did not create. See that check for what a bare
# reading cost.
default_port_was_taken="no"
port_answers 127.0.0.1 6674 && default_port_was_taken="yes"

# The default `--listen-node` takes when nothing names one. A worker that failed to
# adopt would bind THIS instead, log readiness, and pass every other assertion here --
# which is why assertion 3 looks at it rather than trusting the log line alone.
readonly DEFAULT_NODE_PORT=6674

# Long enough for a cold include-tree walk on a contended runner, which is what the
# node does before it reports ready -- the same cost `fleet-dashboard-e2e` measured and
# the reason its own bound is minutes rather than seconds.
readonly READY_SECONDS=240

# The packaged configuration: a scheduler that is not there (the worker must still come
# up and keep retrying), an advertise naming the ACTIVATED port, and deliberately no
# --listen-node.
"$activator" --listen="127.0.0.1:${port}" -- \
    "$node" \
        --scheduler=127.0.0.1:6675 \
        --advertise="127.0.0.1:${port}" \
        --toolchain=/usr/bin/g++ \
        --cache-memory=0 \
    > "$log" 2>&1 &
activator_pid=$!

listener_ready() { grep -q "Listening on 127.0.0.1:${port}" "$log" 2>/dev/null; }
wait_until listener_ready "the supervisor to open the listening socket" "$activator_pid" "$log" 30

# --- 2. Activation is REAL: nothing runs until somebody connects -------------
#
# Asserted BEFORE the connection, because afterwards the two are indistinguishable.
# Without it this fixture would pass against a node spawned directly, which is exactly
# the configuration it exists to tell apart.
if grep -q 'compile node ready' "$log" 2>/dev/null; then
    fail "the worker started without a connection; this is not socket activation"
fi

# --- 1. One connection brings it up -----------------------------------------
(exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null || true

worker_ready() { grep -q 'compile node ready' "$log" 2>/dev/null; }
wait_until worker_ready "a connection to bring the worker up" "$activator_pid" "$log" "$READY_SECONDS"

# --- 3. It ADOPTED the socket rather than binding its own --------------------
#
# Two halves, and the second is the one with teeth. The node says it adopted; a node
# that did NOT adopt would bind its own default, say nothing about a handover, and be
# invisible to a log check alone.
grep -q 'a supervisor handed over a listening socket' "$log" \
    || fail "the node did not report adopting the handed-over socket"

# **A CONTROLLED comparison, not a bare "is anything there".** The first spelling of
# this check asked whether anything answered on 6674 and failed on this project's own
# dogfood machine, where a `fastcached` daemon holds exactly that port -- conflating
# "the worker bound its own port" with "somebody owns that port". A fixture that only
# works on a clean machine is one developers learn to ignore, and the false positive
# was found by running it rather than by reading it.
#
# So the port is sampled BEFORE activation, and the claim is about what CHANGED.
if [[ "$default_port_was_taken" = "no" ]]; then
    port_answers 127.0.0.1 "$DEFAULT_NODE_PORT" \
        && fail "the default ${DEFAULT_NODE_PORT} became occupied: the worker bound its own port instead of adopting"
    e2e_note "checked: nothing took the default ${DEFAULT_NODE_PORT}"
else
    # **Named rather than skipped silently.** And it is not a hole: a worker that
    # ignored the handover would bind its own default, which is already held here, so
    # the bind would fail and the node would refuse to start -- which assertion 1
    # above would have caught. The check is unavailable; the property is not.
    e2e_note "not checked: ${DEFAULT_NODE_PORT} was already occupied before this run, so a" \
             "non-adopting worker could not have bound it either -- assertion 1 covers it here"
fi

# --- 4. It advertises the ACTIVATED port ------------------------------------
#
# #770's own subject. A worker that came up but advertised 6674 would send every client
# to a port nothing here serves -- the silent failure #594 is about, reached through
# the fixture that was supposed to prove activation works.
grep -q "advertising 127.0.0.1:${port}" "$log" \
    || fail "the worker did not advertise the activated port ${port}"

echo "ok: the packaged socket-activated worker came up, adopted its socket, and advertised ${port}"
