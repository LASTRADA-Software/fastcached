# SPDX-License-Identifier: Apache-2.0
#
# The half that makes `check-staged-stub-spawns.cmake` a guard rather than a hope.
#
# Every case stages a synthetic tree, runs the check over it through a NESTED
# `cmake -P`, and asserts WHICH refusal it got -- not merely that it refused. "It
# refused" and "it refused for the reason this case exists" are different claims and
# only the second one tests anything.
#
# The ACCEPTING arm is first and is not decoration: a check that refused every tree
# would pass every refusing case below, and one stood in this repository for two days.
#
# Judged by OUTPUT rather than by exit code, which is this tree's rule for a
# `cmake -P` check: `message(WARNING)` exits 0 on every CMake while printing
# `CMake Warning`.
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "check-staged-stub-spawns-selftest: FASTCACHED_SOURCE_DIR is required")
endif()

set(checker "${FASTCACHED_SOURCE_DIR}/scripts/check-staged-stub-spawns.cmake")
# OUTSIDE the repository. A fixture's scratch directory inside the tree changes
# what it tests -- the staged trees would be enumerated by every other scan while
# they exist -- and leaves artefacts under `scripts/` if a case aborts.
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    set(FASTCACHED_SCRATCH_DIR "$ENV{TMPDIR}")
endif()
if(NOT FASTCACHED_SCRATCH_DIR)
    set(FASTCACHED_SCRATCH_DIR "/tmp")
endif()
set(scratch "${FASTCACHED_SCRATCH_DIR}/staged-stub-spawns-selftest")
file(REMOVE_RECURSE "${scratch}")

set(ran 0)
set(failed 0)

# Run the checker against a staged tree and assert on its OUTPUT.
# @param 1 what this case is about
# @param 2 the tree
# @param 3 `accept` or `refuse`
# @param 4 a phrase the output must carry
function(Expect what tree want phrase)
    math(EXPR ran "${ran} + 1")
    set(ran "${ran}" PARENT_SCOPE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${checker}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
    set(text "${out}${err}")
    # CMake WRAPS its diagnostics, so a phrase can exist in the output and in no
    # single line of it. Flattened before matching.
    string(REPLACE "\n" " " flat "${text}")
    string(REGEX REPLACE " +" " " flat "${flat}")
    # BOTH words, on ONE line. This harness reads the sub-run's output itself --
    # ctest never sees it -- so it has to spell the whole pattern, or a nested check
    # that merely WARNS is scored a clean pass here while ctest would refuse it.
    # `check-script-check-signals` caught exactly that: the first spelling matched
    # `CMake Error` alone.
    set(refused FALSE)
    if(flat MATCHES "CMake Error|CMake Warning")
        set(refused TRUE)
    endif()
    set(ok TRUE)
    if(want STREQUAL "accept" AND refused)
        set(ok FALSE)
    endif()
    if(want STREQUAL "refuse" AND NOT refused)
        set(ok FALSE)
    endif()
    string(FIND "${flat}" "${phrase}" at)
    if(at EQUAL -1)
        set(ok FALSE)
    endif()
    if(ok)
        message(STATUS "  ok    ${what}")
    else()
        math(EXPR failed "${failed} + 1")
        set(failed "${failed}" PARENT_SCOPE)
        message(SEND_ERROR "STAGED STUB SPAWNS SELFTEST: ${what} -- wanted ${want} carrying '${phrase}'. Got:\n${text}")
    endif()
endfunction()

# Stage a tree carrying the checker's own library and one candidate file.
# @param 1 the tree  @param 2 the candidate's body  @param 3 the table row, or "-"
function(Stage tree body row)
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/scripts/lib")
    configure_file("${FASTCACHED_SOURCE_DIR}/scripts/lib/CheckCommon.cmake"
                   "${tree}/scripts/lib/CheckCommon.cmake" COPYONLY)
    # The roots file too: `fastcached_decline_third_party` refuses outright without
    # it, which would fail every case for one shared wrong reason rather than on
    # its own merits -- and the accepting case would then "refuse" correctly by
    # accident.
    configure_file("${FASTCACHED_SOURCE_DIR}/scripts/lib/third-party-roots.txt"
                   "${tree}/scripts/lib/third-party-roots.txt" COPYONLY)
    file(WRITE "${tree}/scripts/candidate.cmake" "${body}")
    # The checker with its table replaced. The real table is read out of the file
    # rather than restated here, so this fixture cannot drift from its format.
    file(READ "${checker}" checkerText)
    # The staged COPY of the checker is itself a candidate -- it carries the shebang
    # literals it matches on and names `execute_process` in its mechanism list -- so
    # every case's table must classify it, or each case fails on the checker rather
    # than on its own subject. Appended here rather than written into each case's
    # row, so a case states only what it is about.
    set(selfRow "\"scripts/check-staged-stub-spawns.cmake|data|-|the scan's own pattern literals, not a staged stub\"")
    string(REGEX REPLACE "set\\(StagedStubSpawns\n[^)]*\\)" "set(StagedStubSpawns\n    ${row}\n    ${selfRow}\n)" patched "${checkerText}")
    # The row-count assertion is about the REAL table and is meaningless over a staged
    # one, so it is disabled in the copy -- by a REGEX on the number rather than a
    # literal `8`. Written literally, the day the real table grows a ninth row this
    # `REPLACE` matches nothing, every staged checker keeps its `EQUAL 8` assertion,
    # and the two ACCEPTING cases go red carrying a refusal about a row count the
    # fixture invented. A self-test failing for a reason that is not its subject is
    # how a guard gets deleted rather than read.
    string(REGEX REPLACE "if\\(NOT rowCount EQUAL [0-9]+\\)" "if(FALSE)" patched "${patched}")
    file(WRITE "${tree}/scripts/check-staged-stub-spawns.cmake" "${patched}")
    set(checker "${tree}/scripts/check-staged-stub-spawns.cmake" PARENT_SCOPE)
endfunction()

# The leading comment line is LOAD-BEARING. The checker drops line 1, because a
# file's own shebang is not a staged stub -- so a fixture whose stub lands on line 1
# stages no candidate at all, and every case then fails for one shared wrong reason.
# It did: five cases red over a checker that was behaving exactly as specified.
set(stubBody "# a staged stub, below line 1\nfile(WRITE \"x\" \"#!/bin/sh\\nexit 0\\n\")\nexecute_process(COMMAND \"x\")\n")
set(realChecker "${checker}")

# 1. ACCEPTING: a candidate with a well-formed `guarded` row.
Stage("${scratch}/a" "${stubBody}CMAKE_HOST_WIN32\n"
      "\"scripts/candidate.cmake|guarded|CMAKE_HOST_WIN32|the sandbox this needs has no Windows equivalent\"")
Expect("a guarded candidate with a subject reason is accepted" "${scratch}/a" accept "all classified")
set(checker "${realChecker}")

# 2. The defect itself: a candidate nothing classifies.
Stage("${scratch}/b" "${stubBody}" "\"scripts/nothing.cmake|data|-|placeholder\"")
Expect("a candidate with no row is refused" "${scratch}/b" refuse "and no row in")
set(checker "${realChecker}")

# 3. The reason is the discriminator -- a portability excuse is refused.
Stage("${scratch}/c" "${stubBody}CMAKE_HOST_WIN32\n"
      "\"scripts/candidate.cmake|guarded|CMAKE_HOST_WIN32|Windows cannot start a shebang script\"")
Expect("a portability excuse is refused where a subject reason belongs" "${scratch}/c" refuse "PORTABILITY excuse")
set(checker "${realChecker}")

# 4. A guard the file does not carry.
Stage("${scratch}/d" "${stubBody}"
      "\"scripts/candidate.cmake|guarded|CMAKE_HOST_WIN32|the sandbox this needs has no Windows equivalent\"")
Expect("a guard token absent from the file is refused" "${scratch}/d" refuse "does not appear in the file")
set(checker "${realChecker}")

# 5. A stale row -- the file stopped being a candidate.
Stage("${scratch}/e" "# no stub here\nexecute_process(COMMAND \"x\")\n"
      "\"scripts/candidate.cmake|data|-|placeholder\"")
Expect("a row that no longer describes a candidate is refused as stale" "${scratch}/e" refuse "no longer describes a candidate")
set(checker "${realChecker}")

# 6. `adapted` is accepted and is the preferred remedy.
Stage("${scratch}/f" "${stubBody}MINGW\n"
      "\"scripts/candidate.cmake|adapted|MINGW|stages a .cmd form so the fixture runs everywhere\"")
Expect("an adapted candidate is accepted" "${scratch}/f" accept "ok (adapted)")
set(checker "${realChecker}")

# 7. A class nobody enumerated is not a pass.
Stage("${scratch}/g" "${stubBody}MINGW\n"
      "\"scripts/candidate.cmake|sideways|MINGW|whatever\"")
Expect("a class nobody enumerated is refused" "${scratch}/g" refuse "is none of")
set(checker "${realChecker}")

file(REMOVE_RECURSE "${scratch}")
message(STATUS "staged-stub-spawns --self-test: ${ran} case(s) ran, ${failed} failed")
if(failed GREATER 0)
    message(FATAL_ERROR "staged-stub-spawns selftest: ${failed} case(s) failed")
endif()
