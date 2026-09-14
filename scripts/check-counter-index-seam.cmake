# SPDX-License-Identifier: Apache-2.0
#
# A `IMetricsSink::Counter` becomes an index in exactly one place, and this is what
# makes that a fact rather than a sentence (#1366).
#
# What an index at or past `Counter::Last` MEANS is stated once, on
# `IMetricsSink::Carries`, and not here. What this enforces is narrower: that every
# table keyed by the enum reaches its rows through `CounterIndex`
# (`Metrics/IMetricsSink.hpp`) -- directly, or through `CounterCells::Find`, which
# is built on it -- rather than through a cast written at the call site. A cast
# like that compiles, reads exactly like the converter, and carries none of its
# answer for a counter this build has no row for.
#
# Half of the rule is the TYPE, and this is the other half. `CounterCells` has no
# integer subscript, so a table of that type cannot be indexed by a cast at all --
# which is what covers a cast whose operand is a template parameter or an alias,
# because no text scan can see what type an identifier has. What only a scan can
# see is a NEW table keyed by the enum that is not a `CounterCells`: a
# `std::array` indexed by `static_cast<std::size_t>(counter)`, which is an ordinary
# cast no compiler remarks on.
#
# Runs as `cmake -P`, for the reason check-net-boundary.cmake states. Its
# registration carries `FAIL_REGULAR_EXPRESSION`, and that property rather than the
# exit code is the verdict (`scripts/check-script-check-signals.cmake`, #565).
#
# **It fails when its own scan matches nothing**, and when an allowed file converts
# a different number of times than its row says: fewer is a stale row vouching for a
# conversion that has moved, more is a second conversion hiding in the one file
# allowed to hold the first.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-counter-index-seam.cmake
#
# Exit codes: 0 = one converter. 1 = a second one has appeared. (But read the
# OUTPUT, not the code -- see above.)

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# What counts as converting a counter.
#
# A `static_cast` or `std::to_underlying` whose parenthesised operand, up to the end
# of the statement, names `counter` or `Counter` as a whole word -- `counter`,
# `row.counter`, `IMetricsSink::Counter::Last`, `NodeCounter::WorkerJobsCompleted`.
# Stated with its limits rather than implied:
#
#   - an operand spelled otherwise (`Row`, `c`, an alias) is NOT seen; `CounterCells`
#     is what covers that, by having no integer subscript;
#   - the reverse direction, an integer made into a `Counter`, is NOT this rule;
#   - `counters` (a plural, a different name) is not a match, and neither is a
#     `Counter` that appears BEFORE the cast on the line;
#   - the operand runs to the next `;`, so a longer expression naming a counter after
#     the cast is refused rather than missed -- wider than exact, which fails closed.
#
# Full-line and trailing `//` comments are stripped first, so prose that spells the
# cast in backticks is not a violation: a check that failed on the reasoning would
# make the reasoning unwritable.
set(FastCachedCounterConversionRegex
    "(static_cast<[^>]*>|to_underlying)[ \t]*\\([^;]*[Cc]ounter([^A-Za-z0-9_]|$)")
set(FastCachedCounterPrefilter "ounter")

# ---------------------------------------------------------------------------
# Where a conversion may be written. One row per file:
#
#   <path under src/>|<exact number of converting lines>|<why>
#
# The count is exact in both directions -- see the header. Adding a row, or raising a
# count, adds a second place a `Counter` becomes a number, which is the decision #1366
# exists to make deliberate. No row may contain a ';'.
set(FastCachedCounterConverters
    "FastCache/Metrics/IMetricsSink.hpp|2|CounterIndex, the one conversion, and CounterCount, the extent every Counter-keyed table is sized by. The header is included by fastcache-cc, which does not link FastCache, so the extent is a cast here rather than EnumeratorCount."
)

# Which files count as source. Tests included: a test that indexes a table by a cast
# is the same hand-rolled conversion, and the next production site is written by
# copying it.
set(FastCachedCounterSourceGlobs
    "*.hpp" "*.h" "*.hh" "*.hxx" "*.inl" "*.ipp"
    "*.cpp" "*.cc" "*.cxx" "*.hpp.in" "*.h.in"
)

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

set(sourceRoot "${FASTCACHED_SOURCE_DIR}/src")
if(NOT IS_DIRECTORY "${sourceRoot}")
    message(FATAL_ERROR "'${sourceRoot}' is not a directory. Is FASTCACHED_SOURCE_DIR the source root?")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

# Turn a list of shell globs into one anchored regex, so the tree is walked ONCE
# (#502). Verbatim from check-psk-signing-seam.cmake; consolidating the copies is #495.
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

set(allowedPaths)
set(allowedCounts)
set(allowedReasons)
foreach(row IN LISTS FastCachedCounterConverters)
    fastcached_row_fields("${row}" rowPath rowCount rowReason)
    if(NOT rowCount MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Malformed converter row (the count must be a positive integer): ${row}")
    endif()
    list(APPEND allowedPaths "${rowPath}")
    list(APPEND allowedCounts "${rowCount}")
    list(APPEND allowedReasons "${rowReason}")
endforeach()
fastcached_globs_to_regex("${FastCachedCounterSourceGlobs}" sourceRegex)

file(GLOB_RECURSE treeAll LIST_DIRECTORIES false "${sourceRoot}/*")
set(sources ${treeAll})
list(FILTER sources INCLUDE REGEX "${sourceRegex}")

# `list(LENGTH)`, never `if(sources STREQUAL "")`: copying an empty glob result leaves
# `sources` UNDEFINED, and `if()` then compares the literal name. Measured in
# check-psk-signing-seam.cmake, where the reasoning is written out in full.
list(LENGTH sources scannedCount)
if(scannedCount EQUAL 0)
    message(FATAL_ERROR
        "This check walked '${sourceRoot}' and found no source file it knows how to read. "
        "That is the check being broken, not the tree being clean -- and it would report "
        "success on every run from here on. Fix the glob table in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

set(seenCounts)
foreach(allowed IN LISTS allowedPaths)
    list(APPEND seenCounts 0)
endforeach()
set(violations)
set(violationCount 0)
set(allowedConversions 0)

foreach(source IN LISTS sources)
    file(RELATIVE_PATH relativeSource "${sourceRoot}" "${source}")

    # Whole-file prefilter: almost no file names a counter, and splitting every file
    # into lines costs a default-set check seconds (#492).
    file(READ "${source}" content)
    string(FIND "${content}" "${FastCachedCounterPrefilter}" prefilterAt)
    if(prefilterAt EQUAL -1)
        continue()
    endif()

    # VERBATIM, because a violation is printed with its `file:line`. Brackets are
    # blanked there, which is safe for this pattern: it matches on none, and a blanked
    # `table[static_cast<...>(counter)]` still reads `table static_cast<...>(counter) `.
    fastcached_split_lines_verbatim("${content}" lines)

    set(lineNumber 0)
    set(matchedHere 0)
    set(matchedLines)
    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")
        string(REGEX REPLACE "//.*$" "" code "${line}")
        if(NOT code MATCHES "${FastCachedCounterConversionRegex}")
            continue()
        endif()
        math(EXPR matchedHere "${matchedHere} + 1")
        string(STRIP "${line}" shown)
        # A shown line goes into two more lists, and each would split it at its `;`. Dropped
        # rather than escaped again: the `file:line` is what a reader acts on.
        string(REPLACE "\\;" "" shown "${shown}")
        string(REPLACE ";" "" shown "${shown}")
        list(APPEND matchedLines "${relativeSource}:${lineNumber}: ${shown}")
    endforeach()

    if(matchedHere EQUAL 0)
        continue()
    endif()

    list(FIND allowedPaths "${relativeSource}" allowedPosition)
    if(NOT allowedPosition EQUAL -1)
        list(REMOVE_AT seenCounts ${allowedPosition})
        list(INSERT seenCounts ${allowedPosition} ${matchedHere})
        math(EXPR allowedConversions "${allowedConversions} + ${matchedHere}")
        list(GET allowedCounts ${allowedPosition} expected)
        if(matchedHere GREATER expected)
            list(JOIN matchedLines "\n    " shownLines)
            list(APPEND violations
                 "  ${relativeSource} is allowed ${expected} conversion(s) and holds ${matchedHere}:\n    ${shownLines}")
            math(EXPR violationCount "${violationCount} + 1")
        endif()
        continue()
    endif()

    list(JOIN matchedLines "\n    " shownLines)
    list(APPEND violations "  ${relativeSource} converts a counter by hand:\n    ${shownLines}")
    math(EXPR violationCount "${violationCount} + 1")
endforeach()

# ---------------------------------------------------------------------------
# The vacuous-pass guards, both directions: nothing matched at all is the converter
# renamed out from under this check; an allowed file matching FEWER lines than its row
# says is a row vouching for a conversion that is no longer there.
if(allowedConversions EQUAL 0)
    message(FATAL_ERROR
        "This check scanned the tree for a Counter converted to a number and found NONE -- "
        "including in the file allowed to hold the converter. That is not a clean tree, it is a "
        "check that has stopped looking at anything, and it would report success on every run "
        "from here on. The converter has most likely been renamed or moved; update "
        "${CMAKE_CURRENT_LIST_FILE}.")
endif()

set(staleRows)
set(index 0)
foreach(allowed IN LISTS allowedPaths)
    list(GET allowedCounts ${index} expected)
    list(GET seenCounts ${index} seen)
    if(seen LESS expected)
        list(APPEND staleRows "${allowed}: the row says ${expected}, the file holds ${seen}")
    endif()
    math(EXPR index "${index} + 1")
endforeach()
list(LENGTH staleRows staleCount)
if(staleCount GREATER 0)
    list(JOIN staleRows "\n  " staleReport)
    message(FATAL_ERROR
        "STALE converter row(s): the file holds fewer conversions than its row allows:\n  "
        "${staleReport}\n\n"
        "A row that allows more than the file does vouches for a conversion that has moved, and "
        "would wave through the next one written there. Lower the count, or move the row with "
        "the converter. The table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

if(violationCount GREATER 0)
    list(JOIN violations "\n" violationReport)
    set(table "")
    set(index 0)
    foreach(allowed IN LISTS allowedPaths)
        list(GET allowedCounts ${index} expected)
        list(GET allowedReasons ${index} reason)
        string(APPEND table "  ${allowed} (${expected})\n      ${reason}\n")
        math(EXPR index "${index} + 1")
    endforeach()
    message(FATAL_ERROR
        "A Counter is converted to a number outside the one converter:\n"
        "${violationReport}\n\n"
        "What an index at or past Counter::Last means is stated on IMetricsSink::Carries, and a "
        "cast written here answers none of it. Reach a table keyed by the enum through "
        "CounterCells::Find, or ask CounterIndex (src/FastCache/Metrics/IMetricsSink.hpp) and "
        "handle its absent answer; a loop that only needs distinct values can number its rows "
        "itself. Only these may convert:\n\n"
        "${table}\n"
        "Not covered, so do not read a pass as covering it: an operand not spelled "
        "counter/Counter (CounterCells' missing subscript covers that one) and an integer made "
        "INTO a Counter. The table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH allowedPaths converterCount)
message("counter index seam: ${scannedCount} source(s) scanned; ${allowedConversions} conversion(s) in "
        "the ${converterCount} file(s) allowed to hold them, none anywhere else")
