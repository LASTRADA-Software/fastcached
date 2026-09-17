# SPDX-License-Identifier: Apache-2.0
#
# `check-membership-fakes.cmake` is driven against staged trees, in BOTH directions.
#
# A guard nobody has watched refuse is not a guard, and a guard nobody has watched ACCEPT is
# not known to work either -- so half these cases expect a clean run. The refusing half is
# cheap to get right and the accepting half is what catches a pattern that has quietly started
# matching everything.
#
# ## What each case stages
#
# One miniature tree per case, because the check's subject is a whole-tree question (what does
# the enumeration reach, and does the shared header still define the fakes):
#
#   <case>/src/tests/MembershipFakes.hpp    two shared fakes, or none
#   <case>/src/apps/node/Subject_test.cpp   the text under test, or absent
#
# ## Read from the OUTPUT, never from the exit code
#
# `message(WARNING)` exits 0 on every CMake while printing `CMake Warning`, so an exit status
# cannot tell a refusal from a remark. Each case matches the check's own terminal text, and the
# output is FLATTENED first: CMake wraps its diagnostics at a column that depends on the scratch
# path's length, so a phrase can exist in the output and in no single line of it.
#
# ## The exemption arms stage a COPY of the check
#
# The exemption table is a `set()` inside the check, and it stays there rather than becoming an
# overridable path: an override would be a way to silence the check from a command line. So the
# two exemption cases copy the check (and the helper library it includes) into the scratch tree
# and rewrite the table in the copy. Each asserts it was judged by the STAGED check, because a
# case that silently read the shipped empty table would go green while testing nothing.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-membership-fakes.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()
set(helperLibrary "${FASTCACHED_SOURCE_DIR}/scripts/lib/CheckCommon.cmake")
if(NOT EXISTS "${helperLibrary}")
    message(FATAL_ERROR "the helper library is missing: ${helperLibrary}")
endif()
# The row parser, which this file uses for its own case table and the staged copies re-include.
include("${helperLibrary}")

find_package(Git QUIET)

# ---------------------------------------------------------------------------
# The staged texts. `~sc~` stands in for a `;`, because these live in CMake LIST rows and a
# real `;` would split one row into two before any parser saw it.
set(twoFakesHeader
    "#pragma once\n"
    "namespace FastCache::Testing\n{\n"
    "class ListedMembership final: public Distributed::IMembershipOracle\n{\n}~sc~\n"
    "class FixedMembership final: public Distributed::IMembershipOracle\n{\n}~sc~\n"
    "} // namespace\n")
string(REPLACE ";" "" twoFakesHeader "${twoFakesHeader}")

# A header that defines no implementation at all: the shape that makes the check's pattern
# report a clean tree for the wrong reason.
set(noFakesHeader
    "#pragma once\n// The fakes used to be here.\nnamespace FastCache::Testing\n{\n} // namespace\n")

set(subject_uses-shared
    "#include <tests/MembershipFakes.hpp>\n"
    "TEST_CASE(\"admits a member\", \"[node]\")\n{\n"
    "    ListedMembership const oracle { { \"10.0.0.1\" }, Distributed::MembershipParticipant::FleetMemberList }~sc~\n"
    "}\n")
string(REPLACE ";" "" subject_uses-shared "${subject_uses-shared}")

set(subject_own-fake
    "class MyOwnMembership final: public Distributed::IMembershipOracle\n{\n"
    "  public:\n    int Explain() const { return 0~sc~ }\n}~sc~\n")
string(REPLACE ";" "" subject_own-fake "${subject_own-fake}")

# The same declaration wrapped, which is what a formatter produces once the name is long. A
# line-based reader fails OPEN on exactly this.
set(subject_wrapped
    "class AVeryLongFakeNameThatForcesAWrap final:\n"
    "    public Distributed::IMembershipOracle\n{\n}~sc~\n")
string(REPLACE ";" "" subject_wrapped "${subject_wrapped}")

set(subject_commented
    "// class MyOwnMembership final: public Distributed::IMembershipOracle\n"
    "/// A fixture could derive from Distributed::IMembershipOracle, and this one does not.\n"
    "/* class Block: public IMembershipOracle {}~sc~ */\n"
    "int Nothing = 1~sc~\n")
string(REPLACE ";" "" subject_commented "${subject_commented}")

set(subject_parameter
    "bool Admits(Distributed::IMembershipOracle const& oracle, std::string_view host)~sc~\n"
    "void Take(Distributed::IMembershipOracle const* oracle)~sc~\n")
string(REPLACE ";" "" subject_parameter "${subject_parameter}")

# ---------------------------------------------------------------------------
# Stage one case's tree and run the check over it.
#
# @param name The case index, which names its directory.
# @param fakes `two` or `none`.
# @param subject The token naming a staged text, or `absent` to stage no test file at all.
# @param exemptionRow A row to put in a COPY of the check, or `-` to run the shipped one.
# @param mode `walk`, `git`, or `none` for a case that refuses before a mode is reached.
# @param outOutput Set to the check's combined output.
# @param outStaged Set to TRUE when the run used a staged copy of the check.
function(fastcached_stage_and_run name fakes subject exemptionRow mode outOutput outStaged)
    set(tree "${FASTCACHED_SCRATCH_DIR}/case-${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/tests")
    file(MAKE_DIRECTORY "${tree}/src/apps/node")

    if(fakes STREQUAL "two")
        file(WRITE "${tree}/src/tests/MembershipFakes.hpp" "${twoFakesHeader}")
    elseif(fakes STREQUAL "none")
        file(WRITE "${tree}/src/tests/MembershipFakes.hpp" "${noFakesHeader}")
    else()
        message(FATAL_ERROR "case `${name}`: unknown fakes kind `${fakes}`")
    endif()

    if(NOT subject STREQUAL "absent")
        if(NOT DEFINED subject_${subject})
            message(FATAL_ERROR "case `${name}`: unknown subject `${subject}`")
        endif()
        file(WRITE "${tree}/src/apps/node/Subject_test.cpp" "${subject_${subject}}")
        # A second test file that is always clean, so a refusal is attributable to the subject
        # rather than to "some file in the tree".
        file(WRITE "${tree}/src/apps/node/Neighbour_test.cpp"
             "#include <tests/MembershipFakes.hpp>\nint Neighbour = 1;\n")
    endif()

    # Which check runs. The shipped one unless the case is about an exemption row.
    set(runCheck "${check}")
    set(staged FALSE)
    if(NOT exemptionRow STREQUAL "-")
        # `~bar~` back to the `|` the check's own row parser wants. It cannot travel as a `|`
        # because this file's case rows are `|`-split themselves.
        string(REPLACE "~bar~" "|" exemptionRow "${exemptionRow}")
        if(NOT GIT_EXECUTABLE AND mode STREQUAL "git")
            message(FATAL_ERROR "case `${name}`: git mode needs git")
        endif()
        file(MAKE_DIRECTORY "${tree}/scripts/lib")
        file(READ "${check}" checkText)
        string(FIND "${checkText}" "set(FastCachedMembershipFakeExemptions)" tableAt)
        if(tableAt EQUAL -1)
            # The table was renamed or reformatted, so this case can no longer inject a row --
            # and would otherwise run the SHIPPED empty table and pass while testing nothing.
            message(FATAL_ERROR
                "case `${name}`: the exemption table declaration was not found in the check, so a "
                "row cannot be staged. This case tests nothing until the anchor is updated.")
        endif()
        string(REPLACE "set(FastCachedMembershipFakeExemptions)"
               "set(FastCachedMembershipFakeExemptions \"${exemptionRow}\")\nmessage(STATUS \"membership-fakes: STAGED-CHECK\")"
               checkText "${checkText}")
        file(WRITE "${tree}/scripts/check-membership-fakes.cmake" "${checkText}")
        file(COPY "${helperLibrary}" DESTINATION "${tree}/scripts/lib")
        set(runCheck "${tree}/scripts/check-membership-fakes.cmake")
        set(staged TRUE)
    endif()

    if(NOT mode MATCHES "^(walk|git|none)$")
        message(FATAL_ERROR "case `${name}`: unknown mode `${mode}`")
    endif()
    if(mode STREQUAL "git")
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR "case `${name}`: git mode needs git, and none was found")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}"
                        RESULT_VARIABLE initStatus OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A
                        RESULT_VARIABLE addStatus OUTPUT_QUIET ERROR_QUIET)
        if(NOT initStatus EQUAL 0 OR NOT addStatus EQUAL 0)
            message(FATAL_ERROR
                "case `${name}`: could not stage a git index (init=${initStatus} add=${addStatus}), so "
                "this case would silently run as a WALK -- the mode it is not about")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${runCheck}"
        OUTPUT_VARIABLE runOutput
        ERROR_VARIABLE runError
        RESULT_VARIABLE runStatus)

    # CMake wraps its diagnostics at a column that depends on this path's length, so a phrase
    # can be in the output and in no line of it. Flatten before anybody matches.
    set(combined "${runOutput}${runError}")
    string(REPLACE "\n" " " combined "${combined}")
    string(REGEX REPLACE "[ \t]+" " " combined "${combined}")

    set(${outOutput} "${combined}" PARENT_SCOPE)
    set(${outStaged} "${staged}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases. `name|fakes|subject|exemption|mode|expect|mustAppear`
#
# `mustAppear` needles are separated by ` && `; `~sc~` stands in for a `;` and `~bar~` for a
# `|`, neither of which can travel literally through a `;`-separated list of `|`-split rows.
set(FastCachedMembershipFakeCases
    # Both directions on a clean tree, and BOTH enumeration modes: the mode is part of the
    # check's output and is asserted per case, because a fixture exercising the walk while CI
    # exercises the index is a guard passing on a path nobody ships.
    "clean-walk|two|uses-shared|-|walk|pass|no test file defines its own"
    "clean-git|two|uses-shared|-|git|pass|no test file defines its own"

    # The refusing direction, and the wrapped arm is the one a line-based reader would miss.
    "duplicate|two|own-fake|-|git|refuse|class MyOwnMembership && 1 duplicated fake(s)"
    "wrapped|two|wrapped|-|git|refuse|class AVeryLongFakeNameThatForcesAWrap"

    # Accepting arms that a pattern gone greedy would fail. Without the `parameter` case a check
    # refusing every mention of the interface would pass `commented` for the wrong reason.
    "commented|two|commented|-|git|pass|no test file defines its own"
    "parameter|two|parameter|-|git|pass|no test file defines its own"

    # The matched-nothing guard: the pattern finding nothing in the shared header is the one
    # failure that makes every result above meaningless, in the direction that reads as clean.
    "no-shared-fakes|none|uses-shared|-|none|refuse|found 0 shared fake(s)"

    # A scan that enumerated nothing must refuse rather than report a clean tree.
    "no-tests|two|absent|-|git|refuse|enumerated no test file"

    # The exemption table, both ways round. The honoured arm is why the table is trustworthy at
    # all; the stale arm is why a row cannot outlive its site.
    "exemption-honoured|two|own-fake|src/apps/node/Subject_test.cpp~bar~a staged reason|git|pass|no test file defines its own"
    "stale-exemption|two|uses-shared|src/apps/node/Gone_test.cpp~bar~a staged reason|git|refuse|STALE row(s) && src/apps/node/Gone_test.cpp")

# ---------------------------------------------------------------------------
set(failures "")
set(caseCount 0)

foreach(row IN LISTS FastCachedMembershipFakeCases)
    fastcached_row_fields("${row}" caseName caseFakes caseSubject caseExemption caseMode caseExpect caseMustAppear)
    math(EXPR caseCount "${caseCount} + 1")

    fastcached_stage_and_run("${caseName}" "${caseFakes}" "${caseSubject}" "${caseExemption}"
                             "${caseMode}" output staged)

    # WHICH check ran, on every case that staged one. A case that silently fell back to the
    # shipped (empty) table would pass while asserting nothing about exemptions -- and in the
    # refusing row it would look exactly like a case that worked.
    if(NOT caseExemption STREQUAL "-")
        string(FIND "${output}" "membership-fakes: STAGED-CHECK" position)
        if(position EQUAL -1)
            list(APPEND failures
                 "${caseName}: the run did not report the STAGED check, so it may have been judged against the shipped exemption table")
        endif()
    endif()

    # The enumeration mode, asserted both ways: a case cannot quietly exercise the path it is
    # not about.
    if(caseMode STREQUAL "none")
        # NO mode, and that is the assertion rather than the absence of one: this case's tree
        # refuses at the positive anchor, which the check asks BEFORE it enumerates. So the
        # ORDERING is what is pinned here -- an anchor asked after the walk would print a mode,
        # and a tree whose pattern matches nothing would have been enumerated for a verdict
        # that could not mean anything.
        foreach(spelling "membership-fakes: mode: git-index" "membership-fakes: mode: walk")
            string(FIND "${output}" "${spelling}" position)
            if(NOT position EQUAL -1)
                list(APPEND failures
                     "${caseName}: saw `${spelling}`, so the enumeration ran before the pattern was anchored")
            endif()
        endforeach()
    else()
        if(caseMode STREQUAL "git")
            set(wanted "membership-fakes: mode: git-index")
            set(unwanted "membership-fakes: mode: walk")
        else()
            set(wanted "membership-fakes: mode: walk")
            set(unwanted "membership-fakes: mode: git-index")
        endif()
        string(FIND "${output}" "${wanted}" position)
        if(position EQUAL -1)
            list(APPEND failures "${caseName}: expected `${wanted}` and did not see it")
        endif()
        string(FIND "${output}" "${unwanted}" position)
        if(NOT position EQUAL -1)
            list(APPEND failures "${caseName}: ran as `${unwanted}`, which is not the path this case is about")
        endif()
    endif()

    # The verdict, read from the check's own text -- and the failure signal is TWO words.
    #
    # This harness runs the check as a SUB-PROCESS, so ctest never sees that output and cannot
    # apply its own `FAIL_REGULAR_EXPRESSION`: the whole pattern has to be spelled here or a
    # sub-run that merely WARNS is scored a clean pass while ctest would refuse it.
    # `message(WARNING)` exits 0 on every CMake and prints `CMake Warning`, which is why the
    # status cannot answer this either. Both words on one line, which is also what
    # `script-check-signals` reads -- it found this file matching `CMake Error` alone.
    if(output MATCHES "CMake Error|CMake Warning")
        set(refusedHere TRUE)
    else()
        set(refusedHere FALSE)
    endif()
    if(caseExpect STREQUAL "refuse")
        if(NOT refusedHere)
            list(APPEND failures "${caseName}: expected a refusal and the check did not refuse")
        endif()
    elseif(caseExpect STREQUAL "pass")
        if(refusedHere)
            list(APPEND failures "${caseName}: expected a clean run and the check refused")
        endif()
    else()
        message(FATAL_ERROR "case `${caseName}`: unknown expectation `${caseExpect}`")
    endif()

    if(NOT caseMustAppear STREQUAL "-")
        string(REPLACE " && " ";" needles "${caseMustAppear}")
        foreach(needle IN LISTS needles)
            string(REPLACE "~sc~" ";" needle "${needle}")
            string(FIND "${output}" "${needle}" position)
            if(position EQUAL -1)
                list(APPEND failures "${caseName}: expected to see `${needle}` and did not")
            endif()
        endforeach()
    endif()
endforeach()

# How many cases RAN, printed whatever the verdict: a self-test that stopped early must not look
# like one that judged something.
message(STATUS "membership-fakes-selftest: ${caseCount} case(s) ran")

if(NOT failures STREQUAL "")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    list(LENGTH failures failureCount)
    message(FATAL_ERROR "membership-fakes-selftest: ${failureCount} case(s) failed of ${caseCount}")
endif()

message(STATUS
    "membership-fakes-selftest: all ${caseCount} case(s) behaved -- the check refuses a duplicated "
    "fake (wrapped or not), accepts a mention and a parameter, refuses when its own pattern finds "
    "no shared fake, refuses an empty enumeration, and honours an exemption row without outliving it")
