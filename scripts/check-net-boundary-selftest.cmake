# SPDX-License-Identifier: Apache-2.0
#
# `net-boundary` must be SEEN to refuse, on each thing it claims and on nothing
# else.
#
# This check had **no selftest at all**. It is registered at
# `src/tests/CMakeLists.txt` and nothing has ever watched it refuse, which by this
# tree's own standard means it is not a guard -- and it had been carrying a wrong
# rule for as long as the rule had existed. #517 records it: the `..` test was
# written `"(^|/)\.\.(/|$)"`, `\.` is not a valid escape in a CMake quoted
# argument, CMake dropped the backslash, and the pattern that actually evaluated
# was `(^|/)..(/|$)` -- where `.` matches ANY character. So the rule caught any
# two-character path segment and a genuine `..` only by accident. It never fired,
# for a reason that has nothing to do with correctness: no include in this tree
# happens to have a two-character segment.
#
# ## The two cases #517 asks for, and why both directions are needed
#
# `dotdot` proves a genuine `..` IS reported. `twochar` proves
# `<FastCache/Net/Xy/Foo.hpp>` is NOT. Only the second one fails under the old
# regex and only the first is what the rule is for, so a selftest with one of them
# is half a selftest.
#
# ## The `bracket` case, and why the obvious version of it proves nothing
#
# #518 classified this check as PARTIAL -- "summary byte-identical before and
# after injection, full output changes" -- from an injection on a CLEAN tree. That
# experiment understates it, and the reason is worth keeping: this check only
# reports violations, so everything a merged list element swallows is something it
# had nothing to say about. On a clean tree the injection changes the output not at
# all.
#
# Measured with a violation PLANTED, three arms:
#
#     violation alone                     exit 1, violation NAMED, 33 lines
#     violation + a stray `]` before it   exit 0, violation NOT named, 1 line
#     stray `]` alone                     exit 0, clean
#
# So a comment bracket made this check pass over a broken boundary. `bracket`
# below is that middle arm, and `bracketAlone` is the third -- without it, a check
# that refused every bracket would pass `bracket` for the wrong reason.
#
# That is also why this selftest compares the FULL output and never a subject
# count: the count is exactly what could not see this.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-net-boundary-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-net-boundary.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
set(failures "")

# ---------------------------------------------------------------------------
# A tree the check can scan: the two standalone directories it names, and the
# three leaf headers it verifies carry no FastCache include of their own. The
# leaves are written EMPTY rather than omitted -- a missing leaf is its own
# refusal, and a case that tripped that instead of the rule under test would pass
# for the wrong reason.
#
# `netBody` is the whole of the Net source, because where a stray bracket sits
# decides whether it reproduces anything.
function(fastcached_make_tree name netBody outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Net")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Async")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Core")
    file(WRITE "${tree}/src/FastCache/Core/Clock.hpp" "#pragma once\n")
    file(WRITE "${tree}/src/FastCache/Core/Ranges.hpp" "#pragma once\n")
    file(WRITE "${tree}/src/FastCache/Core/Profiling.hpp" "#pragma once\n")
    file(WRITE "${tree}/src/FastCache/Async/Task.hpp" "#pragma once\n")
    file(WRITE "${tree}/src/FastCache/Net/Probe.cpp" "${netBody}")
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

# The includes every case starts from: one in-unit, one leaf, one system. All
# legal, so a case that refuses is refusing what it added.
set(baseIncludes
"#pragma once
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <vector>
")

# ---------------------------------------------------------------------------
# 1. The baseline. Every case below is evidence only if this passes -- a check
#    that refuses everything is exactly as useless as one that refuses nothing
#    and looks like rigour.
fastcached_make_tree("clean" "${baseIncludes}" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "clean: a tree with only legal includes was refused -- the check refuses everything, and no other case here means anything")
endif()

# ---------------------------------------------------------------------------
# 2. A genuine `..` walk IS reported. This is the direction the rule exists for,
#    and under #517's broken regex it was caught only by accident.
fastcached_make_tree("dotdot"
"${baseIncludes}#include <FastCache/Net/../Core/Logger.hpp>
" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "dotdot: an angled include walking out with `..` was accepted -- that is the rule this check carries")
else()
    string(FIND "${output}" ".." position)
    if(position EQUAL -1)
        list(APPEND failures "dotdot: refused without naming `..`, so the reader cannot tell which rule fired")
    endif()
endif()

# ---------------------------------------------------------------------------
# 3. A two-character path SEGMENT is NOT a `..` walk. This is the half that
#    actually failed: `(^|/)..(/|$)` matches `Xy`, so `<FastCache/Net/Xy/Foo.hpp>`
#    was reported as walking out. Nothing in the real tree has a two-character
#    segment, which is why the defect never surfaced.
fastcached_make_tree("twochar"
"${baseIncludes}#include <FastCache/Net/Xy/Foo.hpp>
" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    string(FIND "${output}" "walks out" position)
    if(NOT position EQUAL -1)
        list(APPEND failures "twochar: `<FastCache/Net/Xy/Foo.hpp>` was reported as walking out with `..` -- the `..` test is matching any two characters, which is #517")
    endif()
endif()

# ---------------------------------------------------------------------------
# 4. A cross-boundary include is reported. Net/ and Async/ may reach themselves
#    and the three named leaves; Core/Bytes.hpp is neither.
fastcached_make_tree("crossboundary"
"${baseIncludes}#include <FastCache/Core/Bytes.hpp>
" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "crossboundary: `<FastCache/Core/Bytes.hpp>` from Net/ was accepted -- the boundary is not being enforced at all")
endif()

# ---------------------------------------------------------------------------
# 5. THE case. A stray `]` in a comment on a KEPT line -- an `#include` line, the
#    only kind this reader keeps -- must not hide the violation below it.
#
#    Before the fix this passed with the violation unnamed and one line of output.
#    A bracket on a line the REGEX filter DROPS would prove nothing, which is why
#    it goes on an include.
fastcached_make_tree("bracket"
"#pragma once
#include <FastCache/Async/Task.hpp>   // a stray ] in a comment
#include <FastCache/Core/Bytes.hpp>
#include <vector>
" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "bracket: a stray `]` in a comment on an include line hid a real cross-boundary include -- `file(STRINGS)` merged the list elements after it and the check passed over a broken boundary")
else()
    string(FIND "${output}" "Bytes.hpp" position)
    if(position EQUAL -1)
        list(APPEND failures "bracket: refused, but never named `Bytes.hpp` -- something other than the planted violation fired, so this case is not testing what it claims")
    endif()
endif()

# ---------------------------------------------------------------------------
# 6. And the bracket ALONE is not a violation. Without this, a check that simply
#    refused anything containing a bracket would pass case 5 for the wrong
#    reason -- which is the shape of a green probe that could not have failed.
fastcached_make_tree("bracketAlone"
"#pragma once
#include <FastCache/Async/Task.hpp>   // a stray ] in a comment
#include <vector>
" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "bracketAlone: a stray `]` with no violation behind it was refused -- case 5 would then pass for the wrong reason")
endif()

# ---------------------------------------------------------------------------
# 7. A scan that examined nothing must refuse rather than report success. #510
#    turns on this refusal existing and it is what made a silent blinding visible.
set(tree "${root}/vacuous")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}/src/FastCache/Net")
file(MAKE_DIRECTORY "${tree}/src/FastCache/Async")
file(MAKE_DIRECTORY "${tree}/src/FastCache/Core")
file(WRITE "${tree}/src/FastCache/Core/Clock.hpp" "#pragma once\n")
file(WRITE "${tree}/src/FastCache/Core/Ranges.hpp" "#pragma once\n")
file(WRITE "${tree}/src/FastCache/Core/Profiling.hpp" "#pragma once\n")
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "vacuous: a tree with no sources at all was reported as passing -- two empty lists agree perfectly")
endif()

# ---------------------------------------------------------------------------
list(LENGTH failures failureCount)
if(failureCount GREATER 0)
    string(REPLACE ";" "\n  " printable "${failures}")
    message(FATAL_ERROR
        "check-net-boundary-selftest: ${failureCount} case(s) did not behave as claimed:\n"
        "  ${printable}\n")
endif()

message(STATUS "check-net-boundary-selftest: 7 case(s), each seen to behave as claimed")
