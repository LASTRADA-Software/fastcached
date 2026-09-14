# SPDX-License-Identifier: Apache-2.0
#
# `vendor-tui-link-closure-refuses` must be SEEN to reach each of its verdicts (#1376).
#
# The check reads a build's exit status AND its output, and the verdict that matters is the one a
# two-state reading loses: a canary that failed for an unrelated reason is not a guard that refused.
# Each case below hands the check a stand-in command instead of a build:
#
#   linked            exits 0                                   refused: the guard accepted
#   otherReason       fails, never naming the symbol            refused: says nothing about the guard
#   refused           fails, naming the symbol                  ACCEPTED: the guard refuses as it must
#   didNotRun         a program that does not exist             refused: no verdict at all
#   symbolElsewhere   exits 0 while printing the symbol         refused: naming it is not failing
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-vendor-link-canary-selftest.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

foreach(required IN ITEMS FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-vendor-link-canary.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(symbol "SignalHandler::hasPendingSigint")
file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${FASTCACHED_SCRATCH_DIR}")

# A stand-in build: prints @p text, then exits 0 or not. `message(FATAL_ERROR)` is how a
# `cmake -P` script exits non-zero; its `CMake Error` stays inside the check's captured output.
function(fastcached_stub name text fails out)
    set(stub "${FASTCACHED_SCRATCH_DIR}/${name}.cmake")
    if(fails)
        file(WRITE "${stub}" "message(\"${text}\")\nmessage(FATAL_ERROR \"the stand-in build stops\")\n")
    else()
        file(WRITE "${stub}" "message(\"${text}\")\n")
    endif()
    set(${out} "${CMAKE_COMMAND};-P;${stub}" PARENT_SCOPE)
endfunction()

set(ran 0)
set(mismatches "")

# Runs the check with @p command and requires its output to contain @p expected, and to contain
# `CMake Error` exactly when @p refuses.
function(fastcached_judge name command refuses expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_CANARY_SYMBOL=${symbol}" "-DFASTCACHED_CANARY_COMMAND=${command}"
                -P "${check}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
    )
    string(APPEND output "${errors}")
    # Both words, as ctest's own `FASTCACHED_SCRIPT_CHECK_FAILED` reads a check: a sub-run that
    # merely WARNS must not be scored a clean pass here while ctest would refuse it.
    set(refused OFF)
    if(output MATCHES "CMake Error|CMake Warning")
        set(refused ON)
    endif()
    string(FIND "${output}" "${expected}" said)
    set(problem "")
    if(refuses AND NOT refused)
        set(problem "accepted, and must refuse")
    elseif(NOT refuses AND refused)
        set(problem "refused, and must accept")
    elseif(said EQUAL -1)
        set(problem "reached the right side for the wrong reason: no `${expected}`")
    endif()
    math(EXPR count "${ran} + 1")
    set(ran ${count} PARENT_SCOPE)
    if(NOT problem STREQUAL "")
        set(mismatches "${mismatches}\n  ${name}: ${problem}\n${output}" PARENT_SCOPE)
    endif()
endfunction()

fastcached_stub(linked "linking fastcache-tui-linkprobe-canary" OFF command)
fastcached_judge(linked "${command}" ON "SUCCEEDED")

fastcached_stub(otherReason "ninja: error: unknown target 'fastcache-tui-linkprobe-canary'" ON command)
fastcached_judge(otherReason "${command}" ON "without naming")

fastcached_stub(refused "undefined reference to `endo::platform::${symbol}()'" ON command)
fastcached_judge(refused "${command}" OFF "refused as it must")

fastcached_judge(didNotRun "${FASTCACHED_SCRATCH_DIR}/no-such-build-tool" ON "did not run")

fastcached_stub(symbolElsewhere "warning: ${symbol} is deprecated" OFF command)
fastcached_judge(symbolElsewhere "${command}" ON "SUCCEEDED")

if(NOT mismatches STREQUAL "")
    message(FATAL_ERROR "vendor-link-canary-selftest: ${ran} case(s) ran, and these did not judge as they must:${mismatches}")
endif()
message(STATUS "vendor-link-canary-selftest: ${ran} case(s) ran, every verdict as it must be")
