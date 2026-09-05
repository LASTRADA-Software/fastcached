#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Drive `scripts/reactor-teardown-gate.cmake` against staged canary outcomes and
# assert each verdict, including WHICH refusal it gave.
#
# **Asserting "it refused" would not be enough, and that is the whole design here.**
# The gate's first check is "did the canary establish the arrangement at all", so a
# stub that fails to START -- wrong mode, missing interpreter, bad shebang -- produces
# no output and is refused by that check. Refusal-only assertions would therefore
# pass for every want-fail case even with the gate's later checks deleted, and would
# pass for a stub that never ran (#723). So each case names the refusal it expects.
#
# Constrained to bash 3.2: macOS ships a 2007 `/bin/bash` and this runs in the
# default ctest set on every platform CI builds. No `mapfile`, no `declare -A`, no
# `${var^^}`, no `local -n`.
#
# Reports failure by printing `CMake Error`, because that is the contract every
# script check here is registered under -- see `src/tests/CMakeLists.txt`.

set -u

if [ $# -ne 1 ]; then
    echo "CMake Error: usage: $0 <path-to-reactor-teardown-gate.cmake>" >&2
    exit 1
fi
gate="$1"

if [ ! -f "$gate" ]; then
    echo "CMake Error: reactor-teardown-gate-selftest: no gate script at '$gate'" >&2
    exit 1
fi

cmakeBin="${CMAKE_COMMAND:-cmake}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

failures=0
cases=0

# Stage one canary outcome and assert the gate's verdict.
#   $1 name  $2 expected verdict (refuse|pass)  $3 expected message substring
#   $4 exit code the stub returns  $5.. lines the stub prints
# Which stub form the gate can execute here. Asked of the HOST rather than guessed
# from a shell feature, because it decides how the stub is written, not how it runs.
case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW* | MSYS* | CYGWIN* | Windows_NT) windows=yes ;;
    *) windows=no ;;
esac

stage_and_check() {
    name="$1"; want="$2"; wantMsg="$3"; code="$4"; shift 4

    # The gate runs the canary through CMake's `execute_process`, which on Windows is
    # `CreateProcess` and cannot start a `#!/bin/sh` script -- it fails ENOEXEC,
    # "inappropriate file type or format". Git Bash CAN start one, so a stub that this
    # script executes happily is a stub the gate cannot execute at all. Stage the form
    # the gate can actually run.
    if [ "$windows" = "yes" ]; then
        stub="$work/$name.cmd"
        {
            echo "@echo off"
            for line in "$@"; do
                echo "echo $line"
            done
            echo "exit /b $code"
        } > "$stub"
    else
        stub="$work/$name"
        {
            echo '#!/bin/sh'
            for line in "$@"; do
                echo "echo \"$line\""
            done
            echo "exit $code"
        } > "$stub"
        chmod 0755 "$stub"
    fi

    # The stub is executed BY the gate, so it must genuinely run. Probed through
    # CMake rather than through this shell, because those are different launchers and
    # only one of them is the one under test: probing with the shell is what let a
    # stub the gate could never start pass this check on Windows, leaving four cases
    # "refused" for a reason that had nothing to do with the guard. A probe must use
    # the executor that will run the thing.
    if ! "$cmakeBin" -E env "$stub" > /dev/null 2>&1; then
        probe=$?
        if [ "$probe" = "126" ] || [ "$probe" = "127" ] || [ "$probe" = "1" ]; then
            echo "CMake Error: reactor-teardown-gate-selftest: stub '$name' could not be executed by CMake (exit $probe)" >&2
            failures=$((failures + 1))
            return
        fi
    fi

    cases=$((cases + 1))
    out="$("$cmakeBin" -DFASTCACHED_CANARY="$stub" -P "$gate" 2>&1)"
    if echo "$out" | grep -q "CMake Error"; then
        got="refuse"
    else
        got="pass"
    fi

    if [ "$got" != "$want" ]; then
        echo "CMake Error: reactor-teardown-gate-selftest: case '$name' expected $want, got $got" >&2
        echo "$out" >&2
        failures=$((failures + 1))
        return
    fi

    # Whitespace-normalised before matching: `message(FATAL_ERROR ...)` re-wraps its
    # text, so a two-word substring can arrive split across a newline and two spaces
    # of indent. Matching the raw output made this selftest report a right verdict as
    # wrong, which is the instrument being broken rather than the gate.
    flat="$(echo "$out" | tr '\n' ' ' | tr -s ' ')"
    if ! echo "$flat" | grep -q "$wantMsg"; then
        echo "CMake Error: reactor-teardown-gate-selftest: case '$name' gave the right verdict for the wrong" \
             "reason -- expected to see '$wantMsg'" >&2
        echo "$out" >&2
        failures=$((failures + 1))
    fi
}

established="canary: arrangement established; asserting teardown"

# The guard was deleted or compiled out: the canary reached the assertion and lived.
stage_and_check survived refuse "the canary SURVIVED" 0 \
    "$established" "canary: SURVIVED -- the teardown guard did not refuse"

# The canary self-diagnosed and never reached the assertion. It exits 0 to say so,
# which is exactly the shape that must NOT read as the guard working.
stage_and_check never_started refuse "never established a running" 0 \
    "canary: the reactor never started; nothing was tested"

# Also 0, also never armed: the predicate answered safe when it should not have.
stage_and_check predicate_safe refuse "answered SAFE for a reactor that was running" 0 \
    "canary: predicate answered SAFE with the reactor running off-thread"

# It died, but of something else. A non-zero exit is not proof; the diagnostic is.
stage_and_check other_death refuse "said nothing about racing the" 139 \
    "$established" "Segmentation fault"

# It died AND named the guard. The only passing shape.
stage_and_check genuine pass "was watched refusing" 134 \
    "$established" "Assertion failed: races the completion dispatch"

# A count, so a run that stopped early cannot look like one that judged everything.
echo "reactor-teardown-gate-selftest: ran $cases case(s), $failures failure(s)"

if [ "$failures" -ne 0 ]; then
    exit 1
fi
