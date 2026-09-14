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
# decisions get driven over a tree this file stages.
#
# ## The tree is staged FROM the check, never from a copy of it
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

# The tree is staged by `file(REMOVE_RECURSE)` on the caller-supplied path itself,
# so the path is checked before anything is deleted. Absolute and at least two segments
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
# **The synthetic tree is staged from `FastCachedTsanScope` and the REAL gate,
# never from a copy of either.** It is a `src/` mirroring every row of that table
# plus a copy of `scripts/tsan-gate.sh`, so the child invocation runs the shipped
# scope table and the shipped tag expression against files this file wrote. A
# self-test that carried its own two-row scope would be a second thing to be
# wrong, which is the defect the check's own header records once.
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

# ## One tree, staged once, restored after every case
#
# Every case runs against ONE tree, in which every row of the real scope table
# exists and is covered. A case changes it only through `FastCachedWrite`,
# `FastCachedAppend` and `FastCachedRemove`, which record the path, and
# `FastCachedSelftestCase` restores exactly the recorded paths once the verdict
# is in. A tree per case cost 23 stagings of 13 files, and on DrvFs under a
# gate's load that alone took this test to 116 s of its 120 s (#1423).
# Copying a staged original per case is no cheaper there: a `file(COPY)` cost two
# to three `file(WRITE)`s in the same profile.
#
# A shared tree has one new way to be wrong: a change nobody recorded outlives
# its case, and the next case is judged on a tree it did not set up. A later
# write to the same path would then hide it. So every helper first asserts that
# the path it is about to change is still at its baseline, and the last case
# accepts the restored tree. Together they refuse a leak at the first case that
# touches the same path, or at the end.
#
# What a per-case tree bought was a directory to inspect after a failure, so a
# FAILING case copies the tree to `failed-<case>` before restoring it.
set(tree "${FASTCACHED_SCRATCH_DIR}/tree")
set(FastCachedTouched "")
set(FastCachedLastCase "staging")

# The baseline, read once. The gate and the root CMakeLists are the REAL files'
# bytes, so the baseline exercises the real declarations: the Catch2 watermark
# (#312) reads the root CMakeLists, and a tree without one refuses before any
# scope case is reached.
file(READ "${FastCachedTsanGate}" FastCachedBaselineGate)
file(READ "${FastCachedProjectCMakeLists}" FastCachedBaselineCMakeLists)
set(FastCachedBaselineRow "TEST_CASE(\"baseline\", \"[${selftestTag}]\")\n{\n}\n")

# The path a row is staged at, relative to the tree.
function(FastCachedStagedPath row out)
    if(row MATCHES "_test\.cpp$")
        set("${out}" "${row}" PARENT_SCOPE)
    else()
        set("${out}" "${row}/Baseline_test.cpp" PARENT_SCOPE)
    endif()
endfunction()

# What the baseline holds at @p relative: its text in `<out>` and whether it
# exists at all in `<out>_present`. A path no row names is absent.
function(FastCachedBaselineOf relative out)
    set("${out}_present" TRUE PARENT_SCOPE)
    if(relative STREQUAL "scripts/tsan-gate.sh")
        set("${out}" "${FastCachedBaselineGate}" PARENT_SCOPE)
        return()
    endif()
    if(relative STREQUAL "CMakeLists.txt")
        set("${out}" "${FastCachedBaselineCMakeLists}" PARENT_SCOPE)
        return()
    endif()
    foreach(row IN LISTS FastCachedTsanScope)
        FastCachedStagedPath("${row}" staged)
        if(relative STREQUAL staged)
            set("${out}" "${FastCachedBaselineRow}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set("${out}" "" PARENT_SCOPE)
    set("${out}_present" FALSE PARENT_SCOPE)
endfunction()

# Put one path back as the baseline holds it; a path the baseline does not have
# is removed, so a case that CREATED a file leaves nothing behind either.
function(FastCachedRestorePath relative)
    FastCachedBaselineOf("${relative}" baseline)
    if(baseline_present)
        file(WRITE "${tree}/${relative}" "${baseline}")
    else()
        file(REMOVE "${tree}/${relative}")
    endif()
endfunction()

# Refuse to change a path that an earlier case left changed. Asked once per path
# per case, before its first change.
function(FastCachedRequireBaseline relative)
    if(relative IN_LIST FastCachedTouched)
        return()
    endif()
    FastCachedBaselineOf("${relative}" baseline)
    set(held FALSE)
    if(EXISTS "${tree}/${relative}")
        file(READ "${tree}/${relative}" actual)
        if(baseline_present AND actual STREQUAL baseline)
            set(held TRUE)
        endif()
    elseif(NOT baseline_present)
        set(held TRUE)
    endif()
    if(NOT held)
        message(FATAL_ERROR
            "check-tsan-scope-selftest: ${relative} is no longer at its baseline, and "
            "the last case to run was `${FastCachedLastCase}`. That case or an earlier "
            "one changed it without FastCachedWrite, FastCachedAppend or FastCachedRemove, "
            "so the restore never put it back and every case since was judged on a tree "
            "it did not set up. Every change to ${tree} goes through those three.")
    endif()
endfunction()

function(FastCachedStageTree)
    file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}")
    FastCachedRestorePath("scripts/tsan-gate.sh")
    FastCachedRestorePath("CMakeLists.txt")
    foreach(row IN LISTS FastCachedTsanScope)
        FastCachedStagedPath("${row}" staged)
        FastCachedRestorePath("${staged}")
    endforeach()
endfunction()

# The only ways a case changes the tree. Exactly two arguments, because text
# holding a `;` must arrive as ONE argument: joining an ARGN would drop it.
function(FastCachedWrite relative text)
    if(NOT ARGC EQUAL 2)
        message(FATAL_ERROR "FastCachedWrite takes a path and ONE text argument (${ARGC} given)")
    endif()
    FastCachedRequireBaseline("${relative}")
    file(WRITE "${tree}/${relative}" "${text}")
    list(APPEND FastCachedTouched "${relative}")
    set(FastCachedTouched "${FastCachedTouched}" PARENT_SCOPE)
endfunction()

function(FastCachedAppend relative text)
    if(NOT ARGC EQUAL 2)
        message(FATAL_ERROR "FastCachedAppend takes a path and ONE text argument (${ARGC} given)")
    endif()
    FastCachedRequireBaseline("${relative}")
    file(APPEND "${tree}/${relative}" "${text}")
    list(APPEND FastCachedTouched "${relative}")
    set(FastCachedTouched "${FastCachedTouched}" PARENT_SCOPE)
endfunction()

function(FastCachedRemove relative)
    FastCachedRequireBaseline("${relative}")
    file(REMOVE "${tree}/${relative}")
    list(APPEND FastCachedTouched "${relative}")
    set(FastCachedTouched "${FastCachedTouched}" PARENT_SCOPE)
endfunction()

# One case. `expect` is `pass` or `refuse`; `needle` is a phrase that must
# appear in the flattened output either way -- so the accepting direction
# asserts something POSITIVE was reported and not merely that nothing was.
# Runs against the shared tree, then restores every path the case touched.
function(FastCachedSelftestCase which expect needle)
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
        set(problem "expected to ${expect}, and the verdict was ${verdict}")
    elseif(NOT flat MATCHES "${needle}")
        set(problem "did not report `${needle}`")
    endif()

    math(EXPR selftestRan "${selftestRan} + 1")
    set(selftestRan "${selftestRan}" PARENT_SCOPE)
    if(problem STREQUAL "")
        message("  ok   ${which}")
    else()
        file(COPY "${tree}/" DESTINATION "${FASTCACHED_SCRATCH_DIR}/failed-${which}")
        message("  FAIL ${which}: ${problem} "
                "(its tree is kept in ${FASTCACHED_SCRATCH_DIR}/failed-${which})")
        message("       ${flat}")
        math(EXPR selftestFailed "${selftestFailed} + 1")
        set(selftestFailed "${selftestFailed}" PARENT_SCOPE)
    endif()

    list(REMOVE_DUPLICATES FastCachedTouched)
    foreach(relative IN LISTS FastCachedTouched)
        FastCachedRestorePath("${relative}")
    endforeach()
    set(FastCachedTouched "" PARENT_SCOPE)
    set(FastCachedLastCase "${which}" PARENT_SCOPE)
endfunction()

# The file every mutating case reaches for: a covered file inside a scoped
# DIRECTORY, so an added case can be shown to leave the scope while the file
# around it stays covered. That arrangement IS #317 -- a per-file check
# passes on it.
set(victim "src/FastCache/Async/Baseline_test.cpp")

message("== check-tsan-scope-selftest")

# -- the accepting direction, first, and asserting a positive report -------
FastCachedStageTree()
FastCachedSelftestCase("baseline-tree-is-accepted" pass "case.s. in")

string(CONCAT text
    "TEST_CASE(\"a name long enough to push the tag string onto its own line\",\n"
    "          \"[${selftestTag}]\")\n{\n}\n")
FastCachedWrite("${victim}" "${text}")
FastCachedSelftestCase("tag-string-on-the-next-line" pass "case.s. in")

string(CONCAT text
    "TEST_CASE(\"first half of a name \"\n"
    "          \"second half of a name\",\n"
    "          \"[${selftestTag}]\")\n{\n}\n")
FastCachedWrite("${victim}" "${text}")
FastCachedSelftestCase("a-name-spelled-as-two-adjacent-literals" pass "case.s. in")

# An UNMATCHED parenthesis, not a matched pair. A reader that counted
# parentheses without removing the string literals first would still close a
# matched pair correctly and pass such a case for the wrong reason; one `(`
# leaves it inside the header, swallowing every case after it. So this case
# is driven in the REFUSING direction, with an unselected case behind the
# tricky one: the finding proves the reader got past it.
string(CONCAT text
    "TEST_CASE(\"a name with an unmatched ( in it\", \"[${selftestTag}]\")\n{\n}\n"
    "TEST_CASE(\"the case behind it\", \"[somethingelse]\")\n{\n}\n")
FastCachedWrite("${victim}" "${text}")
FastCachedSelftestCase("an-unmatched-parenthesis-inside-a-case-name" refuse "the case behind it")

string(CONCAT text
    "// a comment carrying an unbalanced ] bracket\n"
    "TEST_CASE(\"ordinary\", \"[${selftestTag}]\")\n{\n}\n")
FastCachedWrite("${victim}" "${text}")
FastCachedSelftestCase("an-unbalanced-bracket-alone-changes-nothing" pass "case.s. in")

# -- the refusing direction ------------------------------------------------
FastCachedAppend("${victim}" "TEST_CASE(\"added later\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("an-unselected-case-in-a-covered-file" refuse "added later")

string(CONCAT text
    "// a comment carrying an unbalanced ] bracket\n"
    "TEST_CASE(\"added later\", \"[somethingelse]\")\n{\n}\n")
FastCachedAppend("${victim}" "${text}")
FastCachedSelftestCase("a-violation-behind-an-unbalanced-bracket" refuse "added later")

# The REPORT is text out of a source file, so it has the hazard this file is
# about. Two case names in the current scope contain a `;`, which splits a
# `list(APPEND)` element in two; a `[` would merge two findings into one and
# a refusal would then name fewer cases than it found. Both characters, in
# one name, asserted to come back WHOLE.
FastCachedAppend("${victim}"
    "TEST_CASE(\"a name with a ; and an unbalanced [ in it\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("a-finding-whose-name-holds-a-semicolon-and-a-bracket"
    refuse "a name with a ; and an unbalanced . in it")

FastCachedAppend("${victim}" "TEST_CASE(\"untagged\")\n{\n}\n")
FastCachedSelftestCase("a-case-with-no-tag-string" refuse "no tag string at all")

FastCachedAppend("${victim}"
    "TEST_CASE(\"prose about [${selftestTag}] behaviour\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("a-name-cannot-talk-a-case-into-the-scope" refuse "prose about")

FastCachedAppend("${victim}" "SCENARIO(\"a scenario\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("SCENARIO-is-read-too" refuse "a scenario")

FastCachedAppend("${victim}"
    "TEST_CASE_METHOD(Fixture, \"a fixtured case\", \"[somethingelse]\")\n{\n}\n")
FastCachedSelftestCase("TEST_CASE_METHOD-is-read-too" refuse "a fixtured case")

FastCachedAppend("${victim}" "TEST_CASE(\"never closed\",\n")
FastCachedSelftestCase("a-header-that-never-closes" refuse "still open at end of file")

# The same fault far enough from the end of the file to hit the runaway bound
# instead of the end-of-file arm. Two arms, because they are reported by two
# different messages and a reader meeting one of them must not be told the
# other one's story. The filler is built in memory and written once.
set(text "TEST_CASE(\"never closed\",\n")
foreach(filler RANGE 1 70)
    string(APPEND text "// filler line ${filler}\n")
endforeach()
FastCachedAppend("${victim}" "${text}")
FastCachedSelftestCase("a-header-that-outruns-the-line-bound" refuse "has no closing")

foreach(row IN LISTS FastCachedTsanScope)
    FastCachedStagedPath("${row}" staged)
    FastCachedWrite("${staged}" "// no cases here\n")
endforeach()
FastCachedSelftestCase("a-scope-that-holds-no-case-at-all" refuse "found no Catch2 case")

FastCachedRemove("${victim}")
FastCachedSelftestCase("a-directory-row-with-no-test-files" refuse "contains no ..test.cpp files")

FastCachedRemove("src/FastCache/Core/Clock_test.cpp")
FastCachedSelftestCase("a-file-row-that-does-not-exist" refuse "names neither a directory nor a file")

# -- the Catch2 watermark (#312) ------------------------------------------
# The accepting arm is the baseline case above, which stages the real
# declaration; these two are the directions that must not be silent. The
# bumped one is what the whole tripwire exists to do, and the missing one is
# the reading it must never take as "no, labels are not available".
file(READ "${tree}/CMakeLists.txt" bumped)
string(REPLACE "VERSION ${FastCachedCatch2TagLabelWatermark}" "VERSION 3.9.0" bumped "${bumped}")
FastCachedWrite("CMakeLists.txt" "${bumped}")
FastCachedSelftestCase("a-catch2-bump-past-the-watermark" refuse "past the")

file(READ "${tree}/CMakeLists.txt" undeclared)
string(REPLACE "NAME Catch2" "NAME SomethingElse" undeclared "${undeclared}")
FastCachedWrite("CMakeLists.txt" "${undeclared}")
FastCachedSelftestCase("a-catch2-declaration-this-cannot-read" refuse "could not read a")

FastCachedRemove("CMakeLists.txt")
FastCachedSelftestCase("a-tree-with-no-root-CMakeLists" refuse "does not exist")

# -- the links column (#134) ------------------------------------------------
# Staged by editing the COPIED gate's first `|first-party"` declaration, so the rows
# the cases mutate are the shipped ones. The accepting arm is the baseline above,
# which carries both declarations the shipped table uses.
foreach(linksCase IN ITEMS "no-declaration" "unknown-declaration" "none-without-reason")
    file(READ "${tree}/scripts/tsan-gate.sh" linksGate)
    string(FIND "${linksGate}" "|first-party\"" linksAt)
    if(linksAt EQUAL -1)
        message(FATAL_ERROR
            "check-tsan-scope-selftest: the gate declares no row `first-party`, so the "
            "links-column cases have nothing to mutate. If every row became `none`, "
            "stage these from one of those instead.")
    endif()
    string(SUBSTRING "${linksGate}" 0 ${linksAt} linksBefore)
    math(EXPR linksAfterAt "${linksAt} + 13")
    string(SUBSTRING "${linksGate}" ${linksAfterAt} -1 linksAfter)
    if(linksCase STREQUAL "no-declaration")
        set(linksGate "${linksBefore}\"${linksAfter}")
        set(linksNeedle "declares nothing about what it links")
    elseif(linksCase STREQUAL "unknown-declaration")
        set(linksGate "${linksBefore}|firstparty\"${linksAfter}")
        set(linksNeedle "declares .firstparty. about what")
    else()
        set(linksGate "${linksBefore}|none: \"${linksAfter}")
        set(linksNeedle "declares .none: . about what")
    endif()
    FastCachedWrite("scripts/tsan-gate.sh" "${linksGate}")
    FastCachedSelftestCase("a-row-with-${linksCase}" refuse "${linksNeedle}")
endforeach()

# -- the restore itself ------------------------------------------------------
# Every case above ran against the one tree and put back what it changed. A case
# that passed only because an earlier one left the tree dirty needs a change
# nobody recorded. The helpers refuse such a change when a later case touches the
# same path. Here the whole tree is compared with the baseline, every staged path
# and nothing more, which catches one that no later case touched. So a dirty tree
# at ANY case ends the run red, and the verdicts in the middle are never taken on
# trust. Then the restored tree must still be accepted.
foreach(relative IN ITEMS "scripts/tsan-gate.sh" "CMakeLists.txt")
    FastCachedRequireBaseline("${relative}")
    list(APPEND expectedPaths "${relative}")
endforeach()
foreach(row IN LISTS FastCachedTsanScope)
    FastCachedStagedPath("${row}" staged)
    FastCachedRequireBaseline("${staged}")
    list(APPEND expectedPaths "${staged}")
endforeach()
file(GLOB_RECURSE presentPaths LIST_DIRECTORIES false RELATIVE "${tree}" "${tree}/*")
list(SORT presentPaths)
list(SORT expectedPaths)
if(NOT presentPaths STREQUAL expectedPaths)
    message(FATAL_ERROR
        "check-tsan-scope-selftest: after the last case the tree holds [${presentPaths}], "
        "and the baseline stages [${expectedPaths}]. A case created or removed a file "
        "without FastCachedWrite, FastCachedAppend or FastCachedRemove, so the cases "
        "after it were judged on a tree they did not set up.")
endif()
FastCachedSelftestCase("the-shared-tree-accepts-again-after-every-case" pass "case.s. in")

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
