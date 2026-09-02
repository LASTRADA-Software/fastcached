# SPDX-License-Identifier: Apache-2.0
#
# `catch-skip-return-code` must be SEEN to fail, and on every direction it claims.
#
# The check exists because a registration that omits `SKIP_RETURN_CODE` scores every
# Catch2 skip as a FAILURE. A guard for that is worth nothing until it has been
# watched refusing something, and worth less than nothing if it refuses everything --
# so this drives the real check against synthetic source trees and asserts each
# verdict separately rather than inverting one run with `WILL_FAIL`.
#
# The trees are built here rather than committed, because a fixture CMakeLists holding
# a deliberately-bare `catch_discover_tests` would be a violation of the very rule
# under test, and excluding it would put a hole in the thing being proved.
#
# Six cases, and the last two are the ones that would otherwise rot quietly:
#
#   correct       a registration carrying the property PASSES -- an always-failing
#                 check is as useless as an always-passing one
#   bare          a registration without it FAILS, and NAMES the file
#   wrong-value   `SKIP_RETURN_CODE 77` FAILS. 77 is the GNU convention the
#                 script-driven tests use; Catch2 exits 4 and nothing else, so a
#                 present-but-wrong value looks correct in a diff, satisfies any
#                 "is the property there" scan, and still scores skips as failures
#   continued     the property on a CONTINUATION line PASSES. Legal CMake, and a
#                 line-at-a-time scan would call it a violation
#   none          a tree with no registrations at all FAILS as a broken scan, never
#                 passes as a clean tree
#   excluded      a registration under a build directory is IGNORED -- the first
#                 version of this check walked into `_deps/catch2-src` and reported
#                 vendored third-party code nobody here can edit
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-catch-skip-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-catch-skip-return-code.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}/catch-skip-selftest")
file(REMOVE_RECURSE "${root}")
set(failures "")

# Build a synthetic tree holding one CMakeLists.
# @param name Which case; also the directory.
# @param relative Where the CMakeLists goes, relative to the tree root.
# @param body What it contains.
function(fastcached_make_tree name relative body outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    get_filename_component(directory "${tree}/${relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${tree}/${relative}" "${body}")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the check against a tree and say whether it objected.
function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured
        ERROR_VARIABLE capturedErrors
        RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")

    # The verdict is read from the OUTPUT, never the exit code: `message(FATAL_ERROR)`
    # exits 0 on CMake 3.28, this project's declared minimum, and 1 on 4.x. Reading
    # the status would make this selftest agree with the check on one host and
    # disagree on the other.
    string(FIND "${combined}" "CMake Error" position)
    if(position EQUAL -1)
        set(${outObjected} FALSE PARENT_SCOPE)
    else()
        set(${outObjected} TRUE PARENT_SCOPE)
    endif()
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# 1. Correct.
fastcached_make_tree("correct" "src/thing/CMakeLists.txt"
    "add_executable(thing-tests a.cpp)\ncatch_discover_tests(thing-tests PROPERTIES SKIP_RETURN_CODE 4)\n" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "correct: the check objected to a registration that carries the property -- it now refuses everything, which is as useless as refusing nothing")
endif()

# 2. Bare -- the defect itself.
fastcached_make_tree("bare" "src/thing/CMakeLists.txt"
    "add_executable(thing-tests a.cpp)\ncatch_discover_tests(thing-tests)\n" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "bare: a registration with no SKIP_RETURN_CODE did not fail the check -- which is the whole defect #499 is about")
else()
    string(FIND "${output}" "src/thing/CMakeLists.txt" position)
    if(position EQUAL -1)
        list(APPEND failures "bare: the check objected but did not name the offending file, so a person reading the failure cannot act on it")
    endif()
endif()

# 3. Present but wrong -- 77 is the script-driven convention, not Catch2's.
fastcached_make_tree("wrong-value" "src/thing/CMakeLists.txt"
    "catch_discover_tests(thing-tests PROPERTIES SKIP_RETURN_CODE 77)\n" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "wrong-value: SKIP_RETURN_CODE 77 passed, but Catch2 exits 4 -- the property is present, reads correctly in a diff, and does nothing")
endif()

# 4. Split across lines. Legal CMake; a line-at-a-time scan calls it a violation.
fastcached_make_tree("continued" "src/thing/CMakeLists.txt"
    "catch_discover_tests(thing-tests\n    PROPERTIES\n        SKIP_RETURN_CODE 4\n)\n" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "continued: a registration with the property on a continuation line was reported as a violation -- correct code failing the check")
endif()

# 5. Nothing to find. Must be a broken scan, never a clean tree.
fastcached_make_tree("none" "src/thing/CMakeLists.txt"
    "add_executable(thing-tests a.cpp)\n# no Catch2 registration here at all\n" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "none: a tree with no catch_discover_tests registration passed as clean; zero findings must be reported as a scan that stopped working")
endif()

# 6. A build tree. Vendored third-party registrations are not this rule's business,
#    and the first version of this check reported one inside `_deps/catch2-src`.
fastcached_make_tree("excluded" "src/thing/CMakeLists.txt"
    "catch_discover_tests(thing-tests PROPERTIES SKIP_RETURN_CODE 4)\n" tree)
file(MAKE_DIRECTORY "${tree}/out/build/x/_deps/catch2-src/tests")
file(WRITE "${tree}/out/build/x/_deps/catch2-src/tests/CMakeLists.txt"
     "catch_discover_tests(SelfTest)\n")
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "excluded: a bare registration inside a build tree was reported -- vendored third-party code is not ours to edit and must not fail this check")
endif()

# 7 and 8. The GIT path, which is the one CI takes and which none of the cases above
#    reach -- a synthetic tree is not a git repository, so all six fall back to the
#    directory walk. That gap is exactly how CI found `.cache/CPM/catch2/<hash>/...`
#    after a local run had gone green: the mode under test was not the mode in use.
#
#    7 plants a vendored registration where CPM actually puts one and leaves it
#    UNTRACKED; it must be ignored. 8 tracks a bare one; it must still be caught, so
#    that "ignore what git does not track" cannot degrade into "ignore everything".
find_program(FASTCACHED_GIT NAMES git)
if(NOT FASTCACHED_GIT)
    list(APPEND failures
         "git-mode: no git executable, so the mode CI actually uses could not be exercised at all -- inconclusive, not a pass")
else()
    foreach(case "untracked" "tracked-bare")
        set(tree "${root}/git-${case}")
        file(REMOVE_RECURSE "${tree}")
        file(MAKE_DIRECTORY "${tree}/src/thing")
        file(WRITE "${tree}/src/thing/CMakeLists.txt"
             "catch_discover_tests(thing-tests PROPERTIES SKIP_RETURN_CODE 4)
")
        file(MAKE_DIRECTORY "${tree}/.cache/CPM/catch2/deadbeef/tests")
        file(WRITE "${tree}/.cache/CPM/catch2/deadbeef/tests/CMakeLists.txt"
             "catch_discover_tests(SelfTest)
")

        execute_process(COMMAND "${FASTCACHED_GIT}" init -q "${tree}" RESULT_VARIABLE ignored)
        execute_process(COMMAND "${FASTCACHED_GIT}" -C "${tree}" add "src/thing/CMakeLists.txt"
                        RESULT_VARIABLE ignored)
        if(case STREQUAL "tracked-bare")
            # Force it in, since a real checkout would have it ignored.
            execute_process(COMMAND "${FASTCACHED_GIT}" -C "${tree}" add -f
                            ".cache/CPM/catch2/deadbeef/tests/CMakeLists.txt"
                            RESULT_VARIABLE ignored)
        endif()

        fastcached_run_check("${tree}" objected output)
        if(case STREQUAL "untracked")
            if(objected)
                list(APPEND failures
                     "git-untracked: a vendored registration git does not track was reported -- CPM resolves dependencies inside the source tree and nobody here can edit them")
            endif()
            string(FIND "${output}" "git ls-files" position)
            if(position EQUAL -1)
                list(APPEND failures
                     "git-untracked: the check did not report scanning via git ls-files, so this case exercised the fallback and proves nothing about the mode CI uses")
            endif()
        elseif(NOT objected)
            list(APPEND failures
                 "git-tracked-bare: a TRACKED bare registration was not reported -- deriving the set from git must not degrade into ignoring everything")
        endif()
    endforeach()
endif()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`catch-skip-return-code` is what stops a sixth test binary from scoring")
    message("every Catch2 skip as a failure. Each case above drives it against a")
    message("synthetic tree and asserts ONE verdict, so a failure here names the")
    message("direction that broke rather than only that something did.")
    message(FATAL_ERROR "catch skip selftest: ${failureCount} verdict(s) wrong")
endif()

message(STATUS "catch skip selftest: 8 synthetic tree(s), every verdict as expected")
