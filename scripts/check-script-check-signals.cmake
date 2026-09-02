# SPDX-License-Identifier: Apache-2.0
#
# Every `cmake -P` check registered in src/tests/CMakeLists.txt must carry
# FAIL_REGULAR_EXPRESSION, as the SECOND of two independent failure signals.
#
# ## The reason this file used to give, which was wrong (#565)
#
# It said `message(FATAL_ERROR)` in script mode prints `CMake Error ...` and exits
# **0** on CMake 3.28, this project's declared minimum -- so a bare `cmake -P`
# registration was a check ctest marked PASSED however loudly it objected, and the
# property was the only thing standing between thirteen checks and silence.
#
# That does not reproduce. Re-measured on CMake 3.28.3 with one probe file per
# shape -- bare, inside `if()`, inside a function, inside `foreach()`, after prior
# output, and after `cmake_minimum_required()` -- `FATAL_ERROR` exits **1** in every
# one. A clean script exits 0 in the same harness, so the probe could tell them
# apart. The original measurement is unreproducible rather than conditional: no
# shape tested reproduces it on that version.
#
# It was wrong a second and independent way, which matters because the argument
# leaned on it: "on the platform CI builds on" was never true of 3.28. CI installs
# no CMake, so it uses the runner image's, and `ubuntu-24.04` at the release these
# runs use ships CMake **3.31.5** and 4.1.2. The declared minimum and the CI
# toolchain were being treated as the same thing.
#
# ## Why the requirement stays anyway
#
# Because a guard is not deleted on the strength of its motivation being wrong.
# What changes is its status: FAIL_REGULAR_EXPRESSION is defence in depth, not the
# only signal. A `-P` check that objects both exits nonzero and prints `CMake
# Error` -- always both, since no route to a nonzero exit is silent -- so two
# independent things must break before a failing check reports green.
#
# The cost is one property per registration. The benefit is a ctest that stopped
# honouring exit codes, or a wrapper that captured and re-emitted them, still
# fails the check.
#
# ## What proves each signal
#
# Both, separately, or "defence in depth" is one signal and a story:
# `script-check-canary` prints the pattern and exits 0, so only the property can
# fail it; `script-check-exit-canary` exits nonzero and prints nothing, so only the
# exit code can. Neither can pass on the other's signal, and that is the property
# #565 found the old single canary lacked.
#
# This file covers the third case, which neither canary can: a registration that
# never opts in. That is the likeliest of the three and the one that reads as a
# working check.
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
# Exit codes: nonzero on failure, and the output also carries `CMake Error`. Both,
# because a `-P` script cannot fail silently -- see above.

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
    message("Script checks carry two independent failure signals, and this is the")
    message("second one: the exit code, plus `CMake Error` in the output. Add")
    message("")
    message("    FAIL_REGULAR_EXPRESSION \"\${FASTCACHED_SCRIPT_CHECK_FAILED}\"")
    message("")
    message("to the set_tests_properties() block, alongside LABELS and TIMEOUT.")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR "script-check signals: ${violationCount} check(s) cannot fail")
endif()

message(STATUS
    "script-check signals: ${scriptCheckCount} `cmake -P` check(s), all able to report failure")
