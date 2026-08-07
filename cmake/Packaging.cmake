# SPDX-License-Identifier: Apache-2.0
# Packaging (CPack)
#
# Generators:
#   - Windows: WIX (MSI) — installs fastcached.exe + fastcache-cc.exe and
#              registers fastcached as an auto-start service
#   - Linux  : TGZ + DEB + RPM — installs both binaries plus the systemd units
#   - macOS  : TGZ (no service integration; launchd is not wired up)
#
# The service-integration assets themselves (units, sysusers.d, tmpfiles.d, the
# example config, the WiX fragment) live under packaging/ and are installed by
# packaging/CMakeLists.txt, which exports the variables consumed below. This
# file only describes how the generators wrap what is already installed.
#
# Everything ships in a single Runtime component: the FastCache/CowTree static
# libraries are deliberately not installed (see the comment in the top-level
# CMakeLists.txt), so there is no SDK component to separate out.

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
else()
    if(NOT CPACK_GENERATOR)
        set(CPACK_GENERATOR "TGZ")
    endif()
endif()

set(CPACK_COMPONENTS_ALL Runtime)

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

include(CPack)

# Component declarations must come after include(CPack).
cpack_add_component(Runtime
    DISPLAY_NAME "fastcached"
    DESCRIPTION  "The fastcached daemon and the fastcache-cc compiler launcher."
    REQUIRED
)
