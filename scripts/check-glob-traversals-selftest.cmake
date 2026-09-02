# SPDX-License-Identifier: Apache-2.0
#
# `glob-traversals` must be SEEN to refuse, on each thing it claims and on nothing else.
#
# The rule it enforces is a PROXY (see the check's header), so the cases here are the
# difference between a guard that discriminates and one that merely runs. Two of them
# exist because the check got them wrong while it was being written, which is the best
# reason a case can have:
#
#   prose        the check reported its OWN remediation example, printed from inside a
#                `message()`. A call is a statement; matching the text anywhere on the
#                line makes a checker fail on its own documentation.
#   line-number  a stray `]` in a comment merges lines under CMake's list grouping, and
#                the check reported a call at line 217 that was at line 307. A guard
#                whose file:line is wrong sends the reader to the wrong place, which is
#                worse than silence because it looks like help.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-glob-traversals-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-glob-traversals.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}/glob-traversals-selftest")
file(REMOVE_RECURSE "${root}")
set(failures "")

# Build a tree holding one scripts/ directory with one file in it.
function(fastcached_make_tree name body outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/scripts")
    file(WRITE "${tree}/scripts/check-subject.cmake" "${body}")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")
    string(FIND "${combined}" "CMake Error" position)
    if(position EQUAL -1)
        set(${outObjected} FALSE PARENT_SCOPE)
    else()
        set(${outObjected} TRUE PARENT_SCOPE)
    endif()
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# 1. One pattern. Must pass, or the check refuses everything and says nothing.
fastcached_make_tree("one" "file(GLOB_RECURSE out \"\${d}/*\")\n" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "one: a single-pattern call was refused -- the check now refuses everything, which is as useless as refusing nothing")
endif()

# 2. Two patterns. The defect itself, in its plainest form.
fastcached_make_tree("two" "file(GLOB_RECURSE out \"\${d}/*.cpp\" \"\${d}/*.hpp\")\n" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "two: a two-pattern call was not refused -- that is two traversals of the tree, which is the whole defect")
else()
    string(FIND "${output}" "passes 2 patterns" position)
    if(position EQUAL -1)
        list(APPEND failures "two: the refusal did not say how many patterns, so the reader cannot see the cost")
    endif()
endif()

# 3. An unquoted list expansion: ONE argument, as many traversals as the list is long.
#    This is the spelling the original defect used, and an argument count alone passes
#    it -- so this case is the difference between the rule and its appearance.
fastcached_make_tree("expansion" "file(GLOB_RECURSE out LIST_DIRECTORIES false \${rootGlobs})\n" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "expansion: an unquoted \${rootGlobs} was accepted -- one argument and nineteen traversals is exactly how check-sccache-backend-caveat reached 78.7s")
endif()

# 4. Keywords are not patterns. Miscounting them would refuse correct code.
fastcached_make_tree("keywords"
    "file(GLOB_RECURSE out RELATIVE \"\${base}\" LIST_DIRECTORIES false CONFIGURE_DEPENDS \"\${d}/*\")\n" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "keywords: RELATIVE/LIST_DIRECTORIES/CONFIGURE_DEPENDS were counted as patterns, so a correct call was refused")
endif()

# 5. Prose is not a call. The check reported its own remediation example once.
fastcached_make_tree("prose"
    "message(\"    file(GLOB_RECURSE all \\\"a\\\" \\\"b\\\")\")\nfile(GLOB_RECURSE out \"\${d}/*\")\n" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "prose: a GLOB_RECURSE inside a message() string was counted as a call, so the check fails on its own documentation")
endif()

# 6. Nothing to find must be a broken scan, never a clean tree.
fastcached_make_tree("none" "# no globbing here at all\nset(x 1)\n" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "none: a scripts/ directory with no GLOB_RECURSE at all passed as clean; zero findings must be reported as a scan that stopped working")
endif()

# 7. The reported line must be the real one. A stray `]` in a comment merges lines
#    under CMake's list grouping, and a file:line that points somewhere else is worse
#    than no line at all.
set(body "# a comment with a stray ] bracket\n")
foreach(i RANGE 1 20)
    string(APPEND body "# filler ${i}\n")
endforeach()
string(APPEND body "file(GLOB_RECURSE out \"\${d}/*.a\" \"\${d}/*.b\")\n")
fastcached_make_tree("lineno" "${body}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "lineno: the two-pattern call after a stray bracket was not refused at all")
else()
    string(FIND "${output}" "check-subject.cmake:22:" position)
    if(position EQUAL -1)
        list(APPEND failures "lineno: the refusal did not name line 22, so a stray `]` in a comment is still drifting the line numbers this check reports")
    endif()
endif()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`glob-traversals` enforces a PROXY for traversals-per-root, so what it")
    message("refuses and what it lets through both have to be pinned. Each case above")
    message("asserts ONE verdict, so a failure names the direction that broke.")
    message(FATAL_ERROR "glob traversals selftest: ${failureCount} verdict(s) wrong")
endif()

message(STATUS "glob traversals selftest: 7 synthetic tree(s), every verdict as expected")
