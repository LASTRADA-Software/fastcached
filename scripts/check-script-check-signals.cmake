# SPDX-License-Identifier: Apache-2.0
#
# Every `cmake -P` check registered in src/tests/CMakeLists.txt must carry a
# failure signal, because it cannot report one with an exit code.
#
# `message(FATAL_ERROR)` in script mode prints `CMake Error ...` and exits **0**
# on CMake 3.28 -- this project's declared minimum. Measured: 3.28.3 exits 0,
# 4.3.1 exits 1. So a check registered as a bare `cmake -P` is a check ctest
# marks PASSED however loudly it objected, on the platform CI builds on. Thirteen
# were registered that way, several of them the only thing standing behind a rule
# that has already been a bug.
#
# The fix is FAIL_REGULAR_EXPRESSION, and the fix's own weakness is that it is a
# property somebody has to remember. `script-check-canary` covers the case where
# the mechanism stops working; this covers the case where a new registration
# never opts into it -- which is the far likelier of the two, and the one that
# reads as a working check.
#
# The set of checks is READ from src/tests/CMakeLists.txt, never restated here.
# A second copy of the list is not a cross-check; it is a second thing to be
# wrong, and it would go stale in the direction that reports green.
#
# Runs as `cmake -P` for the reason check-test-names.cmake gives: it reads a
# file, compares strings and reports, so a .sh + .ps1 pair would be two
# implementations of one rule differing only in syntax. It is therefore subject
# to its own rule, and is registered with the property like everything else.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-script-check-signals.cmake
#
# Exit codes: 0 always -- see above. The verdict is the presence of `CMake Error`
# in the output.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(testsFile "${FASTCACHED_SOURCE_DIR}/src/tests/CMakeLists.txt")
if(NOT EXISTS "${testsFile}")
    message(FATAL_ERROR "the test registration file is missing: ${testsFile}")
endif()

# Read and split by hand rather than with `file(STRINGS)`, which returns a LIST:
# a line containing a semicolon becomes two elements, the line numbers drift and
# every verdict after it is drawn from the wrong place. Escaping first is what
# keeps one line one element.
#
# A semicolon is only HALF the hazard. CMake's list grouping also treats `[` and
# `]` as structure, so one unbalanced bracket -- in a COMMENT, where nobody is
# thinking about CMake syntax -- merges every following line into one element.
#
# This check is the sharp case for that, because going blind does NOT make it
# fail. It counts registrations and asserts each one can report failure, so a
# splitter that stops seeing most of the file leaves a smaller set that still
# passes unanimously. Measured while fixing #502: a bracket in a comment took it
# from 21 registrations to 3, and it reported success both times. An emptiness
# guard cannot catch that -- 3 is not 0 -- so the fix has to be here, in the
# splitter, rather than in a check on the count.
#
# Two precisions #495 measured, which this comment originally lacked. Only an
# UNBALANCED bracket groups; `[[nodiscard]]` is completely harmless, so treating
# every `[` and `]` as structure is broader than the truth. And replacing them is
# safe here by WHAT THIS CHECK MATCHES -- `add_test(NAME ...)` and property names,
# none containing a bracket -- rather than by construction. A pattern that did
# contain one would break silently. `check-worker-refusals-counted` is the worked
# example: the same fix took it from three refusal spellings to zero, and it now
# walks its lines without ever building a CMake list.
file(READ "${testsFile}" content)
string(REPLACE ";" "\\;" content "${content}")
string(REPLACE "[" " " content "${content}")
string(REPLACE "]" " " content "${content}")
string(REPLACE "\r\n" "\n" content "${content}")
string(REPLACE "\n" ";" lines "${content}")

# ---------------------------------------------------------------------------
# Pass 1: which tests are registered by running a `cmake -P` script.
#
# The name and the `-P` sit in different lines of one `add_test()`, so the scan
# remembers the most recent NAME and attributes the script to it. A `-P` reached
# with no NAME in hand is reported rather than skipped: it means this scan lost
# track, and a verdict drawn from a lost scan is worth nothing.
set(scriptChecks "")
set(violations "")
set(pendingName "")
set(lineNumber 0)

foreach(line IN LISTS lines)
    math(EXPR lineNumber "${lineNumber} + 1")
    if(line MATCHES "^[ \t]*#")
        continue()
    endif()

    if(line MATCHES "NAME[ \t]+\"([^\"]+)\"")
        set(pendingName "${CMAKE_MATCH_1}")
    endif()

    if(line MATCHES "-P[ \t]+\"")
        if(pendingName STREQUAL "")
            list(APPEND violations
                 "src/tests/CMakeLists.txt:${lineNumber}: a `cmake -P` registration with no NAME above it; this scan cannot attribute it, so its verdict on this file means nothing")
        else()
            list(APPEND scriptChecks "${pendingName}")
        endif()
        set(pendingName "")
    endif()
endforeach()

# A scan that matched nothing would report success while checking nothing, which
# is the whole shape this file argues against.
if(NOT scriptChecks)
    message(FATAL_ERROR
        "no `cmake -P` test registration was found in ${testsFile} at all; this check would pass vacuously")
endif()
list(REMOVE_DUPLICATES scriptChecks)
list(LENGTH scriptChecks scriptCheckCount)

# ---------------------------------------------------------------------------
# Pass 2: each of them must be given the failure signal.
#
# The property is matched by NAME rather than by its value, so a check that
# spells the pattern some other way still counts as having answered the
# question -- the point is that somebody decided, not that they decided this.
# `FASTCACHED_SCRIPT_CHECK_FAILED` exists so nobody has to.
foreach(check IN LISTS scriptChecks)
    string(REPLACE "$" "\\$" escaped "${check}")
    string(REPLACE "{" "\\{" escaped "${escaped}")
    string(REPLACE "}" "\\}" escaped "${escaped}")

    set(signalled FALSE)
    set(inBlock FALSE)
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*#")
            continue()
        endif()
        if(line MATCHES "set_tests_properties\\([ \t]*\"${escaped}\"")
            set(inBlock TRUE)
        elseif(inBlock AND line MATCHES "FAIL_REGULAR_EXPRESSION")
            set(signalled TRUE)
            break()
        elseif(inBlock AND line MATCHES "^[ \t]*\\)[ \t]*$")
            set(inBlock FALSE)
        endif()
    endforeach()

    if(NOT signalled)
        list(APPEND violations
             "`${check}` runs a `cmake -P` script but has no FAIL_REGULAR_EXPRESSION, so ctest marks it PASSED whatever the script reports")
    endif()
endforeach()

if(violations)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("A `cmake -P` script cannot fail its own test: message(FATAL_ERROR) prints")
    message("`CMake Error` and exits 0 on CMake 3.28, this project's minimum. Add")
    message("")
    message("    FAIL_REGULAR_EXPRESSION \"\${FASTCACHED_SCRIPT_CHECK_FAILED}\"")
    message("")
    message("to the set_tests_properties() block, alongside LABELS and TIMEOUT.")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR "script-check signals: ${violationCount} check(s) cannot fail")
endif()

message(STATUS
    "script-check signals: ${scriptCheckCount} `cmake -P` check(s), all able to report failure")
