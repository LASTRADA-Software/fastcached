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

execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --is-inside-work-tree
    WORKING_DIRECTORY "${FASTCACHED_SOURCE_DIR}"
    OUTPUT_VARIABLE insideWorkTree
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE revParseResult
)
if(NOT revParseResult STREQUAL "0" OR NOT insideWorkTree STREQUAL "true")
    message("SKIP: ${FASTCACHED_SOURCE_DIR} is not a git work tree "
            "(an exported source tarball, for instance), so there is no index "
            "to inspect")
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
