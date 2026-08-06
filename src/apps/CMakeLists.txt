# SPDX-License-Identifier: Apache-2.0
# Executables. Every shipped or shippable binary lives under src/apps/<name>/;
# the libraries they link (FastCache, CowTree) are siblings under src/.
#
# Each app is one row in the table below: a directory, the option that gates
# it, that option's default, and its help text. Adding an app is adding a row
# — no new if/else arm here.

set(FASTCACHED_APPS
    #  directory                   option                          default  description
    "fastcached|FASTCACHED_BUILD_DAEMON|ON|Build the fastcached daemon"
    "fastcache-cc|FASTCACHED_BUILD_LAUNCHER|ON|Build the fastcache-cc compiler launcher"
    "compile-cache-testclient|FASTCACHED_BUILD_TESTCLIENT|OFF|Build the compile-cache test client"
)

foreach(_app IN LISTS FASTCACHED_APPS)
    string(REPLACE "|" ";" _fields "${_app}")
    list(GET _fields 0 _dir)
    list(GET _fields 1 _option)
    list(GET _fields 2 _default)
    list(GET _fields 3 _help)

    option(${_option} "${_help}" ${_default})
    if(${_option})
        add_subdirectory(${_dir})
    endif()
endforeach()
