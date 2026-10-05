# SPDX-License-Identifier: Apache-2.0
#
# How ctest SCORES `compile-cache-dispatch` when the wiring cannot run: a decision failure is
# FAILED, and a clean decision table is SKIPPED.
#
# The dispatch check has two halves, and its registration carries both FAIL_REGULAR_EXPRESSION
# and SKIP_REGULAR_EXPRESSION. ctest lets the skip pattern OUTRANK the failure pattern, so a check
# that reported a broken decision table and then printed its skip marker, because no toolchain
# could run the wiring, scored `***Skipped` and `100% tests passed` -- measured on CMake/ctest
# 4.3.1. The pure half needs no toolchain, so its failure is a verdict on every host, and
# `EndWithoutWiring` in the check now ends such a run FAILED.
#
# A property of the SCORING cannot be read off the script's output by this script without
# restating ctest's precedence rule here, which is the rule that was misread. So this asks ctest:
# it writes a CTestTestfile.cmake whose tests run the real check under the SAME two patterns the
# real registration carries -- read back from the build tree with `ctest --show-only=json-v1`,
# never spelled here, so a registration that changes either pattern is what gets measured -- runs
# ctest over it, and reads each test's verdict.
#
#   planted, no compiler        FAILED   the decision failure outranks the absent wiring
#   planted, broken toolchain   FAILED   the same through the second exit, the stand-in's build
#   clean, no compiler          SKIPPED  the control: the skip still reaches ctest
#   clean, broken toolchain     SKIPPED  the control for the second exit
#
# The "broken toolchain" is this CMake itself named as the C++ compiler: it exists, so the check
# passes its first exit, and it compiles nothing, so the stand-in's configure fails.
#
# The planted failure is one extra decision row the check appends when
# FASTCACHED_DISPATCH_PLANT_DECISION_FAILURE is set; nothing else sets it.
#
# Verdict is read from the OUTPUT, not the exit code; see src/tests/CMakeLists.txt, which
# registers this with FAIL_REGULAR_EXPRESSION (#565).

cmake_minimum_required(VERSION 3.28)

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR FASTCACHED_CTEST_COMMAND FASTCACHED_BUILD_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} must be set (cmake -D${required}=... -P ${CMAKE_CURRENT_LIST_FILE})")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

# The registration's two patterns, as ctest holds them.
execute_process(
    COMMAND "${FASTCACHED_CTEST_COMMAND}" --test-dir "${FASTCACHED_BUILD_DIR}" --show-only=json-v1
            -R "^compile-cache-dispatch$"
    RESULT_VARIABLE listed OUTPUT_VARIABLE listing ERROR_VARIABLE listingError TIMEOUT 120)
if(NOT listed EQUAL 0)
    message(FATAL_ERROR "check-compile-cache-dispatch-verdict: ctest could not list the registration (${listed}): "
                        "${listingError}")
endif()
string(JSON registered LENGTH "${listing}" tests)
if(NOT registered EQUAL 1)
    message(FATAL_ERROR "check-compile-cache-dispatch-verdict: ${registered} tests named compile-cache-dispatch "
                        "in ${FASTCACHED_BUILD_DIR}, expected exactly one")
endif()
set(FAIL_REGULAR_EXPRESSION "")
set(SKIP_REGULAR_EXPRESSION "")
string(JSON propertyCount LENGTH "${listing}" tests 0 properties)
math(EXPR lastProperty "${propertyCount} - 1")
foreach(index RANGE 0 ${lastProperty})
    string(JSON property GET "${listing}" tests 0 properties ${index} name)
    if(property STREQUAL "FAIL_REGULAR_EXPRESSION" OR property STREQUAL "SKIP_REGULAR_EXPRESSION")
        string(JSON patternCount LENGTH "${listing}" tests 0 properties ${index} value)
        math(EXPR lastPattern "${patternCount} - 1")
        foreach(patternIndex RANGE 0 ${lastPattern})
            string(JSON pattern GET "${listing}" tests 0 properties ${index} value ${patternIndex})
            list(APPEND ${property} "${pattern}")
        endforeach()
    endif()
endforeach()
foreach(property FAIL_REGULAR_EXPRESSION SKIP_REGULAR_EXPRESSION)
    if("${${property}}" STREQUAL "")
        message(FATAL_ERROR "check-compile-cache-dispatch-verdict: compile-cache-dispatch carries no ${property}, "
                            "so there is no precedence between the two to measure")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-compile-cache-dispatch.cmake")
set(absentCompiler "${FASTCACHED_SCRATCH_DIR}/no-such-compiler/cl.exe")

# name | planted | compiler | verdict
set(verdictRows
    "planted-no-compiler|ON|${absentCompiler}|Failed"
    "planted-broken-toolchain|ON|${CMAKE_COMMAND}|Failed"
    "clean-no-compiler|OFF|${absentCompiler}|Skipped"
    "clean-broken-toolchain|OFF|${CMAKE_COMMAND}|Skipped"
)

set(suite "${FASTCACHED_SCRATCH_DIR}/suite")
file(REMOVE_RECURSE "${suite}")
file(MAKE_DIRECTORY "${suite}")
set(testfile "# Written by ${CMAKE_CURRENT_LIST_FILE}\n")
foreach(row IN LISTS verdictRows)
    fastcached_row_fields("${row}" name planted compiler verdict)
    set(arguments
        "[==[${CMAKE_COMMAND}]==]"
        "[==[-DFASTCACHED_SOURCE_DIR=${FASTCACHED_SOURCE_DIR}]==]"
        "[==[-DFASTCACHED_SCRATCH_DIR=${FASTCACHED_SCRATCH_DIR}/${name}]==]"
        "[==[-DFASTCACHED_CXX_COMPILER=${compiler}]==]"
        "[==[-DFASTCACHED_DISPATCH_PLANT_DECISION_FAILURE=${planted}]==]")
    if(FASTCACHED_GENERATOR)
        list(APPEND arguments "[==[-DFASTCACHED_GENERATOR=${FASTCACHED_GENERATOR}]==]")
    endif()
    if(FASTCACHED_MAKE_PROGRAM)
        list(APPEND arguments "[==[-DFASTCACHED_MAKE_PROGRAM=${FASTCACHED_MAKE_PROGRAM}]==]")
    endif()
    list(APPEND arguments "-P" "[==[${check}]==]")
    list(JOIN arguments " " commandLine)
    string(APPEND testfile
           "add_test([==[${name}]==] ${commandLine})\n"
           "set_tests_properties([==[${name}]==] PROPERTIES FAIL_REGULAR_EXPRESSION [==[${FAIL_REGULAR_EXPRESSION}]==] "
           "SKIP_REGULAR_EXPRESSION [==[${SKIP_REGULAR_EXPRESSION}]==] TIMEOUT 300)\n")
endforeach()
file(WRITE "${suite}/CTestTestfile.cmake" "${testfile}")

execute_process(
    COMMAND "${FASTCACHED_CTEST_COMMAND}" --test-dir "${suite}" --output-on-failure
    OUTPUT_VARIABLE ctestOutput ERROR_VARIABLE ctestError TIMEOUT 600)
set(ctestReport "${ctestOutput}${ctestError}")

list(LENGTH verdictRows expectedCount)
set(violations "")
set(judged 0)
foreach(row IN LISTS verdictRows)
    fastcached_row_fields("${row}" name planted compiler verdict)
    # One result line per test: `N/M Test #N: <name> .....   Passed` or `***Failed` / `***Skipped`.
    string(REGEX MATCH "Test +#[0-9]+: ${name} [^\n]*" resultLine "${ctestReport}")
    if(resultLine STREQUAL "")
        list(APPEND violations "${name}: ctest printed no result line for it")
        continue()
    endif()
    string(REGEX MATCH "(\\*\\*\\*Failed|\\*\\*\\*Skipped|Passed)" scored "${resultLine}")
    string(REPLACE "***" "" scored "${scored}")
    if(NOT scored STREQUAL verdict)
        list(APPEND violations "${name}: ctest scored it '${scored}', expected '${verdict}' -- ${resultLine}")
    endif()
    math(EXPR judged "${judged} + 1")
endforeach()

if(violations OR NOT judged EQUAL expectedCount)
    string(REPLACE ";" "\n" violations "${violations}")
    # ctest's own report carries the nested checks' `CMake Error`; spell it out of the evidence so
    # the only failure pattern here is the verdict's.
    string(REPLACE "CMake Error" "CMake-Error" ctestReport "${ctestReport}")
    string(REPLACE "CMake Warning" "CMake-Warning" ctestReport "${ctestReport}")
    message(FATAL_ERROR
        "check-compile-cache-dispatch-verdict: judged ${judged} of ${expectedCount} rows:\n${violations}\n\n"
        "ctest said:\n${ctestReport}")
endif()
message(STATUS "check-compile-cache-dispatch-verdict: ${judged} rows scored as the table says "
               "(a planted decision failure FAILED at both exits before the wiring; a clean table SKIPPED)")
