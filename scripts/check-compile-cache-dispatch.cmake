# SPDX-License-Identifier: Apache-2.0
#
# Whether configure's `[cache] dispatch` line is TRUE of the build it generates, and whether the
# two settings the configure environment SEEDS stay where a `-D` put them.
#
# fastcache-cc dispatches a miss to a compile fleet when `FASTCACHE_SCHEDULER` names one, and it
# read that only from the environment of whatever ran the build. So a developer who set it in one
# shell and built from another got a build that compiled everything locally, with configure having
# said only that fastcache-cc was the launcher: measured on a workstation, 341 misses and
# `NOT_CONFIGURED` on every one of them, found from a fleet dashboard reading 0 compiling.
# `cmake/portable/CompileCache.cmake` now decides dispatch at configure, holds it in the cache,
# bakes it into the launcher and says which way it decided (`_fc_hold_setting`,
# `_fc_dispatch_state`). `FASTCACHE_ADDR` is held by the same rule: the environment seeds a tree
# with no entry, and a later difference is REPORTED and never applied, because a rule that let
# the environment retarget a held value could not see a `-D` typing the value already held.
#
# Two halves, because a configure's OUTPUT is the module's CLAIM and the generated buildsystem is
# the artefact a compile runs (#187):
#
#   1. The DECISION, as a pure computation over every combination of a cache entry and the
#      configure environment -- including the ones a shell cannot easily stage. A failure here is
#      reported and half 2 still runs, so one broken function does not hide what the wiring does.
#   2. The WIRING: the fixture project configured with a stand-in fastcache-cc, in sequences that
#      reconfigure one build tree the ways a real one is reconfigured -- by hand with and without a
#      `-D`, from shells that do and do not hold the variables, and by ninja's own regeneration --
#      with the values every `LAUNCHER =` line of the generated build.ninja carries compared with
#      the table AND with what the configure printed, directly.
#
# The stand-in is BUILT here, from a few lines of C++, rather than being a script: the module's
# probe runs it as a program and requires `fastcache-cc: MISS key=` on stderr, and a shell script
# is not a program on Windows -- where this defect was measured. A toolchain that cannot build it
# is reported with `SKIP: `, never as a verdict about the module.
#
# Verdict is read from the OUTPUT, not the exit code; see src/tests/CMakeLists.txt, which
# registers this with FAIL_REGULAR_EXPRESSION (#565).

cmake_minimum_required(VERSION 3.28)

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR FASTCACHED_CXX_COMPILER)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set (cmake -D${required}=... -P ${CMAKE_CURRENT_LIST_FILE})")
    endif()
endforeach()

# Under `cmake -P` the module returns after its pure computations.
include("${FASTCACHED_SOURCE_DIR}/cmake/portable/CompileCache.cmake")
foreach(command _fc_hold_setting _fc_unapplied_note _fc_dispatch_state)
    if(NOT COMMAND ${command})
        message(FATAL_ERROR
            "${command} is not defined: the define-only guard in cmake/portable/CompileCache.cmake returned "
            "before it, or it was renamed and this check now proves nothing.")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

# ---------------------------------------------------------------------------
# Half 1: the decision.
#
# setting | cached | cacheValue | envPresent | envValue | expected value | fragment | about
#
# For FASTCACHE_SCHEDULER the fragment is looked for in the dispatch line. For FASTCACHE_ADDR it is
# looked for in the unapplied-value note, and `<none>` means no note may be produced. `envValue`
# is what the module passes: the raw scheduler, and for the address what `_fc_resolve_addr`
# resolved -- the default when the name is absent. No fragment may hold a ';' (CheckCommon.cmake).
set(decisionRows
    "FASTCACHE_SCHEDULER|OFF||OFF|||dispatch: off (no FASTCACHE_SCHEDULER in the configure environment, and this build tree now holds that|first configure, nothing said anywhere: off, and it says how to turn it on"
    "FASTCACHE_SCHEDULER|OFF||ON|10.0.0.9:6674|10.0.0.9:6674|dispatch: 10.0.0.9:6674 (configured, seeded from the configure environment|first configure: the environment seeds"
    "FASTCACHE_SCHEDULER|ON|10.0.0.9:6674|OFF||10.0.0.9:6674|dispatch: 10.0.0.9:6674 (configured, held in this build tree's cache|a re-run from a shell WITHOUT the variable keeps it -- the measured failure"
    "FASTCACHE_SCHEDULER|ON|10.0.0.9:6674|ON|10.0.0.5:6674|10.0.0.9:6674|the 10.0.0.5:6674 in the environment is NOT applied -- -DFASTCACHE_SCHEDULER=10.0.0.5:6674 retargets it|a NEW environment value is reported and never applied"
    "FASTCACHE_SCHEDULER|ON|10.0.0.9:6674|ON|10.0.0.9:6674|10.0.0.9:6674|not probed -- each compile records DISPATCHED or DECLINED in invocations.log|the same environment value: nothing unapplied, and the line says it is configuration"
    "FASTCACHE_SCHEDULER|ON|10.0.0.7:6674|ON|10.0.0.9:6674|10.0.0.7:6674|dispatch: 10.0.0.7:6674 (configured, held in this build tree's cache, where a -DFASTCACHE_SCHEDULER|a -D on the first configure outranks the environment"
    "FASTCACHE_SCHEDULER|ON|10.0.0.5:6674|ON|10.0.0.1:6674|10.0.0.5:6674|dispatch: 10.0.0.5:6674 (configured, held|a -D equal to the value already held still outranks a new environment value -- measured retargeted before"
    "FASTCACHE_SCHEDULER|ON||ON|10.0.0.9:6674||which outranks the 10.0.0.9:6674 in the environment|an EMPTY -D turns dispatch off whatever the environment says -- measured turning it ON before"
    "FASTCACHE_SCHEDULER|ON||ON|10.0.0.9:6674||-DFASTCACHE_SCHEDULER=10.0.0.9:6674 turns it on)|an off that outranks the environment says how to take the environment's value"
    "FASTCACHE_SCHEDULER|ON||OFF|||dispatch: off (FASTCACHE_SCHEDULER is empty in this build tree's cache|an off held from before, with nothing in the environment"
    "FASTCACHE_ADDR|OFF||OFF|127.0.0.1:6674|127.0.0.1:6674|<none>|first configure without the name: the default seeds"
    "FASTCACHE_ADDR|OFF||ON|||<none>|first configure with the name set but EMPTY: the opt-out seeds (#372)"
    "FASTCACHE_ADDR|ON|127.0.0.1:6674|OFF|127.0.0.1:6674|127.0.0.1:6674|<none>|a shell without the name says nothing"
    "FASTCACHE_ADDR|ON|127.0.0.1:6674|ON|10.0.0.5:6674|127.0.0.1:6674|[cache] FASTCACHE_ADDR is '10.0.0.5:6674' in the environment and NOT applied: this build tree holds '127.0.0.1:6674', and the environment only seeds a tree with no FASTCACHE_ADDR entry -- -DFASTCACHE_ADDR=10.0.0.5:6674 applies it|a later environment is reported, never applied"
    "FASTCACHE_ADDR|ON||ON|10.0.0.5:6674||this build tree holds it empty|an opt-out -D outranks an address in the environment"
    "FASTCACHE_ADDR|ON|10.0.0.5:6674|ON||10.0.0.5:6674|FASTCACHE_ADDR is set but empty in the environment and NOT applied|a set-but-empty environment does not opt a held tree out: -DFASTCACHE_ADDR= does"
    "FASTCACHE_ADDR|ON|10.0.0.5:6674|ON|10.0.0.5:6674|10.0.0.5:6674|<none>|the same address in the environment is no difference"
)

# One row that must fail, for `compile-cache-dispatch-verdict` alone: it asks how a decision
# failure is SCORED, which only a failing decision can answer. Nothing else sets this.
if(FASTCACHED_DISPATCH_PLANT_DECISION_FAILURE)
    list(APPEND decisionRows
         "FASTCACHE_SCHEDULER|OFF||OFF|||a fragment no dispatch line holds|planted by compile-cache-dispatch-verdict")
endif()

# Where half 1's failures wait for half 2. Reported at every exit below; on an exit before the
# wiring they are the verdict (EndWithoutWiring), since a toolchain that cannot build the stand-in
# says nothing about the pure functions.
set(decisionFailures "")
set(decisions 0)
set(decidedOn 0)
set(decidedOff 0)
set(decidedNotes 0)
foreach(row IN LISTS decisionRows)
    fastcached_row_fields("${row}" setting cached cacheValue envPresent envValue wantValue wantFragment about)
    _fc_hold_setting("${cached}" "${cacheValue}" "${envPresent}" "${envValue}" gotValue gotSeeded gotUnapplied)
    math(EXPR decisions "${decisions} + 1")

    if(NOT "${gotValue}" STREQUAL "${wantValue}")
        list(APPEND decisionFailures "${setting} '${about}': held '${gotValue}', expected '${wantValue}'")
    endif()
    if(cached AND gotSeeded)
        list(APPEND decisionFailures "${setting} '${about}': reports a value it held as seeded")
    endif()

    if(setting STREQUAL "FASTCACHE_SCHEDULER")
        _fc_dispatch_state("${gotValue}" "${gotSeeded}" "${gotUnapplied}" "${envValue}" gotLine gotEntry)
        # One prefix for both states, so a reader or a script looking for `[cache] dispatch:` finds
        # the line whichever way the configure decided.
        string(FIND "${gotLine}" "[cache] dispatch: " prefixAt)
        if(NOT prefixAt EQUAL 0)
            list(APPEND decisionFailures "${setting} '${about}': said '${gotLine}', which does not open with the "
                                         "'[cache] dispatch: ' both states share")
        endif()
        # The claim and the artefact from one value: the entry is baked in BOTH states, empty when
        # off, which is what stops the build's environment turning on a dispatch said to be off.
        if(NOT "${gotEntry}" STREQUAL "FASTCACHE_SCHEDULER=${wantValue}")
            list(APPEND decisionFailures
                 "${setting} '${about}': bakes '${gotEntry}', expected 'FASTCACHE_SCHEDULER=${wantValue}'")
        endif()
        if("${wantValue}" STREQUAL "")
            math(EXPR decidedOff "${decidedOff} + 1")
        else()
            math(EXPR decidedOn "${decidedOn} + 1")
        endif()
    else()
        set(gotLine "<none>")
        if(gotUnapplied)
            _fc_unapplied_note("${setting}" "${gotValue}" "${envValue}" gotLine)
            math(EXPR decidedNotes "${decidedNotes} + 1")
        endif()
    endif()
    string(FIND "${gotLine}" "${wantFragment}" at)
    if(at EQUAL -1)
        list(APPEND decisionFailures "${setting} '${about}': said '${gotLine}', expected it to contain '${wantFragment}'")
    endif()
endforeach()

if(decisions EQUAL 0 OR decidedOn EQUAL 0 OR decidedOff EQUAL 0 OR decidedNotes EQUAL 0)
    message(FATAL_ERROR
        "check-compile-cache-dispatch: ${decisions} decision row(s): ${decidedOn} dispatch on, ${decidedOff} off, "
        "${decidedNotes} unapplied address; a state is unexercised, so the others pass with nothing to disagree with them.")
endif()

# Every exit below reports half 1 first.
macro(ReportDecisionFailures)
    if(decisionFailures)
        string(REPLACE ";" "\n" decisionReport "${decisionFailures}")
        message(WARNING "check-compile-cache-dispatch: decision mismatch(es) over ${decisions} rows:\n${decisionReport}")
    endif()
endmacro()

# An exit before half 2 could run. Only the WIRING is unmeasured there: half 1 needs no toolchain
# and was judged, so a decision failure ends the check FAILED and never with the skip marker.
# ctest lets SKIP_REGULAR_EXPRESSION outrank FAIL_REGULAR_EXPRESSION -- measured, a `CMake
# Warning` followed by the marker scores `***Skipped` -- so a failure reported beside the marker
# is a failure nobody sees. `compile-cache-dispatch-verdict` holds a planted decision failure with
# no compiler to FAILED, under this check's own registration properties.
#
# The reason arrives in a VARIABLE, `wiringUnmeasured`: a macro substitutes its arguments
# textually, and the stand-in's reason carries a toolchain's output -- which is also why the
# failure spells the marker out of it.
macro(EndWithoutWiring)
    ReportDecisionFailures()
    if(decisionFailures)
        string(REPLACE "SKIP: " "SKIP- " wiringUnmeasuredInFailure "${wiringUnmeasured}")
        message(FATAL_ERROR "check-compile-cache-dispatch: the decisions above fail; the wiring was not measured, "
                            "because ${wiringUnmeasuredInFailure}")
    endif()
    message("SKIP: ${wiringUnmeasured}")
    return()
endmacro()

# ---------------------------------------------------------------------------
# Half 2: the wiring.

set(fixtureDir "${FASTCACHED_SOURCE_DIR}/src/tests/compile-cache-fixture")
set(moduleDir "${FASTCACHED_SOURCE_DIR}/cmake/portable")
if(NOT EXISTS "${FASTCACHED_CXX_COMPILER}")
    set(wiringUnmeasured "no C++ compiler at ${FASTCACHED_CXX_COMPILER}, so no project can be configured")
    EndWithoutWiring()
endif()

set(commonArguments "-DCMAKE_CXX_COMPILER=${FASTCACHED_CXX_COMPILER}")
if(FASTCACHED_MAKE_PROGRAM)
    list(APPEND commonArguments "-DCMAKE_MAKE_PROGRAM=${FASTCACHED_MAKE_PROGRAM}")
endif()
if(FASTCACHED_GENERATOR)
    list(APPEND commonArguments -G "${FASTCACHED_GENERATOR}")
endif()

# The stand-in: a program that reports a cache miss and exits 0, which is everything the module's
# probe asks of fastcache-cc. It is never run by a build -- nothing here builds the fixture.
set(standInSource "${FASTCACHED_SCRATCH_DIR}/stand-in")
set(standInBuild "${FASTCACHED_SCRATCH_DIR}/stand-in-build")
file(REMOVE_RECURSE "${standInSource}" "${standInBuild}")
file(WRITE "${standInSource}/CMakeLists.txt"
     "cmake_minimum_required(VERSION 3.28)\nproject(fastcache-cc-stand-in LANGUAGES CXX)\n"
     "add_executable(fastcache-cc-stand-in main.cpp)\n")
file(WRITE "${standInSource}/main.cpp"
     "#include <cstdio>\nint main() { std::fputs(\"fastcache-cc: MISS key=dispatch-stand-in\\n\", stderr); return 0; }\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${standInSource}" -B "${standInBuild}" ${commonArguments}
    RESULT_VARIABLE standInConfigured OUTPUT_VARIABLE standInOutput ERROR_VARIABLE standInError TIMEOUT 300)
if(standInConfigured EQUAL 0)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${standInBuild}" --config Release
        RESULT_VARIABLE standInBuilt OUTPUT_VARIABLE standInOutput ERROR_VARIABLE standInError TIMEOUT 300)
else()
    set(standInBuilt "${standInConfigured}")
endif()
# One traversal, filtered: a GLOB_RECURSE with two patterns walks the tree twice (#502).
file(GLOB_RECURSE standInCandidates LIST_DIRECTORIES false "${standInBuild}/*")
list(FILTER standInCandidates INCLUDE REGEX "/fastcache-cc-stand-in(\\.exe)?$")
if(NOT standInBuilt EQUAL 0 OR NOT standInCandidates)
    # Not a verdict: a Windows shell that is not a Developer shell has a cl.exe that cannot compile.
    string(REPLACE "CMake Error" "CMake-Error" standInReport "${standInOutput}${standInError}")
    string(REPLACE "CMake Warning" "CMake-Warning" standInReport "${standInReport}")
    string(CONCAT wiringUnmeasured "the stand-in fastcache-cc does not build with this toolchain (${standInBuilt}), "
                  "so no configure below would be measuring the module:\n${standInReport}")
    EndWithoutWiring()
endif()
list(GET standInCandidates 0 standIn)

# Every value of one setting that the generated buildsystem's launcher lines carry.
#
# Read off build.ninja's per-rule `LAUNCHER =` lines, never the cache: the module sets the launcher
# as a normal variable, which `CMakeCache.txt` does not record (#187). The fixture is always
# configured with Ninja here when the parent build is -- another generator is reported as
# unreadable, a THIRD state, never as "nothing wired".
# @param binaryDir The configured fixture.
# @param name The setting, as the launcher's environment names it.
# @param valuesVar Receives the distinct values, as a list; an empty value is kept as `<empty>`
#                  and a line without the setting as `<absent>`.
# @param linesVar Receives how many LAUNCHER lines were read.
function(LauncherValues binaryDir name valuesVar linesVar)
    set(found "")
    set(lines 0)
    if(EXISTS "${binaryDir}/build.ninja")
        file(STRINGS "${binaryDir}/build.ninja" launcherLines REGEX "^ *LAUNCHER *= *.+$")
        foreach(line IN LISTS launcherLines)
            math(EXPR lines "${lines} + 1")
            if(line MATCHES "${name}=([^ \"]*)")
                # QUOTED: a group that matched nothing leaves CMAKE_MATCH_1 UNSET (measured,
                # CMAKE_MATCH_COUNT 0), and `if(VAR STREQUAL "")` does not fire for an unset VAR.
                if("${CMAKE_MATCH_1}" STREQUAL "")
                    list(APPEND found "<empty>")
                else()
                    list(APPEND found "${CMAKE_MATCH_1}")
                endif()
            else()
                list(APPEND found "<absent>")
            endif()
        endforeach()
    endif()
    list(REMOVE_DUPLICATES found)
    set(${valuesVar} "${found}" PARENT_SCOPE)
    set(${linesVar} "${lines}" PARENT_SCOPE)
endfunction()

# What the configure's own dispatch line CLAIMS, parsed back out of its output: the endpoint, or
# `<empty>` for off. Compared with the wired value directly, so a row wrong in both its expected
# columns cannot pass by agreeing with itself.
# @param output The configure's output.
# @param claimVar Receives the claimed value, or `<unreadable: ...>` when there is not exactly one line.
function(DispatchClaim output claimVar)
    string(REGEX MATCHALL "\\[cache\\] dispatch[ :]" said "${output}")
    list(LENGTH said saidCount)
    if(NOT saidCount EQUAL 1)
        set(${claimVar} "<unreadable: ${saidCount} dispatch lines>" PARENT_SCOPE)
    elseif(output MATCHES "\\[cache\\] dispatch: off \\(")
        set(${claimVar} "<empty>" PARENT_SCOPE)
    elseif(output MATCHES "\\[cache\\] dispatch: ([^ ]+) \\(configured, ")
        set(${claimVar} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${claimVar} "<unreadable: a dispatch line of neither form>" PARENT_SCOPE)
    endif()
endfunction()

# name | step | environment | defines | wired scheduler | wired address | fragment
#
#   step         fresh (a new build tree), again (`cmake -S -B` over the previous row's tree), or
#                regenerate (the previous row's tree, re-run by NINJA: CMakeCache.txt is touched and
#                `build.ninja` is built, which is the path a CMakeLists.txt edit takes mid-build).
#   environment  `NAME=value` items, space-separated, or `-`. Both settings are UNSET unless named
#                here: the process running ctest may well hold one, and this project's own build
#                instructions set FASTCACHE_SCHEDULER.
#   defines      `NAME=value` items passed as `-D`, value possibly empty, or `-`. A regenerate row
#                takes none, since ninja passes none.
#   wired        What every LAUNCHER line must carry; `<empty>` for a present, empty value.
#   fragment     Text the run's output must contain.
#
# The `again` and `regenerate` rows are the ones that matter most, because every defect this
# module has had here lived in a SECOND configure: ninja's re-run turning dispatch off, a retarget
# turning an explicit `-DFASTCACHE_SCHEDULER=` back on, and the bookkeeping a retarget needed never
# being written.
set(wiringRows
    "unset-is-off|fresh|-|FASTCACHE_ADDR=127.0.0.1:1|<empty>|127.0.0.1:1|dispatch: off (no FASTCACHE_SCHEDULER in the configure environment"
    "environment-seeds|fresh|FASTCACHE_SCHEDULER=10.0.0.9:6674|FASTCACHE_ADDR=127.0.0.1:1|10.0.0.9:6674|127.0.0.1:1|dispatch: 10.0.0.9:6674 (configured, seeded from the configure environment"
    "rerun-without-it-keeps-it|again|-|FASTCACHE_ADDR=127.0.0.1:1|10.0.0.9:6674|127.0.0.1:1|dispatch: 10.0.0.9:6674 (configured, held in this build tree's cache"
    "ninja-regeneration-without-it-keeps-it|regenerate|-|-|10.0.0.9:6674|127.0.0.1:1|dispatch: 10.0.0.9:6674 (configured, held in this build tree's cache"
    "a-new-environment-is-reported-not-applied|again|FASTCACHE_SCHEDULER=10.0.0.5:6674|FASTCACHE_ADDR=127.0.0.1:1|10.0.0.9:6674|127.0.0.1:1|-DFASTCACHE_SCHEDULER=10.0.0.5:6674 retargets it"
    "a-define-retargets|again|FASTCACHE_SCHEDULER=10.0.0.5:6674|FASTCACHE_ADDR=127.0.0.1:1 FASTCACHE_SCHEDULER=10.0.0.5:6674|10.0.0.5:6674|127.0.0.1:1|dispatch: 10.0.0.5:6674 (configured, held"
    "a-define-equal-to-the-held-value-outranks-the-environment|again|FASTCACHE_SCHEDULER=10.0.0.1:6674|FASTCACHE_ADDR=127.0.0.1:1 FASTCACHE_SCHEDULER=10.0.0.5:6674|10.0.0.5:6674|127.0.0.1:1|the 10.0.0.1:6674 in the environment is NOT applied"
    "a-held-define-survives-a-rerun-with-the-environment|again|FASTCACHE_SCHEDULER=10.0.0.1:6674|FASTCACHE_ADDR=127.0.0.1:1|10.0.0.5:6674|127.0.0.1:1|dispatch: 10.0.0.5:6674 (configured, held"
    "a-held-define-survives-ninja-regeneration-with-the-environment|regenerate|FASTCACHE_SCHEDULER=10.0.0.1:6674|-|10.0.0.5:6674|127.0.0.1:1|the 10.0.0.1:6674 in the environment is NOT applied"
    "empty-define-turns-it-off|again|FASTCACHE_SCHEDULER=10.0.0.5:6674|FASTCACHE_ADDR=127.0.0.1:1 FASTCACHE_SCHEDULER=|<empty>|127.0.0.1:1|which outranks the 10.0.0.5:6674 in the environment"
    "off-survives-a-rerun-with-the-environment|again|FASTCACHE_SCHEDULER=10.0.0.5:6674|FASTCACHE_ADDR=127.0.0.1:1|<empty>|127.0.0.1:1|-DFASTCACHE_SCHEDULER=10.0.0.5:6674 turns it on)"
    "measured-an-off-tree|fresh|-|FASTCACHE_ADDR=127.0.0.1:1|<empty>|127.0.0.1:1|dispatch: off (no FASTCACHE_SCHEDULER in the configure environment"
    "measured-stays-off-under-an-empty-define-from-a-shell-with-it|again|FASTCACHE_SCHEDULER=10.0.0.9:6674|FASTCACHE_ADDR=127.0.0.1:1 FASTCACHE_SCHEDULER=|<empty>|127.0.0.1:1|which outranks the 10.0.0.9:6674 in the environment"
    "measured-stays-off-without-a-define-from-a-shell-with-it|again|FASTCACHE_SCHEDULER=10.0.0.9:6674|FASTCACHE_ADDR=127.0.0.1:1|<empty>|127.0.0.1:1|which outranks the 10.0.0.9:6674 in the environment"
    "a-define-outranks-the-environment-on-a-first-configure|fresh|FASTCACHE_SCHEDULER=10.0.0.9:6674|FASTCACHE_ADDR=127.0.0.1:1 FASTCACHE_SCHEDULER=10.0.0.7:6674|10.0.0.7:6674|127.0.0.1:1|dispatch: 10.0.0.7:6674 (configured, held in this build tree's cache, where a -DFASTCACHE_SCHEDULER"
    "address-seeded-from-the-environment|fresh|FASTCACHE_ADDR=127.0.0.1:2|-|<empty>|127.0.0.1:2|Enabling fastcache-cc"
    "address-held-over-a-new-environment|again|FASTCACHE_ADDR=127.0.0.1:3|-|<empty>|127.0.0.1:2|[cache] FASTCACHE_ADDR is '127.0.0.1:3' in the environment and NOT applied: this build tree holds '127.0.0.1:2'"
    "address-define-equal-to-the-held-value-outranks-the-environment|again|FASTCACHE_ADDR=127.0.0.1:3|FASTCACHE_ADDR=127.0.0.1:2|<empty>|127.0.0.1:2|-DFASTCACHE_ADDR=127.0.0.1:3 applies it"
    "address-held-over-ninja-regeneration-with-another|regenerate|FASTCACHE_ADDR=127.0.0.1:4|-|<empty>|127.0.0.1:2|[cache] FASTCACHE_ADDR is '127.0.0.1:4' in the environment and NOT applied"
    "address-define-retargets|again|FASTCACHE_ADDR=127.0.0.1:4|FASTCACHE_ADDR=127.0.0.1:4|<empty>|127.0.0.1:4|Enabling fastcache-cc"
)

set(violations "")
set(wiringRowsRead 0)
set(regenerated 0)
set(freshTrees 0)
foreach(row IN LISTS wiringRows)
    fastcached_row_fields("${row}" name step environment defines wantScheduler wantAddress wantFragment)

    set(environmentArguments "--unset=FASTCACHE_SCHEDULER" "--unset=FASTCACHE_ADDR")
    if(NOT environment STREQUAL "-")
        string(REPLACE " " ";" assignments "${environment}")
        list(APPEND environmentArguments ${assignments})
    endif()
    set(defineArguments "")
    if(NOT defines STREQUAL "-")
        string(REPLACE " " ";" assignments "${defines}")
        foreach(assignment IN LISTS assignments)
            list(APPEND defineArguments "-D${assignment}")
        endforeach()
    endif()

    if(step STREQUAL "fresh")
        # Numbered, not named: a row name is a sentence, and a tree under it put the compiler
        # check's own scratch objects past MAX_PATH on Windows, failing the configure (measured).
        math(EXPR freshTrees "${freshTrees} + 1")
        set(binaryDir "${FASTCACHED_SCRATCH_DIR}/tree${freshTrees}")
        file(REMOVE_RECURSE "${binaryDir}")
    elseif(NOT DEFINED binaryDir OR NOT (step STREQUAL "again" OR step STREQUAL "regenerate"))
        message(FATAL_ERROR "wiring row ${name}: step '${step}' is not fresh, nor again or regenerate after a fresh row")
    endif()
    if(step STREQUAL "regenerate")
        if(defineArguments)
            message(FATAL_ERROR "wiring row ${name}: a regenerate row takes no -D, since ninja passes none")
        endif()
        # CMakeCache.txt is an input of build.ninja's own RERUN_CMAKE edge, so touching it and
        # asking for build.ninja is ninja deciding to re-run CMake -- `--regenerate-during-build`,
        # with nothing on its command line but the two directories. Whether it DID is asserted
        # below from the output: only a CMake run prints the dispatch line.
        file(TOUCH_NOCREATE "${binaryDir}/CMakeCache.txt")
        set(command "${CMAKE_COMMAND}" --build "${binaryDir}" --target build.ninja)
    else()
        set(command "${CMAKE_COMMAND}" -S "${fixtureDir}" -B "${binaryDir}"
                    ${commonArguments}
                    "-DFASTCACHED_MODULE_DIR=${moduleDir}"
                    "-DFASTCACHE_CC=${standIn}" "-DSCCACHE=" "-DCCACHE="
                    ${defineArguments})
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${environmentArguments} ${command}
        RESULT_VARIABLE configured
        OUTPUT_VARIABLE configureOutput
        ERROR_VARIABLE configureError
        TIMEOUT 300)
    set(output "${configureOutput}${configureError}")
    # Neutralised before it can be echoed: this check's own verdict is read from its output, and a
    # nested configure's warning is not this check failing.
    string(REPLACE "CMake Error" "CMake-Error" echoed "${output}")
    string(REPLACE "CMake Warning" "CMake-Warning" echoed "${echoed}")

    if(NOT configured EQUAL 0)
        list(APPEND violations "${name}: ${step} failed (${configured}); the module must never fail a configure\n${echoed}")
        continue()
    endif()
    string(FIND "${output}" "Enabling fastcache-cc" enabledAt)
    if(enabledAt EQUAL -1)
        list(APPEND violations "${name}: no configure that took the stand-in as the launcher ran (a regeneration "
                               "ninja did not perform, or the row was not taken), so nothing below measures it\n${echoed}")
        continue()
    endif()
    if(step STREQUAL "regenerate")
        math(EXPR regenerated "${regenerated} + 1")
    endif()

    string(FIND "${output}" "${wantFragment}" saidAt)
    if(saidAt EQUAL -1)
        list(APPEND violations "${name}: expected the output to contain \"${wantFragment}\"\n${echoed}")
    endif()

    LauncherValues("${binaryDir}" FASTCACHE_SCHEDULER wiredScheduler lines)
    LauncherValues("${binaryDir}" FASTCACHE_ADDR wiredAddress addressLines)
    if(lines EQUAL 0)
        list(APPEND violations "${name}: no LAUNCHER line in ${binaryDir}/build.ninja to read (another generator, "
                               "or no compile edge): the wiring is UNREAD, which is not the same as unwired")
        continue()
    endif()
    math(EXPR wiringRowsRead "${wiringRowsRead} + 1")
    if(NOT "${wiredScheduler}" STREQUAL "${wantScheduler}")
        list(APPEND violations
             "${name}: the ${lines} LAUNCHER line(s) carry FASTCACHE_SCHEDULER [${wiredScheduler}], the table expects [${wantScheduler}]")
    endif()
    if(NOT "${wiredAddress}" STREQUAL "${wantAddress}")
        list(APPEND violations
             "${name}: the ${lines} LAUNCHER line(s) carry FASTCACHE_ADDR [${wiredAddress}], the table expects [${wantAddress}]")
    endif()
    # The claim against the artefact, with the table out of it.
    DispatchClaim("${output}" claimed)
    if(NOT "${claimed}" STREQUAL "${wiredScheduler}")
        list(APPEND violations
             "${name}: configure CLAIMED dispatch [${claimed}] and the LAUNCHER line(s) carry FASTCACHE_SCHEDULER "
             "[${wiredScheduler}]\n${echoed}")
    endif()
endforeach()

ReportDecisionFailures()
list(LENGTH wiringRows wiringRowCount)
if(violations)
    string(REPLACE ";" "\n\n" violations "${violations}")
    message(FATAL_ERROR "check-compile-cache-dispatch:\n${violations}")
endif()
if(NOT wiringRowsRead EQUAL wiringRowCount)
    message(FATAL_ERROR
        "check-compile-cache-dispatch: read the wiring of ${wiringRowsRead} of ${wiringRowCount} rows; the rest "
        "asserted only what configure said.")
endif()
if(regenerated EQUAL 0)
    message(FATAL_ERROR "check-compile-cache-dispatch: no row reached a configure through ninja's own regeneration")
endif()
if(decisionFailures)
    message(FATAL_ERROR "check-compile-cache-dispatch: the wiring agrees, and half 1's decisions above do not")
endif()
message(STATUS "check-compile-cache-dispatch: ${decisions} decisions and ${wiringRowsRead} wired configures "
               "(${regenerated} through ninja's regeneration) agree")
