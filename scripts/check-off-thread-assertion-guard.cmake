# SPDX-License-Identifier: Apache-2.0
#
# Every executable that links Catch2 carries `OffThreadAssertionGuard` (#1211).
#
# ## What this reads, and why not CMakeLists.txt
#
# The top-level `CMakeLists.txt` makes the guard an INTERFACE source of the Catch2 target, so
# it reaches an executable through the link rather than through a list. That is a CLAIM
# about what CMake will do; the artefact is the generated link line, and this reads it the
# way `check-error-popups.cmake` reads its own: from `build.ninja`, never from a list of
# targets. A Catch2 that arrives by a route the attachment does not reach -- a second copy,
# a target that stopped being the one resolved, a link that names the library by path --
# builds, runs and passes, and is wrong only on the run where a helper thread asserts.
#
# An executable LINKS Catch2 when its link edge for this configuration names the Catch2
# library itself (`libCatch2.a`, `libCatch2d.a`, `Catch2.lib`, `Catch2d.lib`, or a shared
# spelling), at a boundary: `libCatch2Main.a` is a different library and is not the test. It
# is COVERED when the same edge names the guard's object.
#
# ## Outcomes
#
# An executable that links Catch2 and is not covered is REFUSED, by name. So is every way
# of not being able to ask: no link edge that links Catch2 at all, an anchor (`FastCacheTest`)
# that is not among the covered, and an edge carrying a `;` or a `[`, which this reader
# cannot split safely. A generator that writes no `build.ninja` is a SKIP, naming it.
#
# Usage: cmake -DBUILD_DIR=<dir> -DCONFIG=<config> -DANCHOR=<target>
#          -P check-off-thread-assertion-guard.cmake
cmake_minimum_required(VERSION 3.28)

foreach(required BUILD_DIR CONFIG ANCHOR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "off-thread-assertion-guard: ${required} must be set")
    endif()
endforeach()

# Report one refusal on the output the registration reads, and count it. A FUNCTION, not a
# macro: a macro substitutes its arguments textually and CMake re-parses them.
# @param ARGV the sentence, in pieces that are joined with nothing between them
set(refusals 0)
function(fc_refuse)
    string(JOIN "" sentence ${ARGV})
    message(SEND_ERROR "off-thread-assertion-guard: ${sentence}")
    math(EXPR count "${refusals} + 1")
    set(refusals ${count} PARENT_SCOPE)
endfunction()

set(ninjaFile "${BUILD_DIR}/build.ninja")
if(NOT EXISTS "${ninjaFile}")
    message("SKIP: off-thread-assertion-guard: ${BUILD_DIR} has no build.ninja, so its link lines are in a "
            "format this check does not read. The presets all generate Ninja; a reader claiming to parse "
            "another generator's output would be worse than none.")
    return()
endif()

file(STRINGS "${ninjaFile}" linkEdges REGEX "_EXECUTABLE_LINKER__")

set(catchEdges 0)
set(covered 0)
set(anchorProven FALSE)
foreach(edge IN LISTS linkEdges)
    # FIND rather than a regex class: CMake's regex reads a backslash inside `[...]` as itself,
    # so `[;\\[]` also matched every Windows path. A `;` needs no test -- `file(STRINGS)` has
    # already split on it, which at worst hands an edge's tail to the next element, and that
    # tail names no rule and is skipped. An unmatched `[` is what merges elements.
    string(FIND "${edge}" "[" bracketAt)
    if(NOT bracketAt EQUAL -1)
        fc_refuse("a link edge carries an opening bracket, which this reader cannot split safely, so nothing it "
                  "says about that executable could be trusted: ${edge}")
        continue()
    endif()
    string(REPLACE "\\" "/" edge "${edge}")
    if(NOT edge MATCHES "_EXECUTABLE_LINKER__([^ ]+)_${CONFIG} ")
        continue()
    endif()
    set(name "${CMAKE_MATCH_1}")
    if(NOT edge MATCHES "(^|[ /])(lib)?Catch2d?\\.(a|lib|so|dylib)( |$)")
        continue()
    endif()
    math(EXPR catchEdges "${catchEdges} + 1")
    if(edge MATCHES "OffThreadAssertionGuard\\.cpp\\.(o|obj)( |$)")
        math(EXPR covered "${covered} + 1")
        message("ok: ${name}")
        if(name STREQUAL ANCHOR)
            set(anchorProven TRUE)
        endif()
    else()
        fc_refuse("${name} links Catch2 and its '${CONFIG}' link line carries no OffThreadAssertionGuard "
                  "object, so an assertion on one of its helper threads damages the heap instead of failing "
                  "the case (#1211). The guard is an INTERFACE source of the Catch2 target in the top-level "
                  "CMakeLists.txt: an executable that reaches Catch2 some other way -- a second copy, or the "
                  "library named by path -- does not inherit it. Link the Catch2 target instead.")
    endif()
endforeach()

if(catchEdges EQUAL 0)
    fc_refuse("no '${CONFIG}' link edge in ${ninjaFile} links Catch2, so this derivation found nothing to "
              "check -- which is not the same as everything being covered. Either the tests are not built "
              "here, or the library is spelled in a way this reader does not recognise.")
elseif(NOT anchorProven)
    fc_refuse("${ANCHOR}, the one test executable certain to exist, is not among the covered, so a derivation "
              "that proves nothing about it proves nothing at all.")
endif()

if(refusals EQUAL 0)
    message("off-thread-assertion-guard: ${covered} of ${catchEdges} executable(s) linking Catch2 carry the guard")
endif()
