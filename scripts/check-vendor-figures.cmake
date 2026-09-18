# SPDX-License-Identifier: Apache-2.0
#
# Refuses when a file or line count `vendor/VENDOR.md` states about the vendored tree is not what the
# tree holds (#1376).
#
# VENDOR.md states the size of the copy in prose -- the total, the supporting files beside `tui`, how
# many of those are headers, what upstream's own `src/tui` is -- and every one of those figures was
# maintained by hand beside a hand-maintained file set. That is a total stated beside a list without
# being derived from it, and it drifted: `ac102948` corrected the copy's line count after a local
# change added a test file and the sentence did not move. So the figures are recomputed here from the
# tree and `vendor/MANIFEST`, and a stated figure that differs is refused naming both numbers.
#
# What each figure is, so the derivation is the definition rather than a guess at it:
#
#   the copy                  every file under vendor/endo; lines are newline characters, as `wc -l`
#   supporting files          the copy minus endo/tui; headers are `.hpp`, implementation files `.cpp`
#   local changes' new files  MANIFEST lines marked `local-change-new`
#   upstream `src/tui`        endo/tui minus its `local-change-new` files; lines rounded to thousands
#
# Every figure phrase must be FOUND. A phrase this check cannot find is a refusal, never a pass: a
# reworded sentence would otherwise leave the check comparing nothing and agreeing perfectly. The
# prose is read with its line breaks folded to spaces, because markdown wraps a sentence wherever the
# editor did.
#
# ## Every root, not only `vendor/endo`
#
# The figures above are endo's, because the prose they are read from is about endo's structure. What
# every copy has in common is a size, so VENDOR.md's "The copies" table carries one row per root of
# `scripts/lib/third-party-roots.txt` -- the root in backticks, its upstream, its files, its lines -- and each
# row is recomputed here from that root's files, the same way `the copy` is. The roots are READ, never
# restated (#178 added the second, `vendor/monocypher`): a root with no row is refused, since a copy
# VENDOR.md does not describe is one nobody checks the size of, and so is a row naming a directory the
# roots file does not.
#
# NOT covered, stated so it is not over-read: figures VENDOR.md quotes as MEASUREMENTS at a moment
# (the formatter's "146 of 165" on its first run, upstream's `coro`/`endo-platform` size) describe
# the world when they were taken and are deliberately left pinned rather than recomputed.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<root> -P scripts/check-vendor-figures.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "vendor-figures: FASTCACHED_SOURCE_DIR must be set")
endif()

set(vendor "${FASTCACHED_SOURCE_DIR}/vendor")
set(document "${vendor}/VENDOR.md")
set(manifest "${vendor}/MANIFEST")
foreach(required IN ITEMS document manifest)
    if(NOT EXISTS "${${required}}")
        message(FATAL_ERROR "vendor-figures: ${${required}} is missing, so there is nothing to compare")
    endif()
endforeach()

# ---- what the tree holds --------------------------------------------------------------------------

file(GLOB_RECURSE copy LIST_DIRECTORIES false RELATIVE "${vendor}" "${vendor}/endo/*")
list(SORT copy)
if(NOT copy)
    message(FATAL_ERROR "vendor-figures: found no files under ${vendor}/endo, so every figure would be "
                        "compared against zero")
endif()

# Newline characters in @p path, as `wc -l` counts.
function(fastcached_count_lines path out)
    file(READ "${path}" text)
    string(REGEX REPLACE "[^\n]" "" newlines "${text}")
    string(LENGTH "${newlines}" count)
    set(${out} ${count} PARENT_SCOPE)
endfunction()

file(STRINGS "${manifest}" manifestLines REGEX "local-change-new[ \t]*$")
set(newFiles "")
foreach(line IN LISTS manifestLines)
    if(line MATCHES "^[0-9a-f]+  (vendor/[^ ]+)  local-change-new")
        string(REGEX REPLACE "^vendor/" "" relative "${CMAKE_MATCH_1}")
        list(APPEND newFiles "${relative}")
    endif()
endforeach()

set(copyFiles 0)
set(copyLines 0)
set(tuiFiles 0)
set(tuiLines 0)
set(supportingHeaders 0)
set(supportingImplementations 0)
set(tuiNewFiles 0)
set(tuiNewLines 0)
foreach(file IN LISTS copy)
    fastcached_count_lines("${vendor}/${file}" lines)
    math(EXPR copyFiles "${copyFiles} + 1")
    math(EXPR copyLines "${copyLines} + ${lines}")
    if(file MATCHES "^endo/tui/")
        math(EXPR tuiFiles "${tuiFiles} + 1")
        math(EXPR tuiLines "${tuiLines} + ${lines}")
        if(file IN_LIST newFiles)
            math(EXPR tuiNewFiles "${tuiNewFiles} + 1")
            math(EXPR tuiNewLines "${tuiNewLines} + ${lines}")
        endif()
    elseif(file MATCHES "\\.hpp$")
        math(EXPR supportingHeaders "${supportingHeaders} + 1")
    elseif(file MATCHES "\\.cpp$")
        math(EXPR supportingImplementations "${supportingImplementations} + 1")
    endif()
endforeach()
math(EXPR supportingFiles "${copyFiles} - ${tuiFiles}")
math(EXPR supportingLines "${copyLines} - ${tuiLines}")
math(EXPR upstreamTuiFiles "${tuiFiles} - ${tuiNewFiles}")
math(EXPR upstreamTuiThousands "(${tuiLines} - ${tuiNewLines} + 500) / 1000")
list(LENGTH newFiles localNewFiles)

# ---- what VENDOR.md says ----------------------------------------------------------------------------

file(READ "${document}" prose)
string(REGEX REPLACE "[ \t\r\n]+" " " prose "${prose}")

set(problems "")
set(compared 0)

# Match @p pattern in the prose and compare each capture group with the derived value named at its
# position in the remaining arguments.
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

fastcached_figures("copy"
    "`src/tui` upstream is ([0-9,]+) files and ~([0-9]+)k lines; with the ([0-9,]+) supporting files below and ([0-9,]+) local changes' new test files \\(\"Local changes\"\\) the copy is \\*\\*([0-9,]+) files, ([0-9,]+) lines\\*\\*"
    "upstream tui files:upstreamTuiFiles" "upstream tui thousands of lines:upstreamTuiThousands"
    "supporting files:supportingFiles" "local changes' new files:localNewFiles"
    "files in the copy:copyFiles" "lines in the copy:copyLines")

fastcached_figures("supporting files"
    "It is ([0-9,]+) files and ([0-9,]+) lines: ([0-9,]+) headers, plus \\*\\*([0-9,]+) implementation files\\*\\*"
    "supporting files:supportingFiles" "supporting lines:supportingLines"
    "supporting headers:supportingHeaders" "supporting implementation files:supportingImplementations")

fastcached_figures("header-only"
    "\\*\\*([0-9,]+) of the ([0-9,]+) are header-only"
    "supporting headers:supportingHeaders" "supporting files:supportingFiles")

# ---- every root, as "The copies" table states it --------------------------------------------------

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
               "(${copyFiles} files, ${copyLines} lines; ${supportingFiles} supporting; "
               "${rootsCompared} root(s) in The copies)")
