# SPDX-License-Identifier: Apache-2.0
#
# Does `message(FATAL_ERROR)` in `cmake -P` actually reach ctest as a non-zero
# exit code, on THIS machine, with THIS CMake?
#
# ## Why this is measured rather than remembered
#
# `check-script-check-signals.cmake` and `.agent/rules/build-and-toolchain.md`
# both used to state, as a measurement with versions attached, that it prints
# `CMake Error ...` and exits **0** on CMake 3.28 -- this project's declared
# minimum -- which was the sole stated reason thirteen `cmake -P` hygiene checks
# carry `FAIL_REGULAR_EXPRESSION`. It does not reproduce anywhere it has been
# looked for: two machines in #565, and a third pass across six CMake versions
# and six script shapes, all reading exit **1**.
#
# A claim in that form is the one this repository treats as authoritative, so a
# correction that is itself only a sentence would sit in exactly the position the
# wrong sentence sat in. This asks the question instead, on every platform CI
# builds, on every run -- which is the difference between a citation and a
# measurement, and it is cheap: three `cmake -P` invocations that compile
# nothing.
#
# ## What it asserts, and the controls that make the answer mean something
#
# The subject is one reading and it is worth nothing alone: a harness that
# reported 1 for everything would pass the subject and prove nothing. So two
# controls bracket it -- a child that SUCCEEDS must read 0, and a child pointed
# at a script that does not exist must read non-zero. That is the rule from
# `.agent/rules/testing.md` about a signal that cannot be false in the failing
# case, and it is the rule that caught the first attempt at this very
# measurement: a sweep across six CMake versions reported exit 0 for five of them
# because the shell read `$?` after a command substitution had already run.
#
# ## What a red one MEANS
#
# Not that this file is broken. It means the note's condition has been found: on
# this platform a `cmake -P` check that objects loudly still exits 0, so
# `FAIL_REGULAR_EXPRESSION` is the only thing standing between thirteen hygiene
# checks and reporting PASSED whatever they found. Record the platform and the
# CMake version here; do not delete the property.
#
# ## The three arms are this same file
#
# Re-invoked with `-DFASTCACHED_FATAL_EXIT_CHILD=`, rather than written to a
# scratch directory: script mode has no binary directory, and `$ENV{TMPDIR}` is
# `TEMP` on Windows. One file, no temporaries, identical on every platform.
#
# Usage:
#   cmake [-DFASTCACHED_FATAL_EXIT_READINGS=<subject>;<ok>;<missing>] \
#         -P scripts/check-fatal-error-exit.cmake
#
#   FASTCACHED_FATAL_EXIT_READINGS  stage the three readings instead of measuring
#                                   them, so the VERDICT can be driven against a
#                                   record. `check-fatal-error-exit-selftest`
#                                   uses it; nothing else should.
#
# Exit codes: whatever CMake gives it. The verdict is the presence of
# `CMake Error` in the output -- which is this file obeying the rule it exists to
# keep honest, and would remain correct even if its own answer were the red one.

cmake_minimum_required(VERSION 3.28)

# The arms. Each is this file, re-entered; `return()` keeps the parent's body out
# of a child's run.
if(DEFINED FASTCACHED_FATAL_EXIT_CHILD)
    if(FASTCACHED_FATAL_EXIT_CHILD STREQUAL "fatal")
        message(FATAL_ERROR "check-fatal-error-exit: the subject arm; this failure is the point")
    elseif(FASTCACHED_FATAL_EXIT_CHILD STREQUAL "quiet")
        message(STATUS "check-fatal-error-exit: the success control ran and said nothing")
        return()
    endif()
    # Named rather than defaulted to the quiet arm. A typo'd arm falling through
    # to `quiet` would make the SUCCESS control the thing that ran while the
    # subject reading was attributed to `fatal` -- a fourth state arriving
    # silently as a third, and in the direction that reads as a healthy machine.
    message(FATAL_ERROR "check-fatal-error-exit: unknown child arm `${FASTCACHED_FATAL_EXIT_CHILD}`")
endif()

# One reading. Runs the child and reports only its exit status; the child's own
# output is captured rather than inherited, so a `CMake Error` the subject arm
# prints on purpose cannot be mistaken for this script's verdict.
#
# @param 1 outVar The variable to set to the child's exit status.
# @param 2 script The script to run.
# @param 3 arm    The value of FASTCACHED_FATAL_EXIT_CHILD, or "" for none.
# @param 4 requireObjection TRUE when this child is supposed to OBJECT, so an
#          exit of 0 with nothing on the output reads `silent` rather than `0`.
function(ReadChildExitStatus outVar script arm requireObjection)
    set(childArgs "")
    if(NOT arm STREQUAL "")
        set(childArgs "-DFASTCACHED_FATAL_EXIT_CHILD=${arm}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${childArgs} -P "${script}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE ignoredOut
        ERROR_VARIABLE ignoredErr
    )
    # A command that could not START answers a STRING ("No such file or
    # directory"), not a number, and a string is not a reading. Reported as its
    # own state rather than folded into "non-zero", because a spawn that failed
    # and a child that failed are fixed in different places.
    if(NOT status MATCHES "^-?[0-9]+$")
        set("${outVar}" "unspawned" PARENT_SCOPE)
        return()
    endif()
    # A child that was supposed to object and exited 0 having said nothing did
    # not run the `message(FATAL_ERROR)` this file measures -- an arm renamed, a
    # dispatch that stopped reaching it. Its own state, because reported as a
    # bare `0` it comes out as the loud "FATAL_ERROR exits 0 here" discovery
    # below: an instrument fault wearing the finding's clothes, which is the
    # one outcome a file arguing for measurement over memory must not produce.
    if(requireObjection AND status EQUAL 0 AND NOT "${ignoredOut}${ignoredErr}" MATCHES "CMake Error")
        set("${outVar}" "silent" PARENT_SCOPE)
        return()
    endif()
    set("${outVar}" "${status}" PARENT_SCOPE)
endfunction()

if(DEFINED FASTCACHED_FATAL_EXIT_READINGS)
    set(readings "${FASTCACHED_FATAL_EXIT_READINGS}")
    list(LENGTH readings readingCount)
    if(NOT readingCount EQUAL 3)
        message("CMake Error: FASTCACHED_FATAL_EXIT_READINGS needs exactly three readings "
                "(<subject>;<ok>;<missing>), got ${readingCount}: ${readings}")
        return()
    endif()
    list(GET readings 0 subjectStatus)
    list(GET readings 1 controlOkStatus)
    list(GET readings 2 controlMissingStatus)
    set(origin "staged")
else()
    ReadChildExitStatus(subjectStatus "${CMAKE_CURRENT_LIST_FILE}" "fatal" TRUE)
    ReadChildExitStatus(controlOkStatus "${CMAKE_CURRENT_LIST_FILE}" "quiet" FALSE)
    ReadChildExitStatus(controlMissingStatus
                        "${CMAKE_CURRENT_LIST_DIR}/no-such-script-a1b2c3.cmake" "" FALSE)
    set(origin "measured")
endif()

message(STATUS "check-fatal-error-exit: CMake ${CMAKE_VERSION} on ${CMAKE_HOST_SYSTEM_NAME} (${origin})")
message(STATUS "  message(FATAL_ERROR) in -P  -> exit ${subjectStatus}")
message(STATUS "  control, a script that succeeds -> exit ${controlOkStatus}")
message(STATUS "  control, a script that is absent -> exit ${controlMissingStatus}")

# The controls first. Without them the subject is a reading with nothing to
# compare against, and "1 for everything" would read exactly like a pass.
set(refused FALSE)
if(NOT controlOkStatus STREQUAL "0")
    message("CMake Error: the success control read ${controlOkStatus} rather than 0, so this "
            "harness cannot report a zero at all and its answer about the subject means nothing")
    set(refused TRUE)
endif()
if(controlMissingStatus STREQUAL "0" OR controlMissingStatus STREQUAL "unspawned")
    message("CMake Error: the absent-script control read ${controlMissingStatus}, so this harness "
            "cannot distinguish a child that failed from one that never ran")
    set(refused TRUE)
endif()

if(NOT refused)
    if(subjectStatus STREQUAL "0")
        message("CMake Error: message(FATAL_ERROR) in `cmake -P` exits 0 on CMake ${CMAKE_VERSION} "
                "(${CMAKE_HOST_SYSTEM_NAME}). This is the condition #565 went looking for and did "
                "not find: on this platform a hygiene check reports PASSED however loudly it "
                "objected, so FAIL_REGULAR_EXPRESSION is the only thing carrying every one of them. "
                "Record the platform and version rather than removing the property.")
    elseif(subjectStatus STREQUAL "silent")
        message("CMake Error: the subject arm exited 0 without printing `CMake Error` at all, so "
                "it never reached the message(FATAL_ERROR) this measures. That is an instrument "
                "fault -- an arm renamed, a dispatch that stopped reaching it -- and NOT the #565 "
                "condition. Fix the arm; do not record a platform against this.")
    elseif(subjectStatus STREQUAL "unspawned")
        message("CMake Error: the subject arm could not be started, so nothing was measured. "
                "That is not the same as an exit code of 0 and must not be reported as one.")
    else()
        message(STATUS "check-fatal-error-exit: the exit code is a usable failure signal here")
    endif()
endif()
