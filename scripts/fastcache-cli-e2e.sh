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
#   * **A real `fastcache-compile-node`**, which speaks the `0xFC` wire and NOTHING
#     else. No scripted exchange can establish what that binary does to a RESP client,
#     and what it does is the whole reason the node verbs exist: it closes having sent
#     nothing. Cases 11-13 are that.

set -uo pipefail

FASTCACHED=""
CLI=""
NODE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) FASTCACHED="$2"; shift 2 ;;
        --cli)        CLI="$2";        shift 2 ;;
        --node)       NODE="$2";       shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

readonly SKIP=77
[[ -n "$FASTCACHED" && -x "$FASTCACHED" ]] || { echo "fastcached not found: '$FASTCACHED'; skipping"; exit "$SKIP"; }
[[ -n "$CLI" && -x "$CLI" ]] || { echo "fastcache-cli not found: '$CLI'; skipping"; exit "$SKIP"; }

# The node is OPTIONAL: `FASTCACHED_BUILD_NODE` can be off, and the eight cases that
# need no node are worth running anyway. Cases 11-13 announce that they were skipped
# rather than passing silently -- a green run that quietly covered less is the shape
# this repository keeps paying for.
HAVE_NODE=0
[[ -n "$NODE" && -x "$NODE" ]] && HAVE_NODE=1

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
# case 1b: `help` answers about a topic, and refuses a word that names nothing
# ---------------------------------------------------------------------------------
# The unit tests cover the parse and the rendering; this covers the half that is the
# product -- the EXIT CODE. `help nosuchverb` printed 114 lines and exited **0** while
# the bare `nosuchverb` exits 2, so the operand said something and was heard by nobody.
echo "==> case 1b: help, with and without a topic"

run_cli help
expect_status 0 "bare help still answers"
expect_stdout "EXIT CODES" "the whole help carries every section"

run_cli help get
expect_status 0 "help with a known topic answers"
expect_stdout "on a compile node" "the topic page carries the per-verb details"
# It is a PAGE, not the whole help: both contain the word `get`, so asserting that
# would pass under the discard this case exists for.
refute_stdout "EXIT CODES" "a topic page is one verb, not the whole text"

run_cli help nosuchverb
expect_status 2 "a topic naming no command is a usage error"
expect_stderr "nosuchverb" "and the refusal names the word that was typed"

# The other settled question. `--version get` printed the version and exited 0.
run_cli --version get
expect_status 2 "a question that takes no topic refuses one"
expect_stderr "--version" "and names the flag it is about"

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


# ---------------------------------------------------------------------------------
# case 9: the memcached-text verbs against a real daemon
#
# What this reaches that `fastcache-cli-tests` cannot is the SERVER: every unit case
# scripts the reply this client expects, so a verb whose argument order or command
# word is wrong passes there and is refused by the daemon here. `gat`'s reversed
# operand order and the lower-case command words are exactly that class.
# ---------------------------------------------------------------------------------
echo "==> case 9: the memcached verbs reach the daemon"
start_daemon

run_cli set mc-key hello
expect_status 0 "a value to work on"

run_cli touch mc-key 300
expect_status 0 "touch on a live key"

run_cli touch no-such-key 300
# The server answered NOT_FOUND. That is a miss (1), not unreachable (3) and not a
# refusal (4) -- the discrimination the exit-code table exists for.
expect_status 1 "touch on an absent key"
expect_stderr "no such key" "touch says why it answered no"

run_cli inspect mc-key --format=kv
expect_status 0 "inspect on a live key"
# The renamed flags, and the wire spellings gone. `me` writes `exp=` and `la=`; a
# reader gets `ttl_seconds` and `last_access_seconds`, and asserting the ABSENCE of
# the wire names is what makes this fail if the rename stops happening.
expect_stdout "ttl_seconds=" "inspect renames the ttl flag"
expect_stdout "value_bytes=" "inspect renames the size flag"
expect_stdout "cas=" "inspect reports the cas token"
refute_stdout "exp=" "the wire spelling of the ttl flag is gone"
refute_stdout "la=" "the wire spelling of the last-access flag is gone"

run_cli inspect no-such-key
expect_status 1 "inspect on an absent key"

# `add` refuses a key that exists and `replace` refuses one that does not -- opposite
# conditions, so a handler that got them the wrong way round passes one and fails the
# other rather than both.
run_cli add mc-key second
expect_status 1 "add onto an existing key"
expect_stderr "needs the key absent" "add says which condition failed"

run_cli add fresh-key first
expect_status 0 "add onto an absent key"

run_cli replace no-such-key v
expect_status 1 "replace onto an absent key"

# The value begins with a dash, which is what `--` is for: without it the option
# parser reads it as a flag. Written this way deliberately rather than avoided -- a
# cache stores arbitrary bytes, so a leading dash is ordinary, and this is the only
# case in the suite that exercises the escape.
run_cli append -- mc-key "-suffix"
expect_status 0 "append onto an existing key, with a value that starts with a dash"
run_cli get mc-key
expect_stdout "hello-suffix" "append landed at the end"

run_cli prepend -- mc-key "prefix-"
expect_status 0 "prepend onto an existing key"
run_cli get mc-key
expect_stdout "prefix-hello-suffix" "prepend landed at the front"

# gat: the expiry comes FIRST, which is the wire's order and the opposite of touch.
# A row that reordered them would be refused by the daemon as a bad exptime, which is
# a failure only a live server can produce.
run_cli gat 300 mc-key
expect_status 0 "gat with the expiry first"
expect_stdout "mc-key" "gat named the key it returned"

run_cli gat 300 no-such-key
expect_status 1 "gat where nothing exists"
expect_stderr "none of the keys exist" "gat says the wire named no misses"

# cas, end to end: read the token the server actually issued, spend it, then spend it
# again. The second attempt must be refused -- a compare-and-swap that accepts a
# stale token is the one failure mode this verb exists to prevent, and it cannot be
# demonstrated without a server that issues real tokens.
run_cli gats 300 mc-key --format=kv
expect_status 0 "gats reports a cas token"
# Not `sed ... | head -1`: `head` leaves after the first line, `sed` takes SIGPIPE,
# and under pipefail the pipeline reports SED status -- a false negative on the
# SUCCESS path. Capture every match and take the first line with no fork.
casMatches="$(sed -n 's/^cas=\([0-9][0-9]*\).*/\1/p' "$WORK/out")"
casToken="${casMatches%%$'\n'*}"
[[ -n "$casToken" ]] || {
    e2e_note "stdout: $(cat "$WORK/out")"
    fail "gats did not report a cas token to spend"
}
e2e_note "the server issued cas token $casToken"

run_cli cas mc-key swapped "$casToken"
expect_status 0 "cas with the token the server issued"
run_cli get mc-key
expect_stdout "swapped" "the compare-and-swap stored the new value"

run_cli cas mc-key again "$casToken"
expect_status 1 "cas with a token the value has outlived"
expect_stderr "changed since that cas token" "the stale cas says why"

# mc-stats reaches families `stats` cannot see.
run_cli mc-stats settings --format=kv
expect_status 0 "mc-stats settings"
settingsRows=$(count_lines "$WORK/out")
[[ "$settingsRows" -gt 0 ]] || fail "mc-stats settings rendered nothing"
e2e_note "mc-stats settings rendered $settingsRows rows"

run_cli mc-stats reset
# Refused by THIS CLIENT, before anything is sent: the daemon answers `RESET` while
# resetting nothing, so relaying it would report a reset that did not happen.
expect_status 2 "mc-stats reset is refused rather than relayed"
expect_stderr "not a stats family this client offers" "the refusal lists the families"

# A value that begins with a dash and NO `--`: refused, and the refusal names the
# escape. The bare "unrecognised argument" left an operator reading a flag list for a
# flag they never typed, which is the one case where a leading dash is ordinary.
run_cli append mc-key "-suffix"
expect_status 2 "a dash-leading value without -- is a usage error"
expect_stderr "put \`--\` before the operands" "the refusal names the -- escape"

run_cli cache-memlimit 512
expect_status 0 "cache-memlimit is accepted"
expect_stderr "NOT persisted" "cache-memlimit says the change does not survive a restart"

# ---------------------------------------------------------------------------------
# case 10: the memcached verbs under --requirepass
#
# **The one behaviour no unit test can establish**, because it is the SERVER's: the
# memcached text protocol has no AUTH verb, so this daemon answers every verb but
# `version` and `quit` with `CLIENT_ERROR authentication required` and ENDS THE
# SESSION. The client cannot fix that with a credential, and the case asserts both
# that it is refused and that the refusal explains why -- the server's own sentence
# does not mention that the protocol lacks the verb.
# ---------------------------------------------------------------------------------
echo "==> case 10: a memcached verb cannot authenticate, and says so"
start_daemon --requirepass s3cret

# WITH the credential configured, which is the case that matters: a client holding a
# valid token still cannot run these, and it must not be told to supply one.
run_cli --token-file="$tokenFile" touch mc-key 300
expect_status 4 "touch against a password-protected daemon"
expect_stderr "authentication required" "the server's own sentence is relayed"
expect_stderr "has no AUTH verb" "the refusal explains that no credential can help"

# The RESP verbs on the same address DO authenticate, which is what the advisory
# tells the operator to reach for -- asserted rather than merely promised.
run_cli --token-file="$tokenFile" set mc-key v
expect_status 0 "a RESP verb with the same credential on the same address"

# ---------------------------------------------------------------------------------
# cases 11-13: against a real `fastcache-compile-node`
#
# **What no scripted exchange can establish.** `fastcache-compile-node` speaks the
# `0xFC` compile-cache wire and nothing else, and what it DOES to a RESP client is a
# property of that binary: it closes having sent a single byte. Measured, not assumed.
# ---------------------------------------------------------------------------------
if [[ "$HAVE_NODE" -eq 0 ]]; then
    e2e_note "cases 11-13 SKIPPED: fastcache-compile-node was not built"
else
    echo "==> case 11: the node verbs against a real compile node"

    nodePort="$(free_port)"
    nodeAdminPort="$(free_port)"
    nodeLog="$WORK/node-$nodePort.log"
    # `--scheduler` naming itself: this node schedules nothing and registers with
    # nobody, which is fine here and is what the startup refusal demands be stated.
    # `--no-toolchain-discovery` with an explicit `--toolchain` keeps the include-tree
    # walk out of the fixture -- it is over 300 s cold and measures nothing this case
    # is about.
    "$NODE" --listen-node="127.0.0.1:$nodePort" \
            --admin-listen="127.0.0.1:$nodeAdminPort" \
            --scheduler="127.0.0.1:$nodePort" \
            --no-toolchain-discovery --toolchain=cc > "$nodeLog" 2>&1 &
    nodePid=$!
    _CLI_E2E_PIDS="$_CLI_E2E_PIDS $nodePid"
    wait_for_port 127.0.0.1 "$nodePort" "$nodePid" "fastcache-compile-node" "$nodeLog"

    # `run_cli` dials `$port`, which is the daemon's. The node is a different address,
    # so these run it directly -- and the helper's redirections are kept so every
    # `expect_*` above still applies.
    run_node() {
        set +e
        "$CLI" --addr="127.0.0.1:$nodePort" "$@" > "$WORK/out" 2> "$WORK/err"
        status=$?
        set -e
        return 0
    }

    run_node node
    expect_status 0 "the node answers node-status"
    expect_stdout "components" "the node reports which components it runs"
    expect_stdout_line "^admin-port +$nodeAdminPort\$" "the reported admin port is the one it bound"

    # **Absent is not zero, end to end.** This node runs no consensus, so it has no
    # minted identity -- and the JSON must carry `null` rather than an empty string
    # somebody could paste into `--raft-peer`.
    run_node node --format=json
    expect_status 0 "node renders as JSON"
    expect_stdout '"node-id":null' "an unminted identity is null, not an empty string"

    # A surface it does not run gets NO field. Discovery is off here, so a
    # `discovery-port` of any value -- including 0 -- is the defect.
    refute_stdout "discovery-port" "a surface the node does not run gets no field at all"

    run_node node-metrics
    expect_status 0 "the node answers node-metrics"
    # The zero rows are PRESENT. A freshly started node has served nothing, so a
    # client that dropped zeroes would report almost nothing here and still exit 0 --
    # which is the failure a check for one non-zero counter cannot see.
    expect_stdout_line "_total +0\$" "a counter that never moved is reported as 0"

    echo "==> case 12: version is ANSWERED by a node, not merely explained"

    # The verb the whole fallback exists for. Before it, this reported *the server
    # closed the connection without answering* -- true, and useless.
    run_node version
    expect_status 0 "version against a node"
    expect_stdout "server_kind" "the answer says WHICH kind of server replied"
    expect_stdout "fastcache-compile-node" "and names it"

    echo "==> case 13: a cache verb is refused BY NAME, never left to close in silence"

    # THE discrimination this whole change is about. A compile node holds no user
    # keyspace, so `get` has no 0xFC equivalent -- and the operator must be told what
    # the endpoint IS rather than that a connection closed.
    run_node get some-key
    expect_status 4 "a cache verb against a node is refused by name, not unreachable"
    expect_stderr "fastcache-compile-node" "the refusal names what the endpoint is"
    expect_stderr "holds no user keyspace" "and why the verb cannot be answered"

    # The stats ladder reaches the node's own counters, and it DISCOVERED the admin
    # port over 0xFC -- no `--admin-addr` was given anywhere in this case.
    run_node stats --format=kv
    expect_status 0 "stats against a node with no --admin-addr"
    expect_stdout_line "^source=(metrics|node-metrics)\$" "a 0xFC-reachable rung answered"

    # And the rung that answered is NOT `info`: RESP is unreachable here, so an `info`
    # source would mean the ladder had somehow answered from a wire this binary does
    # not speak.
    refute_stdout "source=info" "the ladder did not claim RESP INFO answered"
fi

echo "fastcache-cli E2E OK"
