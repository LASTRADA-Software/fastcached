# SPDX-License-Identifier: Apache-2.0
# Packaging (CPack)
#
# Generators:
#   - Windows: WIX (MSI) — installs fastcached.exe + fastcache-cc.exe and
#              registers fastcached as an auto-start service
#   - Linux  : TGZ + DEB + RPM — installs both binaries plus the systemd units
#   - macOS  : TGZ + productbuild (.pkg) — installs both binaries under
#              /opt/fastcached, puts them on PATH, and offers the launchd
#              registration as installer choices
#
# The service-integration assets themselves (units, sysusers.d, tmpfiles.d, the
# example config, the launchd installer scripts, the WiX fragment) live under
# packaging/ and are installed by packaging/CMakeLists.txt, which exports the
# variables consumed below. This file only describes how the generators wrap
# what is already installed.
#
# Linux and Windows ship a single Runtime component: the FastCache/CowTree
# static libraries are deliberately not installed (see the comment in the
# top-level CMakeLists.txt), so there is no SDK component to separate out.
# macOS additionally splits the two launchd registrations into their own
# components, because CPack maps one component to one installer choice and the
# two are alternatives rather than both-or-neither.

include(GNUInstallDirs)

# --- Common metadata -------------------------------------------------------
# Sourced from what the project already defines — project() and
# cmake/Version.cmake — rather than restating the same strings here.

set(CPACK_PACKAGE_NAME                "fastcached")
set(CPACK_PACKAGE_VERSION             "${FASTCACHED_VERSION}")
set(CPACK_PACKAGE_VENDOR              "LASTRADA Software")
set(CPACK_PACKAGE_HOMEPAGE_URL        "https://github.com/LASTRADA-Software/fastcached")
set(CPACK_PACKAGE_CONTACT             "Christian Parpart <christian@parpart.family>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_RESOURCE_FILE_README        "${CMAKE_SOURCE_DIR}/README.md")
# WiX only accepts a .txt or .rtf license and rejects an extensionless path
# outright ("unsupported WiX License file extension ''"), so the repository's
# LICENSE is copied to a .txt alongside the build rather than renamed in the
# tree. Harmless for the archive/DEB/RPM generators, which take any path.
configure_file("${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
set(CPACK_RESOURCE_FILE_LICENSE       "${CMAKE_BINARY_DIR}/LICENSE.txt")
set(CPACK_PACKAGE_INSTALL_DIRECTORY   "fastcached")
set(CPACK_PACKAGE_FILE_NAME           "fastcached-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_VERBATIM_VARIABLES          ON)
set(CPACK_STRIP_FILES                 ON)

# --- macOS signing / notarization knobs ------------------------------------
# All empty by default: an unsigned package still builds and installs, so forks
# and pull-request builds (which never receive the repository secrets) are not
# broken by their absence. CI fills these in from secrets.
#
# The identity strings are the certificate common names as `security
# find-identity -v` prints them, e.g.
#   Developer ID Application: Christian Parpart (6T525MU9UR)
#   Developer ID Installer: Christian Parpart (6T525MU9UR)
set(FASTCACHED_MACOS_SIGN_IDENTITY_APP "" CACHE STRING
    "Developer ID Application identity: signs the Mach-O binaries and the .dmg")
set(FASTCACHED_MACOS_SIGN_IDENTITY_PKG "" CACHE STRING
    "Developer ID Installer identity: signs the .pkg (a different certificate type)")
set(FASTCACHED_MACOS_NOTARY_PROFILE "fastcached-notary" CACHE STRING
    "notarytool keychain profile (see `xcrun notarytool store-credentials`)")
option(FASTCACHED_MACOS_NOTARIZE "Submit the macOS artifacts to Apple's notary service" OFF)
option(FASTCACHED_MACOS_BUILD_DMG "Also wrap the macOS .pkg in a .dmg" ON)

# Apple rejects an unsigned or ad-hoc-signed submission — but only after the
# multi-minute round trip to the notary service. Fail here instead, where the
# mistake costs nothing.
if(FASTCACHED_MACOS_NOTARIZE AND (NOT FASTCACHED_MACOS_SIGN_IDENTITY_APP OR NOT FASTCACHED_MACOS_SIGN_IDENTITY_PKG))
    message(FATAL_ERROR
        "FASTCACHED_MACOS_NOTARIZE needs both signing identities: "
        "FASTCACHED_MACOS_SIGN_IDENTITY_APP (Developer ID Application, for the binaries and the .dmg) "
        "and FASTCACHED_MACOS_SIGN_IDENTITY_PKG (Developer ID Installer, for the .pkg). "
        "Apple rejects unsigned submissions.")
endif()

# --- Per-generator selection ----------------------------------------------

if(WIN32)
    if(NOT CPACK_GENERATOR)
        set(CPACK_GENERATOR "WIX")
    endif()
elseif(UNIX AND NOT APPLE)
    if(NOT CPACK_GENERATOR)
        set(CPACK_GENERATOR "TGZ;DEB;RPM")
    endif()

    # Root the payload at / rather than the conventional /usr, because /etc
    # cannot live under a /usr prefix and systemd will not read config from
    # /usr/etc/fastcached. Every prefix-relative destination therefore spells
    # its own `usr/` — see FASTCACHED_INSTALL_* in the top-level CMakeLists.txt
    # and the asset table in packaging/CMakeLists.txt.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/")
elseif(APPLE AND FASTCACHED_PACKAGE_ROOT_PREFIX)
    if(NOT CPACK_GENERATOR)
        set(CPACK_GENERATOR "TGZ;productbuild")
    endif()

    # Same reasoning as Linux, plus a constraint of the generator: CPack always
    # invokes `pkgbuild --install-location /`, so the only way to place a file is
    # to spell its absolute path as a prefix-relative destination. Pointing this
    # at /opt/fastcached instead would appear to work for the binaries and then
    # write /etc/paths.d and /Library/LaunchDaemons straight onto the *build
    # host*, since an absolute install(DESTINATION) escapes the staging tree.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/")
else()
    if(NOT CPACK_GENERATOR)
        set(CPACK_GENERATOR "TGZ")
    endif()
endif()

if(APPLE AND FASTCACHED_PACKAGE_ROOT_PREFIX)
    set(CPACK_COMPONENTS_ALL Runtime LaunchAgent LaunchDaemon)
else()
    set(CPACK_COMPONENTS_ALL Runtime)
endif()

# --- DEB (Debian / Ubuntu) -------------------------------------------------

if(UNIX AND NOT APPLE)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_DEBIAN_PACKAGE_SECTION    "database")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE   "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_FILE_NAME          "DEB-DEFAULT")

    # Runtime dependencies are DERIVED from the linked ELF, never hand-listed.
    # The set genuinely varies with how the build was configured: yaml-cpp,
    # zstd and lz4 are CPM-vendored statically unless a system package happens
    # to be installed, and OpenSSL is a real shared dependency only when
    # FASTCACHED_ENABLE_TLS=ON. A hard-coded list would silently rot.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

    # systemd ships the sysusers/tmpfiles tooling the maintainer scripts use.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "systemd")

    # dpkg reads the architecture from the build host. CMAKE_SYSTEM_PROCESSOR
    # is the wrong spelling for it (x86_64 vs amd64, aarch64 vs arm64), so ask
    # dpkg itself when it is available and otherwise leave CPack's own
    # detection in place — an empty Architecture field produces a package dpkg
    # refuses to install.
    find_program(DPKG_EXECUTABLE dpkg)
    if(DPKG_EXECUTABLE)
        execute_process(COMMAND "${DPKG_EXECUTABLE}" --print-architecture
            OUTPUT_VARIABLE _deb_arch
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
    if(NOT _deb_arch)
        # No dpkg on the build host (e.g. building the .deb from Fedora).
        # Translate the few names that actually differ; anything else already
        # matches Debian's spelling.
        set(_arch_map "x86_64=amd64" "aarch64=arm64" "armv7l=armhf" "i686=i386")
        set(_deb_arch "${CMAKE_SYSTEM_PROCESSOR}")
        foreach(_entry IN LISTS _arch_map)
            string(REPLACE "=" ";" _pair "${_entry}")
            list(GET _pair 0 _from)
            list(GET _pair 1 _to)
            if(CMAKE_SYSTEM_PROCESSOR STREQUAL _from)
                set(_deb_arch "${_to}")
                break()
            endif()
        endforeach()
    endif()
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_deb_arch}")

    # Maintainer scripts, plus a generated `conffiles` listing every config
    # file so dpkg preserves local edits across upgrades. There is no
    # CPACK_DEBIAN_PACKAGE_CONFFILES variable — the control file has to be
    # written out and shipped through CONTROL_EXTRA like the scriptlets.
    if(FASTCACHED_SCRIPTLET_DIR)
        set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
            "${FASTCACHED_SCRIPTLET_DIR}/deb/postinst"
            "${FASTCACHED_SCRIPTLET_DIR}/deb/prerm"
            "${FASTCACHED_SCRIPTLET_DIR}/deb/postrm"
        )

        if(FASTCACHED_PACKAGE_CONFIG_FILES)
            string(REPLACE ";" "\n" _conffiles "${FASTCACHED_PACKAGE_CONFIG_FILES}")
            file(WRITE "${FASTCACHED_SCRIPTLET_DIR}/deb/conffiles" "${_conffiles}\n")
            list(APPEND CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
                "${FASTCACHED_SCRIPTLET_DIR}/deb/conffiles")
        endif()
    endif()
endif()

# --- RPM (Fedora / RHEL / openSUSE) ----------------------------------------

if(UNIX AND NOT APPLE)
    set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")
    set(CPACK_RPM_PACKAGE_GROUP   "Applications/Databases")
    set(CPACK_RPM_PACKAGE_URL     "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_RPM_PACKAGE_VENDOR  "${CPACK_PACKAGE_VENDOR}")
    set(CPACK_RPM_FILE_NAME       "RPM-DEFAULT")

    # Same reasoning as SHLIBDEPS above: let rpmbuild derive the dependencies.
    set(CPACK_RPM_PACKAGE_AUTOREQ ON)
    set(CPACK_RPM_PACKAGE_REQUIRES "systemd")

    if(FASTCACHED_SCRIPTLET_DIR)
        set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE   "${FASTCACHED_SCRIPTLET_DIR}/rpm/post")
        set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE  "${FASTCACHED_SCRIPTLET_DIR}/rpm/preun")
        set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE "${FASTCACHED_SCRIPTLET_DIR}/rpm/postun")
    endif()

    # %config(noreplace): keep the admin's edited YAML on upgrade, dropping the
    # new default alongside as .rpmnew.
    if(FASTCACHED_PACKAGE_CONFIG_FILES)
        set(_rpm_user_filelist "")
        foreach(_config IN LISTS FASTCACHED_PACKAGE_CONFIG_FILES)
            list(APPEND _rpm_user_filelist "%config(noreplace) ${_config}")
        endforeach()
        set(CPACK_RPM_USER_FILELIST "${_rpm_user_filelist}")
    endif()

    # These directories belong to the filesystem/systemd packages. Without the
    # exclusion the generated RPM claims ownership of them and conflicts on
    # install.
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
        "/etc"
        "/usr/lib"
        "/usr/lib/systemd"
        "/usr/lib/systemd/system"
        "/usr/lib/systemd/user"
        "/usr/lib/sysusers.d"
        "/usr/lib/tmpfiles.d"
        "/usr/share/doc"
    )
endif()

# --- WiX (Windows MSI) -----------------------------------------------------

if(WIN32)
    # Stable upgrade GUID. NEVER regenerate — without a stable GUID,
    # consecutive installer versions cannot upgrade each other in place.
    # (Minted for fastcached; it must not be shared with any other product,
    # or that product's installer would treat fastcached as an upgrade of it.)
    set(CPACK_WIX_UPGRADE_GUID "581D183D-47A3-44FC-97E9-4B5491E792D8")

    # WiX 4+ uses the unified `wix.exe` binary. CMake's CPack WIX module accepts
    # only the values "3" or "4" — WiX 5 is API-compatible with WiX 4 and is
    # driven through the same `wix.exe` interface, so we keep this at 4.
    set(CPACK_WIX_VERSION 4)
    if(NOT CPACK_WIX_ROOT)
        foreach(_root "C:/Program Files/WiX Toolset v5.0"
                      "C:/Program Files/WiX Toolset v4.0")
            if(EXISTS "${_root}/bin/wix.exe")
                set(CPACK_WIX_ROOT "${_root}")
                break()
            endif()
        endforeach()
    endif()

    set(CPACK_WIX_PROGRAM_MENU_FOLDER "fastcached")
    set(CPACK_WIX_PROPERTY_ARPHELPLINK     "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")

    # No Start Menu shortcuts: both binaries are console programs (a daemon and
    # a compiler launcher) invoked from a shell or by the service manager.
    set(CPACK_PACKAGE_EXECUTABLES "")

    # Registers fastcached as an auto-start service and starts it unless the
    # user opted out. See packaging/windows/service-actions.xml.
    if(FASTCACHED_WIX_PATCH_FILE)
        set(CPACK_WIX_PATCH_FILE "${FASTCACHED_WIX_PATCH_FILE}")
    endif()

    # Pull in the redistributable VC++ runtime so the MSI can land on a clean
    # machine without a separate vcredist install. UCRT is on by default.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT Runtime)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)
endif()

# --- productbuild (macOS .pkg) ---------------------------------------------

if(APPLE AND FASTCACHED_PACKAGE_ROOT_PREFIX)
    # Mint once, never change: macOS keys package receipts on this, and the
    # uninstaller finds what to `pkgutil --forget` by matching the prefix. Same
    # discipline as CPACK_WIX_UPGRADE_GUID above. Defined in the top-level
    # CMakeLists.txt, which is the only scope both this file and
    # packaging/CMakeLists.txt can read it from.
    if(NOT FASTCACHED_MACOS_BUNDLE_ID)
        message(FATAL_ERROR
            "FASTCACHED_MACOS_BUNDLE_ID is empty. CPACK_PRODUCTBUILD_IDENTIFIER would fall back to "
            "a generated identifier, the package receipts would not carry the prefix the uninstaller "
            "greps for, and `fastcached-uninstall` would silently leave macOS believing fastcached "
            "is still installed.")
    endif()
    set(CPACK_PRODUCTBUILD_IDENTIFIER "${FASTCACHED_MACOS_BUNDLE_ID}")

    # Which locations the installer offers. Stated explicitly because
    # cmake_minimum_required(3.28) leaves CMP0161 unset, and CPack then warns.
    #
    # DOMAINS_USER stays FALSE deliberately: it is not merely unhelpful for a
    # payload rooted at /, it makes the whole install run as the invoking user
    # instead of root, so every postinstall assumption (writing /etc/paths.d,
    # creating the service account, symlinking into /usr/local/bin) quietly
    # fails while the installer still reports success.
    set(CPACK_PRODUCTBUILD_DOMAINS          TRUE)
    set(CPACK_PRODUCTBUILD_DOMAINS_ANYWHERE FALSE)
    set(CPACK_PRODUCTBUILD_DOMAINS_USER     FALSE)
    set(CPACK_PRODUCTBUILD_DOMAINS_ROOT     TRUE)

    # Installer panes. productbuild rejects any resource that is not .rtfd,
    # .rtf, .html or .txt, so the project's README.md — fine for every other
    # generator — has to be replaced here rather than reused. These two also do
    # real work: the welcome pane explains that the service choices are
    # alternatives, and the read-me pane carries the "open a new terminal"
    # caveat and the uninstaller invocation, both of which a user otherwise has
    # no way to discover.
    set(CPACK_RESOURCE_FILE_WELCOME "${CMAKE_SOURCE_DIR}/packaging/macos/welcome.html")
    set(CPACK_RESOURCE_FILE_README  "${CMAKE_SOURCE_DIR}/packaging/macos/readme.html")

    # <COMPONENT> in these variable names is the component name uppercased.
    if(FASTCACHED_MACOS_SCRIPT_DIR)
        set(CPACK_POSTFLIGHT_RUNTIME_SCRIPT      "${FASTCACHED_MACOS_SCRIPT_DIR}/postinstall-runtime")
        set(CPACK_POSTFLIGHT_LAUNCHAGENT_SCRIPT  "${FASTCACHED_MACOS_SCRIPT_DIR}/postinstall-launchagent")
        set(CPACK_POSTFLIGHT_LAUNCHDAEMON_SCRIPT "${FASTCACHED_MACOS_SCRIPT_DIR}/postinstall-launchdaemon")
    endif()

    # Signing. Two *different* certificate types are involved and mixing them up
    # is the usual failure: a "Developer ID Application" certificate signs
    # Mach-O binaries and disk images, while only a "Developer ID Installer"
    # certificate can sign a .pkg. Empty by default so a fork or a PR build
    # still produces an installable (if unsigned) package.
    if(FASTCACHED_MACOS_SIGN_IDENTITY_PKG)
        set(CPACK_PKGBUILD_IDENTITY_NAME     "${FASTCACHED_MACOS_SIGN_IDENTITY_PKG}")
        set(CPACK_PRODUCTBUILD_IDENTITY_NAME "${FASTCACHED_MACOS_SIGN_IDENTITY_PKG}")
    endif()

    # The two hook scripts run as `cmake -P`, which only sees CPACK_-prefixed
    # variables, so the knobs are re-exported under that prefix.
    set(CPACK_FASTCACHED_SIGN_IDENTITY_APP "${FASTCACHED_MACOS_SIGN_IDENTITY_APP}")
    set(CPACK_FASTCACHED_MACOS_PREFIX      "${FASTCACHED_MACOS_PREFIX}")
    set(CPACK_FASTCACHED_NOTARIZE          "${FASTCACHED_MACOS_NOTARIZE}")
    set(CPACK_FASTCACHED_NOTARY_PROFILE    "${FASTCACHED_MACOS_NOTARY_PROFILE}")
    set(CPACK_FASTCACHED_BUILD_DMG         "${FASTCACHED_MACOS_BUILD_DMG}")
    # CPack builds packages in a staging directory and copies them back
    # afterwards; a .dmg this hook creates is not part of that copy, so it must
    # be written to its final home directly.
    set(CPACK_FASTCACHED_OUTPUT_DIRECTORY  "${CMAKE_BINARY_DIR}")

    # Signing must happen after CPACK_STRIP_FILES and before pkgbuild seals the
    # payload — the pre-build hook is the only point that satisfies both.
    set(CPACK_PRE_BUILD_SCRIPTS  "${CMAKE_SOURCE_DIR}/cmake/MacOSSignBinaries.cmake")
    set(CPACK_POST_BUILD_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake/MacOSNotarizePkg.cmake")
endif()

include(CPack)

# Component declarations must come after include(CPack).
cpack_add_component(Runtime
    DISPLAY_NAME "fastcached"
    DESCRIPTION  "The fastcached daemon and the fastcache-cc compiler launcher."
    REQUIRED
)

if(APPLE AND FASTCACHED_PACKAGE_ROOT_PREFIX)
    # The two launchd registrations are alternatives: both bind 127.0.0.1:6674
    # and fastcached has no unix-socket endpoint to fall back on. productbuild
    # cannot express mutual exclusion — CPack only ever generates && / ||
    # dependency expressions for a choice's `selected` attribute — so the
    # postinstall scripts arbitrate at install time (the daemon wins) and the
    # descriptions below tell the user what the checkboxes really mean.
    cpack_add_component(LaunchAgent
        DISPLAY_NAME "Start at login (recommended)"
        DESCRIPTION  "Run fastcached as you, starting at login. Best for a development machine. Ignored if the system-wide service below is also selected."
    )
    # DISABLED renders the choice unchecked (start_selected="false"), so a
    # click-through install gets the per-user agent rather than a root daemon.
    cpack_add_component(LaunchDaemon
        DISPLAY_NAME "Start at boot, system-wide"
        DESCRIPTION  "Run fastcached as a dedicated service account from boot, shared by every user. Creates the _fastcached account. Takes precedence over the per-user option above."
        DISABLED
    )
endif()
