# SPDX-License-Identifier: Apache-2.0
#
# Assert that cmake/portable/CompileCache.cmake can actually install a launcher
# other half of check-compile-cache-autoinstall.cmake, which only proves it
# declines gracefully. A module that declined every time would pass that one
# perfectly.
#
# It runs offline, against a release mirror this script builds in a temporary
# directory and serves over file://. That is not a contrivance for the test's
# sake: FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE exists so an organisation can
# install from its own mirror, and pointing it at a local tree is exactly what
# such a site does. Doing it this way keeps the check deterministic and free of
# GitHub's 60-requests-per-hour limit, which a CI fleet behind one egress
# address would otherwise share out among its jobs — and a check that skips
# because it was rate-limited verifies nothing.
#
# What it covers that the decline paths cannot: composing the asset name,
# finding the launcher at the per-platform path inside the archive, unpacking,
# marking it executable, moving it into the shared staging directory under the
# right version and platform, running it, and handing it to the selection logic.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_WORK_DIR=<scratch>
#         -DFASTCACHED_CXX_COMPILER=<c++> [-DFASTCACHED_MAKE_PROGRAM=<make>]
#         [-DFASTCACHED_GENERATOR=<gen>]
#         -P scripts/check-compile-cache-install.cmake
#
# Exit codes: 0 = installed as specified, or skipped. 1 = it did not.

cmake_minimum_required(VERSION 3.28)

# Oracle for the module's own published-asset table: what each host must ask
# for, spelled out here independently rather than read out of the module, so
# that renaming an asset or moving a binary inside the archive is caught instead
# of being agreed to by both sides at once. The same reason ServiceControl_test
# re-states the service arguments and LauncherCli_test re-states the FASTCACHE_*
# variables.
#
#   <host system>|<host processor>|<asset infix>|<path inside the archive>
#
# A host with no row skips rather than fails: the module is allowed to publish
# for platforms this oracle has not learned about yet, and on such a host there
# is simply nothing here to compare against.
set(FastCachedInstallOracle
    "Linux|x86_64|Linux-x86_64|usr/bin/fastcache-cc"
    "Darwin|arm64|Darwin-arm64|opt/fastcached/bin/fastcache-cc"
)

# The version the mirror publishes. Deliberately not a version that exists, so a
# bug that reached the real GitHub anyway could not accidentally succeed.
set(mirrorVersion "9.8.7")

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_WORK_DIR FASTCACHED_CXX_COMPILER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set (cmake -D${required}=... -P ${CMAKE_CURRENT_LIST_FILE})")
    endif()
endforeach()

set(fixtureDir "${FASTCACHED_SOURCE_DIR}/src/tests/compile-cache-fixture")
set(moduleDir "${FASTCACHED_SOURCE_DIR}/cmake/portable")
if(NOT IS_DIRECTORY "${fixtureDir}")
    message(FATAL_ERROR "no fixture project at ${fixtureDir}")
endif()

if(CMAKE_HOST_WIN32)
    message("SKIP: the launcher sandbox this needs has no Windows equivalent "
            "(see check-compile-cache-autoinstall.cmake's header)")
    return()
endif()
if(NOT EXISTS "${FASTCACHED_CXX_COMPILER}")
    message("SKIP: no C++ compiler at ${FASTCACHED_CXX_COMPILER}, so no project can be configured")
    return()
endif()
find_program(hostUname NAMES uname)
if(NOT hostUname)
    message("SKIP: no uname found, so a sandboxed configure cannot detect its own host")
    return()
endif()

# CMAKE_HOST_SYSTEM_PROCESSOR is populated by project(), which never runs in
# script mode, so it is empty here and cannot be the thing matched on.
# cmake_host_system_information answers the same question without a project and
# in the same spellings the module's table uses.
cmake_host_system_information(RESULT hostProcessor QUERY OS_PLATFORM)

set(platform "")
foreach(row IN LISTS FastCachedInstallOracle)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 rowSystem)
    list(GET fields 1 rowProcessor)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "${rowSystem}" AND hostProcessor STREQUAL "${rowProcessor}")
        list(GET fields 2 platform)
        list(GET fields 3 member)
        break()
    endif()
endforeach()
if(NOT platform)
    message("SKIP: no published fastcache-cc is expected for "
            "${CMAKE_HOST_SYSTEM_NAME}-${hostProcessor}, so there is nothing to install")
    return()
endif()

file(REMOVE_RECURSE "${FASTCACHED_WORK_DIR}")
file(MAKE_DIRECTORY "${FASTCACHED_WORK_DIR}")

# Build the archive the module will be asked to install: the same name and the
# same interior layout a real release has, holding a launcher that answers
# --version and nothing else. A script rather than a compiled binary, because
# what is under test is the plumbing around the file, not the file.
set(stem "fastcached-${mirrorVersion}-${platform}")
set(payloadRoot "${FASTCACHED_WORK_DIR}/payload")
get_filename_component(memberDir "${payloadRoot}/${stem}/${member}" DIRECTORY)
file(MAKE_DIRECTORY "${memberDir}")
file(WRITE "${payloadRoot}/${stem}/${member}"
     "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'fastcache-cc ${mirrorVersion}'; exit 0; fi\nexit 3\n")
file(CHMOD "${payloadRoot}/${stem}/${member}"
     PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

set(mirrorDir "${FASTCACHED_WORK_DIR}/mirror/fastcached-mirror/releases/download/v${mirrorVersion}")
file(MAKE_DIRECTORY "${mirrorDir}")
file(ARCHIVE_CREATE
     OUTPUT "${mirrorDir}/${stem}.tar.gz"
     PATHS "${payloadRoot}/${stem}"
     FORMAT gnutar
     COMPRESSION GZip
     WORKING_DIRECTORY "${payloadRoot}")

set(sandbox "${FASTCACHED_WORK_DIR}/sandbox")
file(MAKE_DIRECTORY "${sandbox}/usr/bin")
file(CREATE_LINK "${hostUname}" "${sandbox}/usr/bin/uname" COPY_ON_ERROR SYMBOLIC)

set(stageDir "${FASTCACHED_WORK_DIR}/stage")
set(buildDir "${FASTCACHED_WORK_DIR}/build")
set(arguments
    "-DFASTCACHED_MODULE_DIR=${moduleDir}"
    "-DCMAKE_CXX_COMPILER=${FASTCACHED_CXX_COMPILER}"
    "-DCMAKE_FIND_ROOT_PATH=${sandbox}"
    "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=ONLY"
    "-DFASTCACHE_AUTO_INSTALL=ON"
    "-DFASTCACHE_AUTO_INSTALL_DIR=${stageDir}"
    "-DFASTCACHE_AUTO_INSTALL_REPO=fastcached-mirror"
    "-DFASTCACHE_AUTO_INSTALL_VERSION=${mirrorVersion}"
    "-DFASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE=file://${FASTCACHED_WORK_DIR}/mirror")
if(FASTCACHED_MAKE_PROGRAM)
    list(APPEND arguments "-DCMAKE_MAKE_PROGRAM=${FASTCACHED_MAKE_PROGRAM}")
endif()
if(FASTCACHED_GENERATOR)
    list(APPEND arguments -G "${FASTCACHED_GENERATOR}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${fixtureDir}" -B "${buildDir}" ${arguments}
    RESULT_VARIABLE configureResult
    OUTPUT_VARIABLE configureOutput
    ERROR_VARIABLE configureError
    TIMEOUT 300)
set(output "${configureOutput}${configureError}")

set(violations "")
if(NOT configureResult EQUAL 0)
    list(APPEND violations "configure failed (${configureResult})")
endif()

string(FIND "${output}" "Auto-installed fastcache-cc" announced)
if(announced EQUAL -1)
    list(APPEND violations "the install was never announced")
endif()

# Where the binary has to land, spelled out here rather than globbed for: the
# version and the platform are both in the path so that one shared staging
# directory can serve several of each, and a layout that quietly lost either
# would still glob just fine.
set(expected "${stageDir}/${mirrorVersion}/${platform}/fastcache-cc")
if(NOT EXISTS "${expected}")
    file(GLOB_RECURSE staged "${stageDir}/*")
    list(APPEND violations "expected the launcher at ${expected}, found [${staged}]")
else()
    execute_process(COMMAND "${expected}" --version
                    RESULT_VARIABLE stagedResult
                    OUTPUT_VARIABLE stagedOutput
                    ERROR_QUIET
                    TIMEOUT 30)
    string(STRIP "${stagedOutput}" stagedOutput)
    if(NOT stagedResult EQUAL 0)
        # Chiefly: it must have been made executable on the way in.
        list(APPEND violations "the staged launcher does not run (${stagedResult})")
    elseif(NOT stagedOutput STREQUAL "fastcache-cc ${mirrorVersion}")
        list(APPEND violations "the staged launcher answered '${stagedOutput}'")
    endif()
endif()

if(violations)
    list(JOIN violations "\n  " report)
    message(FATAL_ERROR
        "compile-cache install from a mirror did not work as specified:\n  ${report}\n\n"
        "Configure output was:\n${output}")
endif()

message("compile-cache install: ${platform} launcher fetched, staged and runnable")
