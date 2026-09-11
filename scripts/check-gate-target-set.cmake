# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated.
cmake_minimum_required(VERSION 3.28)
#
# The local gate turns on the same default-OFF targets CI does, so its `ctest`
# total describes the same target set.
#
# ## Why this exists (#1144, #1197)
#
# A ctest total is comparable only against the same tree, the same platform AND
# the same target set. `FASTCACHED_BUILD_TESTCLIENT` and
# `FASTCACHED_BUILD_BENCHMARKS` default OFF, so one commit on one platform gives
# two different totals depending only on which `-D` flags were passed -- and the
# smaller number does not look wrong, the tree just looks smaller.
#
# #1144 fixed that by turning the flags on in `scripts/local-gate.sh` and keeping
# them in a `gate_target_flags` table, because the failure it guards against is a
# THIRD such target arriving and nobody noticing: `src/apps/CMakeLists.txt` gates
# each executable on a table row, so adding one is adding a row, and a target that
# defaults OFF is invisible to the gate rather than absent from CI.
#
# **That fix's own comment cited `ctest -R gate-target-set` as already refusing the
# drift, and no such test existed anywhere in the tree.** The claim was removed
# rather than left standing -- a comment asserting what nothing checks cannot fail
# -- and this is the check it named.
#
# ## Both sides are DERIVED
#
# The gate's set is read from `gate_target_flags` in `scripts/local-gate.sh` and
# CI's from the `-D` flags in `.github/workflows/build.yml`. Restating either here
# would make this a second thing to be wrong rather than a cross-check, and one
# that had drifted would agree with itself perfectly on every run.
#
# A third derivation catches the failure neither set can see on its own: a flag
# NAME that gates nothing. A misspelled `-DFASTCACHED_BUILD_BENCHMARK=ON` sets an
# unused cache variable, changes no target, and fails nothing -- so every option
# named on either side must be one this project declares, read from the
# `FASTCACHED_APPS` table and the root `option()` calls.
#
# ## What this deliberately does NOT model
#
# WHICH jobs run ctest. CI's side is every `FASTCACHED_BUILD_*` flag the workflow
# turns ON, wherever it appears, because deriving "this job runs tests" from the
# YAML is fragile in the direction that fails SILENT. The cost is stated rather
# than hidden: if a job ever turns one of these on without running tests, this
# check refuses and the fix is to decide deliberately -- either the gate matches
# it or the row is exempted with a reason. Refusing where the answer is unclear is
# the direction this repository has already chosen for `ci-scope.sh`.
#
# A flag turned OFF is not read. `-DFASTCACHED_BUILD_TESTS=OFF` in the packaging
# jobs turns the whole suite off, which is a different question from which
# optional TARGETS a suite covers, and folding the two would be a state collapse
# in the check written to prevent one.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-gate-target-set.cmake
#
# Exit: the verdict is the OUTPUT, not the status -- read through
# `FAIL_REGULAR_EXPRESSION`, because a `-P` script that merely WARNS exits 0.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(FastCachedGateScript "${FASTCACHED_SOURCE_DIR}/scripts/local-gate.sh")
set(FastCachedWorkflow "${FASTCACHED_SOURCE_DIR}/.github/workflows/build.yml")
set(FastCachedAppTable "${FASTCACHED_SOURCE_DIR}/src/apps/CMakeLists.txt")
set(FastCachedRootLists "${FASTCACHED_SOURCE_DIR}/CMakeLists.txt")

foreach(required
        "${FastCachedGateScript}" "${FastCachedWorkflow}"
        "${FastCachedAppTable}" "${FastCachedRootLists}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR
            "check-gate-target-set: ${required} does not exist.\n"
            "All four sides are derived from files, so a missing one is a check "
            "that cannot answer -- which must not be reported as a check that "
            "found nothing. The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# One reader for all four files.
#
# `file(READ)` and a walk by offset, never `file(STRINGS)`: that returns a LIST,
# and an unbalanced `[` or `]` on a kept line merges elements -- which destroys
# the line boundaries comment-stripping depends on, so a commented-out flag could
# be counted as a live one. A workflow file is full of `[` (`runs-on`, matrix
# lists, `${{ }}` expressions), so this is not a hypothetical here.
#
# Full-line comments are stripped before matching, because A COMMENT IS NOT A CALL
# SITE -- `local-gate.sh`'s own header discusses these flag names in prose for a
# dozen lines directly above the table.
#
# This is the third offset walk among the `check-*.cmake` scripts. Consolidating
# that idiom is #495 and is deliberately not done here: absorbing it would close
# one ticket by swallowing another.
# ---------------------------------------------------------------------------
function(FastCachedMatchInFile path startMarker endMarker pattern out)
    file(READ "${path}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    set(matches "")
    if(startMarker STREQUAL "")
        set(inRegion TRUE)
        set(sawRegion TRUE)
    else()
        set(inRegion FALSE)
        set(sawRegion FALSE)
    endif()

    while(NOT content STREQUAL "")
        string(FIND "${content}" "\n" newline)
        if(newline EQUAL -1)
            set(line "${content}")
            set(content "")
        else()
            string(SUBSTRING "${content}" 0 ${newline} line)
            math(EXPR newline "${newline} + 1")
            string(SUBSTRING "${content}" ${newline} -1 content)
        endif()

        if(line MATCHES "^[ \t]*#")
            continue()
        endif()

        if(NOT startMarker STREQUAL "" AND NOT inRegion)
            if(line MATCHES "${startMarker}")
                set(inRegion TRUE)
                set(sawRegion TRUE)
            endif()
            continue()
        endif()

        if(inRegion AND NOT endMarker STREQUAL "" AND line MATCHES "${endMarker}")
            set(inRegion FALSE)
            continue()
        endif()

        if(inRegion)
            string(REGEX MATCHALL "${pattern}" lineMatches "${line}")
            list(APPEND matches ${lineMatches})
        endif()
    endwhile()

    if(NOT startMarker STREQUAL "" AND NOT sawRegion)
        message(FATAL_ERROR
            "check-gate-target-set: could not find the region opening with "
            "`${startMarker}` in ${path}.\n"
            "That region is where this side of the comparison is read from. If it "
            "changed shape, this reader changes with it -- do not restore a copy "
            "of the set here. The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()
    if(inRegion AND NOT endMarker STREQUAL "")
        message(FATAL_ERROR
            "check-gate-target-set: the region opening with `${startMarker}` in "
            "${path} is never closed by `${endMarker}`.\n"
            "A region that runs to end-of-file would sweep in every later line, "
            "which reads as a much larger set than anybody wrote. The rule lives "
            "in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()

    set("${out}" "${matches}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The three sets.
# ---------------------------------------------------------------------------
FastCachedMatchInFile("${FastCachedGateScript}"
    "^gate_target_flags=\\(" "^\\)" "FASTCACHED_BUILD_[A-Z_]+" gateFlags)
list(REMOVE_DUPLICATES gateFlags)

FastCachedMatchInFile("${FastCachedWorkflow}"
    "" "" "-DFASTCACHED_BUILD_[A-Z_]+=ON" ciFlagAssignments)
set(ciFlags "")
foreach(assignment IN LISTS ciFlagAssignments)
    string(REGEX REPLACE "^-D" "" flagName "${assignment}")
    string(REGEX REPLACE "=ON$" "" flagName "${flagName}")
    list(APPEND ciFlags "${flagName}")
endforeach()
list(REMOVE_DUPLICATES ciFlags)

# `$`-anchored, and that is not decoration. Written as a prefix
# (`^set\(FASTCACHED_APPS`) the marker also matches `set(FASTCACHED_APPS_RENAMED`,
# so a renamed table went on being read as if nothing had happened and the
# self-test case for it could not fire. It was the case that found it.
FastCachedMatchInFile("${FastCachedAppTable}"
    "^set\\(FASTCACHED_APPS$" "^\\)" "\\|FASTCACHED_BUILD_[A-Z_]+\\|(ON|OFF)\\|" appRows)
FastCachedMatchInFile("${FastCachedRootLists}"
    "" "" "option\\(FASTCACHED_BUILD_[A-Z_]+ " rootOptions)

set(declaredOptions "")
set(defaultOnOptions "")
foreach(appRow IN LISTS appRows)
    string(REGEX MATCH "FASTCACHED_BUILD_[A-Z_]+" appOption "${appRow}")
    list(APPEND declaredOptions "${appOption}")
    if(appRow MATCHES "\\|ON\\|")
        list(APPEND defaultOnOptions "${appOption}")
    endif()
endforeach()
foreach(rootOption IN LISTS rootOptions)
    string(REGEX MATCH "FASTCACHED_BUILD_[A-Z_]+" rootOptionName "${rootOption}")
    list(APPEND declaredOptions "${rootOptionName}")
endforeach()
list(REMOVE_DUPLICATES declaredOptions)

# ---------------------------------------------------------------------------
# Two empty lists agree perfectly. Every side is refused when it reads empty,
# before any comparison, because a comparison of two nothings is the clean run
# this check exists to make impossible.
# ---------------------------------------------------------------------------
if(NOT gateFlags)
    message(FATAL_ERROR
        "check-gate-target-set: `gate_target_flags` in ${FastCachedGateScript} "
        "names no FASTCACHED_BUILD_* option.\n"
        "An empty table would agree with an empty CI set, so this is refused "
        "rather than compared. The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()
if(NOT ciFlags)
    message(FATAL_ERROR
        "check-gate-target-set: ${FastCachedWorkflow} passes no "
        "`-DFASTCACHED_BUILD_*=ON`.\n"
        "Either CI stopped turning the default-OFF targets on -- in which case "
        "the gate should stop too, deliberately -- or this reader stopped "
        "matching. The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()
if(NOT declaredOptions)
    message(FATAL_ERROR
        "check-gate-target-set: found no FASTCACHED_BUILD_* option declared in "
        "${FastCachedAppTable} or ${FastCachedRootLists}.\n"
        "Without that set every flag name would validate, including a misspelled "
        "one, which is the silent failure this side exists to catch. The rule "
        "lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

# ---------------------------------------------------------------------------
# The verdicts.
# ---------------------------------------------------------------------------
set(problems "")

foreach(flag IN LISTS gateFlags)
    list(FIND declaredOptions "${flag}" isDeclared)
    if(isDeclared EQUAL -1)
        string(APPEND problems
            "  the gate names `${flag}`, which this project declares nowhere. A "
            "`-D` naming no option sets an unused cache variable and changes no "
            "target\n")
        continue()
    endif()
    list(FIND defaultOnOptions "${flag}" isDefaultOn)
    if(NOT isDefaultOn EQUAL -1)
        string(APPEND problems
            "  the gate turns on `${flag}`, which already defaults ON. The row "
            "buys nothing and says the target is optional when it is not -- a row "
            "that has stopped describing the tree\n")
        continue()
    endif()
    list(FIND ciFlags "${flag}" isInCi)
    if(isInCi EQUAL -1)
        string(APPEND problems
            "  the gate turns on `${flag}` and ${FastCachedWorkflow} does not. "
            "The gate's ctest total then counts targets CI's does not, so the two "
            "numbers are not comparable in the direction that looks reassuring\n")
    endif()
endforeach()

foreach(flag IN LISTS ciFlags)
    list(FIND declaredOptions "${flag}" isDeclared)
    if(isDeclared EQUAL -1)
        string(APPEND problems
            "  the workflow passes `-D${flag}=ON`, which this project declares "
            "nowhere. It sets an unused cache variable and builds nothing extra, "
            "silently\n")
        continue()
    endif()
    list(FIND defaultOnOptions "${flag}" isDefaultOn)
    if(NOT isDefaultOn EQUAL -1)
        string(APPEND problems
            "  the workflow passes `-D${flag}=ON`, which already defaults ON. "
            "Refused on this side too, and symmetrically: the comparison is over "
            "targets that are OFF unless asked for, so a default-ON flag either "
            "means the default moved -- which both sides then need revisiting -- "
            "or the flag buys nothing. Ignoring it here would be the quieter "
            "answer and the wrong one\n")
        continue()
    endif()
    list(FIND gateFlags "${flag}" isInGate)
    if(isInGate EQUAL -1)
        string(APPEND problems
            "  ${FastCachedWorkflow} turns on `${flag}` and the gate does not. "
            "This is #1144's defect exactly: the gate's total is smaller and the "
            "tree just looks smaller\n")
    endif()
endforeach()

if(NOT problems STREQUAL "")
    string(REPLACE ";" ", " renderedGate "${gateFlags}")
    string(REPLACE ";" ", " renderedCi "${ciFlags}")
    message(FATAL_ERROR
        "The local gate's optional-target set and CI's do not agree:\n${problems}\n"
        "    gate_target_flags in scripts/local-gate.sh : ${renderedGate}\n"
        "    -DFASTCACHED_BUILD_*=ON in build.yml       : ${renderedCi}\n\n"
        "Fix it at whichever side is wrong -- both are read from their own files, "
        "so there is nothing to edit here. A ctest total is comparable only "
        "against the same tree, the same platform AND the same target set, and a "
        "gate covering less than CI reports a smaller number rather than a "
        "failure.")
endif()

list(LENGTH gateFlags gateCount)
list(LENGTH ciFlags ciCount)
list(LENGTH declaredOptions declaredCount)
string(REPLACE ";" ", " renderedGate "${gateFlags}")
message("gate target set: ${gateCount} optional target(s) turned on by the local "
        "gate and ${ciCount} by build.yml, and they are the same set "
        "(${renderedGate}); ${declaredCount} FASTCACHED_BUILD_* option(s) declared")
