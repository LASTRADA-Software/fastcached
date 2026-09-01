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
#   5. The page is self-contained         — no script tag and nothing fetched, so
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
tls_self_signed=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --node) node="$2"; shift 2 ;;
        --tls-cert) tls_cert="$2"; shift 2 ;;
        --tls-key) tls_key="$2"; shift 2 ;;
        --tls-self-signed) tls_self_signed="yes"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77
[[ -n "$node" && -x "$node" ]] || { echo "fastcache-compile-node not found: '$node'; skipping"; exit "$SKIP"; }

# When a certificate is named this run is the HTTPS one, and it needs a client
# that speaks TLS. `/dev/tcp` cannot, so this half -- and only this half -- depends
# on curl and skips without it rather than silently testing the plaintext path
# under a name that says otherwise.
# `tls` is set for either spelling -- a named certificate or a generated one --
# because everything downstream only cares whether the port speaks TLS.
tls=""
[[ -n "$tls_cert" || -n "$tls_self_signed" ]] && tls="yes"
if [[ -n "$tls" ]]; then
    [[ -z "$tls_cert" || ( -r "$tls_cert" && -r "$tls_key" ) ]] \
        || { echo "TLS material unreadable; skipping"; exit "$SKIP"; }
    command -v curl >/dev/null 2>&1 || { echo "curl not found and TLS was asked for; skipping"; exit "$SKIP"; }
fi

workdir="$(mktemp -d)"
node_pid=""

# The cluster's pre-shared key, which every node here shares.
#
# Not decoration and not a workaround for the startup rule: these nodes leave
# `--bind` at the wildcard and say `--fleet-open`, so another machine genuinely
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
    [[ -n "$node_pid" ]] && kill "$node_pid" 2>/dev/null
    [[ -n "$node_pid" ]] && wait "$node_pid" 2>/dev/null
    rm -rf "$workdir"
}
trap cleanup EXIT

# The shared helpers: `fail`, `free_port`, `wait_for_port`, `wait_until` and the
# plain-HTTP half of `http_get`, one copy for every POSIX fixture (#449).
#
# Two of this file's copies had learnt things the others never did, and both
# travel into the shared version rather than being lost: the issued-port ledger,
# and -- the one with teeth -- that `read` returns non-zero on a final chunk with
# no trailing newline, so a naive loop drops it. The JSON document this fixture
# reads is ONE line with no newline at all, so without that the whole body
# vanishes and every assertion about it fails for a reason that has nothing to do
# with the server.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "fleet dashboard E2E" "$workdir"

# Every failure dumps the node's log first; cleanup takes it away.
dump_node_log() {
    [[ -f "${workdir}/node.log" ]] && { echo "--- node log ---" >&2; cat "${workdir}/node.log" >&2; }
    return 0
}
e2e_on_fail dump_node_log

# How long a bounded wait in this fixture may take.
#
# 240 seconds, and the number is what it is because of what happens BEFORE the
# admin surface binds: the node resolves each `--toolchain` first, which walks
# the compiler's include tree. That is thousands of cold stat and read calls,
# near-instant on a warm developer machine and minutes on a contended CI runner
# whose page cache holds none of it.
#
# The cost is paid ONCE per runner, and the three fixtures below measured it
# exactly. Run back to back against the same node and the same `/usr/bin/c++`:
#
#     fleet-dashboard-e2e              timed out at  60s
#     fleet-dashboard-tls-e2e          passed in    5.08s
#     fleet-dashboard-self-signed-e2e  passed in    1.27s
#
# Nothing differs between them but page-cache warmth, so whichever runs first
# pays for all three -- and 10s, then 60s, were each enough on a quiet runner
# and not on a busy one. `EpollSocket::WriteVectored` took 26s in that same run,
# which is what "busy" looked like.
#
# Generous rather than unbounded: a wait nothing can end is a suite timeout
# naming nothing, which this repository has already paid for once. This one
# named the port, the elapsed time and the node's own last log line, and that is
# what made two runner failures diagnosable from the output alone.
e2e_wait_seconds 240

# GET one path off the admin surface, with this fixture's two optional headers.
#
# A thin adapter rather than a second implementation: the plain-HTTP path is the
# shared `http_get`, which takes header lines variadically. What stays here is
# the part that is about THIS fixture -- that one of its three ctest
# registrations serves the same routes over TLS, where `/dev/tcp` cannot help.
# `-k` because the checked-in fixture certificate is self-signed for 'localhost'
# and what this asserts is that the handshake happens and the routes answer
# behind it, not that a test fixture chains to a public root.
#
# @param 1 port
# @param 2 path
# @param 3 optional Authorization header value
# @param 4 optional If-None-Match value. Second header on purpose: the parse loop
#          used to stop at the first one it recognised, which stayed correct
#          exactly until there were two.
dash_get() {
    local port="$1" path="$2" auth="${3:-}" etag="${4:-}"

    if [[ -n "$tls" ]]; then
        local args=(-sk -i -m 10)
        [[ -n "$auth" ]] && args+=(-H "Authorization: ${auth}")
        [[ -n "$etag" ]] && args+=(-H "If-None-Match: ${etag}")
        curl "${args[@]}" "https://127.0.0.1:${port}${path}"
        return 0
    fi

    local headers=()
    [[ -n "$auth" ]] && headers+=("Authorization: ${auth}")
    [[ -n "$etag" ]] && headers+=("If-None-Match: ${etag}")
    http_get 127.0.0.1 "$port" "$path" ${headers[@]+"${headers[@]}"}
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

# The flags that turn TLS on, or none of them.
#
# Expanded below as `${tls_args[@]+"${tls_args[@]}"}` rather than plainly, which
# looks redundant and is not: under `set -u`, bash 3.2 treats expanding an EMPTY
# array as an unbound variable and aborts. That is the bash macOS still ships, so
# the plaintext run -- the only one whose array is empty -- died on the runner
# while both TLS runs passed. bash 4.4 made the plain form safe, which is why no
# Linux job ever saw it.
tls_args=()
[[ -n "$tls_cert" ]] && tls_args=(--tls-cert "$tls_cert" --tls-key "$tls_key")
[[ -n "$tls_self_signed" ]] && tls_args=(--tls-self-signed)

"$node" \
    --scheduler "127.0.0.1:${sched_port}" \
    --toolchain "$toolchain" \
    --port "$worker_port" \
    --advertise "127.0.0.1:${worker_port}" \
    --serve-scheduler \
    --listen-node "$sched_port" \
    --fleet-open \
    --cluster-key-file "$cluster_key" \
    --admin-listen "$admin_port" \
    --dashboard \
    --dashboard-token-file "${workdir}/token" \
    --cache-memory 0 \
    ${tls_args[@]+"${tls_args[@]}"} \
    > "${workdir}/node.log" 2>&1 &
node_pid=$!

wait_for_port 127.0.0.1 "$admin_port" "$node_pid" "the node's admin surface" "${workdir}/node.log"

# ---------------------------------------------------------------- 2. /metrics
# First, because it is the assertion that protects every existing deployment: the
# dashboard shares this port, and a credential or a route leaking onto /metrics
# breaks every scraper an operator has pointed at it.
metrics="$(dash_get "$admin_port" /metrics)"
[[ "$metrics" == HTTP/1.1\ 200* ]] || fail "/metrics did not answer 200 without a credential: ${metrics%%$'\n'*}"
[[ "$metrics" == *fastcached_* ]] || fail "/metrics answered 200 but carried no series"

health="$(dash_get "$admin_port" /healthz)"
[[ "$health" == HTTP/1.1\ 200* ]] || fail "/healthz did not answer 200 without a credential"

# ------------------------------------------------------------- 3. credential
anonymous="$(dash_get "$admin_port" /fleet)"
[[ "$anonymous" == HTTP/1.1\ 401* ]] || fail "/fleet served without a credential: ${anonymous%%$'\n'*}"
# A 401 with no challenge is one a browser shows as a broken page rather than
# prompting for -- which would make the page unreachable from the laptop it exists
# to be opened on.
[[ "$anonymous" == *WWW-Authenticate:\ Basic* ]] || fail "/fleet refused without a challenge a browser can act on"

wrong="$(dash_get "$admin_port" /fleet "Bearer not-the-token")"
[[ "$wrong" == HTTP/1.1\ 401* ]] || fail "/fleet accepted the wrong token"

# Both spellings, because one is what a script sends and the other is what a
# browser can be made to prompt for.
basic_value="$(printf ':%s' "$TOKEN" | base64 | tr -d '\n')"
for auth in "Bearer ${TOKEN}" "Basic ${basic_value}"; do
    page="$(dash_get "$admin_port" /fleet "$auth")"
    [[ "$page" == HTTP/1.1\ 200* ]] || fail "/fleet refused a valid credential (${auth%% *}): ${page%%$'\n'*}"
done

# ------------------------------------------------------- 1, 4, 5. the document
page="$(dash_get "$admin_port" /fleet "Bearer ${TOKEN}")"
[[ "$page" == *Content-Type:\ text/html* ]] || fail "/fleet did not answer HTML"
[[ "$page" == *'<!doctype html>'* ]] || fail "/fleet answered HTML without a doctype"

# Self-contained: no script, and nothing fetched from another host, so it renders
# on an air-gapped network. Asked as "no attribute a browser resolves points off
# this origin" rather than "no absolute URL anywhere": the inline sparkline's
# `xmlns` is an XML namespace NAME, which looks like a URL and is never fetched --
# and a check that failed on it would push the sparkline out of the page for a
# reason that was never true.
[[ "$page" != *'<script'* ]] || fail "the dashboard carries a script tag"
[[ "$page" != *'src="http'* ]] || fail "the dashboard loads something from another host"
[[ "$page" != *'href="http'* ]] || fail "the dashboard links a stylesheet from another host"
[[ "$page" != *'@import'* ]] || fail "the dashboard imports a stylesheet"

json="$(dash_get "$admin_port" /fleet.json "Bearer ${TOKEN}")"
[[ "$json" == *Content-Type:\ application/json* ]] || fail "/fleet.json did not answer JSON"
[[ "$json" == *'"role":"leader"'* ]] || fail "the node did not report itself as the fleet's leader"

# The node registers with its own scheduler on its heartbeat, so the machine
# appears once the first REGISTER lands. Bounded, and it says what it waited for.
# A bespoke CONDITION through the shared loop, rather than a bespoke loop. The
# hand-written one this replaces reported `timed out after $((WAIT_TICKS / 10))s`
# -- a duration computed from the loop shape and never observed, which is the one
# reading that would have said whether the runner was slow.
worker_is_listed() {
    json="$(dash_get "$admin_port" /fleet.json "Bearer ${TOKEN}")"
    [[ "$json" == *"127.0.0.1:${worker_port}"* ]]
}
wait_until worker_is_listed "the worker to appear in /fleet.json" \
    "$node_pid" "${workdir}/node.log" 240

# And it is one MACHINE, whatever it serves: the grain a fleet total is computed
# over. A page listing registry entries would double-count a node's cores.
[[ "$json" == *'"toolchains":1'* ]] || fail "/fleet.json did not report the machine's toolchain count"

# The version travelled from the node's own binary, through the REGISTER capacity
# record, to the leader's report. Asserted end to end because every seam in that
# path is one where it could silently become empty -- and empty is a legitimate
# value here, so nothing else would notice.
[[ "$json" != *'"version":null'* ]] || fail "/fleet.json reported no version for a node that has one"
[[ "$json" == *'"version":"'* ]] || fail "/fleet.json carried no version field at all"
[[ "$page" == *'>version<'* ]] || fail "/fleet has no version column"

# ------------------------------------------------------- 6. charts over time
# Every chart the page references is its own resource. Asked for by name rather
# than scraped out of the page, so a chart that stopped being served would fail
# here rather than quietly become a broken image in a browser.
for chart in dispatched refusals capacity hit-rate; do
    # **Unauthenticated first.** An image URL that answered without a credential
    # would leak the fleet's whole history while /fleet itself stayed locked --
    # which is the one way this feature could have made the surface less safe.
    open_chart="$(dash_get "$admin_port" "/fleet/chart/${chart}.svg?range=24h")"
    [[ "$open_chart" == HTTP/1.1\ 401* ]] \
        || fail "/fleet/chart/${chart}.svg served without a credential: ${open_chart%%$'\n'*}"

    svg="$(dash_get "$admin_port" "/fleet/chart/${chart}.svg?range=24h" "Bearer ${TOKEN}")"
    [[ "$svg" == HTTP/1.1\ 200* ]] || fail "/fleet/chart/${chart}.svg did not answer 200: ${svg%%$'\n'*}"
    [[ "$svg" == *Content-Type:\ image/svg+xml* ]] || fail "/fleet/chart/${chart}.svg did not answer SVG"
    [[ "$svg" == *'<svg '* ]] || fail "/fleet/chart/${chart}.svg answered without an SVG root"
    [[ "$svg" != *'<script'* ]] || fail "/fleet/chart/${chart}.svg carried a script"
done

# The conditional GET the whole arrangement exists for.
svg="$(dash_get "$admin_port" "/fleet/chart/dispatched.svg?range=24h" "Bearer ${TOKEN}")"
etag="$(printf '%s' "$svg" | tr -d '\r' | sed -n 's/^ETag: //p' | head -1)"
[[ -n "$etag" ]] || fail "a chart was served with no ETag, so a browser can never revalidate it"
[[ "$svg" == *Cache-Control:\ max-age=* ]] || fail "a chart was served with no Cache-Control"

cached="$(dash_get "$admin_port" "/fleet/chart/dispatched.svg?range=24h" "Bearer ${TOKEN}" "$etag")"
[[ "$cached" == HTTP/1.1\ 304* ]] || fail "a chart did not answer 304 to its own ETag: ${cached%%$'\n'*}"
# RFC 9110 forbids content on a 304, and a Content-Length a client reads before
# finding the connection closed is reported as a truncated response.
[[ "$cached" != *Content-Length:* ]] || fail "a 304 carried a Content-Length"
[[ "$cached" != *'<svg'* ]] || fail "a 304 carried a body"
[[ "$cached" == *ETag:* ]] || fail "a 304 dropped the validator the client needs next time"

# An unknown range is refused rather than quietly served as a different one.
bad_range="$(dash_get "$admin_port" "/fleet/chart/dispatched.svg?range=30d" "Bearer ${TOKEN}")"
[[ "$bad_range" == HTTP/1.1\ 400* ]] || fail "an unknown range was not refused: ${bad_range%%$'\n'*}"
# An unknown chart is a 404, not whichever chart happened to be first in the table.
bad_chart="$(dash_get "$admin_port" "/fleet/chart/nonesuch.svg?range=24h" "Bearer ${TOKEN}")"
[[ "$bad_chart" == HTTP/1.1\ 404* ]] || fail "an unknown chart was not refused: ${bad_chart%%$'\n'*}"

# And the series behind those charts, so anything on the page can be checked
# without a browser.
open_series="$(dash_get "$admin_port" "/fleet/series.json?range=24h")"
[[ "$open_series" == HTTP/1.1\ 401* ]] || fail "/fleet/series.json served without a credential"
series="$(dash_get "$admin_port" "/fleet/series.json?range=24h" "Bearer ${TOKEN}")"
[[ "$series" == HTTP/1.1\ 200* ]] || fail "/fleet/series.json did not answer 200: ${series%%$'\n'*}"
[[ "$series" == *Content-Type:\ application/json* ]] || fail "/fleet/series.json did not answer JSON"
[[ "$series" == *'"range":"24h"'* ]] || fail "/fleet/series.json did not name the range it answered for"
[[ "$series" == *'"dispatched":['* ]] || fail "/fleet/series.json carried no dispatched series"

# The page points at those resources rather than inlining them.
[[ "$page" == *'/fleet/chart/dispatched.svg?range=24h'* ]] \
    || fail "/fleet did not reference the chart resources"

# An unknown path is still a plain 404 rather than anything the dashboard added.
missing="$(dash_get "$admin_port" /nope "Bearer ${TOKEN}")"
[[ "$missing" == HTTP/1.1\ 404* ]] || fail "an unknown path no longer answers 404"

if [[ -n "$tls" ]]; then
    # And that the handshake is real rather than the port merely answering: a
    # plaintext request to a TLS port must fail, not be served.
    plaintext_code="$(curl -s -m 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:${admin_port}/healthz" || true)"
    [[ "$plaintext_code" != "200" ]] || fail "a plaintext request to the TLS admin port was served"

    if [[ -n "$tls_self_signed" ]]; then
        # A generated certificate has to be valid for the NAME an operator types,
        # not merely exist: an unknown issuer is one browser warning they can
        # accept, and a name mismatch on top of it is a second, much harder one.
        #
        # Asked with `openssl x509 -checkhost` rather than by reading curl's exit
        # code, because that code is not a stable way to tell "untrusted issuer"
        # from "wrong name" -- measured here, a mismatch surfaced as a timeout
        # rather than as the documented 51, which would have made this assertion
        # quietly vacuous.
        if command -v openssl >/dev/null 2>&1; then
            pem="${workdir}/served.pem"
            echo | openssl s_client -connect "127.0.0.1:${admin_port}" 2>/dev/null \
                 | openssl x509 -outform PEM > "$pem" 2>/dev/null
            [[ -s "$pem" ]] || fail "could not read the certificate the node is serving"

            # `-checkhost` reports its answer in the OUTPUT and exits 0 either
            # way, which is why this greps rather than testing the exit code --
            # the negative control below is what caught that, and without it this
            # whole block would have passed while asserting nothing.
            checks_name() {
                openssl x509 -in "$pem" -noout "$1" "$2" 2>/dev/null | grep -q "does match"
            }

            checks_name -checkhost localhost \
                || fail "the generated certificate is not valid for the name 'localhost'"
            checks_name -checkip 127.0.0.1 \
                || fail "the generated certificate is not valid for 127.0.0.1"
            # The negative control, so the two above cannot pass by matching
            # everything -- a certificate valid for any name is not a certificate.
            if checks_name -checkhost elsewhere.invalid; then
                fail "the generated certificate matches a name nobody asked for"
            fi

            # And the fingerprint the node logged is the one on the wire -- the
            # only thing that authenticates a certificate nothing signed.
            logged="$(grep -oE 'fingerprint [0-9a-f]{64}' "${workdir}/node.log" | awk '{print $2}' | head -1)"
            [[ -n "$logged" ]] || fail "the node did not report the generated certificate's fingerprint"
            wire="$(openssl x509 -in "$pem" -noout -fingerprint -sha256 2>/dev/null \
                    | sed 's/.*=//' | tr -d ':' | tr 'A-F' 'a-f')"
            [[ "$wire" == "$logged" ]] \
                || fail "the logged fingerprint (${logged}) is not the one served (${wire})"
        fi

        echo "PASS: fleet dashboard served over HTTPS from a generated certificate"
    else
        echo "PASS: fleet dashboard served over HTTPS, credential enforced, /metrics unchanged"
    fi
else
    echo "PASS: fleet dashboard served, credential enforced, /metrics unchanged"
fi
