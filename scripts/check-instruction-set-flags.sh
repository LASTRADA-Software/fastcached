#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Run scripts/check-instruction-set-flags.cmake over one compile database, for a workflow step, and turn what it
# PRINTED into an exit status (#1442).
#
# ## Why a step, and why this wrapper
#
# The ctests `instruction-set-flags` and `instruction-set-flags-plant` read the database of every configuration that
# runs ctest. The Package jobs configure what SHIPS with tests off, and macOS packages from a configure line no test
# leg uses, so no ctest ever reads their database. A step there asks the same check of the build that ships.
#
# A step is judged by its exit status, and a `cmake -P` check must not be: `message(WARNING)` exits 0, and a script
# that never ran is a `CMake Error` too. So this reads the output and says which of THREE things happened:
#
#   0  clean             the check printed its own summary, and no CMake Error or CMake Warning
#   1  refused           the check printed its own `instruction-set-flags:` verdict in a CMake Error
#   2  did not conclude  anything else -- cmake missing or not starting, the script missing, an error that is not the
#                        check's verdict, a warning, no summary. Nothing was judged, and this says so rather than
#                        passing or blaming the tree.
#
# cmake's own exit status is printed and decides nothing.
#
# Usage:
#   bash scripts/check-instruction-set-flags.sh [--cmake <cmake>] <compile_commands.json>
#   bash scripts/check-instruction-set-flags.sh --self-test [--cmake <cmake>]
#
# bash 3.2: macOS runs this under its system interpreter.

set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cmake_command="cmake"

# A path as the native cmake reads it. Git Bash hands POSIX spellings (`/d/a/...`) to a Windows program only through
# its argument conversion, which the call below switches off, so the conversion is done here, explicitly.
# @param 1 A path as this shell spells it.
NativePath() {
    case "$(uname -s)" in
        MINGW* | MSYS* | CYGWIN*) cygpath -m "$1" ;;
        *) printf '%s\n' "$1" ;;
    esac
}

# The outcome of one run, from its output alone. PURE, so every branch is a self-test case.
# @param 1 The check's combined output.
# @return echoes clean, refused or inconclusive
Classify() {
    local flat
    flat="$(printf '%s' "$1" | tr '\r\n\t' '   ' | tr -s ' ')"
    local error="no" warning="no" verdict="no" summary="no"
    case "$flat" in *"CMake Error"*) error="yes" ;; esac
    case "$flat" in *"CMake Warning"*) warning="yes" ;; esac
    # The check spells every verdict `instruction-set-flags: <text>` -- a colon and a space. Its own file name,
    # `check-instruction-set-flags.cmake:<line>`, does not match that.
    case "$flat" in *"instruction-set-flags: "*) verdict="yes" ;; esac
    case "$flat" in *"-- instruction-set-flags: "*" first-party unit(s) judged, none carries a global instruction-set flag"*) summary="yes" ;; esac
    if [ "$error" = "no" ] && [ "$warning" = "no" ] && [ "$summary" = "yes" ]; then
        echo "clean"
    elif [ "$error" = "yes" ] && [ "$verdict" = "yes" ]; then
        echo "refused"
    else
        echo "inconclusive"
    fi
}

# Run the check over @p database and print its output, then one verdict line; return 0, 1 or 2.
# @param 1 The compile database.
RunAndJudge() {
    local database="$1" output status outcome
    output="$(MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$cmake_command" \
        "-DFASTCACHED_SOURCE_DIR=$(NativePath "$repo_root")" \
        "-DFASTCACHED_COMPILE_DATABASE=$(NativePath "$database")" \
        -P "$(NativePath "${repo_root}/scripts/check-instruction-set-flags.cmake")" 2>&1)"
    status=$?
    printf '%s\n' "$output"
    outcome="$(Classify "$output")"
    case "$outcome" in
        clean)
            echo "check-instruction-set-flags: clean -- ${database} (cmake exited ${status})"
            return 0
            ;;
        refused)
            echo "check-instruction-set-flags: REFUSED -- ${database} carries a global instruction-set flag, or cannot be judged; the check's own words are above (cmake exited ${status})"
            return 1
            ;;
        *)
            echo "check-instruction-set-flags: DID NOT CONCLUDE -- neither the check's summary nor its verdict was printed, so NOTHING was judged; this is not a finding about ${database} (cmake exited ${status})"
            return 2
            ;;
    esac
}

# One compile database compiling the real src/FastCache/Core/Sha256.cpp with @p flags.
# @param 1 Where to write it.
# @param 2 Extra flags.
WriteDatabase() {
    local root
    root="$(NativePath "$repo_root")"
    printf '[{"directory": "%s/build", "file": "%s/src/FastCache/Core/Sha256.cpp", "command": "/usr/bin/g++ -O2 %s -c ../src/FastCache/Core/Sha256.cpp"}]\n' \
        "$root" "$root" "$2" > "$1"
}

SelfTest() {
    local tmp ran=0 failures=0
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/instruction-set-flags-selftest.XXXXXX")" || { echo "cannot create a scratch directory"; exit 2; }
    trap 'rm -rf -- "$tmp"' EXIT

    # @param 1 Case name. @param 2 Expected status. @param 3.. the command.
    Expect() {
        local name="$1" want="$2" out got
        shift 2
        out="$("$@" 2>&1)"
        got=$?
        ran=$((ran + 1))
        if [ "$got" != "$want" ]; then
            failures=$((failures + 1))
            printf '  %s: exited %s, wanted %s\n%s\n' "$name" "$got" "$want" "$out"
        fi
    }
    # @param 1 Case name. @param 2 Expected outcome. @param 3 Output to classify.
    ExpectClass() {
        local got
        got="$(Classify "$3")"
        ran=$((ran + 1))
        if [ "$got" != "$2" ]; then
            failures=$((failures + 1))
            printf '  %s: classified %s, wanted %s\n' "$1" "$got" "$2"
        fi
    }

    WriteDatabase "$tmp/clean.json" "-DNDEBUG"
    WriteDatabase "$tmp/sha.json" "-msha"

    # A cmake that prints what a check never would: the wrapper must not read it as either verdict.
    printf '#!/bin/bash\necho "-- instruction-set-flags: 1 first-party unit(s) judged, none carries a global instruction-set flag"\necho "CMake Warning at x.cmake:1 (message):"\necho "  something else"\nexit 0\n' > "$tmp/warns"
    printf '#!/bin/bash\necho "CMake Error: Error processing file: /nowhere/check-instruction-set-flags.cmake"\nexit 1\n' > "$tmp/foreign-error"
    chmod +x "$tmp/warns" "$tmp/foreign-error"

    local self="${repo_root}/scripts/check-instruction-set-flags.sh"
    Expect cleanDatabase 0 bash "$self" --cmake "$cmake_command" "$tmp/clean.json"
    Expect refusedDatabase 1 bash "$self" --cmake "$cmake_command" "$tmp/sha.json"
    Expect missingDatabase 1 bash "$self" --cmake "$cmake_command" "$tmp/absent.json"
    Expect cmakeNeverStarted 2 bash "$self" --cmake "$tmp/no-such-cmake" "$tmp/clean.json"
    Expect warningBesideSummary 2 bash "$self" --cmake "$tmp/warns" "$tmp/clean.json"
    Expect errorNotTheChecks 2 bash "$self" --cmake "$tmp/foreign-error" "$tmp/clean.json"
    Expect noArguments 2 bash "$self"

    ExpectClass classifyClean clean "-- instruction-set-flags: 3 first-party unit(s) judged, none carries a global instruction-set flag; 0 unit(s) outside src/ declined"
    ExpectClass classifyRefused refused "CMake Error at /r/scripts/check-instruction-set-flags.cmake:400 (message):
  instruction-set-flags: 1 problem(s) in
  \`db\`:
    src/a.cpp: \`-msha\`, because it is an -m flag no row has judged"
    ExpectClass classifyFileNameIsNotAVerdict inconclusive "CMake Error at /r/scripts/check-instruction-set-flags.cmake:12 (foo):
  Unknown CMake command \"foo\"."
    ExpectClass classifyEmpty inconclusive ""

    if [ "$failures" -ne 0 ]; then
        echo "check-instruction-set-flags --self-test: ${ran} case(s) ran, ${failures} did not judge as they must"
        exit 1
    fi
    echo "check-instruction-set-flags --self-test: ${ran} case(s) ran, every outcome as it must be"
    exit 0
}

# The self-test runs after the loop, so a --cmake written after --self-test still applies: the ctest registration
# passes --self-test straight after the script, which is how check-selftest-registered knows the mode is run.
self_test="no"
database=""
while [ $# -gt 0 ]; do
    case "$1" in
        --cmake)
            [ $# -ge 2 ] || { echo "check-instruction-set-flags: --cmake needs a value"; exit 2; }
            cmake_command="$2"
            shift 2
            ;;
        --self-test)
            self_test="yes"
            shift
            ;;
        *)
            database="$1"
            shift
            ;;
    esac
done

if [ "$self_test" = "yes" ]; then
    SelfTest
fi
if [ -z "$database" ]; then
    echo "check-instruction-set-flags: DID NOT CONCLUDE -- no compile database named. Usage: bash scripts/check-instruction-set-flags.sh [--cmake <cmake>] <compile_commands.json>"
    exit 2
fi
RunAndJudge "$database"
exit $?
