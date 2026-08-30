# SPDX-License-Identifier: Apache-2.0
#
# Target-file guard hygiene: fail when a test registration names an OPTIONAL
# executable through `$<TARGET_FILE:>` without first asking whether that target
# exists.
#
# `$<TARGET_FILE:x>` naming a target that was not built is not a test that gets
# skipped. It is a hard error at GENERATE time, so the whole configuration
# fails:
#
#   CMake Error at src/tests/CMakeLists.txt:943 (add_test):
#     Error evaluating generator expression: $<TARGET_FILE:fastcached>
#     No target "fastcached"
#
# That is #390. Two blocks were guarded on their own feature -- `tls-smoke` on
# `FASTCACHED_ENABLE_TLS`, the sccache smokes on `SCCACHE` -- and on nothing
# else, so `-DFASTCACHED_BUILD_DAEMON=OFF` refused to generate. The two
# conditions are independent: a feature can be enabled while the binary it needs
# is not built.
#
# It hid because it is conditional on the DEVELOPER's machine rather than on the
# tree. The sccache rows are themselves gated on finding sccache, so a checkout
# without sccache installed never reaches the broken reference and the flag
# appears to work. The error then names CMake rather than the option, on some
# machines and not others.
#
# Which targets are optional is READ from `src/apps/CMakeLists.txt`, never
# restated here. That file's `FASTCACHED_APPS` table is the one place an app and
# the option gating it are written down, and a second copy is not a cross-check
# -- it is a second thing to be wrong. A new app therefore arrives under this
# guard by existing, without anyone remembering to add it.
#
# Runs as `cmake -P`, for the reason check-test-names.cmake gives: this reads
# files, compares strings and reports, so a .sh + .ps1 pair would be two
# implementations of one rule differing only in syntax.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-target-file-guards.cmake
#
# Exit codes: 0 = every optional target is asked about before it is named.
# 1 = at least one is not.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# ---------------------------------------------------------------------------
# The optional targets, read from the app table rather than listed again.
set(appTable "${FASTCACHED_SOURCE_DIR}/src/apps/CMakeLists.txt")
if(NOT EXISTS "${appTable}")
    message(FATAL_ERROR "the app table is missing: ${appTable}")
endif()

file(STRINGS "${appTable}" appRows REGEX "^[ \t]*\"[A-Za-z0-9_-]+\\|FASTCACHED_BUILD_")
set(optionalTargets "")
foreach(row IN LISTS appRows)
    if(row MATCHES "\"([A-Za-z0-9_-]+)\\|")
        list(APPEND optionalTargets "${CMAKE_MATCH_1}")
    endif()
endforeach()

# A guard that found nothing to guard reports success while checking nothing,
# which is the failure mode this whole file exists to argue against.
if(NOT optionalTargets)
    message(FATAL_ERROR
        "no optional targets were read from ${appTable}; this check would pass vacuously")
endif()
list(LENGTH optionalTargets optionalTargetCount)

# ---------------------------------------------------------------------------
# Every file that may register a test. A file this does not scan is a hole that
# reports green.
file(GLOB_RECURSE cmakeFiles "${FASTCACHED_SOURCE_DIR}/src/*CMakeLists.txt")

set(violations "")
set(referenceCount 0)
set(guardedCount 0)

foreach(cmakeFile IN LISTS cmakeFiles)
    file(RELATIVE_PATH relative "${FASTCACHED_SOURCE_DIR}" "${cmakeFile}")

    # Read and split by hand rather than with `file(STRINGS)`. That command
    # returns a LIST, so a line containing a semicolon -- which several comments
    # in this tree do -- becomes two elements: the line numbers drift and an
    # `if()` condition can be torn in half, after which every verdict for the
    # rest of the file is drawn from the wrong stack. Escaping first is what
    # keeps one line one element.
    file(READ "${cmakeFile}" content)
    string(REPLACE ";" "\\;" content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")

    # The directory an app is defined in is added only when that app's option is
    # on, so inside `src/apps/<t>/` the target `<t>` exists by construction and
    # needs no guard. A reference to any OTHER optional target from there is an
    # ordinary cross-target reference and is checked like the rest.
    set(selfTarget "")
    if(relative MATCHES "^src/apps/([A-Za-z0-9_-]+)/CMakeLists\\.txt$")
        set(selfTarget "${CMAKE_MATCH_1}")
    endif()

    # `frames` is the stack of enclosing `if` chains, one entry per chain,
    # holding every condition that chain has spelled so far. All of them count:
    # inside an `elseif`, the earlier conditions are known FALSE, so
    # `if(NOT TARGET x) ... elseif(SCCACHE)` establishes the target exists just
    # as `if(TARGET x)` does. Asking whether the target is MENTIONED, rather
    # than parsing the boolean, is deliberate -- this guard is about the
    # question having been asked at all.
    set(frames "")
    set(lineNumber 0)

    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")

        if(line MATCHES "^[ \t]*if[ \t]*\\((.*)\\)[ \t]*$")
            list(APPEND frames "${CMAKE_MATCH_1}")
        elseif(line MATCHES "^[ \t]*elseif[ \t]*\\((.*)\\)[ \t]*$")
            set(extra "${CMAKE_MATCH_1}")
            list(POP_BACK frames top)
            list(APPEND frames "${top} ${extra}")
        elseif(line MATCHES "^[ \t]*endif[ \t]*\\(")
            list(POP_BACK frames)
        endif()

        # A comment naming the expression is prose, not a reference. The
        # paragraph above `compile-cache-e2e` in the launcher's own CMakeLists
        # spells out this exact rule and was read as breaking it -- a checker
        # failing on the documentation of the thing it checks.
        if(line MATCHES "^[ \t]*#")
            continue()
        endif()

        foreach(target IN LISTS optionalTargets)
            if(NOT line MATCHES "\\$<TARGET_FILE:${target}>")
                continue()
            endif()
            if(target STREQUAL selfTarget)
                continue()
            endif()
            math(EXPR referenceCount "${referenceCount} + 1")

            set(guarded FALSE)
            foreach(frame IN LISTS frames)
                if(frame MATCHES "TARGET[ \t]+${target}([ \t)]|$)")
                    set(guarded TRUE)
                    break()
                endif()
            endforeach()

            if(guarded)
                math(EXPR guardedCount "${guardedCount} + 1")
            else()
                list(APPEND violations
                     "${relative}:${lineNumber}: $<TARGET_FILE:${target}> is reached without any enclosing `TARGET ${target}` condition")
            endif()
        endforeach()
    endforeach()

    # An unbalanced stack means the parse above lost track, and every verdict
    # from that file afterwards was drawn from the wrong conditions. Reported
    # rather than absorbed: a checker that quietly recovers is one that quietly
    # stops checking.
    if(frames)
        list(LENGTH frames leftOver)
        list(APPEND violations
             "${relative}: ${leftOver} unclosed if() block(s) at end of file; this checker could not follow it, so its verdicts on this file mean nothing")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# A reference count of zero means the scan matched nothing -- a moved file, a
# renamed target, a glob that stopped globbing -- and "no violations" would then
# be a statement about the checker rather than about the tree.
if(referenceCount EQUAL 0)
    message(FATAL_ERROR
        "no $<TARGET_FILE:> reference to any of the ${optionalTargetCount} optional targets was found at all; "
        "this check would pass vacuously")
endif()

if(violations)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("A `$<TARGET_FILE:x>` naming a target that was not built fails the CMake")
    message("GENERATE step -- the whole configure, not just that test. Guard the block on")
    message("`TARGET x` as well as on whatever feature makes the test interesting, and say")
    message("what was skipped, the way the blocks around it do.")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR "target-file guard hygiene: ${violationCount} unguarded reference(s)")
endif()

message(STATUS
    "target-file guard hygiene: ${guardedCount} reference(s) to ${optionalTargetCount} optional target(s), all guarded")
