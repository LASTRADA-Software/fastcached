# SPDX-License-Identifier: Apache-2.0
#
# Drives `fastcached_tracked_files` in `scripts/lib/CheckCommon.cmake` over staged trees.
#
# ## Why this file exists at all
#
# Before #1485 the enumeration was eight byte-identical copies, which is eight chances for one
# of them to enumerate a different set than the rule it enforces claims to cover. Consolidating
# them removes those eight chances and **adds one**: every enumerating check now reads its file
# set through this one function, so a defect here is a defect in all of them at once, and it
# would be silent in the direction that matters -- an over-broad enumeration reports on files a
# rule was never written for, an under-broad one reports clean over files it never read. A
# consolidation whose single point of failure has no guard on it has moved the risk rather than
# removed it. #1485's acceptance says so in as many words.
#
# ## What is asserted, and why it is these things
#
# **The MODE is part of the answer and every case asserts it.** There are three, they cover
# different file sets, and CI takes the git one: a guard whose self-test exercised the walk while
# CI exercised git has already shipped in this tree and passed, because the mode under test was
# not the mode in use.
#
# **The third mode is the one the copies could not say.** `git ls-files` succeeding and naming
# NOTHING is a different event from there being no index, and every copy reported the latter for
# both -- a true-sounding sentence about the environment that is false. A fresh `git init` with
# nothing staged produces it, which is the shape a staged tree naturally has, so this is not a
# hypothetical state: it is the state most self-tests in this repository are actually in.
#
# **That a FILTER applies in BOTH modes is asserted as an EQUALITY between them**, not as two
# separate counts that happen to look right. A filter living in the pathspec on one side and in
# the glob on the other passes every per-mode assertion and still covers two different sets.
#
# ## In-process for answers, a subprocess for refusals
#
# The answer cases call the function directly: the decision is a pure computation over a
# directory, so there is nothing to gain from an interpreter in between and a count is easier to
# read than a wrapped diagnostic. The refusal cases cannot be in-process -- `message(FATAL_ERROR)`
# would end this script rather than be observed -- so they run a generated driver under
# `cmake -P` and the verdict is read from its OUTPUT, flattened first, because CMake wraps
# diagnostics at a column that depends on the scratch path's length.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FASTCACHED_SOURCE_DIR OR FASTCACHED_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR OR FASTCACHED_SCRATCH_DIR STREQUAL "")
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(library "${FASTCACHED_SOURCE_DIR}/scripts/lib/CheckCommon.cmake")
if(NOT EXISTS "${library}")
    message(FATAL_ERROR "the library under test is missing: ${library}")
endif()
include("${library}")

if(NOT COMMAND fastcached_tracked_files)
    message(FATAL_ERROR
        "fastcached_tracked_files is not defined by ${library}. Refused rather than reported "
        "as zero failures -- a self-test that could not reach its subject is not one that "
        "judged it.")
endif()

if(NOT GIT_EXECUTABLE)
    find_package(Git QUIET)
endif()
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR
        "git is needed: two of the three modes are git's, and a run that silently exercised "
        "only the walk would be the defect this file exists to catch.")
endif()

set(caseCount 0)
set(failureCount 0)

# ---------------------------------------------------------------------------
# Staging.

## Plant a tree of files. Rows are `relative/path|content`.
## @param tree The directory to build, wiped first.
## @param ARGN The rows.
function(StageTree tree)
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}")
    foreach(row IN LISTS ARGN)
        string(FIND "${row}" "|" bar)
        if(bar EQUAL -1)
            message(FATAL_ERROR "staged row has no `|`: ${row}")
        endif()
        string(SUBSTRING "${row}" 0 ${bar} path)
        math(EXPR after "${bar} + 1")
        string(SUBSTRING "${row}" ${after} -1 body)
        get_filename_component(parent "${tree}/${path}" DIRECTORY)
        file(MAKE_DIRECTORY "${parent}")
        file(WRITE "${tree}/${path}" "${body}\n")
    endforeach()
endfunction()

## Make a staged tree a git repository.
## @param tree The directory.
## @param stage TRUE to `git add -A`; FALSE leaves an index that names nothing.
function(MakeRepository tree stage)
    execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}" OUTPUT_QUIET ERROR_QUIET)
    if(stage)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A
                        OUTPUT_QUIET ERROR_QUIET)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Verdicts. A failing case prints and is counted; it does not end the run, or the cases after
# the first failure would be reported as neither passed nor failed.

## Record one expectation.
## @param name The case name.
## @param what What was being asserted, for the diagnostic.
## @param actual The observed value.
## @param expected The required value.
function(Expect name what actual expected)
    math(EXPR caseCount "${caseCount} + 1")
    set(caseCount "${caseCount}" PARENT_SCOPE)
    if(actual STREQUAL expected)
        message(STATUS "  ${name} / ${what}: ok")
    else()
        message(STATUS "  ${name} / ${what}: FAILED")
        message(STATUS "      expected: ${expected}")
        message(STATUS "      actual:   ${actual}")
        math(EXPR failureCount "${failureCount} + 1")
        set(failureCount "${failureCount}" PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# The corpus every mode case is asked about. One `.cpp` and one `.hpp` under `src/`, one `.cpp`
# outside it, one file that is not C++ at all, and one matching file under each excluded
# directory name -- so a walk that forgot the exclusion list answers a different count rather
# than the same one.
# A `;` in a row would split the CMake list, so the bodies carry none -- none of these files
# is ever parsed as C++, only enumerated.
set(corpus
    "src/Alpha.cpp|int alpha()"
    "src/Alpha.hpp|int alpha()"
    "tools/Beta.cpp|int beta()"
    "docs/notes.md|not C++"
    "out/build/Stale.cpp|int stale()"
    "_deps/vendored/Dep.cpp|int dep()"
    ".cache/Cached.cpp|int cached()"
    ".claude/Agent.cpp|int agent()"
)

set(cxxFilter "\\.(cpp|hpp)$")

# ---------------------------------------------------------------------------
# `fastcached_work_tree_state`: all THREE answers, each arranged deterministically.
#
# Every one of them has to be REACHABLE by a fixture or the third is a branch nobody has
# watched, and two of the three are easy to arrange only by accident. `not-a-work-tree` is the
# awkward one -- a directory outside every repository is not something this fixture can
# guarantee, since the scratch directory may sit under `out/build/...` -- so it is arranged by
# asking about a repository's own `.git` DIRECTORY, which git answers `false` for while
# succeeding. That is the state's own meaning, not a trick: inside the repository, outside the
# work tree.
set(probeTree "${FASTCACHED_SCRATCH_DIR}/workTreeStateProbe")
StageTree("${probeTree}" "src/Alpha.cpp|int alpha()")
MakeRepository("${probeTree}" TRUE)

fastcached_work_tree_state("${probeTree}" probeState)
Expect("workTreeState" "a checkout" "${probeState}" "work-tree")

fastcached_work_tree_state("${probeTree}/.git" probeState)
Expect("workTreeState" "inside the repository, outside the work tree"
    "${probeState}" "not-a-work-tree")

set(realGit "${GIT_EXECUTABLE}")
set(GIT_EXECUTABLE "${probeTree}/no-such-git-binary")
fastcached_work_tree_state("${probeTree}" probeState)
set(GIT_EXECUTABLE "${realGit}")
# A git that cannot be RUN is `no-git` and not `not-a-work-tree`: a missing binary and a broken
# one are one fact for every caller, and neither says anything about the directory.
Expect("workTreeState" "git cannot answer" "${probeState}" "no-git")

# --- 1. git, every tracked file, filtered ----------------------------------------------------
set(tree "${FASTCACHED_SCRATCH_DIR}/gitAllTracked")
StageTree("${tree}" ${corpus})
MakeRepository("${tree}" TRUE)
fastcached_tracked_files("${tree}"
    GLOBS "*" FILTER "${cxxFilter}" FILES_OUT files MODE_OUT mode)
Expect("gitAllTracked" "mode" "${mode}" "git ls-files")
# The excluded directory names are NOT excluded on the git path, deliberately: git tracks what
# the repository contains, and a tracked `out/` is a real file somebody committed. The
# exclusion list exists for the walk, where an untracked build tree is in the way.
Expect("gitAllTracked" "files" "${files}"
    ".cache/Cached.cpp;.claude/Agent.cpp;_deps/vendored/Dep.cpp;out/build/Stale.cpp;src/Alpha.cpp;src/Alpha.hpp;tools/Beta.cpp")

# --- 2. git, narrowed by a pathspec ----------------------------------------------------------
fastcached_tracked_files("${tree}"
    PATHSPECS "src/*.cpp" "src/*.hpp"
    GLOBS "src/*.cpp" "src/*.hpp" FILES_OUT files MODE_OUT mode)
Expect("gitPathspec" "mode" "${mode}" "git ls-files")
Expect("gitPathspec" "files" "${files}" "src/Alpha.cpp;src/Alpha.hpp")

# --- 3. git cannot answer at all ------------------------------------------------------------
# The same corpus, with the git probe made unable to answer. The walk's exclusion list is what
# must remove the four build-tree copies, and the `docs/notes.md` row is what the FILTER must
# remove.
#
# **The probe is disabled rather than the repository merely being absent, because "absent" is
# not something this fixture can arrange.** A staged tree with no `.git` of its own still sits
# INSIDE a work tree whenever the scratch directory does -- and it does: every ctest
# registration here puts it under `CMAKE_CURRENT_BINARY_DIR`, which is `out/build/...` in the
# repository. `git -C <tree> rev-parse --is-inside-work-tree` then answers **true**, `ls-files`
# names nothing under that directory, and the honest mode is the THIRD one. Measured: this
# assertion passed with the scratch directory in `TEMP` and failed with it in `out/`, which is
# a fixture whose verdict is about where somebody put their build tree. Pointing
# `GIT_EXECUTABLE` at a path that cannot execute makes the branch deterministic and is the
# branch a source tarball actually takes -- git absent and git unable to answer are one arm of
# the function, by design.
set(tree "${FASTCACHED_SCRATCH_DIR}/walkGitCannotAnswer")
StageTree("${tree}" ${corpus})
set(realGit "${GIT_EXECUTABLE}")
set(GIT_EXECUTABLE "${tree}/no-such-git-binary")
fastcached_tracked_files("${tree}"
    GLOBS "*" FILTER "${cxxFilter}" FILES_OUT walkFiles MODE_OUT mode)
set(GIT_EXECUTABLE "${realGit}")
Expect("walkGitCannotAnswer" "mode" "${mode}" "directory walk (no git index)")
Expect("walkGitCannotAnswer" "files" "${walkFiles}" "src/Alpha.cpp;src/Alpha.hpp;tools/Beta.cpp")

# --- 4. a git index that names nothing -- the third mode -------------------------------------
# `git init` with nothing staged. The copies this function replaced answered
# `directory walk (no git index)` here, in a tree that HAS one, and a case asserting the mode
# would have believed it.
set(tree "${FASTCACHED_SCRATCH_DIR}/gitIndexNamesNothing")
StageTree("${tree}" ${corpus})
MakeRepository("${tree}" FALSE)
fastcached_tracked_files("${tree}"
    GLOBS "*" FILTER "${cxxFilter}" FILES_OUT files MODE_OUT mode)
Expect("gitIndexNamesNothing" "mode" "${mode}"
    "directory walk (git index names no matching file)")
Expect("gitIndexNamesNothing" "files" "${files}" "src/Alpha.cpp;src/Alpha.hpp;tools/Beta.cpp")

# --- 5. a pathspec the index cannot satisfy is the same third mode --------------------------
# Staged, so the index is not empty -- but nothing it names matches the question. That is still
# "the index answered and named no matching file", and it must not read as "there is no index".
set(tree "${FASTCACHED_SCRATCH_DIR}/gitPathspecMatchesNothing")
StageTree("${tree}" ${corpus})
MakeRepository("${tree}" TRUE)
fastcached_tracked_files("${tree}"
    PATHSPECS "nowhere/*.cpp"
    GLOBS "src/*.cpp" FILES_OUT files MODE_OUT mode)
Expect("gitPathspecMatchesNothing" "mode" "${mode}"
    "directory walk (git index names no matching file)")
Expect("gitPathspecMatchesNothing" "files" "${files}" "src/Alpha.cpp")

# --- 6. the filter covers both modes, asserted as an equality -------------------------------
# Case 1 and case 3 asked the same question of the same corpus through different modes. Their
# answers differ only by the build-tree copies, which git legitimately tracks and the walk
# legitimately skips -- so the comparison is of the part both modes are about, and a filter
# that applied to one side only would break it.
set(gitSrcSubset "src/Alpha.cpp;src/Alpha.hpp;tools/Beta.cpp")
fastcached_tracked_files("${FASTCACHED_SCRATCH_DIR}/gitAllTracked"
    PATHSPECS "src/*" "tools/*"
    GLOBS "src/*" "tools/*" FILTER "${cxxFilter}" FILES_OUT gitFiles MODE_OUT gitMode)
Expect("filterInBothModes" "git mode" "${gitMode}" "git ls-files")
Expect("filterInBothModes" "git answer equals the walk's" "${gitFiles}" "${gitSrcSubset}")
Expect("filterInBothModes" "walk answer equals the git one" "${walkFiles}" "${gitSrcSubset}")

# --- 7. no FILTER keeps everything the question named ---------------------------------------
# Including `docs/notes.md`, which is how a caller asks about files that are not C++ --
# `check-catch-skip-return-code.cmake` asks for `*CMakeLists.txt` this way.
set(tree "${FASTCACHED_SCRATCH_DIR}/noFilter")
StageTree("${tree}" ${corpus})
MakeRepository("${tree}" TRUE)
fastcached_tracked_files("${tree}"
    PATHSPECS "docs/*"
    GLOBS "docs/*" FILES_OUT files MODE_OUT mode)
Expect("noFilter" "mode" "${mode}" "git ls-files")
Expect("noFilter" "files" "${files}" "docs/notes.md")

# --- 8. the single-backslash pattern really is over-broad -----------------------------------
# #1485's origin: the extension regex went in as `"\.(cpp|...)$"`, CMake unescaped the argument
# before the regex engine saw it, the `\.` arrived as a dot matching ANY character, and the
# enumeration came back 79 shell scripts too many -- every downstream verdict still green,
# because a `.sh` file contains no C++. This case pins the FACT that makes the doubled
# backslash load-bearing, so the next author cannot "simplify" it back believing the two
# spellings equivalent. It asserts the over-broad pattern MATCHES the shell script; the
# corrected one, exercised by every case above, does not.
set(tree "${FASTCACHED_SCRATCH_DIR}/overBroadPattern")
StageTree("${tree}"
    "src/Alpha.cpp|int alpha()"
    "scripts/check-apt-update.sh|#!/bin/sh")
MakeRepository("${tree}" TRUE)
# The alternation is the REAL one, `h` included. That bare `h` is what does the damage: with
# the dot matching any character, `.sh` is `s` followed by the `h` alternative. A truncated
# alternation without it does NOT reproduce the defect -- measured, by writing this case that
# way first and watching it fail -- so the case would have read as a refutation of the very
# claim it was written to pin.
set(realAlternation "(cpp|cc|cxx|hpp|hh|hxx|h|ixx|cppm)$")
fastcached_tracked_files("${tree}"
    GLOBS "*" FILTER "\.${realAlternation}" FILES_OUT overBroad MODE_OUT mode)
Expect("overBroadPattern" "the single-backslash pattern swallows a .sh file"
    "${overBroad}" "scripts/check-apt-update.sh;src/Alpha.cpp")
fastcached_tracked_files("${tree}"
    GLOBS "*" FILTER "\\.${realAlternation}" FILES_OUT corrected MODE_OUT mode)
Expect("overBroadPattern" "the doubled-backslash pattern does not"
    "${corrected}" "src/Alpha.cpp")

# ---------------------------------------------------------------------------
# Refusals. A generated driver under `cmake -P`, because a FATAL_ERROR observed in-process
# would end this script instead of being judged.

set(driver "${FASTCACHED_SCRATCH_DIR}/refusal-driver.cmake")

## Run one malformed call and require a phrase in its output.
## @param name The case name.
## @param phrase The phrase the flattened output must contain.
## @param arguments The argument text after the source directory.
function(ExpectRefusal name phrase arguments)
    math(EXPR caseCount "${caseCount} + 1")
    set(caseCount "${caseCount}" PARENT_SCOPE)
    file(WRITE "${driver}"
        "cmake_minimum_required(VERSION 3.20)\n"
        "include(\"${library}\")\n"
        "fastcached_tracked_files(\"${FASTCACHED_SCRATCH_DIR}/gitAllTracked\" ${arguments})\n"
        "message(STATUS \"the call returned, which it must not have\")\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${driver}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE status)
    # CMake wraps its diagnostics, so a phrase can be present in the output and in no line of
    # it. Flatten before matching.
    string(REPLACE "\n" " " flat "${out}${err}")
    string(REGEX REPLACE "[ \t]+" " " flat "${flat}")
    if(status EQUAL 0)
        message(STATUS "  ${name}: FAILED -- the malformed call was accepted")
        math(EXPR failureCount "${failureCount} + 1")
        set(failureCount "${failureCount}" PARENT_SCOPE)
    elseif(NOT flat MATCHES "${phrase}")
        message(STATUS "  ${name}: FAILED -- refused, but not for the stated reason")
        message(STATUS "      wanted: ${phrase}")
        message(STATUS "      got:    ${flat}")
        math(EXPR failureCount "${failureCount} + 1")
        set(failureCount "${failureCount}" PARENT_SCOPE)
    else()
        message(STATUS "  ${name}: ok")
    endif()
endfunction()

ExpectRefusal("missingGlobs" "GLOBS is required"
    "FILTER \"${cxxFilter}\" FILES_OUT files MODE_OUT mode")
ExpectRefusal("missingFilesOut" "FILES_OUT is required"
    "GLOBS \"*\" MODE_OUT mode")
ExpectRefusal("missingModeOut" "MODE_OUT is required"
    "GLOBS \"*\" FILES_OUT files")
ExpectRefusal("unknownKeyword" "unrecognised argument"
    "GLOBS \"*\" FILES_OUT files MODE_OUT mode PATHSPEC \"src/*\"")

# ---------------------------------------------------------------------------
# The count is printed whatever the verdict: a self-test that stopped early must not look like
# one that judged something.

if(failureCount GREATER 0)
    message(FATAL_ERROR
        "tracked-files-selftest: ${failureCount} of ${caseCount} assertion(s) failed")
endif()
message(STATUS "tracked-files-selftest: ${caseCount} assertion(s) ran, all as expected")
