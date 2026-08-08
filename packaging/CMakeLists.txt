# SPDX-License-Identifier: Apache-2.0
# Platform service-integration assets: systemd units and their supporting
# account/state declarations on Linux, launchd plists and PATH wiring on macOS,
# the WiX service fragment on Windows.
#
# Every installed asset is one row in a table below — source path, install
# destination, kind, installed name, and owning package component. Adding a man
# page, a logrotate snippet, or an AppArmor profile is adding a row, not a new
# install() call, and one loop installs whichever table the platform selected.
#
# `kind` is what makes the table more than a loop:
#   config  -> also exported as the package-manager config-file list, so "which
#              files are configuration" is stated once and reused by the DEB and
#              RPM generators
#   program -> installed with the executable bit set
#   data    -> a plain installed file
#
# `component` is what makes it more than one package: on macOS the installer
# offers the launchd registration as selectable choices, and CPack maps one
# component to one choice.

include(GNUInstallDirs)

# The systemd unit name. Single source of truth: it names the installed unit
# file and is substituted into every DEB/RPM scriptlet.
set(FASTCACHED_SERVICE_NAME "fastcached.service" CACHE STRING "systemd unit name")

# Install destinations, RELATIVE to CMAKE_INSTALL_PREFIX so that CPack (which
# stages by pointing the prefix at a private package root) and DESTDIR builds
# both work without the destination escaping the staging tree.
#
# The packages set CPACK_PACKAGING_INSTALL_PREFIX=/ (see cmake/Packaging.cmake)
# so these resolve to the only locations that work: systemd reads units from
# /usr/lib/systemd, and config must be in /etc — under the conventional /usr
# prefix the config would land at /usr/etc/fastcached, where nothing looks.
# That is also why `usr/` is spelled explicitly below.
#
# usr/lib rather than CMAKE_INSTALL_LIBDIR: the latter resolves to lib64 on
# RPM-based 64-bit systems, where systemd does not look either.
set(FASTCACHED_SYSTEMD_SYSTEM_DIR "usr/lib/systemd/system")
set(FASTCACHED_SYSTEMD_USER_DIR   "usr/lib/systemd/user")
set(FASTCACHED_SYSUSERS_DIR       "usr/lib/sysusers.d")
set(FASTCACHED_TMPFILES_DIR       "usr/lib/tmpfiles.d")
set(FASTCACHED_SYSCONF_DIR        "etc/fastcached")

# macOS identity. All three are substituted into the installer scripts and the
# uninstaller, and the bundle id is additionally the CPack productbuild
# identifier — the prefix of every receipt the uninstaller forgets.
#
# The bundle id must match LaunchdLabelPrefix + the lowercased default service
# name in src/FastCache/Platform/ServiceControl.cpp: the daemon derives its own
# launchd label from its --service-name, and the scripts here look the job up by
# that label. ServiceControl_test.cpp pins the derivation.
#
# Never change the bundle id once released: macOS keys package receipts and
# upgrade detection on it, the same discipline as CPACK_WIX_UPGRADE_GUID.
set(FASTCACHED_MACOS_BUNDLE_ID       "software.lastrada.fastcached")
set(FASTCACHED_MACOS_SERVICE_ACCOUNT "_fastcached")

# The service assets only make sense in the root-rooted package layout: their
# destinations (usr/lib/systemd, etc/) are written assuming the payload lands
# at /. A conventional prefixed install would scatter them to
# <prefix>/usr/lib/systemd, where systemd never looks, so skip them entirely.
if(FASTCACHED_PACKAGE_ROOT_PREFIX AND UNIX AND NOT APPLE)
    # source | destination | kind | installed-name | component
    #
    # The user unit is renamed on install: the source name disambiguates it in
    # the tree, but `systemctl --user start fastcached` requires it to land as
    # fastcached.service inside the user unit directory.
    set(FASTCACHED_PLATFORM_ASSETS
        "linux/fastcached.service|${FASTCACHED_SYSTEMD_SYSTEM_DIR}|data|fastcached.service|Runtime"
        "linux/fastcached-user.service|${FASTCACHED_SYSTEMD_USER_DIR}|data|fastcached.service|Runtime"
        "linux/fastcached.sysusers|${FASTCACHED_SYSUSERS_DIR}|data|fastcached.conf|Runtime"
        "linux/fastcached.tmpfiles|${FASTCACHED_TMPFILES_DIR}|data|fastcached.conf|Runtime"
        "linux/fastcached.yaml|${FASTCACHED_SYSCONF_DIR}|config|fastcached.yaml|Runtime"
    )
elseif(FASTCACHED_PACKAGE_ROOT_PREFIX AND APPLE)
    # The generated scripts are referenced from the build tree; see below.
    set(FASTCACHED_PLATFORM_ASSETS
        # The live fastcached.yaml is deliberately NOT here. A .pkg overwrites
        # its payload unconditionally on every install — there is no conffile or
        # %config(noreplace) equivalent — so shipping it would silently discard
        # the operator's edits on upgrade. The Runtime postinstall seeds the real
        # file from this .default copy only when it is absent.
        "linux/fastcached.yaml|${FASTCACHED_MACOS_PREFIX}/etc|data|fastcached.yaml.default|Runtime"
        "macos/paths.d.fastcached|etc/paths.d|data|fastcached|Runtime"
        "${CMAKE_CURRENT_BINARY_DIR}/macos/fastcached-uninstall|${FASTCACHED_MACOS_PREFIX}/bin|program|fastcached-uninstall|Runtime"
    )
endif()

# One loop installs whichever table the platform selected above. An empty table
# (Windows, which has no asset rows of its own) simply installs nothing.
if(FASTCACHED_PACKAGE_ROOT_PREFIX)
    set(_config_files "")
    foreach(_asset IN LISTS FASTCACHED_PLATFORM_ASSETS)
        string(REPLACE "|" ";" _fields "${_asset}")
        list(GET _fields 0 _source)
        list(GET _fields 1 _destination)
        list(GET _fields 2 _kind)
        list(GET _fields 3 _name)
        list(GET _fields 4 _component)

        # An absolute source is a generated file already in the build tree; a
        # relative one is shipped from this directory.
        if(NOT IS_ABSOLUTE "${_source}")
            set(_source "${CMAKE_CURRENT_SOURCE_DIR}/${_source}")
        endif()

        if(_kind STREQUAL "program")
            install(PROGRAMS "${_source}"
                DESTINATION "${_destination}"
                RENAME "${_name}"
                COMPONENT "${_component}"
            )
        else()
            install(FILES "${_source}"
                DESTINATION "${_destination}"
                RENAME "${_name}"
                COMPONENT "${_component}"
            )
        endif()

        # The exported list names the file's absolute path on the installed
        # system (the packages mount the payload at /), which is the form dpkg
        # conffiles and rpm %config expect — not the staging path.
        if(_kind STREQUAL "config")
            list(APPEND _config_files "/${_destination}/${_name}")
        endif()
    endforeach()

    # Consumed by Packaging.cmake for the DEB conffiles list and the RPM
    # %config(noreplace) filelist.
    set(FASTCACHED_PACKAGE_CONFIG_FILES "${_config_files}"
        CACHE INTERNAL "Package-manager config files (never replaced on upgrade)")
endif()

if(FASTCACHED_PACKAGE_ROOT_PREFIX AND UNIX AND NOT APPLE)

    # --- Maintainer scripts -------------------------------------------------
    # Generated rather than shipped verbatim so the unit name and the
    # sysusers/tmpfiles paths above are substituted in, keeping one source of
    # truth. CPack references the configured copies in the build tree.

    # Absolute paths as seen on the installed system — the scriptlets run there.
    set(FASTCACHED_SYSUSERS_FILE "/${FASTCACHED_SYSUSERS_DIR}/fastcached.conf")
    set(FASTCACHED_TMPFILES_FILE "/${FASTCACHED_TMPFILES_DIR}/fastcached.conf")

    set(FASTCACHED_SCRIPTLETS deb/postinst deb/prerm deb/postrm
                              rpm/post rpm/preun rpm/postun)
    foreach(_script IN LISTS FASTCACHED_SCRIPTLETS)
        get_filename_component(_script_name "${_script}" NAME)
        get_filename_component(_script_dir "${_script}" DIRECTORY)
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/${_script_dir}/${_script_name}")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/linux/${_script}.in" "${_out}" @ONLY)
        # dpkg refuses to run a maintainer script that is not executable.
        file(CHMOD "${_out}" PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    endforeach()

    set(FASTCACHED_SCRIPTLET_DIR "${CMAKE_CURRENT_BINARY_DIR}"
        CACHE INTERNAL "Directory holding the configured DEB/RPM maintainer scripts")
elseif(FASTCACHED_PACKAGE_ROOT_PREFIX AND APPLE)
    # --- Installer scripts --------------------------------------------------
    # Generated, not shipped verbatim, so the prefix, bundle identifier and
    # service-account name each have exactly one definition. The uninstaller in
    # particular must agree with the installer about all three, or it silently
    # removes nothing.
    #
    # One row per script: source stem | installed name (empty = keep the stem).
    # The postinstall scripts are named by CPack convention and referenced from
    # Packaging.cmake through CPACK_POSTFLIGHT_<COMPONENT>_SCRIPT; the
    # uninstaller is payload and is installed by the asset table above.
    set(FASTCACHED_MACOS_SCRIPTS
        "postinstall-runtime"
        "postinstall-launchagent"
        "postinstall-launchdaemon"
        "fastcached-uninstall"
    )

    foreach(_script IN LISTS FASTCACHED_MACOS_SCRIPTS)
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/macos/${_script}")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/macos/${_script}.sh.in" "${_out}" @ONLY)
        # An installer script that is not executable is skipped silently by
        # macOS Installer — the package appears to install and does nothing.
        file(CHMOD "${_out}" PERMISSIONS
            OWNER_READ OWNER_WRITE OWNER_EXECUTE
            GROUP_READ GROUP_EXECUTE
            WORLD_READ WORLD_EXECUTE)
    endforeach()

    set(FASTCACHED_MACOS_SCRIPT_DIR "${CMAKE_CURRENT_BINARY_DIR}/macos"
        CACHE INTERNAL "Directory holding the configured macOS installer scripts")

    # Reset the Linux-only exports; see the comment in the else() branch.
    set(FASTCACHED_SCRIPTLET_DIR ""
        CACHE INTERNAL "Directory holding the configured DEB/RPM maintainer scripts")
else()
    # Both exports must be written on EVERY configure, not only when the branch
    # above runs. They are CACHE INTERNAL, so a value left over from an earlier
    # configure survives a reconfigure that turns the branch off — and cpack
    # would then emit conffiles/%config entries for a payload no longer
    # installed, failing rpmbuild with "File not found" and shipping a DEB whose
    # conffiles list names an absent path.
    set(FASTCACHED_PACKAGE_CONFIG_FILES ""
        CACHE INTERNAL "Package-manager config files (never replaced on upgrade)")
    set(FASTCACHED_SCRIPTLET_DIR ""
        CACHE INTERNAL "Directory holding the configured DEB/RPM maintainer scripts")
endif()

if(NOT APPLE OR NOT FASTCACHED_PACKAGE_ROOT_PREFIX)
    set(FASTCACHED_MACOS_SCRIPT_DIR ""
        CACHE INTERNAL "Directory holding the configured macOS installer scripts")
endif()

if(WIN32)
    # The MSI ships the same example config next to the executables; the
    # service command line registered by --install-service points at it.
    #
    # NOTE: unlike the DEB conffile and the RPM %config(noreplace), this file is
    # a plain component-owned file, so a subsequent MSI OVERWRITES operator
    # edits. That is why the shipped copy is entirely commented out — losing it
    # costs nothing. Anyone customising it should keep the real settings in a
    # separate file and point the service at it:
    #
    #     fastcached.exe --uninstall-service
    #     fastcached.exe --install-service --config="C:\ProgramData\fastcached\fastcached.yaml"
    #
    # Making WiX preserve it instead needs a per-file component with a registry
    # keypath and NeverOverwrite, which the CPack WiX generator does not expose
    # through the patch fragment.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/linux/fastcached.yaml"
        DESTINATION "."
        COMPONENT Runtime
    )

    set(FASTCACHED_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/windows/service-actions.xml"
        CACHE INTERNAL "WiX patch fragment registering the fastcached service")
endif()
