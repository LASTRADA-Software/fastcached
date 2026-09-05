# SPDX-License-Identifier: Apache-2.0
#
# CPACK_PRE_BUILD_SCRIPTS hook: code-sign the staged Mach-O binaries.
#
# This runs in the one window where signing is possible at all: after CPack has
# installed the payload into its staging tree (and applied CPACK_STRIP_FILES),
# and before pkgbuild seals it. Signing earlier is pointless — `strip` rewrites
# the binary and invalidates the signature, and on arm64 an invalid signature is
# not a warning but a refusal to execute, so the package would install a daemon
# that dies with "Killed: 9".
#
# Runs as a `cmake -P` script, so only CPACK_-prefixed variables are visible.

# Script mode sets no policies, so every policy-gated construct in this file
# would mean different things depending on which CMake ran it -- `if(x IN_LIST y)`
# errors out on 3.28 and answers on 4.x. CPack runs this hook the same way ctest
# runs a `cmake -P` check, so the same declaration applies, and #680 put it inside
# `script-check-signals`'s pass 3 rather than leaving it to be remembered.
cmake_minimum_required(VERSION 3.28)

if(NOT CPACK_FASTCACHED_SIGN_IDENTITY_APP)
    return()
endif()

# The staged tree is laid out per component; the executables all belong to
# Runtime, but glob both shapes so a future component split does not silently
# stop signing something. The prefix comes from FASTCACHED_MACOS_PREFIX rather
# than being spelled here: hardcoding `opt/fastcached` made a relocated build
# glob a path that does not exist, and the FATAL_ERROR below then turned a
# perfectly valid configuration into an unsignable one.
file(GLOB_RECURSE _binaries
    "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/*/${CPACK_FASTCACHED_MACOS_PREFIX}/bin/fastcache*"
    "${CPACK_TEMPORARY_INSTALL_DIRECTORY}/${CPACK_FASTCACHED_MACOS_PREFIX}/bin/fastcache*"
)

if(NOT _binaries)
    message(FATAL_ERROR
        "MacOSSignBinaries: found nothing to sign under ${CPACK_TEMPORARY_INSTALL_DIRECTORY}. "
        "Signing was requested, so shipping an unsigned package here would be worse than failing.")
endif()

foreach(_binary IN LISTS _binaries)
    # Skip anything that is not a Mach-O image — the uninstaller is a shell script,
    # and codesign would reject it.
    #
    # Asked of the FILE rather than of its name, and that is the point. This used to
    # be an allow-list of binary names, which meant every new executable was silently
    # left unsigned: the glob picked it up, the filter dropped it, nothing failed, and
    # the first sign of trouble was Apple's notary service rejecting the whole archive
    # with "The binary is not signed with a valid Developer ID certificate" — one
    # release cycle later, on a job most changes never run. Adding an app is supposed
    # to be adding a row to the app table, and this was a second place that had to be
    # edited in step with it. Now it is not.
    execute_process(COMMAND /usr/bin/file --brief "${_binary}"
                    OUTPUT_VARIABLE _kind
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    COMMAND_ERROR_IS_FATAL ANY)
    if(NOT _kind MATCHES "Mach-O")
        message(STATUS "MacOSSignBinaries: skipping non-Mach-O ${_binary} (${_kind})")
        continue()
    endif()

    # Extended attributes left by the build (resource forks, quarantine flags)
    # make codesign fail with "resource fork, Finder information, or similar
    # detritus not allowed".
    execute_process(COMMAND xattr -c "${_binary}" COMMAND_ERROR_IS_FATAL ANY)

    # --options=runtime enables the hardened runtime and --timestamp requests a
    # secure timestamp; notarization rejects a submission missing either. No
    # entitlements file: fastcached needs neither JIT nor relaxed library
    # validation, and entitlements it does not need would only widen its
    # attack surface.
    message(STATUS "Signing ${_binary}")
    execute_process(
        COMMAND codesign --force --timestamp --options=runtime
                --sign "${CPACK_FASTCACHED_SIGN_IDENTITY_APP}" "${_binary}"
        COMMAND_ERROR_IS_FATAL ANY
    )
    execute_process(
        COMMAND codesign --verify --strict --verbose=2 "${_binary}"
        COMMAND_ERROR_IS_FATAL ANY
    )
endforeach()
