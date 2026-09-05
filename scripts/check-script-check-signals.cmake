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
# This file used to justify itself by saying that `message(FATAL_ERROR)` in
# script mode prints `CMake Error ...` and exits **0** on CMake 3.28, this
# project's declared minimum -- "measured: 3.28.3 exits 0, 4.3.1 exits 1".
#
# **That does not reproduce, and the sentence was the sole stated reason for
# thirteen registrations** (#565). Measured across six CMake versions -- 3.22.6,
# 3.25.2, 3.27.9, 3.28.3, 3.31.6, 4.3.0 -- and six script shapes -- bare, inside
# `if()`, inside `function()`, inside `foreach()`, inside `macro()`, and
# `SEND_ERROR` -- the exit code is **1** every time, with controls proving the
# harness could read a zero. `ctest -R fatal-error-exit` now asks the same
# question on every platform CI builds rather than leaving it a sentence.
#
# The likely mechanism, because it is worth recording how a measured claim came
# out wrong: a TRUE neighbouring fact carried one clause too far. A `-P` script
# genuinely cannot CHOOSE its exit code before CMake 3.29 -- `cmake_language(EXIT)`
# is 3.29, and on 3.28.3 it is an unknown command -- which is why the SKIP
# direction correctly uses `SKIP_REGULAR_EXPRESSION` rather than
# `SKIP_RETURN_CODE 77`. "Cannot choose its exit code" was then read as "cannot
# signal failure by exit code", and `message(FATAL_ERROR)` gives you exactly one
# code you did not choose, 1, which is the only one a failure needs. The two
# reproducible ways to see a 0 are both instrument shapes and neither is a CMake
# version: an unguarded pipeline (`cmake -P … | tail` reports the pipe), and a
# nested `cmake -P` whose `RESULT_VARIABLE` is unread.
#
# **The requirement stays, and its reasons are now the true ones.** Two, and
# neither was the stated one:
#
#   * `message(WARNING)` exits **0** on every version above while printing
#     `CMake Warning`, so only an output verdict can hear a check that warns --
#     the pattern covers both words for that reason (#517), and
#     `script-check-warning-canary` is the half that proves it.
#   * a check that shells out to another CMake without reading `RESULT_VARIABLE`
#     exits 0 with its child's `CMake Error` on the output. That is a real shape
#     for a check that drives another script, and it is what
#     `script-check-canary` is now built out of.
#
# The fix's own weakness is that it is a property somebody has to remember.
# `script-check-canary` covers the case where the mechanism stops working; this
# covers the case where a new registration never opts into it -- which is the far
# likelier of the two, and the one that reads as a working check.
#
# And the third question, which is about the canaries rather than the checks:
# `WILL_FAIL` inverts ctest's whole verdict, and that verdict is `non-zero exit OR
# the pattern matched`, so a canary exiting non-zero passes on its status alone and
# proves nothing about the property. Pass 4 runs every WILL_FAIL registration's
# script and refuses a non-zero one -- the alternative being one more sentence
# somebody has to remember, which is what #565 was about.
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
# Exit codes: whatever CMake gives it, which for `message(FATAL_ERROR)` is 1 on
# every version measured. The verdict is still read from the presence of
# `CMake Error` in the output, because that is the rule this file enforces and a
# check exempting itself from its own rule is not one.

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
set(sawVacuousCanary FALSE)
set(willFailRegistrations "")

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
    set(willFail FALSE)
    set(inBlock FALSE)
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*#")
            continue()
        endif()
        if(line MATCHES "set_tests_properties\\([ \t]*\"${escaped}\"")
            set(inBlock TRUE)
        elseif(inBlock AND line MATCHES "FAIL_REGULAR_EXPRESSION")
            set(signalled TRUE)
        elseif(inBlock AND line MATCHES "WILL_FAIL[ \t]+TRUE")
            # Recorded rather than acted on here; pass 4 is where it is spent,
            # beside the resolved path it needs. The scan no longer stops at the
            # first property it recognises, because two of them are wanted now.
            set(willFail TRUE)
        elseif(inBlock AND line MATCHES "^[ \t]*\\)[ \t]*$")
            set(inBlock FALSE)
            break()
        endif()
    endforeach()

    if(willFail)
        list(APPEND willFailRegistrations "${registration}")
    endif()

    if(NOT signalled)
        set(sawMissingSignal TRUE)
        list(APPEND violations
             "`${check}` runs a `cmake -P` script but has no FAIL_REGULAR_EXPRESSION, so ctest marks it PASSED whatever the script reports")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The declaration test, once, because pass 3 and pass 3b have two different
# subjects and one rule. A macro rather than a function so the append lands in
# the caller's scope -- the alternative is a PARENT_SCOPE dance around a list,
# which is more moving parts for the same effect.
#
# Anchored at line start, so the COMMENT explaining an absence is not read as a
# presence, and `LIMIT_COUNT 1` bounds MATCHES rather than `LIMIT_INPUT`, which
# bounds BYTES -- a declaration below a byte bound would be scored as absent.
macro(fastcached_require_cmake_minimum resolvedPath shownPath runBy)
    file(STRINGS "${resolvedPath}" _declaration
         REGEX "^cmake_minimum_required\\(" LIMIT_COUNT 1)
    if(NOT _declaration)
        set(sawMissingDeclaration TRUE)
        list(APPEND violations
             "`${shownPath}`, ${runBy}, declares no cmake_minimum_required, so every policy is unset in it and a policy-gated construct will mean different things on 3.28 and on 4.x")
    endif()
endmacro()

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
    fastcached_require_cmake_minimum("${resolved}" "${scriptPath}" "run by `${check}`")

    # -----------------------------------------------------------------------
    # Pass 4, folded into pass 3 because it needs the same resolved path: a
    # WILL_FAIL registration's script must exit **0**.
    #
    # ctest fails a test when its exit code is non-zero OR the FAIL pattern
    # matches, and WILL_FAIL inverts that whole verdict. So a canary that exits
    # non-zero is green on its exit code alone: delete FAIL_REGULAR_EXPRESSION
    # from every registration in this tree and it would go on reporting success,
    # having proved nothing about the property it exists to prove. That is
    # exactly the vacuous shape #565 found in `script-check-canary`, and the
    # canaries now avoid it only by exiting 0 -- which was a property nothing
    # asserted, i.e. the same "a property somebody has to remember" this file
    # exists to stop relying on.
    #
    # Only the exit status is asserted. The other half announces itself: a canary
    # that stops PRINTING the pattern passes under ctest, WILL_FAIL inverts it,
    # and the canary itself goes red by name.
    #
    # The child's output is CAPTURED, never inherited -- a canary prints
    # `CMake Error` on purpose and inheriting it would trip this check's own
    # FAIL_REGULAR_EXPRESSION and report a violation for a canary that works.
    if(registration IN_LIST willFailRegistrations)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -P "${resolved}"
            RESULT_VARIABLE canaryStatus
            OUTPUT_VARIABLE canaryOut
            ERROR_VARIABLE canaryErr
        )
        if(NOT canaryStatus STREQUAL "0")
            set(sawVacuousCanary TRUE)
            list(APPEND violations
                 "`${check}` is registered WILL_FAIL and its script exits ${canaryStatus} -- ctest's verdict is `non-zero OR pattern`, so WILL_FAIL inverts a pass it got from the exit code alone, and the canary would stay green with every FAIL_REGULAR_EXPRESSION in this file deleted")
        endif()
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Pass 3b: and the `cmake -P` scripts CPack runs, which no ctest registration
# names (#680).
#
# `cmake/MacOSSignBinaries.cmake` and `cmake/MacOSNotarizePkg.cmake` are
# CPACK_PRE_BUILD_SCRIPTS / CPACK_POST_BUILD_SCRIPTS. CPack executes them in
# script mode exactly as ctest executes a check, so every policy is unset in them
# for the same reason -- but they are reached through `cmake/Packaging.cmake`
# rather than through a registration, so pass 3 walked straight past them.
#
# They are the WORSE case rather than the lesser one. A policy misread in a check
# is a red test; here it is an unsigned or unnotarized package on the release leg,
# discovered by whoever installs it. And the release leg is the one place in this
# project where a defect is most expensive to find.
#
# DISCOVERED from `cmake/Packaging.cmake`, never listed here. A second copy of the
# hook paths would be a second thing to be wrong, and it would go stale in the
# direction that reports green -- the same argument pass 1 makes for reading the
# registrations out of the tree.
#
# ## What this does NOT cover, said plainly so the next reader does not assume it
#
# Two more `cmake -P` scripts in this tree are GENERATED at run time by
# `scripts/check-fetch-transfer-bound.py`. They do not exist when this check runs
# and no static scan can reach them. That is a limit of this check, not a claim
# that every `cmake -P` script in the repository is covered. It is stated because
# #497 counted them and left them out deliberately, and a reader who finds them
# later would otherwise conclude this scan had missed them silently.
set(packagingFile "${FASTCACHED_SOURCE_DIR}/cmake/Packaging.cmake")
set(cpackHookCount 0)

# ONE finding for one cause. Without the `else()` the loop below still runs, each
# read comes back empty, and a single missing file reports as three violations --
# a count that overstates what is wrong is the same defect as one that
# understates it, and this pass has already been caught miscounting once.
set(cpackHookVars "")
if(NOT EXISTS "${packagingFile}")
    list(APPEND violations
         "cmake/Packaging.cmake is not there, so the CPack hook scripts cannot be discovered and this pass would vouch for nothing while reporting nothing")
else()
    set(cpackHookVars CPACK_PRE_BUILD_SCRIPTS CPACK_POST_BUILD_SCRIPTS)
endif()

foreach(hookVar IN LISTS cpackHookVars)
    # `LIMIT_COUNT 1`, which is the one `file(STRINGS)` shape that cannot merge
    # elements: with a single element there is nothing for an unbalanced `[` in a
    # neighbouring line to merge WITH. Every other reader in this tree that skips
    # it is bracket-vulnerable, and two of them fail SILENTLY when it bites.
    file(STRINGS "${packagingFile}" hookLine
         REGEX "^[ \t]*set\\(${hookVar}[ \t]" LIMIT_COUNT 1)

    # An empty scan is a REFUSAL, not a pass. Two empty lists agree perfectly, and
    # a hook variable that was renamed would otherwise take this whole pass quietly
    # out of service -- which is `node-config-reference`'s rule and the failure this
    # check would reproduce without it.
    if(NOT hookLine)
        list(APPEND violations
             "no `set(${hookVar} ...)` in cmake/Packaging.cmake -- either the hook is gone or it is spelled in a way this scan does not recognise, and in both cases the script it names is no longer checked")
        continue()
    endif()

    if(NOT hookLine MATCHES "\"([^\"]+)\"")
        # Escaped BEFORE it is interpolated, for the reason this file's header
        # gives about `file(STRINGS)`: a `;` reaching `list(APPEND)` becomes an
        # ELEMENT BOUNDARY, so one violation prints as two lines and is COUNTED
        # as two. Measured while writing this pass -- a literal `;` in the message
        # above made a single finding report as "2 finding(s)", a check
        # miscounting its own output. The convention in this file is that a
        # violation string carries no raw semicolon, and anything interpolated out
        # of a file has to be made to obey it.
        string(REPLACE ";" "\\;" shownHookLine "${hookLine}")
        list(APPEND violations
             "`set(${hookVar} ...)` in cmake/Packaging.cmake names no quoted path this scan can read: ${shownHookLine}")
        continue()
    endif()
    set(hookPath "${CMAKE_MATCH_1}")
    string(REGEX REPLACE "^\\$\\{[A-Za-z_0-9]+\\}/" "" hookPath "${hookPath}")

    set(resolvedHook "${FASTCACHED_SOURCE_DIR}/${hookPath}")
    if(NOT EXISTS "${resolvedHook}")
        string(REPLACE ";" "\\;" shownHookPath "${hookPath}")
        list(APPEND violations
             "cmake/Packaging.cmake sets ${hookVar} to `${shownHookPath}`, which is not there")
        continue()
    endif()

    math(EXPR cpackHookCount "${cpackHookCount} + 1")
    fastcached_require_cmake_minimum("${resolvedHook}" "${hookPath}" "run by CPack as ${hookVar}")
endforeach()

if(violations)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    if(sawMissingSignal)
    message("A `cmake -P` check is judged by its OUTPUT here, not by its exit code:")
    message("the pattern must also hear a check that merely WARNS, and a warning")
    message("exits 0 on every CMake. Add")
    message("")
    message("    FAIL_REGULAR_EXPRESSION \"\${FASTCACHED_SCRIPT_CHECK_FAILED}\"")
    message("")
    message("to the set_tests_properties() block, alongside LABELS and TIMEOUT.")
    message("")
    endif()
    if(sawVacuousCanary)
    message("A WILL_FAIL registration inverts ctest's WHOLE verdict, and that verdict is")
    message("`non-zero exit OR the pattern matched`. A canary that exits non-zero therefore")
    message("passes on its exit code alone and says nothing about the pattern -- which is")
    message("the shape #565 found. Make the script exit 0 and print `CMake Error` (a nested")
    message("`cmake -P` whose RESULT_VARIABLE is unread does exactly that), so the pattern")
    message("is the only thing left that can fail it.")
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

# A count of BAD things says nothing about whether the good things exist, so the
# canary pass gets its own positive control: the two WILL_FAIL registrations are
# what the whole FAIL_REGULAR_EXPRESSION mechanism rests on, and a scan that found
# none of them would have checked nothing while printing the same summary.
list(LENGTH willFailRegistrations willFailCount)
if(willFailCount EQUAL 0)
    message(FATAL_ERROR
        "no WILL_FAIL registration was found in ${testsFile} at all; the canaries that "
        "prove the FAIL_REGULAR_EXPRESSION mechanism still works are gone, or this scan "
        "has stopped seeing them -- either way pass 4 checked nothing")
endif()

message(STATUS
    "script-check signals: ${scriptCheckCount} `cmake -P` registration(s), all able to "
    "report failure and all running a script that declares a CMake minimum; "
    "${willFailCount} WILL_FAIL canary/canaries, all exiting 0 so the pattern is what "
    "decides them; plus ${cpackHookCount} CPack hook script(s) discovered from "
    "cmake/Packaging.cmake")
