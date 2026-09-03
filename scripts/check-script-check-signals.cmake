# SPDX-License-Identifier: Apache-2.0
#
# Every `cmake -P` check registered in src/tests/CMakeLists.txt must be one ctest
# can HEAR, and one whose meaning does not depend on which CMake ran it.
#
# Two rules over one subject: it must carry a failure signal, because a script
# cannot report one with an exit code; and the script behind it must declare a
# CMake minimum, because script mode sets no policies. Pass 3 states the second
# in full.
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

cmake_minimum_required(VERSION 3.28)

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
set(scriptRegistrations "")
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

    if(line MATCHES "-P[ \t]+\"([^\"]+)\"")
        # Carried VERBATIM. Turning it into a path on disk is pass 3's job and
        # is done there, beside the `EXISTS` that consumes it -- split across
        # two passes, the transformation and the diagnostic that reports it
        # failing sat ninety lines apart and neither could be read alone.
        set(scriptPath "${CMAKE_MATCH_1}")
        if(pendingName STREQUAL "")
            list(APPEND violations
                 "src/tests/CMakeLists.txt:${lineNumber}: a `cmake -P` registration with no NAME above it; this scan cannot attribute it, so its verdict on this file means nothing")
        else()
            list(APPEND scriptRegistrations "${pendingName}|${scriptPath}")
        endif()
        set(pendingName "")
    endif()
endforeach()

# A scan that matched nothing would report success while checking nothing, which
# is the whole shape this file argues against.
if(NOT scriptRegistrations)
    message(FATAL_ERROR
        "no `cmake -P` test registration was found in ${testsFile} at all; this check would pass vacuously")
endif()
list(REMOVE_DUPLICATES scriptRegistrations)
list(LENGTH scriptRegistrations scriptCheckCount)

# Split one `<test name>|<-P value as written>` row.
#
# A plain split rather than the general row splitter three other scripts carry:
# neither field can hold a `|`, and the field count is asserted rather than
# assumed. The name and the path travel as ONE row because they are one
# registration -- two lists appended in step are two lists that can stop being
# in step, and nothing would say so.
#
# @param row The `<name>|<path>` row.
# @param nameOut Set to the ctest name.
# @param pathOut Set to the `-P` value exactly as the registration spells it.
function(fastcached_registration_fields row nameOut pathOut)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 2)
        message(FATAL_ERROR
            "registration row split into ${fieldCount} field(s) where 2 are wanted: ${row}")
    endif()
    list(GET fields 0 rowName)
    list(GET fields 1 rowPath)
    set(${nameOut} "${rowName}" PARENT_SCOPE)
    set(${pathOut} "${rowPath}" PARENT_SCOPE)
endfunction()

set(sawMissingSignal FALSE)
set(sawMissingDeclaration FALSE)

# ---------------------------------------------------------------------------
# Pass 2: each of them must be given the failure signal.
#
# The property is matched by NAME rather than by its value, so a check that
# spells the pattern some other way still counts as having answered the
# question -- the point is that somebody decided, not that they decided this.
# `FASTCACHED_SCRIPT_CHECK_FAILED` exists so nobody has to.
foreach(registration IN LISTS scriptRegistrations)
    fastcached_registration_fields("${registration}" check scriptPath)
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
        set(sawMissingSignal TRUE)
        list(APPEND violations
             "`${check}` runs a `cmake -P` script but has no FAIL_REGULAR_EXPRESSION, so ctest marks it PASSED whatever the script reports")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Pass 3: each of those scripts must declare a CMake minimum.
#
# Script mode has no project, so every policy starts UNSET, and a policy-gated
# construct then means different things depending on which CMake ran it.
# Measured on 3.28.3, this project's declared minimum and the version CI runs,
# against the same script with a declaration added:
#
#   if("b" IN_LIST haystack)   CMake Error, exit 1 -- errors before answering
#   if(TRUE)                   exit 0, and the branch is NOT taken
#
# Both behave on 4.x, where those policies were removed and the NEW behaviour is
# unconditional. So an author's machine can disagree with CI -- and, the half
# with no other remedy, with the REVIEWER's machine: a reviewer on 4.x cannot
# reproduce either of those findings at all. That is why the declaration is the
# fix rather than reviewer care. See .agent/rules/build-and-toolchain.md.
#
# REQUIRED rather than enumerated. The set of policy-gated constructs grows with
# every CMake release, so a guard that lists them is stale by construction and a
# guard that demands the declaration is not.
#
# It lives here, rather than in a check of its own, because of the PARSER. Pass 1
# is forty lines of NAME/`-P` pairing plus two refuse-to-conclude invariants, and
# a separate script would have to duplicate that or restate the list of scripts --
# and a second copy of the list is what this file exists to argue against. The
# tempting reason, that a new script "would itself need the declaration", is not
# one: it would be registered here like every other check and covered by this very
# pass.
foreach(registration IN LISTS scriptRegistrations)
    fastcached_registration_fields("${registration}" check scriptPath)

    # Resolving the `-P` value happens HERE, beside the `EXISTS` it feeds,
    # rather than in pass 1 where nothing consumes it yet. Every registration
    # spells the root the same way today; one that does not is reported rather
    # than skipped, because that is this scan losing track of its subject.
    string(REGEX REPLACE "^\\$\\{[A-Za-z_0-9]+\\}/" "" scriptPath "${scriptPath}")
    if(scriptPath MATCHES "\\$\\{")
        # `EXISTS` below would refuse it too. This says WHY in the scan's own
        # terms rather than blaming the tree -- it exists only to reword that,
        # so do not add a fourth mechanism beside it.
        list(APPEND violations
             "`${check}` names `${scriptPath}`, which still holds an unexpanded CMake variable; the registration spells its path in a way pass 1 does not understand")
        continue()
    endif()

    set(resolved "${FASTCACHED_SOURCE_DIR}/${scriptPath}")
    if(NOT EXISTS "${resolved}")
        list(APPEND violations
             "`${check}` is registered to run `${scriptPath}`, which is not there")
        continue()
    endif()

    # Anchored at line start, so the COMMENT explaining an absence is not read as
    # a presence. Not hypothetical: a word-match for `cmake_minimum_required`
    # scored check-sccache-backend-caveat.cmake as compliant on the strength of a
    # comment saying it had none, and returned 10 where the answer is 11.
    #
    # `LIMIT_COUNT` bounds MATCHES, and never `LIMIT_INPUT`, which bounds BYTES:
    # a declaration below a byte bound would be scored as absent, which is the
    # same wrong answer the word-match gave. The saving is small either way --
    # measured 27 ms against 37 ms over these 30 files on a 9p mount, since a
    # compliant file stops reading at its declaration and only a violating one
    # is read to the end. Correctness is why this line reads as it does, not
    # the 10 ms.
    file(STRINGS "${resolved}" declaration REGEX "^cmake_minimum_required\\(" LIMIT_COUNT 1)
    if(NOT declaration)
        set(sawMissingDeclaration TRUE)
        list(APPEND violations
             "`${scriptPath}`, run by `${check}`, declares no cmake_minimum_required, so every policy is unset in it and a policy-gated construct will mean different things on 3.28 and on 4.x")
    endif()
endforeach()

if(violations)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    if(sawMissingSignal)
    message("A `cmake -P` script cannot fail its own test: message(FATAL_ERROR) prints")
    message("`CMake Error` and exits 0 on CMake 3.28, this project's minimum. Add")
    message("")
    message("    FAIL_REGULAR_EXPRESSION \"\${FASTCACHED_SCRIPT_CHECK_FAILED}\"")
    message("")
    message("to the set_tests_properties() block, alongside LABELS and TIMEOUT.")
    message("")
    endif()
    if(sawMissingDeclaration)
    message("A `cmake -P` script states its policies, because script mode sets none:")
    message("")
    message("    cmake_minimum_required(VERSION 3.28)")
    message("")
    message("before the first line that is not a comment. Without it `if(... IN_LIST ...)`")
    message("errors out on 3.28 and answers on 4.x, and `if(TRUE)` is false on 3.28 and")
    message("true on 4.x -- so the check disagrees with CI, and with whoever reviews it.")
    endif()
    list(LENGTH violations violationCount)
    message(FATAL_ERROR
        "script-check signals: ${violationCount} finding(s) across ${scriptCheckCount} "
        "registration(s)")
endif()

message(STATUS
    "script-check signals: ${scriptCheckCount} `cmake -P` registration(s), all able to "
    "report failure and all running a script that declares a CMake minimum")
