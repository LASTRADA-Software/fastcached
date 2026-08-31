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
report() {
    echo "  $1"
    FAILURES=$((FAILURES + 1))
}

# expect_ok <name> <expected-substring> <args...>
expect_ok() {
    name="$1"; needle="$2"; shift 2
    out="$("$NODE" "$@" --print-surfaces 2>&1)"
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL: $name"
        report "expected success, got exit $rc"
        report "output: $out"
        return
    fi
    case "$out" in
        *"$needle"*) echo "ok: $name" ;;
        *)
            echo "FAIL: $name"
            report "expected output to contain: $needle"
            report "output: $out"
            ;;
    esac
}

# expect_refusal <name> <expected-substring> <args...>
expect_refusal() {
    name="$1"; needle="$2"; shift 2
    out="$("$NODE" "$@" --print-surfaces 2>&1)"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "FAIL: $name"
        report "expected a refusal, got exit 0"
        report "output: $out"
        return
    fi
    case "$out" in
        *"$needle"*) echo "ok: $name" ;;
        *)
            echo "FAIL: $name"
            report "expected output to contain: $needle"
            report "output: $out"
            ;;
    esac
}

# --- a setting in the file takes effect -------------------------------------
#
# `listen_node` rather than something cheaper, because `--print-surfaces` prints
# the address it resolves to: a key that did nothing would leave the cache row
# reading "not served", which is a different line rather than a missing one.
cat >"$WORK/good.yaml" <<'YAML'
scheduler: "cache.internal:6675"
listen_node: "0.0.0.0:6699"
YAML
expect_ok "a setting in the file takes effect" "0.0.0.0:6699" "--config=$WORK/good.yaml"

# --- the command line wins over the file ------------------------------------
expect_ok "the command line wins over the file" "0.0.0.0:6698" \
    "--config=$WORK/good.yaml" "--listen-node=0.0.0.0:6698"

# --- absent ------------------------------------------------------------------
#
# Named and not there is FILE NOT FOUND, not a parse error: an operator who
# mistyped a path must not be sent hunting for a syntax mistake in a file that
# does not exist.
expect_refusal "a named file that is absent is refused by name" "FileNotFound" \
    "--config=$WORK/definitely-not-here.yaml"

# --- malformed ---------------------------------------------------------------
printf 'scheduler: [1,\n' >"$WORK/malformed.yaml"
expect_refusal "a malformed file is refused as a parse error" "ParseError" \
    "--config=$WORK/malformed.yaml"

# --- a key nothing reads -----------------------------------------------------
printf 'schedular: "typo:6675"\n' >"$WORK/typo.yaml"
expect_refusal "a key naming no setting is refused" "UnknownKey" "--config=$WORK/typo.yaml"

# --- unreadable --------------------------------------------------------------
#
# Skipped rather than asserted when running as root, which can read a 0000 file:
# a case that cannot fail is worse than one that is absent, because it reports
# success for a property nobody checked. `id -u` rather than $EUID, which bash
# 3.2 has but `sh` does not guarantee.
printf 'scheduler: "cache.internal:6675"\n' >"$WORK/locked.yaml"
chmod 000 "$WORK/locked.yaml" 2>/dev/null
if [ "$(id -u)" = "0" ]; then
    echo "skip: an unreadable file is refused (running as root, which can read it)"
elif [ -r "$WORK/locked.yaml" ]; then
    echo "skip: an unreadable file is refused (this filesystem ignores the mode)"
else
    expect_refusal "an unreadable named file is refused" "FileNotFound" "--config=$WORK/locked.yaml"
fi
chmod 644 "$WORK/locked.yaml" 2>/dev/null

# --- a file of comments is a working configuration ---------------------------
#
# The shipped reference is exactly this shape, so a reader that treated an empty
# document as a failure would refuse every fresh package install.
printf '# nothing uncommented yet\n' >"$WORK/comments.yaml"
expect_ok "a file of nothing but comments starts normally" "compile" "--config=$WORK/comments.yaml"

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
    *)
        echo "FAIL: a file found by the lookup is read with no --config"
        report "output: $out"
        ;;
esac

# --- and a discovered file that is not there is ORDINARY ---------------------
#
# The other half of the same rule, and the half a strict reading would break: a
# machine with no configuration file at all must start on built-in defaults
# rather than refuse. Only a path the operator NAMED is strict.
rm -rf "$WORK/xdg/fastcache-compile-node"
out="$(XDG_CONFIG_HOME="$WORK/xdg" HOME="$WORK" "$NODE" --print-surfaces 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok: no configuration file anywhere is not an error"
else
    echo "FAIL: no configuration file anywhere is not an error"
    report "expected success, got exit $rc"
    report "output: $out"
fi
fi

# --- the shipped reference parses --------------------------------------------
#
# It is 240 lines nobody compiles, and a stray character in it is a package that
# installs a worker which cannot start. Located relative to this script so the
# check follows the file rather than a build layout.
REFERENCE="$(dirname "$0")/../packaging/linux/fastcache-compile-node.yaml"
if [ -f "$REFERENCE" ]; then
    expect_ok "the shipped reference configuration parses" "compile" "--config=$REFERENCE"
else
    echo "FAIL: the shipped reference configuration parses"
    report "not found at $REFERENCE"
fi

if [ "$FAILURES" -ne 0 ]; then
    echo "node-config-file-e2e: $FAILURES failure(s)" >&2
    exit 1
fi
echo "node-config-file-e2e: every case behaved"
