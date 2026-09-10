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
# Since #815 the rows also pin the GATE, in both directions, because the caveat and
# the gate are one mechanism read from two ends: sccache is opt-in
# (`-DALLOW_SCCACHE_FALLBACK=ON`), so a row that asserts the caveat must now opt in
# to reach it -- and a row that does not opt in must see sccache passed over, said
# out loud, and no caveat, since a caveat about a launcher this build did not pick
# is noise. Which makes the opt-in row the one that keeps #170's warning alive: the
# alternative remedy for #815 was to have CI set `CMAKE_CXX_COMPILER_LAUNCHER`
# externally, which this module honours by returning EARLY -- taking the caveat out
# of the log with it, silently, on the platform where the hazard exists.
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
#   cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_SCRATCH_DIR=<scratch>
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

# ...and what the caveat must still SAY, beyond opening with that fragment.
#
#   <element>|<accepted spellings, ' / ' separated>
#
# The marker above pins how the warning starts, which is not the same as pinning
# that it remains useful: the text could lose `fastcache-cc` entirely, or stop
# naming which compilers are exposed, and every assertion here would still pass.
# The same three elements are required of every prose surface that recommends
# pointing sccache at fastcached (`ctest -R sccache-backend-caveat`, issue #170),
# and this is the caveat that reaches a developer at the moment of the mistake
# rather than hours later, so it is held to them too. Wording is free.
set(FastCachedCaveatElements
    "the exposed compilers|MSVC / clang-cl"
    "the mechanism|/showIncludes / /EP"
    "the remedy|fastcache-cc"
)

# One row per selection outcome, pipe-delimited:
#
#   <name>|<expected output>|<forbidden output>|<stand-in launcher>|<severity>|<extra -D args>|<wired>
#
# <wired> is what the GENERATED BUILDSYSTEM must carry as the compiler launcher:
# `sccache`, `ccache`, or `none`. It is the column #187 is about. Every other field
# here asserts what the module PRINTED, and a module that printed
# `-- [cache] Enabling sccache ...` with the full caveat at the right severity while
# wiring no launcher at all would satisfy all of them -- the same defect class the
# rest of this file exists to close, one layer down. The configure line is the
# module\'s claim about itself; the launcher in the generated buildsystem is the
# artefact a compile actually runs.
#
# <severity> is `warning` when the row must produce a CMake Warning carrying the
# caveat, and empty when it must produce no warning at all. Both halves are
# checked: asserting the text alone would let the caveat be downgraded to a
# `message(STATUS)` -- the one thing its own comment argues against, since the
# symptom arrives hours later and a status line does not survive a scroll -- and
# asserting the severity alone would pass on any warning at all. <expected> is
# therefore free to name which launcher won, which every row does.
#
# <stand-in launcher> is one of sccache, ccache, sccache-and-ccache or none, and
# names which of the module's launcher variables are pointed at a real program; the
# others are forced empty so the outcome does not depend on what the host happens
# to have installed. fastcache-cc is always forced empty — its own row is
# conditional on a daemon answering, which is a different property with its own
# test.
#
# `sccache-and-ccache` is the reporting host of #815 exactly: both installed, and
# the question is which one a build gets when nobody said. "Nothing is selected"
# and "the next launcher is selected" are two different proofs that sccache was not
# taken automatically, and only the second one is the situation people are in.
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
    "sccache-row|Enabling sccache|${sccacheForbidden}|sccache|${sccacheSeverity}|-DALLOW_SCCACHE_FALLBACK=ON|sccache"
    "sccache-not-auto|Not using sccache|Enabling sccache|sccache|||none"
    "sccache-not-preferred|Enabling ccache|Enabling sccache|sccache-and-ccache|||ccache"
    "ccache-is-silent|Enabling ccache|${FastCachedCaveatMarker}|ccache|||ccache"
    "disabled-is-silent|disabled by USE_COMPILER_CACHE=OFF|${FastCachedCaveatMarker}|sccache||-DUSE_COMPILER_CACHE=OFF|none"
    "nothing-installed|No compiler-cache launcher found|${FastCachedCaveatMarker}|none|||none"
)

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
if(NOT EXISTS "${FASTCACHED_CXX_COMPILER}")
    message("SKIP: no C++ compiler at ${FASTCACHED_CXX_COMPILER}, so no project can be configured")
    return()
endif()

# Any real program will do: the module records the path and wires it as a
# launcher, and this script never builds. `cmake` and `ctest` are the two programs
# a CMake script can always name, and they sit beside each other in every install.
#
# **Two rather than one, and they must DIFFER** (#187). One program for both
# launchers makes the wiring assertion below a presence check -- it could say a
# launcher was wired and never which -- so `sccache-not-preferred`, whose entire
# claim is that ccache won over sccache, would be proved by a status line and by
# nothing in the buildsystem. Distinct paths make the row's claim readable off the
# artefact a compile actually runs.
set(sccacheStandIn "${CMAKE_COMMAND}")
set(ccacheStandIn "${CMAKE_CTEST_COMMAND}")
if(sccacheStandIn STREQUAL ccacheStandIn OR ccacheStandIn STREQUAL "")
    message(FATAL_ERROR
        "the two stand-in launchers must be different real programs, or the wiring assertion "
        "cannot tell which launcher was wired: sccache=[${sccacheStandIn}] ccache=[${ccacheStandIn}]")
endif()

# One path, two spellings. Compare the PATH, never the bytes a generator wrote.
#
# Measured on the three Windows legs, which is where this failed and where the Linux
# run could not have shown it. `build.ninja` writes
#
#     LAUNCHER = "C:\Program Files\CMake\bin\cmake.exe"
#
# -- QUOTED because the path holds a space, and with BACKSLASH separators -- while
# `${CMAKE_COMMAND}` is `C:/Program Files/CMake/bin/cmake.exe`. A `STREQUAL` over
# those is false for one and the same file, and on Linux the two spellings are
# identical, so a green run there says nothing about it. The strings are recorded
# rather than described, so nobody simplifies the normalisation away on the platform
# that does not need it.
#
# Three normalisations, each a FACT about the spelling rather than a loosening of the
# comparison: a matched surrounding pair of double quotes is the generator's escaping
# and not part of the name; `file(TO_CMAKE_PATH)` is what CMake itself uses to settle
# separators; and the comparison folds case only on Windows, where the filesystem
# does. Nothing else is relaxed -- a wrong launcher must still read as wrong, which
# is the whole of what this rule buys.
#
# @param raw The value as the buildsystem spells it.
# @param outVar Receives the comparable form.
function(FastCachedNormalisePath raw outVar)
    set(value "${raw}")
    string(STRIP "${value}" value)
    # Only a MATCHED pair: stripping an unmatched quote would silently accept a
    # truncated value, which is the one way this could go wrong quietly.
    if(value MATCHES "^\"(.*)\"$")
        set(value "${CMAKE_MATCH_1}")
    endif()
    file(TO_CMAKE_PATH "${value}" value)
    if(WIN32)
        string(TOLOWER "${value}" value)
    endif()
    set(${outVar} "${value}" PARENT_SCOPE)
endfunction()

# What a generated buildsystem says about the compiler launcher.
#
# `CMAKE_CXX_COMPILER_LAUNCHER` is set by the module as a NORMAL variable, so
# `CMakeCache.txt` cannot answer this -- measured across eleven build trees, unset
# in every one, five of them demonstrably running sccache. What carries it is the
# generated buildsystem, which spells it differently per generator: Ninja emits a
# per-rule `LAUNCHER = <path>` variable, and the Makefile generators put the
# program at the head of the compile command in `build.make`.
#
# @param binaryDir The configured build tree.
# @param launchersVar Receives the distinct launcher paths found, as a list.
# @param readableVar Receives `yes` when this generator's buildsystem could be
#        read at all, and `no` when it could not -- which is a THIRD state and is
#        reported as one, never folded into "no launcher was wired".
function(FastCachedWiredLaunchers binaryDir launchersVar readableVar)
    set(found "")
    set(readable "no")

    if(EXISTS "${binaryDir}/build.ninja")
        set(readable "yes")
        file(STRINGS "${binaryDir}/build.ninja" launcherLines REGEX "^ *LAUNCHER *= *.+$")
        foreach(line IN LISTS launcherLines)
            string(REGEX REPLACE "^ *LAUNCHER *= *" "" value "${line}")
            FastCachedNormalisePath("${value}" value)
            if(NOT value STREQUAL "")
                list(APPEND found "${value}")
            endif()
        endforeach()
    else()
        file(GLOB_RECURSE makeFiles "${binaryDir}/CMakeFiles/*/build.make")
        if(makeFiles)
            set(readable "yes")
            foreach(makeFile IN LISTS makeFiles)
                file(STRINGS "${makeFile}" compileLines REGEX "CXX_COMPILER_LAUNCHER|\$\(CXX_DEFINES\)")
                foreach(line IN LISTS compileLines)
                    if(line MATCHES "^[ 	]*[^ 	]*(cmake|ctest)[^ 	]*[ 	]+[^ 	]*(c\+\+|clang|gcc|cl)")
                        FastCachedNormalisePath("${CMAKE_MATCH_0}" makeValue)
                        list(APPEND found "${makeValue}")
                    endif()
                endforeach()
            endforeach()
        endif()
    endif()

    list(REMOVE_DUPLICATES found)
    set(${launchersVar} "${found}" PARENT_SCOPE)
    set(${readableVar} "${readable}" PARENT_SCOPE)
endfunction()

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
file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}/canary")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${fixtureDir}" -B "${FASTCACHED_SCRATCH_DIR}/canary"
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

# How many rows had their WIRING read, as opposed to merely their output.
# Asserted after the loop: a reader that stopped matching would leave every row
# silently unasserted, and a check that asserts nothing reports clean -- which is
# the shape #187 is about, arriving in #187's own fix.
set(wiringRowsRead 0)
set(wiringGenerators "")

foreach(row IN LISTS FastCachedCaveatRows)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 name)
    list(GET fields 1 expected)
    list(GET fields 2 forbidden)
    list(GET fields 3 standIn)
    list(GET fields 4 severity)
    list(GET fields 5 extra)
    list(GET fields 6 wired)

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
    # Exact names, never a substring match: "sccache" CONTAINS "ccache", so a
    # `MATCHES` would point BOTH variables at a program for the sccache row and
    # quietly test a different situation than the row says it does. An unknown name
    # is refused rather than silently behaving like `none`, which would be a row
    # that configures, asserts and proves nothing.
    string(REPLACE "-and-" ";" standInNames "${standIn}")
    foreach(standInName IN LISTS standInNames)
        if(standInName STREQUAL "sccache")
            set(sccachePath "${sccacheStandIn}")
        elseif(standInName STREQUAL "ccache")
            set(ccachePath "${ccacheStandIn}")
        elseif(NOT standInName STREQUAL "none")
            message(FATAL_ERROR "row ${name}: unknown stand-in launcher '${standInName}'")
        endif()
    endforeach()
    set(launcherArguments "-DFASTCACHE_CC=" "-DSCCACHE=${sccachePath}" "-DCCACHE=${ccachePath}")

    set(rowBinaryDir "${FASTCACHED_SCRATCH_DIR}/${name}/build")
    file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}/${name}")

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
        else()
            # It is the caveat. Is it still a useful one?
            foreach(elementRow IN LISTS FastCachedCaveatElements)
                string(FIND "${elementRow}" "|" elementSeparator)
                string(SUBSTRING "${elementRow}" 0 ${elementSeparator} elementName)
                math(EXPR elementSpellingStart "${elementSeparator} + 1")
                string(SUBSTRING "${elementRow}" ${elementSpellingStart} -1 elementSpellings)

                set(elementPresent FALSE)
                string(REPLACE " / " ";" spellings "${elementSpellings}")
                foreach(spelling IN LISTS spellings)
                    string(FIND "${output}" "${spelling}" spellingAt)
                    if(NOT spellingAt EQUAL -1)
                        set(elementPresent TRUE)
                        break()
                    endif()
                endforeach()
                if(NOT elementPresent)
                    list(APPEND violations
                        "${name}: the caveat no longer names ${elementName} (${elementSpellings})\n${output}")
                endif()
            endforeach()
        endif()
    elseif(NOT warnedAt EQUAL -1)
        list(APPEND violations "${name}: expected no CMake Warning at all\n${output}")
    endif()

    # And what it WIRED (#187). Everything above reads the configure output, which
    # is the module's claim about itself; this reads the generated buildsystem,
    # which is the artefact a compile runs through.
    FastCachedWiredLaunchers("${rowBinaryDir}" rowLaunchers rowReadable)
    if(rowReadable STREQUAL "yes")
        math(EXPR wiringRowsRead "${wiringRowsRead} + 1")

        set(wantedLauncher "")
        if(wired STREQUAL "sccache")
            FastCachedNormalisePath("${sccacheStandIn}" wantedLauncher)
        elseif(wired STREQUAL "ccache")
            FastCachedNormalisePath("${ccacheStandIn}" wantedLauncher)
        elseif(NOT wired STREQUAL "none")
            # Refused rather than treated as `none`, which would be a row that
            # configures, asserts and proves nothing -- the same reason the
            # stand-in column refuses an unknown name.
            message(FATAL_ERROR "row ${name}: unknown wired launcher '${wired}'")
        endif()

        if(wantedLauncher STREQUAL "")
            if(NOT rowLaunchers STREQUAL "")
                list(APPEND violations
                     "${name}: the buildsystem wires a compiler launcher [${rowLaunchers}] where this row expects none")
            endif()
        else()
            if(rowLaunchers STREQUAL "")
                list(APPEND violations
                     "${name}: the module said so but the buildsystem wires NO compiler launcher; the configure line is a claim, the buildsystem is the artefact\n${output}")
            else()
                # Every binding, not merely one of them: a module that wired the
                # right launcher for one target and the wrong one for another would
                # satisfy a check that stopped at the first match, and a partially
                # cached build is exactly the state this whole file is about.
                foreach(oneLauncher IN LISTS rowLaunchers)
                    if(NOT oneLauncher STREQUAL "${wantedLauncher}")
                        list(APPEND violations
                             "${name}: the buildsystem wires [${oneLauncher}] where this row expects the ${wired} stand-in [${wantedLauncher}]")
                    endif()
                endforeach()
            endif()
        endif()
    else()
        # THIRD state, and it is reported rather than folded into "nothing was
        # wired". A generator whose buildsystem this cannot read leaves the row's
        # wiring unasserted, and an unasserted rule that renders like a passing one
        # is what the ticket this closes is about.
        list(APPEND wiringGenerators "${name}")
    endif()
endforeach()

# The wiring rule, summarised -- and asserted, because the ways it can quietly
# stop applying are the interesting ones.
if(NOT wiringGenerators STREQUAL "")
    message("compile-cache caveat: the generated buildsystem could not be read for "
            "${wiringGenerators}, so what those rows WIRED was not asserted "
            "(generator: ${FASTCACHED_GENERATOR})")
endif()
if(wiringRowsRead EQUAL 0)
    if(FASTCACHED_GENERATOR MATCHES "Ninja|Makefiles")
        # A generator this reader claims to handle, and it read nothing: the reader
        # has stopped matching. A violation rather than a note, because every row's
        # wiring assertion is then vacuous and the check reports clean.
        list(APPEND violations
             "the wiring reader matched no row on a ${FASTCACHED_GENERATOR} tree, so nothing asserted what any row WIRED; that is the defect this rule exists to remove")
    else()
        message("compile-cache caveat: no row's wiring was read on this generator "
                "(${FASTCACHED_GENERATOR}); the output rules above still ran")
    endif()
else()
    message("compile-cache caveat: read the wired launcher for ${wiringRowsRead} row(s)")
endif()

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
