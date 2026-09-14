# SPDX-License-Identifier: Apache-2.0
#
# `counter-extent-link` must be SEEN to refuse (#1361).
#
# That check's passing run says a skewed unit failed to link and its control linked. What
# it cannot say about itself is whether it would notice the guard being GONE -- and a link
# fixture has more ways to report a pass for the wrong reason than most checks: the skewed
# unit falling back to the healthy header, a definition that never links at all, an anchor
# that no longer places the skew. So each is built here from a copy of the tree's two files:
#
#   `noanchor`     -- the header's `static` initializer removed, so nothing references the
#                     symbol. The skewed unit LINKS, and the check must say so.
#   `nodefinition` -- the definition renamed, so even the control cannot link. The check must
#                     refuse as a broken control rather than read the skewed failure as a pass.
#   `noplace`      -- no `Last,` where the skew is inserted. Refused before anything builds.
#
# One configuration (Debug) for the two that build: the refusal under test is the script's,
# and the Release half of the guard itself is what the check proper runs.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> -DFASTCACHED_CXX_COMPILER=<c++>
#         [-DFASTCACHED_GENERATOR=<gen>] [-DFASTCACHED_MAKE_PROGRAM=<make>]
#         -P scripts/check-counter-extent-link-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output; `SKIP: `
# when the check under test itself skips (no working compiler environment).

cmake_minimum_required(VERSION 3.28)

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR FASTCACHED_CXX_COMPILER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-counter-extent-link.cmake")
set(header "${FASTCACHED_SOURCE_DIR}/src/FastCache/Metrics/IMetricsSink.hpp")
set(definition "${FASTCACHED_SOURCE_DIR}/src/FastCache/Metrics/IMetricsSink.cpp")
foreach(input IN ITEMS "${check}" "${header}" "${definition}")
    if(NOT EXISTS "${input}")
        message(FATAL_ERROR "missing: ${input}")
    endif()
endforeach()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
file(READ "${header}" headerText)
file(READ "${definition}" definitionText)

# The lines each case removes. Each must occur exactly once in the tree's files, or this
# selftest has stopped describing the guard and would build a tree identical to the real one.
set(anchorLine "[[maybe_unused]] static bool const CounterExtentLinked = CounterSkew::RequireExtent({});")
set(definitionLine "bool RequireExtent(Extent<CounterCount> /*extent*/) noexcept")
foreach(pair IN ITEMS "headerText|anchorLine" "definitionText|definitionLine")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 textVar)
    list(GET pair 1 lineVar)
    string(FIND "${${textVar}}" "${${lineVar}}" first)
    string(FIND "${${textVar}}" "${${lineVar}}" last REVERSE)
    if(first EQUAL -1 OR NOT first EQUAL last)
        message(FATAL_ERROR "the selftest's anchor `${${lineVar}}` does not occur exactly once; update ${CMAKE_CURRENT_LIST_FILE}")
    endif()
endforeach()

set(failures)
set(cases 0)
set(skipped FALSE)

# @param name The case.
# @param headerBody What the tree's header holds.
# @param definitionBody What the tree's definition holds.
# @param outObjected Set TRUE when the check reported `CMake Error` or `CMake Warning`.
# @param outOutput Set to its output, whitespace collapsed.
# @param outSkipped Set TRUE when the check skipped.
function(fastcached_run_case name headerBody definitionBody outObjected outOutput outSkipped)
    set(tree "${root}/${name}")
    file(WRITE "${tree}/src/FastCache/Metrics/IMetricsSink.hpp" "${headerBody}")
    file(WRITE "${tree}/src/FastCache/Metrics/IMetricsSink.cpp" "${definitionBody}")
    set(arguments
        "-DFASTCACHED_SOURCE_DIR=${tree}" "-DFASTCACHED_SCRATCH_DIR=${tree}/s"
        "-DFASTCACHED_CXX_COMPILER=${FASTCACHED_CXX_COMPILER}" "-DFASTCACHED_CONFIGS=Debug")
    if(FASTCACHED_GENERATOR)
        list(APPEND arguments "-DFASTCACHED_GENERATOR=${FASTCACHED_GENERATOR}")
    endif()
    if(FASTCACHED_MAKE_PROGRAM)
        list(APPEND arguments "-DFASTCACHED_MAKE_PROGRAM=${FASTCACHED_MAKE_PROGRAM}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" ${arguments} -P "${check}"
                    OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${captured}${capturedErrors}")
    set(objected FALSE)
    if(combined MATCHES "CMake Error|CMake Warning")
        set(objected TRUE)
    endif()
    set(sawSkip FALSE)
    if(combined MATCHES "SKIP: ")
        set(sawSkip TRUE)
    endif()
    set(${outObjected} ${objected} PARENT_SCOPE)
    set(${outOutput} "${combined}" PARENT_SCOPE)
    set(${outSkipped} ${sawSkip} PARENT_SCOPE)
endfunction()

# 1. The guard removed.
math(EXPR cases "${cases} + 1")
string(REPLACE "${anchorLine}" "" noAnchorHeader "${headerText}")
fastcached_run_case(noanchor "${noAnchorHeader}" "${definitionText}" objected output sawSkip)
if(sawSkip)
    # The check's own output is not repeated: a skip must not carry text the registration reads as a failure.
    message("SKIP: counter-extent-link skipped in this environment, so none of its refusals can be watched")
    return()
endif()
if(NOT objected)
    list(APPEND failures "noanchor: with the header's static initializer removed the check still passed -- it cannot tell a working guard from none")
else()
    string(FIND "${output}" "LINKED against the real definition" position)
    if(position EQUAL -1)
        list(APPEND failures "noanchor: it refused, but not as a skewed unit that LINKED, so the reader is sent somewhere else: ${output}")
    endif()
endif()

# 2. No definition: the control cannot link either.
math(EXPR cases "${cases} + 1")
string(REPLACE "${definitionLine}" "bool RenamedCounterExtent(Extent<CounterCount> /*extent*/) noexcept"
       renamedDefinition "${definitionText}")
fastcached_run_case(nodefinition "${headerText}" "${renamedDefinition}" objected output sawSkip)
if(NOT objected)
    list(APPEND failures "nodefinition: nothing defines the symbol and the check passed -- a skewed link failing for any reason was read as the guard")
else()
    string(FIND "${output}" "the CONTROL did not link" position)
    if(position EQUAL -1)
        list(APPEND failures "nodefinition: it refused, but not as a broken control: ${output}")
    endif()
endif()

# 3. Nowhere to place the skew.
math(EXPR cases "${cases} + 1")
string(REPLACE "\n        Last,\n" "\n        Last\n" noPlaceHeader "${headerText}")
fastcached_run_case(noplace "${noPlaceHeader}" "${definitionText}" objected output sawSkip)
if(NOT objected)
    list(APPEND failures "noplace: the skew could not be inserted and the check passed -- it built two copies of one header")
else()
    string(FIND "${output}" "cannot place the skew" position)
    if(position EQUAL -1)
        list(APPEND failures "noplace: it refused, but not as an anchor it could not find: ${output}")
    endif()
endif()

if(failures)
    list(LENGTH failures failureCount)
    string(REPLACE ";" "\n  " rendered "${failures}")
    message(FATAL_ERROR "counter-extent-link selftest: ${failureCount} of ${cases} case(s) wrong\n  ${rendered}")
endif()
message(STATUS "counter extent link selftest: ${cases} case(s), every verdict as expected")
