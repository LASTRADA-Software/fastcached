#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end check of `fastcached --migrate-storage`.
#
# What it covers that the unit tests cannot: the CLI wiring, and — the part that
# actually decides whether the feature works for an operator — that the flag
# converts the store files that are really there. `MigrateConfiguredStorage` and
# `ExistingStorePaths` live in `main.cpp`, which is in no test target, and a
# conversion that walked a different set of files than the daemon opens would
# report success while leaving the daemon refusing to start on the one it missed.
#
# Both shapes `--storage` can take are exercised: one file, and a directory of
# `shard-NN.cow`. The stores are created by the daemon itself rather than by this
# script, so the layout under test is the real one.
set -uo pipefail

SKIP=77
FASTCACHED=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fastcached) FASTCACHED="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$FASTCACHED" || ! -x "$FASTCACHED" ]]; then
    echo "migrate-storage-e2e: no fastcached binary — skipping" >&2
    exit "$SKIP"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "migrate-storage E2E FAILED: $*" >&2; exit 1; }

# EVERY invocation names this file. Without it the daemon would find whatever
# machine-wide config the host has, and on a developer box with a real
# `storage_path:` in it the "memory-only must fail" case below would convert
# their actual cache in place. An empty file the operator named is still a
# config the operator named, so nothing is discovered behind it.
EMPTY_CONFIG="$WORK/empty.yaml"
: > "$EMPTY_CONFIG"

fc() { "$FASTCACHED" --config "$EMPTY_CONFIG" "$@"; }

# A free TCP port, in bash: the repo's own idiom, so the fixture needs no
# interpreter beyond the shell it is already written in.
port() {
    local p
    for _ in $(seq 1 50); do
        p=$(( (RANDOM % 20000) + 40000 ))
        if ! (exec 3<>"/dev/tcp/127.0.0.1/$p") 2>/dev/null; then
            echo "$p"
            return 0
        fi
        exec 3<&- 2>/dev/null
    done
    return 1
}

# Start the daemon just long enough for it to create its store(s), then stop it.
# The conversion must run against a store nobody has open.
# $1: the directory the store files appear in. $2..: the daemon's storage flags.
create_store() {
    local watch="$1"; shift
    local p
    p="$(port)" || fail "could not allocate a port"

    # The binary directly, never the `fc` wrapper: backgrounding a shell
    # FUNCTION makes $! the subshell rather than the daemon, so the kill below
    # would land on the wrapper and leave the daemon holding the store -- and
    # `wait` would then block forever. Found exactly that way.
    "$FASTCACHED" --config "$EMPTY_CONFIG" --port="$p" "$@" &
    local pid=$!

    # Bounded, and it says what it waited for. The daemon's own liveness is
    # checked each pass: one that dies at startup would otherwise present as a
    # ten-second wait for a file, which names the wrong problem entirely.
    local waited=0
    while [[ $waited -lt 100 ]]; do
        [[ -n "$(find "$watch" -name '*.cow' 2>/dev/null | head -1)" ]] && break
        kill -0 "$pid" 2>/dev/null || fail "the daemon exited before creating a store under $watch"
        sleep 0.1
        waited=$((waited + 1))
    done
    [[ -n "$(find "$watch" -name '*.cow' 2>/dev/null | head -1)" ]] \
        || { kill "$pid" 2>/dev/null; fail "waited 10s for a .cow file under $watch and none appeared"; }

    # The store file exists before the daemon has finished starting; let it
    # settle before taking it away.
    sleep 0.3
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}

# ---------------------------------------------------------------- single file
mkdir -p "$WORK/single"
SINGLE="$WORK/single/cache.cow"
create_store "$WORK/single" "--storage=$SINGLE"
[[ -f "$SINGLE" ]] || fail "the daemon did not create $SINGLE"

out="$(fc --migrate-storage "--storage=$SINGLE" 2>&1)" || fail "single-file conversion exited non-zero: $out"
grep -q "nothing to do" <<<"$out" || fail "expected a no-op over a current store, got: $out"
echo "single file: $out"

# ------------------------------------------------------------- sharded layout
SHARDS="$WORK/shards"
mkdir -p "$SHARDS"
create_store "$SHARDS" "--storage=$SHARDS" --storage-shards=4
count="$(find "$SHARDS" -name 'shard-*.cow' | wc -l)"
[[ "$count" -eq 4 ]] || fail "the daemon created $count shard file(s), expected 4"

# Deliberately WITHOUT --storage-shards: the conversion must find the four
# stores from the directory rather than from the shard formula, which without
# that flag follows the CPU count and would walk a different set here than the
# daemon did above.
out="$(fc --migrate-storage "--storage=$SHARDS" 2>&1)" || fail "sharded conversion exited non-zero: $out"

for i in 0 1 2 3; do
    grep -q "shard-0$i.cow" <<<"$out" || fail "shard-0$i.cow was not converted; output: $out"
done
# Counted by matching the expected shape rather than by counting lines, so a
# stray line on the stream cannot turn a correct run into a failure.
converted="$(grep -c 'shard-0[0-9].cow: already at' <<<"$out")"
[[ "$converted" -eq 4 ]] || fail "expected exactly 4 store lines, got $converted in: $out"
echo "sharded: 4 store(s) reported, found without --storage-shards"

# ----------------------------------------------------- a path naming no store
if fc --migrate-storage "--storage=$WORK/definitely-absent.cow" >/dev/null 2>&1; then
    fail "converting a path with no store should have exited non-zero"
fi

# --------------------------------------------- a directory holding no shards
mkdir -p "$WORK/empty-dir"
if fc --migrate-storage "--storage=$WORK/empty-dir" >/dev/null 2>&1; then
    fail "converting a directory with no shard files should have exited non-zero"
fi

# ---------------------------------------------------------------- memory-only
if fc --migrate-storage >/dev/null 2>&1; then
    fail "converting with no --storage should have exited non-zero"
fi

echo "migrate-storage E2E PASSED"
