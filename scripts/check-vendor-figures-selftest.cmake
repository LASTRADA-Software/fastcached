# SPDX-License-Identifier: Apache-2.0
#
# `vendor-figures` must be SEEN to refuse a stale figure and to accept a true one (#1376).
#
# Each case builds a synthetic `vendor/` -- a tree and a VENDOR.md -- and requires one verdict. The
# accepting cases matter as much as the refusing ones: a check that refused every tree would pass
# every refusal below.
#
#   clean              one root, its row true                                accepts
#   wrappedAndGrouped  the row's figures with a thousands comma, the table
#                      wrapped where an editor put it                        accepts
#   rootStale          ac102948's drift: a local change added a test file and
#                      the row's line and file counts did not move          refuses, naming the lines
#   rowReworded        the row no longer in the table's shape                refuses: nothing compared
#   emptyRoot          a root that holds no files                            refuses
#   secondRootCounted  a second root, with a true row in The copies          accepts, counting both
#   secondRootMissing  a second root with no row                            refuses: nothing compared
#   secondRootStale    a second root whose row states the wrong lines       refuses, naming the lines
#   rowNamesNoRoot     a row for a directory the roots file does not name    refuses
#
# Each of the last four was run against a mutant of the check when the table arrived (#178): walking
# the first root only fails secondRootCounted, secondRootMissing and secondRootStale, and dropping the
# refusal of a row naming no root fails rowNamesNoRoot. The cases that read `vendor/endo`'s own
# structural figures went with that copy (#1596).
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

# The tree every case starts from: the roots file names `vendor/first` alone, which holds 3 files
# and 6 lines.
function(fastcached_tree name out)
    set(root "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${root}")
    file(WRITE "${root}/scripts/lib/third-party-roots.txt" "vendor/first\n")
    file(WRITE "${root}/vendor/first/a.c" "a\nb\n")
    file(WRITE "${root}/vendor/first/a.h" "a\n")
    file(WRITE "${root}/vendor/first/sub/b.c" "1\n2\n3\n")
    set(${out} "${root}" PARENT_SCOPE)
endfunction()

# VENDOR.md with one row, for `vendor/first`, stating @p files and @p lines.
function(fastcached_document root files lines)
    file(WRITE "${root}/vendor/VENDOR.md"
        "# vendor\n\n"
        "## The copies\n\n| root | upstream | files | lines |\n|---|---|---|---|\n"
        "| `vendor/first` | first | ${files} | ${lines} |\n")
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
fastcached_document("${root}" 3 6)
fastcached_judge(clean "${root}" OFF "2 figure(s) in vendor/VENDOR.md match the tree")

fastcached_tree(wrappedAndGrouped root)
# Built in memory and written once: an append per line is an open per line, and on DrvFs under a
# gate's load those 1,200 opens alone outran the test's 60 s.
string(REPEAT "x\n" 1200 big)
file(WRITE "${root}/vendor/first/big.h" "${big}")
file(WRITE "${root}/vendor/VENDOR.md"
    "## The copies\n\n| root | upstream | files |\nlines |\n|---|---|---|---|\n"
    "| `vendor/first` | first | 4 |\n1,206 |\n")
fastcached_judge(wrappedAndGrouped "${root}" OFF "match the tree")

fastcached_tree(rootStale root)
# ac102948: the row still counts the copy as it was before the test file arrived.
file(WRITE "${root}/vendor/first/a_test.c" "t\nu\n")
fastcached_document("${root}" 3 6)
fastcached_judge(rootStale "${root}" ON "vendor/first lines = 6, the tree holds 8")

fastcached_tree(rowReworded root)
file(WRITE "${root}/vendor/VENDOR.md" "`vendor/first` is 3 files and 6 lines.\n")
fastcached_judge(rowReworded "${root}" ON "could not find the `vendor/first` row of The copies phrase")

set(root "${FASTCACHED_SCRATCH_DIR}/emptyRoot")
file(REMOVE_RECURSE "${root}")
file(WRITE "${root}/scripts/lib/third-party-roots.txt" "vendor/first\n")
file(MAKE_DIRECTORY "${root}/vendor/first")
fastcached_document("${root}" 0 0)
fastcached_judge(emptyRoot "${root}" ON "the root `vendor/first` holds no files")

# A second root (#178): `vendor/second` holds 2 files, 3 lines, and is named by the roots file.
#
# @param root A tree from fastcached_tree.
function(fastcached_second_root root)
    file(WRITE "${root}/scripts/lib/third-party-roots.txt" "vendor/first\nvendor/second\n")
    file(WRITE "${root}/vendor/second/s.c" "s\nt\n")
    file(WRITE "${root}/vendor/second/LICENCE" "l\n")
endfunction()

fastcached_tree(secondRootCounted root)
fastcached_second_root("${root}")
fastcached_document("${root}" 3 6)
file(APPEND "${root}/vendor/VENDOR.md" "| `vendor/second` | second | 2 | 3 |\n")
fastcached_judge(secondRootCounted "${root}" OFF "4 figure(s) in vendor/VENDOR.md match the tree")

fastcached_tree(secondRootMissing root)
fastcached_second_root("${root}")
fastcached_document("${root}" 3 6)
fastcached_judge(secondRootMissing "${root}" ON "could not find the `vendor/second` row of The copies phrase")

fastcached_tree(secondRootStale root)
fastcached_second_root("${root}")
fastcached_document("${root}" 3 6)
file(APPEND "${root}/vendor/VENDOR.md" "| `vendor/second` | second | 2 | 2 |\n")
fastcached_judge(secondRootStale "${root}" ON "vendor/second lines = 2, the tree holds 3")

fastcached_tree(rowNamesNoRoot root)
fastcached_document("${root}" 3 6)
file(APPEND "${root}/vendor/VENDOR.md" "| `vendor/gone` | gone | 2 | 3 |\n")
fastcached_judge(rowNamesNoRoot "${root}" ON "The copies names `vendor/gone`, which scripts/lib/third-party-roots.txt does not")

if(NOT mismatches STREQUAL "")
    message(FATAL_ERROR "vendor-figures-selftest: ${ran} case(s) ran, and these did not judge as they must:${mismatches}")
endif()
message(STATUS "vendor-figures-selftest: ${ran} case(s) ran, every verdict as it must be")
