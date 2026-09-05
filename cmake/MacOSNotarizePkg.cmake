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

function(fastcached_notarize artifact)
    if(NOT CPACK_FASTCACHED_NOTARIZE)
        return()
    endif()

    # Bounded, and retried only on a STALL. An unbounded `--wait` does not avoid
    # an ending, it hands the ending to the runner, which answers after thirty
    # minutes with "The action 'Package' has timed out" and no statement of which
    # operation did not finish (#376). That is the rule this file's own `hdiutil`
    # block a few lines down already applied; the call sitting next to it did not
    # get it.
    #
    # 600 s against a documented 1-5 minutes, two attempts: generous enough that a
    # slow-but-working Apple is never cut off, and 20 minutes of worst case still
    # leaves the 30-minute step budget room to report rather than be killed.
    #
    # **A stall is retried; a REJECTION is not.** Apple answering "Invalid" is a
    # verdict, and retrying a verdict just spends the budget to be told the same
    # thing. `_rc` separates them: non-zero means no answer arrived, zero means one
    # did and `status` decides it.
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
    set(_notaryTimeout 600)
    set(_rc "not attempted")

    foreach(_attempt RANGE 1 ${_notaryAttempts})
        message(STATUS
            "Notarizing ${artifact} (attempt ${_attempt}/${_notaryAttempts}, "
            "waits for Apple, typically 1-5 min)")
        execute_process(
            COMMAND xcrun notarytool submit "${artifact}"
                    --keychain-profile "${CPACK_FASTCACHED_NOTARY_PROFILE}"
                    --wait --output-format json
            TIMEOUT ${_notaryTimeout}
            OUTPUT_VARIABLE _out
            RESULT_VARIABLE _rc
        )
        if(_rc STREQUAL "0")
            break()
        endif()
        message(WARNING
            "notarytool submit for ${artifact} did not answer (${_rc}); retrying")
    endforeach()

    # No answer at all, across every attempt. Distinct from a refusal below, and
    # named separately because the operator actions differ: this one is Apple or
    # the network, that one is the artifact.
    if(NOT _rc STREQUAL "0")
        message(FATAL_ERROR
            "notarytool submit for ${artifact} did not answer within "
            "${_notaryTimeout}s across ${_notaryAttempts} attempt(s): ${_rc}")
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
        message(FATAL_ERROR "Notarization of ${artifact} was not accepted: ${_out}")
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
            message(FATAL_ERROR "stapler ${_step} failed for ${artifact}: ${_stapleRc}")
        endif()
    endforeach()
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
