# SPDX-License-Identifier: Apache-2.0
#
# The reader for the exit contract `SKIP_RETURN_CODE 4` rests on.
#
# Every `catch_discover_tests` registration in this tree tells ctest that exit 4 means
# SKIPPED (`check-catch-skip-return-code`). That is exact only under a contract Catch2 has
# kept since 3.7.1: a run whose every case skipped exits 4, and ANY assertion failure exits
# one fixed value whatever the count. Before 3.7.1 the exit code WAS the failed-assertion
# count, so a case failing exactly four assertions exited 4 and ctest scored it *skipped*
# (#1128), and no rule in this tree could see it. So the contract is asserted HERE, against
# the Catch2 the tree actually links, by running each shape of `src/tests/CatchSkipCanary.cpp`
# in its own process -- the way `catch_discover_tests` runs a case -- and reading its status:
#
#   1. every SKIP-ONLY shape exits 4: a bare skip, a skip after a padded and a twice-padded
#      summary column, a skip in a later section after earlier ones ran;
#   2. every shape carrying ANY failure exits ONE value, the same for all of them, that is
#      neither 0 nor 4: one failure, four (the old collision), three hundred (past the old
#      255 clamp), a skip beside four failures, and the two that echo a skip's text;
#   3. a passing case exits 0, so a binary exiting non-zero for everything is a broken
#      canary rather than a contract that holds.
#
# The failure code is not asserted to be 42. What `SKIP_RETURN_CODE 4` needs is that a
# failure cannot spell 4 and does not move with the count; the canary's `static_assert`s pin
# 4 and the failure code against `catch2/catch_session.hpp` at compile time, and reading
# those constants here would mean finding the header of whichever route `find_package` took,
# a second way to be wrong about which Catch2 is linked.
#
# **A red here is a DISCOVERY, not a defect in the tree.** It means the linked Catch2 no
# longer keeps the contract -- most likely a different Catch2 than the exact pin names,
# reached through another route -- and every Catch2 verdict in the run is suspect until it
# is understood. It is not a reason to edit this file until it passes.
#
# Usage:
#   cmake -DCANARY=<path> -P scripts/check-catch-skip-exit-contract.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CANARY)
    message(FATAL_ERROR "CANARY must be set to the catch-skip-canary executable")
endif()
if(NOT EXISTS "${CANARY}")
    message(FATAL_ERROR "catch skip exit contract: no canary at '${CANARY}'")
endif()

set(failures "")

# Run one canary case and hand back its exit status.
#
# A canary that did not RUN is not one that answered nothing -- run from a CMake built
# for another platform the binary cannot be launched at all, `execute_process` yields
# empty output, and every assertion below would then "fail" while naming the wrong
# subject. Refused by name instead.
#
# @param tag Catch2 tag selecting the case.
# @param outStatus Receives the exit status.
function(fastcached_run tag outStatus)
    execute_process(
        COMMAND "${CANARY}" "${tag}"
        OUTPUT_VARIABLE captured
        ERROR_VARIABLE capturedErrors
        RESULT_VARIABLE status)
    if(NOT status MATCHES "^[0-9]+$")
        message("")
        message("  The canary could not be EXECUTED: ${status}")
        message("  binary: ${CANARY}")
        message("")
        message("That is not a verdict about Catch2. Most likely this CMake and that")
        message("binary are for different platforms -- the failure presents as an empty")
        message("report rather than as an error.")
        message(FATAL_ERROR "catch skip exit contract: the canary did not run, so there is no evidence")
    endif()
    set(combined "${captured}${capturedErrors}")
    if(combined STREQUAL "")
        message("")
        message("  The canary ran (exit ${status}) for `${tag}` and printed NOTHING.")
        message("  binary: ${CANARY}")
        message("")
        message("A Catch2 binary always reports. Empty output means this is not the")
        message("binary this check thinks it is -- and without a report there is no")
        message("evidence about Catch2's behaviour in EITHER direction.")
        message("")
        message("This arm exists because the first version of this check lacked it and")
        message("reported `premises have moved` for a stand-in that produced no output at")
        message("all -- naming Catch2 as changed when the instrument was simply not")
        message("Catch2. Did-not-report and reported-something-else are two states.")
        message(FATAL_ERROR "catch skip exit contract: the canary produced no output to read")
    endif()
    set(${outStatus} "${status}" PARENT_SCOPE)
endfunction()

# 3. The control first: a case that passes exits 0.
fastcached_run("[canarypass]" passStatus)
if(NOT passStatus EQUAL 0)
    list(APPEND failures
         "the PASSING control exited ${passStatus}, not 0 -- the canary itself is broken, so no other status below says anything about Catch2")
endif()

# 1. Every skip-only shape exits 4, which is what `SKIP_RETURN_CODE 4` reads as skipped.
set(skipShapes "[canaryskip]" "[canarypadskip]" "[canarywidepad]" "[canarysectionskip]")
foreach(tag IN LISTS skipShapes)
    fastcached_run("${tag}" status)
    if(NOT status EQUAL 4)
        list(APPEND failures
             "the skip-only shape ${tag} exited ${status}, not 4 -- every `SKIP_RETURN_CODE 4` registration now scores that skip as a FAILURE (#499 returning)")
    endif()
endforeach()

# 2. Every shape carrying a failure exits one value, whatever the count, and it is not 4.
set(failShapes "[canaryfailone]" "[canaryfail]" "[canaryfailmany]" "[canarymixed]" "[canarydecoy]" "[canaryadversary]")
set(failureCode "")
foreach(tag IN LISTS failShapes)
    fastcached_run("${tag}" status)
    if(status EQUAL 4)
        list(APPEND failures
             "the failing shape ${tag} exited 4, the skip code -- ctest scores it SKIPPED, a genuinely failing test inside `100% tests passed` (#1128 returning)")
    elseif(status EQUAL 0)
        list(APPEND failures
             "the failing shape ${tag} exited 0 -- a failure Catch2 did not report as one")
    endif()
    if(failureCode STREQUAL "")
        set(failureCode "${status}")
        set(failureCodeFrom "${tag}")
    elseif(NOT status EQUAL failureCode)
        list(APPEND failures
             "the failing shape ${tag} exited ${status} where ${failureCodeFrom} exited ${failureCode} -- the failure code moves with the shape again, so SOME failure count can reach 4")
    endif()
endforeach()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("These are the exit contract `SKIP_RETURN_CODE 4` rests on, run against the")
    message("Catch2 this tree links. A red here means that Catch2 no longer keeps it --")
    message("most likely not the build the exact pin in the top-level CMakeLists names.")
    message("")
    message("That is a discovery and not a defect. Find which Catch2 is linked and why;")
    message("until then no Catch2 verdict in this run can be read. Do NOT edit this")
    message("check until it passes.")
    # FINDINGS, not shapes: one shape can break the contract twice (it exits 4 AND differs from
    # its siblings), and a count of shapes would overstate what is wrong by exactly that.
    message(FATAL_ERROR "catch skip exit contract: ${failureCount} finding(s) against the contract SKIP_RETURN_CODE 4 rests on")
endif()

list(LENGTH skipShapes skipCount)
list(LENGTH failShapes failCount)
message(STATUS "catch skip exit contract: ${skipCount} skip-only shape(s) exit 4, ${failCount} failing shape(s) all exit ${failureCode}, the passing control exits 0")
