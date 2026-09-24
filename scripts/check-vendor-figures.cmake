# SPDX-License-Identifier: Apache-2.0
#
# Refuses when a file or line count `vendor/VENDOR.md` states about a vendored copy is not what the
# tree holds (#1376).
#
# VENDOR.md states each copy's size, and a size maintained by hand beside a hand-maintained file set
# drifts: `ac102948` corrected a line count after a local change added a test file and the sentence
# did not move. So the figures are recomputed here from the tree, and a stated figure that differs is
# refused naming both numbers.
#
# VENDOR.md's "The copies" table carries one row per root of `scripts/lib/third-party-roots.txt` --
# the root in backticks, its upstream, its files, its lines -- and each row is recomputed here from
# that root's files; lines are newline characters, as `wc -l` counts them. The roots are READ, never
# restated (#178 added `vendor/monocypher`; #1596 removed `vendor/endo`, whose structural figures --
# the supporting files beside `tui`, how many were headers -- this check also read while it was
# here): a root with no row is refused, since a copy VENDOR.md does not describe is one nobody checks
# the size of, and so is a row naming a directory the roots file does not.
#
# Every figure phrase must be FOUND. A row this check cannot find is a refusal, never a pass: a
# reworded table would otherwise leave the check comparing nothing and agreeing perfectly. The prose
# is read with its line breaks folded to spaces, because markdown wraps a sentence wherever the
# editor did.
#
# NOT covered, stated so it is not over-read: figures VENDOR.md quotes as MEASUREMENTS at a moment
# (the formatter's "146 of 165" on its first run) describe the world when they were taken and are
# deliberately left pinned rather than recomputed.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<root> -P scripts/check-vendor-figures.cmake

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "vendor-figures: FASTCACHED_SOURCE_DIR must be set")
endif()

set(document "${FASTCACHED_SOURCE_DIR}/vendor/VENDOR.md")
if(NOT EXISTS "${document}")
    message(FATAL_ERROR "vendor-figures: ${document} is missing, so there is nothing to compare")
endif()

function(fastcached_count_lines path out)
    file(READ "${path}" text)
    string(REGEX REPLACE "[^\n]" "" newlines "${text}")
    string(LENGTH "${newlines}" count)
    set(${out} ${count} PARENT_SCOPE)
endfunction()

file(READ "${document}" prose)
string(REGEX REPLACE "[ \t\r\n]+" " " prose "${prose}")

set(problems "")
set(compared 0)

# Compare each figure of the FIRST match of @p pattern against the variable its `name:variable` pair
# names, in capture order. A phrase that is not found is itself a problem.
function(fastcached_figures phrase pattern)
    if(NOT prose MATCHES "${pattern}")
        set(problems "${problems}\n  could not find the ${phrase} phrase (/${pattern}/); a figure this check cannot read is one it cannot compare" PARENT_SCOPE)
        return()
    endif()
    set(index 0)
    set(local "${problems}")
    set(count ${compared})
    foreach(pair IN LISTS ARGN)
        math(EXPR index "${index} + 1")
        string(REPLACE "," "" stated "${CMAKE_MATCH_${index}}")
        string(REPLACE ":" ";" parts "${pair}")
        list(GET parts 0 name)
        list(GET parts 1 variable)
        math(EXPR count "${count} + 1")
        if(NOT stated EQUAL ${${variable}})
            set(local "${local}\n  ${phrase}: VENDOR.md states ${name} = ${CMAKE_MATCH_${index}}, the tree holds ${${variable}}")
        endif()
    endforeach()
    set(problems "${local}" PARENT_SCOPE)
    set(compared ${count} PARENT_SCOPE)
endfunction()

fastcached_third_party_roots("${FASTCACHED_SOURCE_DIR}" thirdPartyRoots)
set(rootsCompared 0)
foreach(root IN LISTS thirdPartyRoots)
    file(GLOB_RECURSE rootFiles LIST_DIRECTORIES false "${FASTCACHED_SOURCE_DIR}/${root}/*")
    set(rootFileCount 0)
    set(rootLineCount 0)
    foreach(file IN LISTS rootFiles)
        fastcached_count_lines("${file}" lines)
        math(EXPR rootFileCount "${rootFileCount} + 1")
        math(EXPR rootLineCount "${rootLineCount} + ${lines}")
    endforeach()
    if(rootFileCount EQUAL 0)
        string(APPEND problems "\n  the root `${root}` holds no files, so its row would be compared against zero")
        continue()
    endif()
    # The root is a path, not a pattern: every regex metacharacter in it is escaped.
    string(REGEX REPLACE "([][.*+?^$(){}|\\\\])" "\\\\\\1" rootPattern "${root}")
    set(copyRootFiles ${rootFileCount})
    set(copyRootLines ${rootLineCount})
    fastcached_figures("`${root}` row of The copies"
        "\\| `${rootPattern}` \\| [^|]* \\| ([0-9,]+) \\| ([0-9,]+) \\|"
        "${root} files:copyRootFiles" "${root} lines:copyRootLines")
    math(EXPR rootsCompared "${rootsCompared} + 1")
endforeach()

# A row naming a directory the roots file does not is a copy that was removed, or never a root: either
# way the table claims a size for something no check will ever count again.
string(REGEX MATCHALL "\\| `[^`|]+` \\| [^|]* \\| [0-9,]+ \\| [0-9,]+ \\|" copyRows "${prose}")
foreach(row IN LISTS copyRows)
    string(REGEX MATCH "^\\| `([^`|]+)`" ignored "${row}")
    if(NOT "${CMAKE_MATCH_1}" IN_LIST thirdPartyRoots)
        string(APPEND problems
            "\n  The copies names `${CMAKE_MATCH_1}`, which scripts/lib/third-party-roots.txt does not; remove the "
            "row, or add the root")
    endif()
endforeach()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR
        "vendor-figures: vendor/VENDOR.md does not describe the vendored tree:${problems}\n"
        "Correct the sentence to what the tree holds. These figures are derived, so the tree is right "
        "unless the file set itself is what went wrong -- in which case fix the tree, not the prose.")
endif()
message(STATUS "vendor-figures: ${compared} figure(s) in vendor/VENDOR.md match the tree "
               "(${rootsCompared} root(s) in The copies)")
