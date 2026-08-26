#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end test of the fleet dashboard (POSIX). Starts a real
# fastcache-compile-node with a scheduler surface and an admin surface, and asks
# the dashboard the questions no unit test can:
#
#   1. The routes are actually reachable  — a unit test drives `MakeFleetRoutes`
#                                           directly; only this proves the rows
#                                           reach the accept loop, over a real
#                                           socket, on a real bound port.
#   2. /metrics did not change            — the whole reason the dashboard shares
#                                           `--admin-listen` is that an operator's
#                                           existing scraper keeps working. If a
#                                           credential or a route ever leaks onto
#                                           that path, every deployment breaks at
#                                           once and this is what says so.
#   3. The credential is enforced         — and answers with a challenge a browser
#                                           can prompt for, in both the Basic and
#                                           Bearer spellings.
#   4. A worker appears in the fleet      — the node registers with its own
#                                           scheduler, so the page shows a machine
#                                           rather than an empty table. That is the
#                                           end-to-end path from REGISTER through
#                                           the registry to the rendered document.
#   5. The page is self-contained         — no script tag and no external URL, so
#                                           it renders on the air-gapped network a
#                                           build fleet usually lives on.
#
# Registered in `src/tests/CMakeLists.txt` rather than beside the node, because a
# script-driven test naming an executable belongs where every target already
# exists -- `src/apps` walks its table in order, and a `$<TARGET_FILE:>` guard on a
# binary configured later silently skips the test forever.

set -uo pipefail

node=""
tls_cert=""
tls_key=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --node) node="$2"; shift 2 ;;
        --tls-cert) tls_cert="$2"; shift 2 ;;
        --tls-key) tls_key="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77
[[ -n "$node" && -x "$node" ]] || { echo "fastcache-compile-node not found: '$node'; skipping"; exit "$SKIP"; }

# When a certificate is named this run is the HTTPS one, and it needs a client
# that speaks TLS. `/dev/tcp` cannot, so this half -- and only this half -- depends
# on curl and skips without it rather than silently testing the plaintext path
# under a name that says otherwise.
if [[ -n "$tls_cert" ]]; then
    [[ -r "$tls_cert" && -r "$tls_key" ]] || { echo "TLS material unreadable; skipping"; exit "$SKIP"; }
    command -v curl >/dev/null 2>&1 || { echo "curl not found and TLS was asked for; skipping"; exit "$SKIP"; }
fi

workdir="$(mktemp -d)"
node_pid=""

cleanup() {
    [[ -n "$node_pid" ]] && kill "$node_pid" 2>/dev/null
    [[ -n "$node_pid" ]] && wait "$node_pid" 2>/dev/null
    rm -rf "$workdir"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    [[ -f "${workdir}/node.log" ]] && { echo "--- node log ---" >&2; cat "${workdir}/node.log" >&2; }
    exit 1
}

# Ports are allocated per run rather than fixed: four more fixed ports are four
# more ways to collide with whatever else a CI runner is doing, and the failure
# reads as "the dashboard is broken" when it means "something else was listening".
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

# GET one path and echo the whole response, headers included.
#
# `/dev/tcp` rather than curl, because a fixture that skips when curl is absent
# tests nothing on the machine that lacks it. Every read is bounded with `read -t`:
# the endpoint closes the connection itself, so a healthy server ends the loop --
# and a WEDGED one, which is part of what this probe exists to detect, would
# otherwise hang the suite instead of failing it.
# @param 1 port
# @param 2 path
# @param 3 optional Authorization header value
http_get() {
    local port="$1" path="$2" auth="${3:-}" line="" body=""

    # Over TLS `/dev/tcp` cannot help, so the HTTPS run goes through curl. `-k`
    # because the checked-in fixture certificate is self-signed for 'localhost'
    # and what this asserts is that the handshake happens and the routes answer
    # behind it, not that a test fixture chains to a public root.
    if [[ -n "$tls_cert" ]]; then
        local args=(-sk -i -m 10)
        [[ -n "$auth" ]] && args+=(-H "Authorization: ${auth}")
        curl "${args[@]}" "https://127.0.0.1:${port}${path}"
        return 0
    fi

    exec 3<>"/dev/tcp/127.0.0.1/${port}" || return 1
    {
        printf 'GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n' "$path"
        [[ -n "$auth" ]] && printf 'Authorization: %s\r\n' "$auth"
        printf 'Connection: close\r\n\r\n'
    } >&3
    while IFS= read -r -t 5 line <&3; do body+="${line}"$'\n'; done
    # `read` sets `line` and returns non-zero on a final chunk with no trailing
    # newline, so the loop above drops it. That matters here more than it looks:
    # the JSON document is ONE line with no newline at all, so without this the
    # whole body vanishes and every assertion about it fails for a reason that has
    # nothing to do with the server.
    [[ -n "$line" ]] && body+="$line"
    exec 3<&-
    printf '%s' "$body"
}

# Block until something answers on a port, or the process behind it dies.
#
# Waiting on the listener rather than sleeping a fixed amount: a cold CI runner
# takes noticeably longer to get there than a warm developer machine, and a fixed
# sleep is either flaky or slow. Bounded, and it says what it waited for.
wait_for_port() {
    local port="$1" pid="$2" what="$3"
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/${port}") 2>/dev/null; then return 0; fi
        if ! kill -0 "$pid" 2>/dev/null; then fail "$what died before it listened on ${port}"; fi
        sleep 0.1
    done
    fail "timed out after 10s waiting for $what to listen on ${port}"
}

admin_port="$(free_port)"
sched_port="$(free_port)"
worker_port="$(free_port)"
readonly TOKEN="dashboard-e2e-secret-token"
printf '%s\n' "$TOKEN" > "${workdir}/token"

# A toolchain that exists on every machine this runs on. The dashboard does not
# compile anything, so any real binary the node accepts will do.
toolchain="$(command -v c++ || command -v g++ || command -v cc)"
[[ -n "$toolchain" ]] || { echo "no C++ driver on PATH; skipping"; exit "$SKIP"; }

tls_args=()
[[ -n "$tls_cert" ]] && tls_args=(--tls-cert "$tls_cert" --tls-key "$tls_key")

"$node" \
    --scheduler "127.0.0.1:${sched_port}" \
    --toolchain "$toolchain" \
    --port "$worker_port" \
    --advertise "127.0.0.1:${worker_port}" \
    --listen-scheduler "$sched_port" \
    --fleet-open \
    --admin-listen "$admin_port" \
    --dashboard \
    --dashboard-token-file "${workdir}/token" \
    --listen-cache "" \
    "${tls_args[@]}" \
    > "${workdir}/node.log" 2>&1 &
node_pid=$!

wait_for_port "$admin_port" "$node_pid" "the node's admin surface"

# ---------------------------------------------------------------- 2. /metrics
# First, because it is the assertion that protects every existing deployment: the
# dashboard shares this port, and a credential or a route leaking onto /metrics
# breaks every scraper an operator has pointed at it.
metrics="$(http_get "$admin_port" /metrics)"
[[ "$metrics" == HTTP/1.1\ 200* ]] || fail "/metrics did not answer 200 without a credential: ${metrics%%$'\n'*}"
[[ "$metrics" == *fastcached_* ]] || fail "/metrics answered 200 but carried no series"

health="$(http_get "$admin_port" /healthz)"
[[ "$health" == HTTP/1.1\ 200* ]] || fail "/healthz did not answer 200 without a credential"

# ------------------------------------------------------------- 3. credential
anonymous="$(http_get "$admin_port" /fleet)"
[[ "$anonymous" == HTTP/1.1\ 401* ]] || fail "/fleet served without a credential: ${anonymous%%$'\n'*}"
# A 401 with no challenge is one a browser shows as a broken page rather than
# prompting for -- which would make the page unreachable from the laptop it exists
# to be opened on.
[[ "$anonymous" == *WWW-Authenticate:\ Basic* ]] || fail "/fleet refused without a challenge a browser can act on"

wrong="$(http_get "$admin_port" /fleet "Bearer not-the-token")"
[[ "$wrong" == HTTP/1.1\ 401* ]] || fail "/fleet accepted the wrong token"

# Both spellings, because one is what a script sends and the other is what a
# browser can be made to prompt for.
basic_value="$(printf ':%s' "$TOKEN" | base64 | tr -d '\n')"
for auth in "Bearer ${TOKEN}" "Basic ${basic_value}"; do
    page="$(http_get "$admin_port" /fleet "$auth")"
    [[ "$page" == HTTP/1.1\ 200* ]] || fail "/fleet refused a valid credential (${auth%% *}): ${page%%$'\n'*}"
done

# ------------------------------------------------------- 1, 4, 5. the document
page="$(http_get "$admin_port" /fleet "Bearer ${TOKEN}")"
[[ "$page" == *Content-Type:\ text/html* ]] || fail "/fleet did not answer HTML"
[[ "$page" == *'<!doctype html>'* ]] || fail "/fleet answered HTML without a doctype"

# Self-contained: no script and no external asset, so it renders on an air-gapped
# network and carries nothing a browser has to fetch.
[[ "$page" != *'<script'* ]] || fail "the dashboard carries a script tag"
[[ "$page" != *'http://'* && "$page" != *'https://'* ]] || fail "the dashboard references an external URL"

json="$(http_get "$admin_port" /fleet.json "Bearer ${TOKEN}")"
[[ "$json" == *Content-Type:\ application/json* ]] || fail "/fleet.json did not answer JSON"
[[ "$json" == *'"role":"leader"'* ]] || fail "the node did not report itself as the fleet's leader"

# The node registers with its own scheduler on its heartbeat, so the machine
# appears once the first REGISTER lands. Bounded, and it says what it waited for.
registered=""
for _ in $(seq 1 100); do
    json="$(http_get "$admin_port" /fleet.json "Bearer ${TOKEN}")"
    if [[ "$json" == *"127.0.0.1:${worker_port}"* ]]; then registered="yes"; break; fi
    if ! kill -0 "$node_pid" 2>/dev/null; then fail "the node died before it registered with its own scheduler"; fi
    sleep 0.1
done
[[ -n "$registered" ]] || fail "timed out after 10s waiting for the worker to appear in /fleet.json"

# And it is one MACHINE, whatever it serves: the grain a fleet total is computed
# over. A page listing registry entries would double-count a node's cores.
[[ "$json" == *'"toolchains":1'* ]] || fail "/fleet.json did not report the machine's toolchain count"

# An unknown path is still a plain 404 rather than anything the dashboard added.
missing="$(http_get "$admin_port" /nope "Bearer ${TOKEN}")"
[[ "$missing" == HTTP/1.1\ 404* ]] || fail "an unknown path no longer answers 404"

if [[ -n "$tls_cert" ]]; then
    # And that the handshake is real rather than the port merely answering: a
    # plaintext request to a TLS port must fail, not be served.
    plaintext_code="$(curl -s -m 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:${admin_port}/healthz" || true)"
    [[ "$plaintext_code" != "200" ]] || fail "a plaintext request to the TLS admin port was served"
    echo "PASS: fleet dashboard served over HTTPS, credential enforced, /metrics unchanged"
else
    echo "PASS: fleet dashboard served, credential enforced, /metrics unchanged"
fi
