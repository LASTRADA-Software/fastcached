# SPDX-License-Identifier: Apache-2.0
#
# No MSVC-style link in a debug-info configuration is incremental.
#
# `cmake/IncrementalLink.cmake` rewrites CMake's `/debug /INCREMENTAL` defaults to
# `/INCREMENTAL:NO`, because an incremental relink of a test executable failed with LNK1163
# five times on one host and a plain retry then linked (the evidence, and why the rewrite is
# not gated on the compiler cache, are in that file). That rewrite is a CLAIM; the artefact is
# the link line. A normal variable set in the wrong scope, a target that adds its own
# `/INCREMENTAL`, or a CMake that stops reading the variable would each leave a build that
# configures cleanly and links incrementally again. So this reads `build.ninja`.
#
# ## What is judged
#
# Every EXECUTABLE, SHARED_LIBRARY and MODULE_LIBRARY link edge of this configuration. Its
# `LINK_FLAGS` must spell `/INCREMENTAL:NO` and no other form of the switch, in either
# introducer and any case -- `/debug` with no `/INCREMENTAL` at all links incrementally by
# default, so absence is refused as well. A static library is archived by lib.exe and has no
# such switch; an edge of another configuration is not this build's.
#
# ## Outcomes
#
# A judged edge that breaks the rule is REFUSED, by target. So is every way of not being
# able to ask: no judged edge at all, and an anchor (`FastCacheTest`) whose edge was not
# found and passed. A toolchain that is not MSVC-style, a configuration that carries no debug
# information by default, and a generator that writes no `build.ninja` are each a SKIP naming
# which -- none of them has an `/INCREMENTAL` to judge.
#
# Usage: cmake -DBUILD_DIR=<dir> -DCONFIG=<config> -DMSVC_STYLE=<bool> -DANCHOR=<target>
#          -P check-incremental-link.cmake

cmake_minimum_required(VERSION 3.28)

foreach(required BUILD_DIR CONFIG MSVC_STYLE ANCHOR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "incremental-link: ${required} must be set")
    endif()
endforeach()

if(NOT MSVC_STYLE)
    message("SKIP: incremental-link: this is not an MSVC-style toolchain, so no link here is spelled "
            "with /INCREMENTAL and there is nothing to judge")
    return()
endif()
if(NOT CONFIG MATCHES "^(Debug|RelWithDebInfo)$")
    message("SKIP: incremental-link: '${CONFIG}' is not a debug-info configuration; CMake links it "
            "with /INCREMENTAL:NO already and cmake/IncrementalLink.cmake does not touch it")
    return()
endif()
set(ninjaFile "${BUILD_DIR}/build.ninja")
if(NOT EXISTS "${ninjaFile}")
    message("SKIP: incremental-link: ${BUILD_DIR} has no build.ninja, so its link lines are in a "
            "format this check does not read and it cannot say how anything was linked")
    return()
endif()

set(problems 0)

# Report one refusal on the output the registration reads, and count it. A FUNCTION, not a
# macro: a macro substitutes its arguments textually and CMake re-parses them.
# @param ARGV the sentence, in pieces that are joined with nothing between them
function(fc_refuse)
    list(JOIN ARGV "" text)
    message("CMake Error: incremental-link: ${text}")
    math(EXPR count "${problems} + 1")
    set(problems ${count} PARENT_SCOPE)
endfunction()

# An edge's outputs may carry an absolute path, whose drive colon Ninja escapes as `$:`, so
# the head is found by the `: <rule>` that follows them rather than by the first colon.
#
# Only the two kinds of line this reads survive the filter: a link edge's head and a
# LINK_FLAGS binding. Neither carries a bracket or a semicolon in a build CMake generates, so
# splitting them into a list is safe. A binding belongs to the edge above it, which is why
# EVERY linker edge is kept -- a static library's `LINK_FLAGS` must land on the static
# library, not on the executable before it.
file(STRINGS "${ninjaFile}" lines REGEX "^build .*: [A-Za-z0-9_]+_LINKER__|^  LINK_FLAGS = ")

set(judged 0)
set(anchorProven FALSE)
set(edgeTarget "")
set(edgeFlags "")
set(edgeJudged FALSE)

# Judge the edge collected so far, if it is one this check judges.
function(fc_judge_edge)
    if(NOT edgeJudged)
        return()
    endif()
    math(EXPR count "${judged} + 1")
    set(judged ${count} PARENT_SCOPE)
    string(TOUPPER " ${edgeFlags} " upper)
    string(REGEX MATCHALL " [-/]INCREMENTAL[^ ]*" spellings "${upper}")
    set(bad "")
    set(sawNo FALSE)
    foreach(spelling IN LISTS spellings)
        string(STRIP "${spelling}" spelling)
        if(spelling MATCHES "^[-/]INCREMENTAL:NO$")
            set(sawNo TRUE)
        else()
            list(APPEND bad "${spelling}")
        endif()
    endforeach()
    if(bad)
        list(JOIN bad " " shown)
        fc_refuse("${edgeTarget} links with ${shown} in '${CONFIG}' (LINK_FLAGS = ${edgeFlags}). "
                  "cmake/IncrementalLink.cmake removes every spelling of the switch from CMake's "
                  "linker-flag defaults, so either that rewrite never reached this target -- the "
                  "module is not included, or is included after the directory that defines it -- or "
                  "something put the switch back: a target's link options, or a directory that set "
                  "the variable again. Fix whichever it is; do not add /INCREMENTAL:NO beside it, since "
                  "which of two spellings wins is the linker's choice")
    elseif(NOT sawNo)
        fc_refuse("${edgeTarget} links with no /INCREMENTAL:NO in '${CONFIG}' (LINK_FLAGS = ${edgeFlags}), "
                  "and /debug alone links incrementally. The rewrite in cmake/IncrementalLink.cmake did "
                  "not reach it: it must be included before the directory that defines this target")
    elseif(edgeTarget STREQUAL ANCHOR)
        set(anchorProven TRUE PARENT_SCOPE)
    endif()
    set(problems ${problems} PARENT_SCOPE)
endfunction()

foreach(line IN LISTS lines)
    if(line MATCHES "^build .*: [A-Za-z0-9_]+_(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY|STATIC_LIBRARY)_LINKER__([^ ]+)_([A-Za-z0-9]+)( |$)")
        fc_judge_edge()
        set(kind "${CMAKE_MATCH_1}")
        set(edgeTarget "${CMAKE_MATCH_2}")
        set(edgeFlags "")
        set(edgeJudged FALSE)
        if(NOT kind STREQUAL "STATIC_LIBRARY" AND CMAKE_MATCH_3 STREQUAL CONFIG)
            set(edgeJudged TRUE)
        endif()
    elseif(line MATCHES "^  LINK_FLAGS = (.*)$")
        set(edgeFlags "${CMAKE_MATCH_1}")
    endif()
endforeach()
fc_judge_edge()

if(judged EQUAL 0)
    fc_refuse("no executable, shared or module link edge for '${CONFIG}' was found in ${ninjaFile}, "
              "so nothing was shown to link non-incrementally and a pass would be vacuous")
elseif(NOT anchorProven)
    fc_refuse("the anchor '${ANCHOR}' was not found linking with /INCREMENTAL:NO, so the reading "
              "cannot show it reaches even the one executable certain to be here")
endif()

message("incremental-link: ${judged} link edge(s) for '${CONFIG}' read, ${problems} problem(s)")
if(problems GREATER 0)
    message(FATAL_ERROR "incremental-link: ${problems} problem(s)")
endif()
