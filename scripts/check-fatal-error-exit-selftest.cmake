# SPDX-License-Identifier: Apache-2.0
#
# Drive every verdict of `check-fatal-error-exit.cmake` against a staged record,
# in both directions.
#
# The check's acquisition half needs a real CMake to spawn and its answer depends
# on the machine, so the DECISION is split out behind
# `FASTCACHED_FATAL_EXIT_READINGS` and only the decision is driven here -- the
# rule `.agent/rules/testing.md` states about a stand-in whose measurement is
# comparable to the instrument's own overhead. Acquisition is left alone; it is
# exercised by the check itself on every platform CI builds.
#
# Both directions, because a check that has only ever been seen to pass has told
# you nothing and one that fails on a correct tree is worse than absent. The
# want-pass case is the reading every machine looked at so far actually produces,
# so a change that made the check refuse everything would fail here rather than
# turning every platform red at once.
#
# The child's output is CAPTURED, never inherited: the refusing cases print
# `CMake Error` on purpose, and inheriting that would trip this script's own
# FAIL_REGULAR_EXPRESSION and report a self-test failure for a case that worked.
# It is printed only when a case comes out the wrong way, where tripping the
# pattern is exactly right.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-fatal-error-exit-selftest.cmake
#
# Exit codes: whatever CMake gives it; the verdict is read from the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message("CMake Error: FASTCACHED_SOURCE_DIR is required")
    return()
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-fatal-error-exit.cmake")
if(NOT EXISTS "${check}")
    message("CMake Error: ${check} does not exist, so nothing was driven")
    return()
endif()

# <what> | <readings, comma-separated> | want-pass|want-refuse
#
# Commas rather than semicolons inside a row: a row is split on `|` into a CMake
# list, and a semicolon in a reading would split it further -- three fields
# became five and every `list(GET)` after the first read the wrong thing.
set(cases
    "the readings every machine has produced so far|1,0,1|want-pass"
    "a negative status is still a failure signal|-1,0,1|want-pass"
    "the note's claim, found: FATAL_ERROR exits 0|0,0,1|want-refuse"
    "a harness that cannot report a zero at all|1,1,1|want-refuse"
    "a harness that reads an absent script as success|1,0,0|want-refuse"
    "a subject that never started is not an exit code of 0|unspawned,0,1|want-refuse"
    "a subject that exited 0 having said nothing is an instrument fault|silent,0,1|want-refuse"
    "an absent-script control that never started|1,0,unspawned|want-refuse"
    "a record with the wrong number of readings|1,0|want-refuse"
)

set(failures 0)
set(ran 0)
message(STATUS "FATAL-ERROR-EXIT SELF-TEST")
foreach(case IN LISTS cases)
    string(REPLACE "|" ";" parts "${case}")
    list(GET parts 0 what)
    list(GET parts 1 readings)
    list(GET parts 2 want)
    string(REPLACE "," ";" readings "${readings}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_FATAL_EXIT_READINGS=${readings}" -P "${check}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    # `string(FIND)` rather than `MATCHES`, which is the idiom every sibling
    # selftest here uses: the needle is a literal and a regex would only add a
    # way for one to acquire a metacharacter later.
    set(combined "${out}${err}")
    string(FIND "${combined}" "CMake Error" position)
    if(position EQUAL -1)
        set(got "want-pass")
    else()
        set(got "want-refuse")
    endif()
    math(EXPR ran "${ran} + 1")
    if(got STREQUAL want)
        message(STATUS "  ok   ${what}")
    else()
        math(EXPR failures "${failures} + 1")
        message(STATUS "  FAIL ${what}: wanted ${want}, got ${got}; the check said:")
        message("${combined}")
    endif()
endforeach()

# The count is printed for the reason `.agent/rules/build-and-toolchain.md`
# records about a self-test that stops early: a run that ended at case three must
# not look like one that judged eight.
if(failures EQUAL 0)
    message(STATUS "FATAL-ERROR-EXIT SELF-TEST PASSED (${ran} cases)")
else()
    message("CMake Error: ${failures} of ${ran} self-test cases came out the wrong way")
endif()
