# SPDX-License-Identifier: Apache-2.0
#
# Drives `check-incremental-link.cmake` against staged `build.ninja` files, in both
# directions.
#
# Each file holds link edges in the shape CMake's Ninja generator writes them: the edge head
# naming `<LANG>_<KIND>_LINKER__<target>_<config>`, then its indented bindings. A refusal
# asserts WHICH refusal, by a phrase only that refusal carries: every refusal prints
# `CMake Error`, so asserting that alone would pass with the arms exchanged.
#
# Usage: cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_SCRATCH_DIR=<dir> -P <this>

cmake_minimum_required(VERSION 3.28)

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-incremental-link.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}")
set(failures "")
set(cases 0)

# One link edge, as the Ninja generator writes it.
# @param outVar receives the text
# @param kind EXECUTABLE, SHARED_LIBRARY, MODULE_LIBRARY or STATIC_LIBRARY
# @param target the target name
# @param config the configuration the rule is for
# @param flags the LINK_FLAGS value, or `<none>` for no binding
function(fc_edge outVar kind target config flags)
    set(text "build target\\${target}.out: CXX_${kind}_LINKER__${target}_${config} src\\CMakeFiles\\${target}.dir\\a.cpp.obj || x\n")
    string(APPEND text "  CONFIG = ${config}\n")
    if(NOT flags STREQUAL "<none>")
        string(APPEND text "  LINK_FLAGS = ${flags}\n")
    endif()
    string(APPEND text "  TARGET_FILE = target\\${target}.out\n\n")
    set(${outVar} "${text}" PARENT_SCOPE)
endfunction()

# Stage one build tree.
# @param name the tree's directory name under the scratch root
# @param ninja the build.ninja body, or `<none>` for no file
# @param outVar receives the tree's path
function(fc_stage name ninja outVar)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(MAKE_DIRECTORY "${tree}")
    if(NOT ninja STREQUAL "<none>")
        file(WRITE "${tree}/build.ninja" "${ninja}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the check against a tree and record whether it matched expectations.
# @param label what the case establishes
# @param tree the staged tree
# @param config the configuration to judge
# @param msvcStyle the toolchain answer
# @param want `pass`, `refuse` or `skip`
# @param phrase text the output must carry
function(fc_case label tree config msvcStyle want phrase)
    math(EXPR count "${cases} + 1")
    set(cases ${count} PARENT_SCOPE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${tree}" "-DCONFIG=${config}" "-DMSVC_STYLE=${msvcStyle}"
                -DANCHOR=FastCacheTest -P "${check}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE status)
    set(output "${out}${err}")
    string(REPLACE "\n" " " flat "${output}")
    string(REGEX REPLACE " +" " " flat "${flat}")
    set(got "pass")
    if(flat MATCHES "CMake Error|CMake Warning")
        set(got "refuse")
    elseif(flat MATCHES "SKIP: ")
        set(got "skip")
    endif()
    string(FIND "${flat}" "${phrase}" at)
    if(NOT got STREQUAL want)
        set(failures "${failures}\n  FAIL ${label}: expected ${want}, got ${got}:\n${output}" PARENT_SCOPE)
    elseif(at EQUAL -1)
        set(failures "${failures}\n  FAIL ${label}: ${want} as expected but without '${phrase}':\n${output}" PARENT_SCOPE)
    else()
        message("  ok   ${label}")
    endif()
endfunction()

fc_edge(anchor EXECUTABLE FastCacheTest Debug "/machine:x64 /debug /INCREMENTAL:NO /subsystem:console")
fc_edge(shared SHARED_LIBRARY plugin Debug "/machine:x64 /debug /INCREMENTAL:NO")
# A static library carries flags of its own and no switch; it must neither be judged nor
# lend its binding to the edge above it.
fc_edge(archive STATIC_LIBRARY FastCache Debug "/machine:x64")
# Another configuration's edge is not this build's, whatever it says.
fc_edge(otherConfig EXECUTABLE FastCacheTest Release "/machine:x64 /debug /INCREMENTAL")

# ---- the passing direction --------------------------------------------------------------

fc_stage(clean "${anchor}${archive}${shared}${otherConfig}" tree)
fc_case("every judged edge says /INCREMENTAL:NO; a static library and another configuration are not judged"
    "${tree}" Debug ON pass "2 link edge(s) for 'Debug' read, 0 problem(s)")

fc_edge(lower EXECUTABLE lowercase Debug "/machine:x64 -debug -incremental:no")
fc_stage(case-insensitive "${anchor}${lower}" tree)
fc_case("the switch is matched in either introducer and any case, as link.exe reads it"
    "${tree}" Debug ON pass "2 link edge(s) for 'Debug' read, 0 problem(s)")

fc_edge(relAnchor EXECUTABLE FastCacheTest RelWithDebInfo "/machine:x64 /debug /INCREMENTAL:NO")
fc_stage(relwithdebinfo "${relAnchor}" tree)
fc_case("RelWithDebInfo is judged too" "${tree}" RelWithDebInfo ON pass "1 link edge(s) for 'RelWithDebInfo' read")

# ---- the refusing direction -------------------------------------------------------------

fc_edge(bare EXECUTABLE bare-switch Debug "/machine:x64 /debug /INCREMENTAL /subsystem:console")
fc_stage(bare "${anchor}${bare}" tree)
fc_case("CMake's own default, /INCREMENTAL, is refused by target" "${tree}" Debug ON refuse
    "bare-switch links with /INCREMENTAL in 'Debug'")

fc_edge(both EXECUTABLE both-spellings Debug "/machine:x64 /debug /INCREMENTAL /INCREMENTAL:NO")
fc_stage(both "${anchor}${both}" tree)
fc_case("two spellings are refused rather than left to the linker's last-one-wins" "${tree}" Debug ON refuse
    "both-spellings links with /INCREMENTAL in 'Debug'")

fc_edge(absent EXECUTABLE debug-only Debug "/machine:x64 /debug /subsystem:console")
fc_stage(absent "${anchor}${absent}" tree)
fc_case("/debug with no switch at all links incrementally, so absence is refused" "${tree}" Debug ON refuse
    "debug-only links with no /INCREMENTAL:NO in 'Debug'")

fc_edge(unbound EXECUTABLE no-binding Debug "<none>")
fc_stage(unbound "${anchor}${unbound}${archive}" tree)
fc_case("an edge with no LINK_FLAGS is refused, and the static library below lends it nothing" "${tree}" Debug ON refuse
    "no-binding links with no /INCREMENTAL:NO in 'Debug'")

# The shape `catch_discover_tests` gives a test executable: implicit outputs, one of them an
# absolute path whose drive colon Ninja escapes as `$:`. Found by its rule, not its first colon.
set(driveEdge "build target\\drive.exe src\\drive_tests.cmake | D$:/b/src/drive_tests.cmake: CXX_EXECUTABLE_LINKER__drive_Debug a.obj\n  LINK_FLAGS = /machine:x64 /debug /INCREMENTAL\n\n")
fc_stage(drive-colon "${anchor}${driveEdge}" tree)
fc_case("an edge whose outputs carry an escaped drive colon is still found, and judged" "${tree}" Debug ON refuse
    "drive links with /INCREMENTAL in 'Debug'")

fc_stage(no-anchor "${shared}" tree)
fc_case("the anchor must be found and pass" "${tree}" Debug ON refuse "the anchor 'FastCacheTest' was not found")

fc_edge(badAnchor EXECUTABLE FastCacheTest Debug "/machine:x64 /debug /INCREMENTAL")
fc_stage(bad-anchor "${badAnchor}" tree)
fc_case("a refused anchor is not a proven one" "${tree}" Debug ON refuse "the anchor 'FastCacheTest' was not found")

fc_stage(vacuous "${archive}${otherConfig}" tree)
fc_case("no judged edge is a refusal, never a pass" "${tree}" Debug ON refuse "a pass would be vacuous")

# ---- the skipping direction -------------------------------------------------------------

fc_stage(skip-toolchain "${bare}" tree)
fc_case("a toolchain that is not MSVC-style is skipped, whatever its file says" "${tree}" Debug OFF skip
    "not an MSVC-style toolchain")
fc_case("Release is skipped, whatever its file says" "${tree}" Release ON skip "'Release' is not a debug-info configuration")
fc_stage(no-ninja "<none>" tree)
fc_case("no build.ninja is skipped by name" "${tree}" Debug ON skip "has no build.ninja")

message("incremental-link-selftest: ${cases} case(s) run")
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "incremental-link-selftest failed:${failures}")
endif()
