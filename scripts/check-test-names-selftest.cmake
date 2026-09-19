# SPDX-License-Identifier: Apache-2.0
#
# `check-test-names.cmake` is driven against staged trees, in BOTH directions.
#
# It had no self-test at all, which is what let three holes sit in it (#1261): a
# case whose name WRAPS to the next line left the reader entirely, a case declared
# in a file not named `*_test.cpp` was never globbed, and two of Catch2's
# case-declaring macros were missing from a pattern whose comment said all of them
# were there. None of the three lowers a number anybody compares -- a case that is
# never seen cannot -- so every one of them reported clean.
#
# ## The three cases that discriminate
#
# `wrapped-duplicate`, `unsuffixed-duplicate` and `template-method-dash` are the
# regression cases, and each is built so that the PRE-#1261 reader accepts it:
#
#   * `wrapped-duplicate` -- the same name twice, once inline and once wrapped. The
#     old reader saw one case and no duplicate.
#   * `unsuffixed-duplicate` -- the same name in a `_test.cpp` and in a plain
#     `.cpp`. The old glob read only the first.
#   * `template-method-dash` -- a dash-leading name on `TEMPLATE_TEST_CASE_METHOD`,
#     which the old macro pattern did not match.
#
# A case asserting only that the check "still passes on a clean tree" would be
# green under all three.
#
# ## Counts are asserted, not just verdicts
#
# The failure mode here is UNDER-counting, and an under-count is silent by
# construction: every one of the three holes produces a correct-looking verdict
# over a smaller population. So the accepting cases assert how many cases the
# check SAW, which is the only thing that moves when a reader stops reading.
#
# ## Every case runs a STAGED copy of the check
#
# `FastCachedDuplicateNameExemptions` ships with a row describing a real duplicate
# in this repository, and the check refuses a row that no longer describes one. A
# synthetic tree contains no such duplicate, so the shipped table is stale over
# every tree staged here and every case would refuse for that reason instead of
# its own. The copy therefore replaces the table -- with the case's own row, or
# with nothing -- and each run is asserted to have been judged by the copy.
#
# ## Read from the OUTPUT, never from the exit code
#
# `message(WARNING)` exits 0 on every CMake while printing `CMake Warning`, so an
# exit status cannot tell a refusal from a remark. Each case matches the check's
# own terminal text, FLATTENED first: CMake wraps its diagnostics at a column that
# depends on the scratch path's length, so a phrase can exist in the output and in
# no single line of it.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-test-names.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

# ---------------------------------------------------------------------------
# The staged texts. Written as file CONTENT rather than compiled, because the
# check reads bytes: a fixture type that does not exist is fine, and a name that
# no compiler would accept is the point of several of these.
set(body "{\n    SUCCEED();\n}\n")

set(file_plain "TEST_CASE(\"alpha stays distinct\", \"[t]\")\n${body}\nTEST_CASE(\"beta stays distinct\", \"[t]\")\n${body}")
set(file_dash "TEST_CASE(\"-cache-dir gives the node an on-disk tier\", \"[t]\")\n${body}\nTEST_CASE(\"beta stays distinct\", \"[t]\")\n${body}")
set(file_duplicate "TEST_CASE(\"alpha stays distinct\", \"[t]\")\n${body}\nTEST_CASE(\"alpha stays distinct\", \"[u]\")\n${body}")

# The wrapped shape, as clang-format produces it for a long name: the macro opens
# and the literal is on the NEXT line.
set(file_wrapped_duplicate "TEST_CASE(\"alpha stays distinct\", \"[t]\")\n${body}\nTEST_CASE(\n    \"alpha stays distinct\",\n    \"[u]\")\n${body}")
set(file_wrapped_unique "TEST_CASE(\"alpha stays distinct\", \"[t]\")\n${body}\nTEST_CASE(\n    \"beta stays distinct\",\n    \"[u]\")\n${body}")

# A case in a file the `*_test.cpp` convention does not cover -- a bench or a
# canary. It registers exactly like any other. Two variants, because the glob has
# to be watched COUNTING one as well as refusing one: a widened glob that found
# the file and then dropped its cases would pass the refusing case alone.
set(file_unsuffixed "TEST_CASE(\"alpha stays distinct\", \"[bench]\")\n${body}")
set(file_unsuffixed_unique "TEST_CASE(\"gamma stays distinct\", \"[bench]\")\n${body}")

set(file_template_method "TEMPLATE_TEST_CASE_METHOD(Fixture, \"-cache-dir gives the node an on-disk tier\", \"[t]\", int)\n${body}")
set(file_scenario_method "SCENARIO_METHOD(Fixture, \"-cache-dir gives the node an on-disk tier\", \"[t]\")\n${body}")

# An identifier in the family that the table does not name.
set(file_unknown_macro "FASTCACHE_TEST_CASE(\"gamma stays distinct\", \"[t]\")\n${body}")

# A macro that opens and never produces a literal within the lookahead.
set(file_unreadable "TEST_CASE(\n    MakeName(1),\n    MakeTags(2),\n    3,\n    4,\n    5,\n    6,\n    7,\n    8,\n    9,\n    10)\n${body}")

# No macro at all: an ordinary source file.
set(file_no_cases "int Answer()\n{\n    return 42;\n}\n")

# ---------------------------------------------------------------------------
# Stage one case's tree and run a copy of the check over it.
#
# @param name The case name, which names its directory.
# @param layout Semicolon-free spec of `relativePath=textVariable` pairs, `+`-joined.
# @param exemptionRow A row for the staged exemption table, or `-` for none.
# @param outOutput Set to the check's combined, flattened output.
# @param outStaged Set to TRUE when the run used the staged copy.
function(fastcached_stage_and_run name layout exemptionRow outOutput outStaged)
    set(tree "${FASTCACHED_SCRATCH_DIR}/case-${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src")
    file(MAKE_DIRECTORY "${tree}/scripts")

    if(NOT layout STREQUAL "-")
        string(REPLACE "+" ";" entries "${layout}")
        foreach(entry IN LISTS entries)
            string(FIND "${entry}" "=" at)
            if(at EQUAL -1)
                message(FATAL_ERROR "case `${name}`: layout entry has no '=': ${entry}")
            endif()
            string(SUBSTRING "${entry}" 0 ${at} relative)
            math(EXPR at "${at} + 1")
            string(SUBSTRING "${entry}" ${at} -1 which)
            if(NOT DEFINED file_${which})
                message(FATAL_ERROR "case `${name}`: unknown staged text `${which}`")
            endif()
            get_filename_component(parent "${tree}/${relative}" DIRECTORY)
            file(MAKE_DIRECTORY "${parent}")
            file(WRITE "${tree}/${relative}" "${file_${which}}")
        endforeach()
    endif()

    # The exemption table, replaced in a COPY. Both anchors are asserted: a case
    # that silently ran the shipped table would refuse every tree here as stale
    # and look like a working guard while testing nothing.
    file(READ "${check}" checkText)
    string(FIND "${checkText}" "set(FastCachedDuplicateNameExemptions" tableAt)
    if(tableAt EQUAL -1)
        message(FATAL_ERROR
            "the exemption table declaration was not found in ${check}, so no case here "
            "can stage one. Every case would run the shipped table, which is stale over "
            "a synthetic tree, and this self-test would test nothing.")
    endif()
    string(SUBSTRING "${checkText}" 0 ${tableAt} beforeTable)
    string(SUBSTRING "${checkText}" ${tableAt} -1 tail)
    string(FIND "${tail}" "\n)\n" closeAt)
    if(closeAt EQUAL -1)
        message(FATAL_ERROR
            "the exemption table in ${check} is no longer closed by a `)` at the start of "
            "a line, so this self-test cannot find its end.")
    endif()
    math(EXPR closeAt "${closeAt} + 3")
    string(SUBSTRING "${tail}" ${closeAt} -1 afterTable)

    set(stagedRows "")
    if(NOT exemptionRow STREQUAL "-")
        set(stagedRows "\"${exemptionRow}\"")
    endif()
    # The helper goes beside the staged copy: the check includes it by
    # `CMAKE_CURRENT_LIST_DIR`, so a copy anywhere else fails to include and every
    # case would refuse for a reason that is not its own.
    file(COPY "${FASTCACHED_SOURCE_DIR}/scripts/lib/CheckCommon.cmake"
         DESTINATION "${tree}/scripts/lib")
    file(WRITE "${tree}/scripts/check-test-names.cmake"
         "${beforeTable}set(FastCachedDuplicateNameExemptions ${stagedRows})\n"
         "message(STATUS \"test-names: STAGED-CHECK\")\n"
         "${afterTable}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}"
                -P "${tree}/scripts/check-test-names.cmake"
        OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(combined "${out}${err}")
    string(REGEX REPLACE "[\r\n]+" " " combined "${combined}")
    string(REGEX REPLACE " +" " " combined "${combined}")
    set(${outOutput} "${combined}" PARENT_SCOPE)
    if(combined MATCHES "STAGED-CHECK")
        set(${outStaged} TRUE PARENT_SCOPE)
    else()
        set(${outStaged} FALSE PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# The cases. `name|layout|exemptionRow|expect|needle|cases`
#
# `expect` is `accept` or `refuse`, read from the OUTPUT. `needle` is a phrase that
# must be present, so a case cannot pass on the wrong refusal. `cases` is the case
# count the check must report, or `-` where the run refuses before reporting one.
set(cases
    # --- the accepting direction, with its count -----------------------------
    "clean|src/Alpha_test.cpp=plain|-|accept|survive the CTest round trip|2"
    # A wrapped name is READ, so the count includes it. Under the old reader this
    # tree was also accepted -- with a count of 1, which nothing asserted.
    "wrapped-unique|src/Alpha_test.cpp=wrapped_unique|-|accept|survive the CTest round trip|2"
    # A case in a plain `.cpp` and one in a `.hpp` are both COUNTED, which the
    # refusing regression below cannot show: a glob that found the file and then
    # read no cases out of it would still produce that refusal's absence.
    "unsuffixed-counted|src/Alpha_test.cpp=plain+src/apps/bench/Thing.cpp=unsuffixed_unique|-|accept|survive the CTest round trip|3"
    "header-counted|src/Alpha_test.cpp=plain+src/tests/Helper.hpp=unsuffixed_unique|-|accept|survive the CTest round trip|3"

    # --- the refusing direction ----------------------------------------------
    "dash|src/Alpha_test.cpp=dash|-|refuse|begin with '-'|-"
    "duplicate|src/Alpha_test.cpp=duplicate|-|refuse|declared more than once|-"

    # --- the three #1261 regressions -----------------------------------------
    "wrapped-duplicate|src/Alpha_test.cpp=wrapped_duplicate|-|refuse|declared more than once|-"
    "unsuffixed-duplicate|src/Alpha_test.cpp=plain+src/apps/bench/Thing.cpp=unsuffixed|-|refuse|declared more than once|-"
    "template-method-dash|src/Alpha_test.cpp=template_method|-|refuse|begin with '-'|-"
    "scenario-method-dash|src/Alpha_test.cpp=scenario_method|-|refuse|begin with '-'|-"

    # --- states that used to be a silent `continue()` ------------------------
    "unknown-macro|src/Alpha_test.cpp=plain+src/Beta_test.cpp=unknown_macro|-|refuse|does not list|-"
    "unreadable-name|src/Alpha_test.cpp=plain+src/Beta_test.cpp=unreadable|-|refuse|could not read|-"

    # --- the vacuity guard, in both of its arms ------------------------------
    "no-files|-|-|refuse|reporting on nothing|-"
    "no-cases|src/Alpha.cpp=no_cases|-|refuse|reporting on nothing|-"

    # --- the exemption table, both directions --------------------------------
    "exemption-honoured|src/Alpha_test.cpp=duplicate|alpha stays distinct~bar~a staged reason|accept|survive the CTest round trip|2"
    "exemption-stale|src/Alpha_test.cpp=plain|alpha stays distinct~bar~a staged reason|refuse|no longer describe|-"
)

set(ran 0)
set(failures "")
foreach(row IN LISTS cases)
    string(REPLACE "|" ";" fields "${row}")
    list(GET fields 0 name)
    list(GET fields 1 layout)
    list(GET fields 2 exemptionRow)
    list(GET fields 3 expect)
    list(GET fields 4 needle)
    list(GET fields 5 expectedCases)
    string(REPLACE "~bar~" "|" exemptionRow "${exemptionRow}")

    fastcached_stage_and_run("${name}" "${layout}" "${exemptionRow}" output staged)
    math(EXPR ran "${ran} + 1")

    if(NOT staged)
        list(APPEND failures
             "${name}: the staged copy of the check did not run, so the shipped exemption "
             "table was read and this case tested nothing -- ${output}")
        continue()
    endif()

    # The verdict, from the diagnostic channel on ONE flattened line: a check that
    # merely WARNS must read as a refusal too, which an exit status cannot say.
    if(output MATCHES "CMake Error|CMake Warning")
        set(verdict "refuse")
    else()
        set(verdict "accept")
    endif()

    if(NOT verdict STREQUAL expect)
        list(APPEND failures "${name}: expected ${expect}, got ${verdict} -- ${output}")
    elseif(NOT output MATCHES "${needle}")
        list(APPEND failures
             "${name}: ${verdict} was right but the words were not: expected `${needle}` -- ${output}")
    elseif(NOT expectedCases STREQUAL "-")
        # The half a verdict cannot carry. Each of #1261's three holes leaves the
        # verdict correct and the population smaller.
        if(NOT output MATCHES "hygiene: ([0-9]+) case")
            list(APPEND failures "${name}: the run reported no case count -- ${output}")
        elseif(NOT CMAKE_MATCH_1 STREQUAL "${expectedCases}")
            list(APPEND failures
                 "${name}: the check saw ${CMAKE_MATCH_1} case(s), expected ${expectedCases}. "
                 "A reader that stops reading leaves the verdict right and the count short.")
        endif()
    endif()
endforeach()

# A self-test that stopped early must not look like one that judged something.
if(ran EQUAL 0)
    message(FATAL_ERROR "check-test-names self-test ran no cases, so it asserted nothing.")
endif()

if(failures)
    list(JOIN failures "\n  " report)
    message(FATAL_ERROR "check-test-names self-test: ${ran} case(s) ran, and:\n  ${report}")
endif()

message("check-test-names self-test: ${ran} case(s) ran, every verdict and every count as expected")
