#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end fixture for `fastcache-cli` against a real `fastcached`.
#
# What the unit tests cannot reach, and therefore what this is for:
#
#   * **The exit codes, against a live server.** A cache miss must exit 1 and an
#     unreachable daemon must exit 3. That discrimination is the whole reason the
#     outcome table has six rows rather than two, and a scripted exchange can only
#     prove the mapping -- not that the real refusals arrive as the right kind.
#   * **The stats ladder's rungs.** `/metrics` and `INFO` are on different ports and
#     answer with wildly different amounts. Which one wins, and whether the loser is
#     reported as *not asked* rather than *did not answer*, is a property of two real
#     listeners.
#   * **`--raw` byte fidelity**, which on Windows depends on stdout being switched out
#     of text mode. A value containing a newline is corrupted otherwise, on exactly
#     one platform.
#   * **The authentication gate**, whose interesting arm needs a daemon configured to
#     demand a credential.

set -uo pipefail

FASTCACHED=""
CLI=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) FASTCACHED="$2"; shift 2 ;;
        --cli)        CLI="$2";        shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77
[[ -n "$FASTCACHED" && -x "$FASTCACHED" ]] || { echo "fastcached not found: '$FASTCACHED'; skipping"; exit "$SKIP"; }
[[ -n "$CLI" && -x "$CLI" ]] || { echo "fastcache-cli not found: '$CLI'; skipping"; exit "$SKIP"; }

WORK="$(mktemp -d)"

# Reaps the daemons BEFORE removing the work tree. Every `fail` between those two
# points -- including the one a wait raises on expiry -- exits without reaching the
# kill, which would leave a daemon running and then delete its directory underneath it.
_CLI_E2E_PIDS=""
_cli_e2e_cleanup() {
    local pid
    for pid in $_CLI_E2E_PIDS; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    rm -rf "$WORK"
}
trap _cli_e2e_cleanup EXIT

# Sourced after the EXIT trap is installed, because `e2e_begin` installs a TERM trap
# whose whole purpose is to let the EXIT trap run.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"
e2e_begin "fastcache-cli E2E" "$WORK"

# EVERY daemon invocation names this file. Without it the daemon finds whatever
# machine-wide configuration the host has, and on a developer box with a real
# `storage_path:` in it these cases would run against their actual cache.
EMPTY_CONFIG="$WORK/empty.yaml"
: > "$EMPTY_CONFIG"

# ---------------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------------

# Start a daemon and wait for it to answer. Sets `port` and `metricsPort`.
# $@ extra daemon arguments.
start_daemon() {
    port="$(free_port)"
    metricsPort="$(free_port)"
    local log="$WORK/daemon-$port.log"
    "$FASTCACHED" --config "$EMPTY_CONFIG" --port="$port" \
        --metrics --metrics-port="$metricsPort" "$@" > "$log" 2>&1 &
    local pid=$!
    # Tracked before anything can fail, so the EXIT trap reaps it on every path.
    _CLI_E2E_PIDS="$_CLI_E2E_PIDS $pid"
    wait_for_port 127.0.0.1 "$port" "$pid" "fastcached" "$log"
}

# Run the CLI against the current daemon and record its status in `status`.
# Its stdout lands in `$WORK/out` and its stderr in `$WORK/err`, so a case can
# assert on either -- which matters here, because the remarks are deliberately not
# on stdout.
# $@ CLI arguments.
run_cli() {
    set +e
    "$CLI" --addr="127.0.0.1:$port" "$@" > "$WORK/out" 2> "$WORK/err"
    status=$?
    set -e
    return 0
}

# Assert the last `run_cli` exited with a given status.
# $1 expected status, $2 what was being checked.
expect_status() {
    local expected="$1" what="$2"
    if [[ "$status" != "$expected" ]]; then
        e2e_note "stdout: $(cat "$WORK/out")"
        e2e_note "stderr: $(cat "$WORK/err")"
        fail "$what: expected exit $expected, got $status"
    fi
}

# Assert the last run's stdout contains a fixed string.
# $1 the string, $2 what was being checked.
expect_stdout() {
    grep -qF -- "$1" "$WORK/out" || {
        e2e_note "stdout: $(cat "$WORK/out")"
        fail "$2: stdout does not contain '$1'"
    }
}

# Assert the last run's stdout does NOT contain a fixed string.
# $1 the string, $2 what was being checked.
refute_stdout() {
    if grep -qF -- "$1" "$WORK/out"; then
        e2e_note "stdout: $(cat "$WORK/out")"
        fail "$2: stdout unexpectedly contains '$1'"
    fi
}

# Assert the last run's stdout has a line matching an anchored pattern.
#
# A substring check cannot do this job: `ttl=` is a prefix of `ttl=299`, so
# `grep -F ttl=` cannot tell an absent expiry from a live one -- which is exactly the
# distinction these cases exist to make.
# $1 the extended regular expression, $2 what was being checked.
expect_stdout_line() {
    grep -qE -- "$1" "$WORK/out" || {
        e2e_note "stdout: $(cat "$WORK/out")"
        fail "$2: no stdout line matches /$1/"
    }
}

# Assert the last run's stderr contains a fixed string.
# $1 the string, $2 what was being checked.
expect_stderr() {
    grep -qF -- "$1" "$WORK/err" || {
        e2e_note "stderr: $(cat "$WORK/err")"
        fail "$2: stderr does not contain '$1'"
    }
}

# ---------------------------------------------------------------------------------
# case 1: the daemon answers at all
# ---------------------------------------------------------------------------------
echo "==> case 1: ping"
start_daemon
run_cli ping
expect_status 0 "ping"
expect_stdout "PONG" "ping"

# ---------------------------------------------------------------------------------
# case 2: a store and a read back
# ---------------------------------------------------------------------------------
echo "==> case 2: set, get, del"
run_cli set greeting hello
expect_status 0 "set"
run_cli get greeting
expect_status 0 "get"
expect_stdout "hello" "get"
run_cli del greeting
expect_status 0 "del"
expect_stdout "1" "del reports the count"

# ---------------------------------------------------------------------------------
# case 3: THE discrimination -- a miss is not an unreachable server
#
# The single most important assertion in this file. A script that retries one and
# gives up on the other cannot be written if the two share an exit code, and every
# cache client that conflates them has taught its users to ignore failures.
# ---------------------------------------------------------------------------------
echo "==> case 3: a miss exits 1, an unreachable daemon exits 3"
run_cli get definitely-not-stored
expect_status 1 "a miss"
expect_stderr "no such key" "a miss says which key"

# A port nothing serves. Deliberately port 1, which is refused immediately rather
# than timing out, so this case costs microseconds instead of the connect budget.
set +e
"$CLI" --addr=127.0.0.1:1 get greeting > "$WORK/out" 2> "$WORK/err"
status=$?
set -e
expect_status 3 "an unreachable daemon"

# And a usage error is neither of those, nor a success.
set +e
"$CLI" --addr="127.0.0.1:$port" no-such-verb > "$WORK/out" 2> "$WORK/err"
status=$?
set -e
expect_status 2 "an unknown verb"

# ---------------------------------------------------------------------------------
# case 4: every format renders, and an absence stays an absence
# ---------------------------------------------------------------------------------
echo "==> case 4: the five formats"
run_cli set a 1
expect_status 0 "set a"

for format in human json kv tsv csv; do
    run_cli mget a missing --format="$format"
    expect_status 0 "mget --format=$format"
    [[ -s "$WORK/out" ]] || fail "mget --format=$format wrote nothing"
done

# JSON's absent cell is a real null -- never a 0, which would be a claim, and never
# the string "-", which would parse and lie.
run_cli mget a missing --format=json
expect_stdout '"value":null' "json renders an absence as null"
refute_stdout '"value":0' "json must not render an absence as zero"
refute_stdout '"value":"-"' "json must not render an absence as a dash"

# Validated with a real parser where one is available. Absent is reported as its own
# outcome rather than silently passing, because "no parser here" and "the document
# parsed" are different things.
if command -v python3 > /dev/null 2>&1; then
    python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert isinstance(d,list) and d[1]["value"] is None, d' \
        "$WORK/out" || fail "the json output is not a parseable document with a null"
    e2e_note "json validated with python3"
else
    e2e_note "python3 absent: the json document was matched textually and not parsed"
fi

# ---------------------------------------------------------------------------------
# case 5: ttl's three states are separable on stdout
#
# `-2` means no such key and `-1` means exists-with-no-expiry. As a bare number both
# render as an absence and are separable only by the exit code, which is a state
# collapse in the one verb whose job is that distinction.
# ---------------------------------------------------------------------------------
echo "==> case 5: ttl reports three states"
run_cli set withttl v --ttl=300
expect_status 0 "set --ttl"

run_cli ttl withttl --format=kv
expect_status 0 "ttl of a key with an expiry"
expect_stdout "exists=true" "ttl: the key exists"
expect_stdout_line '^ttl=[0-9]+$' "ttl: a live expiry is a number"

run_cli ttl a --format=kv
expect_status 0 "ttl of a key with no expiry"
expect_stdout "exists=true" "ttl: the key exists"
# Anchored and empty: the key exists and has NO expiry. Distinguished from the case
# above only by what follows the `=`, which is why the pattern is anchored.
expect_stdout_line '^ttl=$' "ttl: no expiry renders as an absence"

run_cli ttl gone --format=kv
expect_status 1 "ttl of a key that does not exist"
expect_stdout "exists=false" "ttl: the key does not exist"
# All three states share an absent-or-numeric `ttl`; `exists` plus the exit code is
# what separates them, and asserting only one of those would pass under a collapse.
expect_stdout_line '^ttl=$' "ttl: a missing key reports no expiry"

# ---------------------------------------------------------------------------------
# case 6: --raw hands over the bytes unaltered
#
# On Windows this depends on stdout being switched out of text mode: otherwise every
# newline becomes CRLF and the value comes back longer than it went in. The
# assertion is therefore on the BYTE COUNT, which is what a translation changes and
# what a string comparison in a shell would hide.
# ---------------------------------------------------------------------------------
echo "==> case 6: --raw is byte-exact"
printf 'line1\nline2\n' > "$WORK/payload"
run_cli set multiline "$(cat "$WORK/payload")"
expect_status 0 "set a multi-line value"

set +e
"$CLI" --addr="127.0.0.1:$port" get multiline --raw > "$WORK/raw" 2> "$WORK/err"
status=$?
set -e
expect_status 0 "get --raw"
# `$(cat)` strips the trailing newline, so what was stored is the payload without it.
#
# `count_bytes`, not `$(wc -c < ...)`: BSD `wc` pads its count with blanks, so a
# captured `11` is `      11` on macOS and the string comparison below failed there
# while passing on Linux -- reporting `wrote       11 bytes, expected 11`, whose two
# numbers are equal. The `expected` side never had the bug because `$(( ))`
# normalises, which is exactly why only one of the two spellings was suspicious.
expected=$(( $(count_bytes "$WORK/payload") - 1 ))
actual=$(count_bytes "$WORK/raw")
# The message names BOTH causes rather than guessing one. It read "(newline
# translation?)" and the failure it actually reported was a padded count -- so a
# reader on macOS was pointed at a Windows text-mode bug, with two equal numbers on
# screen. A diagnosis in a failure message is a claim like any other.
if [[ "$actual" != "$expected" ]]; then
    fail "--raw wrote [$actual] bytes, expected [$expected] -- newline translation, or a count that is not a bare integer"
fi

# ---------------------------------------------------------------------------------
# case 7: the stats ladder, both rungs
# ---------------------------------------------------------------------------------
echo "==> case 7: stats names its source"
run_cli stats --format=kv
expect_status 0 "stats with no admin address"
expect_stdout "source=info" "stats falls back to INFO"
# NOT ASKED, not *did not answer*. Reporting a failure for an endpoint nothing
# dialled sends an operator to check a listener that was never contacted.
expect_stderr "was not asked" "stats says the rich source was not asked"
if grep -qF "did not answer" "$WORK/err"; then
    e2e_note "stderr: $(cat "$WORK/err")"
    fail "stats reported 'did not answer' for a source it never dialled"
fi
infoRows=$(count_lines "$WORK/out")

run_cli --admin-addr="127.0.0.1:$metricsPort" stats --format=kv
expect_status 0 "stats with an admin address"
expect_stdout "source=metrics" "stats prefers /metrics"
metricsRows=$(count_lines "$WORK/out")

# The ladder is only worth having if the rungs differ, so the difference is asserted
# rather than assumed.
# ROWS, not fields: this counts what the CLI RENDERED, which carries the `source`
# field the ladder prepends. The daemon's INFO handler emits one fewer, and calling
# both numbers "fields" is how a wrong count reached an advisory. What the case
# asserts is the ORDERING, which needs no agreement on either figure.
[[ "$metricsRows" -gt "$infoRows" ]] \
    || fail "/metrics rendered $metricsRows rows and INFO $infoRows; the ladder buys nothing"
e2e_note "INFO rendered $infoRows rows, /metrics rendered $metricsRows"

# ---------------------------------------------------------------------------------
# case 8: the authentication gate
#
# Its own daemon, because `--requirepass` is a startup decision.
# ---------------------------------------------------------------------------------
echo "==> case 8: a credential is demanded and accepted"
start_daemon --requirepass s3cret

run_cli get anything
# The server answered and declined. That is `refused` (4), not unreachable (3) and
# not a miss (1) -- three different problems with three different fixes.
expect_status 4 "an unauthenticated read"
expect_stderr "FASTCACHE_TOKEN" "the refusal says how to supply a credential"

tokenFile="$WORK/token"
printf 's3cret\n' > "$tokenFile"
run_cli --token-file="$tokenFile" set secret-key v
expect_status 0 "a credentialled store"
run_cli --token-file="$tokenFile" get secret-key
expect_status 0 "a credentialled read"
expect_stdout "v" "the credentialled read returned the value"

# A credential offered to a server that wants none is a REMARK, not a failure: the
# command still runs. Refusing would give a client with a token configured a
# permanent failure against a server that never needed one.
echo "==> case 8b: a credential the server does not want is a remark"
start_daemon
run_cli --token-file="$tokenFile" ping
expect_status 0 "ping with an unnecessary credential still succeeds"
expect_stderr "does not require one" "the unnecessary credential is reported"

echo "fastcache-cli E2E OK"
