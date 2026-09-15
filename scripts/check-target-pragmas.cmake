# SPDX-License-Identifier: Apache-2.0
#
# No source under `src/` enables an instruction set with a target pragma (#1442).
#
# `#pragma GCC target(...)` and `#pragma clang attribute push (__attribute__((target(...))), ...)` enable
# an instruction set for every function after them in the unit, inline copies included -- the hazard
# `scripts/check-instruction-set-flags.cmake` refuses as a flag, with no flag for a compile database to
# show. It is a fact about the TREE rather than about a build, so it is asked once, here, and not in every
# configuration that check reads.
#
# Per-function attributes (`__attribute__((target(...)))`, `[[gnu::target]]`) are the legitimate spelling
# and are not refused; a pragma mentioned inside a comment is not a use.
#
# ## The prefilter, and why it is not the word `pragma`
#
# `fastcached_scan_code_lines` is quadratic in a file's length, and every header here carries
# `#pragma once`, so a file is walked only when ONE line holds every literal word the pattern needs, in
# order. That cannot miss a use: comment stripping only removes text (or puts a space where a `/*...*/`
# was), so a line that is a use after stripping still holds those words before it. The run reports how
# many files got past the prefilter.
#
# Not covered: a pragma assembled by a macro or split across a line continuation, and one inside a string
# literal -- the reader is a line reader.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<tree> -P scripts/check-target-pragmas.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if("${FASTCACHED_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "target-pragmas: FASTCACHED_SOURCE_DIR must be set")
endif()

set(Pattern "#[ \t]*pragma[ \t]+(GCC[ \t]+target|clang[ \t]+attribute.*target)")
set(Prefilter "pragma[^\n]*(GCC[^\n]*target|clang[^\n]*attribute[^\n]*target)")

file(GLOB_RECURSE sourceFiles LIST_DIRECTORIES false "${FASTCACHED_SOURCE_DIR}/src/*")
set(problems "")
set(scanned 0)
set(walked 0)
foreach(path IN LISTS sourceFiles)
    # A `configure_file` template (`Version.hpp.in`) becomes a header first-party units include.
    if(NOT path MATCHES "\\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|ipp)(\\.in)?$")
        continue()
    endif()
    math(EXPR scanned "${scanned} + 1")
    file(READ "${path}" content)
    if(NOT content MATCHES "${Prefilter}")
        continue()
    endif()
    math(EXPR walked "${walked} + 1")
    fastcached_scan_code_lines("${content}" "${Pattern}" hits)
    foreach(hit IN LISTS hits)
        if(hit MATCHES "^use:([0-9]+)$")
            cmake_path(RELATIVE_PATH path BASE_DIRECTORY "${FASTCACHED_SOURCE_DIR}" OUTPUT_VARIABLE shown)
            list(APPEND problems "${shown}:${CMAKE_MATCH_1}: a target pragma enables an instruction set for every function after it in the unit")
        endif()
    endforeach()
endforeach()

if(scanned EQUAL 0)
    message(FATAL_ERROR "target-pragmas: no C or C++ source under `${FASTCACHED_SOURCE_DIR}/src/`, so nothing has been judged")
endif()
if(NOT problems STREQUAL "")
    list(LENGTH problems problemCount)
    list(JOIN problems "\n  " problemText)
    message(FATAL_ERROR
        "target-pragmas: ${problemCount} target pragma(s) under src/:\n  ${problemText}\n"
        "Ask for the instructions per function -- __attribute__((target(...))) -- and run that function only once the "
        "CPU has been asked (#1420: Core/CpuFeatures, and the instruction-set extension entry in "
        ".agent/rules/build-and-toolchain.md).")
endif()
message(STATUS "target-pragmas: ${scanned} source(s) under src/ carry no target pragma (${walked} got past the prefilter)")
