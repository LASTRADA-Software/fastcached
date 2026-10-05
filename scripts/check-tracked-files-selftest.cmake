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
    fastcached_scratch_git("${GIT_EXECUTABLE}" scratchGit)
    execute_process(COMMAND ${scratchGit} init -q "${tree}" OUTPUT_QUIET ERROR_QUIET)
    if(stage)
        execute_process(COMMAND ${scratchGit} -C "${tree}" add -A
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

# --- 9. CONTAINING: the files holding a needle, in git mode ---------------------------------
# The corpus bodies are the needles: `alpha` is in both src files, `beta` in tools/, and
# `not C++` in docs/notes.md -- which the C++ FILTER must keep out of the answer even though git
# finds it, because CONTAINING_OUT is a subset of FILES_OUT and never a second question.
fastcached_tracked_files("${FASTCACHED_SCRATCH_DIR}/gitAllTracked"
    PATHSPECS "src/*" "tools/*" "docs/*"
    GLOBS "src/*" "tools/*" "docs/*" FILTER "${cxxFilter}"
    CONTAINING "alpha" "not C++" CONTAINING_OUT gitHolding
    FILES_OUT gitAll MODE_OUT gitMode)
Expect("containingGit" "mode" "${gitMode}" "git ls-files")
Expect("containingGit" "files are the whole set" "${gitAll}" "${gitSrcSubset}")
Expect("containingGit" "holding a needle" "${gitHolding}" "src/Alpha.cpp;src/Alpha.hpp")

# --- 10. CONTAINING in the walk, and the SAME answer as git ---------------------------------
# The walk reads each file; git searches its index's files. Two mechanisms, one question, so the
# case is an equality with case 9 rather than a second literal.
set(realGit "${GIT_EXECUTABLE}")
set(GIT_EXECUTABLE "${FASTCACHED_SCRATCH_DIR}/no-such-git-binary")
fastcached_tracked_files("${FASTCACHED_SCRATCH_DIR}/walkGitCannotAnswer"
    GLOBS "src/*" "tools/*" "docs/*" FILTER "${cxxFilter}"
    CONTAINING "alpha" "not C++" CONTAINING_OUT walkHolding
    FILES_OUT walkAll MODE_OUT walkMode)
set(GIT_EXECUTABLE "${realGit}")
Expect("containingWalk" "mode" "${walkMode}" "directory walk (no git index)")
Expect("containingWalk" "holding a needle equals git's" "${walkHolding}" "${gitHolding}")

# --- 11. a needle nobody holds is an EMPTY answer, and the set is still whole ---------------
fastcached_tracked_files("${FASTCACHED_SCRATCH_DIR}/gitAllTracked"
    PATHSPECS "src/*" "tools/*"
    GLOBS "src/*" "tools/*" FILTER "${cxxFilter}"
    CONTAINING "no file says this" CONTAINING_OUT noneHolding
    FILES_OUT noneAll MODE_OUT noneMode)
Expect("containingNothing" "holding a needle" "${noneHolding}" "")
Expect("containingNothing" "files are the whole set" "${noneAll}" "${gitSrcSubset}")

# --- 12. any ONE needle is enough -------------------------------------------------------------
fastcached_tracked_files("${FASTCACHED_SCRATCH_DIR}/gitAllTracked"
    PATHSPECS "src/*" "tools/*"
    GLOBS "src/*" "tools/*" FILTER "${cxxFilter}"
    CONTAINING "no file says this" "beta" CONTAINING_OUT oneHolding
    FILES_OUT oneAll MODE_OUT oneMode)
Expect("containingAnyNeedle" "holding a needle" "${oneHolding}" "tools/Beta.cpp")

# --- 13. CONTAINING_BYTES: which files hold a BYTE, judged on the true bytes ----------------
# The bytes a text read loses -- a CR, a 0x1A, a NUL and whatever sits behind one -- are the
# point, so each is planted. Written through `cmake -E echo`, which writes its argument's bytes
# and one LF, because `file(WRITE)` turns `\n` into CRLF on Windows and would plant a CR in
# every file. A NUL cannot be spelled at all, so it arrives in a tar header, which pads its
# fields with them; the second archive's member carries a BEL, BEHIND those NULs. Not named
# `nul.*`: that is a reserved device name on Windows, extension or not, and git cannot open it.
string(ASCII 7 bel)
string(ASCII 13 cr)
string(ASCII 26 sub)
set(bytesTree "${FASTCACHED_SCRATCH_DIR}/bytes")
file(REMOVE_RECURSE "${bytesTree}")
file(MAKE_DIRECTORY "${bytesTree}")
## Write `body` and one LF to `path` as bytes.
function(WriteBytes path body)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${body}" OUTPUT_FILE "${path}")
endfunction()
WriteBytes("${bytesTree}/clean.txt" "plain text")
WriteBytes("${bytesTree}/bel.txt" "ring ${bel} bell")
WriteBytes("${bytesTree}/cr.txt" "dos line${cr}")
WriteBytes("${bytesTree}/sub.txt" "stop${sub}here")
# `0{` is 30 7b: the raw hex holds `07` across the byte boundary, and the file holds no 0x07.
WriteBytes("${bytesTree}/straddle.txt" "x0{y")
foreach(archive IN ITEMS "zeros|plain" "behindzeros|ring ${bel} bell")
    string(REPLACE "|" ";" archive "${archive}")
    list(GET archive 0 archiveName)
    list(GET archive 1 memberBody)
    set(member "${FASTCACHED_SCRATCH_DIR}/member-${archiveName}")
    WriteBytes("${member}" "${memberBody}")
    file(ARCHIVE_CREATE OUTPUT "${bytesTree}/${archiveName}.tar" PATHS "${member}" FORMAT gnutar)
    file(REMOVE "${member}")
endforeach()
MakeRepository("${bytesTree}" TRUE)

## Ask the bytes tree one byte question.
## @param outVar Name of the variable to receive CONTAINING_OUT.
## @param outMode Name of the variable to receive MODE_OUT.
## @param ARGN The tokens.
function(AskBytes outVar outMode)
    fastcached_tracked_files("${bytesTree}" GLOBS "*" FILES_OUT all MODE_OUT mode
        CONTAINING_BYTES ${ARGN} CONTAINING_OUT holding)
    set(${outVar} "${holding}" PARENT_SCOPE)
    set(${outMode} "${mode}" PARENT_SCOPE)
endfunction()

AskBytes(gitBel gitBytesMode 07)
Expect("bytesGit" "mode" "${gitBytesMode}" "git ls-files")
Expect("bytesGit" "a BEL, one behind NULs included, and not one straddling a byte boundary"
    "${gitBel}" "behindzeros.tar;bel.txt")
AskBytes(gitNul gitBytesMode 00)
Expect("bytesGit" "a NUL" "${gitNul}" "behindzeros.tar;zeros.tar")
AskBytes(gitLost gitBytesMode 0d 1a)
Expect("bytesGit" "a CR and a 0x1A, which a text read loses" "${gitLost}" "cr.txt;sub.txt")
AskBytes(gitNone gitBytesMode 7f)
Expect("bytesGit" "a byte nobody holds" "${gitNone}" "")

# --- 14. the walk reads HEX, and gives git's answers -----------------------------------------
set(realGit "${GIT_EXECUTABLE}")
set(GIT_EXECUTABLE "${FASTCACHED_SCRATCH_DIR}/no-such-git-binary")
AskBytes(walkBel walkBytesMode 07)
AskBytes(walkNul walkBytesMode 00)
AskBytes(walkLost walkBytesMode 0d 1a)
AskBytes(walkNone walkBytesMode 7f)
set(GIT_EXECUTABLE "${realGit}")
Expect("bytesWalk" "mode" "${walkBytesMode}" "directory walk (no git index)")
Expect("bytesWalk" "a BEL equals git's" "${walkBel}" "${gitBel}")
Expect("bytesWalk" "a NUL equals git's" "${walkNul}" "${gitNul}")
Expect("bytesWalk" "a CR and a 0x1A equal git's" "${walkLost}" "${gitLost}")
Expect("bytesWalk" "a byte nobody holds equals git's" "${walkNone}" "${gitNone}")

# --- 15. a git whose grep cannot run is ANSWERED, by reading -----------------------------------
# A git built without PCRE refuses `-P`; `grep.threads=-1` makes this one refuse `grep` the same
# way while `ls-files` still answers, so the mode stays git's and only the search fails. The
# answer must be the one a working search gives -- a failed search is not one that found nothing.
fastcached_scratch_git("${GIT_EXECUTABLE}" scratchGit)
execute_process(COMMAND ${scratchGit} -C "${bytesTree}" config grep.threads -1)
AskBytes(fallbackBel fallbackMode 07)
AskBytes(fallbackNul fallbackMode 00)
execute_process(COMMAND ${scratchGit} -C "${bytesTree}" config --unset grep.threads)
Expect("bytesGrepFails" "mode" "${fallbackMode}" "git ls-files")
Expect("bytesGrepFails" "a BEL equals a working search's" "${fallbackBel}" "${gitBel}")
Expect("bytesGrepFails" "a NUL equals a working search's" "${fallbackNul}" "${gitNul}")

# --- 16. MISSING_OUT: what the index names and the tree does not have ----------------------
# git mode lists the files it CAN open (`grep -L` with a pattern that never matches), so a file
# deleted after `git add` is what it did not list -- and an empty file, which has no line for
# any pattern to reach, must still count as present.
set(missingTree "${FASTCACHED_SCRATCH_DIR}/missing")
file(REMOVE_RECURSE "${missingTree}")
file(MAKE_DIRECTORY "${missingTree}")
WriteBytes("${missingTree}/kept.txt" "ring ${bel} bell")
WriteBytes("${missingTree}/gone.txt" "ring ${bel} bell")
file(TOUCH "${missingTree}/empty.txt")

# A symlink, where this host can make one git records as a link. git searches regular files
# only: it neither reads what a link points at nor lists it with `-L`, so without the seam
# setting links apart a file reached through one would hold nothing and exist nowhere.
set(linksReachable FALSE)
file(CREATE_LINK "kept.txt" "${missingTree}/link-to-kept" RESULT linkResult SYMBOLIC)
file(CREATE_LINK "nowhere.txt" "${missingTree}/dangling" RESULT danglingResult SYMBOLIC)
MakeRepository("${missingTree}" TRUE)
file(REMOVE "${missingTree}/gone.txt")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${missingTree}" ls-files -s -- ./link-to-kept
                OUTPUT_VARIABLE missingStaged)
if(linkResult STREQUAL "0" AND danglingResult STREQUAL "0" AND missingStaged MATCHES "120000 [^\t]*\tlink-to-kept")
    set(linksReachable TRUE)
endif()

## Ask the missing tree which files it lacks and which hold a BEL byte -- the two questions a
## failing search falls back on.
## @param outMissing Name of the variable to receive MISSING_OUT.
## @param outBytes Name of the variable to receive CONTAINING_OUT for byte 07.
## @param outMode Name of the variable to receive MODE_OUT.
function(AskMissing outMissing outBytes outMode)
    fastcached_tracked_files("${missingTree}" GLOBS "*" FILES_OUT all MODE_OUT mode
        MISSING_OUT missing CONTAINING_BYTES 07 CONTAINING_OUT byteHolding)
    set(${outMissing} "${missing}" PARENT_SCOPE)
    set(${outBytes} "${byteHolding}" PARENT_SCOPE)
    set(${outMode} "${mode}" PARENT_SCOPE)
endfunction()

AskMissing(gitMissing gitLinkBytes gitMissingMode)
# A literal is asked on its own, and WITH MISSING_OUT: without it the deleted file and the link
# to nothing are refused by name (case 17), which is a different question from this one.
fastcached_tracked_files("${missingTree}" GLOBS "*" FILES_OUT unused MODE_OUT unusedMode
    CONTAINING "ring" CONTAINING_OUT gitLinkText MISSING_OUT gitTextMissing)
Expect("missingGit" "a literal question reports the same missing files" "${gitTextMissing}" "${gitMissing}")
Expect("missingGit" "mode" "${gitMissingMode}" "git ls-files")
if(linksReachable)
    Expect("missingGit" "a deleted file and a link to nothing, and not an empty file"
        "${gitMissing}" "dangling;gone.txt")
    Expect("missingGit" "a link's target is searched for a byte" "${gitLinkBytes}" "kept.txt;link-to-kept")
    Expect("missingGit" "a link's target is searched for a literal" "${gitLinkText}" "kept.txt;link-to-kept")
else()
    Expect("missingGit" "a deleted file, and not an empty file" "${gitMissing}" "gone.txt")
    Expect("missingGit" "the byte" "${gitLinkBytes}" "kept.txt")
    message(STATUS "  missingGit: the symlink cases did NOT run -- this host made no link git "
                   "records as one (links ${linkResult}, ${danglingResult}), so they are asserted "
                   "only where it can, which the Linux legs are")
endif()

# The same questions with git's search failing: every file is asked one at a time, and the
# answers must be the ones git gave.
execute_process(COMMAND ${scratchGit} -C "${missingTree}" config grep.threads -1)
AskMissing(fallbackMissing fallbackLinkBytes fallbackMissingMode)
# The TEXT search fails the same way and is answered the same way -- by reading -- where it used
# to be refused outright: any stderr makes a search untrusted, a failure included.
fastcached_tracked_files("${missingTree}" GLOBS "*" FILES_OUT unused MODE_OUT fallbackTextMode
    CONTAINING "ring" CONTAINING_OUT fallbackLinkText MISSING_OUT fallbackTextMissing)
execute_process(COMMAND ${scratchGit} -C "${missingTree}" config --unset grep.threads)
Expect("missingGrepFails" "mode" "${fallbackMissingMode}" "git ls-files")
Expect("missingGrepFails" "missing equals a working search's" "${fallbackMissing}" "${gitMissing}")
Expect("missingGrepFails" "a byte equals a working search's" "${fallbackLinkBytes}" "${gitLinkBytes}")
Expect("textGrepFails" "mode" "${fallbackTextMode}" "git ls-files")
Expect("textGrepFails" "a literal equals a working search's" "${fallbackLinkText}" "${gitLinkText}")
Expect("textGrepFails" "missing equals a working search's" "${fallbackTextMissing}" "${gitMissing}")

# In a walk every file was FOUND on disk, so only a link to nothing is missing.
set(realGit "${GIT_EXECUTABLE}")
set(GIT_EXECUTABLE "${FASTCACHED_SCRATCH_DIR}/no-such-git-binary")
AskMissing(walkMissing walkLinkBytes walkMissingMode)
set(GIT_EXECUTABLE "${realGit}")
Expect("missingWalk" "mode" "${walkMissingMode}" "directory walk (no git index)")
if(linksReachable)
    Expect("missingWalk" "a link to nothing" "${walkMissing}" "dangling")
    Expect("missingWalk" "a link's target is searched" "${walkLinkBytes}" "kept.txt;link-to-kept")
else()
    Expect("missingWalk" "nothing" "${walkMissing}" "")
endif()

# ---------------------------------------------------------------------------
# Refusals. A generated driver under `cmake -P`, because a FATAL_ERROR observed in-process
# would end this script instead of being judged.

set(driver "${FASTCACHED_SCRATCH_DIR}/refusal-driver.cmake")

## Run one call that must be refused, over one tree, and require a phrase in its output.
## @param name The case name.
## @param phrase The phrase the flattened output must match.
## @param tree The source directory.
## @param git The git the call sees -- a path to nothing makes it a walk.
## @param arguments The argument text after the source directory.
function(ExpectRefusalIn name phrase tree git arguments)
    math(EXPR caseCount "${caseCount} + 1")
    set(caseCount "${caseCount}" PARENT_SCOPE)
    file(WRITE "${driver}"
        "cmake_minimum_required(VERSION 3.20)\n"
        "set(GIT_EXECUTABLE \"${git}\")\n"
        "include(\"${library}\")\n"
        "fastcached_tracked_files(\"${tree}\" ${arguments})\n"
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

## A malformed call, over the tree every argument-grammar case uses.
## @param name The case name.
## @param phrase The phrase the flattened output must match.
## @param arguments The argument text after the source directory.
function(ExpectRefusal name phrase arguments)
    ExpectRefusalIn("${name}" "${phrase}" "${FASTCACHED_SCRATCH_DIR}/gitAllTracked" "${GIT_EXECUTABLE}"
        "${arguments}")
    set(caseCount "${caseCount}" PARENT_SCOPE)
    set(failureCount "${failureCount}" PARENT_SCOPE)
endfunction()

ExpectRefusal("missingGlobs" "GLOBS is required"
    "FILTER \"${cxxFilter}\" FILES_OUT files MODE_OUT mode")
ExpectRefusal("missingFilesOut" "FILES_OUT is required"
    "GLOBS \"*\" MODE_OUT mode")
ExpectRefusal("missingModeOut" "MODE_OUT is required"
    "GLOBS \"*\" FILES_OUT files")
ExpectRefusal("unknownKeyword" "unrecognised argument"
    "GLOBS \"*\" FILES_OUT files MODE_OUT mode PATHSPEC \"src/*\"")
ExpectRefusal("containingWithoutOut" "CONTAINING needs CONTAINING_OUT"
    "GLOBS \"*\" FILES_OUT files MODE_OUT mode CONTAINING alpha")
ExpectRefusal("containingOutWithoutNeedles" "CONTAINING_OUT names no CONTAINING"
    "GLOBS \"*\" FILES_OUT files MODE_OUT mode CONTAINING_OUT holding")
ExpectRefusal("bytesWithoutOut" "CONTAINING needs CONTAINING_OUT"
    "GLOBS \"*\" FILES_OUT files MODE_OUT mode CONTAINING_BYTES 07")
# An uppercase digit never matches the lowercase hex a read produces, and a byte above 7f is a
# different number to PCRE in a UTF-8 locale: both would be needles that find nothing.
ExpectRefusal("bytesUppercase" "two lowercase hex digits from 00 to 7f"
    "GLOBS \"*\" FILES_OUT files MODE_OUT mode CONTAINING_BYTES 0D CONTAINING_OUT holding")
ExpectRefusal("bytesAboveAscii" "two lowercase hex digits from 00 to 7f"
    "GLOBS \"*\" FILES_OUT files MODE_OUT mode CONTAINING_BYTES 80 CONTAINING_OUT holding")

# --- 17. every file is ACCOUNTED FOR: judged, missing, or refused by name -------------------
# `git grep` exits 0 over a file it cannot open, saying so on stderr only, and skips a deleted
# file and a symlink without a word -- so each of those was left out of an answer, which is a
# file PASSED. The seam's defect (#1485's consolidation) was exactly that: `::htonl(` planted in a
# chmod-000 `Endian.hpp` failed the per-file check it replaced and passed the seam. That plant is
# case 17a, as it was found.
#
# One tree per state, so a refusal names the state that caused it and no other.
set(missingGit "${FASTCACHED_SCRATCH_DIR}/no-such-git-binary")
if(CMAKE_HOST_WIN32)
    set(unreadableHow "a deny-read access list")
else()
    set(unreadableHow "chmod 000")
endif()

## Make a file unreadable by this process, the way this platform does it: `chmod 000` on POSIX,
## where it does not bite as root; a deny-read entry for Everyone on Windows, where chmod only
## sets the read-only attribute. Whether it BIT is asked by reading, never assumed -- and read
## with `file(READ)` in a child, the primitive the seam uses, because `cmake -E cat` exits 0 with
## no output over a file a Windows access list denies (measured), which would read as "readable".
## @param path The file.
## @param outBit Name of the variable set TRUE when the file can no longer be read.
function(MakeUnreadable path outBit)
    if(CMAKE_HOST_WIN32)
        execute_process(COMMAND icacls "${path}" /deny "*S-1-1-0:(RD)" OUTPUT_QUIET ERROR_QUIET)
    else()
        execute_process(COMMAND chmod 000 "${path}" OUTPUT_QUIET ERROR_QUIET)
    endif()
    set(probe "${FASTCACHED_SCRATCH_DIR}/read-probe.cmake")
    file(WRITE "${probe}" "file(READ \"${path}\" content)\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${probe}"
                    OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE readStatus)
    if(readStatus EQUAL 0)
        set(${outBit} FALSE PARENT_SCOPE)
    else()
        set(${outBit} TRUE PARENT_SCOPE)
    endif()
endfunction()

## Undo `MakeUnreadable`, so the scratch tree can be removed by whoever comes next.
## @param path The file.
function(MakeReadable path)
    if(NOT EXISTS "${path}" AND NOT IS_SYMLINK "${path}")
        file(GLOB listed "${path}")
        if(NOT listed)
            return()
        endif()
    endif()
    if(CMAKE_HOST_WIN32)
        execute_process(COMMAND icacls "${path}" /remove:d "*S-1-1-0" OUTPUT_QUIET ERROR_QUIET)
    else()
        execute_process(COMMAND chmod 644 "${path}" OUTPUT_QUIET ERROR_QUIET)
    endif()
endfunction()

# 17a. Unreadable: tracked, present, and this process cannot open it.
set(unreadableTree "${FASTCACHED_SCRATCH_DIR}/unreadable")
set(lockedFile "${unreadableTree}/src/FastCache/Core/Endian.hpp")
MakeReadable("${lockedFile}")
StageTree("${unreadableTree}"
    "src/FastCache/Core/Bytes.hpp|plain bytes"
    "src/FastCache/Core/Endian.hpp|return ::htonl(value)")
MakeRepository("${unreadableTree}" TRUE)
MakeUnreadable("${lockedFile}" unreadableBit)
if(unreadableBit)
    set(lockedRefusal "cannot judge a file it cannot read -- src/FastCache/Core/Endian.hpp in .* tracked, present, and not readable")
    ExpectRefusalIn("unreadableGitText" "${lockedRefusal}" "${unreadableTree}" "${GIT_EXECUTABLE}"
        "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode CONTAINING \"htonl(\" CONTAINING_OUT holding")
    ExpectRefusalIn("unreadableGitBytes" "${lockedRefusal}" "${unreadableTree}" "${GIT_EXECUTABLE}"
        "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode CONTAINING_BYTES 3a CONTAINING_OUT holding")
    # MISSING_OUT beside it changes nothing: an unreadable file is THERE, so it is never missing.
    ExpectRefusalIn("unreadableGitNotMissing" "${lockedRefusal}" "${unreadableTree}" "${GIT_EXECUTABLE}"
        "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode CONTAINING \"htonl(\" CONTAINING_OUT holding MISSING_OUT missing")
    # A walk refuses it too. On Windows `EXISTS` cannot see an access list, so there the refusal
    # is CMake's own failed `file(READ)` naming the path -- a refusal either way, never a pass.
    if(CMAKE_HOST_WIN32)
        set(walkLockedRefusal "failed to open for reading.*Endian.hpp")
    else()
        set(walkLockedRefusal "${lockedRefusal}")
    endif()
    ExpectRefusalIn("unreadableWalkText" "${walkLockedRefusal}" "${unreadableTree}" "${missingGit}"
        "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode CONTAINING \"htonl(\" CONTAINING_OUT holding")
    # A question about PRESENCE reads no contents, so it answers: present, not missing.
    fastcached_tracked_files("${unreadableTree}" GLOBS "src/*" FILES_OUT unused MODE_OUT unusedMode
        MISSING_OUT unreadableMissing)
    Expect("unreadablePresence" "an unreadable file is not missing, in git mode" "${unreadableMissing}" "")
    set(realGit "${GIT_EXECUTABLE}")
    set(GIT_EXECUTABLE "${missingGit}")
    fastcached_tracked_files("${unreadableTree}" GLOBS "src/*" FILES_OUT unused MODE_OUT unusedMode
        MISSING_OUT unreadableWalkMissing)
    set(GIT_EXECUTABLE "${realGit}")
    Expect("unreadablePresence" "an unreadable file is not missing, in a walk" "${unreadableWalkMissing}" "")
else()
    message(STATUS "  unreadable: the unreadable cases did NOT run -- ${unreadableHow} did not stop "
                   "this process reading the file (running as root, or a filesystem without "
                   "permissions), so they are asserted only where it does, which CI's legs are")
endif()
MakeReadable("${lockedFile}")

# 17b. Deleted from the work tree: `git grep` skips it and says nothing at all.
set(deletedTree "${FASTCACHED_SCRATCH_DIR}/deleted")
StageTree("${deletedTree}"
    "src/Kept.hpp|return ::htonl(value)"
    "src/Gone.hpp|return ::htonl(value)")
MakeRepository("${deletedTree}" TRUE)
file(REMOVE "${deletedTree}/src/Gone.hpp")
set(goneRefusal "cannot judge a file it cannot read -- src/Gone.hpp in .* named by the index and not in the work tree")
ExpectRefusalIn("deletedGitText" "${goneRefusal}" "${deletedTree}" "${GIT_EXECUTABLE}"
    "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode CONTAINING \"htonl(\" CONTAINING_OUT holding")
ExpectRefusalIn("deletedGitBytes" "${goneRefusal}" "${deletedTree}" "${GIT_EXECUTABLE}"
    "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode CONTAINING_BYTES 3a CONTAINING_OUT holding")
# Asked for, it is MISSING_OUT's answer, and the rest is still judged.
fastcached_tracked_files("${deletedTree}" GLOBS "src/*" FILES_OUT unused MODE_OUT unusedMode
    CONTAINING "htonl(" CONTAINING_OUT deletedHolding MISSING_OUT deletedMissing)
Expect("deletedAsked" "the deleted file is missing" "${deletedMissing}" "src/Gone.hpp")
Expect("deletedAsked" "and the file that is there is judged" "${deletedHolding}" "src/Kept.hpp")

# 17c. A link to nothing: git never searches a symlink, and reading one finds nothing.
if(linksReachable)
    set(danglingTree "${FASTCACHED_SCRATCH_DIR}/dangling")
    StageTree("${danglingTree}" "src/Kept.hpp|return ::htonl(value)")
    file(CREATE_LINK "Nowhere.hpp" "${danglingTree}/src/Dangling.hpp" SYMBOLIC)
    # A link to THAT link: its target is a directory entry that is listed, so a classifier that
    # follows one hop calls it unreadable. The end of the chain is nothing.
    file(CREATE_LINK "Dangling.hpp" "${danglingTree}/src/Chained.hpp" SYMBOLIC)
    MakeRepository("${danglingTree}" TRUE)
    set(danglingRefusal "cannot judge a file it cannot read -- src/Chained.hpp, src/Dangling.hpp in .* not in the work tree")
    ExpectRefusalIn("danglingGitText" "${danglingRefusal}" "${danglingTree}" "${GIT_EXECUTABLE}"
        "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode CONTAINING \"htonl(\" CONTAINING_OUT holding")
    ExpectRefusalIn("danglingWalkText" "${danglingRefusal}" "${danglingTree}" "${missingGit}"
        "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode CONTAINING \"htonl(\" CONTAINING_OUT holding")
    fastcached_tracked_files("${danglingTree}" GLOBS "src/*" FILES_OUT unused MODE_OUT unusedMode
        MISSING_OUT chainGitMissing)
    Expect("danglingChain" "a link to a link to nothing is missing, in git mode"
        "${chainGitMissing}" "src/Chained.hpp;src/Dangling.hpp")
    set(realGit "${GIT_EXECUTABLE}")
    set(GIT_EXECUTABLE "${missingGit}")
    fastcached_tracked_files("${danglingTree}" GLOBS "src/*" FILES_OUT unused MODE_OUT unusedMode
        MISSING_OUT chainWalkMissing)
    set(GIT_EXECUTABLE "${realGit}")
    Expect("danglingChain" "a link to a link to nothing is missing, in a walk"
        "${chainWalkMissing}" "src/Chained.hpp;src/Dangling.hpp")
else()
    message(STATUS "  dangling: the link-to-nothing cases did NOT run -- this host made no link git "
                   "records as one, so they are asserted only where it can, which the Linux legs are")
endif()

# 17e. A link CHAIN is followed to its end, and a cycle is nothing. Hop2 -> Hop1 -> an unreadable
# Target is THERE and unreadable, never missing -- a classifier that stops after one hop calls it
# missing; CycleA <-> CycleB opens as nothing (`ELOOP`) and is missing -- one that stops anywhere
# in the loop finds a listed link and calls it unreadable. A presence question, so neither is
# refused and both mistakes show as a wrong MISSING_OUT.
if(linksReachable)
    set(chainTree "${FASTCACHED_SCRATCH_DIR}/chains")
    MakeReadable("${chainTree}/src/Target.hpp")
    StageTree("${chainTree}" "src/Target.hpp|return ::htonl(value)")
    file(CREATE_LINK "Target.hpp" "${chainTree}/src/Hop1.hpp" SYMBOLIC)
    file(CREATE_LINK "Hop1.hpp" "${chainTree}/src/Hop2.hpp" SYMBOLIC)
    file(CREATE_LINK "CycleB.hpp" "${chainTree}/src/CycleA.hpp" SYMBOLIC)
    file(CREATE_LINK "CycleA.hpp" "${chainTree}/src/CycleB.hpp" SYMBOLIC)
    MakeRepository("${chainTree}" TRUE)
    MakeUnreadable("${chainTree}/src/Target.hpp" chainBit)
    if(NOT chainBit)
        message(STATUS "  chains: the chain-to-an-unreadable-file half is not DISCRIMINATING here -- "
                       "${unreadableHow} did not stop this process reading the target, so every hop "
                       "opens; the cycle half still is")
    endif()
    fastcached_tracked_files("${chainTree}" GLOBS "src/*" FILES_OUT unused MODE_OUT unusedMode
        MISSING_OUT chainsGitMissing)
    Expect("linkChains" "a cycle is missing and a chain to an unreadable file is not, in git mode"
        "${chainsGitMissing}" "src/CycleA.hpp;src/CycleB.hpp")
    set(realGit "${GIT_EXECUTABLE}")
    set(GIT_EXECUTABLE "${missingGit}")
    fastcached_tracked_files("${chainTree}" GLOBS "src/*" FILES_OUT unused MODE_OUT unusedMode
        MISSING_OUT chainsWalkMissing)
    set(GIT_EXECUTABLE "${realGit}")
    Expect("linkChains" "a cycle is missing and a chain to an unreadable file is not, in a walk"
        "${chainsWalkMissing}" "src/CycleA.hpp;src/CycleB.hpp")
    MakeReadable("${chainTree}/src/Target.hpp")
endif()

# 17d. A directory a WALK cannot list: `file(GLOB_RECURSE)` skips it in silence, so the files
# inside are never found at all -- for every caller, whatever it asked. POSIX only: on Windows
# neither `EXISTS` nor `IS_READABLE` sees a deny-list access list and a glob has no error channel,
# so the walk there is blind to it, a blind spot the seam's comment names as failing OPEN.
if(CMAKE_HOST_WIN32)
    message(STATUS "  unlistable: the unlistable-directory case did NOT run -- on Windows the walk "
                   "cannot see an access list on a directory, so the files under such a directory "
                   "are PASSED unread: this fails OPEN here (see the seam's comment); asserted on "
                   "the POSIX legs")
else()
    set(unlistableTree "${FASTCACHED_SCRATCH_DIR}/unlistable")
    if(IS_DIRECTORY "${unlistableTree}/src/Locked")
        execute_process(COMMAND chmod 755 "${unlistableTree}/src/Locked")
    endif()
    StageTree("${unlistableTree}"
        "src/Kept.hpp|plain"
        "src/Locked/Inside.hpp|return ::htonl(value)")
    execute_process(COMMAND chmod 000 "${unlistableTree}/src/Locked")
    file(GLOB unlistableProbe "${unlistableTree}/src/Locked/*")
    if(unlistableProbe)
        message(STATUS "  unlistable: the unlistable-directory case did NOT run -- chmod 000 did not "
                       "stop this process listing the directory (running as root?)")
    else()
        ExpectRefusalIn("unlistableWalk"
            "cannot list a directory -- src/Locked in .* the walk finds no file inside"
            "${unlistableTree}" "${missingGit}" "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode")
        # The pattern's own prefix, unreadable, lists as nothing at all: refused as well.
        execute_process(COMMAND chmod 755 "${unlistableTree}/src/Locked")
        execute_process(COMMAND chmod 000 "${unlistableTree}/src")
        ExpectRefusalIn("unlistablePrefix"
            "cannot list a directory -- src in .* the walk finds no file inside"
            "${unlistableTree}" "${missingGit}" "GLOBS \"src/*\" FILES_OUT files MODE_OUT mode")
        execute_process(COMMAND chmod 755 "${unlistableTree}/src")
    endif()
    execute_process(COMMAND chmod 755 "${unlistableTree}/src")
    execute_process(COMMAND chmod 755 "${unlistableTree}/src/Locked")
endif()

# ---------------------------------------------------------------------------
# The count is printed whatever the verdict: a self-test that stopped early must not look like
# one that judged something.

if(failureCount GREATER 0)
    message(FATAL_ERROR
        "tracked-files-selftest: ${failureCount} of ${caseCount} assertion(s) failed")
endif()
message(STATUS "tracked-files-selftest: ${caseCount} assertion(s) ran, all as expected")
