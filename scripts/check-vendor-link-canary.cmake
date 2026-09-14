# SPDX-License-Identifier: Apache-2.0
#
# The vendored link probe must be SEEN to refuse (#1376).
#
# `fastcache-tui-linkprobe` links every vendored object the build compiles, and a green build is
# its accepting direction. Its refusing direction had been watched once, by hand. A guard nobody
# watches refuse is not known to work: a linker told to allow undefined symbols, or objects that
# stopped reaching the link, would leave the probe passing over a tree missing an implementation,
# which is the exact failure it exists for. So this builds `fastcache-tui-linkprobe-canary` -- the
# same probe with one implementation left out -- and requires the link to FAIL naming the
# definition it lacks.
#
# THREE verdicts, because two are not enough and the third is the one that gets collapsed:
#
#   the canary LINKED                      refused: the guard accepts a tree missing a definition,
#                                          or the filter in vendor/CMakeLists.txt removed nothing
#   it FAILED, not naming the symbol       refused: it failed for some other reason (a compile error,
#                                          a missing target), which says nothing about the guard
#   it FAILED, naming the symbol           the guard refuses as it must
#
# Usage:
#   cmake -DFASTCACHED_BUILD_DIR=<dir> -DFASTCACHED_BUILD_CONFIG=<config> \
#         -DFASTCACHED_CANARY_TARGET=<target> -DFASTCACHED_CANARY_SYMBOL=<text> \
#         -P scripts/check-vendor-link-canary.cmake
#
# `FASTCACHED_CANARY_COMMAND` replaces the build with a stand-in, which is how the selftest drives
# each verdict without a build tree. The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

foreach(required IN ITEMS FASTCACHED_CANARY_SYMBOL)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "vendor-link-canary: ${required} must be set")
    endif()
endforeach()

if(DEFINED FASTCACHED_CANARY_COMMAND)
    set(command ${FASTCACHED_CANARY_COMMAND})
    set(what "the stand-in command")
else()
    foreach(required IN ITEMS FASTCACHED_BUILD_DIR FASTCACHED_CANARY_TARGET)
        if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
            message(FATAL_ERROR "vendor-link-canary: ${required} must be set")
        endif()
    endforeach()
    set(command "${CMAKE_COMMAND}" --build "${FASTCACHED_BUILD_DIR}" --target "${FASTCACHED_CANARY_TARGET}")
    # A multi-config generator needs the configuration; a single-config one ignores it.
    if(DEFINED FASTCACHED_BUILD_CONFIG AND NOT "${FASTCACHED_BUILD_CONFIG}" STREQUAL "")
        list(APPEND command --config "${FASTCACHED_BUILD_CONFIG}")
    endif()
    set(what "building ${FASTCACHED_CANARY_TARGET}")
endif()

execute_process(
    COMMAND ${command}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
)
string(APPEND output "${errors}")

# A command that did not START answers with a string rather than an exit status. Read as a
# failure it would be "failed for another reason" at best, and it is neither verdict.
if(NOT result MATCHES "^-?[0-9]+$")
    message(FATAL_ERROR "vendor-link-canary: ${what} did not run (${result}), so no verdict was reached.")
endif()

if(result EQUAL 0)
    message(FATAL_ERROR
        "vendor-link-canary: ${what} SUCCEEDED, and it must not. The canary is the link probe with one "
        "implementation left out, so it linking means the probe would accept a tree missing that "
        "definition: either the linker has been told to allow undefined symbols, or the object FILTER "
        "in vendor/CMakeLists.txt removed nothing (object paths are the generator's). Do not relax this "
        "check -- a link probe that cannot refuse is the defect #1376 records.")
endif()

string(FIND "${output}" "${FASTCACHED_CANARY_SYMBOL}" named)
if(named EQUAL -1)
    string(LENGTH "${output}" length)
    if(length GREATER 2000)
        math(EXPR start "${length} - 2000")
        string(SUBSTRING "${output}" ${start} 2000 output)
    endif()
    message(FATAL_ERROR
        "vendor-link-canary: ${what} failed (exit ${result}) without naming `${FASTCACHED_CANARY_SYMBOL}`, "
        "so it failed for some other reason and says nothing about whether the link probe refuses. "
        "The tail of its output:\n${output}")
endif()

message(STATUS "vendor-link-canary: refused as it must -- ${what} failed (exit ${result}) naming "
               "`${FASTCACHED_CANARY_SYMBOL}`")
