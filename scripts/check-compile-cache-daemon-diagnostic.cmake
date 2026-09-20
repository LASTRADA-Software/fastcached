# SPDX-License-Identifier: Apache-2.0
#
# Assert that when FASTCACHE_AUTO_START's daemon dies on startup, the configure says
# WHY and not merely with what number ([#1538](https://github.com/LASTRADA-Software/fastcached/issues/1538)).
#
# ## What went wrong
#
# `_fc_auto_start_fastcached` spawned the staged daemon under `OUTPUT_QUIET ERROR_QUIET`
# and reported `fastcached exited immediately (${_rc})`. The commonest failure there is a
# dynamic loader that cannot resolve a library, which answers **127** — a number that
# reads as *not found* for a binary CMake has just staged and knows the path of. The one
# sentence that explains it, `error while loading shared libraries: libyaml-cpp.so.0.8`,
# was on the child's stderr and was captured by nobody.
#
# ## Why an end-to-end configure and not a unit test of the helper
#
# `_fc_first_line` is a pure function and its parse is worth nothing on its own: what
# failed here was the ACQUISITION — two `execute_process` calls that threw the stream
# away. A test over the helper would have passed on the broken tree. So this drives the
# module's real path, through the same file:// mirror
# `check-compile-cache-daemon-staging.cmake` uses, with a staged `fastcached` that fails
# the way the loader fails.
#
# ## Both directions
#
# A guard nobody has watched ACCEPT is not known to work, so this asserts two shapes
# rather than one:
#
#   * a daemon that dies with a stderr line -> the line is in the configure output,
#     beside the exit status;
#   * a daemon that dies SILENTLY -> the message degrades to the status alone and grows
#     no empty suffix, which is what says the capture is read rather than pasted.
#
# Neither is a shape a passing tree reaches by accident: the fake in the staging test
# exits 3 with nothing on either stream, and that test was green throughout the defect.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_SCRATCH_DIR=<scratch>
#         -DFASTCACHED_CXX_COMPILER=<c++> [-DFASTCACHED_MAKE_PROGRAM=<make>]
#         [-DFASTCACHED_GENERATOR=<gen>]
#         -P scripts/check-compile-cache-daemon-diagnostic.cmake
#
# Exit codes: 0 = both shapes reported as specified, or skipped. 1 = they did not.

cmake_minimum_required(VERSION 3.28)

# The asset table's own oracle, independent of the module's copy for the reason
# check-compile-cache-daemon-staging.cmake states: a rename on one side must be caught
# rather than agreed to by both sides at once.
#
#   <host system>|<host processor>|<asset infix>|<launcher path>|<daemon path>
set(FastCachedInstallOracle
    "Linux|x86_64|Linux-x86_64|usr/bin/fastcache-cc|usr/bin/fastcached"
    "Darwin|arm64|Darwin-arm64|opt/fastcached/bin/fastcache-cc|opt/fastcached/bin/fastcached"
)

# Deliberately not a version that exists, so a bug that reached the real GitHub anyway
# could not accidentally succeed.
set(mirrorVersion "9.8.7")

# The sentence a loader prints, as the daemon's stub will. Long enough to be a real
# phrase and short enough that no CMake status line could wrap it away.
set(loaderLine "error while loading shared libraries: libyaml-cpp.so.0.8")

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR FASTCACHED_CXX_COMPILER)
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

cmake_host_system_information(RESULT hostProcessor QUERY OS_PLATFORM)

set(platform "")
foreach(row IN LISTS FastCachedInstallOracle)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 rowSystem)
    list(GET fields 1 rowProcessor)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "${rowSystem}" AND hostProcessor STREQUAL "${rowProcessor}")
        list(GET fields 2 platform)
        list(GET fields 3 launcherMember)
        list(GET fields 4 daemonMember)
        break()
    endif()
endforeach()
if(NOT platform)
    message("SKIP: no published fastcached is expected for "
            "${CMAKE_HOST_SYSTEM_NAME}-${hostProcessor}, so there is nothing to start")
    return()
endif()

file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${FASTCACHED_SCRATCH_DIR}")

# Run one configure against a mirror whose staged daemon fails as described.
#
# @param case A short label for the failure message.
# @param daemonBody The shell body the fake fastcached runs when it is NOT asked
#                   --version; it must exit non-zero, which is what puts the module on
#                   the branch under test.
# @param addrPort A port nothing on this host listens on, so `_fc_daemon_answering`
#                 finds nothing and the module proceeds to stage and spawn. PASSED
#                 rather than derived: a first draft computed it from the length of
#                 `case`, and both case names are six characters long -- so the two
#                 runs shared one port, under a comment claiming the derivation kept
#                 them apart. A derivation that derives nothing is worse than a
#                 literal, because it reads as though somebody had thought about it.
# @param outVar Receives the configure's combined output.
function(RunFailingStart case daemonBody addrPort outVar)
    set(root "${FASTCACHED_SCRATCH_DIR}/${case}")
    set(stem "fastcached-${mirrorVersion}-${platform}")
    set(payloadRoot "${root}/payload")

    # The launcher answers --version and nothing else, exactly as the staging test's
    # does: this case is about the DAEMON's failure, and a launcher that also failed
    # would stop the configure before it got here.
    get_filename_component(launcherDir "${payloadRoot}/${stem}/${launcherMember}" DIRECTORY)
    file(MAKE_DIRECTORY "${launcherDir}")
    file(WRITE "${payloadRoot}/${stem}/${launcherMember}"
         "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'fastcache-cc ${mirrorVersion}'; exit 0; fi\nexit 3\n")
    file(CHMOD "${payloadRoot}/${stem}/${launcherMember}"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

    # --version must still succeed: the module probes the staged daemon that way before
    # it ever spawns one, so a stub that failed everything would be refused earlier and
    # this case would pass for the wrong reason.
    get_filename_component(daemonDir "${payloadRoot}/${stem}/${daemonMember}" DIRECTORY)
    file(MAKE_DIRECTORY "${daemonDir}")
    file(WRITE "${payloadRoot}/${stem}/${daemonMember}"
         "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then echo 'fastcached ${mirrorVersion}'; exit 0; fi\n${daemonBody}\n")
    file(CHMOD "${payloadRoot}/${stem}/${daemonMember}"
         PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

    set(mirrorDir "${root}/mirror/fastcached-mirror/releases/download/v${mirrorVersion}")
    file(MAKE_DIRECTORY "${mirrorDir}")
    # `cmake -E tar` rather than file(ARCHIVE_CREATE): the latter's WORKING_DIRECTORY
    # option needs CMake 3.31, newer than the 3.28 floor this script targets.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar czf "${mirrorDir}/${stem}.tar.gz" --format=gnutar "${stem}"
        WORKING_DIRECTORY "${payloadRoot}"
        RESULT_VARIABLE archiveResult
        ERROR_VARIABLE archiveError)
    if(NOT archiveResult EQUAL 0)
        message(FATAL_ERROR "could not build the mirror archive: ${archiveResult} ${archiveError}")
    endif()

    set(sandbox "${root}/sandbox")
    file(MAKE_DIRECTORY "${sandbox}/usr/bin")
    file(CREATE_LINK "${hostUname}" "${sandbox}/usr/bin/uname" COPY_ON_ERROR SYMBOLIC)

    set(arguments
        "-DFASTCACHED_MODULE_DIR=${moduleDir}"
        "-DCMAKE_CXX_COMPILER=${FASTCACHED_CXX_COMPILER}"
        "-DCMAKE_FIND_ROOT_PATH=${sandbox}"
        "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=ONLY"
        "-DFASTCACHE_AUTO_INSTALL=ON"
        "-DFASTCACHE_AUTO_INSTALL_DIR=${root}/stage"
        "-DFASTCACHE_AUTO_INSTALL_REPO=fastcached-mirror"
        "-DFASTCACHE_AUTO_INSTALL_VERSION=${mirrorVersion}"
        "-DFASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE=file://${root}/mirror"
        "-DFASTCACHE_AUTO_START=ON"
        "-DFASTCACHE_AUTO_START_STORAGE_DIR=${root}/storage"
        "-DFASTCACHE_ADDR=127.0.0.1:${addrPort}")
    if(FASTCACHED_MAKE_PROGRAM)
        list(APPEND arguments "-DCMAKE_MAKE_PROGRAM=${FASTCACHED_MAKE_PROGRAM}")
    endif()
    if(FASTCACHED_GENERATOR)
        list(APPEND arguments -G "${FASTCACHED_GENERATOR}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${fixtureDir}" -B "${root}/build" ${arguments}
        RESULT_VARIABLE configureResult
        OUTPUT_VARIABLE configureOutput
        ERROR_VARIABLE configureError
        TIMEOUT 300)
    if(NOT configureResult EQUAL 0)
        message(FATAL_ERROR "the ${case} configure failed (${configureResult}); this module must never fail one:\n"
                            "${configureOutput}${configureError}")
    endif()
    set(${outVar} "${configureOutput}${configureError}" PARENT_SCOPE)
endfunction()

set(violations "")

# --- The shape #1538 was reported as -------------------------------------------------
# `printf` rather than `echo`, because `echo`'s handling of a leading `-` and of escapes
# is not portable across /bin/sh implementations and the line under test is data.
RunFailingStart("loader"
    "printf '%s\\n' '${loaderLine}' >&2\nexit 127"
    18701
    loaderOutput)

string(FIND "${loaderOutput}" "Not starting a daemon: fastcached exited immediately (127)" loaderStatusAt)
if(loaderStatusAt EQUAL -1)
    list(APPEND violations
         "the configure never reported the daemon exiting immediately with 127:\n${loaderOutput}")
endif()
string(FIND "${loaderOutput}" "${loaderLine}" loaderLineAt)
if(loaderLineAt EQUAL -1)
    list(APPEND violations
         "the daemon's own first stderr line is nowhere in the configure output. That line is the "
         "whole diagnosis -- 127 alone reads as 'not found' for a binary the module just staged "
         "(#1538). Output was:\n${loaderOutput}")
endif()

# --- And the shape that says the capture is READ rather than pasted -------------------
RunFailingStart("silent" "exit 4" 18702 silentOutput)

string(FIND "${silentOutput}" "Not starting a daemon: fastcached exited immediately (4)" silentStatusAt)
if(silentStatusAt EQUAL -1)
    list(APPEND violations
         "a daemon that died silently was not reported at all, or not with its status:\n${silentOutput}")
endif()
# An empty capture must degrade to the old text rather than to text with a dangling
# separator. Asserting on the colon is what distinguishes "no words were available" from
# "the words were appended unconditionally and happened to be empty".
string(FIND "${silentOutput}" "fastcached exited immediately (4):" silentSuffixAt)
if(NOT silentSuffixAt EQUAL -1)
    list(APPEND violations
         "a daemon that said nothing still got a ':' suffix, so the message appends whatever it "
         "captured without asking whether there was anything:\n${silentOutput}")
endif()

if(violations)
    foreach(violation IN LISTS violations)
        message("FAIL: ${violation}")
    endforeach()
    message(FATAL_ERROR "compile-cache-daemon-diagnostic: the start failure is not explained")
endif()

message(STATUS "compile-cache-daemon-diagnostic: 2 case(s) ran, both explained")
