# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated, which has already cost this tree once
# (CMP0057, where `if(... IN_LIST ...)` silently did nothing in a check).
cmake_minimum_required(VERSION 3.28)
#
# Test-name hygiene: fail if a Catch2 case is named something the CTest
# registration cannot pass back to the runner.
#
# `catch_discover_tests` registers each case as
# `add_test(NAME <case> COMMAND <exe> <case>)`, so the name is handed to Catch2
# as a command-line ARGUMENT. Catch2 parses its arguments before it parses test
# specs, so a name beginning with `-` is read as an option:
#
#   * `--help wins over whatever follows it` — Catch2 recognises `--help`,
#     prints its usage and exits 0. CTest reports a PASS for a case that never
#     ran. That is the shape this repository keeps a list about: nothing fails,
#     and the thing an operator was told is covered is not.
#   * `--cache-dir gives the node an on-disk tier` — Catch2 does not recognise
#     it, prints `Unrecognised token` and exits non-zero. CTest reports a
#     failure the case does not have, and it passes when run by hand, which is
#     the worst way to spend an afternoon.
#
# Both were live in this tree. The first had been silently not running since it
# was written.
#
# Runs as `cmake -P`, for the reasons check-repository-hygiene.cmake gives at
# length: this reads files, compares strings and reports, so a .sh + .ps1 pair
# would be two implementations of one rule differing only in syntax.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-test-names.cmake
#
# Exit codes: 0 = every case name survives the round trip. 1 = at least one does
# not.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# ---------------------------------------------------------------------------
# Every Catch2 macro that names a case. All four, not just the one this tree
# happens to use today: a guard that covers the spelling in front of it and not
# the neighbouring one is a guard somebody walks around without meaning to, and a
# `TEMPLATE_TEST_CASE` would reintroduce exactly the failure below.
set(FastCachedCaseMacroPattern "^[ \t]*(TEST_CASE|TEST_CASE_METHOD|TEMPLATE_TEST_CASE|SCENARIO)[ \t]*\\(")

# ---------------------------------------------------------------------------
# Which files hold Catch2 cases. The suffix the whole tree uses; a file this does
# not scan is a hole that reports green.
file(GLOB_RECURSE testSources "${FASTCACHED_SOURCE_DIR}/src/*_test.cpp")

set(violations "")
set(scannedCount 0)
set(caseCount 0)

foreach(source IN LISTS testSources)
    math(EXPR scannedCount "${scannedCount} + 1")
    # Read the bytes and walk the newlines. Never build a CMake list from file
    # content, and never neutralise anything in it.
    #
    # `file(STRINGS)` returns a LIST, and one UNBALANCED `[` or `]` makes CMake's
    # list parser swallow every following element. Measured on this check: one `]`
    # appended to a TEST_CASE line took it from 2992 cases to 2985, and it PASSED
    # both times -- the file count did not move, so nothing in the summary said so.
    #
    # The REGEX argument does not protect it: it filters BEFORE the list is built.
    #
    # Neutralising the brackets -- the fix the other converted readers use -- is
    # WRONG here and was tried first. Catch2 tags are literally `[cache]` and a case
    # name may contain brackets, so replacing them rewrites the very text this check
    # extracts and round-trips: it moved the count to 2990 and altered every tag.
    #
    # Walking newlines preserves the text exactly and costs nothing measurable --
    # 1s across all 192 sources against 1s for `file(STRINGS)`, same 2992 lines.
    file(READ "${source}" sourceRest)
    string(REPLACE "\r\n" "\n" sourceRest "${sourceRest}")
    file(RELATIVE_PATH relative "${FASTCACHED_SOURCE_DIR}" "${source}")
    while(NOT sourceRest STREQUAL "")
        string(FIND "${sourceRest}" "\n" sourceNewline)
        if(sourceNewline EQUAL -1)
            set(line "${sourceRest}")
            set(sourceRest "")
        else()
            string(SUBSTRING "${sourceRest}" 0 ${sourceNewline} line)
            math(EXPR sourceNewline "${sourceNewline} + 1")
            string(SUBSTRING "${sourceRest}" ${sourceNewline} -1 sourceRest)
        endif()
    
        # What the REGEX argument used to do, now that the reader cannot.
        if(NOT line MATCHES "${FastCachedCaseMacroPattern}")
            continue()
        endif()
        # The case name is the first string literal on the line. Taken that way
        # rather than as "the first argument", which is what lets one expression
        # cover TEST_CASE_METHOD too -- its name is the SECOND argument, and a
        # fixture type is never a string literal, so there is nothing ahead of it
        # to match by mistake.
        if(NOT line MATCHES "\"([^\"]*)\"")
            continue()
        endif()
        set(caseName "${CMAKE_MATCH_1}")
        math(EXPR caseCount "${caseCount} + 1")
        if(caseName MATCHES "^-")
            list(APPEND violations "  ${relative}: \"${caseName}\"")
        endif()
    endwhile()
endforeach()

if(NOT violations STREQUAL "")
    list(JOIN violations "\n" report)
    message(FATAL_ERROR
        "Catch2 case name(s) begin with '-':\n${report}\n\n"
        "catch_discover_tests passes the name to the runner as an argument, and "
        "Catch2 reads a leading dash as an option rather than as a test spec. A "
        "recognised one (--help) makes CTest report a pass for a case that never "
        "ran; an unrecognised one makes it report a failure the case does not "
        "have.\n"
        "Rename the case so the flag is not the first thing in it -- "
        "\"Naming --cache-dir gives the node an on-disk tier\" rather than "
        "\"--cache-dir gives the node an on-disk tier\".\n"
        "The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

message("test-name hygiene: ${caseCount} case(s) across ${scannedCount} file(s) survive the CTest round trip")
