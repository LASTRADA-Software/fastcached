# SPDX-License-Identifier: Apache-2.0
#
# The Net boundary: `Net/` and `Async/` must reach nothing but each other and a
# named handful of leaf headers.
#
# `src/FastCache/Net/` is meant to be lifted out of this codebase and upstreamed
# into contour as a standalone library. That is a property of the *include graph*,
# and an include graph drifts silently: nothing fails, nothing warns, and the edge
# is only discovered by whoever tries the lift. Issue #100 counted ten such edges
# that had accumulated exactly that way.
#
# Why a scan rather than a target that compiles the set. Compiling it would mean a
# second full build of `Net/` + `Async/` against a staged include root, in every
# configuration, on every platform -- and a staged root copied at configure time
# goes stale the moment a header changes, which is precisely when the answer
# matters. This reads the same include graph the compiler would, from the sources
# themselves, in milliseconds. Given the project's separate rule that public
# headers are self-contained, a set closed under inclusion is a set that compiles
# standalone, which is the property being claimed.
#
# Runs as `cmake -P`, for the reasons check-repository-hygiene.cmake states at
# length: this compares strings and reports, so a .sh + .ps1 pair would be two
# implementations of one rule differing only in syntax, and cmake is the one tool
# guaranteed present.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-net-boundary.cmake
#
# Exit codes: 0 = the boundary holds. 1 = at least one edge crosses it.

# ---------------------------------------------------------------------------
# What the standalone unit IS. One row per directory whose sources are scanned
# AND which those sources may include from:
#
#   <path under src/FastCache>|<why it travels with Net>
#
# Adding a row here widens what gets upstreamed, so it is meant to be a decision
# somebody makes in review rather than something that happens by include.
#
# No row may contain a ';' -- these are CMake lists, and a semicolon inside a row
# would split it into two.

cmake_minimum_required(VERSION 3.28)

set(FastCachedNetStandaloneDirs
    "Net|The library itself: sockets, listeners, connectors, framing, TLS."
    "Async|The coroutine and event-loop vocabulary ISocket is expressed IN. ISocket::Read and Write return Task<T>, IoAwaitable is the reactor's completion hook, and EpollSocket / IocpSocket / KqueueSocket are the reactors' own I/O side -- there is no Net without it. Moving that vocabulary into Net/ instead would leave Async/ unusable without Net/, or duplicate Task."
)

# ---------------------------------------------------------------------------
# Individual headers the unit may also include. Every one of these is checked
# below to contain no FastCache include of its own, which is what makes the row
# safe: a leaf that grew a dependency would drag the whole of Core/ back across
# the boundary while this check still passed.
#
#   <path under src/FastCache>|<why it travels with Net>
set(FastCachedNetStandaloneLeaves
    "Core/Clock.hpp|IClock and TimePoint -- the injected time seam every deadline in Net/ and every timer in Async/ is expressed in. A Net that carried its own clock interface would fragment the one seam the entire codebase injects."
    "Core/Ranges.hpp|FindOrNull -- a toolchain-portability shim, not a domain type. std::array's iterator is a raw pointer on libstdc++ and libc++ and a class on the MSVC STL, so a call site that stores one has no portable spelling. KqueueReactor is the caller."
    "Core/Profiling.hpp|The FC_ZONE_* Tracy macros, which expand to (void) 0 unless the build defined FC_TRACY_ENABLED. Macros only -- no types and no code. Dropping the three socket.* zones would cost the documented profiling breakdown to remove a header that carries nothing."
)

# ---------------------------------------------------------------------------
# Test sources are deliberately out of scope, and the reason is not laziness:
# what gets lifted is the library, and a test may legitimately reach for a
# fixture anywhere in this tree. Net/HealthProbe_test.cpp drives the daemon's own
# AdminHttpServer, which is the entire point of that case -- gating it would
# force either a second AdminHttpServer fake inside Net/ or the loss of the one
# test that proves the probe works against the real thing.
set(FastCachedNetStandaloneTestSuffix "_test.cpp")

# Which files count as source. Wider than the two extensions this tree happens to
# use today, because a file this does not scan is a hole that reports green: a
# `Net/Foo.h` or a generated `Net/Foo.hpp.in` would be compiled by the build and
# invisible here. `.hpp.in` is not hypothetical -- `Core/Version.hpp.in` is a live
# convention in this tree.
set(FastCachedNetStandaloneSourceGlobs
    "*.hpp" "*.h" "*.hh" "*.hxx" "*.inl" "*.ipp"
    "*.cpp" "*.cc" "*.cxx" "*.hpp.in" "*.h.in"
)

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

set(sourceRoot "${FASTCACHED_SOURCE_DIR}/src/FastCache")
if(NOT IS_DIRECTORY "${sourceRoot}")
    message(FATAL_ERROR "'${sourceRoot}' is not a directory. Is FASTCACHED_SOURCE_DIR the source root?")
endif()

# Split a "path|reason" table into two parallel lists, so the reason can be
# printed beside the rule it explains rather than being a comment nobody reads.
function(fastcached_split_rows rows pathsOut reasonsOut)
    set(paths "")
    set(reasons "")
    foreach(row IN LISTS ${rows})
        string(FIND "${row}" "|" separator)
        if(separator EQUAL -1)
            message(FATAL_ERROR "Malformed row (no '|'): ${row}")
        endif()
        string(SUBSTRING "${row}" 0 ${separator} rowPath)
        math(EXPR reasonStart "${separator} + 1")
        string(SUBSTRING "${row}" ${reasonStart} -1 rowReason)
        list(APPEND paths "${rowPath}")
        list(APPEND reasons "${rowReason}")
    endforeach()
    set(${pathsOut} "${paths}" PARENT_SCOPE)
    set(${reasonsOut} "${reasons}" PARENT_SCOPE)
endfunction()

fastcached_split_rows(FastCachedNetStandaloneDirs standaloneDirs standaloneDirReasons)

# Turn a list of shell globs into one anchored regex, so a directory can be walked
# ONCE and the results filtered in memory.
#
# `file(GLOB_RECURSE var a b c)` traverses the tree once PER PATTERN. On DrvFs one
# traversal of `src/` costs 2.09 s, so a list of N patterns is N x that -- and the call
# site reads as a single glob, which is what made the cost invisible (#502).
#
# This is the fourth copy of this idiom across the hygiene checks; consolidating them
# into a shared module is #495, deliberately not pre-empted here.
# @param globs The shell globs, each like `*.hpp` or `*.hpp.in`.
# @param outVar Set to an anchored alternation regex.
function(fastcached_globs_to_regex globs outVar)
    set(parts "")
    foreach(glob IN LISTS globs)
        string(REPLACE "." "PLACEHOLDERDOT" one "${glob}")
        string(REPLACE "*" ".*" one "${one}")
        string(REPLACE "PLACEHOLDERDOT" "\\." one "${one}")
        list(APPEND parts "${one}")
    endforeach()
    string(REPLACE ";" "|" joined "${parts}")
    set(${outVar} "(${joined})$" PARENT_SCOPE)
endfunction()

fastcached_globs_to_regex("${FastCachedNetStandaloneSourceGlobs}" sourceRegex)
fastcached_split_rows(FastCachedNetStandaloneLeaves standaloneLeaves standaloneLeafReasons)

# ---------------------------------------------------------------------------
# Every in-tree include in one file, as paths relative to src/FastCache -- so
# `#include <FastCache/Core/Clock.hpp>` yields `Core/Clock.hpp`.
#
# It reads EVERY `#include` rather than only the ones already spelled
# `<FastCache/...>`, because the spellings it would otherwise skip are exactly the
# ones that break the boundary while still reporting green:
#
#   * `#include "../Core/Logger.hpp"` -- a relative escape, and the likeliest real
#     drift: it compiles, it links, and it looks local.
#   * `#include "FastCache/Core/Logger.hpp"` -- quoted rather than angled. Not this
#     tree's convention, which is exactly why nothing would notice a first one.
#   * `#include <FastCache/Net/../Core/Logger.hpp>` -- a `..` that matches an
#     allowed prefix and then walks straight out of it.
#
# So a quoted include, and a `..` anywhere in an angled one, are both REPORTED
# rather than read as something this check can resolve. That is failing closed: a
# spelling it cannot classify is refused instead of waved through, the same choice
# the compile-cache opcode table makes about an opcode it does not know. A plain
# `<vector>` or `<openssl/ssl.h>` names nothing in this tree and is not its business.
#
# @param filePath File to read.
# @param includesOut Set to the list of in-tree include targets, relative to
#        src/FastCache. A spelling that cannot be classified is returned verbatim
#        behind a `!` marker, which the caller reports as its own violation.
# Read whole and split by hand, never `file(STRINGS)`.
#
# `file(STRINGS)` returns a CMake LIST, and CMake's list parser treats an
# UNBALANCED `[` or `]` as grouping -- so one stray bracket in a comment on a
# KEPT line merges that element with the ones after it, and every include past
# the bracket disappears from the scan. `file(STRINGS)` takes a PATH, so there is
# no content to neutralise beforehand; the only fix is to read the file and split
# it here.
#
# **Measured on this check, and it is the SILENT class rather than the loud one.**
# Three arms on `src/FastCache/Net/TcpClient.cpp`, with a planted
# `#include <FastCache/Core/Bytes.hpp>` -- a real boundary violation:
#
#   violation alone                      exit 1, violation NAMED, 33 lines
#   violation + a stray `]` before it     exit 0, violation NOT named, 1 line
#   stray `]` alone                       exit 0, clean  (the bracket is not itself a violation)
#
# So a comment bracket makes this check pass over a genuine violation. Note the
# middle arm is why the obvious experiment proves nothing: on a CLEAN tree the
# injection changes the output not at all, because everything the merge swallows
# is something the check had nothing to say about. #518 classified this as
# "summary identical, full output changes" from exactly that experiment; with a
# violation planted, the real behaviour is a green run over a broken boundary.
#
# A `REGEX` filter is not a defence either, only a coincidence of which lines
# survive it: exposure is a property of (reader, file, surviving lines), so the
# filter is applied AFTER the split instead.
#
# Fifth-and-counting copy of this splitting idiom; consolidating them into one
# module is [#495](https://github.com/LASTRADA-Software/fastcached/issues/495)
# and deliberately not done here, because doing it inside a fix for #518 would
# swallow another ticket silently.
function(fastcached_fastcache_includes filePath includesOut)
    file(READ "${filePath}" content)
    string(REPLACE "\\" " " content "${content}")
    string(REPLACE ";" " " content "${content}")
    string(REPLACE "[" " " content "${content}")
    string(REPLACE "]" " " content "${content}")
    string(REGEX REPLACE "\r?\n" ";" lines "${content}")
    list(FILTER lines INCLUDE REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
    set(includes "")
    foreach(line IN LISTS lines)
        set(target "")
        set(quoted FALSE)
        if(line MATCHES "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>")
            set(target "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\"")
            set(target "${CMAKE_MATCH_1}")
            set(quoted TRUE)
        endif()

        if(target STREQUAL "")
            continue()
        endif()

        if(quoted)
            list(APPEND includes "!a quoted include, which this tree does not use: \"${target}\"")
        # A character class, not an escape, because `\.` is not a valid CMake escape
        # sequence -- and the cost of that is a WARNING, not a wrong answer.
        #
        # Measured in situ before changing anything, by instrumenting the rule and
        # asking it what it decided for every include in the tree:
        #
        #     FastCache/Net/../Core/Logger.hpp   offered, rule FIRED
        #     FastCache/Net/Xy/Foo.hpp           offered, rule DECLINED
        #
        # So the rule was behaviourally correct in both directions and the fix below
        # changes no verdict. That is worth stating because the obvious reading is the
        # other one: an invalid escape LOOKS like it must degrade to `.` matching any
        # character, and a two-character path segment would then be reported as walking
        # out. Tested at top level, outside a function, the same pattern DOES match
        # `Net/Xy/` -- so the reading is not unreasonable, it is just not what this code
        # does. Ask the code, not a reconstruction of it.
        #
        # What the invalid escape actually costs is the diagnostic: it warns once per
        # evaluation, and this function runs per include per file -- 629 warnings and
        # 6291 lines of output on a clean tree, while the check passes. The warning
        # naming the defect was buried inside the wall the defect produced, which is
        # why it stood (#517).
        #
        # `[.]` sidesteps the escaping question rather than answering it. That also
        # removes a real fragility: the pattern is correct HERE and stops being correct
        # the moment it is passed through a variable, because the extra expansion drops
        # the backslash. A spelling nobody can get wrong beats an escape that is right
        # only in the context it was written in.
        elseif(target MATCHES "(^|/)[.][.](/|$)")
            list(APPEND includes "!an include that walks out with '..': <${target}>")
        elseif(target MATCHES "^FastCache/")
            string(REGEX REPLACE "^FastCache/" "" relative "${target}")
            list(APPEND includes "${relative}")
        endif()
    endforeach()
    set(${includesOut} "${includes}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Half one: each leaf really is a leaf.
set(leafViolations "")
foreach(leaf IN LISTS standaloneLeaves)
    set(leafPath "${sourceRoot}/${leaf}")
    if(NOT EXISTS "${leafPath}")
        list(APPEND leafViolations "  ${leaf}\n      is named in the leaf table but does not exist")
    else()
        fastcached_fastcache_includes("${leafPath}" leafIncludes)
        if(NOT leafIncludes STREQUAL "")
            # Markers and real includes alike: a leaf that reaches anything, by
            # any spelling, has stopped being a leaf.
            string(REPLACE "!" "" leafIncludeList "${leafIncludes}")
            list(JOIN leafIncludeList ", " leafIncludeText)
            list(APPEND leafViolations
                "  ${leaf}\n      travels with Net/ only because it depends on nothing, and it now includes: ${leafIncludeText}")
        endif()
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Half two: nothing in the unit reaches outside it.
set(scannedCount 0)
set(edgeViolations "")
set(missingUnits "")

foreach(dir IN LISTS standaloneDirs)
    if(NOT IS_DIRECTORY "${sourceRoot}/${dir}")
        list(APPEND missingUnits "  ${dir}/\n      is named in the unit table but is not a directory")
        continue()
    endif()

    # ONE traversal per unit directory, filtered afterwards.
    file(GLOB_RECURSE unitAll LIST_DIRECTORIES false "${sourceRoot}/${dir}/*")
    set(unitSources ${unitAll})
    list(FILTER unitSources INCLUDE REGEX "${sourceRegex}")

    # A directory that contributes nothing is a renamed or mistyped row, and the
    # whole check would then pass by scanning nothing at all -- which is the one
    # failure mode a boundary test must not be allowed to have.
    #
    # `"${unitSources}"` is QUOTED, and that is the whole guard. Unquoted, this
    # condition never fired in the one case it exists for. `file(GLOB_RECURSE)`
    # over an empty directory yields an empty list, `set(unitSources ${unitAll})`
    # with an empty argument UNSETS the variable, and CMake's `if(VAR STREQUAL
    # "")` treats an UNDEFINED left operand as the literal string `unitSources` --
    # which is not equal to `""`, so the check walked on and reported success.
    #
    # Measured, on 3.28 semantics and on the 4.3 here:
    #
    #     set(empty_list)
    #     if(empty_list STREQUAL "")      did NOT fire
    #     if("${empty_list}" STREQUAL "") FIRED
    #     if(NOT empty_list)              FIRED
    #
    # The sibling accumulators in this file are safe only because
    # `set(missingUnits "")` above DEFINES them as the empty string; this one is
    # assigned inside the loop from a glob that can come back empty, so it is the
    # single instance where the idiom breaks. `check-net-boundary-selftest`'s
    # `vacuous` case is what caught it -- the check reported
    # "0 source(s) across 2 directory/directories reach only themselves" and
    # exited 0 over a tree with no sources in it at all.
    if("${unitSources}" STREQUAL "")
        list(APPEND missingUnits "  ${dir}/\n      exists but holds no source this check knows how to read")
    endif()
    foreach(unitSource IN LISTS unitSources)
        string(LENGTH "${FastCachedNetStandaloneTestSuffix}" suffixLength)
        string(LENGTH "${unitSource}" sourceLength)
        if(sourceLength GREATER suffixLength)
            math(EXPR suffixStart "${sourceLength} - ${suffixLength}")
            string(SUBSTRING "${unitSource}" ${suffixStart} -1 tail)
            if(tail STREQUAL "${FastCachedNetStandaloneTestSuffix}")
                continue()
            endif()
        endif()

        math(EXPR scannedCount "${scannedCount} + 1")
        file(RELATIVE_PATH relativeSource "${sourceRoot}" "${unitSource}")
        fastcached_fastcache_includes("${unitSource}" sourceIncludes)

        foreach(included IN LISTS sourceIncludes)
            # A spelling the scanner refused to resolve. Reported as itself
            # rather than tested against the table, which it is not a path for.
            if(included MATCHES "^!")
                string(REGEX REPLACE "^!" "" complaint "${included}")
                list(APPEND edgeViolations "  ${relativeSource}\n      has ${complaint}")
                continue()
            endif()

            set(allowed FALSE)
            foreach(allowedDir IN LISTS standaloneDirs)
                if(included MATCHES "^${allowedDir}/")
                    set(allowed TRUE)
                    break()
                endif()
            endforeach()
            if(NOT allowed)
                list(FIND standaloneLeaves "${included}" leafPosition)
                if(NOT leafPosition EQUAL -1)
                    set(allowed TRUE)
                endif()
            endif()
            if(NOT allowed)
                list(APPEND edgeViolations "  ${relativeSource}\n      includes FastCache/${included}")
            endif()
        endforeach()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
if(NOT leafViolations STREQUAL "" OR NOT edgeViolations STREQUAL "" OR NOT missingUnits STREQUAL "")
    set(report "")
    if(NOT missingUnits STREQUAL "")
        list(JOIN missingUnits "\n" missingReport)
        string(APPEND report "The standalone unit is not where this check expects it:\n${missingReport}\n")
    endif()
    if(NOT edgeViolations STREQUAL "")
        list(JOIN edgeViolations "\n" edgeReport)
        string(APPEND report "Include(s) reach outside the Net standalone unit:\n${edgeReport}\n")
    endif()
    if(NOT leafViolations STREQUAL "")
        list(JOIN leafViolations "\n" leafReport)
        string(APPEND report "Leaf header(s) are no longer dependency-free:\n${leafReport}\n")
    endif()

    set(rulebook "")
    set(index 0)
    foreach(dir IN LISTS standaloneDirs)
        list(GET standaloneDirReasons ${index} reason)
        string(APPEND rulebook "  ${dir}/\n      ${reason}\n")
        math(EXPR index "${index} + 1")
    endforeach()
    set(index 0)
    foreach(leaf IN LISTS standaloneLeaves)
        list(GET standaloneLeafReasons ${index} reason)
        string(APPEND rulebook "  ${leaf}\n      ${reason}\n")
        math(EXPR index "${index} + 1")
    endforeach()

    message(FATAL_ERROR
        "${report}"
        "src/FastCache/Net/ is meant to be lifted out of this tree and upstreamed, "
        "so it and everything that travels with it may include only:\n\n"
        "${rulebook}\n"
        "Close the edge rather than widening the table: push the dependency up to a "
        "caller (Cc::DialEndpoint is the precedent), or move the file to the layer "
        "that owns it. Widen the table only for a header that depends on nothing at "
        "all, and say in its row why it has to travel.\n"
        "The table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH standaloneDirs dirCount)
list(LENGTH standaloneLeaves leafCount)
message("net boundary: ${scannedCount} source(s) across ${dirCount} directory/directories "
        "reach only themselves plus ${leafCount} dependency-free leaf header(s)")
