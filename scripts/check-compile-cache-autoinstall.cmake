# SPDX-License-Identifier: Apache-2.0
#
# Assert that cmake/CompileCache.cmake declines to auto-install fastcache-cc
# gracefully — and above all *without failing the configure* — whenever it
# cannot install one.
#
# That is the property issue #48 turns on. A build with no compiler cache is a
# slow build; a build whose configure aborts because a cache could not be
# downloaded is no build at all, and the module is vendored into repositories
# that have never heard of fastcached. Every way the fetch can go wrong
# therefore has to end in one status line and a fall-through to sccache, ccache,
# or plain compilation.
#
# Written as a `cmake -P` script for the reasons spelled out at the top of
# check-repository-hygiene.cmake: the logic is the same everywhere, so a .sh and
# a .ps1 would be two implementations of one rule, each free to rot without the
# other noticing — and cmake is the one prerequisite a CMake project always has.
#
# The *sandbox* below is the part that is not portable, which is why this skips
# on Windows. To reach the code under test the three launchers must be
# unfindable, and CMAKE_FIND_ROOT_PATH_MODE_PROGRAM is the only knob that makes
# find_program come up empty without also hiding the toolchain. It redirects
# CMake's own `uname` lookup too, and a host whose architecture is unknown takes
# the "no binary published for this platform" path before reaching the one under
# test — so the sandbox is seeded with a uname of its own. Windows resolves the
# host architecture from the environment instead and needs no such seed, but its
# generators find more of their toolchain through find_program than a sandbox
# can safely stand in for. The rules being checked are plain CMake and identical
# on every platform, so covering them on Linux and macOS covers them.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_WORK_DIR=<scratch>
#         -DFASTCACHED_CXX_COMPILER=<c++> [-DFASTCACHED_MAKE_PROGRAM=<make>]
#         [-DFASTCACHED_GENERATOR=<gen>]
#         -P scripts/check-compile-cache-autoinstall.cmake
#
# Exit codes: 0 = every row behaved, or skipped. 1 = at least one row did not.

cmake_minimum_required(VERSION 3.28)

# One row per way the auto-install can decline, pipe-delimited:
#
#   <name>|<expected output>|<forbidden output>|<extra -D arguments>
#
# Every row additionally requires the configure to succeed: an exit code is the
# whole point here, so it is asserted for all of them rather than spelled per
# row. An empty expected or forbidden field means "do not check that".
#
# No row may contain a ';' — these are CMake lists, and a semicolon inside a row
# would split it in two. Arguments are separated by single spaces, so no
# argument may contain one either. A fifth way to decline is a fifth row and
# nothing below changes.
set(FastCachedAutoInstallRows
    "api-unreachable|Not auto-installing fastcache-cc: cannot reach the GitHub API||-DFASTCACHE_AUTO_INSTALL=ON -DFASTCACHE_AUTO_INSTALL_API=http://127.0.0.1:1"
    "off-by-default||Not auto-installing fastcache-cc|"
    "opted-out-empty-addr|Not auto-installing fastcache-cc: FASTCACHE_ADDR is empty||-DFASTCACHE_AUTO_INSTALL=ON -DFASTCACHE_ADDR="
    "not-a-version|is not a numeric X.Y.Z version||-DFASTCACHE_AUTO_INSTALL=ON -DFASTCACHE_AUTO_INSTALL_VERSION=not-a-version"
    "no-such-release|Not auto-installing fastcache-cc:||-DFASTCACHE_AUTO_INSTALL=ON -DFASTCACHE_AUTO_INSTALL_VERSION=9.9.9"
)

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_WORK_DIR FASTCACHED_CXX_COMPILER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set (cmake -D${required}=... -P ${CMAKE_CURRENT_LIST_FILE})")
    endif()
endforeach()

set(fixtureDir "${FASTCACHED_SOURCE_DIR}/src/tests/compile-cache-fixture")
set(moduleDir "${FASTCACHED_SOURCE_DIR}/cmake")
if(NOT IS_DIRECTORY "${fixtureDir}")
    message(FATAL_ERROR "no fixture project at ${fixtureDir}")
endif()

if(CMAKE_HOST_WIN32)
    message("SKIP: the launcher sandbox this needs has no Windows equivalent (see the header)")
    return()
endif()

# Seed the sandbox with a uname, so host detection inside it still answers and
# the rows exercise the decline paths rather than the unknown-platform one.
find_program(hostUname NAMES uname)
if(NOT hostUname)
    message("SKIP: no uname found, so a sandboxed configure cannot detect its own host")
    return()
endif()
if(NOT EXISTS "${FASTCACHED_CXX_COMPILER}")
    message("SKIP: no C++ compiler at ${FASTCACHED_CXX_COMPILER}, so no project can be configured")
    return()
endif()

set(sandbox "${FASTCACHED_WORK_DIR}/sandbox")
file(REMOVE_RECURSE "${sandbox}")
file(MAKE_DIRECTORY "${sandbox}/usr/bin")
file(CREATE_LINK "${hostUname}" "${sandbox}/usr/bin/uname" COPY_ON_ERROR SYMBOLIC)

# Shared by every row: a real project, a compiler named outright, and a program
# search that can find neither fastcache-cc nor sccache nor ccache.
set(commonArguments
    "-DFASTCACHED_MODULE_DIR=${moduleDir}"
    "-DCMAKE_CXX_COMPILER=${FASTCACHED_CXX_COMPILER}"
    "-DCMAKE_FIND_ROOT_PATH=${sandbox}"
    "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=ONLY")
if(FASTCACHED_MAKE_PROGRAM)
    list(APPEND commonArguments "-DCMAKE_MAKE_PROGRAM=${FASTCACHED_MAKE_PROGRAM}")
endif()
if(FASTCACHED_GENERATOR)
    list(APPEND commonArguments -G "${FASTCACHED_GENERATOR}")
endif()

set(violations "")
list(LENGTH FastCachedAutoInstallRows rowCount)

foreach(row IN LISTS FastCachedAutoInstallRows)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 name)
    list(GET fields 1 expected)
    list(GET fields 2 forbidden)
    list(GET fields 3 extra)

    set(extraArguments "")
    if(NOT extra STREQUAL "")
        string(REPLACE " " ";" extraArguments "${extra}")
    endif()

    # A staging directory of its own per row, never the user's: a test must not
    # write into ~/.cache, and two rows must not inherit each other's download.
    set(rowBinaryDir "${FASTCACHED_WORK_DIR}/${name}/build")
    set(rowStageDir "${FASTCACHED_WORK_DIR}/${name}/stage")
    file(REMOVE_RECURSE "${FASTCACHED_WORK_DIR}/${name}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -S "${fixtureDir}" -B "${rowBinaryDir}"
                ${commonArguments}
                "-DFASTCACHE_AUTO_INSTALL_DIR=${rowStageDir}"
                ${extraArguments}
        RESULT_VARIABLE configureResult
        OUTPUT_VARIABLE configureOutput
        ERROR_VARIABLE configureError
        TIMEOUT 300)
    set(output "${configureOutput}${configureError}")

    if(NOT configureResult EQUAL 0)
        list(APPEND violations
             "${name}: configure failed (${configureResult}), but declining to auto-install must never fail a configure\n${output}")
        continue()
    endif()

    if(NOT expected STREQUAL "")
        string(FIND "${output}" "${expected}" foundAt)
        if(foundAt EQUAL -1)
            list(APPEND violations "${name}: expected to see \"${expected}\"\n${output}")
        endif()
    endif()

    if(NOT forbidden STREQUAL "")
        string(FIND "${output}" "${forbidden}" foundAt)
        if(NOT foundAt EQUAL -1)
            list(APPEND violations "${name}: did not expect to see \"${forbidden}\"\n${output}")
        endif()
    endif()

    # Whatever it decided, it must not have left a launcher wired up: falling
    # back means falling back all the way.
    if(EXISTS "${rowStageDir}")
        file(GLOB_RECURSE staged "${rowStageDir}/*")
        if(staged)
            list(APPEND violations "${name}: staged ${staged} even though the install was declined")
        endif()
    endif()
endforeach()

if(violations)
    list(JOIN violations "\n\n" report)
    message(FATAL_ERROR
        "compile-cache auto-install did not degrade gracefully:\n\n${report}\n\n"
        "Declining to install a compiler cache must always be a status message and a "
        "fall-through, never a configure failure — the module is vendored into projects "
        "that must keep building when it cannot reach the network.")
endif()

message("compile-cache auto-install: ${rowCount} decline path(s) checked, all fell back cleanly")
