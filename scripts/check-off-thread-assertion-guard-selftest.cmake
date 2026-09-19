# SPDX-License-Identifier: Apache-2.0
#
# Drives `check-off-thread-assertion-guard.cmake` against staged build trees, in both
# directions (#1211).
#
# Each tree holds only a `build.ninja` with link edges in the shapes CMake writes, POSIX and
# Windows spellings both. A refusal asserts WHICH refusal, by a phrase only that refusal
# carries: every refusal prints `CMake Error`, so asserting that alone would pass with the
# arms exchanged. And the output is flattened before it is searched, because CMake wraps
# its diagnostics and a phrase can straddle a line break.
#
# Usage: cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_SCRATCH_DIR=<dir> -P <this>
cmake_minimum_required(VERSION 3.28)

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()
set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-off-thread-assertion-guard.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()
file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}")

set(failures "")
set(cases 0)

# One POSIX link edge, as Ninja writes it for a single-configuration build.
# @param outVar receives the line
# @param name the executable's target name
# @param config the configuration the rule is for
# @param inputs what the edge links, space-separated
function(fc_edge outVar name config inputs)
    set(${outVar} "build target/${name}: CXX_EXECUTABLE_LINKER__${name}_${config} ${inputs} || cmake_object_order_depends_target_${name}\n" PARENT_SCOPE)
endfunction()

# Stage a tree and run the check over it.
# @param label what the case is about
# @param ninja the build.ninja body, or `<none>` for no file
# @param wantPass TRUE when the check must accept the tree
# @param phrase a phrase the output must carry
function(fc_case label ninja wantPass phrase)
    math(EXPR next "${cases} + 1")
    set(cases ${next} PARENT_SCOPE)
    set(tree "${FASTCACHED_SCRATCH_DIR}/case-${next}")
    file(MAKE_DIRECTORY "${tree}")
    if(NOT ninja STREQUAL "<none>")
        file(WRITE "${tree}/build.ninja" "${ninja}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${tree}" -DCONFIG=Debug -DANCHOR=FastCacheTest -P "${check}"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        RESULT_VARIABLE status
    )
    set(text "${out}${err}")
    string(REPLACE "\n" " " flat "${text}")
    string(REGEX REPLACE " +" " " flat "${flat}")
    set(refused FALSE)
    if(flat MATCHES "CMake Error|CMake Warning")
        set(refused TRUE)
    endif()
    set(problem "")
    if(wantPass AND refused)
        set(problem "was REFUSED but must be accepted")
    elseif(NOT wantPass AND NOT refused)
        set(problem "was ACCEPTED but must be refused")
    endif()
    string(FIND "${flat}" "${phrase}" at)
    if(problem STREQUAL "" AND at EQUAL -1)
        set(problem "did not say '${phrase}'")
    endif()
    if(NOT problem STREQUAL "")
        list(APPEND failures "case ${next} (${label}) ${problem}; the check said: ${flat}")
        set(failures "${failures}" PARENT_SCOPE)
    endif()
endfunction()

set(guard "src/tests/CMakeFiles/FastCacheTest.dir/__/tests/OffThreadAssertionGuard.cpp.o")
set(catch2 "_deps/catch2-build/src/libCatch2d.a")

# 1. The ordinary tree: the anchor links Catch2 and carries the guard, and a product binary
#    that links no Catch2 is not asked about at all.
fc_edge(anchor FastCacheTest Debug "src/tests/CMakeFiles/FastCacheTest.dir/a.cpp.o ${guard} | ${catch2}")
fc_edge(product fastcached Debug "src/apps/fastcached/CMakeFiles/fastcached.dir/main.cpp.o | libFastCache.a")
fc_case("covered anchor, product binary ignored" "${anchor}${product}" TRUE "1 of 1 executable(s) linking Catch2 carry the guard")

# 2. A second test binary links Catch2 and lacks the guard: refused, by name.
fc_edge(bare node-tests Debug "src/apps/node/CMakeFiles/node-tests.dir/b.cpp.o | ${catch2}")
fc_case("uncovered Catch2 executable" "${anchor}${bare}" FALSE "node-tests links Catch2 and its 'Debug' link line carries no OffThreadAssertionGuard")

# 3. Nothing links Catch2 at all: a derivation that found nothing is refused, never green.
fc_case("no Catch2 edge" "${product}" FALSE "links Catch2, so this derivation found nothing to check")

# 4. The anchor is absent while another binary is covered.
fc_edge(other cli-tests Debug "src/apps/cli/CMakeFiles/cli-tests.dir/OffThreadAssertionGuard.cpp.o | ${catch2}")
fc_case("anchor not proven" "${other}" FALSE "FastCacheTest, the one test executable certain to exist")

# 5. Windows spellings: backslashes, `.obj`, `Catch2d.lib`.
set(windows "build target\\FastCacheTest.exe: CXX_EXECUTABLE_LINKER__FastCacheTest_Debug src\\tests\\CMakeFiles\\FastCacheTest.dir\\__\\tests\\OffThreadAssertionGuard.cpp.obj | _deps\\catch2-build\\src\\Catch2d.lib\n")
fc_case("Windows spellings" "${windows}" TRUE "1 of 1 executable(s) linking Catch2 carry the guard")

# 6. `libCatch2Maind.a` alone is not Catch2: a boundary, not a substring.
fc_edge(mainOnly main-only Debug "x.cpp.o | _deps/catch2-build/src/libCatch2Maind.a")
fc_case("Catch2Main is not Catch2" "${anchor}${mainOnly}" TRUE "1 of 1 executable(s) linking Catch2 carry the guard")

# 7. Another configuration's edge is not this configuration's question.
fc_edge(release node-tests Release "b.cpp.o | ${catch2}")
fc_case("other configuration ignored" "${anchor}${release}" TRUE "1 of 1 executable(s) linking Catch2 carry the guard")

# 8. An edge this reader cannot split is refused rather than guessed at.
fc_edge(bracket odd-tests Debug "odd[1].cpp.o ${guard} | ${catch2}")
fc_case("bracket in an edge" "${anchor}${bracket}" FALSE "which this reader cannot split safely")

# 9. No build.ninja: a SKIP naming why, never a pass.
fc_case("no build.ninja" "<none>" TRUE "SKIP: off-thread-assertion-guard")

if(NOT failures STREQUAL "")
    foreach(failure IN LISTS failures)
        message(SEND_ERROR "off-thread-assertion-guard-selftest: ${failure}")
    endforeach()
endif()
message("off-thread-assertion-guard-selftest: ${cases} case(s) ran")
