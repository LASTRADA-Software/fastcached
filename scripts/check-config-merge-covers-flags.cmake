# SPDX-License-Identifier: Apache-2.0
#
# Every `*Explicit` bit a CLI flag sets must be consulted by the config merge.
#
# ## The regression this exists to catch, which has happened four times
#
# `CliResult` carries one `bool <field>Explicit` per typed flag -- the "the operator
# typed this" tracker -- and `Config::Merge` is a list of
# `MergeField(dst, cli, &CliResult::fooExplicit, &Config::foo)` rows that consult
# them. Add a flag and forget the row, and the flag parses, validates, appears in
# `--help`, and works perfectly **until the operator also has a config file** -- at
# which point the file's value silently wins and the command line is discarded.
#
# Nothing logs it. The operator's mental model, "the command line overrides the
# file", is correct for every other flag they have tried. `ConfigMerge.cpp`'s own
# comment records this happening to `serviceName`, `lruRecency`, `cpuAffinity` and
# `notifyKeyspaceEvents`, each found by a human reading a diff
# ([#130](https://github.com/LASTRADA-Software/fastcached/issues/130)).
#
# ## What this checks, and what it deliberately does not
#
# **Presence, not correctness.** A row's *shape* is already checked by the compiler:
# `MergeField(dst, cli, &CliResult::fooExplicit, &Config::foo)` is type-checked, so a
# row that names the wrong `Config` field for its bit does not build. What nothing
# checked is that a row EXISTS at all, and that is precisely the failure above -- a
# bit declared and consulted by nobody.
#
# So the two halves cover it between them, and neither alone does. This is the cheap
# half; #130 also proposes driving `Merge` off the flag table itself, which would make
# the two one fact rather than two that agree. That remains the better end state.
#
# ## Why a scan rather than a test
#
# A test would have to build a distinguishable value per field through a type-erased
# `apply`, and `Config` has no `operator==` to compare against -- #130 says as much.
# The scan asks a narrower question it can actually answer.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(declaringFile "src/FastCache/Config/CliParser.hpp")
set(mergingFile "src/FastCache/Config/ConfigMerge.cpp")

foreach(relative IN ITEMS "${declaringFile}" "${mergingFile}")
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        message(FATAL_ERROR "a file this check reads is missing: ${relative}")
    endif()
endforeach()

# Read and split by hand rather than with `file(STRINGS)`, which returns a LIST: a
# line containing a semicolon becomes two elements and every line number after it
# drifts. C++ is made of semicolons, so this is not a corner case here.
function(fastcached_lines_of relative outVar)
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" content)
    string(REPLACE ";" "\\;" content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    set(${outVar} "${lines}" PARENT_SCOPE)
endfunction()

set(declared "")
fastcached_lines_of("${declaringFile}" declaringLines)
foreach(line IN LISTS declaringLines)
    # Prose, not a declaration. A bit named only in a doc comment is not a bit.
    if(line MATCHES "^[ \t]*(//|///|\\*)")
        continue()
    endif()
    if(line MATCHES "bool[ \t]+([A-Za-z0-9_]+Explicit)[ \t]*\\{")
        list(APPEND declared "${CMAKE_MATCH_1}")
    endif()
endforeach()

set(merged "")
fastcached_lines_of("${mergingFile}" mergingLines)
foreach(line IN LISTS mergingLines)
    if(line MATCHES "^[ \t]*(//|///|\\*)")
        continue()
    endif()
    if(line MATCHES "&CliResult::([A-Za-z0-9_]+Explicit)")
        list(APPEND merged "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(REMOVE_DUPLICATES merged)
list(LENGTH declared declaredCount)
list(LENGTH merged mergedCount)

# A scan that matches NOTHING is a scan that has stopped looking at what it thinks it
# is -- a renamed field, a moved file, a changed spelling -- and reporting that as
# "no gaps" turns a broken check into success, which is the direction this project
# keeps getting wrong. Both sides, because either going quiet is the same failure.
if(declaredCount EQUAL 0 OR mergedCount EQUAL 0)
    message("")
    message("  Found ${declaredCount} explicit bit(s) in ${declaringFile}")
    message("  and ${mergedCount} merge row(s) in ${mergingFile}.")
    message("")
    message("Zero on either side means this scan is no longer reading what it was")
    message("written against. Fix the scan; do not read this as an absence of gaps.")
    message(FATAL_ERROR "check-config-merge-covers-flags found nothing to check")
endif()

set(missing "")
foreach(bit IN LISTS declared)
    if(NOT bit IN_LIST merged)
        list(APPEND missing "${bit}")
    endif()
endforeach()

list(LENGTH missing missingCount)
if(NOT missingCount EQUAL 0)
    string(REPLACE ";" ", " names "${missing}")
    message("")
    message("  ${missingCount} flag(s) set an explicit bit that the config merge never reads:")
    message("      ${names}")
    message("")
    message("Each of those parses, validates and appears in --help, and is then silently")
    message("discarded whenever the operator also has a config file: the file's value wins")
    message("and nothing says so. Add a MergeField row in ${mergingFile}.")
    message("")
    message(FATAL_ERROR "check-config-merge-covers-flags found ${missingCount} unmerged flag(s)")
endif()

message(STATUS "config merge covers all ${declaredCount} explicit flag bit(s)")
