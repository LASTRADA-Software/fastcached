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
# Set only when a case NAMES them, so every case that does not exercises the
# absent-variable arm -- which for the stall policy is the fail-closed default, and
# that is the arm a release depends on (#1156).
if(NOT "$ENV{STALL_FATAL}" STREQUAL "")
    set(CPACK_FASTCACHED_NOTARY_STALL_IS_FATAL "$ENV{STALL_FATAL}")
endif()
if(NOT "$ENV{NOTARY_BUDGET}" STREQUAL "")
    set(CPACK_FASTCACHED_NOTARY_BUDGET_SECONDS "$ENV{NOTARY_BUDGET}")
endif()
include("$ENV{HOOK}")
message(STATUS "DRIVER-COMPLETED")
DRIVE

cases=0
failures=0

# @param 1 name  @param 2 expect completed|fatal  @param 3 substring the output must carry
#        @param 4 how many submit attempts the fake must have seen
#        @param 5 the stall policy, or empty to leave the variable UNSET
#        @param 6 the per-artefact budget in seconds, or empty for the shipped 600
run_case() {
    name="$1"; want="$2"; wantMsg="$3"; wantSubmits="$4"
    stallFatal="${5:-}"; budget="${6:-}"
    cases=$((cases + 1))
    : > "$work/submits"
    out="$(PATH="$work/bin:$PATH" ART="$work/thing.pkg" HOOK="$hook" \
           STALL_FATAL="$stallFatal" NOTARY_BUDGET="$budget" \
           cmake -P "$work/drive.cmake" 2>&1)"
    got=fatal
    grep -q 'DRIVER-COMPLETED' <<< "$out" && got=completed

    if [[ $got != "$want" ]]; then
        refuse "case '$name' expected $want, got $got"
        printf '%s\n' "$out" | sed 's/^/    /' >&2
        failures=$((failures + 1))
        return
    fi
    # FLATTENED before matching, because CMake wraps its diagnostics at about 74
    # columns and a phrase can then exist in the output and in no single LINE of it.
    # `.agent/rules/build-and-toolchain.md` records that as its own trap; it bit here
    # the moment a verdict sentence grew past one line, and the symptom is the worst
    # available one -- "the right outcome for the wrong reason" about a message that
    # says exactly the right thing. A herestring rather than a pipe: `producer |
    # grep -q` is a false negative under `pipefail` on the SUCCESS path.
    flat="$(printf '%s' "$out" | tr '\n' ' ' | tr -s ' ')"
    if ! grep -q -- "$wantMsg" <<< "$flat"; then
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
run_case refusal_is_not_retried fatal "the answer was not Accepted" 1

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
run_case never_answers fatal "did not answer the notarization submission" 2

# --------------------------------------------------------------------------
# The budget is per ARTEFACT and spent ACROSS attempts, so a SLOW failure is not
# retried where a FAST one is (#1156).
#
# The two cases above and this one are the same fake in every respect except how
# long it takes to fail, which is the whole discrimination: `notarytool submit`
# uploads and enqueues a NEW submission, so retrying after a stall joins the back of
# the same slow queue with less time than the first attempt had. Measured on the run
# that filed #1156, two attempts spent twenty minutes to be told the same thing
# twice.
#
# The budget is shrunk through `CPACK_FASTCACHED_NOTARY_BUDGET_SECONDS` because no
# test can wait 600 s to watch this; that seam exists for this case and for nothing
# else, and `cmake/Packaging.cmake` does not export it. Six seconds, so the FIRST
# attempt still clears the minimum-attempt floor the hook derives from the budget --
# a case that failed by skipping attempt one would pass this assertion for the wrong
# reason.
fake_xcrun '#!/bin/sh
case "$2" in submit) echo sub >> '"$work"'/submits;; esac
sleep 30
exit 3'
run_case a_slow_failure_is_not_retried fatal "did not answer the notarization submission" 1 "" 6

# --------------------------------------------------------------------------
# And WHERE a stall is fatal is a policy, not a property of the stall (#1156).
#
# The control is every case above: none of them sets the variable, so all of them
# run on the absent-variable arm and expect `fatal`. That arm is what a release
# depends on, and an implementation that defaulted the other way would pass this
# case and fail those.
fake_xcrun '#!/bin/sh
case "$2" in submit) echo sub >> '"$work"'/submits;; esac
exit 3'
run_case stall_is_reported_when_the_policy_says_so completed "Not fatal on this ref" 2 OFF

# A REJECTION is not covered by that policy and must not be: Apple having looked and
# refused is a fact about the artefact. Same OFF policy as the case above, opposite
# outcome -- which is what makes the softening a discrimination rather than a switch
# that turns the guard off.
fake_xcrun '#!/bin/sh
case "$2" in
  submit) echo sub >> '"$work"'/submits; echo "{\"id\":\"11111111-2222-3333-4444-555555555555\",\"status\":\"Invalid\"}";;
  log) echo "LOG-FETCHED";;
esac
exit 0'
run_case a_refusal_is_fatal_under_every_policy fatal "the answer was not Accepted" 1 OFF

echo "notarize-retry: ran $cases case(s), $failures failure(s)"
[[ $failures -eq 0 ]] || exit 1
