#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# `fastcached_notarize` retries a STALL and never retries a VERDICT.
#
# `cmake/MacOSNotarizePkg.cmake` bounds `notarytool submit --wait` and retries it,
# because an unbounded wait hands the ending to the runner, which answers after
# thirty minutes with no statement of what did not finish
# ([#376](https://github.com/LASTRADA-Software/fastcached/issues/376)).
#
# **The property that needs a guard is not the timeout, it is the distinction.**
# Apple answering "Invalid" is an answer; retrying it spends the budget to be told
# the same thing, and on a release path that is half an hour per attempt. `_rc`
# separates "no answer arrived" from "one did, and `status` decides it" -- and that
# is exactly the kind of condition somebody simplifies away while making the retry
# "more robust". A guard nobody has watched refuse is not a guard.
#
# It drives the REAL function out of the real file with a fake `xcrun` on PATH, so
# what is exercised is what ships rather than a restatement of it -- which is the
# rule this change's own rulebook entry is about.
#
# Runs anywhere `cmake` and a shell do; it never invokes a macOS tool.
set -uo pipefail

refuse() { echo "CMake Error: notarize-retry: $*" >&2; }

repo="${FASTCACHED_SOURCE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
hook="$repo/cmake/MacOSNotarizePkg.cmake"
[[ -f $hook ]] || { refuse "no notarize hook at '$hook'"; exit 1; }
command -v cmake >/dev/null 2>&1 || { refuse "cmake not found"; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"
: > "$work/thing.pkg"

cat > "$work/drive.cmake" <<'DRIVE'
set(CPACK_FASTCACHED_NOTARIZE ON)
set(CPACK_FASTCACHED_BUILD_DMG OFF)
set(CPACK_FASTCACHED_NOTARY_PROFILE "fake-profile")
set(CPACK_PACKAGE_FILES "$ENV{ART}")
include("$ENV{HOOK}")
message(STATUS "DRIVER-COMPLETED")
DRIVE

cases=0
failures=0

# @param 1 name  @param 2 expect completed|fatal  @param 3 substring the output must carry
#        @param 4 how many submit attempts the fake must have seen
run_case() {
    name="$1"; want="$2"; wantMsg="$3"; wantSubmits="$4"
    cases=$((cases + 1))
    : > "$work/submits"
    out="$(PATH="$work/bin:$PATH" ART="$work/thing.pkg" HOOK="$hook" \
           cmake -P "$work/drive.cmake" 2>&1)"
    got=fatal
    grep -q 'DRIVER-COMPLETED' <<< "$out" && got=completed

    if [[ $got != "$want" ]]; then
        refuse "case '$name' expected $want, got $got"
        printf '%s\n' "$out" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi
    if ! grep -q -- "$wantMsg" <<< "$out"; then
        refuse "case '$name' gave the right outcome for the wrong reason -- expected '$wantMsg'"
        printf '%s\n' "$out" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi
    # The count is the point of the whole file: a verdict must cost ONE submission.
    seen=$(wc -l < "$work/submits" | tr -d ' ')
    if [[ $seen != "$wantSubmits" ]]; then
        refuse "case '$name' made $seen submit attempt(s), expected $wantSubmits"
        failures=$((failures + 1))
    fi
}

# @param 1 the fake xcrun's body
fake_xcrun() { printf '%s\n' "$1" > "$work/bin/xcrun"; chmod 0755 "$work/bin/xcrun"; }

fake_xcrun '#!/bin/sh
case "$2" in submit) echo sub >> '"$work"'/submits; echo "{\"id\":\"11111111-2222-3333-4444-555555555555\",\"status\":\"Accepted\"}";; esac
exit 0'
run_case accepted completed "DRIVER-COMPLETED" 1

fake_xcrun '#!/bin/sh
case "$2" in submit) echo sub >> '"$work"'/submits
  if [ "$(wc -l < '"$work"'/submits | tr -d " ")" -le 1 ]; then exit 3; fi
  echo "{\"id\":\"11111111-2222-3333-4444-555555555555\",\"status\":\"Accepted\"}";; esac
exit 0'
run_case stall_then_accept completed "did not answer" 2

# The one this file exists for. A refusal is an ANSWER; retrying it buys nothing and
# costs the step budget.
fake_xcrun '#!/bin/sh
case "$2" in
  submit) echo sub >> '"$work"'/submits; echo "{\"id\":\"11111111-2222-3333-4444-555555555555\",\"status\":\"Invalid\"}";;
  log) echo "LOG-FETCHED";;
esac
exit 0'
run_case refusal_is_not_retried fatal "was not accepted" 1

# And a refusal must still fetch the log, or the rejection is undebuggable.
fake_xcrun '#!/bin/sh
case "$2" in
  submit) echo sub >> '"$work"'/submits; echo "{\"id\":\"11111111-2222-3333-4444-555555555555\",\"status\":\"Invalid\"}";;
  log) echo "LOG-FETCHED";;
esac
exit 0'
run_case refusal_fetches_log fatal "LOG-FETCHED" 1

fake_xcrun '#!/bin/sh
case "$2" in submit) echo sub >> '"$work"'/submits;; esac
exit 3'
run_case never_answers fatal "did not answer within" 2

echo "notarize-retry: ran $cases case(s), $failures failure(s)"
[[ $failures -eq 0 ]] || exit 1
