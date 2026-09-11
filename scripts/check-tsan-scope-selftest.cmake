# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated.
cmake_minimum_required(VERSION 3.28)
#
# `tsan-scope-hygiene` must be SEEN to refuse, on each thing it claims and on
# nothing else -- and to ACCEPT, which is the direction a guard is not known to
# work in until somebody has watched it.
#
# `tsan-scope-hygiene` runs the check against THIS tree, where a green run is
# equally consistent with the check working and with the check having quietly
# stopped reading. #317 is exactly that: a file-level match reported COVERED over
# a case that had left the sanitized scope, and every signal said clean. So the
# decisions get driven over trees this file stages.
#
# ## The trees are staged FROM the check, never from a copy of it
#
# This file `include()`s `check-tsan-scope.cmake` with
# `FastCachedTsanScopeDefinitionsOnly` set, which returns before the scan, and
# then stages one file per row of the check's own `FastCachedTsanScope` and
# copies the real `scripts/tsan-gate.sh` and the real root `CMakeLists.txt` in
# beside them. So a row added to the check is a row this file stages, and the
# cases run against the tags the gate actually spells.
#
# A self-test carrying its own two-row scope would be the defect the check's
# header already records once: a second list is not a cross-check, it is a second
# thing to be wrong -- and one that had drifted would agree with itself perfectly
# on every run.
#
# ## The verdict is the OUTPUT, and it is FLATTENED first
#
# `message(WARNING)` exits 0 on every CMake while printing a diagnostic, so a
# check that merely warned would pass an exit-code test. And **CMake WRAPS a
# diagnostic at about 74 columns**, so a phrase can exist in the output and in no
# single LINE of it -- this repository has measured zero matches for a
# `FATAL_ERROR` phrase crossing that column, and a mutation harness once called
# all three of its arms green while the thing it drove was red. Every expectation
# here is matched against the flattened text.
#
# ## A spawn that never RAN is not a verdict
#
# `execute_process` reports a failure to LAUNCH as a STRING rather than a number,
# and a discarded `RESULT_VARIABLE` reads that as the check having answered --
# every `refuse` arm then passes and every `pass` arm fails, which presents as a
# rule regression and sends the next reader hunting a defect that is not there
# (#747, on `check-net-boundary-selftest`). It is INCONCLUSIVE here and says so.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-tsan-scope-selftest.cmake
#
# Exit: the verdict is the OUTPUT, not the status -- read through
# `FAIL_REGULAR_EXPRESSION`, because a `-P` script that merely WARNS exits 0.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(FastCachedScopeCheck "${FASTCACHED_SOURCE_DIR}/scripts/check-tsan-scope.cmake")
if(NOT EXISTS "${FastCachedScopeCheck}")
    message(FATAL_ERROR "the check under test is missing: ${FastCachedScopeCheck}")
endif()

# Definitions only: this returns before the scan, so nothing here has to restate
# the scope table, the gate path or the tags parsed out of it.
set(FastCachedTsanScopeDefinitionsOnly TRUE)
include("${FastCachedScopeCheck}")

if(NOT FastCachedTsanScope OR NOT FastCachedTsanScopeTags)
    message(FATAL_ERROR
        "check-tsan-scope-selftest: including ${FastCachedScopeCheck} yielded no "
        "scope table or no tags.\n"
        "Every case below stages from those, so an empty one would run twenty "
        "cases against nothing and report them all green -- which is this "
        "file's own subject, one level up.")
endif()

# Every tree starts with `file(REMOVE_RECURSE)` on a caller-supplied path, so the
# path is checked before anything is deleted. Absolute and at least two segments
# deep: a relative one would be resolved against whatever directory ctest ran
# this in, and `/` or `/x` is nothing a scratch tree should ever be.
if(NOT IS_ABSOLUTE "${FASTCACHED_SCRATCH_DIR}"
   OR NOT FASTCACHED_SCRATCH_DIR MATCHES "[^/\\]+[/\\][^/\\]+")
    message(FATAL_ERROR
        "check-tsan-scope-selftest: FASTCACHED_SCRATCH_DIR "
        "(${FASTCACHED_SCRATCH_DIR}) must be an absolute path at least two "
        "segments deep; this file removes it recursively.")
endif()

# A guard nobody has watched refuse is not a guard, and a guard nobody has
# watched ACCEPT is not known to work -- #1031 is two days of a gate failing
# CLOSED and unconditionally with a confidently worded false cause. So every case
# below states the direction it drives, and the first one is the accepting one.
#
# **The synthetic trees are staged from `FastCachedTsanScope` and the REAL gate,
# never from a copy of either.** Each tree is a `src/` mirroring every row of that
# table plus a copy of `scripts/tsan-gate.sh`, so the child invocation runs the
# shipped scope table and the shipped tag expression against files this function
# wrote. A self-test that carried its own two-row scope would be a second thing
# to be wrong, which is the defect the check's own header records once.
#
# The verdict is read from the child's OUTPUT and never from its exit code alone:
# `message(WARNING)` exits 0 while printing a diagnostic, so a check that only
# warned would pass an exit-code test. **And CMake WRAPS a diagnostic at about 74
# columns**, so every expectation is matched against the FLATTENED output --
# measured in this repository at zero matches for a `FATAL_ERROR` phrase that
# crosses the column, with a mutation harness once calling all three of its arms
# green while the thing it drove was red.
list(GET FastCachedTsanScopeTags 0 selftestTag)

set(selftestRan 0)
set(selftestFailed 0)

# Stage a tree in which every row of the real scope table exists and is
# covered. Returns the directory. `which` names the case, so a failure names
# a directory somebody can go and look at rather than one shared path that
# the next case has already overwritten.
function(FastCachedStageTree which out)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${which}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/scripts")
    file(COPY "${FastCachedTsanGate}" DESTINATION "${tree}/scripts")
    # The Catch2 watermark (#312) reads the root CMakeLists, so a tree without
    # one refuses before any scope case is reached. Copied rather than
    # synthesised, so the baseline exercises the real declaration.
    file(COPY "${FastCachedProjectCMakeLists}" DESTINATION "${tree}")
    foreach(row IN LISTS FastCachedTsanScope)
        if(row MATCHES "_test\\.cpp$")
            set(staged "${tree}/${row}")
        else()
            set(staged "${tree}/${row}/Baseline_test.cpp")
        endif()
        file(WRITE "${staged}"
            "TEST_CASE(\"baseline\", \"[${selftestTag}]\")\n{\n}\n")
    endforeach()
    set("${out}" "${tree}" PARENT_SCOPE)
endfunction()

# One case. `expect` is `pass` or `refuse`; `needle` is a phrase that must
# appear in the flattened output either way -- so the accepting direction
# asserts something POSITIVE was reported and not merely that nothing was.
function(FastCachedSelftestCase which tree expect needle)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}"
                -P "${FastCachedScopeCheck}"
        OUTPUT_VARIABLE childOut
        ERROR_VARIABLE childErr
        RESULT_VARIABLE childStatus
        ENCODING NONE)

    # A spawn that never RAN is not a verdict. `execute_process` reports a
    # failure to LAUNCH as a STRING rather than a number, and reading that as an
    # answer makes every `refuse` arm PASS -- output empty, no `CMake Error`, so
    # `verdict` reads `pass` -- while every accepting arm fails. That presents as
    # a scope regression and sends the next reader hunting a defect that is not
    # there (#747, measured on `check-net-boundary-selftest`).
    if(NOT childStatus MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "check-tsan-scope-selftest: INCONCLUSIVE -- the check could not be "
            "RUN for case ${which} (${childStatus}). No arm was evaluated, so "
            "no arm's verdict means anything. This is NOT a scope regression; "
            "re-run, and if it persists the spawn is the subject.")
    endif()

    string(REGEX REPLACE "[\r\n]+" " " flat "${childOut} ${childErr}")
    string(REGEX REPLACE " +" " " flat "${flat}")

    # The failure signal is TWO words, and this harness reads the sub-run's
    # output ITSELF -- ctest never sees it -- so it has to spell the whole
    # pattern. Matching `CMake Error` alone scores a sub-run that merely WARNS
    # as a clean pass here while ctest's FAIL_REGULAR_EXPRESSION refuses it,
    # which makes this harness MORE PERMISSIVE than the thing it stands for.
    # `message(WARNING)` exits 0 on every CMake, so the status arm cannot cover
    # it either. Both words on one line, because the scan reads a line at a time.
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

# The file every mutating case reaches for: a covered file inside a scoped
# DIRECTORY, so an added case can be shown to leave the scope while the file
# around it stays covered. That arrangement IS #317 -- a per-file check
# passes on it.
set(victim "src/FastCache/Async/Baseline_test.cpp")

message("== check-tsan-scope-selftest")

# -- the accepting direction, first, and asserting a positive report -------
FastCachedStageTree("baseline" tree)
FastCachedSelftestCase("baseline-tree-is-accepted" "${tree}" pass "case.s. in")

FastCachedStageTree("multiline" tree)
file(WRITE "${tree}/${victim}"
    "TEST_CASE(\"a name long enough to push the tag string onto its own line\",\n"
    "          \"[${selftestTag}]\")\n{\n}\n")
FastCachedSelftestCase("tag-string-on-the-next-line" "${tree}" pass "case.s. in")

FastCachedStageTree("adjacent" tree)
file(WRITE "${tree}/${victim}"
    "TEST_CASE(\"first half of a name \"\n"
    "          \"second half of a name\",\n"
    "          \"[${selftestTag}]\")\n{\n}\n")
FastCachedSelftestCase("a-name-spelled-as-two-adjacent-literals" "${tree}" pass "case.s. in")

# An UNMATCHED parenthesis, not a matched pair. A reader that counted
# parentheses without removing the string literals first would still close a
# matched pair correctly and pass such a case for the wrong reason; one `(`
# leaves it inside the header, swallowing every case after it. So this case
# is driven in the REFUSING direction, with an unselected case behind the
# tricky one: the finding proves the reader got past it.
FastCachedStageTree("parens" tree)
file(WRITE "${tree}/${victim}"
    "TEST_CASE(\"a name with an unmatched ( in it\", \"[${selftestTag}]\")\n{\n}\n"
    "TEST_CASE(\"the case behind it\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("an-unmatched-parenthesis-inside-a-case-name" "${tree}" refuse "the case behind it")

FastCachedStageTree("brackets-only" tree)
file(WRITE "${tree}/${victim}"
    "// a comment carrying an unbalanced ] bracket\n"
    "TEST_CASE(\"ordinary\", \"[${selftestTag}]\")\n{\n}\n")
FastCachedSelftestCase("an-unbalanced-bracket-alone-changes-nothing" "${tree}" pass "case.s. in")

# -- the refusing direction ------------------------------------------------
FastCachedStageTree("added-case" tree)
file(APPEND "${tree}/${victim}"
    "TEST_CASE(\"added later\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("an-unselected-case-in-a-covered-file" "${tree}" refuse "added later")

FastCachedStageTree("brackets-and-violation" tree)
file(APPEND "${tree}/${victim}"
    "// a comment carrying an unbalanced ] bracket\n"
    "TEST_CASE(\"added later\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("a-violation-behind-an-unbalanced-bracket" "${tree}" refuse "added later")

# The REPORT is text out of a source file, so it has the hazard this file is
# about. Two case names in the current scope contain a `;`, which splits a
# `list(APPEND)` element in two; a `[` would merge two findings into one and
# a refusal would then name fewer cases than it found. Both characters, in
# one name, asserted to come back WHOLE.
FastCachedStageTree("punctuated-name" tree)
file(APPEND "${tree}/${victim}"
    "TEST_CASE(\"a name with a ; and an unbalanced [ in it\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("a-finding-whose-name-holds-a-semicolon-and-a-bracket"
    "${tree}" refuse "a name with a ; and an unbalanced . in it")

FastCachedStageTree("no-tag-string" tree)
file(APPEND "${tree}/${victim}" "TEST_CASE(\"untagged\")\n{\n}\n")
FastCachedSelftestCase("a-case-with-no-tag-string" "${tree}" refuse "no tag string at all")

FastCachedStageTree("name-carries-a-tag" tree)
file(APPEND "${tree}/${victim}"
    "TEST_CASE(\"prose about [${selftestTag}] behaviour\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("a-name-cannot-talk-a-case-into-the-scope" "${tree}" refuse "prose about")

FastCachedStageTree("scenario" tree)
file(APPEND "${tree}/${victim}" "SCENARIO(\"a scenario\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("SCENARIO-is-read-too" "${tree}" refuse "a scenario")

FastCachedStageTree("test-case-method" tree)
file(APPEND "${tree}/${victim}"
    "TEST_CASE_METHOD(Fixture, \"a fixtured case\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("TEST_CASE_METHOD-is-read-too" "${tree}" refuse "a fixtured case")

FastCachedStageTree("runaway" tree)
file(APPEND "${tree}/${victim}" "TEST_CASE(\"never closed\",\n")
FastCachedSelftestCase("a-header-that-never-closes" "${tree}" refuse "still open at end of file")

# The same fault far enough from the end of the file to hit the runaway bound
# instead of the end-of-file arm. Two arms, because they are reported by two
# different messages and a reader meeting one of them must not be told the
# other one's story.
FastCachedStageTree("runaway-bound" tree)
file(APPEND "${tree}/${victim}" "TEST_CASE(\"never closed\",
")
foreach(filler RANGE 1 70)
    file(APPEND "${tree}/${victim}" "// filler line ${filler}
")
endforeach()
FastCachedSelftestCase("a-header-that-outruns-the-line-bound" "${tree}" refuse "has no closing")

FastCachedStageTree("no-cases" tree)
foreach(row IN LISTS FastCachedTsanScope)
    if(row MATCHES "_test\\.cpp$")
        file(WRITE "${tree}/${row}" "// no cases here\n")
    else()
        file(WRITE "${tree}/${row}/Baseline_test.cpp" "// no cases here\n")
    endif()
endforeach()
FastCachedSelftestCase("a-scope-that-holds-no-case-at-all" "${tree}" refuse "found no Catch2 case")

FastCachedStageTree("empty-directory" tree)
file(REMOVE "${tree}/src/FastCache/Async/Baseline_test.cpp")
FastCachedSelftestCase("a-directory-row-with-no-test-files" "${tree}" refuse "contains no ..test.cpp files")

FastCachedStageTree("missing-file-row" tree)
file(REMOVE "${tree}/src/FastCache/Core/Clock_test.cpp")
FastCachedSelftestCase("a-file-row-that-does-not-exist" "${tree}" refuse "names neither a directory nor a file")

# -- the Catch2 watermark (#312) ------------------------------------------
# The accepting arm is the baseline case above, which stages the real
# declaration; these two are the directions that must not be silent. The
# bumped one is what the whole tripwire exists to do, and the missing one is
# the reading it must never take as "no, labels are not available".
FastCachedStageTree("catch2-bumped" tree)
file(READ "${tree}/CMakeLists.txt" bumped)
string(REPLACE "VERSION ${FastCachedCatch2TagLabelWatermark}" "VERSION 3.9.0" bumped "${bumped}")
file(WRITE "${tree}/CMakeLists.txt" "${bumped}")
FastCachedSelftestCase("a-catch2-bump-past-the-watermark" "${tree}" refuse "past the")

FastCachedStageTree("catch2-undeclared" tree)
file(READ "${tree}/CMakeLists.txt" undeclared)
string(REPLACE "NAME Catch2" "NAME SomethingElse" undeclared "${undeclared}")
file(WRITE "${tree}/CMakeLists.txt" "${undeclared}")
FastCachedSelftestCase("a-catch2-declaration-this-cannot-read" "${tree}" refuse "could not read a")

FastCachedStageTree("no-cmakelists" tree)
file(REMOVE "${tree}/CMakeLists.txt")
FastCachedSelftestCase("a-tree-with-no-root-CMakeLists" "${tree}" refuse "does not exist")

# The count is printed because a self-test that STOPPED early must not look
# like one that judged everything: `set -e`'s CMake equivalent is a
# FATAL_ERROR anywhere above, and eight cases reported green is what that
# looks like from the outside.
message("check-tsan-scope-selftest: ${selftestRan} case(s) ran, ${selftestFailed} failed")
if(NOT selftestFailed EQUAL 0)
    message(FATAL_ERROR
        "check-tsan-scope-selftest: ${selftestFailed} of ${selftestRan} "
        "case(s) failed.\n"
        "The cases are in ${CMAKE_CURRENT_LIST_FILE}; the rule they drive is in "
        "${FastCachedScopeCheck}. Read the FAIL line before either: it says "
        "whether the check took the wrong DIRECTION or merely failed to REPORT "
        "the phrase the case asked for, and those are fixed in different files.")
endif()
