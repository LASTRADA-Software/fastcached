# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated.
cmake_minimum_required(VERSION 3.28)
#
# `gate-target-set` must be SEEN to refuse, on each thing it claims and on nothing
# else -- and to ACCEPT, which is the direction a guard is not known to work in
# until somebody has watched it.
#
# The subject makes that unusually easy to get wrong. `gate-target-set` runs
# against THIS tree, where the two sides agree, so it reports the same clean line
# whether the comparison is live or the reader has stopped matching. And the
# defect it exists for -- #1144's, where CI builds a target the gate does not --
# is invisible on a tree that has never drifted.
#
# ## The trees are staged from the REAL four files
#
# Each case copies `scripts/local-gate.sh`, `.github/workflows/build.yml`,
# `src/apps/CMakeLists.txt` and the root `CMakeLists.txt` verbatim and mutates one
# of them. Nothing here restates a flag set: a second list is not a cross-check,
# it is a second thing to be wrong.
#
# Every mutation ASSERTS ITS ANCHOR MATCHED EXACTLY ONCE -- count, not presence,
# so `missing` and `not unique` both fire. An edit script that silently anchored
# on nothing reports success for work it did not do, and the case then passes or
# fails for a reason that has nothing to do with the rule.
#
# ## The verdict is the OUTPUT, and it is FLATTENED first
#
# `message(WARNING)` exits 0 on every CMake while printing a diagnostic, so a
# check that merely warned would pass an exit-code test. And CMake WRAPS a
# diagnostic at about 74 columns, so a phrase can exist in the output and in no
# single LINE of it.
#
# ## A spawn that never RAN is not a verdict
#
# `execute_process` reports a failure to LAUNCH as a STRING rather than a number,
# and a discarded `RESULT_VARIABLE` reads that as the check having answered --
# every `refuse` arm then passes and every `pass` arm fails, which presents as a
# rule regression and sends the next reader hunting a defect that is not there.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-gate-target-set-selftest.cmake
#
# Exit: the verdict is the OUTPUT, not the status.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(FastCachedCheck "${FASTCACHED_SOURCE_DIR}/scripts/check-gate-target-set.cmake")
set(FastCachedGateScript "${FASTCACHED_SOURCE_DIR}/scripts/local-gate.sh")
set(FastCachedWorkflow "${FASTCACHED_SOURCE_DIR}/.github/workflows/build.yml")
set(FastCachedAppTable "${FASTCACHED_SOURCE_DIR}/src/apps/CMakeLists.txt")
set(FastCachedRootLists "${FASTCACHED_SOURCE_DIR}/CMakeLists.txt")
foreach(required
        "${FastCachedCheck}" "${FastCachedGateScript}" "${FastCachedWorkflow}"
        "${FastCachedAppTable}" "${FastCachedRootLists}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "the subject is missing: ${required}")
    endif()
endforeach()

# Every tree starts with `file(REMOVE_RECURSE)` on a caller-supplied path, so the
# path is checked before anything is deleted. Absolute and at least two segments
# deep: a relative one would be resolved against whatever directory ctest ran this
# in, and `/` or `/x` is nothing a scratch tree should ever be.
if(NOT IS_ABSOLUTE "${FASTCACHED_SCRATCH_DIR}"
   OR NOT FASTCACHED_SCRATCH_DIR MATCHES "[^/\\]+[/\\][^/\\]+")
    message(FATAL_ERROR
        "check-gate-target-set-selftest: FASTCACHED_SCRATCH_DIR "
        "(${FASTCACHED_SCRATCH_DIR}) must be an absolute path at least two "
        "segments deep; this file removes it recursively.")
endif()

set(selftestRan 0)
set(selftestFailed 0)

# The names the staged app row introduces. A declared, default-OFF option that
# neither real side names is what makes both drift directions expressible without
# inventing a set of our own.
set(stagedOption "FASTCACHED_BUILD_STAGED")
set(stagedRow "    \"staged-app|${stagedOption}|OFF|A staged app row\"")
set(typoOption "FASTCACHED_BUILD_BENCHMARK")

function(FastCachedStageGateTree which out)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${which}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/scripts")
    file(MAKE_DIRECTORY "${tree}/.github/workflows")
    file(MAKE_DIRECTORY "${tree}/src/apps")
    file(COPY "${FastCachedCheck}" DESTINATION "${tree}/scripts")
    file(COPY "${FastCachedGateScript}" DESTINATION "${tree}/scripts")
    file(COPY "${FastCachedWorkflow}" DESTINATION "${tree}/.github/workflows")
    file(COPY "${FastCachedAppTable}" DESTINATION "${tree}/src/apps")
    file(COPY "${FastCachedRootLists}" DESTINATION "${tree}")
    set("${out}" "${tree}" PARENT_SCOPE)
endfunction()

# How many times `needle` appears in `text`, literally. `string(REGEX MATCHALL)`
# cannot answer this -- an anchor is ordinary text and may hold `(`, `[` or `.` --
# so it is a FIND walk.
function(FastCachedCountLiteral text needle out)
    set(seen 0)
    set(rest "${text}")
    string(LENGTH "${needle}" needleLength)
    while(TRUE)
        string(FIND "${rest}" "${needle}" at)
        if(at EQUAL -1)
            break()
        endif()
        math(EXPR seen "${seen} + 1")
        math(EXPR after "${at} + ${needleLength}")
        string(SUBSTRING "${rest}" ${after} -1 rest)
    endwhile()
    set("${out}" "${seen}" PARENT_SCOPE)
endfunction()

# Replace `old` by `new` in a staged file, refusing unless the anchor matched
# EXACTLY once. `expected` lets a case state a count other than one deliberately.
function(FastCachedMutate path old new expected)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "check-gate-target-set-selftest: cannot mutate ${path}: it does not "
            "exist. The staging step is the subject here, not the rule.")
    endif()
    file(READ "${path}" body)
    FastCachedCountLiteral("${body}" "${old}" seen)
    if(NOT seen EQUAL expected)
        message(FATAL_ERROR
            "check-gate-target-set-selftest: the anchor\n    ${old}\n"
            "matched ${seen} time(s) in ${path}, expected ${expected}.\n"
            "A mutation that anchored on nothing stages an UNCHANGED tree, and "
            "the case then passes or fails for a reason unrelated to the rule.")
    endif()
    string(REPLACE "${old}" "${new}" body "${body}")
    file(WRITE "${path}" "${body}")
endfunction()

function(FastCachedGateCase which tree expect needle)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}"
                -P "${tree}/scripts/check-gate-target-set.cmake"
        OUTPUT_VARIABLE childOut
        ERROR_VARIABLE childErr
        RESULT_VARIABLE childStatus
        ENCODING NONE)

    if(NOT childStatus MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "check-gate-target-set-selftest: INCONCLUSIVE -- the check could not "
            "be RUN for case ${which} (${childStatus}). No arm was evaluated, so "
            "no arm's verdict means anything. This is NOT a rule regression.")
    endif()

    string(REGEX REPLACE "[\r\n]+" " " flat "${childOut} ${childErr}")
    string(REGEX REPLACE " +" " " flat "${flat}")

    # Both words of the failure signal, because this harness reads the sub-run's
    # output itself and ctest never sees it. Matching `CMake Error` alone would
    # score a sub-run that merely WARNS as clean here while ctest's
    # FAIL_REGULAR_EXPRESSION refuses it -- a harness MORE PERMISSIVE than the
    # thing it stands for.
    set(verdict "pass")
    if(NOT childStatus EQUAL 0 OR flat MATCHES "CMake Error|CMake Warning")
        set(verdict "refuse")
    endif()

    set(problem "")
    if(NOT verdict STREQUAL expect)
        set(problem "expected to ${expect}, ${verdict}d")
    elseif(NOT flat MATCHES "${needle}")
        set(problem "did not report `${needle}`")
    endif()

    math(EXPR selftestRan "${selftestRan} + 1")
    set(selftestRan "${selftestRan}" PARENT_SCOPE)
    if(problem STREQUAL "")
        message("  ok   ${which}")
    else()
        message("  FAIL ${which}: ${problem}")
        message("       ${flat}")
        math(EXPR selftestFailed "${selftestFailed} + 1")
        set(selftestFailed "${selftestFailed}" PARENT_SCOPE)
    endif()
endfunction()

message("== check-gate-target-set-selftest")

# -- the accepting direction, first, asserting a positive report ------------
FastCachedStageGateTree("baseline" tree)
FastCachedGateCase("baseline-tree-is-accepted" "${tree}" pass "they are the same set")

# -- comment stripping, both halves. A reader that strips nothing passes the
#    refusing arm below by seeing the commented row; a reader that strips a whole
#    line whenever it holds a `#` fails the accepting arm above it. Only the pair
#    separates them.
FastCachedStageGateTree("commented-row" tree)
FastCachedMutate("${tree}/scripts/local-gate.sh"
    "\n    FASTCACHED_BUILD_BENCHMARKS\n" "\n#    FASTCACHED_BUILD_BENCHMARKS\n" 1)
FastCachedGateCase("a-commented-gate-row-is-not-a-row" "${tree}" refuse "FASTCACHED_BUILD_BENCHMARKS")

FastCachedStageGateTree("comment-inside-table" tree)
FastCachedMutate("${tree}/scripts/local-gate.sh"
    "gate_target_flags=(\n"
    "gate_target_flags=(\n    # not ${typoOption}, and not ${stagedOption} either\n" 1)
FastCachedGateCase("a-flag-name-in-a-comment-inside-the-table-is-not-a-row" "${tree}" pass "they are the same set")

# -- the two drift directions ------------------------------------------------
#
# Both need a declared, default-OFF option that neither real side names, so each
# case adds an app row first. Adding it to the app table alone changes nothing --
# a declared option nobody turns on is the ordinary case -- which is what makes
# the second mutation the whole content of each case.
FastCachedStageGateTree("gate-ahead" tree)
FastCachedMutate("${tree}/src/apps/CMakeLists.txt"
    "\n)\n" "\n${stagedRow}\n)\n" 1)
FastCachedMutate("${tree}/scripts/local-gate.sh"
    "\n    FASTCACHED_BUILD_BENCHMARKS\n"
    "\n    FASTCACHED_BUILD_BENCHMARKS\n    ${stagedOption}\n" 1)
FastCachedGateCase("a-gate-flag-CI-does-not-pass-is-refused" "${tree}" refuse "the gate turns on .${stagedOption}. and")

FastCachedStageGateTree("ci-ahead" tree)
FastCachedMutate("${tree}/src/apps/CMakeLists.txt"
    "\n)\n" "\n${stagedRow}\n)\n" 1)
FastCachedMutate("${tree}/.github/workflows/build.yml"
    "-DFASTCACHED_BUILD_TESTCLIENT=ON"
    "-DFASTCACHED_BUILD_TESTCLIENT=ON -D${stagedOption}=ON" 4)
FastCachedGateCase("a-CI-flag-the-gate-does-not-turn-on-is-refused" "${tree}" refuse "1144.s defect exactly")

# -- a flag name that gates nothing -----------------------------------------
FastCachedStageGateTree("gate-typo" tree)
FastCachedMutate("${tree}/scripts/local-gate.sh"
    "\n    FASTCACHED_BUILD_BENCHMARKS\n"
    "\n    FASTCACHED_BUILD_BENCHMARKS\n    ${typoOption}\n" 1)
FastCachedGateCase("an-undeclared-gate-flag-is-refused" "${tree}" refuse "declares nowhere")

FastCachedStageGateTree("ci-typo" tree)
FastCachedMutate("${tree}/.github/workflows/build.yml"
    "-DFASTCACHED_BUILD_TESTCLIENT=ON"
    "-DFASTCACHED_BUILD_TESTCLIENT=ON -D${typoOption}=ON" 4)
FastCachedGateCase("an-undeclared-CI-flag-is-refused" "${tree}" refuse "declares nowhere")

# -- a default-ON option on either side, refused symmetrically ---------------
FastCachedStageGateTree("gate-default-on" tree)
FastCachedMutate("${tree}/scripts/local-gate.sh"
    "\n    FASTCACHED_BUILD_BENCHMARKS\n"
    "\n    FASTCACHED_BUILD_BENCHMARKS\n    FASTCACHED_BUILD_DAEMON\n" 1)
FastCachedGateCase("a-default-ON-flag-on-the-gate-side-is-refused" "${tree}" refuse "already defaults ON")

FastCachedStageGateTree("ci-default-on" tree)
FastCachedMutate("${tree}/.github/workflows/build.yml"
    "-DFASTCACHED_BUILD_TESTCLIENT=ON"
    "-DFASTCACHED_BUILD_TESTCLIENT=ON -DFASTCACHED_BUILD_DAEMON=ON" 4)
FastCachedGateCase("a-default-ON-flag-on-the-CI-side-is-refused" "${tree}" refuse "already defaults ON")

# -- the ways this check can stop being able to answer -----------------------
#
# Each of these is a state that would otherwise compare two empty sets and report
# a clean run, which is the shape the check exists to make impossible.
FastCachedStageGateTree("empty-gate-table" tree)
FastCachedMutate("${tree}/scripts/local-gate.sh"
    "gate_target_flags=(\n    FASTCACHED_BUILD_TESTCLIENT\n    FASTCACHED_BUILD_BENCHMARKS\n)"
    "gate_target_flags=(\n)" 1)
FastCachedGateCase("an-empty-gate-table-is-refused" "${tree}" refuse "names no FASTCACHED_BUILD")

FastCachedStageGateTree("renamed-gate-table" tree)
FastCachedMutate("${tree}/scripts/local-gate.sh"
    "gate_target_flags=(" "gate_optional_targets=(" 1)
FastCachedGateCase("a-renamed-gate-table-is-refused" "${tree}" refuse "could not find the region")

FastCachedStageGateTree("workflow-passes-nothing" tree)
file(READ "${tree}/.github/workflows/build.yml" workflowBody)
string(REPLACE "-DFASTCACHED_BUILD_TESTCLIENT=ON" "" workflowBody "${workflowBody}")
string(REPLACE "-DFASTCACHED_BUILD_BENCHMARKS=ON" "" workflowBody "${workflowBody}")
file(WRITE "${tree}/.github/workflows/build.yml" "${workflowBody}")
FastCachedGateCase("a-workflow-passing-no-flag-is-refused" "${tree}" refuse "passes no")

FastCachedStageGateTree("nothing-declared" tree)
FastCachedMutate("${tree}/src/apps/CMakeLists.txt"
    "set(FASTCACHED_APPS" "set(FASTCACHED_APPS_RENAMED" 1)
FastCachedGateCase("an-unreadable-app-table-is-refused" "${tree}" refuse "could not find the region")

FastCachedStageGateTree("missing-workflow" tree)
file(REMOVE "${tree}/.github/workflows/build.yml")
FastCachedGateCase("a-missing-workflow-is-refused" "${tree}" refuse "does not exist")

FastCachedStageGateTree("missing-gate-script" tree)
file(REMOVE "${tree}/scripts/local-gate.sh")
FastCachedGateCase("a-missing-gate-script-is-refused" "${tree}" refuse "does not exist")

# A self-test that stopped early must not look like one that judged something.
message("== ${selftestRan} case(s) run, ${selftestFailed} failed")

if(NOT selftestFailed EQUAL 0)
    message(FATAL_ERROR
        "check-gate-target-set-selftest: ${selftestFailed} of ${selftestRan} "
        "case(s) failed. Each names the direction it drives, so a failure says "
        "whether the check stopped refusing something or started refusing "
        "something correct.")
endif()
