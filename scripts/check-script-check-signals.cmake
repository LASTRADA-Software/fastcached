# SPDX-License-Identifier: Apache-2.0
#
# Every `cmake -P` check registered in src/tests/CMakeLists.txt must be one ctest
# can HEAR, and one whose meaning does not depend on which CMake ran it.
#
# Three rules over one subject: it must carry a failure signal, because a script
# cannot report one with an exit code; the script behind it must declare a CMake
# minimum, because script mode sets no policies; and everything that READS that
# signal must read ALL of it. Pass 3 states the second in full and pass 5 the
# third.
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
set(sawVerdictHalfRead FALSE)
set(willFailRegistrations "")

# ---------------------------------------------------------------------------
# Pass 2, first half: what each `set_tests_properties()` block carries, in ONE
# walk of the file (#679).
#
# This used to be a walk PER REGISTRATION -- 71 registrations against a 5,070-line
# file, where one pass answers the same question. The ticket recorded 30 x 2,126
# and about 150 ms of a 190 ms run; both halves have grown since, and the figures
# in this file are the ones measured on the tree that changed it rather than the
# ones the ticket carried.
#
# The rows are `<test name>|<signalled>|<will fail>` and the FIRST block for a name
# wins, which is what the per-registration walk did by stopping at the one it
# found. One list of rows rather than parallel lists, for the reason
# `fastcached_registration_fields` gives one row for a name and a path: two lists
# appended in step are two lists that can stop being in step.
#
# The name is now compared EXACTLY where it used to be a regex over a partially
# escaped name -- `$`, `{` and `}` escaped, and every other metacharacter left
# standing, so a name holding a `.` would have matched a block belonging to a
# different test. That is stricter, not merely different, and it changes nothing
# here: measured over the 154 `add_test` names in this file, three carry
# `${...}` (they are registered inside `foreach()` loops) and those were already
# escaped; none carries any other metacharacter. Both spellings therefore agree on
# this tree, and the equivalence was demonstrated per registration rather than
# argued -- see the ticket.
set(propertyBlocks "")
set(currentBlock "")
set(currentSignalled FALSE)
set(currentWillFail FALSE)

# The row for a test name, or "" when no block was found for it.
#
# A scan rather than a variable named after the test, because three of these names
# ARE the text `${...}`: `set(_seen_${check} ...)` would expand it and record the
# answer under a name no lookup could ask for.
#
# @param wanted The ctest name.
# @param rowOut Set to the matching `<name>|<signalled>|<will fail>` row, or "".
function(fastcached_block_row wanted rowOut)
    set(${rowOut} "" PARENT_SCOPE)
    foreach(row IN LISTS propertyBlocks)
        string(FIND "${row}" "|" barAt)
        if(barAt EQUAL -1)
            message(FATAL_ERROR "a property-block row carries no separator: ${row}")
        endif()
        string(SUBSTRING "${row}" 0 ${barAt} rowName)
        if(rowName STREQUAL wanted)
            set(${rowOut} "${row}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# Close the block in hand and file what it carried. A macro, so the append lands in
# this scope; called at the closing `)` and again at end of file, because the
# per-registration walk recorded a property the moment it saw it and did not need
# the block to be closed for that.
macro(fastcached_close_block)
    if(NOT currentBlock STREQUAL "")
        list(APPEND propertyBlocks "${currentBlock}|${currentSignalled}|${currentWillFail}")
        set(currentBlock "")
    endif()
endmacro()

foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*#")
        continue()
    endif()

    if(line MATCHES "set_tests_properties\\([ \t]*\"([^\"]+)\"")
        fastcached_close_block()
        set(candidate "${CMAKE_MATCH_1}")
        fastcached_block_row("${candidate}" existingRow)
        if(existingRow STREQUAL "")
            set(currentBlock "${candidate}")
            set(currentSignalled FALSE)
            set(currentWillFail FALSE)
        endif()
    elseif(NOT currentBlock STREQUAL "" AND line MATCHES "FAIL_REGULAR_EXPRESSION")
        set(currentSignalled TRUE)
    elseif(NOT currentBlock STREQUAL "" AND line MATCHES "WILL_FAIL[ \t]+TRUE")
        # Recorded rather than acted on here; pass 4 is where it is spent, beside
        # the resolved path it needs. The scan does not stop at the first property
        # it recognises, because two of them are wanted.
        set(currentWillFail TRUE)
    elseif(NOT currentBlock STREQUAL "" AND line MATCHES "^[ \t]*\\)[ \t]*$")
        fastcached_close_block()
    endif()
endforeach()
fastcached_close_block()

# A walk that stops recognising blocks is LOUD either way -- every registration then
# looks unsignalled -- so this control is not about silence, and saying it was would
# be the tidier claim rather than the true one. It is about ATTRIBUTION. Measured, by
# renaming `set_tests_properties` throughout a staged copy: without this the check
# reports 71 findings against 71 checks that have nothing wrong with them, and with
# it, one finding that names the walk. An instrument fault wearing the findings'
# clothes is the outcome a file arguing for measurement over memory must not produce.
#
# A floor rather than equality: blocks outnumber `cmake -P` registrations, every ctest
# test here having one. So it can only be crossed by a walk that has lost most of the
# file, never by one registration legitimately going without a block.
list(LENGTH propertyBlocks propertyBlockCount)
if(propertyBlockCount LESS scriptCheckCount)
    message(FATAL_ERROR
        "read ${propertyBlockCount} set_tests_properties() block(s) out of ${testsFile} for "
        "${scriptCheckCount} `cmake -P` registration(s); every one of them has a block, so "
        "this walk has stopped recognising the shape and the verdicts below are drawn from "
        "a file it only partly read")
endif()

# ---------------------------------------------------------------------------
# Pass 2, second half: each of them must be given the failure signal.
#
# The property is matched by NAME rather than by its value, so a check that
# spells the pattern some other way still counts as having answered the
# question -- the point is that somebody decided, not that they decided this.
# `FASTCACHED_SCRIPT_CHECK_FAILED` exists so nobody has to.
#
# The finding names the ONE check it belongs to, which is the way a map gets this
# wrong: a lookup that blurred two registrations together would report a violation
# against a check that has nothing wrong with it, and nothing about the message
# would say so.
foreach(registration IN LISTS scriptRegistrations)
    fastcached_registration_fields("${registration}" check scriptPath)
    fastcached_block_row("${check}" blockRow)

    set(signalled FALSE)
    set(willFail FALSE)
    if(NOT blockRow STREQUAL "")
        string(REPLACE "|" ";" blockFields "${blockRow}")
        list(LENGTH blockFields blockFieldCount)
        if(NOT blockFieldCount EQUAL 3)
            message(FATAL_ERROR
                "property-block row split into ${blockFieldCount} field(s) where 3 are wanted: ${blockRow}")
        endif()
        list(GET blockFields 1 signalled)
        list(GET blockFields 2 willFail)
    endif()

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

# ---------------------------------------------------------------------------
# Pass 5: everything that READS the signal must read ALL of it (#672).
#
# The signal is two words. `FASTCACHED_SCRIPT_CHECK_FAILED` is
# `CMake Error|CMake Warning` and both halves are canaried -- `script-check-canary`
# for the first, `script-check-warning-canary` for the second -- because
# `message(WARNING)` exits 0 on every CMake while changing meaning, and an unset
# policy, a deprecated command or a dev warning all arrive that way.
#
# A selftest harness runs the check it is about as a SUB-PROCESS and reads the
# verdict out of the captured output itself. ctest never sees that output, so the
# registration's pattern does not reach it and the harness has to spell the rule
# again. Twenty of them spelled half of it -- `CMake Error` alone -- so a sub-run
# that merely WARNED was scored a clean pass by the very fixtures whose job is to
# prove a guard bites. Measured on the tree that fixed it, by giving every check an
# unconditional `message(WARNING)`: 2 of 22 harnesses objected before, 22 of 22
# after. A harness laxer than the registration it stands for is a fake more
# permissive than the thing it models.
#
# So this pass is the reason nobody has to remember, which is the argument pass 2
# makes for the registrations one level up.
#
# ## What it recognises, and what it deliberately does not
#
# A line that tests text for the literal `CMake Error` -- `string(FIND)`,
# `string(REGEX)` or `MATCHES` -- must spell `CMake Warning` on the same line. That
# is narrow on purpose: it is exact about the shape it reads and the remedy is one
# word.
#
# It cannot see a harness that reads NO verdict at all. Two here did not: one
# asserted only substrings, and one defaulted its `must appear` needle to the error
# word. Both were given an explicit warning assertion rather than this scan being
# widened, because "runs a sub-`cmake -P` and draws a conclusion from it" has no
# reliable static shape and a guard that guessed at it would refuse correct files.
# Said plainly, so a green pass here is not read as coverage of that.
#
# ## Deliberate must not be spelled like forgotten
#
# Some sites read the words `CMake Error` for a reason that is not a verdict: a case
# TABLE spells it in a `must not appear` field to mean "this row expects
# acceptance", and `check-fatal-error-exit` asks whether one specific
# `message(FATAL_ERROR)` arm produced one, where a warning would be the wrong answer
# rather than a stricter one. Those carry `verdict-error-only:` and a REASON in the
# comment block immediately above, and the count is PRINTED on every run -- a marker
# with no reason is refused, and a marker nobody ever reads would spell "forgot" in
# the vocabulary of "decided".
#
# ## Why the walk builds no list
#
# The same reason pass 1 escapes before splitting, one step further: this is a
# FIND/SUBSTRING walk that never hands a line to CMake's list parser, so an
# unbalanced `[` in a comment cannot merge two lines and move a reported line
# number. The whole-file test comes first, because most files under scripts/ hold
# neither word and reading those line by line is the cost this repository has
# already paid once.
set(verdictSites 0)
set(verdictExceptions 0)
file(GLOB checkScripts "${FASTCACHED_SOURCE_DIR}/scripts/*.cmake")

# A glob that matched nothing would report success having read no file at all,
# which is the vacuous shape this whole file argues against.
if(NOT checkScripts)
    message(FATAL_ERROR
        "no scripts/*.cmake was found under ${FASTCACHED_SOURCE_DIR} at all; pass 5 would "
        "vouch for every verdict reader in this tree while reading none of them")
endif()

foreach(scriptFile IN LISTS checkScripts)
    file(READ "${scriptFile}" scriptText)
    # verdict-error-only: the whole-file pre-filter of this very scan, so it reads the
    # word rather than a verdict. Marked rather than exempting this file, because a
    # real verdict reader added here later must still be caught.
    if(NOT scriptText MATCHES "CMake Error")
        continue()
    endif()
    file(RELATIVE_PATH shownScript "${FASTCACHED_SOURCE_DIR}" "${scriptFile}")

    set(rest "${scriptText}")
    set(scriptLine 0)
    set(pendingReason "")
    set(sawMarker FALSE)
    while(NOT rest STREQUAL "")
        string(FIND "${rest}" "\n" newlineAt)
        if(newlineAt EQUAL -1)
            set(line "${rest}")
            set(rest "")
        else()
            string(SUBSTRING "${rest}" 0 ${newlineAt} line)
            math(EXPR afterNewline "${newlineAt} + 1")
            string(SUBSTRING "${rest}" ${afterNewline} -1 rest)
        endif()
        math(EXPR scriptLine "${scriptLine} + 1")

        # A COMMENT is not a call site. It is where an exception is DECLARED, though,
        # so a marker is collected here and spent on the next line of code -- which is
        # what makes "the comment block immediately above" the only place it can go.
        if(line MATCHES "^[ \t]*#")
            if(line MATCHES "verdict-error-only:[ \t]*(.*)$")
                string(STRIP "${CMAKE_MATCH_1}" pendingReason)
                set(sawMarker TRUE)
            endif()
            continue()
        endif()
        if(line MATCHES "^[ \t]*$")
            continue()
        endif()

        # The needle first, because it is a `string(FIND)` and the quote-strip below it is
        # a regex, and the overwhelming majority of lines in the 44 files that carry the
        # word do not carry it themselves. The saving is SMALL and is recorded as such:
        # whole check, min of 5, 1557 ms per-line against 1510 ms per-candidate -- about
        # 3%, where the cost of the pass as a whole is ~350 ms. Measured 2026-09-10 on
        # Windows 11 / Git Bash / native NTFS / CMake 4.3.1, conditions PINNED rather
        # than pointed at. The order is what it is because the cheap test belongs first,
        # not because 3% was worth buying.
        #
        # verdict-error-only: this is the detector itself, reading a line of SOURCE for
        # the word rather than reading a sub-run for a verdict.
        string(FIND "${line}" "CMake Error" errorWordAt)

        set(isReader FALSE)
        if(NOT errorWordAt EQUAL -1)
            # The OPERATOR is looked for with the quoted spans removed, and the NEEDLE in
            # the line as written. A line whose every token sits inside a string is
            # PRINTING, not testing -- and this file's own remedy text prints the
            # compliant spelling, so without this it counted as a verdict reader and the
            # "no compliant reader" control could never have fired. That is
            # check-glob-traversals' defect (a checker reporting its own documentation)
            # arriving in the POSITIVE CONTROL, where it is worse: it refuses nothing,
            # it vouches.
            string(REGEX REPLACE "\"[^\"]*\"" "" unquoted "${line}")
            if(unquoted MATCHES "string\\(FIND|string\\(REGEX|MATCHES")
                set(isReader TRUE)
            endif()
        endif()

        if(isReader)
            string(FIND "${line}" "CMake Warning" warningWordAt)
            if(NOT warningWordAt EQUAL -1)
                math(EXPR verdictSites "${verdictSites} + 1")
            elseif(sawMarker AND NOT pendingReason STREQUAL "")
                math(EXPR verdictExceptions "${verdictExceptions} + 1")
            elseif(sawMarker)
                set(sawVerdictHalfRead TRUE)
                list(APPEND violations
                     "${shownScript}:${scriptLine}: `verdict-error-only:` with no reason after it -- a marker nobody can read spells `forgot` in the vocabulary of `decided`")
            else()
                set(sawVerdictHalfRead TRUE)
                list(APPEND violations
                     "${shownScript}:${scriptLine}: tests a captured output for `CMake Error` and not for `CMake Warning`, so a sub-run that merely WARNS is scored a clean pass here while ctest would refuse it")
            endif()
        endif()

        set(pendingReason "")
        set(sawMarker FALSE)
    endwhile()
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
    if(sawVerdictHalfRead)
    message("The failure signal is TWO words. A harness that runs a check as a sub-process")
    message("reads that output itself -- ctest never sees it -- so it has to spell the whole")
    message("pattern or a sub-run that merely WARNS is scored a clean pass:")
    message("")
    message("    if(combined MATCHES \"CMake Error|CMake Warning\")")
    message("")
    message("A site that reads the error word for some OTHER reason -- a case table spelling")
    message("it to mean `this row expects acceptance`, or a probe asking whether one specific")
    message("`message(FATAL_ERROR)` arm fired -- says so instead, in the comment block")
    message("immediately above it:")
    message("")
    message("    # verdict-error-only: <why a warning is the wrong answer here, not a stricter one>")
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
# The same argument for pass 5. Should its scan stop recognising the shape it reads --
# a reformat, a helper that moves the test off one line -- every harness in the tree
# passes it while nothing is checked, and the summary below looks exactly the same.
# BELOW the violations report, like the control that follows it, so a tree that HAS
# findings reports them rather than dying on a control which is also true of it.
if(verdictSites EQUAL 0)
    message(FATAL_ERROR
        "pass 5 found no compliant verdict reader in scripts/*.cmake at all; every selftest "
        "harness here reads one, so this scan has stopped recognising the shape and has "
        "vouched for all of them without reading any")
endif()

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
    "cmake/Packaging.cmake; plus ${verdictSites} sub-run verdict reader(s) spelling both "
    "halves of the signal and ${verdictExceptions} reading the error word for a stated "
    "reason that is not a verdict")
