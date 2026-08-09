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
    # The uninstaller is a shell script, not Mach-O; codesign would reject it.
    if(NOT _binary MATCHES "/(fastcached|fastcache-cc)$")
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
