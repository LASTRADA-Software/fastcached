#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Run scripts/check-instruction-set-flags.cmake over one compile database, for a workflow step, and turn what it
# PRINTED into an exit status (#1442).
#
# ## Why a step, and why this wrapper
#
# The ctests `instruction-set-flags` and `instruction-set-flags-plant` read the database of every configuration that
# runs ctest. The Package jobs configure what SHIPS with tests off, and macOS packages from a configure line and a
# compiler (AppleClang) no test leg uses, so no ctest ever reads their database. A step after each Package job's
# Configure asks both of those questions of the build that ships, before the long build.
#
# That step REPORTS and gates no merge: no Package job is a required context (.agent/rules/packaging-and-release.md),
# so a red one shows on the pull request, in the merge-group report and at release, while the required test legs gate
# through the ctests. The Docker job configures too and is not asked, because its image goes to no registry.
#
# A step is judged by its exit status, and a `cmake -P` check must not be: `message(WARNING)` exits 0, and a script
# that never ran is a `CMake Error` too. So this reads the output of two runs -- the database as it is, and the same
# database with a flag the check must refuse planted into the real commands of `plant_unit` below -- and says which
# of THREE things happened:
#
#   0  clean             the unplanted run printed its summary, the plant run printed its plant summary, and
#                        neither printed a CMake Error or a CMake Warning
#   1  refused           the unplanted run printed its own `instruction-set-flags:` verdict in a CMake Error: a flag,
#                        or a database it cannot judge, and its own words say which
#   2  did not conclude  anything else -- cmake missing or not starting, the script missing, an error that is not the
#                        check's verdict, a warning, no summary, or a plant run that did not pass, which leaves a
#                        clean reading of THIS database never shown able to fail. Nothing was judged, and this says
#                        so rather than passing or blaming the tree.
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
# A first-party unit every shipping configuration compiles, as the `instruction-set-flags-plant` ctest plants.
plant_unit="src/FastCache/Core/Sha256.cpp"

# A path as the native cmake reads it. Git Bash hands POSIX spellings (`/d/a/...`) to a Windows program only through
# its argument conversion, which the call below switches off, so the conversion is done here, explicitly.
# @param 1 A path as this shell spells it.
NativePath() {
    case "${OSTYPE:-}" in
        msys* | cygwin*) cygpath -m "$1" ;;
        *) printf '%s\n' "$1" ;;
    esac
}
native_root="$(NativePath "$repo_root")"

# The outcome of one run, from its output alone.
# @param 1 The check's combined output.
# @param 2 The run's summary up to its first varying part, after `-- instruction-set-flags: `.
# @param 3 The run's summary text after that part.
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
    case "$flat" in *"-- instruction-set-flags: $2"*"$3"*) summary="yes" ;; esac
    if [ "$error" = "no" ] && [ "$warning" = "no" ] && [ "$summary" = "yes" ]; then
        echo "clean"
    elif [ "$error" = "yes" ] && [ "$verdict" = "yes" ]; then
        echo "refused"
    else
        echo "inconclusive"
    fi
}

# Run the check once, print its output and a line naming the outcome, and leave that outcome in `outcome`.
# @param 1 The run's name. @param 2 and 3 Its summary, as Classify takes it. @param 4 The compile database.
# @param 5.. Further -D arguments.
RunOnce() {
    local label="$1" summaryHead="$2" summaryTail="$3" database="$4" output status
    shift 4
    output="$(MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$cmake_command" \
        "-DFASTCACHED_SOURCE_DIR=${native_root}" \
        "-DFASTCACHED_COMPILE_DATABASE=$(NativePath "$database")" \
        ${1+"$@"} \
        -P "${native_root}/scripts/check-instruction-set-flags.cmake" 2>&1)"
    status=$?
    printf '%s\n' "$output"
    outcome="$(Classify "$output" "$summaryHead" "$summaryTail")"
    echo "check-instruction-set-flags: the ${label} run was ${outcome} (cmake exited ${status})"
}

# Run both halves over @p database and print one verdict line; return 0, 1 or 2.
# @param 1 The compile database.
RunAndJudge() {
    local database="$1" unplanted
    RunOnce unplanted "" " first-party unit(s) judged, none carries a global instruction-set flag" "$database"
    unplanted="$outcome"
    RunOnce plant "plant: " " refused as it must; " "$database" "-DFASTCACHED_PLANT_UNIT=${plant_unit}"
    if [ "$unplanted" = "refused" ]; then
        echo "check-instruction-set-flags: REFUSED -- ${database} carries a global instruction-set flag, or cannot be judged; the unplanted run's own words are above"
        return 1
    fi
    if [ "$unplanted" = "clean" ] && [ "$outcome" = "clean" ]; then
        echo "check-instruction-set-flags: clean -- ${database}, and a flag planted into ${plant_unit} was refused as it must be"
        return 0
    fi
    echo "check-instruction-set-flags: DID NOT CONCLUDE -- the unplanted run was ${unplanted} and the plant run ${outcome} (a plant run is refused when the flag it planted was NOT), so NOTHING was judged; this is not a finding about ${database}"
    return 2
}

# One compile database compiling the real `plant_unit` with @p flags, laid out as CMake writes one.
# @param 1 Where to write it.
# @param 2 Extra flags.
WriteDatabase() {
    printf '[\n{\n  "directory": "%s/build",\n  "command": "/usr/bin/g++ -O2 %s -c ../%s",\n  "file": "%s/%s"\n}\n]\n' \
        "$native_root" "$2" "$plant_unit" "$native_root" "$plant_unit" > "$1"
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

    WriteDatabase "$tmp/clean.json" "-DNDEBUG"
    WriteDatabase "$tmp/sha.json" "-msha"

    # Stand-in cmakes, each answering the plant run apart from the unplanted one, printing what the check never would.
    cat > "$tmp/warns" <<'EOF'
#!/bin/bash
case "$*" in
    *FASTCACHED_PLANT_UNIT*) echo '-- instruction-set-flags: plant: `-msha` planted into src/FastCache/Core/Sha256.cpp (1 entr(y/ies)) refused as it must; 1 first-party unit(s) judged; 0 unplanted problem(s) left to the unplanted run' ;;
    *) echo '-- instruction-set-flags: 1 first-party unit(s) judged, none carries a global instruction-set flag; 0 unit(s) outside src/ declined' ;;
esac
echo 'CMake Warning at x.cmake:1 (message):'
echo '  something else'
exit 0
EOF
    cat > "$tmp/foreign-error" <<'EOF'
#!/bin/bash
echo 'CMake Error: Error processing file: /nowhere/check-instruction-set-flags.cmake'
exit 1
EOF
    cat > "$tmp/plant-accepted" <<'EOF'
#!/bin/bash
case "$*" in
    *FASTCACHED_PLANT_UNIT*)
        echo 'CMake Error at /r/scripts/check-instruction-set-flags.cmake:419 (message):'
        echo '  instruction-set-flags: 1 problem(s) in `db`:'
        echo '    plant: `-msha` planted into `src/FastCache/Core/Sha256.cpp` was ACCEPTED -- the check cannot see the flag it exists to refuse'
        exit 1
        ;;
esac
echo '-- instruction-set-flags: 1 first-party unit(s) judged, none carries a global instruction-set flag; 0 unit(s) outside src/ declined'
exit 0
EOF
    chmod +x "$tmp/warns" "$tmp/foreign-error" "$tmp/plant-accepted"

    local self="${repo_root}/scripts/check-instruction-set-flags.sh"
    Expect cleanDatabase 0 bash "$self" --cmake "$cmake_command" "$tmp/clean.json"
    Expect refusedDatabase 1 bash "$self" --cmake "$cmake_command" "$tmp/sha.json"
    Expect missingDatabase 1 bash "$self" --cmake "$cmake_command" "$tmp/absent.json"
    Expect cmakeNeverStarted 2 bash "$self" --cmake "$tmp/no-such-cmake" "$tmp/clean.json"
    Expect warningBesideSummary 2 bash "$self" --cmake "$tmp/warns" "$tmp/clean.json"
    Expect errorNotTheChecks 2 bash "$self" --cmake "$tmp/foreign-error" "$tmp/clean.json"
    Expect plantAccepted 2 bash "$self" --cmake "$tmp/plant-accepted" "$tmp/clean.json"
    Expect noArguments 2 bash "$self"

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
