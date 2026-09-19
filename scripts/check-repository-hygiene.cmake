# SPDX-License-Identifier: Apache-2.0
#
# Repository hygiene: fail if a path that must never be committed is tracked by
# git.
#
# Runs as `cmake -P`, and deliberately not as the .sh + .ps1 pair the other test
# drivers in this directory use. Those pairs exist because they drive processes
# and sockets in ways that genuinely differ per platform (SslStream versus
# `openssl s_client`, Start-Process versus `&`). This check runs git, compares
# strings and reports — nothing platform-specific — so a second copy in a second
# language would be two implementations of one rule differing only in syntax,
# each free to rot without the other noticing. It also has strictly fewer
# prerequisites than either: cmake is the build tool and is therefore always
# present, whereas pwsh is a separate install on Windows (the shipped shell is
# powershell.exe 5.1) and bash is not on a Windows box at all. There is in-repo
# precedent: cmake/MacOSNotarizePkg.cmake and cmake/MacOSSignBinaries.cmake are
# already `cmake -P` scripts.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> [-DGIT_EXECUTABLE=<git>] \
#         -P scripts/check-repository-hygiene.cmake
#
# Exit codes: 0 = clean, or skipped. 1 = at least one forbidden path is tracked.
# A skip prints a line beginning with `SKIP: ` and exits 0, and the CTest
# registration in src/tests/CMakeLists.txt turns that into a Skipped result via
# SKIP_REGULAR_EXPRESSION. The other drivers here use exit 77 with
# SKIP_RETURN_CODE, which is the nicer convention, but a `cmake -P` script cannot
# choose its own exit code before CMake 3.29 (cmake_language(EXIT)) and this
# project supports 3.28 — a SKIP_RETURN_CODE it could never return would be dead
# configuration. Swap both halves the day that floor moves.

# ---------------------------------------------------------------------------
# One row per path that must never be tracked:
#
#   <path relative to the source root>|<why it must not be committed>
#
# The reason is printed on failure, so whoever trips this meets an explanation
# rather than a rule. A second forbidden path is a second row and nothing below
# changes.
#
# No row may contain a ';' — these are CMake lists, and a semicolon inside a row
# would split it into two.

cmake_minimum_required(VERSION 3.28)

set(FastCachedForbiddenTrackedPaths
    "version.txt|The git tag is the single source of truth for the version (see cmake/Version.cmake). A committed version.txt outranks every tag, which makes it a second version carrier that each release has to remember to bump in lock-step - and while it existed it pinned every build, every wire banner and every package to whatever it said. Keep it untracked if you want a local override: that is supported and this check will not complain."
)

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

if(NOT IS_DIRECTORY "${FASTCACHED_SOURCE_DIR}")
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR='${FASTCACHED_SOURCE_DIR}' is not a directory.")
endif()

# The configure step passes the git it already located, so the check and the
# version resolution cannot disagree about which git they mean. The lookup here
# is only for a direct invocation.
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()
if(NOT GIT_EXECUTABLE)
    message("SKIP: no git executable found, so nothing here can be tracked by git")
    return()
endif()

# Asked through `fastcached_work_tree_state` (#1485), the one spelling of this question. It
# answers in THREE values and the two negative ones get different sentences here: a check with
# no index to inspect owes an operator "git cannot answer" or "this is not a checkout", never
# one message covering both. This check has no directory-walk fallback and must not grow one --
# its subject is what the INDEX holds, which a walk cannot see.
include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")
fastcached_work_tree_state("${FASTCACHED_SOURCE_DIR}" workTreeState)
if(workTreeState STREQUAL "no-git")
    # THREE situations reach `no-git`, and only two of them are ordinary (#1565).
    #
    # `git rev-parse --is-inside-work-tree` exits 128 both for a directory that is in
    # no repository and for a directory that IS a checkout whose `.git` cannot be
    # followed -- a worktree pointer naming a gitdir this host cannot reach, which is
    # what a `git worktree add` written with Windows paths looks like from WSL. The
    # seam cannot tell them apart, because it asks git one question and git answers
    # "not a git repository" to both.
    #
    # They are not the same fact and they are not fixed in the same place. An exported
    # source tarball has no index and never will; that is what this check has always
    # skipped for, and the registration in `src/tests/CMakeLists.txt` says so. A
    # checkout git cannot read is a broken HOST, and skipping it means this check has
    # reported nothing about a tree it was pointed straight at -- which is the failure
    # a skip and a pass are indistinguishable for.
    #
    # What separates them is a fact the seam does not carry and this directory does: a
    # `.git` entry. Present and unreadable is the fault; absent is the tarball.
    #
    # It stays in this check rather than becoming a fourth answer from
    # `fastcached_work_tree_state`, because the two callers disagree about it: a check
    # that falls back to a directory WALK is right to treat both alike, and this one
    # has no fallback and must not grow one.
    if(EXISTS "${FASTCACHED_SOURCE_DIR}/.git")
        # `no-git` also covers "the binary could not be RUN", and only the seam can
        # tell that from "it ran and refused" -- so this asks, rather than claiming
        # the narrower fact. A refusal that names the wrong cause is the one thing
        # every message here is written to avoid.
        execute_process(COMMAND "${GIT_EXECUTABLE}" --version
                        RESULT_VARIABLE gitRuns OUTPUT_QUIET ERROR_QUIET)
        if(NOT gitRuns EQUAL 0)
            message(FATAL_ERROR
                "the git at ${GIT_EXECUTABLE} could not be RUN, so nothing here could be "
                "shown to be tracked by it, and ${FASTCACHED_SOURCE_DIR} carries a `.git` "
                "entry -- this is a checkout with a broken tool, not a tree without an "
                "index.\n"
                "       Nothing in this check ran. Fix the git on this host; there is "
                "nothing to change in the tree.")
        endif()
        message(FATAL_ERROR
            "git runs on this host and cannot read ${FASTCACHED_SOURCE_DIR}, which "
            "carries a `.git` entry -- so this is a checkout whose git metadata is "
            "unreachable from here, not a tree without one.\n"
            "       Nothing in this check ran. It reports on what the INDEX holds and "
            "has no directory-walk fallback, so a quiet exit here would be this check "
            "saying nothing about a tree it was pointed straight at.\n"
            "       This is the shape a git worktree takes when its pointer names a path "
            "the running git cannot follow -- a `.git` file written with Windows paths, "
            "read from WSL, or the reverse. Repair it with:\n"
            "         bash scripts/repair-worktree-pointers.sh\n"
            "       run from inside the worktree. If git itself is broken rather than "
            "the pointer, `git -C ${FASTCACHED_SOURCE_DIR} rev-parse "
            "--is-inside-work-tree` prints the reason.")
    endif()
    message("SKIP: ${FASTCACHED_SOURCE_DIR} carries no `.git` entry and git reports no "
            "repository here (an exported source tarball, for instance), so there is no "
            "index to inspect")
    return()
endif()
if(NOT workTreeState STREQUAL "work-tree")
    message("SKIP: ${FASTCACHED_SOURCE_DIR} is inside a repository but outside its work "
            "tree (a `.git` directory, or a bare repository), so there is no index to "
            "inspect")
    return()
endif()

set(trackedViolations "")

foreach(forbiddenRow IN LISTS FastCachedForbiddenTrackedPaths)
    string(REPLACE "|" ";" forbiddenFields "${forbiddenRow}")
    list(GET forbiddenFields 0 forbiddenPath)
    list(GET forbiddenFields 1 forbiddenReason)

    # `ls-files --error-unmatch` asks the *index*, which is the earliest point at
    # which the mistake exists: it fails the moment the file is `git add`ed,
    # before any commit, and in a CI checkout the index is the commit. Mere
    # existence on the filesystem is deliberately not the test — an untracked
    # local version.txt is a supported override and must pass.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" ls-files --error-unmatch -- "${forbiddenPath}"
        WORKING_DIRECTORY "${FASTCACHED_SOURCE_DIR}"
        OUTPUT_QUIET
        ERROR_QUIET
        RESULT_VARIABLE trackedResult
    )

    if(trackedResult STREQUAL "0")
        list(APPEND trackedViolations
            "  ${forbiddenPath}\n      ${forbiddenReason}")
    endif()
endforeach()

list(LENGTH FastCachedForbiddenTrackedPaths forbiddenPathCount)

if(NOT trackedViolations STREQUAL "")
    list(JOIN trackedViolations "\n" violationReport)
    message(FATAL_ERROR
        "Forbidden path(s) are tracked by git:\n\n${violationReport}\n\n"
        "Remove each one from the index and commit that removal:\n"
        "  git rm --cached <path>\n"
        "The file may stay on disk. It must not be tracked.")
endif()

message("repository hygiene: ${forbiddenPathCount} forbidden path(s) checked, "
        "none tracked")
