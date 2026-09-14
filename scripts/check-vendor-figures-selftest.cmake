# SPDX-License-Identifier: Apache-2.0
#
# `vendor-figures` must be SEEN to refuse a stale figure and to accept a true one (#1376).
#
# Each case builds a synthetic `vendor/` -- a tree, a MANIFEST and a VENDOR.md -- and requires one
# verdict. The accepting cases matter as much as the refusing ones: a check that refused every tree
# would pass every refusal below.
#
#   clean              every figure true                                     accepts
#   wrappedAndGrouped  the same, wrapped mid-sentence, with a thousands comma accepts
#   copyTestUncounted  ac102948's drift: a local change added a test file and
#                      the copy's line and file counts did not move         refuses, naming the lines
#   supportingStale    a supporting .cpp added, the 19 not moved              refuses
#   headerCountStale   the header-only sentence left behind                  refuses
#   upstreamStale      a new tui test counted as upstream's                   refuses
#   phraseReworded     a figure sentence rephrased so nothing matches         refuses: nothing compared
#   emptyTree          no vendored files at all                              refuses
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-vendor-figures-selftest.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

foreach(required IN ITEMS FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-vendor-figures.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(ran 0)
set(mismatches "")

# The tree every case starts from: 2 upstream tui files (3 lines), 1 new tui test (2 lines),
# 2 supporting headers (1 line each) and 1 supporting implementation (4 lines).
# Copy: 6 files, 11 lines. Supporting: 3 files, 6 lines, 2 headers, 1 implementation.
function(fastcached_tree name out)
    set(root "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${root}")
    file(WRITE "${root}/vendor/endo/tui/A.cpp" "a\nb\n")
    file(WRITE "${root}/vendor/endo/tui/A.hpp" "a\n")
    file(WRITE "${root}/vendor/endo/tui/A_test.cpp" "t\nu\n")
    file(WRITE "${root}/vendor/endo/platform/P.hpp" "p\n")
    file(WRITE "${root}/vendor/endo/platform/Q.hpp" "q\n")
    file(WRITE "${root}/vendor/endo/platform/P.cpp" "1\n2\n3\n4\n")
    file(WRITE "${root}/vendor/MANIFEST"
        "# header\n"
        "00ff  vendor/endo/platform/P.cpp\n"
        "00ff  vendor/endo/tui/A_test.cpp  local-change-new\n")
    set(${out} "${root}" PARENT_SCOPE)
endfunction()

function(fastcached_document root copyFiles copyLines supporting supportingLines headers implementations headerOnly upstream)
    file(WRITE "${root}/vendor/VENDOR.md"
        "# vendor\n\n"
        "`src/tui` upstream is ${upstream} files and ~0k lines; with the ${supporting} supporting files below and\n"
        "1 local changes' new test files (\"Local changes\") the copy is **${copyFiles} files, ${copyLines} lines**.\n\n"
        "It is ${supporting} files and ${supportingLines} lines: ${headers} headers, plus **${implementations}\n"
        "implementation files** -- and so on.\n\n"
        "**${headerOnly} of the ${supporting} are header-only, and that is all.**\n")
endfunction()

function(fastcached_judge name root refuses expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${root}" -P "${check}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
    )
    string(APPEND output "${errors}")
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
    # Both words, as ctest's own `FASTCACHED_SCRIPT_CHECK_FAILED` reads a check: a sub-run that
    # merely WARNS must not be scored a clean pass here while ctest would refuse it.
    set(refused OFF)
    if(flat MATCHES "CMake Error|CMake Warning")
        set(refused ON)
    endif()
    string(FIND "${flat}" "${expected}" said)
    set(problem "")
    if(refuses AND NOT refused)
        set(problem "accepted, and must refuse")
    elseif(NOT refuses AND refused)
        set(problem "refused, and must accept")
    elseif(said EQUAL -1)
        set(problem "reached the right side for the wrong reason: no `${expected}`")
    endif()
    math(EXPR count "${ran} + 1")
    set(ran ${count} PARENT_SCOPE)
    if(NOT problem STREQUAL "")
        set(mismatches "${mismatches}\n  ${name}: ${problem}\n${output}" PARENT_SCOPE)
    endif()
endfunction()

fastcached_tree(clean root)
fastcached_document("${root}" 6 11 3 6 2 1 2 2)
fastcached_judge(clean "${root}" OFF "12 figure(s) in vendor/VENDOR.md match the tree")

fastcached_tree(wrappedAndGrouped root)
file(WRITE "${root}/vendor/endo/tui/Big.hpp" "")
foreach(i RANGE 1 1200)
    file(APPEND "${root}/vendor/endo/tui/Big.hpp" "x\n")
endforeach()
# Big.hpp is upstream's: 3 upstream tui files, 1,203 upstream lines -> ~1k.
file(WRITE "${root}/vendor/VENDOR.md"
    "`src/tui` upstream is 3 files and ~1k lines; with the 3\nsupporting files below and\n"
    "1 local changes' new test files (\"Local changes\") the copy is **7 files,\n1,211 lines**.\n"
    "It is 3 files and 6 lines: 2 headers, plus **1 implementation files**.\n"
    "**2 of the 3 are header-only**\n")
fastcached_judge(wrappedAndGrouped "${root}" OFF "match the tree")

fastcached_tree(copyTestUncounted root)
# ac102948: the sentence still counts the copy as it was before the test file arrived.
fastcached_document("${root}" 5 9 3 6 2 1 2 2)
fastcached_judge(copyTestUncounted "${root}" ON "lines in the copy = 9, the tree holds 11")

fastcached_tree(supportingStale root)
file(WRITE "${root}/vendor/endo/platform/R.cpp" "r\n")
fastcached_document("${root}" 7 12 3 6 2 1 2 2)
fastcached_judge(supportingStale "${root}" ON "supporting files = 3, the tree holds 4")

fastcached_tree(headerCountStale root)
file(WRITE "${root}/vendor/VENDOR.md"
    "`src/tui` upstream is 2 files and ~0k lines; with the 3 supporting files below and "
    "1 local changes' new test files (\"Local changes\") the copy is **6 files, 11 lines**.\n"
    "It is 3 files and 6 lines: 2 headers, plus **1 implementation files**.\n"
    "**3 of the 3 are header-only**\n")
fastcached_judge(headerCountStale "${root}" ON "supporting headers = 3, the tree holds 2")

fastcached_tree(upstreamStale root)
fastcached_document("${root}" 6 11 3 6 2 1 2 3)
fastcached_judge(upstreamStale "${root}" ON "upstream tui files = 3, the tree holds 2")

fastcached_tree(phraseReworded root)
file(WRITE "${root}/vendor/VENDOR.md"
    "`src/tui` upstream is 2 files and ~0k lines; with the 3 supporting files below and "
    "1 local changes' new test files (\"Local changes\") the copy is **6 files, 11 lines**.\n"
    "The supporting set is 3 files and 6 lines: 2 headers, plus **1 implementation files**.\n"
    "**2 of the 3 are header-only**\n")
fastcached_judge(phraseReworded "${root}" ON "could not find the supporting files phrase")

set(root "${FASTCACHED_SCRATCH_DIR}/emptyTree")
file(REMOVE_RECURSE "${root}")
file(WRITE "${root}/vendor/MANIFEST" "# header\n")
fastcached_document("${root}" 0 0 0 0 0 0 0 0)
fastcached_judge(emptyTree "${root}" ON "found no files under")

if(NOT mismatches STREQUAL "")
    message(FATAL_ERROR "vendor-figures-selftest: ${ran} case(s) ran, and these did not judge as they must:${mismatches}")
endif()
message(STATUS "vendor-figures-selftest: ${ran} case(s) ran, every verdict as it must be")
