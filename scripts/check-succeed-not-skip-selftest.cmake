# SPDX-License-Identifier: Apache-2.0
#
# `succeed-not-skip` must be SEEN to fail, and on every direction it claims.
#
# The check exists because `SUCCEED` where a case could not RUN reports a green result
# for a property nothing established (#685). A guard for that is worth nothing until it
# has been watched refusing something, and worth less than nothing if it refuses
# everything -- so this drives the real check against synthetic trees and asserts each
# verdict SEPARATELY rather than inverting one run with `WILL_FAIL`.
#
# A mutation that reddens everything must not pass here. That is why five cases assert
# the check stays SILENT and each refusing case asserts the phrase naming ITS OWN
# refusal: a check that objected for the wrong reason is a check that will object to the
# wrong code tomorrow.
#
# The trees are built here rather than committed, because a fixture holding a
# deliberately-wrong `SUCCEED` would be a violation of the rule under test, and
# excluding it from the scan would put a hole in the thing being proved.
#
# Ten cases:
#
#   clean          a `SUCCEED` that ends a case PASSES -- an always-failing check is as
#                  useless as an always-passing one
#   bare           `SUCCEED()` with no message PASSES. `Ranges_test.cpp` uses it after a
#                  block of static_asserts: a case that ran with nothing left to assert
#   commented      the idiom quoted in a COMMENT PASSES. This rule is written out in
#                  `.agent/rules/testing.md` and cited in test comments, so a scan that
#                  could not tell prose from code would refuse its own documentation
#   bail-out       `SUCCEED` followed by `return;` FAILS, and NAMES the file
#   vocabulary     a skip-vocabulary message with NO return FAILS. The two signals must
#                  each work alone, or the check is really one signal with a spare
#   unreadable     a `SUCCEED` this scan cannot read on one line FAILS as its own
#                  outcome -- "could not determine" is not "fine"
#   none           test sources with no `SUCCEED` at all FAIL as a broken scan
#   no-sources     a tree with no test source at all FAILS as a broken scan, and says so
#                  differently, because a moved source root and a renamed macro are two
#                  different repairs
#   excluded       a violation under a build tree is IGNORED -- Catch2's own self-tests
#                  are full of `SUCCEED` and nobody here can edit them
#   git            the mode CI actually takes: an untracked vendored violation is
#                  ignored, a TRACKED one is still caught
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check reports
# failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-succeed-not-skip-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-succeed-not-skip.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

# A literal semicolon, which CMake will not let a quoted argument carry: a
# backslash-semicolon inside a quoted string survives into the VALUE as both
# characters, so a fixture written that way lands on disk as source no compiler
# would accept -- and source whose `return` the check cannot recognise. It read as
# the check failing to catch the bail-out shape, when the shape was never in the
# fixture at all. Measured: the bail-out case passed until this was fixed.
string(ASCII 59 SEMI)

set(root "${FASTCACHED_SCRATCH_DIR}/succeed-not-skip-selftest")
file(REMOVE_RECURSE "${root}")
set(failures "")

# A `SUCCEED` that is legitimate: the case ran and had nothing left to assert. Present in
# most trees below so the check's own emptiness guard -- which refuses a scan that found
# no `SUCCEED` at all -- does not fire and mask the verdict actually under test.
set(cleanSite "TEST_CASE(\"ran\")\n{\n    SUCCEED(\"the completion landed on a live block\")${SEMI}\n}\n")

# Build a synthetic tree holding one test source.
# @param name Which case; also the directory.
# @param relative Where the source goes, relative to the tree root.
# @param body What it contains.
# @param outVar Receives the tree path.
function(fastcached_make_tree name relative body outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    get_filename_component(directory "${tree}/${relative}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${tree}/${relative}" "${body}")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the check against a tree and say whether it objected.
# @param tree The tree to scan.
# @param outObjected Receives TRUE when the check reported a failure.
# @param outOutput Receives everything it printed.
function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured
        ERROR_VARIABLE capturedErrors
        RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")

    # The verdict is read from the OUTPUT, never the exit code: `message(FATAL_ERROR)`
    # exits 0 on CMake 3.28, this project's declared minimum, and 1 on 4.x. Reading the
    # status would make this selftest agree with the check on one host and disagree on
    # the other.
    string(FIND "${combined}" "CMake Error" position)
    if(position EQUAL -1)
        set(${outObjected} FALSE PARENT_SCOPE)
    else()
        set(${outObjected} TRUE PARENT_SCOPE)
    endif()
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# Assert some text appears in what the check printed.
# @param output What the check printed.
# @param needle The phrase that must appear.
# @param complaint What to record when it does not.
macro(fastcached_expect_text output needle complaint)
    string(FIND "${output}" "${needle}" fastcachedNeedlePosition)
    if(fastcachedNeedlePosition EQUAL -1)
        list(APPEND failures "${complaint}")
    endif()
endmacro()

# 1. Clean. The check must stay silent on a `SUCCEED` that ends a case.
fastcached_make_tree("clean" "src/thing/Thing_test.cpp" "${cleanSite}" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "clean: the check objected to a SUCCEED that ends a case -- it now refuses everything, which is as useless as refusing nothing")
endif()

# And it must say it took the WALK. A synthetic tree is not a git repository, so every
# case above the git ones exercises the fallback -- asserted on both sides so neither half
# can quietly become the only one exercised.
fastcached_expect_text("${output}" "directory walk (no git index)"
    "clean: the check did not report scanning via the directory walk, so the fallback -- the mode a release tarball with no git index takes -- was not the mode this case exercised")

# 2. `SUCCEED()` with no message. A case that ran with nothing left to assert.
fastcached_make_tree("bare" "src/thing/Thing_test.cpp"
    "TEST_CASE(\"ranges\")\n{\n    static_assert(true)${SEMI}\n    SUCCEED()${SEMI}\n}\n" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "bare: SUCCEED() with no message was reported, but a case that ran with nothing left to assert is exactly what SUCCEED is for")
endif()

# 3. The idiom in a COMMENT. This rule is documented in prose that quotes it, so a scan
#    that cannot tell prose from code refuses the documentation of its own rule.
fastcached_make_tree("commented" "src/thing/Thing_test.cpp"
    "// Never write SUCCEED(\"no loopback listener available on this host\")${SEMI}\n${cleanSite}" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "commented: a SUCCEED quoted in a comment was reported as code, so this check refuses the prose that documents it")
endif()

# 4. The bail-out shape -- the structural half of the defect.
fastcached_make_tree("bail-out" "src/thing/Thing_test.cpp"
    "TEST_CASE(\"bails\")\n{\n    if (!ready)\n    {\n        SUCCEED(\"all good here\")${SEMI}\n        return${SEMI}\n    }\n    CHECK(Property())${SEMI}\n}\n${cleanSite}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "bail-out: a SUCCEED followed by a bare `return` did not fail the check -- that is the shape every one of #685's twenty-one sites had")
else()
    fastcached_expect_text("${output}" "src/thing/Thing_test.cpp"
        "bail-out: the check objected but did not name the offending file, so a person reading the failure cannot act on it")
    fastcached_expect_text("${output}" "followed by a bare"
        "bail-out: the check objected for some other reason than the bail-out shape, so the structural signal is not what fired and may not work at all")
endif()

# 5. Skip vocabulary with NO return. The two signals must each work alone, or one of them
#    is decoration -- and it is the message half that catches the sites written as an
#    `if`/`else` rather than an early return.
fastcached_make_tree("vocabulary" "src/thing/Thing_test.cpp"
    "TEST_CASE(\"cannot\")\n{\n    if (listener == nullptr)\n        SUCCEED(\"no loopback listener available on this host\")${SEMI}\n    else\n        CHECK(Property())${SEMI}\n}\n${cleanSite}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "vocabulary: a SUCCEED whose message says the facility was unavailable passed, so the message signal does nothing on its own and only the bail-out shape is really checked")
else()
    fastcached_expect_text("${output}" "says a facility was not available"
        "vocabulary: the check objected for some other reason than the message, so the vocabulary table is not what fired")
endif()

# 6. Unreadable. Inconclusive is refused as its own outcome, never scored as clean.
fastcached_make_tree("unreadable" "src/thing/Thing_test.cpp"
    "TEST_CASE(\"split\")\n{\n    SUCCEED(\n        \"a message on its own line\")${SEMI}\n}\n${cleanSite}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "unreadable: a SUCCEED this scan cannot read was passed over silently -- a site it could not read is not a site it cleared")
else()
    fastcached_expect_text("${output}" "cannot read the SUCCEED on one line"
        "unreadable: the check objected for some other reason, so it is not reporting the inconclusive case as its own outcome")
endif()

# 7. Nothing to find. Must be a broken scan, never a clean tree.
fastcached_make_tree("none" "src/thing/Thing_test.cpp"
    "TEST_CASE(\"ordinary\")\n{\n    CHECK(Property())${SEMI}\n}\n" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "none: test sources with no SUCCEED at all passed as clean; zero findings must be reported as a scan that stopped working")
else()
    fastcached_expect_text("${output}" "the scan matched nothing and cannot conclude"
        "none: the check objected without saying its scan found no SUCCEED, so a renamed macro would read as ordinary source")
endif()

# 8. No test source at all -- a different repair from case 7, so a different sentence.
fastcached_make_tree("no-sources" "src/thing/Thing.cpp"
    "int Thing()\n{\n    return 0${SEMI}\n}\n" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "no-sources: a tree with no test source at all passed; a moved source root must be reported, not scored as a clean tree")
else()
    fastcached_expect_text("${output}" "matched no test sources"
        "no-sources: the check objected without distinguishing an empty scan from a scan that found sources but no SUCCEED -- two different repairs, one message")
endif()

# 9. A build tree. Catch2's own self-tests are full of `SUCCEED` and are not ours to edit.
fastcached_make_tree("excluded" "src/thing/Thing_test.cpp" "${cleanSite}" tree)
file(MAKE_DIRECTORY "${tree}/out/build/x/_deps/catch2-src/tests")
file(WRITE "${tree}/out/build/x/_deps/catch2-src/tests/Vendored_test.cpp"
     "TEST_CASE(\"vendored\")\n{\n    SUCCEED(\"cannot run here\")${SEMI}\n    return${SEMI}\n}\n")
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "excluded: a violation inside a build tree was reported -- vendored third-party code is not ours to edit and must not fail this check")
endif()

# 10. The GIT path, which is the one CI takes and which no case above reaches -- a
#     synthetic tree is not a git repository. That gap is how the sibling check's
#     equivalent went untested until CI found a vendored file the walk excluded by name.
#
#     `untracked` plants a violation where CPM actually puts one and leaves it untracked;
#     it must be ignored. `tracked-bad` tracks one; it must still be caught, so that
#     "ignore what git does not track" cannot degrade into "ignore everything".
find_program(FASTCACHED_GIT NAMES git)
if(NOT FASTCACHED_GIT)
    list(APPEND failures
         "git-mode: no git executable, so the mode CI actually uses could not be exercised at all -- inconclusive, not a pass")
else()
    foreach(case "untracked" "tracked-bad")
        set(tree "${root}/git-${case}")
        file(REMOVE_RECURSE "${tree}")
        file(MAKE_DIRECTORY "${tree}/src/thing")
        file(WRITE "${tree}/src/thing/Thing_test.cpp" "${cleanSite}")
        file(MAKE_DIRECTORY "${tree}/.cache/CPM/catch2/deadbeef/tests")
        file(WRITE "${tree}/.cache/CPM/catch2/deadbeef/tests/Vendored_test.cpp"
             "TEST_CASE(\"vendored\")\n{\n    SUCCEED(\"cannot run here\")${SEMI}\n}\n")

        execute_process(COMMAND "${FASTCACHED_GIT}" init -q "${tree}" RESULT_VARIABLE ignored)
        execute_process(COMMAND "${FASTCACHED_GIT}" -C "${tree}" add "src/thing/Thing_test.cpp"
                        RESULT_VARIABLE ignored)
        if(case STREQUAL "tracked-bad")
            # Forced in, since a real checkout would have it ignored.
            execute_process(COMMAND "${FASTCACHED_GIT}" -C "${tree}" add -f
                            ".cache/CPM/catch2/deadbeef/tests/Vendored_test.cpp"
                            RESULT_VARIABLE ignored)
        endif()

        fastcached_run_check("${tree}" objected output)
        if(case STREQUAL "untracked")
            if(objected)
                list(APPEND failures
                     "git-untracked: a violation git does not track was reported -- CPM resolves dependencies inside the source tree and nobody here can edit them")
            endif()
            fastcached_expect_text("${output}" "git ls-files"
                "git-untracked: the check did not report scanning via git ls-files, so this case exercised the fallback and proves nothing about the mode CI uses")
        elseif(NOT objected)
            list(APPEND failures
                 "git-tracked-bad: a TRACKED violation was not reported -- deriving the set from git must not degrade into ignoring everything")
        endif()
    endforeach()
endif()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`succeed-not-skip` is what stops a case that could not RUN from reporting a")
    message("PASS. Each case above drives it against a synthetic tree and asserts ONE")
    message("verdict -- and each refusal is asserted by the phrase naming that refusal,")
    message("so a mutation reddening everything fails here rather than looking thorough.")
    message(FATAL_ERROR "succeed-not-skip selftest: ${failureCount} verdict(s) wrong")
endif()

message(STATUS "succeed-not-skip selftest: 10 synthetic tree(s), every verdict as expected")
