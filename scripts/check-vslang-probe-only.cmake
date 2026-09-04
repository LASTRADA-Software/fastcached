# SPDX-License-Identifier: Apache-2.0
#
# `VSLANG` may be set on a PROBE spawn and on nothing else.
#
# `VSLANG` selects `cl`'s diagnostic language. The launcher sets it so that two
# things it PARSES come back in a language it can read: the `/showIncludes` notes
# ([#692](https://github.com/LASTRADA-Software/fastcached/issues/692)) and the
# version banner, which since #195 is the compiler's identity
# ([#200](https://github.com/LASTRADA-Software/fastcached/issues/200)).
#
# Both of those spawns are read by nobody but this process. The REAL compile is
# different in kind: its diagnostics are the developer's, and forcing English there
# would trade a silent performance loss for silently anglicizing every warning and
# error they see. That is not a subtle distinction, and it is exactly the one a
# later change would erase without noticing -- "the notes are still unreadable on
# this machine, put the variable on the compile too" is a plausible next step that
# repairs a parse by breaking a person's build output.
#
# ## Why this is a check and not a comment
#
# The rule is written down in `.agent/rules/compile-cache.md` under "A compiler's
# WORDS are localized too", and in the Doxygen on both entry points. **Nothing read
# it.** Every test that existed asserted the variable REACHES the probe -- which a
# change that also set it on the compile spawn passes unchanged. A rule whose only
# reader is prose is the shape [#683](https://github.com/LASTRADA-Software/fastcached/issues/683)
# was filed about: a plausible claim that nothing can make fail. This is its reader.
#
# ## What is counted, and why it is the assignment rather than the word
#
# The scan counts `EnvironmentAssignment` sites -- lines spelling `.name = "VSLANG"`
# -- and not mentions of `VSLANG`. The word appears in prose on both entry points
# and in the notes explaining the residue, and a scan that counted those would fail
# on an edit to a comment while passing a change that moved the assignment. Counting
# the thing that has an EFFECT is what makes the census mean something.
#
# It is a pinned census rather than a file allow-list, deliberately. `main.cpp`
# holds both the preprocess probe and the real compile, so "may this file mention
# VSLANG" cannot express the rule at all -- the answer is yes, once. A second
# assignment appearing in that same file is precisely the defect, and only a count
# catches it.
#
# Test sources are exempt: a test must be able to construct the assignment to
# exercise it, and one that could not would be asserting nothing.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# One row per site allowed to SET the variable: the file, how many assignments it
# may hold, and what asks for it there. The reason is a column because a row with no
# stated purpose is a row nobody can judge when it starts failing.
set(vslangAllowedSites
    "src/apps/fastcache-cc/main.cpp|1|RunCaptureSplitInEnglish, for the preprocess probe that reads /showIncludes (#692)"
    "src/apps/fastcache-cc/ToolchainProbe.cpp|1|CompilerBanner, for the version banner that is the compiler identity (#200)"
)

# Where an assignment may NOT appear. Scanned as a whole so a new production file
# reaching for it is caught rather than silently permitted by an allow-list that
# does not mention it.
set(vslangScanRoots
    "src/apps/fastcache-cc"
    "src/apps/fastcache-compile-node"
    "src/apps/fastcached"
    "src/FastCache"
)

# The assignment's shape. Anchored on the designated initializer, so a mention in
# prose is not counted and a genuine assignment cannot hide behind whitespace.
set(vslangAssignmentPattern "\\.name[ \t]*=[ \t]*\"VSLANG\"")

# The named helpers that wrap an English request, and how many times each NAME may
# appear in its file -- its definition plus its permitted call sites.
#
# Counting assignments alone leaves the cheapest route to the defect wide open, and
# it is the route the header's own "put the variable on the compile too" edit would
# actually take: `RunCaptureSplitInEnglish` already exists, so routing the real
# compile spawn through it needs NO new assignment. The census stays at one, this
# check stays green, and every diagnostic the developer reads is anglicized. Pinning
# the identifier count is what closes that -- a second call is a third occurrence.
#
# Found by review rather than by this check, which is the part worth keeping: a
# guard written against one route does not cover a second route to the same place
# merely because the same variable is at the end of it.
set(vslangEnglishEntryPoints
    "src/apps/fastcache-cc/main.cpp|RunCaptureSplitInEnglish|2|its definition, plus the single preprocess-probe call (#692)"
)

set(missingRoots "")
set(unexpectedSites "")
set(wrongCounts "")
set(totalAssignments 0)

# --- collect every production source under the scan roots --------------------
set(scanFiles "")
foreach(root IN LISTS vslangScanRoots)
    set(resolvedRoot "${FASTCACHED_SOURCE_DIR}/${root}")
    if(NOT IS_DIRECTORY "${resolvedRoot}")
        list(APPEND missingRoots "  ${root}: named in the scan table and not present")
        continue()
    endif()
    # ONE traversal, then filtered -- two patterns walk the tree twice, which is
    # what `check-glob-traversals` refuses (#502) and what caught this scan on its
    # first run.
    file(GLOB_RECURSE rootFiles LIST_DIRECTORIES false "${resolvedRoot}/*")
    list(FILTER rootFiles INCLUDE REGEX "[.](cpp|hpp)$")
    list(APPEND scanFiles ${rootFiles})
endforeach()

# --- count assignments per file ----------------------------------------------
set(sitesWithAssignments "")
foreach(sourceFile IN LISTS scanFiles)
    file(RELATIVE_PATH relativePath "${FASTCACHED_SOURCE_DIR}" "${sourceFile}")

    # Tests may construct the assignment; that is how the rule is exercised.
    if(relativePath MATCHES "_test\\.cpp$" OR relativePath MATCHES "TestUtils\\.hpp$"
       OR relativePath MATCHES "TestSupport\\.hpp$")
        continue()
    endif()

    # Read whole and split by hand, never `file(STRINGS)`: that returns a CMake
    # LIST, and an UNBALANCED `[` or `]` on a KEPT line merges that element with
    # the ones after it, so a matching line past the bracket stops being counted.
    #
    # **This reader is not exposed on today's tree, and that is a coincidence
    # rather than a defence.** The merge can only reduce a count where a single
    # file holds TWO OR MORE matching lines with the bracket on a non-final one,
    # and every file here holds exactly one -- measured. Add a second assignment
    # to either file and it becomes exposed with nothing to say so. #518 records
    # the same trap from the other side: a `REGEX` filter protects only when an
    # unbalanced bracket cannot land on a line the filter keeps, which is a
    # property of the current file contents and not of the script.
    #
    # The sibling `check-psk-signing-seam` is the same reader on a corpus that
    # DOES have a two-match file, and there one `]` took its count from 15 to 14
    # while it still passed. Same shape, different corpus, opposite verdict --
    # which is why exposure is judged per (reader, file, surviving lines).
    file(READ "${sourceFile}" _vslangText)
    string(REPLACE "\\" " " _vslangText "${_vslangText}")
    string(REPLACE ";" " " _vslangText "${_vslangText}")
    string(REPLACE "[" " " _vslangText "${_vslangText}")
    string(REPLACE "]" " " _vslangText "${_vslangText}")
    string(REGEX REPLACE "\r?\n" ";" matchedLines "${_vslangText}")
    list(FILTER matchedLines INCLUDE REGEX "${vslangAssignmentPattern}")
    list(LENGTH matchedLines matchCount)
    if(matchCount EQUAL 0)
        continue()
    endif()

    math(EXPR totalAssignments "${totalAssignments} + ${matchCount}")
    list(APPEND sitesWithAssignments "${relativePath}|${matchCount}")
endforeach()

# --- judge each site against the table ---------------------------------------
set(seenAllowed "")
foreach(site IN LISTS sitesWithAssignments)
    string(REPLACE "|" ";" siteFields "${site}")
    list(GET siteFields 0 sitePath)
    list(GET siteFields 1 siteCount)

    set(allowedCount "")
    set(allowedReason "")
    foreach(row IN LISTS vslangAllowedSites)
        string(REPLACE "|" ";" rowFields "${row}")
        list(GET rowFields 0 rowPath)
        if(rowPath STREQUAL sitePath)
            list(GET rowFields 1 allowedCount)
            list(GET rowFields 2 allowedReason)
            break()
        endif()
    endforeach()

    if(allowedCount STREQUAL "")
        list(APPEND unexpectedSites
             "  ${sitePath}: sets VSLANG (${siteCount} assignment(s)) and is not a probe site")
        continue()
    endif()

    list(APPEND seenAllowed "${sitePath}")
    if(NOT siteCount EQUAL allowedCount)
        list(APPEND wrongCounts
             "  ${sitePath}: ${siteCount} assignment(s), expected ${allowedCount} -- ${allowedReason}")
    endif()
endforeach()

# --- a site that stopped setting it is equally a change ----------------------
set(silentSites "")
foreach(row IN LISTS vslangAllowedSites)
    string(REPLACE "|" ";" rowFields "${row}")
    list(GET rowFields 0 rowPath)
    list(GET rowFields 2 rowReason)
    # `EXISTS` alone. The earlier spelling also required the joined path NOT to be
    # absolute, which it always is -- so this arm could never fire, and a renamed
    # file was reported as "stopped asking for English", sending the reader hunting
    # for a deleted assignment inside a file that is no longer there.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${rowPath}")
        list(APPEND missingRoots "  ${rowPath}: named in the allow table and not present")
        continue()
    endif()
    if(NOT "${rowPath}" IN_LIST seenAllowed)
        list(APPEND silentSites "  ${rowPath}: sets VSLANG nowhere -- ${rowReason}")
    endif()
endforeach()

# --- the English helpers are called exactly where the table says --------------
set(extraEnglishCalls "")
foreach(row IN LISTS vslangEnglishEntryPoints)
    string(REPLACE "|" ";" rowFields "${row}")
    list(GET rowFields 0 helperPath)
    list(GET rowFields 1 helperName)
    list(GET rowFields 2 helperAllowed)
    list(GET rowFields 3 helperReason)

    set(resolvedHelper "${FASTCACHED_SOURCE_DIR}/${helperPath}")
    if(NOT EXISTS "${resolvedHelper}")
        list(APPEND missingRoots "  ${helperPath}: named in the English-helper table and not present")
        continue()
    endif()

    # Read whole and split by hand, never `file(STRINGS)`: that returns a CMake
    # LIST, and an UNBALANCED `[` or `]` on a KEPT line merges that element with
    # the ones after it, so a matching line past the bracket stops being counted.
    #
    # **This reader is not exposed on today's tree, and that is a coincidence
    # rather than a defence.** The merge can only reduce a count where a single
    # file holds TWO OR MORE matching lines with the bracket on a non-final one,
    # and every file here holds exactly one -- measured. Add a second assignment
    # to either file and it becomes exposed with nothing to say so. #518 records
    # the same trap from the other side: a `REGEX` filter protects only when an
    # unbalanced bracket cannot land on a line the filter keeps, which is a
    # property of the current file contents and not of the script.
    #
    # The sibling `check-psk-signing-seam` is the same reader on a corpus that
    # DOES have a two-match file, and there one `]` took its count from 15 to 14
    # while it still passed. Same shape, different corpus, opposite verdict --
    # which is why exposure is judged per (reader, file, surviving lines).
    file(READ "${resolvedHelper}" _vslangText)
    string(REPLACE "\\" " " _vslangText "${_vslangText}")
    string(REPLACE ";" " " _vslangText "${_vslangText}")
    string(REPLACE "[" " " _vslangText "${_vslangText}")
    string(REPLACE "]" " " _vslangText "${_vslangText}")
    string(REGEX REPLACE "\r?\n" ";" helperLines "${_vslangText}")
    list(FILTER helperLines INCLUDE REGEX "${helperName}")
    list(LENGTH helperLines helperCount)
    if(NOT helperCount EQUAL helperAllowed)
        list(APPEND extraEnglishCalls
             "  ${helperPath}: ${helperName} appears ${helperCount} time(s), expected ${helperAllowed} -- ${helperReason}")
    endif()
endforeach()

# --- vacuity: a scan that finds nothing has stopped checking ------------------
set(vacuous "")
if(totalAssignments EQUAL 0)
    list(APPEND vacuous
         "  no VSLANG assignment found anywhere; this scan can no longer fail and means nothing")
endif()

set(vslangReportSections
    "vacuous|This check has stopped checking anything"
    "missingRoots|The scan table names somewhere that is not there"
    "unexpectedSites|VSLANG set outside a probe"
    "wrongCounts|A probe site's assignment count moved"
    "extraEnglishCalls|An English-request helper is reached from somewhere new"
    "silentSites|A probe site stopped asking for English"
)

set(report "")
foreach(row IN LISTS vslangReportSections)
    string(REPLACE "|" ";" rowFields "${row}")
    list(GET rowFields 0 sectionVariable)
    list(GET rowFields 1 sectionHeading)
    if(NOT "${${sectionVariable}}" STREQUAL "")
        list(JOIN ${sectionVariable} "\n" sectionBody)
        string(APPEND report "${sectionHeading}:\n${sectionBody}\n")
    endif()
endforeach()

if(NOT report STREQUAL "")
    message(FATAL_ERROR
        "${report}"
        "\n`VSLANG` may be set on a spawn whose output only this process reads, and on no "
        "other. The two such spawns are the preprocess probe (it parses /showIncludes notes, "
        "#692) and the version-banner probe (the banner is the compiler's identity, #200).\n\n"
        "Setting it on the REAL compile would make `cl` report every warning and error in "
        "English regardless of the developer's Visual Studio language -- repairing a parse by "
        "breaking a person's build output. That is why this is counted rather than described: "
        "the rule was already written in `.agent/rules/compile-cache.md` and on both entry "
        "points, and every test asserted only that the variable REACHES a probe, which a "
        "change that ALSO set it on the compile passes unchanged (#683).\n\n"
        "If a third probe legitimately needs it, add a row -- with its reason -- to "
        "${CMAKE_CURRENT_LIST_FILE}. If a site here no longer needs it, remove its row in the "
        "same change, so the census keeps meaning what it says.")
endif()

list(LENGTH scanFiles scanFileCount)
list(LENGTH vslangAllowedSites allowedSiteCount)
message("VSLANG probe-only: ${scanFileCount} production source(s) scanned, "
        "${totalAssignments} assignment(s) found at ${allowedSiteCount} permitted site(s), "
        "0 elsewhere")
