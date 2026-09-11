# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated.
cmake_minimum_required(VERSION 3.28)
#
# Every Catch2 test BINARY this tree registers is a row of the ThreadSanitizer
# gate's `TARGETS` table, or carries an exemption here with a written reason.
#
# ## Why this is a check and not a review item (#1209)
#
# `fastcache-cc-tests` sat outside the sanitized scope for as long as it existed.
# No tag could have reached it: the launcher does not link the FastCache library,
# so it is a separate binary, and the gate's other rows name binaries that cannot
# contain its cases. `scripts/check-tsan-scope.cmake` could not see it either --
# that check asks whether a scoped test CASE carries a tag the gate selects, which
# is a question about cases inside binaries the gate already runs.
#
# The ticket enumerated the test binaries by hand and its table listed four. This
# tree registers SIX with `catch_discover_tests`, five of them in a default
# configuration; `fastcache-cli-tests` and `CowTreeTests` were in neither the
# table nor any row. Neither is threaded, so the ticket's conclusion survived its
# census -- but a hand list is exact about what it knows and silent about what it
# does not, and that silence reads identically to complete coverage (#492). The
# census was written by whoever understood the gap best, and it still missed two.
#
# ## What makes this derivable where the threaded census is not
#
# `scripts/tsan-gate.sh`'s header and the `clang-tsan` job both decline to claim
# the scope table names every threaded FILE, and they are right to: that census
# matches `std::thread` in a source, which is a PROXY -- a test reaching threads
# through a helper spawns none of its own, and one naming the type in a comment
# spawns none at all. A check built on it would refuse correct files and miss
# incorrect ones.
#
# The BINARY question has no such gap. A Catch2 test binary is one that
# `catch_discover_tests()` registers, and that registration is a fact about the
# build rather than an inference about a source. So the set is DERIVED here and
# the answer for each member is a decision somebody wrote down.
#
# ## The exemption is the mechanism, and an empty one is refused
#
# A binary nobody has decided about is exactly what this check exists to surface,
# so there is no way to be silent: a row with no reason is refused, a row naming a
# binary that no longer registers is refused as STALE, and a row that is ALSO a
# `TARGETS` row is refused as a contradiction. An exemption nobody must keep true
# would wave through the next one, which is the argument `test-name-hygiene`
# already makes for its own exemption rows.
#
# ## What this check does NOT claim
#
# That the exempt binaries are unthreaded FOREVER. A reason here is a measurement
# with a date, not an invariant; a binary that grows a thread keeps its exemption
# until somebody notices. What the check guarantees is narrower and is the part
# that actually failed: a NEW test binary cannot arrive with nobody having
# decided.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-tsan-binaries.cmake
#
# Exit: the verdict is the OUTPUT, not the status -- read through
# `FAIL_REGULAR_EXPRESSION`, because a `-P` script that merely WARNS exits 0.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# ---------------------------------------------------------------------------
# The binaries the gate RUNS, read from the one reader that parses that table.
#
# `check-tsan-scope.cmake` already walks `TARGETS` and refuses a table it cannot
# read; including it with `FastCachedTsanScopeDefinitionsOnly` returns before its
# own scan. So this file holds no copy of the row set -- a second parser is a
# second thing to be wrong, and one that had drifted would agree with itself
# perfectly on every run.
# ---------------------------------------------------------------------------
set(FastCachedScopeCheck "${FASTCACHED_SOURCE_DIR}/scripts/check-tsan-scope.cmake")
if(NOT EXISTS "${FastCachedScopeCheck}")
    message(FATAL_ERROR
        "check-tsan-binaries: ${FastCachedScopeCheck} does not exist.\n"
        "The gate's TARGETS table is read through that file rather than parsed "
        "again here. If it moved, point this at it; do not restore a copy of the "
        "row set. The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

set(FastCachedTsanScopeDefinitionsOnly TRUE)
include("${FastCachedScopeCheck}")
unset(FastCachedTsanScopeDefinitionsOnly)

if(NOT FastCachedTsanGateTargets)
    message(FATAL_ERROR
        "check-tsan-binaries: including ${FastCachedScopeCheck} yielded no gate "
        "target names.\n"
        "Every verdict below is a comparison against that set, so an empty one "
        "would report every registered binary as uncovered -- or, with the "
        "comparison the other way round, report a clean tree. The rule lives in "
        "${CMAKE_CURRENT_LIST_FILE}.")
endif()

# ---------------------------------------------------------------------------
# The exemptions. One row per binary: `"target|reason"`.
#
# The reason is a forcing function rather than a dead field: it is what a reader
# meeting a red run has to disagree with, and it is what makes a stale row visible
# as stale. Each below states what was MEASURED and where, so it can be re-taken.
#
# NO SEMICOLON IN A REASON. CMake splits a list element on `;`, so a semicolon
# turns one row into two and the tail arrives as a row carrying no `|` -- which is
# what the first draft of this table did, and the refusal named the tail rather
# than the row. That is the rulebook's own literal-`;` overcount, in the table
# whose whole job is that a row means one thing. The refusal below says so, since
# `carries no |` is a true description of a symptom whose cause is somewhere else.
# ---------------------------------------------------------------------------
set(FastCachedTsanBinaryExemptions
    "compile-cache-testclient-tests|FASTCACHED_BUILD_TESTCLIENT is default OFF, so the clang-tsan configure does not declare this target at all, and no source under src/apps/compile-cache-testclient names std::thread, std::jthread, std::async or pthread (measured 2026-09-11 over *.cpp and *.hpp, not only the test sources)"
    "fastcache-cli-tests|the operator client is single-threaded -- no source under src/apps/fastcache-cli names std::thread, std::jthread, std::async or pthread (measured 2026-09-11 over *.cpp and *.hpp)"
    "CowTreeTests|no source under src/CowTree names std::thread, std::jthread, std::async or pthread (measured 2026-09-11 over *.cpp and *.hpp). The CoW store's concurrency question is a SECOND PROCESS -- an exclusive flock taken at Open, refusing the second opener by name -- which is not a data race and which ThreadSanitizer cannot observe from inside one process"
)

# Set by `check-tsan-binaries-selftest.cmake` alone, so that it can stage its
# trees from the row set above and from the gate table this file just read,
# rather than keeping copies of either. It returns here, before the scan.
#
# Nothing else may use this. It is not a mode this check offers to an operator --
# there is no argument that reaches it and `cmake -P` never sets it -- so the
# scanning path cannot be skipped from a command line.
if(FastCachedTsanBinariesDefinitionsOnly)
    return()
endif()

# ---------------------------------------------------------------------------
# The binaries this tree REGISTERS.
#
# Scanned under `src/`, plus the root `CMakeLists.txt`. An INCLUSION list rather
# than a walk with `_deps` denied: an exclusion list bets on the world's layout
# where an inclusion list states your own, and Catch2's own vendored
# `catch_discover_tests(SelfTest)` is exactly the row a denylist would have to go
# on predicting.
#
# Read with `file(READ)` and walked by offset, never `file(STRINGS)`. That reader
# returns a LIST, and an unbalanced `[` or `]` on a kept line merges elements --
# which here would destroy the line boundaries comment-stripping depends on, so a
# commented-out registration could be counted as a real one. The sibling
# `check-tsan-scope.cmake` records the same hazard measured on this very tree.
#
# A COMMENT IS NOT A CALL SITE. This tree holds the word in prose sixteen times --
# `src/tests/CMakeLists.txt` alone has three -- and one of them, spelled
# `catch_discover_tests().`, carries the parenthesis this matches on. So full-line
# comments are stripped BEFORE matching, and the match additionally requires a `(`
# followed by a target name, which is what excludes the backticked mentions. Both
# filters, because either alone lets one class through.
# ---------------------------------------------------------------------------
set(scanFiles "")
file(GLOB_RECURSE srcCMakeFiles "${FASTCACHED_SOURCE_DIR}/src/CMakeLists.txt")
list(APPEND scanFiles ${srcCMakeFiles})
if(EXISTS "${FASTCACHED_SOURCE_DIR}/CMakeLists.txt")
    list(APPEND scanFiles "${FASTCACHED_SOURCE_DIR}/CMakeLists.txt")
endif()

if(NOT scanFiles)
    message(FATAL_ERROR
        "check-tsan-binaries: found no CMakeLists.txt to scan under "
        "${FASTCACHED_SOURCE_DIR}/src.\n"
        "A scan that matches nothing is this reader having stopped working, not a "
        "tree with no test binaries -- and the two produce the same clean run "
        "otherwise. The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

set(registeredBinaries "")
set(registrationSites "")
foreach(scanFile IN LISTS scanFiles)
    file(READ "${scanFile}" scanContent)
    string(REPLACE "\r\n" "\n" scanContent "${scanContent}")
    file(RELATIVE_PATH scanRelative "${FASTCACHED_SOURCE_DIR}" "${scanFile}")
    set(lineNumber 0)
    while(NOT scanContent STREQUAL "")
        string(FIND "${scanContent}" "\n" scanNewline)
        if(scanNewline EQUAL -1)
            set(line "${scanContent}")
            set(scanContent "")
        else()
            string(SUBSTRING "${scanContent}" 0 ${scanNewline} line)
            math(EXPR scanNewline "${scanNewline} + 1")
            string(SUBSTRING "${scanContent}" ${scanNewline} -1 scanContent)
        endif()
        math(EXPR lineNumber "${lineNumber} + 1")

        # A full-line comment, leading blanks allowed. An end-of-line comment
        # after a real registration is deliberately NOT stripped: the call site is
        # what matters and it is already on the line.
        if(NOT line MATCHES "^[ \t]*#")
            string(REGEX MATCHALL "catch_discover_tests[ \t]*\\([A-Za-z0-9_.+-]+" hits "${line}")
            foreach(hit IN LISTS hits)
                string(REGEX REPLACE "^catch_discover_tests[ \t]*\\(" "" hitTarget "${hit}")
                list(APPEND registeredBinaries "${hitTarget}")
                list(APPEND registrationSites "${hitTarget}@${scanRelative}:${lineNumber}")
            endforeach()
        endif()
    endwhile()
endforeach()

list(REMOVE_DUPLICATES registeredBinaries)

if(NOT registeredBinaries)
    message(FATAL_ERROR
        "check-tsan-binaries: scanned the CMakeLists.txt files under "
        "${FASTCACHED_SOURCE_DIR}/src and found no `catch_discover_tests(` "
        "registration at all.\n"
        "Zero rows is the absence of a verdict, not a verdict. Either every Catch2 "
        "binary stopped being registered -- in which case this check has nothing "
        "to enforce and should be removed deliberately -- or this reader stopped "
        "matching. The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

# ---------------------------------------------------------------------------
# The verdicts.
# ---------------------------------------------------------------------------
set(exemptTargets "")
set(problems "")

foreach(exemptionRow IN LISTS FastCachedTsanBinaryExemptions)
    string(FIND "${exemptionRow}" "|" exemptionBar)
    if(exemptionBar EQUAL -1)
        string(APPEND problems
            "  the exemption row `${exemptionRow}` carries no `|`. A row is "
            "`target|reason` -- and the likeliest cause is a SEMICOLON in a "
            "reason, which CMake splits on, so the row above is the tail of one "
            "that looks perfectly well formed in the source\n")
        continue()
    endif()
    string(SUBSTRING "${exemptionRow}" 0 ${exemptionBar} exemptTarget)
    math(EXPR exemptionReasonStart "${exemptionBar} + 1")
    string(SUBSTRING "${exemptionRow}" ${exemptionReasonStart} -1 exemptReason)
    string(STRIP "${exemptReason}" exemptReason)

    if(exemptTarget STREQUAL "")
        string(APPEND problems
            "  an exemption row names no binary before its `|`\n")
        continue()
    endif()
    if(exemptReason STREQUAL "")
        string(APPEND problems
            "  `${exemptTarget}` is exempt with an EMPTY reason. A reason nobody "
            "wrote is `forgot` spelled in the vocabulary of `decided`\n")
        continue()
    endif()

    list(FIND registeredBinaries "${exemptTarget}" exemptIsRegistered)
    if(exemptIsRegistered EQUAL -1)
        string(APPEND problems
            "  `${exemptTarget}` is exempt but no `catch_discover_tests(` "
            "registration names it -- a STALE exemption. Delete the row\n")
        continue()
    endif()

    list(FIND FastCachedTsanGateTargets "${exemptTarget}" exemptIsGated)
    if(NOT exemptIsGated EQUAL -1)
        string(APPEND problems
            "  `${exemptTarget}` is BOTH a TARGETS row in scripts/tsan-gate.sh and "
            "exempt here. It is sanitized; delete the exemption\n")
        continue()
    endif()

    list(APPEND exemptTargets "${exemptTarget}")
endforeach()

set(uncoveredBinaries "")
foreach(binary IN LISTS registeredBinaries)
    list(FIND FastCachedTsanGateTargets "${binary}" isGated)
    list(FIND exemptTargets "${binary}" isExempt)
    if(isGated EQUAL -1 AND isExempt EQUAL -1)
        set(site "")
        foreach(registrationSite IN LISTS registrationSites)
            if(registrationSite MATCHES "^${binary}@(.*)$")
                set(site "${CMAKE_MATCH_1}")
                break()
            endif()
        endforeach()
        string(APPEND uncoveredBinaries "    ${binary}  (registered at ${site})\n")
    endif()
endforeach()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR
        "The ThreadSanitizer binary exemptions in ${CMAKE_CURRENT_LIST_FILE} do "
        "not describe this tree:\n${problems}\n"
        "An exemption nobody must keep true would wave through the next one.")
endif()

if(NOT uncoveredBinaries STREQUAL "")
    string(REPLACE ";" ", " renderedGateTargets "${FastCachedTsanGateTargets}")
    message(FATAL_ERROR
        "These Catch2 test BINARIES are registered with catch_discover_tests and "
        "are neither run by the ThreadSanitizer gate nor exempt:\n"
        "${uncoveredBinaries}\n"
        "The gate runs: ${renderedGateTargets}.\n\n"
        "Two ways to resolve one, and they are not interchangeable. If the binary "
        "holds anything concurrent, add a row to the `TARGETS` table in "
        "scripts/tsan-gate.sh -- and RUN IT FIRST, because a row added blind turns "
        "the clang-tsan job red for the next person -- and add the target to that "
        "job's build step in .github/workflows/build.yml, or the gate refuses a "
        "binary that was never built. If it holds nothing concurrent, add a row to "
        "`FastCachedTsanBinaryExemptions` in ${CMAKE_CURRENT_LIST_FILE} saying what "
        "you measured and when.\n"
        "What must NOT happen is neither: that is the state this check exists to "
        "end, and it is how `fastcache-cc-tests` went unsanitized for its whole "
        "life (#1209).")
endif()

list(LENGTH registeredBinaries registeredCount)
list(LENGTH FastCachedTsanGateTargets gatedCount)
list(LENGTH exemptTargets exemptCount)
list(LENGTH scanFiles scanCount)
string(REPLACE ";" ", " renderedGateTargets "${FastCachedTsanGateTargets}")
string(REPLACE ";" ", " renderedExempt "${exemptTargets}")
message("tsan binaries: ${registeredCount} Catch2 binary(ies) registered across "
        "${scanCount} CMakeLists.txt file(s); ${gatedCount} sanitized "
        "(${renderedGateTargets}); ${exemptCount} exempt with a written reason "
        "(${renderedExempt})")
