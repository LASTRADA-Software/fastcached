# SPDX-License-Identifier: Apache-2.0
#
# CPACK_POST_BUILD_SCRIPTS hook: notarize the .pkg, then wrap it in a .dmg and
# notarize that too.
#
# Runs after the packages exist, with CPACK_PACKAGE_FILES holding their absolute
# paths. Runs as a `cmake -P` script, so only CPACK_-prefixed variables are
# visible.
#
# Order is deliberate and not interchangeable: the .pkg is stapled *before* it
# is wrapped. A notarization ticket stapled to the outer disk image does not
# travel with a .pkg that the user drags out of it, so an offline install of the
# extracted package would fall back to an online Gatekeeper check — exactly the
# "Apple could not verify..." dialog this whole pipeline exists to avoid.

# ----------------------------------------------------------------------------

# Submit @artifact to Apple, wait for the verdict, then staple the ticket.
# Script mode sets no policies, so every policy-gated construct in this file
# would mean different things depending on which CMake ran it -- `if(x IN_LIST y)`
# errors out on 3.28 and answers on 4.x. CPack runs this hook the same way ctest
# runs a `cmake -P` check, so the same declaration applies, and #680 put it inside
# `script-check-signals`'s pass 3 rather than leaving it to be remembered.
cmake_minimum_required(VERSION 3.28)

# ----------------------------------------------------------------------------
# Every way a notarization can END, as ROWS rather than as branches (#1156).
#
# Three separate audiences read this and they were previously served by one
# `message(FATAL_ERROR)` each:
#
#   * whoever is reading the step's log, at the point it happens;
#   * the JOB, which has to decide whether this is fatal HERE -- an artefact that
#     ships is a different question from one that never leaves the runner;
#   * the annotation on the job summary, which is the only line most people see,
#     and which said `Process completed with exit code 1` while the last words in
#     the log were `CPack Error: Cannot initialize the generator TGZ` -- a
#     consequence of aborting this hook, and a sentence about a tar generator for
#     a failure that was Apple's notary service not answering.
#
# So a verdict is RECORDED as well as reported, one line per artefact, and the
# job classifies from the record. That record is also what makes the ordering
# readable at all: `message(STATUS)` goes to stdout and `message(WARNING)` to
# stderr, CMake block-buffers the first, and the two arrive interleaved -- in the
# run that produced #1156 the log shows `attempt 2/2` starting AFTER the fatal
# error that ended the script. The record is written in the order things
# happened.
#
#   token      what the record carries, and what the job's classifier reads
#   fatality   Never  -- reported, never fails the build
#              Policy -- fatal iff CPACK_FASTCACHED_NOTARY_STALL_IS_FATAL
#              Always -- fatal wherever it happens
#   sentence   the finding, naming APPLE where Apple is the subject
#
# **Only the STALL is `Policy`, and that is the whole discrimination.** A stall is
# Apple's queue not answering, and it says nothing about the artefact; a rejection
# is Apple having looked and refused, which is a fact about the artefact and is
# fatal on every ref. Softening the second would be softening the one outcome that
# means the package is wrong.
#
# token|fatality|sentence
set(FASTCACHED_NOTARY_VERDICTS
    "disabled|Never|notarization was not asked for on this build"
    "accepted|Never|Apple accepted the submission and the ticket is stapled"
    "stalled|Policy|Apple did not answer the notarization submission within the budget"
    "rejected|Always|Apple answered the notarization submission and the answer was not Accepted"
    "unstapled|Always|Apple accepted the submission and its ticket could not be stapled"
)

# Record @token against @artifact, then report it at the severity its row says.
#
# @param token One of FASTCACHED_NOTARY_VERDICTS' first fields.
# @param artifact The absolute path of the artefact this verdict is about.
# @param detail What was observed; empty when there is nothing to add.
function(fastcached_notary_verdict token artifact detail)
    get_filename_component(_name "${artifact}" NAME)

    # The table is the authority, and a token in no row is a defect in THIS file
    # rather than a notarization outcome -- so it is refused by name instead of
    # being written to a record the job would then have to guess about. The job's
    # classifier refuses an unknown token from the other side for the same reason:
    # a row added here and not there must not pass quietly.
    set(_fatality "")
    set(_sentence "")
    foreach(_row IN LISTS FASTCACHED_NOTARY_VERDICTS)
        string(REPLACE "|" ";" _fields "${_row}")
        list(GET _fields 0 _rowToken)
        if(_rowToken STREQUAL token)
            list(GET _fields 1 _fatality)
            list(GET _fields 2 _sentence)
        endif()
    endforeach()
    if(_fatality STREQUAL "")
        message(FATAL_ERROR
            "notarization verdict '${token}' is in no row of FASTCACHED_NOTARY_VERDICTS")
    endif()

    # Appended, never truncated, because this hook runs once per CPack GENERATOR
    # -- `cpack -G "productbuild;TGZ"` enters it twice -- and the second pass must
    # not erase the first pass's findings. The job removes the file before cpack,
    # so an absent record afterwards means this hook wrote nothing, which is a
    # state the classifier reports rather than one it reads as "fine".
    if(CPACK_FASTCACHED_NOTARY_VERDICT_FILE)
        file(APPEND "${CPACK_FASTCACHED_NOTARY_VERDICT_FILE}" "${token} ${_name}\n")
    endif()

    if(_fatality STREQUAL "Never")
        message(STATUS "${_name}: ${_sentence}")
        return()
    endif()

    # Absence means FATAL. A policy that softens by accident softens the release
    # leg, which is the one place a missing notarization ticket reaches a user --
    # so the soft answer has to be asked for, in writing, by a caller that set the
    # variable on purpose.
    set(_stallIsFatal TRUE)
    if(DEFINED CPACK_FASTCACHED_NOTARY_STALL_IS_FATAL AND NOT CPACK_FASTCACHED_NOTARY_STALL_IS_FATAL)
        set(_stallIsFatal FALSE)
    endif()

    # Said here, at the point it happens, and said the same way whether it is
    # fatal: the sentence names Apple, and it names what this failure is NOT, so
    # the CPack generator error printed underneath is not read as the cause.
    set(_report "${_sentence}: ${_name}")
    if(NOT detail STREQUAL "")
        string(APPEND _report " (${detail})")
    endif()
    string(APPEND _report
           ". Everything this build controls -- compiling, signing, packaging -- already"
           " succeeded for this artefact; any CPack generator error printed after this"
           " line is a consequence of abandoning this hook and not a second failure.")

    if(_fatality STREQUAL "Policy" AND NOT _stallIsFatal)
        message(WARNING
            "${_report} Not fatal on this ref: nothing ships from it, so an unnotarized "
            "artefact here reaches nobody. It IS fatal on a tag.")
        return()
    endif()
    message(FATAL_ERROR "${_report}")
endfunction()

function(fastcached_notarize artifact)
    if(NOT CPACK_FASTCACHED_NOTARIZE)
        # Recorded rather than returned silently, so the record is TOTAL: every
        # artefact this hook considered has exactly one row, and "no row" means
        # the hook did not run rather than "notarization was off".
        fastcached_notary_verdict(disabled "${artifact}" "")
        return()
    endif()

    # Bounded. An unbounded `--wait` does not avoid an ending, it hands the ending
    # to the runner, which answers after the step budget with "The action 'Package'
    # has timed out" and no statement of which operation did not finish (#376).
    # That is the rule this file's own `hdiutil` block a few lines down already
    # applied; the call sitting next to it did not get it.
    #
    # ## What the number is a bound ON, and why a longer one is not simply better
    #
    # 600 s is **not** a bound on how long Apple may take -- nothing here can know
    # that. It is a bound on **how long this job holds a runner for a third party
    # before it says so**. Three things push back on raising it:
    #
    #   * the step must still be able to REPORT. Every call in this file is bounded
    #     and names itself, and `timeout-minutes` in the packaging job is set from
    #     their SUM. Raise one of them and that sum has to move with it, or the
    #     runner writes the ending and names no operation, which is #376 again;
    #   * this bound is what separates *Apple is slow* from *the submission is
    #     wedged*. One large enough never to fire retires that distinction, which is
    #     the same objection `.agent/rules/testing.md` records against a budget that
    #     has been widened twice;
    #   * a stalled submission is not LOST. The artefact is uploaded and Apple
    #     finishes it in its own time, so the cheap remedy for a tag is to re-run
    #     the job, which usually meets an answer that is already waiting.
    #
    # ## The budget is per ARTEFACT and spent ACROSS attempts (#1156)
    #
    # It used to be per ATTEMPT, so two attempts meant 1200 s per artefact and 2400 s
    # across the .pkg and the .dmg -- against a 30-minute step budget whose comment
    # claimed the worst case fitted. It did not; the arithmetic had counted one call.
    #
    # Worse, the second attempt cannot help with the failure that produced it.
    # `notarytool submit` UPLOADS and ENQUEUES a new submission: if Apple's queue was
    # too slow to answer in the budget, a fresh submission joins the back of that same
    # queue with less time than the first one had. Measured on the run that filed
    # #1156: attempt 1 timed out at 600 s and attempt 2 timed out at 600 s, twenty
    # minutes spent to be told the same thing twice.
    #
    # A retry only helps a failure that returns QUICKLY -- a dropped upload, a
    # transient auth error -- and those leave budget behind by construction. So one
    # number expresses both rules and no branch is needed: **retry a fast failure,
    # never a slow one.** A stall consumes the budget and there is nothing left to
    # retry with.
    #
    # **A REJECTION is not retried either.** Apple answering "Invalid" is a verdict,
    # and retrying a verdict just spends the budget to be told the same thing. `_rc`
    # separates them: non-zero means no answer arrived, zero means one did and
    # `status` decides it.
    #
    # `xcrun` EXECs the tool rather than forking it -- measured on the runner,
    # `macos-15`/Xcode 26.3.0, same pid for `xcrun` and its target -- so
    # `execute_process`'s child IS `notarytool` and a TIMEOUT ends the real work
    # rather than a wrapper. That mattered: `execute_process` waits for stdout EOF
    # and not for the child to exit, so had `xcrun` forked and exited, a TIMEOUT
    # would have left `notarytool` running and reported a SUCCESSFUL submission as
    # a timeout. **That measurement is of this image**; if this job moves to
    # another runner it is worth re-asking rather than assuming.
    set(_notaryAttempts 2)
    set(_notaryBudget 600)

    # An injected seam, and the only thing it is for is `check-notarize-retry.sh`.
    #
    # The property this budget expresses -- a SLOW failure consumes it and leaves
    # nothing to retry with, where a FAST one leaves most of it -- cannot be observed
    # by any test willing to wait 600 s, so the guard that drives this function shrinks
    # it. That guard drives the REAL file on purpose; a copy with the number edited
    # would be a restatement of this code rather than this code.
    #
    # `cmake/Packaging.cmake` does not export it, so no configured build can carry it,
    # and every attempt prints the budget it is spending -- a shrunk one is visible in
    # the log rather than silent.
    if(DEFINED CPACK_FASTCACHED_NOTARY_BUDGET_SECONDS)
        set(_notaryBudget "${CPACK_FASTCACHED_NOTARY_BUDGET_SECONDS}")
    endif()

    # A retry with less than this left cannot upload the artefact and wait for an
    # answer, so it is not made: it would spend a slot to produce the same timeout
    # with a shorter number in it. DERIVED from the budget rather than a constant
    # beside it, so the two cannot be changed apart -- a fixed 30 against a shrunk
    # budget would skip the FIRST attempt and the guard above would be testing that
    # instead of what it says.
    math(EXPR _notaryMinAttempt "${_notaryBudget} / 20")
    if(_notaryMinAttempt LESS 1)
        set(_notaryMinAttempt 1)
    endif()
    set(_notarySpent 0)
    set(_rc "not attempted")

    foreach(_attempt RANGE 1 ${_notaryAttempts})
        math(EXPR _notaryLeft "${_notaryBudget} - ${_notarySpent}")
        if(_notaryLeft LESS ${_notaryMinAttempt})
            break()
        endif()
        message(STATUS
            "Notarizing ${artifact} (attempt ${_attempt}/${_notaryAttempts}, up to "
            "${_notaryLeft}s of a ${_notaryBudget}s budget for this artefact; waits for "
            "Apple, typically 1-5 min)")
        string(TIMESTAMP _notaryBegan "%s" UTC)
        execute_process(
            COMMAND xcrun notarytool submit "${artifact}"
                    --keychain-profile "${CPACK_FASTCACHED_NOTARY_PROFILE}"
                    --wait --output-format json
            TIMEOUT ${_notaryLeft}
            OUTPUT_VARIABLE _out
            RESULT_VARIABLE _rc
        )
        string(TIMESTAMP _notaryEnded "%s" UTC)
        math(EXPR _notarySpent "${_notarySpent} + ${_notaryEnded} - ${_notaryBegan}")
        if(_rc STREQUAL "0")
            break()
        endif()
        message(WARNING
            "notarytool submit for ${artifact} did not answer (${_rc}) after "
            "${_notarySpent}s of a ${_notaryBudget}s budget")
    endforeach()

    # No answer at all. Distinct from a refusal below, and named separately because
    # the operator actions differ: this one is Apple or the network, that one is the
    # artifact. Which of those two is fatal HERE is the job's decision, taken from
    # the row's fatality; this function states what happened and returns.
    if(NOT _rc STREQUAL "0")
        fastcached_notary_verdict(stalled "${artifact}"
                                  "notarytool submit: ${_rc}, after ${_notarySpent}s of ${_notaryBudget}s")
        # Reached only when a stall is NOT fatal on this ref. Stapling an artefact
        # Apple never answered about would fail anyway, and failing there would name
        # `stapler` for a defect that is the notary service's.
        return()
    endif()

    if(NOT _out MATCHES "\"status\":\"?Accepted")
        # The submission id is the only way to find out *why* Apple refused;
        # without the log a rejection is undebuggable, so fetch it before
        # failing. Bounded too: a diagnostic that hangs costs the same half hour
        # as the call it was diagnosing, and it must not be able to turn a clear
        # rejection into a timeout.
        if(_out MATCHES "\"id\":\"([0-9a-fA-F-]+)\"")
            execute_process(
                COMMAND xcrun notarytool log "${CMAKE_MATCH_1}"
                        --keychain-profile "${CPACK_FASTCACHED_NOTARY_PROFILE}"
                TIMEOUT 120
                RESULT_VARIABLE _logRc
            )
            if(NOT _logRc STREQUAL "0")
                message(WARNING
                    "could not fetch the notarization log for ${artifact} (${_logRc}); "
                    "the refusal below is reported without it")
            endif()
        endif()
        fastcached_notary_verdict(rejected "${artifact}" "${_out}")
        # Unreachable while `rejected` is an `Always` row, and kept anyway: the
        # fatality lives in a table a later change can edit, and the failure a
        # missing `return()` would produce is stapling an artefact Apple refused.
        return()
    endif()

    # Bounded for the same reason, and reported by name rather than through
    # `COMMAND_ERROR_IS_FATAL ANY`: that spells a timeout and a refusal the same
    # way, and stapling failures are read by whoever is holding a broken artifact.
    foreach(_step IN ITEMS staple validate)
        execute_process(
            COMMAND xcrun stapler ${_step} "${artifact}"
            TIMEOUT 300
            RESULT_VARIABLE _stapleRc
        )
        if(NOT _stapleRc STREQUAL "0")
            fastcached_notary_verdict(unstapled "${artifact}" "stapler ${_step}: ${_stapleRc}")
            return()
        endif()
    endforeach()

    fastcached_notary_verdict(accepted "${artifact}" "")
endfunction()

# ----------------------------------------------------------------------------

foreach(_package IN LISTS CPACK_PACKAGE_FILES)
    if(NOT _package MATCHES "\\.pkg$")
        continue()
    endif()

    fastcached_notarize("${_package}")

    if(NOT CPACK_FASTCACHED_BUILD_DMG)
        continue()
    endif()

    # A disk image holding exactly the .pkg. hdiutil takes a directory, so stage
    # one; the .pkg is copied rather than moved because CPack still owns it and
    # copies it back to the build tree afterwards.
    # NAME_WLE, not NAME_WE: the latter strips from the *first* dot, which turns
    # fastcached-1.2.3-Darwin-arm64.pkg into "fastcached-1".
    get_filename_component(_name "${_package}" NAME_WLE)
    get_filename_component(_dir "${_package}" DIRECTORY)
    set(_stage "${_dir}/dmgroot")
    set(_dmg "${CPACK_FASTCACHED_OUTPUT_DIRECTORY}/${_name}.dmg")

    file(REMOVE_RECURSE "${_stage}")
    file(MAKE_DIRECTORY "${_stage}")
    file(COPY "${_package}" DESTINATION "${_stage}")

    # hdiutil intermittently wedges on GitHub's runners while a background
    # scanner holds the image it has just written (actions/runner-images#7522).
    # It does not fail, it waits — so an unbounded call turns a forty-second
    # step into a job that burns its entire timeout and reports nothing but
    # "The action 'Package' has timed out". The job kills XProtect before cpack,
    # which is three component packages and a round of code signing too early
    # for it to still be dead by the time we get here, and it is launchd-managed
    # so it comes back on demand regardless.
    #
    # Bounding each attempt is what makes the failure legible: a transient hold
    # clears on the retry, and a persistent one fails in minutes with a message
    # naming hdiutil rather than the whole step.
    set(_dmgAttempts 2)
    set(_dmgTimeout 240)
    set(_dmgResult "not attempted")

    foreach(_attempt RANGE 1 ${_dmgAttempts})
        # A wedged attempt can leave a partial image behind, and -ov alone does
        # not always reclaim it.
        file(REMOVE "${_dmg}")
        message(STATUS "Creating ${_dmg} (attempt ${_attempt}/${_dmgAttempts})")
        execute_process(
            COMMAND hdiutil create -volname "fastcached ${CPACK_PACKAGE_VERSION}"
                    -srcfolder "${_stage}" -fs APFS -format UDZO -ov "${_dmg}"
            TIMEOUT ${_dmgTimeout}
            RESULT_VARIABLE _dmgResult
        )
        if(_dmgResult STREQUAL "0")
            break()
        endif()
        message(WARNING "hdiutil create did not finish (${_dmgResult}); retrying")
    endforeach()

    if(NOT _dmgResult STREQUAL "0")
        message(FATAL_ERROR
            "hdiutil create did not produce ${_dmg} within ${_dmgTimeout}s "
            "across ${_dmgAttempts} attempts: ${_dmgResult}")
    endif()

    file(REMOVE_RECURSE "${_stage}")

    if(CPACK_FASTCACHED_SIGN_IDENTITY_APP)
        # No --options=runtime here: a disk image holds no executable code of
        # its own, and the hardened runtime is a property of a running process.
        message(STATUS "Signing ${_dmg}")
        # The fifth unbounded call. `codesign --timestamp` contacts Apple's
        # timestamp authority, so it has the same stall shape as the notarization
        # above and the same consequence if it never returns.
        execute_process(
            COMMAND codesign --force --timestamp
                    --sign "${CPACK_FASTCACHED_SIGN_IDENTITY_APP}" "${_dmg}"
            TIMEOUT 300
            RESULT_VARIABLE _signRc
        )
        if(NOT _signRc STREQUAL "0")
            message(FATAL_ERROR "codesign failed for ${_dmg}: ${_signRc}")
        endif()
    endif()

    fastcached_notarize("${_dmg}")
endforeach()
