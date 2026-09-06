#!/usr/bin/env bash
# No workflow calls `apt-get update` directly; they go through ci-apt-update.sh.
#
# The runner image ships `packages.microsoft.com` preinstalled, and `apt-get
# update` exits non-zero when ANY configured source is unreachable -- so a 403
# from a mirror nothing here uses fails a step that only wanted the Ubuntu
# archive, and presents as an unrelated red check on whatever branch happened to
# be building (#550).
#
# `ci-apt-update.sh` removes the unused sources first. This refuses a call site
# that skips it, which is the whole reason the wrapper has a chance of surviving:
# eleven sites exist today and the twelfth is written by somebody who has never
# read this.
#
# bash 3.2: macOS ships a 2007 /bin/bash and this runs in the default ctest set.
set -euo pipefail

selfTest=0
for arg in ${1+"$@"}; do
    case "$arg" in
        --self-test) selfTest=1 ;;
        *) echo "usage: $0 [--self-test]" >&2; exit 2 ;;
    esac
done

Check() {
    local root="$1" files hits wrapper
    files=$(ls "$root"/.github/workflows/*.yml 2>/dev/null || true)
    if [ -z "$files" ]; then
        echo "FAIL no workflow files under .github/workflows -- the scan is broken, not the tree"
        return 1
    fi

    # Full-line comments are stripped: a COMMENT is not a call site, and two
    # checks in this tree have refused a correct workflow by matching their own
    # headers.
    hits=$(awk '
        { line = $0; sub(/^[ \t]+/, "", line)
          if (substr(line, 1, 1) == "#") next
          if ($0 ~ /apt-get[ \t]+update/) printf "%s:%d: %s\n", FILENAME, FNR, line
        }' $files)

    wrapper=$(awk '
        { line = $0; sub(/^[ \t]+/, "", line)
          if (substr(line, 1, 1) == "#") next
          if ($0 ~ /ci-apt-update\.sh/) n++
        } END { print n + 0 }' $files)

    if [ -n "$hits" ]; then
        echo "FAIL a workflow calls apt-get update directly; use scripts/ci-apt-update.sh:"
        printf '%s\n' "$hits" | sed 's/^/    /'
        return 1
    fi

    # An empty scan is a refusal, never a pass: with no call site at all this
    # check would report clean over a tree that had stopped using apt entirely,
    # and nobody would notice it had stopped meaning anything.
    if [ "$wrapper" -eq 0 ]; then
        echo "FAIL no workflow calls scripts/ci-apt-update.sh -- either the wrapper is unused or this scan is broken"
        return 1
    fi
    echo "check-apt-update: ${wrapper} wrapped call site(s), 0 direct"
}

if [ "$selfTest" -eq 0 ]; then
    Check "$(cd "$(dirname "$0")/.." && pwd)"
    echo "check-apt-update: OK"
    exit 0
fi

cases=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

Stage() { mkdir -p "$tmp/$1/.github/workflows"; printf 'jobs:\n  j:\n    steps:\n      - run: %s\n' "$2" > "$tmp/$1/.github/workflows/build.yml"; }
Assert() {
    cases=$((cases + 1))
    if Check "$tmp/$2" >/dev/null 2>&1; then [ "$3" = pass ] || { echo "  FAIL: $1 -- expected a refusal"; exit 1; }
    else [ "$3" = fail ] || { echo "  FAIL: $1 -- expected a pass"; exit 1; }; fi
    echo "  ok: $1"
}

Stage wrapped 'bash scripts/ci-apt-update.sh'
Assert "a wrapped call site passes" wrapped pass

Stage direct 'sudo apt-get update'
Assert "a direct apt-get update is refused" direct fail

Stage commented '# sudo apt-get update -- a note, not a call'
mkdir -p "$tmp/commented/.github/workflows"
printf 'jobs:\n  j:\n    steps:\n      - run: |\n          # sudo apt-get update is wrapped\n          bash scripts/ci-apt-update.sh\n' \
    > "$tmp/commented/.github/workflows/build.yml"
Assert "a commented mention is not a call site" commented pass

Stage neither 'echo hello'
Assert "a tree that calls neither is refused, not passed" neither fail

mkdir -p "$tmp/empty"
Assert "a tree with no workflows is refused" empty fail

echo "check-apt-update self-test: $cases case(s) ran, all as expected"
