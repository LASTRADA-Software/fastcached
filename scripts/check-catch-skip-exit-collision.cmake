# SPDX-License-Identifier: Apache-2.0
#
# The reader for #1128's NEGATIVE result.
#
# `.agent/rules/testing.md` and the top-level CMakeLists record that ctest cannot be
# told about a Catch2 skip correctly through any property `catch_discover_tests` offers.
# That is a claim about Catch2's behaviour, and this project's own rule is that **a
# record saying something cannot be done instructs the next session not to try** -- so
# it must have a reader, or it becomes a false rule the day Catch2 changes.
#
# This asserts the FACTS the argument rests on, not the conclusion:
#
#   1. a skip and a four-failure case exit with the SAME status, so the exit channel
#      cannot separate them;
#   2. Catch2's exit code IS the failed-assertion count, which is why no free value
#      exists to move the skip signal to;
#   3. a case that only FAILS can emit a line matching the summary text, so the output
#      channel is shared with the subject;
#   4. a case that both skips and fails reports `failed` in its SUMMARY while keeping
#      `SKIPPED:` in its BODY -- the property that makes the summary sound and the
#      marker unsound.
#
# **A red here is a DISCOVERY, not a defect.** It means one of the premises has moved
# and the ticket should be reopened, because a fix may now be possible. It is not a
# reason to edit this file until it passes.
#
# Usage:
#   cmake -DCANARY=<path> -P scripts/check-catch-skip-exit-collision.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CANARY)
    message(FATAL_ERROR "CANARY must be set to the catch-skip-canary executable")
endif()
if(NOT EXISTS "${CANARY}")
    message(FATAL_ERROR "catch skip collision: no canary at '${CANARY}'")
endif()

set(failures "")

# Run one canary case and hand back its status and output.
#
# A canary that did not RUN is not one that answered nothing -- run from a CMake built
# for another platform the binary cannot be launched at all, `execute_process` yields
# empty output, and every assertion below would then "fail" while naming the wrong
# subject. Refused by name instead.
#
# @param tag Catch2 tag selecting the case.
# @param outStatus Receives the exit status.
# @param outText Receives the combined output.
function(fastcached_run tag outStatus outText)
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
        message(FATAL_ERROR "catch skip collision: the canary did not run, so there is no evidence")
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
        message(FATAL_ERROR "catch skip collision: the canary produced no output to read")
    endif()
    set(${outStatus} "${status}" PARENT_SCOPE)
    set(${outText} "${combined}" PARENT_SCOPE)
endfunction()

# 1 and 2. The collision itself, and why no other exit value is free.
fastcached_run("[canaryskip]" skipStatus skipText)
fastcached_run("[canaryfail]" failStatus failText)

if(NOT skipStatus EQUAL failStatus)
    list(APPEND failures
         "a SKIP exits ${skipStatus} and a four-failure case exits ${failStatus} -- they no longer collide, so the exit status CAN separate skipped from failed and #1128 may be fixable by exit code again")
endif()
if(NOT failStatus EQUAL 4)
    list(APPEND failures
         "a case failing exactly four assertions exited ${failStatus}, not 4 -- Catch2's exit code may no longer BE the failed-assertion count, which is the premise that leaves no free value for a skip signal")
endif()

# 3. The output channel is shared with text the subject controls.
fastcached_run("[canaryadversary]" adversaryStatus adversaryText)
if(NOT adversaryText MATCHES "test cases:[ -~]*1 skipped")
    list(APPEND failures
         "a case that only FAILS no longer emits a line matching the summary text -- the output channel may no longer be shared with author-controlled text, which is the premise that rules out SKIP_REGULAR_EXPRESSION")
endif()
if(NOT adversaryText MATCHES "test cases:[ -~]*1 failed")
    list(APPEND failures
         "the adversarial case's own SUMMARY no longer reports `1 failed`, so this check is no longer measuring what it claims")
endif()

# 4. Failed outranks skipped in the summary, and the marker survives in the body.
fastcached_run("[canarymixed]" mixedStatus mixedText)
if(mixedText MATCHES "test cases:[ -~]*1 skipped")
    list(APPEND failures
         "a case that both skips and fails now reports `1 skipped` in its summary -- `Totals::delta` no longer buckets a case once with failed outranking skipped, and the summary line is no longer a sound discriminator")
endif()
if(NOT mixedText MATCHES "SKIPPED:")
    list(APPEND failures
         "a case that both skips and fails no longer prints the `SKIPPED:` marker in its body -- the reason the marker is unsound as a discriminator may have gone away")
endif()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("These are the premises #1128's NEGATIVE result rests on: that ctest cannot")
    message("be told about a Catch2 skip through any property catch_discover_tests")
    message("offers. A red here means a premise has MOVED.")
    message("")
    message("That is a discovery and not a defect. Reopen the ticket and re-derive --")
    message("a fix may now be possible. Do NOT edit this check until it passes.")
    message(FATAL_ERROR "catch skip collision: ${failureCount} premise(s) of #1128 no longer hold")
endif()

message(STATUS "catch skip collision: skip and four-failure both exit ${skipStatus}; the output channel is still shared with author text; failed still outranks skipped in the summary")
