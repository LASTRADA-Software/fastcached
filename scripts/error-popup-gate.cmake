# SPDX-License-Identifier: Apache-2.0
#
# Runs `error-popup-canary` and requires that its Debug assert ENDED the process, promptly,
# with the assertion's own text -- never a modal dialog waiting for a click (#1389).
#
# Three outcomes, because a canary that can only answer the two expected ones answers one
# of them whatever it sees:
#
#   * the process outlived the bound: on Windows that is the "Debug Assertion Failed" box
#     holding it, which is the defect -- REFUSED, and the refusal says so;
#   * the process exited 0: the assert did not fire at all (NDEBUG, or not a Debug CRT), so
#     nothing was established -- REFUSED;
#   * it exited non-zero: accepted only with the assertion text on its output, so a crash
#     for any other reason is not read as the assert firing.
#
# Usage: cmake -DCANARY=<path> [-DBOUND_SECONDS=60] -P scripts/error-popup-gate.cmake

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CANARY)
    message(FATAL_ERROR "error-popup-gate: CANARY must name the canary executable")
endif()
if(NOT EXISTS "${CANARY}")
    message(FATAL_ERROR "error-popup-gate: the canary does not exist: ${CANARY}")
endif()
if(NOT DEFINED BOUND_SECONDS)
    # Far below the registration's TIMEOUT, so a dialog is reported by THIS script, naming
    # the cause, rather than by ctest's kill, which names nothing.
    set(BOUND_SECONDS 60)
endif()

set(expected "error-popup-canary: this assert must end the process")

string(TIMESTAMP started "%s")
execute_process(
    COMMAND "${CANARY}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT ${BOUND_SECONDS}
    ENCODING NONE
)
string(TIMESTAMP ended "%s")
math(EXPR elapsed "${ended} - ${started}")
set(output "${out}${err}")

if(NOT status MATCHES "^-?[0-9]+$")
    message(FATAL_ERROR
        "error-popup-gate: the canary did not END within ${BOUND_SECONDS}s (${status}). Its assert "
        "fires in the first line of its only case, so a process still alive is one waiting on "
        "something -- on Windows, the \"Debug Assertion Failed\" dialog this canary exists to prove "
        "gone. Check that cmake/ErrorPopups.cmake attached src/tests/ErrorPopupsAtStartup.cpp "
        "to it. Output so far:\n${output}")
endif()

if(status EQUAL 0)
    message(FATAL_ERROR
        "error-popup-gate: the canary exited 0, so its assert never fired -- this build defines "
        "NDEBUG or is not a Debug CRT, and nothing about dialogs was established. Output:\n${output}")
endif()

string(FIND "${output}" "${expected}" position)
if(position EQUAL -1)
    message(FATAL_ERROR
        "error-popup-gate: the canary exited ${status} after ${elapsed}s WITHOUT the assertion text "
        "'${expected}', so it ended for some other reason and the assert was not seen to fire. "
        "Output:\n${output}")
endif()

message(STATUS
    "error-popup-gate: the assert ended the process (exit ${status}, ${elapsed}s by the wall "
    "clock) and printed its own text, with no dialog")
