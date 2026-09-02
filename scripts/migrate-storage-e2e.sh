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
# $1: the directory the store files appear in.
# $2: how many `.cow` files the caller goes on to assert.
# $3..: the daemon's storage flags.
create_store() {
    local watch="$1"; shift
    local expected="$1"; shift
    local p
    p="$(port)" || fail "could not allocate a port"

    # The binary directly, never the `fc` wrapper: backgrounding a shell
    # FUNCTION makes $! the subshell rather than the daemon, so the kill below
    # would land on the wrapper and leave the daemon holding the store -- and
    # `wait` would then block forever. Found exactly that way.
    "$FASTCACHED" --config "$EMPTY_CONFIG" --port="$p" "$@" &
    local pid=$!

    # Waits for the number of stores the CALLER asserts, and that count is a
    # parameter for exactly that reason. It used to break on the FIRST `.cow` to
    # appear and then sleep a fixed 0.3 s before killing the daemon, while the
    # sharded caller went on to assert four -- so whenever shard creation spanned
    # more than that window the daemon was killed partway through and the
    # assertion failed on a correct build (#576, observed as "created 3 shard
    # file(s), expected 4"). Mechanical rather than unlucky: it fires on runner
    # load, not on chance.
    #
    # The comment this replaces said "it says what it waited for", which was
    # true, and was the whole defect -- what it waited for was not what the
    # caller asserts. A guard may wait for a condition STRONGER than the
    # assertion, never a weaker one.
    #
    # Bounded at 10 s still, and the daemon's own liveness is checked each pass:
    # one that dies at startup would otherwise present as a ten-second wait for a
    # file, which names the wrong problem entirely.
    local waited=0
    local seen=0
    while [[ $waited -lt 100 ]]; do
        seen="$(find "$watch" -name '*.cow' 2>/dev/null | wc -l)"
        [[ "$seen" -ge "$expected" ]] && break
        kill -0 "$pid" 2>/dev/null \
            || fail "the daemon exited after creating $seen of $expected store(s) under $watch"
        sleep 0.1
        waited=$((waited + 1))
    done

    # Re-counted rather than trusting the loop to have exited for the right
    # reason: running out of iterations and reaching the count are the same exit
    # from the `while`, so without this a timeout would return quietly and hand
    # the caller a half-built store to assert against -- a fixture timing out
    # INTO a pass, which is the shape this file exists to not have.
    seen="$(find "$watch" -name '*.cow' 2>/dev/null | wc -l)"
    [[ "$seen" -ge "$expected" ]] \
        || { kill "$pid" 2>/dev/null
             fail "waited 10s for $expected store file(s) under $watch and saw $seen"; }

    # The store file exists before the daemon has finished writing it, and this
    # is still a fixed sleep because it stands for a different condition than the
    # count above: the files are all THERE, and this waits for their contents to
    # be readable by the conversion. Narrower than what it used to cover, and the
    # one part of this function that is still a duration rather than a condition.
    sleep 0.3
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}

# ---------------------------------------------------------------- single file
mkdir -p "$WORK/single"
SINGLE="$WORK/single/cache.cow"
create_store "$WORK/single" 1 "--storage=$SINGLE"
[[ -f "$SINGLE" ]] || fail "the daemon did not create $SINGLE"

out="$(fc --migrate-storage "--storage=$SINGLE" 2>&1)" || fail "single-file conversion exited non-zero: $out"
grep -q "nothing to do" <<<"$out" || fail "expected a no-op over a current store, got: $out"
echo "single file: $out"

# ------------------------------------------------------------- sharded layout
SHARDS="$WORK/shards"
mkdir -p "$SHARDS"
create_store "$SHARDS" 4 "--storage=$SHARDS" --storage-shards=4

# Still asserted, and NOT made redundant by the wait above: that waits for at
# least four `*.cow`, this requires exactly four named `shard-NN.cow`. A daemon
# that created five, or that named them something else, passes the wait and
# fails here. Deleting it as duplication would drop the naming and the upper
# bound, which are the parts the conversion below depends on.
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
