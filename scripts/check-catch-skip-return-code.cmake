# SPDX-License-Identifier: Apache-2.0
#
# Every `catch_discover_tests` registration tells ctest that exit 4 means SKIPPED.
#
# A Catch2 case that calls `SKIP(...)` exits **4**. `SKIP_RETURN_CODE` is the only
# thing that tells ctest an exit code means *skipped* rather than *failed*, and none
# of the five Catch2 registrations set it -- so the binary printed `1 skipped` and
# ctest printed `***Failed` for the very same run
# ([#499](https://github.com/LASTRADA-Software/fastcached/issues/499)). Measured on
# one forced skip:
#
#     CowTreeTests "[skiprepro]"   ->  test cases: 1 | 1 skipped   BINARY EXIT: 4
#     ctest -R "..."               ->  ***Failed
#     after SKIP_RETURN_CODE 4     ->  ***Skipped
#
# ## Why this is a check and not just a comment
#
# **Skipped, absent, unstarted and failed are four states**, and a registration that
# omits this property collapses two of them at the reporting layer. It is a false RED,
# which is the insidious direction: the seven `SKIP` sites in this tree are all
# environment-conditional -- three on `AvailableCodecs().size() < 2`, four on being
# able to bind loopback -- so they fire on a constrained runner or a build configured
# without codecs and report a regression that is not there. Nobody had hit it because
# those conditions had not arisen on CI, which is luck rather than coverage.
#
# It also makes `SKIP` unusable as a tool. Whoever adds one, watches the suite go red
# and concludes the skip is wrong will delete the skip rather than suspect the
# registration -- the wrong lesson, learned permanently.
#
# The repository already knew the mechanism: eleven script-driven `add_test`
# registrations in `src/tests/CMakeLists.txt` set `SKIP_RETURN_CODE 77`, one directory
# away from five Catch2 registrations that set nothing. So a comment is not what was
# missing. **A sixth test binary would reopen this by omission exactly as the fifth
# did**, which is the door this closes -- the same argument as
# `check-script-check-signals.cmake`, and the set of registrations is READ from the
# tree rather than restated here, for the same reason.
#
# ## The value is checked, not just the property
#
# `77` is the GNU convention the script-driven tests use because a shell script
# chooses its own exit code. A Catch2 binary does not: Catch2 exits **4** on skip, and
# nothing else. A registration carrying `SKIP_RETURN_CODE 77` would look correct in a
# diff, satisfy any "is the property present" scan, and still score every skip as a
# failure -- so the number is part of the rule.
#
# ## A caution for whoever changes a registration
#
# `catch_discover_tests` writes its test list in a **POST_BUILD** step, so editing the
# CMakeLists and reconfiguring is not enough: the old list survives a full successful
# `cmake --preset`, and the stale result reads exactly like a current one. Measured --
# after the edit and a clean reconfigure the case still reported `***Failed`, and only
# a REBUILD turned it into `***Skipped`. If you are verifying this property by hand,
# rebuild the test target first or you will be reading yesterday's answer.
#
# Runs as `cmake -P`: it reads files, compares strings and reports. See
# `check-script-check-signals.cmake` for why such a check reports failure through its
# OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-catch-skip-return-code.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

# A `cmake -P` script has no project, so every policy starts unset and CMP0057
# (`if(... IN_LIST ...)`) is one of them -- unset, the operator is an unknown argument
# and the script errors out rather than answering, on the CMake version this project
# declares. Stated as a minimum version so the whole set moves together.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# What Catch2 exits with on `SKIP(...)`. Not 77: see the header.
set(catchSkipCode 4)

# Read and split by hand rather than with `file(STRINGS)`, which returns a LIST: a
# line containing a semicolon becomes two elements and every line number after it
# drifts. Backslashes are escaped first, or a line ending in one merges with the next
# and every reported line number after it is wrong.
function(fastcached_read_lines path outVar)
    file(READ "${path}" content)
    string(REPLACE "\\" "\\\\" content "${content}")
    string(REPLACE ";" "\\;" content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(${outVar} "${lines}" PARENT_SCOPE)
endfunction()

# Which CMakeLists this REPOSITORY owns, asked of git rather than inferred from
# directory names.
#
# This is the third time the same defect bit this check, and the first two fixes were
# both the wrong shape. The scan must not see vendored third-party code -- nobody here
# can edit it, and Catch2 ships a bare `catch_discover_tests` of its own. The first
# version globbed from the source root and reported `_deps/catch2-src`. The second
# excluded a LIST of directory names -- `out`, `build`, `_deps` -- and CI then found
# the same Catch2 tree at `.cache/CPM/catch2/<hash>/tests/...`, because CPM resolves
# dependencies inside the source tree under a name that list did not have.
#
# A list of names to exclude is a hand-kept list, which is the exact thing this
# check's own ticket exists to retire. So stop enumerating: `git ls-files` answers
# "what does this repository contain" directly, and a dependency cache is untracked by
# construction whatever it is called and wherever a package manager decides to put it.
# It is also fast -- no stat of a build tree at all.
#
# The same idiom, and the same fallback, as `check-repository-hygiene.cmake`.
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

set(listFiles "")
set(scanSource "")

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" rev-parse --is-inside-work-tree
        OUTPUT_VARIABLE insideWorkTree
        ERROR_VARIABLE gitError
        RESULT_VARIABLE gitStatus
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(gitStatus EQUAL 0 AND insideWorkTree STREQUAL "true")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" ls-files -- "*CMakeLists.txt"
            OUTPUT_VARIABLE tracked
            RESULT_VARIABLE lsStatus
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(lsStatus EQUAL 0 AND NOT tracked STREQUAL "")
            string(REPLACE "\n" ";" listFiles "${tracked}")
            set(scanSource "git ls-files")
        endif()
    endif()
endif()

# No git, or an export with no index. A source export contains no build tree and no
# dependency cache BY CONSTRUCTION -- that is what makes the walk sound here and
# unsound in a working checkout -- so the fallback is a plain recursive glob with the
# build-tree names still excluded, and it says which mode produced the answer.
if(NOT listFiles)
    set(excludeNames "out" "build" "_deps" ".git" ".cache" ".claude")
    file(GLOB_RECURSE walked RELATIVE "${FASTCACHED_SOURCE_DIR}"
         "${FASTCACHED_SOURCE_DIR}/CMakeLists.txt")
    foreach(candidate IN LISTS walked)
        set(skip FALSE)
        foreach(name IN LISTS excludeNames)
            if(candidate MATCHES "(^|/)${name}/")
                set(skip TRUE)
                break()
            endif()
        endforeach()
        if(NOT skip)
            list(APPEND listFiles "${candidate}")
        endif()
    endforeach()
    set(scanSource "directory walk (no git index)")
endif()

list(REMOVE_DUPLICATES listFiles)
list(SORT listFiles)

if(NOT listFiles)
    message("")
    message("  No CMakeLists.txt was found in this repository at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved")
    message("source root, or a FASTCACHED_SOURCE_DIR pointing somewhere else.")
    message(FATAL_ERROR "catch skip return code: the scan matched no CMakeLists and cannot conclude")
endif()

set(registrations "")
set(violations "")

foreach(relative IN LISTS listFiles)
    fastcached_read_lines("${FASTCACHED_SOURCE_DIR}/${relative}" lines)

    set(lineNumber 0)
    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")

        # Prose, not a call. This file's own header names the function it checks.
        if(line MATCHES "^[ \t]*#")
            continue()
        endif()
        if(NOT line MATCHES "catch_discover_tests[ \t]*\\(")
            continue()
        endif()

        list(APPEND registrations "${relative}:${lineNumber}")

        # The property may sit on the same line or on a continuation of the call, so
        # the call is read to its closing parenthesis rather than one line being
        # tested. A registration split across lines is legal CMake and must not read
        # as a violation.
        set(callText "${line}")
        set(depth 0)
        set(scanNumber 0)
        foreach(inner IN LISTS lines)
            math(EXPR scanNumber "${scanNumber} + 1")
            if(scanNumber LESS lineNumber)
                continue()
            endif()
            if(NOT scanNumber EQUAL lineNumber)
                set(callText "${callText} ${inner}")
            endif()
            string(REGEX MATCHALL "\\(" opens "${inner}")
            string(REGEX MATCHALL "\\)" closes "${inner}")
            list(LENGTH opens openCount)
            list(LENGTH closes closeCount)
            math(EXPR depth "${depth} + ${openCount} - ${closeCount}")
            if(depth LESS_EQUAL 0)
                break()
            endif()
        endforeach()

        if(NOT callText MATCHES "SKIP_RETURN_CODE[ \t]+([0-9]+)")
            list(APPEND violations "${relative}:${lineNumber}: sets no SKIP_RETURN_CODE, so every Catch2 skip scores as a FAILURE")
        elseif(NOT CMAKE_MATCH_1 EQUAL catchSkipCode)
            list(APPEND violations
                 "${relative}:${lineNumber}: SKIP_RETURN_CODE ${CMAKE_MATCH_1}, but Catch2 exits ${catchSkipCode} on SKIP -- the property is present and does nothing")
        endif()
    endforeach()
endforeach()

# A scan that found NO registration is not a clean tree -- this project has several
# Catch2 test binaries, so zero means a renamed function, a moved file or a changed
# spelling. Reported as its own failure rather than folded into success, which is the
# direction this project keeps getting wrong.
list(LENGTH registrations registrationCount)
if(registrationCount EQUAL 0)
    message("")
    message("  No `catch_discover_tests` registration was found anywhere.")
    message("")
    message("This project has several Catch2 test binaries, so zero means this scan is")
    message("no longer looking at what it thinks it is. It is not evidence that every")
    message("registration reports skips correctly.")
    message(FATAL_ERROR "catch skip return code: the scan matched nothing and cannot conclude")
endif()

if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("A Catch2 case that calls `SKIP(...)` exits ${catchSkipCode}. Without")
    message("`SKIP_RETURN_CODE ${catchSkipCode}` ctest scores that as a FAILURE, so the")
    message("binary prints `1 skipped` and ctest prints `***Failed` for the same run.")
    message("")
    message("That is a false RED on any runner where an environment-conditional skip")
    message("fires -- and it teaches whoever meets it that the SKIP was wrong, rather")
    message("than the registration. Add to the registration:")
    message("")
    message("    catch_discover_tests(<target> PROPERTIES SKIP_RETURN_CODE ${catchSkipCode})")
    message("")
    message("`catch_discover_tests` writes its list at BUILD time, so rebuild the target")
    message("rather than only reconfiguring -- the old list survives a reconfigure and")
    message("reads exactly like a current one.")
    message(FATAL_ERROR "catch skip return code: ${violationCount} registration(s) score skips as failures")
endif()

message(STATUS
    "catch skip return code: ${registrationCount} Catch2 registration(s) via ${scanSource}, all reporting skips as skips")
