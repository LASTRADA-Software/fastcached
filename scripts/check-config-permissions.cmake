# SPDX-License-Identifier: Apache-2.0
#
# Every machine-wide config the Linux packaging INSTALLS must also be restricted by
# both maintainer scriptlets.
#
# `install(FILES ...)` runs at package-build time, when neither service account
# exists, so it cannot chown -- the payload ships `0644 root:root` and the postinst
# is the only place the mode can be set. Those are two files in two directories
# with nothing connecting them, which is how the packaging shipped a world-readable
# `requirepass` location for as long as it did
# ([#861](https://github.com/LASTRADA-Software/fastcached/issues/861)).
#
# DERIVED from the asset table rather than restating it: a config row added later is
# checked without a second author. And it refuses when either scan matches nothing,
# because an empty asset list and an empty scriptlet agree perfectly.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "check-config-permissions: FASTCACHED_SOURCE_DIR must be set")
endif()

set(_assets "${FASTCACHED_SOURCE_DIR}/packaging/CMakeLists.txt")
if(NOT EXISTS "${_assets}")
    message(FATAL_ERROR "check-config-permissions: cannot read ${_assets}")
endif()

# Rows look like: "linux/<name>.yaml|${FASTCACHED_SYSCONF_DIR}|config|<installed>|Runtime"
file(STRINGS "${_assets}" _lines)
set(_configs "")
foreach(_line IN LISTS _lines)
    if(_line MATCHES "\\|config\\|([A-Za-z0-9._-]+)\\|")
        list(APPEND _configs "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _configs)

list(LENGTH _configs _count)
if(_count EQUAL 0)
    message(FATAL_ERROR
        "check-config-permissions: found NO `|config|` rows in packaging/CMakeLists.txt. "
        "The scan matched nothing, which is not the same as nothing being wrong -- an "
        "empty list agrees with every scriptlet.")
endif()

set(_scriptlets
    "${FASTCACHED_SOURCE_DIR}/packaging/linux/deb/postinst.in"
    "${FASTCACHED_SOURCE_DIR}/packaging/linux/rpm/post.in")

set(_missing "")
foreach(_script IN LISTS _scriptlets)
    if(NOT EXISTS "${_script}")
        message(FATAL_ERROR "check-config-permissions: cannot read ${_script}")
    endif()
    file(READ "${_script}" _body)
    if(NOT _body MATCHES "perm /0004")
        message(FATAL_ERROR
            "check-config-permissions: ${_script} has no world-readable test. The repair "
            "must be conditional, or an operator's deliberately narrowed config is "
            "overwritten on every upgrade.")
    endif()
    foreach(_conf IN LISTS _configs)
        if(NOT _body MATCHES "${_conf}:")
            list(APPEND _missing "${_script} does not restrict ${_conf}")
        endif()
    endforeach()
endforeach()

if(_missing)
    string(REPLACE ";" "\n  " _report "${_missing}")
    message(FATAL_ERROR
        "check-config-permissions: a config file is installed but never restricted:\n  ${_report}\n"
        "`install(FILES)` cannot chown at package-build time, so a config with no "
        "postinst entry ships 0644 root:root -- world-readable, at the location this "
        "product's own guidance tells operators to put `requirepass` into.")
endif()

message(STATUS "config-permissions: ${_count} installed config(s) restricted by both scriptlets")
