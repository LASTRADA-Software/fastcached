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
function(fastcached_notarize artifact)
    if(NOT CPACK_FASTCACHED_NOTARIZE)
        return()
    endif()

    message(STATUS "Notarizing ${artifact} (waits for Apple, typically 1-5 min)")
    execute_process(
        COMMAND xcrun notarytool submit "${artifact}"
                --keychain-profile "${CPACK_FASTCACHED_NOTARY_PROFILE}"
                --wait --output-format json
        OUTPUT_VARIABLE _out
        RESULT_VARIABLE _rc
    )

    if(NOT _rc EQUAL 0 OR NOT _out MATCHES "\"status\":\"?Accepted")
        # The submission id is the only way to find out *why* Apple refused;
        # without the log a rejection is undebuggable, so fetch it before
        # failing.
        if(_out MATCHES "\"id\":\"([0-9a-fA-F-]+)\"")
            execute_process(
                COMMAND xcrun notarytool log "${CMAKE_MATCH_1}"
                        --keychain-profile "${CPACK_FASTCACHED_NOTARY_PROFILE}"
            )
        endif()
        message(FATAL_ERROR "Notarization of ${artifact} was not accepted: ${_out}")
    endif()

    execute_process(COMMAND xcrun stapler staple "${artifact}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND xcrun stapler validate "${artifact}" COMMAND_ERROR_IS_FATAL ANY)
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
    # fastcached-0.0.1-Darwin-arm64.pkg into "fastcached-0".
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
        execute_process(
            COMMAND codesign --force --timestamp
                    --sign "${CPACK_FASTCACHED_SIGN_IDENTITY_APP}" "${_dmg}"
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()

    fastcached_notarize("${_dmg}")
endforeach()
