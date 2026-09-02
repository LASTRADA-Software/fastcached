# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated. That has already cost this tree once, when
# `if(... IN_LIST ...)` (CMP0057) silently did nothing in a check. Here it is
# CMP0007: without it, `list(FILTER)` over a split file warns on every empty
# element, and a check that prints a wall of warnings is a check nobody reads.
cmake_minimum_required(VERSION 3.28)
#
# Target-file guard hygiene: fail when a test registration names an OPTIONAL
# executable through `$<TARGET_FILE:>` without first asking whether that target
# exists -- and, since #423, when it asks a question that can only ever be
# answered no.
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
# The second rule exists because the first one can be satisfied by a guard that
# is not a condition at all. `src/apps` adds its table IN ORDER, so inside
# `src/apps/fastcache-cc/` the target `compile-cache-testclient` -- two rows
# further down -- does not exist yet, and `if(TARGET compile-cache-testclient)`
# is the constant FALSE. A fixture written that way reports its case SKIPPED on
# every machine, in every job, forever, in a sentence indistinguishable from a
# legitimate skip; and the rule above passes it, because it asks whether the
# guard was WRITTEN. That is skipped-versus-absent again, arriving through the
# checker rather than around it. `src/tests/CMakeLists.txt` is where such a test
# belongs, and it is added after `src/apps` so every guard there is real.
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

# Read and split by hand rather than with `file(STRINGS)`.
#
# `file(STRINGS)` returns a CMake LIST, and CMake's list parser treats an UNBALANCED
# `[` or `]` as structure: one of them merges every following element into a single
# one. Measured on this very check, by appending one `]` to an app table row:
#
#     before:  25 reference(s) to 5 optional target(s), all guarded
#     after:   10 reference(s) to 1 optional target(s), all guarded -- and it PASSED
#
# A REGEX argument does not save it: it filters lines BEFORE the list is built, so it
# protects only where an unbalanced bracket cannot appear on a line the filter KEEPS,
# which is an accident of the pattern rather than a property of the reader.
#
# `file(STRINGS)` takes a PATH, so there is no content to neutralise beforehand and
# the fix used for the other reader in this file cannot be applied to it. Reading the
# bytes and splitting them here can be. Balanced brackets are harmless and are left
# alone; only the grouping characters go, and a space preserves every column.
#
# #509 fixed this file's OTHER reader and left this one, because that change was
# scoped to the splitter that led to it rather than to every reader in the file.
file(READ "${appTable}" appTableContent)
string(REPLACE "[" " " appTableSplit "${appTableContent}")
string(REPLACE "]" " " appTableSplit "${appTableSplit}")
string(REPLACE ";" "\;" appTableSplit "${appTableSplit}")
string(REPLACE "\r\n" "\n" appTableSplit "${appTableSplit}")
string(REPLACE "\n" ";" appRows "${appTableSplit}")
list(FILTER appRows INCLUDE REGEX "^[ \t]*\"[A-Za-z0-9_-]+\\|FASTCACHED_BUILD_")
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

# The refusal above is a FLOOR OF ZERO, and a floor of zero cannot catch a
# truncation that leaves something behind. When one unbalanced bracket took this
# check from five optional targets to one, `optionalTargets` was still non-empty
# and every assertion below still passed: 1 is not 0.
#
# So assert COMPLETENESS, not presence. `FASTCACHED_BUILD_<NAME>` appears exactly
# once per row, and `string(REGEX MATCHALL)` over the RAW bytes never passes
# through CMake's list parser -- the matches themselves contain no brackets, so
# that count survives whatever the rest of the file contains. It is derived from
# the same table, never restated: a second list would not be a cross-check, it
# would be a second thing to be wrong.
#
# This is what makes the check robust against the NEXT blinding mechanism rather
# than only against brackets. Any reader defect that drops rows now fails here.
string(REGEX MATCHALL "FASTCACHED_BUILD_[A-Z_]+" appTableOptionTokens "${appTableContent}")
list(REMOVE_DUPLICATES appTableOptionTokens)
list(LENGTH appTableOptionTokens appTableOptionCount)
list(LENGTH optionalTargets optionalTargetsFound)

if(NOT optionalTargetsFound EQUAL appTableOptionCount)
    message(FATAL_ERROR
        "the app table declares ${appTableOptionCount} build option(s) but only "
        "${optionalTargetsFound} optional target(s) were read from it. The reader "
        "lost rows -- every verdict below would be drawn from a table this check "
        "can no longer see in full.")
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
    #
    # A semicolon is only HALF of that hazard, and this check carried the other
    # half open until #502. CMake's list grouping also treats `[` and `]` as
    # structure, so one unbalanced bracket -- in a COMMENT, where nobody is
    # thinking about CMake syntax -- merges every following line into a single
    # element. `check-sccache-backend-caveat.cmake` had already recorded the same
    # failure ("one stray `]` in a comment swallowed 451 lines") and solved it;
    # the reasoning stayed in that file and never reached this one.
    #
    # Observed here: a sentence added to `src/tests/CMakeLists.txt` mentioning a
    # stray `]` swallowed the rest of the file, and all 25 `$<TARGET_FILE:>`
    # references in it became invisible. Note the shape of the near-miss -- the
    # check did not report "all guarded" over a file it could no longer see. It
    # reported that it had found NOTHING and would pass vacuously, which is the
    # `no reference was found at all` refusal below doing exactly its job. Fixing
    # the splitter is what makes that refusal rare rather than load-bearing.
    #
    # Brackets are replaced rather than escaped because nothing here matches on
    # them: the patterns are `$<TARGET_FILE:x>` and `if`/`elseif`/`endif`, and a
    # space preserves every column and every line number.
    #
    # Two precisions that #495 measured and this comment originally lacked. Only an
    # UNBALANCED bracket groups -- `[[nodiscard]]` is completely harmless -- so
    # "CMake treats `[` and `]` as structure" is broader than the truth. And the
    # replacement above is safe here by WHAT THIS CHECK MATCHES, not by construction:
    # none of its patterns contains a bracket. Add one that does and this check
    # breaks silently, the way `check-worker-refusals-counted` did when the same fix
    # was applied to it -- its spellings read `[[nodiscard]] inline ... Refuse(`, and
    # replacing brackets took it from three to zero. That check now reads its lines
    # without building a CMake list at all, which is the fix to copy if a pattern
    # here ever needs a bracket.
    file(READ "${cmakeFile}" content)
    string(REPLACE ";" "\\;" content "${content}")
    string(REPLACE "[" " " content "${content}")
    string(REPLACE "]" " " content "${content}")
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

            # A guard is only a guard where the target it names can exist. Inside
            # `src/apps/<x>/`, every optional target at or after `<x>` in the table
            # is still undefined, so the question was asked of nothing.
            set(staleGuard FALSE)
            if(guarded AND NOT selfTarget STREQUAL "")
                list(FIND optionalTargets "${selfTarget}" selfIndex)
                list(FIND optionalTargets "${target}" targetIndex)
                if(NOT selfIndex EQUAL -1 AND NOT targetIndex EQUAL -1 AND NOT targetIndex LESS selfIndex)
                    set(staleGuard TRUE)
                endif()
            endif()

            # Three outcomes, not two, and the stale one reports as itself rather
            # than as the missing-guard case beside it. They are fixed differently:
            # one asks for a condition to be added, the other for the registration
            # to move, and a reader told the wrong one adds a guard that is already
            # there.
            if(staleGuard)
                list(APPEND violations
                     "${relative}:${lineNumber}: `TARGET ${target}` is asked inside src/apps/${selfTarget}/, which the app table configures FIRST -- the target does not exist yet, so that guard is the constant FALSE and everything it protects is silently dropped. Register it in src/tests/CMakeLists.txt, which is added after src/apps")
            elseif(guarded)
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
    message("")
    message("And a guard asked from src/apps/<x>/ about a target the app table configures")
    message("later is not a guard: it is FALSE on every machine, and the test it protects")
    message("never runs anywhere. Register such a test in src/tests/CMakeLists.txt.")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR "target-file guard hygiene: ${violationCount} unguarded reference(s)")
endif()

message(STATUS
    "target-file guard hygiene: ${guardedCount} reference(s) to ${optionalTargetCount} optional target(s), all guarded")
