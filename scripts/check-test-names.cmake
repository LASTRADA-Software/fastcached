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
# Duplicated case names, and the ONE pair that is allowed (#729).
#
# `catch_discover_tests` registers `add_test(NAME <case> COMMAND <exe> <case>)`
# per case, so two cases sharing a name produce two ctest entries that each run
# the binary filtered by that name -- and Catch2 matches BOTH cases for either
# entry. Nothing is skipped, which is why this reads as harmless and is not:
#
#   * one defect surfaces as TWO red ctest entries, and neither names the case
#     that actually failed;
#   * `ctest -R "^<name>$"` cannot select one of the pair, so a bisect or a
#     targeted re-run silently runs both;
#   * per-case timings and the `--repeat` flake-hunting workflows are attributed
#     to a name that cannot say which case it was.
#
# **The exemptions are a stated decision, not a silent one**, which is the
# `RefuseWithoutCounter` shape this tree uses elsewhere: a row carries its
# REASON, so an allowed duplicate cannot be spelled the same way as a forgotten
# one. And a row that has STOPPED describing a duplicate is refused rather than
# ignored -- an exemption list that can rot silently is a second source of truth,
# and the one thing worse than no exemption is one nobody has to keep true.
#
# An exempted name may not contain `;`, `[`, `]` or `|`: these rows ARE a CMake
# list, which is the hazard this file's reader comment is about. Checked below
# rather than left as a comment nothing reads.
#
# name|reason
set(FastCachedDuplicateNameExemptions
    "A handler freed earlier in the same batch is not dispatched|EpollReactor_test.cpp is wholly inside `#if defined(__linux__)` and KqueueReactor_test.cpp wholly inside `#if defined(__APPLE__)`, so the two cases never co-exist in one build and never register as two ctest entries. The parallel naming is deliberate: it is what makes the same property greppable across the two platform reactors."
)

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
    # Counted for every line, including the ones skipped below, so a reported
    # site is the line an editor jumps to rather than an index into the matches.
    set(lineNo 0)
    while(NOT sourceRest STREQUAL "")
        math(EXPR lineNo "${lineNo} + 1")
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

        # Keyed by DIGEST, never by the name itself. A case name may contain
        # `[`, `]` or `;` -- the tree's tags are literally `[cache]` -- and every
        # one of those corrupts a CMake list or a variable name. An MD5 is
        # list-safe, variable-name-safe, and needs no escaping of the text it
        # stands for. `seenKeys` is therefore a list of hex strings, which is the
        # one shape this file's reader comment says is safe to build.
        string(MD5 nameKey "${caseName}")
        if(DEFINED caseSites_${nameKey})
            set(caseSites_${nameKey} "${caseSites_${nameKey}}\n      ${relative}:${lineNo}")
            math(EXPR caseSeen_${nameKey} "${caseSeen_${nameKey}} + 1")
        else()
            set(caseName_${nameKey} "${caseName}")
            set(caseSites_${nameKey} "      ${relative}:${lineNo}")
            set(caseSeen_${nameKey} 1)
            list(APPEND seenKeys "${nameKey}")
        endif()
    endwhile()
endforeach()

# ---------------------------------------------------------------------------
# The scan found nothing, which is not the same as finding nothing wrong.
#
# Every check here reports by ACCUMULATING violations, so a scan that matched no
# file and a tree with no defect produce byte-identical output: two empty lists
# agree perfectly. Refused before any verdict is drawn from them.
if(scannedCount EQUAL 0 OR caseCount EQUAL 0)
    message(FATAL_ERROR
        "test-name hygiene scanned ${scannedCount} file(s) and found ${caseCount} case(s), "
        "so it is reporting on nothing. Either the glob "
        "(${FASTCACHED_SOURCE_DIR}/src/*_test.cpp) matches no file, or the macro "
        "pattern no longer matches how this tree declares a case. Both make every "
        "check in this file pass vacuously.")
endif()

# ---------------------------------------------------------------------------
# Duplicates, and the exemption rows checked in BOTH directions.
set(duplicateReport "")
set(usedExemptions "")
foreach(nameKey IN LISTS seenKeys)
    if(caseSeen_${nameKey} LESS 2)
        continue()
    endif()
    set(exemptReason "")
    foreach(row IN LISTS FastCachedDuplicateNameExemptions)
        string(FIND "${row}" "|" bar)
        if(bar EQUAL -1)
            message(FATAL_ERROR
                "exemption row carries no '|' separator, so it states no reason:\n  ${row}")
        endif()
        string(SUBSTRING "${row}" 0 ${bar} exemptName)
        if(exemptName STREQUAL "${caseName_${nameKey}}")
            math(EXPR afterBar "${bar} + 1")
            string(SUBSTRING "${row}" ${afterBar} -1 exemptReason)
            list(APPEND usedExemptions "${exemptName}")
        endif()
    endforeach()
    if(NOT exemptReason STREQUAL "")
        continue()
    endif()
    string(APPEND duplicateReport
           "\n  \"${caseName_${nameKey}}\" (${caseSeen_${nameKey}} cases)\n${caseSites_${nameKey}}")
endforeach()

if(NOT duplicateReport STREQUAL "")
    message(FATAL_ERROR
        "Catch2 case name(s) declared more than once:${duplicateReport}\n\n"
        "catch_discover_tests registers one ctest entry per NAME, and Catch2 matches "
        "every case carrying that name -- so each entry runs both cases. Nothing is "
        "skipped, which is why this looks harmless: what breaks is attribution. One "
        "failure appears as two red entries, neither naming the case that failed; "
        "`ctest -R \"^<name>$\"` cannot select one of the pair; and per-case timings "
        "and --repeat flake hunts are filed under a name that cannot say which case "
        "it was.\n"
        "Rename one of them so each states its own subject -- the tags already "
        "differ, and the name should too.\n"
        "If the two genuinely cannot co-exist in one build (mutually exclusive "
        "platform guards, say), add a row to FastCachedDuplicateNameExemptions in "
        "${CMAKE_CURRENT_LIST_FILE} carrying the REASON. An exemption is a stated "
        "decision; leaving the duplicate is not.")
endif()

# A row that has stopped describing a duplicate. Refused rather than ignored: an
# exemption nobody has to keep true is a licence that outlives its argument, and
# the next real duplicate of that name would then be waved through silently.
set(staleExemptions "")
foreach(row IN LISTS FastCachedDuplicateNameExemptions)
    string(FIND "${row}" "|" bar)
    string(SUBSTRING "${row}" 0 ${bar} exemptName)
    if(exemptName MATCHES "[][;|]")
        message(FATAL_ERROR
            "exemption name contains a character that cannot survive a CMake list "
            "(one of ; [ ] |):\n  \"${exemptName}\"\n"
            "Rename the case instead; this table cannot represent it.")
    endif()
    if(NOT exemptName IN_LIST usedExemptions)
        string(APPEND staleExemptions "\n  \"${exemptName}\"")
    endif()
endforeach()

if(NOT staleExemptions STREQUAL "")
    message(FATAL_ERROR
        "FastCachedDuplicateNameExemptions has row(s) that no longer describe a "
        "duplicated case name:${staleExemptions}\n\n"
        "The duplicate was resolved, or the case was renamed or deleted. Remove the "
        "row -- a standing exemption for a name that is now unique would silently "
        "permit the NEXT duplicate of it.\n"
        "The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH FastCachedDuplicateNameExemptions exemptionCount)

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

message("test-name hygiene: ${caseCount} case(s) across ${scannedCount} file(s) survive the CTest round trip, "
        "all names distinct (${exemptionCount} stated exemption(s))")
