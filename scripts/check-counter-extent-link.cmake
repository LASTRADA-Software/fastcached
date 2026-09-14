# SPDX-License-Identifier: Apache-2.0
#
# A unit compiled against a different `IMetricsSink::Counter` must FAIL TO LINK, and this
# builds one to watch it fail (#1361).
#
# Every table keyed by the enum is sized by `Counter::Last`, so a build linking objects
# compiled against two versions of the enum disagrees about those extents -- an ODR
# violation that nothing at run time can be sound against, and that has already happened
# in this tree (#1332: a unit compiled before seven counters were added, a counter dropped
# and a zero reported). `Metrics/IMetricsSink.hpp` makes every including unit ask, at static
# initialisation, for `CounterSkew::RequireExtent` at ITS count, and
# `Metrics/IMetricsSink.cpp` defines it for one count only.
#
# No single-TU test can observe any of that, and a guard nobody has watched refuse is not
# known to work -- the mechanism's whole claim is about what a LINKER keeps, which is a
# claim about a tool. So this builds a fixture project with THIS build's compiler and
# generator:
#
#   control -- a unit against a byte-identical copy of the header, plus the real definition:
#              must LINK. A guard that refused everything would pass the next step alone.
#   skewed  -- the same, plus a unit against a copy with one enumerator inserted before
#              `Last`: must FAIL, with `RequireExtent` in the output, and only after
#              the skewed unit COMPILED -- a compile error would satisfy "the build failed"
#              for a reason that has nothing to do with the linker.
#
# In Debug AND Release, because the optimiser is where a reference that looks load-bearing
# disappears -- measured, and recorded beside the guard in the header: an `inline` shape and
# an uncalled reference each LINKED a skewed unit on at least one compiler. The header's own
# comment says why the call site is a `static` initializer; this is what checks it.
#
# Every source the fixture compiles is a COPY taken into the scratch directory, so a
# selftest can hand this script a tree whose header has the guard removed and watch it
# refuse -- and the copies are asserted byte-identical to the tree's.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir>
#         -DFASTCACHED_CXX_COMPILER=<c++> [-DFASTCACHED_GENERATOR=<gen>]
#         [-DFASTCACHED_MAKE_PROGRAM=<make>] [-DFASTCACHED_CONFIGS=Debug;Release]
#         -P scripts/check-counter-extent-link.cmake
#
# Verdict from the OUTPUT (`FAIL_REGULAR_EXPRESSION`), as every `cmake -P` check here
# (`scripts/check-script-check-signals.cmake`). `SKIP: ` when the fixture cannot configure in
# this environment at all -- a Windows shell that is not a Developer shell has a `cl.exe`
# that exists and cannot compile.

cmake_minimum_required(VERSION 3.28)

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR FASTCACHED_CXX_COMPILER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set (cmake -D${required}=... -P ${CMAKE_CURRENT_LIST_FILE})")
    endif()
endforeach()
if(NOT DEFINED FASTCACHED_CONFIGS)
    set(FASTCACHED_CONFIGS Debug Release)
endif()

set(header "${FASTCACHED_SOURCE_DIR}/src/FastCache/Metrics/IMetricsSink.hpp")
set(definition "${FASTCACHED_SOURCE_DIR}/src/FastCache/Metrics/IMetricsSink.cpp")
foreach(input IN ITEMS "${header}" "${definition}")
    if(NOT EXISTS "${input}")
        message(FATAL_ERROR "the fixture's input is missing: ${input}")
    endif()
endforeach()
if(NOT EXISTS "${FASTCACHED_CXX_COMPILER}")
    message("SKIP: no C++ compiler at ${FASTCACHED_CXX_COMPILER}, so no project can be configured")
    return()
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
# Short directory names on purpose: a compiler's try-compile nests five levels under the build
# directory, and the selftest nests this whole scratch tree again. Past MAX_PATH MSVC's configure
# fails with "Cannot open compiler generated file" (measured).
set(project "${root}/p")

# ---------------------------------------------------------------------------
# The two headers. The skew is ONE enumerator inserted where a real change inserts one --
# before `Last` -- and the anchor must match exactly once, or this would build two copies of
# one header and report the linker as broken.
file(READ "${header}" healthyText)
set(lastAnchor "\n        Last,\n")
string(FIND "${healthyText}" "${lastAnchor}" anchorAt)
string(FIND "${healthyText}" "${lastAnchor}" anchorAtReverse REVERSE)
if(anchorAt EQUAL -1 OR NOT anchorAt EQUAL anchorAtReverse)
    message(FATAL_ERROR
        "cannot place the skew: `Last,` at the enum's indentation occurs ${anchorAt}/${anchorAtReverse} "
        "(first/last offset) in ${header}, and it must occur exactly once. If the enum moved or was "
        "re-indented, update the anchor in ${CMAKE_CURRENT_LIST_FILE}.")
endif()
string(REPLACE "${lastAnchor}" "\n        FixtureSkewInsertedCounter,\n        Last,\n" skewedText "${healthyText}")

# The healthy copy is COPIED, never written back from `file(READ)`: on Windows `file(WRITE)` wrote
# CRLF (measured, 88626 bytes in, 90107 out), which would make the identity check below refuse
# a faithful copy. The skewed copy's line endings do not matter to a compiler.
file(MAKE_DIRECTORY "${project}/healthy/FastCache/Metrics")
file(COPY_FILE "${header}" "${project}/healthy/FastCache/Metrics/IMetricsSink.hpp")
file(WRITE "${project}/skewed/FastCache/Metrics/IMetricsSink.hpp" "${skewedText}")
file(COPY_FILE "${definition}" "${project}/IMetricsSink.cpp")
foreach(pair IN ITEMS "${header}|${project}/healthy/FastCache/Metrics/IMetricsSink.hpp"
                      "${definition}|${project}/IMetricsSink.cpp")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 original)
    list(GET pair 1 copy)
    file(SHA256 "${original}" originalDigest)
    file(SHA256 "${copy}" copyDigest)
    if(NOT originalDigest STREQUAL copyDigest)
        message(FATAL_ERROR "the fixture's copy of ${original} is not byte-identical, so it tests a different header")
    endif()
endforeach()

# The static_assert is the proof that the SKEWED header is the one this unit included: an
# include path that fell back to the healthy copy would link cleanly and read as a guard
# that does not work.
file(WRITE "${project}/healthy.cpp"
    "#include <FastCache/Metrics/IMetricsSink.hpp>\nint HealthyUnit() { return 1; }\n")
file(WRITE "${project}/skewed.cpp"
    "#include <FastCache/Metrics/IMetricsSink.hpp>\n"
    "static_assert(FastCache::IMetricsSink::Counter::FixtureSkewInsertedCounter < FastCache::IMetricsSink::Counter::Last);\n"
    "int SkewedUnit() { return 1; }\n")
file(WRITE "${project}/control_main.cpp"
    "int HealthyUnit();\nint main() { return HealthyUnit() == 1 ? 0 : 1; }\n")
file(WRITE "${project}/skewed_main.cpp"
    "int HealthyUnit();\nint SkewedUnit();\nint main() { return HealthyUnit() + SkewedUnit() == 2 ? 0 : 1; }\n")
file(WRITE "${project}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.28)
project(counter_extent_link LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Separate object libraries, so each unit sees exactly one header and no include order decides which.
add_library(healthy_units OBJECT healthy.cpp IMetricsSink.cpp)
target_include_directories(healthy_units PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/healthy")
add_library(skewed_unit OBJECT skewed.cpp)
target_include_directories(skewed_unit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/skewed")
add_executable(control control_main.cpp $<TARGET_OBJECTS:healthy_units>)
add_executable(skewed skewed_main.cpp $<TARGET_OBJECTS:healthy_units> $<TARGET_OBJECTS:skewed_unit>)
]=])

set(configureArguments "-DCMAKE_CXX_COMPILER=${FASTCACHED_CXX_COMPILER}")
if(FASTCACHED_MAKE_PROGRAM)
    list(APPEND configureArguments "-DCMAKE_MAKE_PROGRAM=${FASTCACHED_MAKE_PROGRAM}")
endif()
if(FASTCACHED_GENERATOR)
    list(APPEND configureArguments -G "${FASTCACHED_GENERATOR}")
endif()

# @param binary The build directory.
# @param config The configuration.
# @param target What to build.
# @param outResult Set to the build's exit status.
# @param outText Set to its output, whitespace runs collapsed (a toolchain wraps long symbols).
function(fastcached_build binary config target outResult outText)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${binary}" --config "${config}" --target "${target}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 300)
    string(REGEX REPLACE "[ \t\r\n]+" " " text "${output}${errors}")
    set(${outResult} "${result}" PARENT_SCOPE)
    set(${outText} "${text}" PARENT_SCOPE)
endfunction()

set(violations "")
set(configsJudged 0)
foreach(config IN LISTS FASTCACHED_CONFIGS)
    set(binary "${root}/${config}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${project}" -B "${binary}" ${configureArguments} "-DCMAKE_BUILD_TYPE=${config}"
        RESULT_VARIABLE configureResult OUTPUT_VARIABLE configureOutput ERROR_VARIABLE configureError TIMEOUT 300)
    if(NOT configureResult EQUAL 0)
        if(configsJudged EQUAL 0)
            # The configure output goes to a FILE and only its path is printed: it holds CMake's own
            # `CMake Error` text, which printed here would turn this SKIP into a failure under the
            # registration's FAIL_REGULAR_EXPRESSION.
            file(WRITE "${root}/configure-failure.log" "${configureOutput}${configureError}")
            message("SKIP: the fixture project does not configure in this environment, so nothing below would be "
                    "measuring the linker (exit ${configureResult}); its output is in ${root}/configure-failure.log")
            return()
        endif()
        message(FATAL_ERROR "the fixture configured for an earlier configuration and not for ${config}:\n"
                            "${configureOutput}${configureError}")
    endif()

    fastcached_build("${binary}" "${config}" control controlResult controlText)
    if(NOT controlResult EQUAL 0)
        list(APPEND violations
             "${config}: the CONTROL did not link -- a healthy unit and the definition, one header between them. "
             "Nothing about the skewed build below can be read while this fails: ${controlText}")
        continue()
    endif()

    fastcached_build("${binary}" "${config}" skewed_unit compileResult compileText)
    if(NOT compileResult EQUAL 0)
        list(APPEND violations
             "${config}: the skewed unit did not COMPILE, so a failure to link it would prove nothing about the "
             "linker: ${compileText}")
        continue()
    endif()

    fastcached_build("${binary}" "${config}" skewed skewedResult skewedText)
    if(skewedResult EQUAL 0)
        list(APPEND violations
             "${config}: a unit compiled against a Counter enum with one more enumerator LINKED against the real "
             "definition. The guard in src/FastCache/Metrics/IMetricsSink.hpp is not reaching the linker in this "
             "configuration -- check that `CounterExtentLinked` is still a `static` whose initializer calls "
             "CounterSkew::RequireExtent, and read that comment for why an inline or unused reference is not one.")
    else()
        string(FIND "${skewedText}" "RequireExtent" named)
        if(named EQUAL -1)
            list(APPEND violations
                 "${config}: the skewed link failed, but its output does not name RequireExtent, so it failed "
                 "for some other reason and says nothing about the guard: ${skewedText}")
        else()
            message("${config}: control linked; skewed unit compiled and its link was refused naming "
                    "RequireExtent")
        endif()
    endif()
    math(EXPR configsJudged "${configsJudged} + 1")
endforeach()

if(NOT violations STREQUAL "")
    list(JOIN violations "\n\n" report)
    message(FATAL_ERROR "counter-extent-link:\n${report}")
endif()
list(LENGTH FASTCACHED_CONFIGS configCount)
if(NOT configsJudged EQUAL configCount)
    message(FATAL_ERROR "counter-extent-link judged ${configsJudged} of ${configCount} configuration(s)")
endif()
message("counter extent link: ${configsJudged} configuration(s) (${FASTCACHED_CONFIGS}), a skewed unit refused at "
        "link time in each, its control linked")
