# SPDX-License-Identifier: Apache-2.0
#
# `check-unique-temp-paths.cmake` is driven against staged trees, in BOTH directions.
#
# A guard nobody has watched refuse is not a guard, and one nobody has watched ACCEPT is not
# known to work either -- the accepting half is what catches a pattern that has quietly started
# matching everything, which for this check is the likely failure: the four files its rule was
# written for now DISCUSS `std::random_device` in prose, so a reader that counted comments would
# refuse the very tree it had just fixed.
#
# ## What each case stages
#
#   <case>/src/FastCache/Core/IRandomSource.hpp   the seam, reading the device -- or not
#   <case>/src/apps/node/Subject.cpp              the text under test
#
# ## Read from the OUTPUT, never from the exit code
#
# `message(WARNING)` exits 0 on every CMake while printing `CMake Warning`, so an exit status
# cannot tell a refusal from a remark. Each case matches the check's own terminal text, FLATTENED
# first: CMake wraps its diagnostics at a column that depends on the scratch path's length, so a
# phrase can exist in the output and in no single line of it.
#
# ## The exemption arms stage a COPY of the check
#
# The exemption table is a `set()` inside the check and stays there rather than becoming an
# overridable path -- an override would be a way to silence the check from a command line. So
# those cases copy the check and rewrite the table in the copy, and each asserts it was judged by
# the STAGED one: a case that silently read the shipped empty table would go green while testing
# nothing.
#
# ## One arm is deliberately not covered
#
# The "enumerated no source file" refusal cannot be reached from a staged tree here, because the
# seam itself is a `.hpp` under `src/` and the positive anchor requires it to exist -- so the
# enumeration always finds at least one file. The arm stays, because the condition it guards is a
# broken enumeration rather than a staged tree, and a check that reports clean over nothing is
# the failure it exists for.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-unique-temp-paths.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()
set(helperLibrary "${FASTCACHED_SOURCE_DIR}/scripts/lib/CheckCommon.cmake")
if(NOT EXISTS "${helperLibrary}")
    message(FATAL_ERROR "the helper library is missing: ${helperLibrary}")
endif()

find_program(GIT_EXECUTABLE git)

# ---------------------------------------------------------------------------
# The staged texts.
set(seam_reading "#pragma once\n#include <random>\nnamespace FastCache {\nclass SystemRandomSource {\n  public:\n    static unsigned Seed() { std::random_device device; return device(); }\n};\n}\n")
set(seam_silent "#pragma once\n#include <random>\nnamespace FastCache {\nclass SystemRandomSource {\n  public:\n    static unsigned Seed() { return 7U; }\n};\n}\n")

# A subject that draws from the device, the shape #1507 came out of.
set(subject_draws "#include <random>\n#include <string>\nstd::string TempName()\n{\n    std::mt19937_64 rng { std::random_device {}() };\n    return \"t-\" + std::to_string(rng());\n}\n")
# The same words, in a comment. This is what the fixed tree looks like.
set(subject_comment "#include <string>\n// Never `std::random_device`: it answers zero for most draws on one host here.\nstd::string TempName() { return \"t\"; }\n")
# Neither: an ordinary file.
set(subject_clean "#include <string>\nstd::string TempName() { return \"t\"; }\n")

# ---------------------------------------------------------------------------
# Stage one case's tree and run the check over it.
#
# @param name The case index, which names its directory.
# @param seam `reading`, `silent`, or `absent`.
# @param subject `draws`, `comment` or `clean`.
# @param exemptionRow A row to put in a COPY of the check, or `-` to run the shipped one.
# @param outOutput Set to the check's combined output.
# @param outStaged Set to TRUE when the run used a staged copy of the check.
function(fastcached_stage_and_run name seam subject exemptionRow outOutput outStaged)
    set(tree "${FASTCACHED_SCRATCH_DIR}/case-${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Core")
    file(MAKE_DIRECTORY "${tree}/src/apps/node")

    if(seam STREQUAL "reading")
        file(WRITE "${tree}/src/FastCache/Core/IRandomSource.hpp" "${seam_reading}")
    elseif(seam STREQUAL "silent")
        file(WRITE "${tree}/src/FastCache/Core/IRandomSource.hpp" "${seam_silent}")
    elseif(NOT seam STREQUAL "absent")
        message(FATAL_ERROR "case `${name}`: unknown seam kind `${seam}`")
    endif()

    if(NOT DEFINED subject_${subject})
        message(FATAL_ERROR "case `${name}`: unknown subject `${subject}`")
    endif()
    file(WRITE "${tree}/src/apps/node/Subject.cpp" "${subject_${subject}}")
    # A second file that is always clean, so a refusal is attributable to the subject rather
    # than to "some file in the tree".
    file(WRITE "${tree}/src/apps/node/Neighbour.cpp" "int Neighbour = 1;\n")

    set(runCheck "${check}")
    set(staged FALSE)
    if(NOT exemptionRow STREQUAL "-")
        file(MAKE_DIRECTORY "${tree}/scripts/lib")
        file(READ "${check}" checkText)
        string(FIND "${checkText}" "set(FastCachedRandomDeviceExemptions \"\")" tableAt)
        if(tableAt EQUAL -1)
            # The table was renamed or reformatted, so this case can no longer inject a row --
            # and would otherwise run the SHIPPED empty table and pass while testing nothing.
            message(FATAL_ERROR
                "case `${name}`: the exemption table declaration was not found in the check, so a "
                "row cannot be staged. This case tests nothing until the anchor is updated.")
        endif()
        string(REPLACE "set(FastCachedRandomDeviceExemptions \"\")"
               "set(FastCachedRandomDeviceExemptions \"${exemptionRow}\")\nmessage(STATUS \"unique-temp-paths: STAGED-CHECK\")"
               checkText "${checkText}")
        file(WRITE "${tree}/scripts/check-unique-temp-paths.cmake" "${checkText}")
        file(COPY "${helperLibrary}" DESTINATION "${tree}/scripts/lib")
        set(runCheck "${tree}/scripts/check-unique-temp-paths.cmake")
        set(staged TRUE)
    endif()

    # A git index, so the enumeration runs in the mode CI runs it in rather than the fallback.
    # The mode is part of the check's own output and is asserted below.
    if(GIT_EXECUTABLE)
        execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}"
                        RESULT_VARIABLE initStatus OUTPUT_QUIET ERROR_QUIET)
        if(initStatus EQUAL 0)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A OUTPUT_QUIET ERROR_QUIET)
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${runCheck}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE status)
    set(combined "${out}${err}")
    # Flattened, because CMake wraps its diagnostics and the wrap column depends on this
    # scratch path's length.
    string(REGEX REPLACE "[\r\n]+" " " combined "${combined}")
    string(REGEX REPLACE " +" " " combined "${combined}")
    set(${outOutput} "${combined}" PARENT_SCOPE)
    set(${outStaged} ${staged} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases. `name|seam|subject|exemptionRow|expect|needle`
#
# `expect` is `accept` or `refuse`, read from the OUTPUT. `needle` is a phrase that must be
# present, so a case cannot pass on the wrong refusal.
set(cases
    "clean|reading|clean|-|accept|the seam is the only caller"
    "violation|reading|draws|-|refuse|read std::random_device outside the seam"
    "comment-only|reading|comment|-|accept|the seam is the only caller"
    "exempt-honoured|reading|draws|src/apps/node/Subject.cpp~bar~a staged reason|accept|the seam is the only caller"
    "exempt-stale|reading|clean|src/apps/node/Subject.cpp~bar~a staged reason|refuse|is stale"
    "seam-absent|absent|clean|-|refuse|the randomness seam is missing"
    "seam-silent|silent|clean|-|refuse|found no use in the seam"
)

set(ran 0)
set(failures "")
foreach(row IN LISTS cases)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 name)
    list(GET fields 1 seam)
    list(GET fields 2 subject)
    list(GET fields 3 exemptionRow)
    list(GET fields 4 expect)
    list(GET fields 5 needle)
    string(REPLACE "~bar~" "|" exemptionRow "${exemptionRow}")

    fastcached_stage_and_run("${name}" "${seam}" "${subject}" "${exemptionRow}" output staged)
    math(EXPR ran "${ran} + 1")

    # The verdict, from the diagnostic channel on ONE flattened line: a check that merely WARNS
    # must read as a refusal too, which an exit status cannot say.
    if(output MATCHES "CMake Error|CMake Warning")
        set(verdict "refuse")
    else()
        set(verdict "accept")
    endif()

    if(NOT verdict STREQUAL expect)
        list(APPEND failures "${name}: expected ${expect}, got ${verdict} -- ${output}")
    elseif(NOT output MATCHES "${needle}")
        list(APPEND failures "${name}: ${verdict} was right but the words were not: expected `${needle}` -- ${output}")
    endif()

    # An exemption case that did not run the staged check tested the shipped empty table.
    if(NOT exemptionRow STREQUAL "-")
        if(NOT staged)
            list(APPEND failures "${name}: the staged check was not used")
        elseif(NOT output MATCHES "STAGED-CHECK")
            list(APPEND failures "${name}: the run did not report STAGED-CHECK, so the shipped table was read")
        endif()
    endif()
endforeach()

if(failures)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("unique-temp-paths-selftest: ${ran} case(s) ran")
    list(LENGTH failures failureCount)
    message(FATAL_ERROR "unique-temp-paths-selftest: ${failureCount} case(s) failed")
endif()

# How many RAN, not only how many failed: a run that stopped early must not read like one that
# judged something.
message(STATUS "unique-temp-paths-selftest: ${ran} case(s) ran, all as expected")
