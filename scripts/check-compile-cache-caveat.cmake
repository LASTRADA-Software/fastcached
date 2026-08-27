# SPDX-License-Identifier: Apache-2.0
#
# Assert that cmake/portable/CompileCache.cmake says so when it selects a
# launcher that can silently produce a wrong build — and that it stays quiet
# when it selects one that cannot.
#
# That is issue #153. Under MSVC and clang-cl, sccache replays a cache hit's
# /showIncludes stream with the absolute paths spelled by the build that STORED
# it, while the text it hashes to find that hit carries no paths at all -- so a
# second checkout shares entries and then records dependencies pointing into the
# first. Measured on this repository: two worktrees at one commit, 137
# cross-worktree hits, 1097 recorded dependency edges pointing at the other
# checkout and none at its own, and `ninja: no work to do` after a real edit. The
# build stays green and the objects are stale.
#
# GCC and Clang are NOT exposed -- their preprocessed output carries the paths, so
# two checkouts do not share entries at all (measured: 0 hits, 2 misses) -- and
# the module carries the caveat on exactly that condition. Which half this run
# asserts therefore comes in as FASTCACHED_MSVC_LIKE, resolved by the caller that
# already knows the compiler rather than re-derived here.
#
# The module cannot fix that — it is sccache's replay, and fastcache-cc exists
# because it does not have it — so what it owes a developer is a word about it.
# This checks that the word is there.
#
# **Both halves, and the second is the point.** A warning that fired for every
# launcher would be a warning nobody reads, so the rows below assert the silence
# as well as the noise: ccache selected says nothing, and neither does caching
# switched off while sccache sits right there.
#
# Written as a `cmake -P` script for the reasons check-repository-hygiene.cmake
# gives at length: the rule is identical everywhere, so a .sh and a .ps1 would be
# two implementations of one rule, each free to rot without the other noticing.
#
# Unlike check-compile-cache-autoinstall.cmake this needs **no sandbox**: it
# names each launcher outright through the `find_program` cache variables the
# module already declares, rather than having to make them unfindable. So it runs
# on every host, Windows included -- which its three siblings do not, and which is
# why it opens with a canary: a Windows shell that is not a Developer shell has a
# cl.exe that exists and cannot compile.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_WORK_DIR=<scratch>
#         -DFASTCACHED_CXX_COMPILER=<c++> [-DFASTCACHED_MSVC_LIKE=ON|OFF]
#         [-DFASTCACHED_MAKE_PROGRAM=<make>] [-DFASTCACHED_GENERATOR=<gen>]
#         -P scripts/check-compile-cache-caveat.cmake
#
# Exit codes: 0 = every row behaved, or skipped. 1 = at least one did not.

cmake_minimum_required(VERSION 3.28)

# The fragment every "must warn" row looks for. One place, because it appears in
# the module and in the rows below, and a caveat reworded in only one of the two
# is a test that passes while saying nothing.
set(FastCachedCaveatMarker "sccache replays a cache hit")

# One row per selection outcome, pipe-delimited:
#
#   <name>|<expected output>|<forbidden output>|<stand-in launcher>|<severity>|<extra -D args>
#
# <severity> is `warning` when the row must produce a CMake Warning carrying the
# caveat, and empty when it must produce no warning at all. Both halves are
# checked: asserting the text alone would let the caveat be downgraded to a
# `message(STATUS)` -- the one thing its own comment argues against, since the
# symptom arrives hours later and a status line does not survive a scroll -- and
# asserting the severity alone would pass on any warning at all. <expected> is
# therefore free to name which launcher won, which every row does.
#
# <stand-in launcher> is one of sccache, ccache or none, and names which of the
# module's launcher variables is pointed at a real program; the others are forced
# empty so the outcome does not depend on what the host happens to have
# installed. fastcache-cc is always forced empty — its own row is conditional on
# a daemon answering, which is a different property with its own test.
#
# The stand-in is never run: selection records a path and wires it as a compiler
# launcher, and nothing here builds. Passing it out of band rather than in the
# row is deliberate — the rows are split on spaces, and a program path on Windows
# contains them.
#
# No row may contain a ';' — these are CMake lists, and a semicolon inside a row
# would split it in two. An empty expected or forbidden field means "do not check
# that".

# The sccache row is the one that varies, because the hazard is the COMPILER's and
# not sccache's alone: MSVC and clang-cl are exposed, GCC and Clang are not, and
# the module carries the caveat on exactly that condition. So this asserts
# whichever half applies to the compiler this build was configured with -- the
# noise on Windows, the silence elsewhere -- and CI covering both covers both.
if(FASTCACHED_MSVC_LIKE)
    set(sccacheForbidden "")
    set(sccacheSeverity "warning")
else()
    set(sccacheForbidden "${FastCachedCaveatMarker}")
    set(sccacheSeverity "")
endif()

set(FastCachedCaveatRows
    "sccache-row|Enabling sccache|${sccacheForbidden}|sccache|${sccacheSeverity}|"
    "ccache-is-silent|Enabling ccache|${FastCachedCaveatMarker}|ccache||"
    "disabled-is-silent|disabled by USE_COMPILER_CACHE=OFF|${FastCachedCaveatMarker}|sccache||-DUSE_COMPILER_CACHE=OFF"
    "nothing-installed|No compiler-cache launcher found|${FastCachedCaveatMarker}|none||"
)

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
if(NOT EXISTS "${FASTCACHED_CXX_COMPILER}")
    message("SKIP: no C++ compiler at ${FASTCACHED_CXX_COMPILER}, so no project can be configured")
    return()
endif()

# Any real program will do: the module records the path and wires it as a
# launcher, and this script never builds. `cmake` itself is the one program a
# CMake script can always name.
set(standInProgram "${CMAKE_COMMAND}")

set(commonArguments
    "-DFASTCACHED_MODULE_DIR=${moduleDir}"
    "-DCMAKE_CXX_COMPILER=${FASTCACHED_CXX_COMPILER}")
if(FASTCACHED_MAKE_PROGRAM)
    list(APPEND commonArguments "-DCMAKE_MAKE_PROGRAM=${FASTCACHED_MAKE_PROGRAM}")
endif()
if(FASTCACHED_GENERATOR)
    list(APPEND commonArguments -G "${FASTCACHED_GENERATOR}")
endif()

# Canary. This is the first of these fixture-configuring checks that runs on
# Windows -- its three siblings skip there -- and a Windows shell that is not a
# Developer shell has a `cl.exe` that exists and cannot compile, because its
# INCLUDE and LIB come from the environment. Without this, every row below fails
# with a message blaming the caveat logic for a toolchain that was never set up.
#
# The same reasoning as scripts/tidy-sweep.sh's canary: a tool that cannot run
# produces silence, and silence read through an assertion looks like a verdict.
# Its build directory is removed first for that very reason -- a stale cache from
# a toolchain that has since moved would fail the canary rather than the rows, and
# `SKIP_REGULAR_EXPRESSION` would then retire this check on that build tree
# permanently and quietly.
file(REMOVE_RECURSE "${FASTCACHED_WORK_DIR}/canary")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${fixtureDir}" -B "${FASTCACHED_WORK_DIR}/canary"
            ${commonArguments}
            "-DFASTCACHE_CC=" "-DSCCACHE=" "-DCCACHE="
    RESULT_VARIABLE canaryResult
    OUTPUT_VARIABLE canaryOutput
    ERROR_VARIABLE canaryError
    TIMEOUT 300)
if(NOT canaryResult EQUAL 0)
    message("SKIP: the fixture project does not configure in this environment, so nothing below "
            "would be measuring the module (${canaryResult}):\n${canaryOutput}${canaryError}")
    return()
endif()

set(violations "")

foreach(row IN LISTS FastCachedCaveatRows)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 name)
    list(GET fields 1 expected)
    list(GET fields 2 forbidden)
    list(GET fields 3 standIn)
    list(GET fields 4 severity)
    list(GET fields 5 extra)

    set(extraArguments "")
    if(NOT extra STREQUAL "")
        string(REPLACE " " ";" extraArguments "${extra}")
    endif()

    # Every launcher named explicitly, so the row's outcome is the module's
    # decision and not the host's inventory. An empty value is what a `find_program`
    # cache variable reads as "already answered, and the answer is nothing", which
    # is also what a `none` row leaves all three as.
    set(sccachePath "")
    set(ccachePath "")
    if(standIn STREQUAL "sccache")
        set(sccachePath "${standInProgram}")
    elseif(standIn STREQUAL "ccache")
        set(ccachePath "${standInProgram}")
    endif()
    set(launcherArguments "-DFASTCACHE_CC=" "-DSCCACHE=${sccachePath}" "-DCCACHE=${ccachePath}")

    set(rowBinaryDir "${FASTCACHED_WORK_DIR}/${name}/build")
    file(REMOVE_RECURSE "${FASTCACHED_WORK_DIR}/${name}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -S "${fixtureDir}" -B "${rowBinaryDir}"
                ${commonArguments}
                ${launcherArguments}
                ${extraArguments}
        RESULT_VARIABLE configureResult
        OUTPUT_VARIABLE configureOutput
        ERROR_VARIABLE configureError
        TIMEOUT 300)
    # Merged, because a caveat is a `message(WARNING ...)` and therefore arrives
    # on stderr while the selection status line arrives on stdout.
    set(output "${configureOutput}${configureError}")

    # A caveat is a warning and must stay one: a module that aborted a configure
    # over a launcher it merely disapproves of would be worse than the hazard.
    # The canary above already proved the fixture configures here, so a failure at
    # this point is the module's and not the environment's.
    if(NOT configureResult EQUAL 0)
        list(APPEND violations "${name}: configure failed (${configureResult}); a caveat must never fail a configure\n${output}")
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

    # And that it arrived at the severity it is meant to. A caveat downgraded to a
    # status line still satisfies every text assertion above while losing the one
    # property it exists for: the symptom shows up hours later and somewhere else,
    # so the line naming it has to still be findable in the log.
    string(FIND "${output}" "CMake Warning" warnedAt)
    string(FIND "${output}" "${FastCachedCaveatMarker}" markerAt)
    if(severity STREQUAL "warning")
        if(warnedAt EQUAL -1)
            list(APPEND violations "${name}: the caveat must be a CMake Warning, not a status line\n${output}")
        endif()
        if(markerAt EQUAL -1)
            list(APPEND violations "${name}: expected the caveat itself, not merely some warning\n${output}")
        endif()
    elseif(NOT warnedAt EQUAL -1)
        list(APPEND violations "${name}: expected no CMake Warning at all\n${output}")
    endif()
endforeach()

if(violations)
    list(JOIN violations "\n\n" report)
    message(FATAL_ERROR
        "compile-cache caveat reporting is wrong:\n\n${report}\n\n"
        "See issue #153: sccache replays a stored hit's dependency paths, so a second "
        "checkout sharing the cache records dependencies into the first one and stops "
        "rebuilding on a header change. The module cannot fix that; it has to say it.")
endif()

list(LENGTH FastCachedCaveatRows rowCount)
message(STATUS "compile-cache caveat reporting is correct (${rowCount} selection outcomes)")
