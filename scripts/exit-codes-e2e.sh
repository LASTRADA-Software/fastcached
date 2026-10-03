#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# What each daemon EXITS with, through the real binaries.
#
# Who reads an exit decides what it must be (`Platform/ProcessExit.hpp`): a START is read
# by a supervisor, which restarts 1 and never 78; a ONE-SHOT command -- an install, an
# uninstall, a conversion, the node's worksheet -- is read by an operator, and answers 2
# for a decision a retry reaches again and 1 for what a retry may get past, a named file
# that is absent included, as a start does; a `--healthcheck` is read by a container
# runtime, which knows 0 and 1 only. The tables that say so are unit-tested. What only a
# process can show is the ROUTING: that each refusal in `main` reaches the table with the
# outcome the command line actually named. A site handing the refusal a start's outcome
# for an install's command line stayed green under every Catch2 case and answered 1 and
# 78 again (the review's P-N1), because neither `main` is in a test target.
#
# So every case here is a refusal, and exits before anything is opened, bound, or
# registered: no `--install-service` here reaches a service manager. Each install case
# carries a service name the registration refuses as well, so even a regression that got
# past the refusal under test would decline rather than register -- and the case would
# still fail, on its NEEDLE, since that refusal says something else.
#
# A start that got past its file by mistake would SERVE, so each start case also names a
# rule the start refuses after loading its file (the "bait"), and a control proves the
# bait: a file that loads, then the rule. Every run is bounded besides.
#
# POSIX-only, registered as a named skip on Windows, for `e2e-helpers-selftest`'s reason:
# the bounded runner is the shared POSIX library.
#
# bash 3.2: no `mapfile`, no `declare -A`, no `${var^^}`, no `local -n`.
#
# Usage:
#   scripts/exit-codes-e2e.sh <path-to-fastcached> <path-to-fastcache-compile-node>
#
# Exit codes: 0 = every case behaved. 1 = at least one did not, or none ran.

set -u

FASTCACHED="${1:-}"
NODE="${2:-}"
if [ -z "$FASTCACHED" ] || [ ! -x "$FASTCACHED" ] || [ -z "$NODE" ] || [ ! -x "$NODE" ]; then
    echo "exit-codes-e2e: usage: $0 <path-to-fastcached> <path-to-fastcache-compile-node>" >&2
    exit 1
fi

# shellcheck source=lib/e2e-common.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/e2e-common.sh"

# ASan's leak check is not what this measures, and a refused start leaves arenas alive.
export ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"

WORK="$(mktemp -d 2>/dev/null || mktemp -d -t fcexitcodes)"
trap 'rm -rf "$WORK"' EXIT
e2e_begin "exit-codes e2e" "$WORK"

# Every case names its configuration file, so no machine-wide or per-user file the
# lookup would otherwise find takes part.
printf '# nothing but a comment\n' >"$WORK/empty.yaml"
printf 'scheduler: [1,\n' >"$WORK/malformed.yaml"
ABSENT="$WORK/definitely-not-here.yaml"
EMPTY="--config=$WORK/empty.yaml"

# The daemon's bait: a keyspace-event grammar the serving rules refuse, 78.
DAEMON_BAIT="--notify-keyspace-events=KZ"
DAEMON_BAIT_SAYS="unknown flag character 'Z'"
# The node's: a TYPED --discovery on a node that turned consensus off, which its startup rules
# refuse, 78. Arrays, so `check-node-fixture-starts` reads the flags each start names: the empty
# --listen-raft= and the --cluster-dir every fixture start owes.
NODE_BAIT=(--listen-node=127.0.0.1:6674 --scheduler=127.0.0.1:6674 --listen-raft= --discovery=255.255.255.255:6681)
NODE_BAIT_SAYS="--discovery needs --listen-raft"
NODE_STATE=("--cluster-dir=$WORK/state")
# A name every registration refuses, the belt on every install and uninstall.
REFUSED_NAME="--service-name=a/b"

# How long one refusal may take. It opens nothing, so this is a bound on a hang, not a
# budget: a case that reaches it has started something it should have refused.
BOUND_SECONDS=30

CASES=0
FAILURES=0

# case_failed <name> <detail...>: one failing CASE is one failure.
case_failed() {
    echo "FAIL: $1"
    shift
    for detail in "$@"; do
        echo "  $detail"
    done
    FAILURES=$((FAILURES + 1))
}

# expect_exit <name> <expected-exit> <expected-substring> <binary> <args...>
#
# The exit exactly, and a substring of what the process said, which is what tells one
# refusal from another that happens to share its code.
expect_exit() {
    name="$1"; want="$2"; needle="$3"; binary="$4"; shift 4
    CASES=$((CASES + 1))
    rc=0
    out="$(run_bounded "$BOUND_SECONDS" "$binary" "$@")" || rc=$?
    outcome="$(e2e_bound_outcome)"
    if [ "$outcome" != "$E2eBoundFinished" ]; then
        case_failed "$name" "the process did not finish inside ${BOUND_SECONDS}s ($outcome): it started something" \
            "output: $out"
        return
    fi
    if [ "$rc" -ne "$want" ]; then
        case_failed "$name" "expected exit $want, got exit $rc" "output: $out"
        return
    fi
    case "$out" in
        *"$needle"*) echo "ok: $name" ;;
        *) case_failed "$name" "expected output to contain: $needle" "output: $out" ;;
    esac
}

# --- fastcached: a START ------------------------------------------------------
expect_exit "fastcached: a start past its file ends at the serving rule (the control)" 78 "$DAEMON_BAIT_SAYS" \
    "$FASTCACHED" "$EMPTY" "$DAEMON_BAIT"
expect_exit "fastcached: a start with an absent named file fails, and is retried" 1 "FileNotFound" \
    "$FASTCACHED" "--config=$ABSENT" "$DAEMON_BAIT"
expect_exit "fastcached: a start with a malformed file is refused" 78 "ParseError" \
    "$FASTCACHED" "--config=$WORK/malformed.yaml" "$DAEMON_BAIT"
expect_exit "fastcached: a start whose command line does not parse is refused" 78 "--no-such-flag" \
    "$FASTCACHED" "--no-such-flag"

# --- fastcached: a ONE-SHOT command ends by the step, as a start does ------------
expect_exit "fastcached: an install with an absent named file fails, and may be retried" 1 "FileNotFound" \
    "$FASTCACHED" "--config=$ABSENT" --install-service "$REFUSED_NAME"
expect_exit "fastcached: an install with a malformed file declines" 2 "ParseError" \
    "$FASTCACHED" "--config=$WORK/malformed.yaml" --install-service "$REFUSED_NAME"
expect_exit "fastcached: an install the serving rules refuse declines" 2 "$DAEMON_BAIT_SAYS" \
    "$FASTCACHED" "$EMPTY" --install-service "$DAEMON_BAIT" "$REFUSED_NAME"
expect_exit "fastcached: an uninstall with a malformed file declines" 2 "ParseError" \
    "$FASTCACHED" "--config=$WORK/malformed.yaml" --uninstall-service "$REFUSED_NAME"
# Off Windows and macOS the registration's own stub declines before the name is read, so
# the needle is the ending's and not the name rejection's text.
expect_exit "fastcached: an uninstall that changes nothing declines" 2 "" \
    "$FASTCACHED" "$EMPTY" --uninstall-service "$REFUSED_NAME"
expect_exit "fastcached: a conversion with an absent named file fails, and may be retried" 1 "FileNotFound" \
    "$FASTCACHED" "--config=$ABSENT" --migrate-storage
expect_exit "fastcached: a conversion with no store to convert declines" 2 "needs --storage" \
    "$FASTCACHED" "$EMPTY" --migrate-storage

# --- fastcached: the verb anywhere in a command line that does not parse -------
expect_exit "fastcached: a bad flag BEFORE the install is an install's refusal" 2 "--no-such-flag" \
    "$FASTCACHED" --no-such-flag --install-service
expect_exit "fastcached: a bad flag AFTER the install is an install's refusal" 2 "--no-such-flag" \
    "$FASTCACHED" --install-service --no-such-flag

# --- fastcached: a PROBE answers 0 or 1 only ------------------------------------
expect_exit "fastcached: a health probe with an absent named file is unhealthy" 1 "FileNotFound" \
    "$FASTCACHED" "--config=$ABSENT" --healthcheck
expect_exit "fastcached: a health probe with a malformed file is unhealthy, never 78" 1 "ParseError" \
    "$FASTCACHED" "--config=$WORK/malformed.yaml" --healthcheck
expect_exit "fastcached: a health probe whose command line does not parse is unhealthy" 1 "--no-such-flag" \
    "$FASTCACHED" --no-such-flag --healthcheck

# --- fastcache-compile-node: a START -------------------------------------------
expect_exit "node: a start past its file ends at the startup rule (the control)" 78 "$NODE_BAIT_SAYS" \
    "$NODE" "$EMPTY" "${NODE_STATE[@]}" "${NODE_BAIT[@]}"
expect_exit "node: a start with an absent named file fails, and is retried" 1 "FileNotFound" \
    "$NODE" "--config=$ABSENT" "${NODE_STATE[@]}" "${NODE_BAIT[@]}"
expect_exit "node: a start whose command line does not parse is refused" 78 "--no-such-flag" \
    "$NODE" --no-such-flag

# --- fastcache-compile-node: a ONE-SHOT command ends by the step, as a start does -
expect_exit "node: the worksheet with an absent named file fails, and may be retried" 1 "FileNotFound" \
    "$NODE" "--config=$ABSENT" --print-surfaces
expect_exit "node: the worksheet with a malformed file declines" 2 "ParseError" \
    "$NODE" "--config=$WORK/malformed.yaml" --print-surfaces
expect_exit "node: a bad flag BEFORE the worksheet is the worksheet's refusal" 2 "--no-such-flag" \
    "$NODE" --no-such-flag --print-surfaces
expect_exit "node: a bad flag AFTER the worksheet is the worksheet's refusal" 2 "--no-such-flag" \
    "$NODE" --print-surfaces --no-such-flag
expect_exit "node: a conversion with no store to convert declines" 2 "needs --cache-dir" \
    "$NODE" "$EMPTY" --migrate-cache
expect_exit "node: an uninstall that changes nothing declines" 2 "" \
    "$NODE" "$EMPTY" --uninstall-service "$REFUSED_NAME"

# --- fastcache-compile-node: the NETWORK verbs, by where the answer came from ----
#
# A port nothing answers on (`free_port`, from below the ephemeral range): a loopback
# dial there is refused at once, so each is a transport that delivered no reply --
# transient, 1, the exit a script retries. Beside each, a refusal made HERE before any
# dial -- 2, a decision. A refusal the PEER replied needs a live peer, and is asserted at
# the runners' seams (`ClusterAdminCli_test`, `CordonCli_test`, `EnrollClient_test`).
NOBODY="127.0.0.1:$(free_port)"
expect_exit "node: a cluster command no scheduler answered is transient" 1 "cannot reach" \
    "$NODE" "$EMPTY" --cluster-status "--scheduler=$NOBODY"
expect_exit "node: a cluster command with nowhere to ask declines" 2 "--scheduler names where to ask" \
    "$NODE" "$EMPTY" --cluster-status
expect_exit "node: an enrollment command no scheduler answered is transient" 1 "cannot reach" \
    "$NODE" "$EMPTY" --enroll-list "--scheduler=$NOBODY"
expect_exit "node: an enrollment command with nowhere to ask declines" 2 "--scheduler names where to ask" \
    "$NODE" "$EMPTY" --enroll-list
expect_exit "node: a machine asking a seed nobody answers is transient" 1 "cannot reach the seed" \
    "$NODE" "$EMPTY" "--enroll-from=$NOBODY" "--cluster-dir=$WORK/joiner"
expect_exit "node: a machine asking a seed that is no address declines" 2 "is not an address to dial" \
    "$NODE" "$EMPTY" "--enroll-from=10.0.0.1" "--cluster-dir=$WORK/joiner"
expect_exit "node: a cordon this machine's node never answered is transient" 1 "cannot reach this machine's node" \
    "$NODE" "$EMPTY" --cordon "--listen-node=$NOBODY"

# A run that judged nothing must not read like one that judged everything.
echo "exit-codes-e2e: $CASES case(s) ran"
if [ "$CASES" -eq 0 ]; then
    echo "exit-codes-e2e: no case ran" >&2
    exit 1
fi
if [ "$FAILURES" -ne 0 ]; then
    echo "exit-codes-e2e: $FAILURES of $CASES case(s) failed" >&2
    exit 1
fi
echo "exit-codes-e2e: every case behaved"
