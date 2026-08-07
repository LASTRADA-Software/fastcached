# SPDX-License-Identifier: Apache-2.0
# Platform service-integration assets: systemd units and their supporting
# account/state declarations on Linux, the WiX service fragment on Windows.
#
# Every installed asset is one row in the table below — source path, install
# destination, and kind. Adding a man page, a logrotate snippet, or an AppArmor
# profile is adding a row, not a new install() call. `kind` is what makes the
# table more than a loop: `config` rows are additionally exported as the
# package-manager config-file list, so "which files are configuration" is
# stated exactly once and reused by both the DEB and RPM generators.

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

# The service assets only make sense in the root-rooted package layout: their
# destinations (usr/lib/systemd, etc/) are written assuming the payload lands
# at /. A conventional prefixed install would scatter them to
# <prefix>/usr/lib/systemd, where systemd never looks, so skip them entirely.
if(UNIX AND NOT APPLE AND FASTCACHED_PACKAGE_ROOT_PREFIX)
    # source | destination | kind | installed-name
    #
    # kind:   config  -> shipped as a package config file (never clobbered on
    #                    upgrade); also installed for the Windows MSI
    #         data    -> plain installed file
    #
    # The user unit is renamed on install: the source name disambiguates it in
    # the tree, but `systemctl --user start fastcached` requires it to land as
    # fastcached.service inside the user unit directory.
    set(FASTCACHED_LINUX_ASSETS
        "linux/fastcached.service|${FASTCACHED_SYSTEMD_SYSTEM_DIR}|data|fastcached.service"
        "linux/fastcached-user.service|${FASTCACHED_SYSTEMD_USER_DIR}|data|fastcached.service"
        "linux/fastcached.sysusers|${FASTCACHED_SYSUSERS_DIR}|data|fastcached.conf"
        "linux/fastcached.tmpfiles|${FASTCACHED_TMPFILES_DIR}|data|fastcached.conf"
        "linux/fastcached.yaml|${FASTCACHED_SYSCONF_DIR}|config|fastcached.yaml"
    )

    set(_config_files "")
    foreach(_asset IN LISTS FASTCACHED_LINUX_ASSETS)
        string(REPLACE "|" ";" _fields "${_asset}")
        list(GET _fields 0 _source)
        list(GET _fields 1 _destination)
        list(GET _fields 2 _kind)
        list(GET _fields 3 _name)

        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_source}"
            DESTINATION "${_destination}"
            RENAME "${_name}"
            COMPONENT Runtime
        )

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
