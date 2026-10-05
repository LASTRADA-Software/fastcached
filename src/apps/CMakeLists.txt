# SPDX-License-Identifier: Apache-2.0
# Executables. Every shipped or shippable binary lives under src/apps/<name>/;
# the libraries they link (FastCache, CowTree) are siblings under src/.
#
# Each app is one row in the table below: a directory, the option that gates
# it, that option's default, the install component it ships under on Windows,
# and its help text. Adding an app is adding a row — no new if/else arm here.

set(FASTCACHED_APPS
    #  directory                   option                          default  component  description
    "fastcached|FASTCACHED_BUILD_DAEMON|ON|Daemon|Build the fastcached daemon"
    "fastcache-cc|FASTCACHED_BUILD_LAUNCHER|ON|Launcher|Build the fastcache-cc compiler launcher"
    "fastcache-compile-node|FASTCACHED_BUILD_NODE|ON|Node|Build the fastcache-compile-node compile worker"
    "fastcache-cli|FASTCACHED_BUILD_CLI|ON|Cli|Build the fastcache-cli operator client"
    "compile-cache-testclient|FASTCACHED_BUILD_TESTCLIENT|OFF|-|Build the compile-cache test client"
    "fastcache-bench|FASTCACHED_BUILD_BENCHMARKS|OFF|-|Build the in-process storage micro-benchmarks"
)

foreach(_app IN LISTS FASTCACHED_APPS)
    string(REPLACE "|" ";" _fields "${_app}")
    list(GET _fields 0 _dir)
    list(GET _fields 1 _option)
    list(GET _fields 2 _default)
    list(GET _fields 3 _component)
    list(GET _fields 4 _help)

    option(${_option} "${_help}" ${_default})
    if(${_option})
        # The component an app's install() names. One per app on Windows, where each is an MSI
        # feature an operator selects; the one shared payload elsewhere, because a .deb, an .rpm
        # and a .pkg install every binary together. `-` is an app that installs nothing.
        if(WIN32)
            set(FASTCACHED_APP_COMPONENT "${_component}")
        else()
            set(FASTCACHED_APP_COMPONENT "${FASTCACHED_SHARED_COMPONENT}")
        endif()
        if(NOT _component STREQUAL "-")
            set_property(GLOBAL PROPERTY "FASTCACHED_COMPONENT_OF_${_dir}" "${FASTCACHED_APP_COMPONENT}")
            set_property(GLOBAL APPEND PROPERTY FASTCACHED_FIRST_PARTY_COMPONENTS "${FASTCACHED_APP_COMPONENT}")
            if(WIN32)
                set_property(GLOBAL APPEND PROPERTY FASTCACHED_APP_COMPONENTS "${FASTCACHED_APP_COMPONENT}")
            endif()
        endif()
        add_subdirectory(${_dir})
    endif()
endforeach()
