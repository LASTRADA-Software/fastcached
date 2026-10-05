#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The compile worker actually reads its configuration file.
#
# `ApplyNodeConfiguration` is unit-tested and correct, and that is not the same
# claim: `PurgeExpired` was correct and tested and had no production caller at
# all. This drives the real binary, so what is asserted is the composition in
# `main` -- the lookup, the read, the apply, and the second parse over it.
#
# It also asks the three questions only a process can answer. "Unreadable" is a
# mode on a file, "absent" is a path that resolves to nothing, and "malformed" is
# a document the reader gives up on; each must produce its own named outcome
# rather than the nearest neighbour, because an operator reads the exit and the
# message and nothing else.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs wherever CI builds. No
# `mapfile`, no `declare -A`, no `${var^^}`, no `local -n`.
#
# Usage:
#   scripts/node-config-file-e2e.sh <path-to-fastcache-compile-node>
#
# Exit codes: 0 = every case behaved. 1 = at least one did not.

set -u

NODE="${1:-}"
if [ -z "$NODE" ] || [ ! -x "$NODE" ]; then
    echo "node-config-file-e2e: usage: $0 <path-to-fastcache-compile-node>" >&2
    exit 1
fi

# ASan's leak check is not what this is measuring, and the worker exits through
# paths that legitimately leave the reactor's arenas alive.
export ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"

# A directory of this run's own. `mktemp -d` rather than a fixed name: the suite
# runs in parallel and two cases sharing a config file is two cases reading each
# other's settings.
WORK="$(mktemp -d 2>/dev/null || mktemp -d -t fcnodecfg)"
trap 'rm -rf "$WORK"' EXIT

FAILURES=0

# `--print-surfaces` is what makes this observable: it renders the RESOLVED
# configuration and exits without opening anything, so what it prints is exactly
# what the merged config says.
#
# The lookup must not reach a real machine-wide file while a case is asserting
# what an EMPTY configuration does, so every invocation names a path.
#
# **Its exit code answers "would this configuration START", not "did the file
# PARSE"** -- and until #582 it answered neither, returning 0 unconditionally. Three
# cases here were reading it as the second claim, which is the one they are about;
# they passed because the flag could not yet make the first. One signal cannot carry
# both questions, so the cases that assert PARSING now supply the flag that startup
# requires, leaving the file's own content the only thing under test.
#
# A node given no flags at all starts -- it serves a fleet of its own and registers its
# worker there -- so a case whose subject is the file needs nothing beside it but a state
# directory of its own, kept out of the account's real one. `--print-surfaces` opens
# nothing, so the directory is never created.
# An array, so `check-node-fixture-starts` reads the --cluster-dir each start names.
STATE_FOR_STARTUP=("--cluster-dir=$WORK/state")

# case_failed <name> <detail...>
#
# One failing CASE is one failure, however many lines explain it: counting the
# detail lines made one case read as "2 failure(s)". Not the shared
# `e2e-common.sh` `fail`, which ends the run -- every case here is reported, and
# the verdict is taken once, at the end.
case_failed() {
    echo "FAIL: $1"
    shift
    for detail in "$@"; do
        echo "  $detail"
    done
    FAILURES=$((FAILURES + 1))
}

# expect_ok <name> <expected-substring> <args...>
expect_ok() {
    name="$1"; needle="$2"; shift 2
    out="$("$NODE" "$@" --print-surfaces 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        case_failed "$name" "expected success, got exit $rc" "output: $out"
        return
    fi
    case "$out" in
        *"$needle"*) echo "ok: $name" ;;
        *) case_failed "$name" "expected output to contain: $needle" "output: $out" ;;
    esac
}

# judge_refusal <name> <expected-exit> <expected-substring> <rc> <out>
judge_refusal() {
    if [ "$4" -ne "$2" ]; then
        case_failed "$1" "expected exit $2, got exit $4" "output: $5"
        return
    fi
    case "$5" in
        *"$3"*) echo "ok: $1" ;;
        *) case_failed "$1" "expected output to contain: $3" "output: $5" ;;
    esac
}

# expect_refusal <name> <expected-exit> <expected-substring> <args...>
#
# The WORKSHEET's refusal: `--print-surfaces` is a one-shot verb, and a one-shot verb
# answers the verbs' usage exit, 2, for EVERY refusal -- a configuration file that
# did not load and a command line that did not parse included. Nothing supervises
# it, so the restart semantics a start's code carries do not apply. The exit column
# stays, so a row states what it expects rather than inheriting it.
expect_refusal() {
    name="$1"; want="$2"; needle="$3"; shift 3
    out="$("$NODE" "$@" --print-surfaces 2>&1)"
    judge_refusal "$name" "$want" "$needle" "$?" "$out"
}

# expect_start_refusal <name> <expected-exit> <expected-substring> <args...>
#
# A START's refusal, which a supervisor reads: 78 is a verdict on what was read,
# which the next start reaches again, so it is not restarted; 1 is an I/O arm -- a
# named file absent or unreadable -- which a boot-time start can race, so it is.
#
# Every one carries STOPS_AT_THE_RULES, a line the startup rules refuse after the
# file is loaded and before anything is opened, so a start that got past the file
# by mistake ends at that rule -- 78, naming it -- instead of serving and hanging
# the fixture. The control below is that line alone, with a file that loads.
#
# The rule is a TYPED --discovery on a node that turned consensus off: it names the empty
# --listen-raft= a fixture owes anyway, so the start could not have joined a fleet even past it.
STOPS_AT_THE_RULES=(--listen-node=127.0.0.1:6674 --listen-raft= --discovery=255.255.255.255:6681)
expect_start_refusal() {
    name="$1"; want="$2"; needle="$3"; shift 3
    out="$("$NODE" "$@" "${STATE_FOR_STARTUP[@]}" "${STOPS_AT_THE_RULES[@]}" 2>&1)"
    judge_refusal "$name" "$want" "$needle" "$?" "$out"
}

# --- a setting in the file takes effect -------------------------------------
#
# `listen_node` rather than something cheaper, because `--print-surfaces` prints
# the address it resolves to: a key that did nothing would leave the cache row
# reading "not served", which is a different line rather than a missing one.
cat >"$WORK/good.yaml" <<'YAML'
cluster_dir: "node-state"
listen_node: "0.0.0.0:6699"
YAML
expect_ok "a setting in the file takes effect" "0.0.0.0:6699" "--config=$WORK/good.yaml"

# --- the command line wins over the file ------------------------------------
expect_ok "the command line wins over the file" "0.0.0.0:6698" \
    "--config=$WORK/good.yaml" "--listen-node=0.0.0.0:6698"

# --- the map opens with the MODE ---------------------------------------------
#
# The recorded mode, not a flag, decides whether consensus runs and which ports open, so
# it is the first thing the map says. Before a first start has minted a record it is the
# solitary one that start will mint -- and `--print-surfaces` writes nothing, so it stays
# that way across calls.
out="$("$NODE" "${STATE_FOR_STARTUP[@]}" --print-surfaces 2>&1)"
first="${out%%$'\n'*}"
first="${first%$'\r'}"
case "$first" in
    "mode: solitary (no cluster minted yet; the first start mints one)") echo "ok: the map opens with the node's mode" ;;
    *)
        echo "FAIL: the map opens with the node's mode"
        report "first line: $first"
        ;;
esac
if [ -e "$WORK/state/formation" ]; then
    echo "FAIL: --print-surfaces minted no formation record"
    report "found $WORK/state/formation"
else
    echo "ok: --print-surfaces minted no formation record"
fi

# --- absent ------------------------------------------------------------------
#
# Named and not there is FILE NOT FOUND, not a parse error: an operator who
# mistyped a path must not be sent hunting for a syntax mistake in a file that
# does not exist.
expect_refusal "a named file that is absent is refused by name, and may be retried" 1 "FileNotFound" \
    "--config=$WORK/definitely-not-here.yaml"

# --- malformed ---------------------------------------------------------------
printf 'listen_node: [1,\n' >"$WORK/malformed.yaml"
expect_refusal "a malformed file is refused as a parse error" 2 "ParseError" \
    "--config=$WORK/malformed.yaml"

# --- the same files at a START, which a supervisor reads --------------------
#
# The worksheet and a start both answer by the ARM -- an I/O arm is transient, a
# verdict on what was read is not -- and only the reader's codes differ: 1 and 2 to
# an operator, 1 and 78 to a supervisor. The control first: the bait line alone,
# over a file that loads, ends at the rule.
printf '# the settings are on the command line\n' >"$WORK/loads.yaml"
expect_start_refusal "a start past its file ends at the startup rule (the control)" 78 \
    "--discovery needs --listen-raft" "--config=$WORK/loads.yaml"
expect_start_refusal "a start with an absent named file fails, and is retried" 1 "FileNotFound" \
    "--config=$WORK/definitely-not-here.yaml"
expect_start_refusal "a start with a malformed file is refused, and is not retried" 78 "ParseError" \
    "--config=$WORK/malformed.yaml"

# --- a command line that does not parse -------------------------------------
#
# The other refusal decided before the verb runs. The verb is what parsed before
# the bad flag, which is where an operator types it; a start with no verb is the
# refusal a supervisor reads.
out="$("$NODE" --print-surfaces --no-such-flag 2>&1)"
judge_refusal "a worksheet whose command line does not parse answers 2" 2 "--no-such-flag" "$?" "$out"
out="$("$NODE" --no-such-flag 2>&1)"
judge_refusal "a start whose command line does not parse is refused" 78 "--no-such-flag" "$?" "$out"

# --- a key nothing reads -----------------------------------------------------
printf 'schedular: "typo:6675"\n' >"$WORK/typo.yaml"
expect_refusal "a key naming no setting is refused" 2 "UnknownKey" "--config=$WORK/typo.yaml"

# --- a key for a node that does not serve ------------------------------------
#
# `scheduler:` names where a one-shot verb is sent. A node that serves finds its scheduler
# from the fleet it joined, so a serving node's file naming one is refused by name rather
# than registering anywhere it says.
printf 'scheduler: "cache.internal:6675"\n' >"$WORK/aimed.yaml"
expect_refusal "a serving node's file naming a scheduler is refused by name" 2 \
    "--scheduler names where a one-shot verb is sent" "${STATE_FOR_STARTUP[@]}" "--config=$WORK/aimed.yaml"

# --- unreadable --------------------------------------------------------------
#
# Skipped rather than asserted when running as root, which can read a 0000 file:
# a case that cannot fail is worse than one that is absent, because it reports
# success for a property nobody checked. `id -u` rather than $EUID, which bash
# 3.2 has but `sh` does not guarantee.
printf 'listen_node: "0.0.0.0:6699"\n' >"$WORK/locked.yaml"
chmod 000 "$WORK/locked.yaml" 2>/dev/null
if [ "$(id -u)" = "0" ]; then
    echo "skip: an unreadable file is refused (running as root, which can read it)"
elif [ -r "$WORK/locked.yaml" ]; then
    echo "skip: an unreadable file is refused (this filesystem ignores the mode)"
else
    expect_refusal "an unreadable named file is refused, and may be retried" 1 "FileUnreadable" \
        "--config=$WORK/locked.yaml"
    expect_start_refusal "a start with an unreadable named file fails, and is retried" 1 "FileUnreadable" \
        "--config=$WORK/locked.yaml"
fi
chmod 644 "$WORK/locked.yaml" 2>/dev/null

# --- the worksheet refuses what a start refuses ------------------------------
#
# A line a real installation could register: `--print-surfaces` is the worksheet an
# operator checks a line against BEFORE installing it, so it must refuse what the start
# refuses, in the start's words. It is a one-shot verb rather than a start, so it says so
# with the verbs' usage exit, 2, and not with a start's code: no supervisor runs it. Beside it
# the line differing by what the refusal's remedy names, which must be accepted -- a worksheet
# refusing everything would pass the first half alone. The file is named so no machine-wide one
# is consulted.
#
# The registered line once named `--serve-scheduler`, and a second pair a `--raft-peer` typed
# twice; both flags are gone -- the formation record decides -- so the pair rides the typed
# `--discovery` rule. The accepted line's consensus address must reach past this machine, since
# a typed `--discovery` that would beacon a loopback address is refused as well.
printf '# the settings are on the command line\n' >"$WORK/lines.yaml"
REGISTERED_LINE=(--listen-node=127.0.0.1:6674 --advertise=127.0.0.1:6674 --discovery=255.255.255.255:6681)
expect_refusal "a typed --discovery without consensus is refused as a start refuses it" 2 \
    "--discovery needs --listen-raft" \
    "--config=$WORK/lines.yaml" "${STATE_FOR_STARTUP[@]}" "${REGISTERED_LINE[@]}" --listen-raft=
expect_ok "the same line with a consensus port is accepted" "36680" \
    "--config=$WORK/lines.yaml" "${STATE_FOR_STARTUP[@]}" "${REGISTERED_LINE[@]}" \
    "--listen-raft=0.0.0.0:36680" "--raft-self=10.0.0.5"

# --- a file of comments is a working configuration ---------------------------
#
# The shipped reference is exactly this shape, so a reader that treated an empty
# document as a failure would refuse every fresh package install.
printf '# nothing uncommented yet\n' >"$WORK/comments.yaml"
expect_ok "a file of nothing but comments starts normally" "compile" \
    "${STATE_FOR_STARTUP[@]}" "--config=$WORK/comments.yaml"

# --- a file this worker FOUND, with no --config at all -----------------------
#
# The discovered path is a different door from the named one, and it is the door
# a package install would use if the unit ever stopped passing --config.
# `EffectiveConfigPath` decides it and is shared with the daemon, so the rule is
# tested where it lives -- what is asserted here is that this binary is WIRED to
# it, which no unit test can see.
#
# $XDG_CONFIG_HOME is the per-user row of that lookup, which needs no privilege
# and no real /etc.
#
# Skipped as root, where the lookup additionally requires every candidate to be
# one only an administrator could have written -- `sudo -E` must not take root's
# configuration out of an account's own $HOME. A case that cannot pass is worse
# than one that is absent.
if [ "$(id -u)" = "0" ]; then
    echo "skip: a file found by the lookup is read with no --config (root; per-user rows are trust-checked)"
    echo "skip: no configuration file anywhere is not an error (needs the case above)"
else
mkdir -p "$WORK/xdg/fastcache-compile-node"
printf 'listen_node: 0.0.0.0:6697\n' > "$WORK/xdg/fastcache-compile-node/fastcache-compile-node.yaml"
out="$(XDG_CONFIG_HOME="$WORK/xdg" HOME="$WORK" "$NODE" --print-surfaces 2>&1)"
case "$out" in
    *"0.0.0.0:6697"*) echo "ok: a file found by the lookup is read with no --config" ;;
    *) case_failed "a file found by the lookup is read with no --config" "output: $out" ;;
esac

# --- and a discovered file that is not there is ORDINARY ---------------------
#
# The other half of the same rule, and the half a strict reading would break: a
# machine with no configuration file at all must start on built-in defaults
# rather than refuse. Only a path the operator NAMED is strict.
rm -rf "$WORK/xdg/fastcache-compile-node"
out="$(XDG_CONFIG_HOME="$WORK/xdg" HOME="$WORK" "$NODE" "${STATE_FOR_STARTUP[@]}" --print-surfaces 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok: no configuration file anywhere is not an error"
else
    case_failed "no configuration file anywhere is not an error" "expected success, got exit $rc" "output: $out"
fi
fi

# --- the shipped reference parses --------------------------------------------
#
# It is 240 lines nobody compiles, and a stray character in it is a package that
# installs a worker which cannot start. Located relative to this script so the
# check follows the file rather than a build layout.
REFERENCE="$(dirname "$0")/../packaging/config/fastcache-compile-node.yaml"
if [ -f "$REFERENCE" ]; then
    expect_ok "the shipped reference configuration parses" "compile" \
        "${STATE_FOR_STARTUP[@]}" "--config=$REFERENCE"
else
    case_failed "the shipped reference configuration parses" "not found at $REFERENCE"
fi

if [ "$FAILURES" -ne 0 ]; then
    echo "node-config-file-e2e: $FAILURES failure(s)" >&2
    exit 1
fi
echo "node-config-file-e2e: every case behaved"
