#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# sccache smoke test (POSIX). Start fastcached, point sccache at it over the
# chosen wire protocol, compile a one-file program twice, and assert the
# second compile is served from cache. Shared by the CI smoke jobs and the
# CTest `smoke` tests so both exercise exactly the same path.
#
# Usage:
#   sccache-smoke.sh --fastcached <path> [--protocol memcached|redis]
#                    [--port <n>] [--compiler <cxx>] [--expect-flavor <name>]
#
# Exit codes: 0 = every assertion held; 1 = ran and something was wrong; 77 = a
# runtime prerequisite (sccache / fastcached / compiler / a backend that is not
# compiled into this sccache) was missing.
#
# **77 IS A SKIP UNDER CTEST AND A JOB FAILURE IN CI**, and the difference is not
# this script's to reconcile. CTest is told what 77 means (`SKIP_RETURN_CODE`);
# a GitHub Actions `run:` step is not, and none of the three smoke jobs carries
# `continue-on-error`, so 77 there fails the step and reddens a REQUIRED context.
# Both readings are correct for their caller: on a developer box an sccache built
# without the backend is a genuinely absent prerequisite, while in CI the sccache
# is installed by the job itself and a missing backend means that job is
# misconfigured. Neither is a reason to soften the exit -- it must simply never be
# reached on a healthy runner.
#
# ## What this asserts, and why one assertion was never enough
#
# The original assertion was `Cache hits >= 1` alone, and sccache satisfies that
# from its own LOCAL DISK cache -- so the fixture passed with no fastcached in the
# picture at all, for as long as it existed (#1318). Three questions have to be
# asked, because each is satisfied by the failing state of the next:
#
#   1. is the backend even IN this sccache        -- `Enabled features:`
#   2. did sccache USE it, or fall back silently  -- `Cache location`
#   3. WHICH DIALECT did it speak                 -- the daemon's own log
#
# (3) is not pedantry: `sccache smoke (memcached text)` exists to cover the
# memcached TEXT protocol, and `Cache location` reports the backend TYPE, so a
# binary-speaking sccache reports `memcached, name: memcached, prefix: /` exactly
# as a text-speaking one does. Without (3) that leg could exercise binary, pass
# everything, and go on claiming text coverage -- which is this ticket's own
# defect re-established deliberately.
set -euo pipefail

fastcached=""
protocol="memcached"
# Empty means DRAW one; `--port` still overrides, which is what CI and a human
# debugging a run need. A FIXED port needs a reaper and this fixture never had one,
# so a run that missed its cleanup left a daemon holding 11611 for the life of the
# machine and every later run failed against it, naming neither the port nor the
# process (#220 is that same defect one fixture over).
#
# The reason offered for the constant was TRUE and did not support it: the backend
# URL must be decided before sccache starts, which is an argument for deciding the
# port EARLY -- not for fixing it. Drawing it early is what this file now does.
port=""
compiler="${CXX:-c++}"
# Which memcached DIALECT this caller expects, exactly. Optional, and the two
# readings are deliberately different rather than one lenient rule:
#
#   given    -- the caller knows which sccache it installed, so the observed
#               flavour must equal this or the run fails. CI is this case: each
#               job pins its own sccache and therefore knows.
#   omitted  -- assert only that the daemon saw a flavour belonging to this
#               PROTOCOL's family, and print which. A developer's sccache version
#               is not knowable here, and demanding an exact dialect would refuse
#               correct machines.
#
# Both directions still fail when the daemon saw NOTHING, which is the case the
# fixture existed for and never covered.
expectFlavor=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached)    fastcached="$2";   shift 2 ;;
        --protocol)      protocol="$2";     shift 2 ;;
        --port)          port="$2";         shift 2 ;;
        --compiler)      compiler="$2";     shift 2 ;;
        --expect-flavor) expectFlavor="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77

command -v sccache >/dev/null 2>&1 || { echo "sccache not found; skipping"; exit "$SKIP"; }

# Is the backend COMPILED IN? Asked of `sccache --help`, which carries
#
#   Enabled features:
#       Redis:     false
#       Memcached: false
#
# and asked BEFORE anything is started, because a backend that is absent makes
# every later assertion a statement about sccache's local disk cache.
#
# This replaces a guard that could never fire. The old one skipped when
# `--start-server` failed with "Cache type not supported with current feature
# configuration" -- measured, with `SCCACHE_MEMCACHED` set on a feature-less
# build: `--start-server` exits **0** and prints `sccache: Starting the server...`.
# It does not refuse, it DEGRADES to local disk. So the check written for exactly
# this condition sat dead while the condition held, which is why nobody saw it.
#
# The cause is packaging rather than sccache: measured, Ubuntu's sccache 0.7.7
# reports `Memcached: false` while the OFFICIAL 0.7.7 release binary reports
# `Memcached: true`. Same version, different build. So "which sccache" is a real
# question a job has to answer, and this is what makes a wrong answer loud.
SccacheFeatureEnabled() {
    local feature="$1" help
    help="$(sccache --help 2>&1)" || return 1
    # Herestring, never `printf | grep -q`: under `pipefail` the producer dies of
    # SIGPIPE when grep exits at the first match, and the pipeline then reports
    # FAILURE on the success path.
    grep -qE "^[[:space:]]*${feature}:[[:space:]]+true[[:space:]]*$" <<< "$help"
}
[[ -n "$fastcached" && -x "$fastcached" ]] || { echo "fastcached not found: '$fastcached'; skipping"; exit "$SKIP"; }
command -v "$compiler" >/dev/null 2>&1 || { echo "compiler not found: '$compiler'; skipping"; exit "$SKIP"; }

# One row per protocol, and every column this fixture needs about it. A table
# rather than four `case` ladders, so adding a backend is a row and cannot be
# half-added -- the failure this file is being repaired from is a question that
# was asked in one place and not another.
#
#   protocol | env var | URL scheme | `Enabled features:` label | `Cache location`
#            needle | the ProtocolFlavor names the daemon may legitimately report
#
# An associative array would be the obvious shape and is BANNED here: a hygiene
# script ctest runs is constrained to bash 3.2, which macOS still ships.
SccacheBackendTable="\
memcached|SCCACHE_MEMCACHED|tcp|Memcached|memcached|memcached-text memcached-binary
redis|SCCACHE_REDIS|redis|Redis|redis|redis-resp"

backendEnv=""
backendScheme=""
backendFeature=""
backendLocation=""
backendFlavors=""
while IFS='|' read -r row_protocol row_env row_scheme row_feature row_location row_flavors; do
    [ "$row_protocol" = "$protocol" ] || continue
    backendEnv="$row_env"
    backendScheme="$row_scheme"
    backendFeature="$row_feature"
    backendLocation="$row_location"
    backendFlavors="$row_flavors"
done <<< "$SccacheBackendTable"

# A protocol with no row is refused rather than defaulted, and the refusal names
# the table -- a silent default here would pick a backend the caller did not ask
# for, which is the whole subject of this file.
if [ -z "$backendEnv" ]; then
    echo "unknown protocol: '$protocol' (no row in SccacheBackendTable)" >&2
    exit 2
fi

# An exact expectation must be one THIS protocol can produce, or the run would be
# asserting something unreachable and would read as a backend failure forever.
if [ -n "$expectFlavor" ]; then
    case " $backendFlavors " in
        *" $expectFlavor "*) ;;
        *)
            echo "--expect-flavor '$expectFlavor' is not a flavour the '$protocol' backend can produce" >&2
            echo "  this protocol's flavours: $backendFlavors" >&2
            exit 2
            ;;
    esac
fi

if ! SccacheFeatureEnabled "$backendFeature"; then
    echo "this sccache is built without the ${backendFeature} backend; skipping" >&2
    sccache --help 2>&1 | sed -n '/Enabled features:/,/^[[:space:]]*$/p' >&2 || true
    echo "  a distribution package may omit it where the official release binary carries it," >&2
    echo "  so this is a question of WHICH sccache, not of configuration." >&2
    exit "$SKIP"
fi

export SCCACHE_NO_DAEMON=0

# The shared fixture library: `free_port`, `port_answers` and the bounded waits.
# Sourced AFTER the prerequisite checks above, which exit 77 for a skip -- the
# library's `fail` exits 1, and a missing sccache is not a failure.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"

workdir="$(mktemp -d)"
src="${workdir}/hello.cpp"
obj="${workdir}/hello.o"
daemon_log="${workdir}/fastcached.log"
cat > "$src" <<'EOF'
#include <string>
int main() { return static_cast<int>(std::string{"hi"}.size()); }
EOF

server_pid=""
cleanup() {
    sccache --stop-server >/dev/null 2>&1 || true
    if [[ -n "$server_pid" ]]; then
        kill "$server_pid" >/dev/null 2>&1 || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$workdir"
}
trap cleanup EXIT

# After the EXIT trap, which is this library's stated precondition: a `fail` raised
# in a subshell arrives here as SIGTERM, and `e2e_begin` turns it back into an
# ordinary exit so the cleanup above still runs.
e2e_begin "sccache smoke (${protocol})" "$workdir"

# Drawn from below the kernel's ephemeral range, where a connect probe can answer:
# a port above the floor may be an outbound connection's local endpoint with nothing
# listening, so the probe says free and the bind still fails.
#
# What keeps the two protocols apart is `RUN_SERIAL`, NOT `free_port`'s ledger. That
# ledger lives at `${_e2e_workdir}/.issued-ports` and `_e2e_workdir` is the `mktemp -d`
# above, so it is per PROCESS -- and the two smokes are two ctest processes with two
# workdirs. It covers a fixture drawing several ports before binding any of them,
# which is one draw here. Stated because the tempting reading is that the ledger
# makes the pair safe, and a reason that reaches further than the fact behind it is
# what gets a `RUN_SERIAL` deleted later as redundant.
[ -n "$port" ] || port="$(free_port)"

# Driven off the table rather than a second `case`, so the env var and the URL
# scheme cannot drift from the feature name that was just checked.
export "${backendEnv}=${backendScheme}://127.0.0.1:${port}"

# `--log-level=trace --log-everything` is what makes the DIALECT observable, and
# both are required. `Connection.cpp` logs `connection accepted (<flavour>)` at
# Trace, gated on `logEverything`, using `ToStringView(ProtocolFlavor)` -- which is
# documented "suitable for log output" and renders `memcached-text`,
# `memcached-binary`, `redis-resp`. At the `--log-level=info` this fixture used to
# run, that line does not appear at all, which is why nothing could tell a
# text-speaking client from a binary-speaking one.
#
# No production change was needed for this: the daemon has always known which
# dialect it was spoken to and has always been able to say so. Nothing asked.
"$fastcached" --port="$port" --log-level=trace --log-everything > "$daemon_log" 2>&1 &
server_pid=$!
# Waits on the LISTENER, bounded, and says which kind of failure it was: a flat
# `sleep 1` is either flaky on a cold runner or slow on a warm one, and when it is
# too short the failure surfaces later as sccache being unable to reach a backend
# that was merely not up yet.
wait_for_port 127.0.0.1 "$port" "$server_pid" "fastcached" "$daemon_log"

sccache --stop-server >/dev/null 2>&1 || true
# A start-server failure is now a plain failure. The "feature configuration"
# special case that used to live here has moved ABOVE, to a question asked of
# `--help` before anything runs -- because this call does not fail for a missing
# backend, it succeeds and falls back, which is what made the old branch dead.
if ! start_err="$(sccache --start-server 2>&1)"; then
    echo "$start_err" >&2
    exit 1
fi
sccache --zero-stats >/dev/null

# First compile: cache miss, and it is what populates fastcached.
sccache "$compiler" -std=c++23 -c "$src" -o "$obj"
rm -f "$obj"

# DID SCCACHE USE THE BACKEND AT ALL? Asked HERE, between the two compiles, and
# the position is the point rather than tidiness. If sccache silently fell back to
# its local disk cache, the daemon will never see a store, so the bounded wait
# below would expire first and report a TIMEOUT -- true, and a diagnosis of the
# wrong thing. Measured: with the backend export suppressed, the wait failed with
# `alive=yes logGrew=no`, which sends a reader to the daemon rather than to the
# fallback that actually happened. Asking first turns that into a sentence naming
# the cause.
#
# Asserted POSITIVELY, by the configured backend's name. The tempting negative --
# "does not say Local disk" -- fails OPEN the day sccache gains a third fallback,
# because a new name is neither `Local disk` nor the backend and an absence test
# accepts it. That is the bet an exclusion list makes about the shape of the world.
#
# Read from THIS capture, never a scan of surrounding output: a CI job's log holds
# several `Cache location` lines from unrelated sccache calls, two of which say
# `Local disk` even on a leg whose backend is healthy.
firstStats="$(sccache --show-stats)"
location="$(grep -E '^[[:space:]]*Cache location' <<< "$firstStats" || true)"
grep -qE "^[[:space:]]*Cache location[[:space:]]+${backendLocation}" <<< "$location" || {
    echo "sccache smoke (${protocol}) FAILED: sccache did not use the ${protocol} backend" >&2
    echo "  expected 'Cache location' to name '${backendLocation}'." >&2
    echo "  got: ${location:-(no Cache location line at all)}" >&2
    echo "  sccache falls back to its own local disk cache SILENTLY, and a hit from" >&2
    echo "  that cache satisfies the hit assertion with no fastcached involved (#1318)." >&2
    exit 1
}

# WAIT FOR THE STORE TO LAND before compiling again. sccache's write to a REMOTE
# backend is asynchronous: measured on official sccache 0.7.7, the daemon logged
# both lookups BEFORE either store --
#
#   GET .sccache_check / SET .sccache_check
#   GET  key=f/7/9/f7914d2e…          <- compile 1, miss
#   GET  key=f/7/9/f7914d2e…          <- compile 2, miss: the store had not landed
#   SET  key=f/7/9/f7914d2e… STORED   <- both stores arrive afterwards
#   SET  key=f/7/9/f7914d2e… STORED
#
# -- so compiling twice back to back is a RACE, and the fixture would report "no
# cache hit" against a daemon that was working perfectly. Nothing noticed for as
# long as this fixture existed because it never reached a remote backend at all
# (#1318); the local disk cache it was really exercising writes synchronously.
#
# A bounded wait on the daemon's own log, not a sleep: it says what it waited for,
# reports the measured cost, and distinguishes a slow machine from a dead one.
# The marker excludes sccache's `.sccache_check` probe, which also STOREs -- an
# object key is sharded hex (`f/7/9/…`), the probe key is not.
wait_for_log 'storage: SET key=[0-9a-f]/' "$server_pid" \
    "fastcached to store the first compile's object" "$daemon_log"

# Second compile: must now be a cache hit.
sccache "$compiler" -std=c++23 -c "$src" -o "$obj"

stats="$(sccache --show-stats)"
echo "$stats"

fail_smoke() {
    echo "sccache smoke (${protocol}) FAILED: $1" >&2
    shift
    for line in "$@"; do echo "  $line" >&2; done
    exit 1
}

# 1. A hit happened at all. Necessary and, on its own, satisfied by sccache's
#    local disk cache with no daemon anywhere -- which is why it is first of
#    three rather than the whole test.
grep -qE "Cache hits[[:space:]]+[1-9]" <<< "$stats" \
    || fail_smoke "no cache hit" "sccache reported no hit on the second compile."

# 2. The backend was already asserted BETWEEN the compiles, where a fallback can
#    be named as a fallback instead of expiring the store wait. Re-asserted here
#    only because the two captures are different invocations and a backend that
#    changed under us would otherwise go unremarked.
location="$(grep -E '^[[:space:]]*Cache location' <<< "$stats" || true)"
grep -qE "^[[:space:]]*Cache location[[:space:]]+${backendLocation}" <<< "$location" \
    || fail_smoke "sccache stopped using the ${protocol} backend mid-run" \
        "expected 'Cache location' to name '${backendLocation}'." \
        "got: ${location:-(no Cache location line at all)}"

# 3. WHICH DIALECT the daemon was spoken to. The daemon is the only thing that
#    knows: `Cache location` reports the backend TYPE, so memcached-text and
#    memcached-binary are indistinguishable from the client side.
observed="$(grep -oE 'connection accepted \([a-z-]+\)' "$daemon_log" \
            | grep -oE '\([a-z-]+\)' | tr -d '()' | sort -u | tr '\n' ' ' || true)"
observed="${observed% }"

[ -n "$observed" ] \
    || fail_smoke "the daemon logged no accepted connection" \
        "nothing matched 'connection accepted (<flavour>)' in the daemon log." \
        "that line is Trace and gated on --log-everything; both are passed above." \
        "an empty result here means sccache never reached this daemon."

if [ -n "$expectFlavor" ]; then
    # Exact: the caller pinned the sccache and therefore knows the dialect.
    [ "$observed" = "$expectFlavor" ] \
        || fail_smoke "the daemon was spoken to in the wrong dialect" \
            "expected exactly '${expectFlavor}', observed '${observed}'." \
            "this leg is NAMED for its dialect, so a different one passing every" \
            "other assertion would be a false coverage claim (#1318)."
else
    # Family: a developer's sccache version is not knowable here, so assert only
    # that the dialect belongs to this protocol, and PRINT it -- a reader of the
    # log can then see which, which is more than was available before.
    for flavor in $observed; do
        case " $backendFlavors " in
            *" $flavor "*) ;;
            *) fail_smoke "the daemon was spoken to in a foreign dialect" \
                    "observed '${flavor}', which is not a '${protocol}' flavour." \
                    "this protocol's flavours: ${backendFlavors}" ;;
        esac
    done
fi

echo "sccache smoke (${protocol}) OK: cache hit observed, served by ${backendLocation}, dialect ${observed}"
exit 0
