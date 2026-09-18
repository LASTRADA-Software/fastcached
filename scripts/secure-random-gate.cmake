# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy the
# project has not stated, and this one tests list membership.
cmake_minimum_required(VERSION 3.28)
#
# Refuse to report green unless separate PROCESSES draw separate handshake nonces.
#
# ## Why processes, and why a gate rather than a Catch2 case
#
# #1527: nonces came from an engine seeded ONCE per process from `std::random_device`, which
# on the host #1507 was measured on answers zero for 57% of draws. Every process that seeded
# zero drew the SAME stream -- and within any one process that stream never repeats, so every
# in-process "two nonces differ" case passed on the broken build. The property that failed is
# across processes, so it is asked of processes: `secure-random-probe --source=os` is run
# `Processes` times, `PerProcess` nonces each, and any nonce seen twice is refused.
#
# ## The control, run FIRST
#
# A repeat detector nobody has watched refuse is not known to work, and a green verdict from a
# broken one is indistinguishable from a sound generator. So before the real run, the probe's
# `seeded-engine` source -- the pre-#1527 construction, seeded with the zero the broken host
# produced, drawn through the real `DrawNonce` -- is run in TWO processes, and the same
# detector must report every one of the second's nonces as a repeat. Anything else refuses.
#
# ## Four outcomes, kept apart
#
#   * a probe run did not exit 0, or printed the wrong number of nonces -> nothing measured, FAIL
#   * the control did not repeat                                       -> the detector is blind, FAIL
#   * the OS source repeated, or drew an all-zero nonce                -> the defect, FAIL
#   * the control repeated and the OS source did not                   -> PASS
#
# ## How likely a false red is, stated rather than implied
#
# With a sound generator, the chance that any two of the `Processes x PerProcess` nonces
# collide is at most (n choose 2) / 2^256 -- below 2^-243 for the 128 drawn here -- and an
# all-zero nonce is 2^-256 per draw. Neither is a flake budget; both are zero for any purpose.
#
# Reports failure by printing `CMake Error`, the contract every `cmake -P` check here is
# registered under -- `src/tests/CMakeLists.txt` holds the one spelling of the pattern.
#
# Usage:
#   cmake -DFASTCACHED_PROBE=<path to secure-random-probe> -P scripts/secure-random-gate.cmake

if(NOT DEFINED FASTCACHED_PROBE)
    message(FATAL_ERROR "secure-random-gate: FASTCACHED_PROBE must be set to the secure-random-probe executable")
endif()
if(NOT EXISTS "${FASTCACHED_PROBE}")
    message(FATAL_ERROR "secure-random-gate: the probe does not exist: ${FASTCACHED_PROBE}")
endif()

set(Processes 8)
set(PerProcess 16)

# Run the probe @p runs times over @p source and collect every nonce, each tagged with the run
# that drew it so a repeat can say which two processes agreed.
function(fastcached_draw_nonces source runs outVar)
    set(tagged "")
    foreach(run RANGE 1 ${runs})
        execute_process(
            COMMAND "${FASTCACHED_PROBE}" "--source=${source}" "--count=${PerProcess}"
            RESULT_VARIABLE probeResult
            OUTPUT_VARIABLE probeOut
            ERROR_VARIABLE probeErr
            TIMEOUT 60
        )
        if(NOT probeResult STREQUAL "0")
            message(FATAL_ERROR
                "secure-random-gate: run ${run} of the ${source} probe exited '${probeResult}', so nothing was "
                "measured. It said:\n${probeOut}${probeErr}")
        endif()

        string(REGEX MATCHALL "nonce [0-9a-f]+" found "${probeOut}")
        list(LENGTH found drawn)
        if(NOT drawn EQUAL PerProcess)
            message(FATAL_ERROR
                "secure-random-gate: run ${run} of the ${source} probe printed ${drawn} nonces where ${PerProcess} "
                "were asked for, so its output is not the probe's contract. It said:\n${probeOut}${probeErr}")
        endif()

        foreach(line IN LISTS found)
            string(SUBSTRING "${line}" 6 -1 hex)
            string(LENGTH "${hex}" digits)
            if(NOT digits EQUAL 64)
                message(FATAL_ERROR
                    "secure-random-gate: run ${run} of the ${source} probe printed a nonce of ${digits} hex digits, "
                    "not 64: ${hex}")
            endif()
            list(APPEND tagged "${run}:${hex}")
        endforeach()
    endforeach()
    set(${outVar} "${tagged}" PARENT_SCOPE)
endfunction()

# Every entry of @p tagged whose nonce an EARLIER entry already carried.
function(fastcached_repeated_nonces tagged outVar)
    set(seen "")
    set(repeats "")
    foreach(entry IN LISTS tagged)
        string(REGEX REPLACE "^[0-9]+:" "" hex "${entry}")
        if(hex IN_LIST seen)
            list(APPEND repeats "${entry}")
        else()
            list(APPEND seen "${hex}")
        endif()
    endforeach()
    set(${outVar} "${repeats}" PARENT_SCOPE)
endfunction()

# The control first: the detector must be seen to fire before its silence means anything.
fastcached_draw_nonces(seeded-engine 2 control)
fastcached_repeated_nonces("${control}" controlRepeats)
list(LENGTH controlRepeats controlRepeated)
if(NOT controlRepeated EQUAL PerProcess)
    message(FATAL_ERROR
        "secure-random-gate: the control -- two processes drawing through an engine seeded zero, which draw "
        "identical streams by construction -- produced ${controlRepeated} repeats where ${PerProcess} were certain. "
        "The repeat detector cannot see a repeat, so its verdict on the operating system's generator below would "
        "mean nothing; nothing was concluded.")
endif()
message(STATUS "secure-random-gate: the control repeated ${controlRepeated} of ${PerProcess} nonces across two "
               "processes, as a zero-seeded engine must")

fastcached_draw_nonces(os ${Processes} drawn)
list(LENGTH drawn total)

foreach(entry IN LISTS drawn)
    if(entry MATCHES ":0+$")
        message(FATAL_ERROR "secure-random-gate: the operating system's generator drew an all-zero nonce (${entry})")
    endif()
endforeach()

fastcached_repeated_nonces("${drawn}" repeats)
list(LENGTH repeats repeated)
if(NOT repeated EQUAL 0)
    message(FATAL_ERROR
        "secure-random-gate: the operating system's generator repeated ${repeated} of ${total} nonces across "
        "${Processes} processes (run:nonce): ${repeats}. A restarted acceptor re-issuing an earlier run's challenge "
        "is exactly what #1527 closes; see FastCache/Core/ISecureRandom.hpp")
endif()

message(STATUS "secure-random-gate: ${total} nonces from ${Processes} processes, none repeated and none zero")
