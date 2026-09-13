# SPDX-License-Identifier: Apache-2.0
#
# Runs `error-popup-product-canary` with and without the suppression variable and requires
# the product policy in BOTH directions (#1389): a failed assert routed to a file when the
# variable is present, and left on the dialog when it is absent.
#
# Both, because either alone is satisfied by a wrong policy: a canary that suppressed
# unconditionally passes the first run, and one that never suppressed passes the second.
#
# The ctest registration running this carries the variable itself, like every test, so the
# "absent" run removes it explicitly -- asserted from the canary's answer, not assumed.
#
# Usage: cmake -DCANARY=<path> -DVARIABLE=<name> -P scripts/error-popup-product-gate.cmake

cmake_minimum_required(VERSION 3.28)

foreach(required CANARY VARIABLE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "error-popup-product-gate: ${required} must be set")
    endif()
endforeach()
if(NOT EXISTS "${CANARY}")
    message(FATAL_ERROR "error-popup-product-gate: the canary does not exist: ${CANARY}")
endif()

# Run the canary under one environment and read its answer.
# @param outVar receives the mode word, or a description of what came back instead
# @param ARGN the `cmake -E env` arguments that shape the environment
function(fc_ask outVar)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${ARGN} "${CANARY}"
        RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err
        TIMEOUT 60 ENCODING NONE)
    if(NOT status EQUAL 0)
        set(${outVar} "<exit ${status}: ${out}${err}>" PARENT_SCOPE)
    elseif(out MATCHES "assert-report-mode=([a-z]+)")
        set(${outVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${outVar} "<no answer: ${out}${err}>" PARENT_SCOPE)
    endif()
endfunction()

fc_ask(asked "${VARIABLE}=1")
fc_ask(unasked "--unset=${VARIABLE}")

set(problems "")
if(NOT asked STREQUAL "file")
    string(APPEND problems
        "\n  with ${VARIABLE} set, a failed assert is routed to '${asked}', not 'file': a product "
        "binary a test spawns would open the dialog and hang the run")
endif()
if(NOT unasked STREQUAL "window")
    string(APPEND problems
        "\n  with ${VARIABLE} unset, a failed assert is routed to '${unasked}', not 'window': a "
        "developer running this build by hand has lost the dialog and the debugger it offers")
endif()
if(NOT problems STREQUAL "")
    message(FATAL_ERROR "error-popup-product-gate: the product policy is wrong in a direction it must hold:${problems}")
endif()
message(STATUS "error-popup-product-gate: routed to a file with ${VARIABLE} set, left on the dialog without it")
