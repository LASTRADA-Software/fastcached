# SPDX-License-Identifier: Apache-2.0
#
# `istreambuf-iterator` must be SEEN to refuse a use, and seen to stay QUIET over the
# prose that documents the rule.
#
# Both arms, or the check is only half-tested -- and the QUIET arm is the one that
# decides whether a check survives. Seven of this tree's eight occurrences are
# comments, including the doc comment on the helper header that exists to be the one
# spelling. A check that refused those would refuse the documentation of its own rule,
# with no escape hatch but rewording the comment, and it would be deleted rather than
# obeyed.
#
# ## The mode is part of the output, and asserted on both sides
#
# This fixture stages synthetic trees, and a synthetic tree is not a git repository.
# So every case would exercise the DIRECTORY WALK while CI exercises GIT -- which is
# exactly how `check-catch-skip-return-code` came to have six green self-test cases
# and a red CI run with no contradiction between them: the mode under test was not
# the mode in use.
#
# So the check names its mode in its output, cases assert it, and the git path is
# staged for real with `git init` plus `git add` -- an index is all `git ls-files`
# needs, so no commit and no identity are required.
#
# ## Line numbers are asserted, not just filenames
#
# `;`, `\`, `[` and `]` are all CMake list structure and C++ is full of all four. Get
# the reading wrong and lines merge: the following line is hidden and every line number
# below it drifts. One case therefore plants all four ABOVE a violation and asserts the
# exact `file:line`, because an assertion on the FILENAME alone passes under the bug.
#
# That case earned its keep immediately. The check first used the house
# `fastcached_read_lines` splitting idiom, and this case failed: a line ending in a
# backslash merges with the next one there -- measured, 4 list elements where 5 are
# right -- which is the one thing that reader's own comment claims it prevents. Because
# the merged element began with `//`, the comment stripper then ate the real code, so
# the use was reported NOWHERE. A false green. The check is list-free now.
#
# The mutations are applied to a SYNTHESISED tree, never to the tree under test, so
# there is no revert step and nothing can be left behind. Each case asserts its
# mutation actually LANDED before any verdict is drawn from it.
#
# The BASELINE is load-bearing: every refusal below is evidence only if the unmutated
# tree passes, or each case is merely observing the same pre-existing failure.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-istreambuf-iterator-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-istreambuf-iterator.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

# ---------------------------------------------------------------------------
# The synthesised ground truth: three sources, none of which USES the iterator and
# two of which explain why not. That mirrors the real tree, where prose outnumbers
# code seven to nothing, and it is what makes the quiet arm meaningful.
set(baseCleanSource
"// SPDX-License-Identifier: Apache-2.0
#include <string>

namespace FastCache
{
int Clean()
{
    return 0;
}
} // namespace FastCache
")

set(baseDocumentedSource
"// SPDX-License-Identifier: Apache-2.0
#include <sstream>

namespace FastCache
{
std::string ReadAll(std::istream& in)
{
    // Via the stream buffer rather than istreambuf_iterator: GCC inlines the
    // iterator's sgetc and reports -Werror=null-dereference inside <streambuf>.
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
} // namespace FastCache
")

set(baseHelperHeader
"// SPDX-License-Identifier: Apache-2.0
#pragma once

/*
 Read a whole file as bytes.

 Sized from the stream and read in one call, deliberately NOT through
 std::istreambuf_iterator. GCC 14 at -O2 inlines that iterator's sgetc and then
 reports -Werror=null-dereference inside <streambuf> itself.
*/
namespace FastCache::Cc
{
} // namespace FastCache::Cc
")

# ---------------------------------------------------------------------------
# Stage a tree, apply one mutation, run the check, return its collapsed output.
function(fastcached_stage_and_run name target from to outOutput outApplied)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")

    # `~n~`, `~lb~`, `~rb~` and `~sc~` are a newline, `[`, `]` and `;`. Those three
    # characters cannot appear literally in a case row: `|` is the field separator and
    # `;` splits the CMake list, while an unbalanced `[` merges every following row
    # into one element -- which is the same bracket-vulnerable reader this check's
    # own line splitter exists to work around, and it bit the sibling fixture's row
    # table first.
    string(REPLACE "~n~" "\n" from "${from}")
    string(REPLACE "~lb~" "[" from "${from}")
    string(REPLACE "~rb~" "]" from "${from}")
    string(REPLACE "~sc~" ";" from "${from}")
    string(REPLACE "~n~" "\n" to "${to}")
    string(REPLACE "~lb~" "[" to "${to}")
    string(REPLACE "~rb~" "]" to "${to}")
    string(REPLACE "~sc~" ";" to "${to}")

    # A `-git` suffix stages a real git index as well, so the git enumeration path is
    # exercised rather than assumed. Both modes run the same cases where it matters.
    set(useGit FALSE)
    if(target MATCHES "^(.*)-git$")
        set(target "${CMAKE_MATCH_1}")
        set(useGit TRUE)
    endif()

    set(cleanText "${baseCleanSource}")
    set(documentedText "${baseDocumentedSource}")
    set(helperText "${baseHelperHeader}")
    set(extraPath "")
    set(extraText "")
    set(writeSources TRUE)
    set(applied TRUE)

    if(target STREQUAL "none")
        # Nothing to apply.
    elseif(target STREQUAL "newfile")
        # `from` is the path, `to` is the content. Always applies -- there is nothing
        # to find -- so the landed-check below is satisfied by construction and the
        # row's needles carry the whole assertion.
        set(extraPath "${from}")
        set(extraText "${to}")
    elseif(target STREQUAL "documented")
        string(FIND "${documentedText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" documentedText "${documentedText}")
        endif()
    elseif(target STREQUAL "helper")
        string(FIND "${helperText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" helperText "${helperText}")
        endif()
    elseif(target STREQUAL "allmentions")
        # Every prose site at once. Stripping only ONE leaves the other standing, the
        # mention count stays non-zero, and the matched-nothing guard correctly does
        # not fire -- so a mutation that hits a single file tests nothing and passes
        # by never reaching the arm it names.
        set(hitCount 0)
        foreach(textVar documentedText helperText)
            string(FIND "${${textVar}}" "${from}" position)
            if(NOT position EQUAL -1)
                math(EXPR hitCount "${hitCount} + 1")
                string(REPLACE "${from}" "${to}" ${textVar} "${${textVar}}")
            endif()
        endforeach()
        if(hitCount LESS 2)
            set(applied FALSE)
        endif()
    elseif(target STREQUAL "nosources")
        set(writeSources FALSE)
    else()
        message(FATAL_ERROR "unknown mutation target `${target}` in case `${name}`")
    endif()

    if(writeSources)
        file(WRITE "${tree}/src/FastCache/Core/Clean.cpp" "${cleanText}")
        file(WRITE "${tree}/src/FastCache/Core/Documented.cpp" "${documentedText}")
        file(WRITE "${tree}/src/apps/fastcache-cc/FileBytes.hpp" "${helperText}")
    else()
        # A tree with a src/ directory and no sources in it, which is what a moved
        # source root looks like from here.
        file(MAKE_DIRECTORY "${tree}/src")
        file(WRITE "${tree}/README.md" "not a source\n")
    endif()

    if(NOT extraPath STREQUAL "")
        file(WRITE "${tree}/${extraPath}" "${extraText}")
    endif()

    if(useGit)
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "case `${name}` asks for the git enumeration path and no git was found. "
                "Skipping it silently would leave the mode CI actually takes untested, "
                "which is the defect this fixture exists to avoid")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}"
                        OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE initStatus)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A
                        OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE addStatus)
        if(NOT initStatus EQUAL 0 OR NOT addStatus EQUAL 0)
            message(FATAL_ERROR
                "case `${name}` could not stage a git index (init=${initStatus} add=${addStatus}), "
                "so it would silently have tested the directory walk instead")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors)
    set(combined "${captured}${capturedErrors}")

    # `message(FATAL_ERROR)` word-wraps at a column that depends on how long the
    # scratch path is, so a multi-word needle can break across a line on one machine
    # and not another. Collapsing whitespace makes the needles portable -- and it
    # matters most for the NEGATIVE assertions, where a wrapped line makes a
    # `must not appear` needle pass for free.
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")

    set(${outOutput} "${combined}" PARENT_SCOPE)
    set(${outApplied} "${applied}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases.
#
#   <name>|<target>|<from>|<to>|<must ALL appear>|<must NONE appear>
#
# Needle fields are ' && '-separated; `-` means none. The field count is asserted per
# row below, because a row that does not parse runs as a different case than it reads
# as.
set(FastCachedIstreambufCases
    # The baseline, in BOTH enumeration modes. Every refusal below is evidence only
    # if these pass -- and asserting the mode is what stops every case silently
    # testing the walk while CI takes the git path.
    "baseline via walk|none|-|-|all prose, none constructing && directory walk (no git index)|CMake Error"
    "baseline via git|none-git|-|-|all prose, none constructing && git ls-files|CMake Error"

    # THE RED ARM: a use planted in a file that carries none, named by file and line.
    "planted use|newfile|src/FastCache/Core/Fresh.cpp|#include <vector>~n~#include <fstream>~n~std::vector<char> Read(std::ifstream& in)~n~{~n~    return { std::istreambuf_iterator<char> { in }, {} }~sc~~n~}~n~|src/FastCache/Core/Fresh.cpp:5: constructs from|-"

    # ...and in git mode too, since that is the enumeration CI uses.
    "planted use via git|newfile-git|src/FastCache/Core/Fresh.cpp|#include <vector>~n~std::vector<char> Read(std::ifstream& in)~n~{~n~    return { std::istreambuf_iterator<char> { in }, {} }~sc~~n~}~n~|src/FastCache/Core/Fresh.cpp:4: constructs from && git ls-files|-"

    # THE QUIET ARM, three comment shapes. This is the direction that gets a check
    # deleted rather than obeyed, and the real tree's seven prose sites are all of
    # these shapes.
    "line comment is not a use|newfile|src/FastCache/Core/Note.cpp|// Never std::istreambuf_iterator here.~n~int Note() { return 0~sc~ }~n~|all prose, none constructing|CMake Error"
    "doc comment is not a use|newfile|src/FastCache/Core/Note.cpp|/// Deliberately NOT through `std::istreambuf_iterator`.~n~int Note() { return 0~sc~ }~n~|all prose, none constructing|CMake Error"
    "block comment is not a use|newfile|src/FastCache/Core/Note.cpp|/*~n~ A paragraph mentioning std::istreambuf_iterator with no leading stars.~n~*/~n~int Note() { return 0~sc~ }~n~|all prose, none constructing|CMake Error"

    # THE PARTIAL-FIX SHAPE: a file that explains the rule AND breaks it. Refused --
    # a prose mention must not excuse the file it sits in.
    "prose and a use in one file|newfile|src/FastCache/Core/Both.cpp|// Never std::istreambuf_iterator: GCC reports null-dereference.~n~#include <vector>~n~std::vector<char> Read(std::ifstream& in)~n~{~n~    return { std::istreambuf_iterator<char> { in }, {} }~sc~~n~}~n~|src/FastCache/Core/Both.cpp:5: constructs from|-"

    # LINE NUMBERS, with the list structure that makes them drift planted above the
    # violation. A filename-only assertion passes under that bug.
    "line number survives brackets and semicolons|newfile|src/FastCache/Core/Drift.cpp|#include <array>~n~int a~lb~4~rb~ = {1, 2, 3, 4}~sc~~n~auto b = a~lb~0~rb~~sc~ auto c = a~lb~1~rb~~sc~~n~// a trailing backslash \\~n~std::vector<char> Read(std::ifstream& in) { return { std::istreambuf_iterator<char> { in }, {} }~sc~ }~n~|src/FastCache/Core/Drift.cpp:5: constructs from|-"

    # --- fails CLOSED ---

    # The token gone from the tree entirely. Not a clean tree: this repository
    # documents the ban in prose, so zero occurrences means the scan broke.
    "no mention anywhere|allmentions|istreambuf_iterator|the character iterator|the scan matched nothing|-"

    # No C++ under src/ at all -- a moved source root.
    "no sources at all|nosources|-|-|matched no C++ sources|-"
)

# ---------------------------------------------------------------------------
set(failures "")
set(caseCount 0)

foreach(row IN LISTS FastCachedIstreambufCases)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 6)
        message(FATAL_ERROR
            "case row has ${fieldCount} fields, expected 6 -- a row that does not parse "
            "would run as a different case than it reads as: [${row}]")
    endif()
    list(GET fields 0 caseName)
    list(GET fields 1 caseTarget)
    list(GET fields 2 caseFrom)
    list(GET fields 3 caseTo)
    list(GET fields 4 caseMustAppear)
    list(GET fields 5 caseMustNotAppear)

    math(EXPR caseCount "${caseCount} + 1")

    fastcached_stage_and_run("${caseCount}" "${caseTarget}" "${caseFrom}" "${caseTo}"
                             output applied)

    if(NOT applied)
        list(APPEND failures
             "${caseName}: the mutation did not apply -- `${caseFrom}` was not found in the staged tree, so this case asserted nothing")
        continue()
    endif()

    if(NOT caseMustAppear STREQUAL "-")
        string(REPLACE " && " ";" needles "${caseMustAppear}")
        foreach(needle IN LISTS needles)
            string(FIND "${output}" "${needle}" position)
            if(position EQUAL -1)
                list(APPEND failures "${caseName}: expected to see `${needle}` and did not")
            endif()
        endforeach()
    endif()

    if(NOT caseMustNotAppear STREQUAL "-")
        string(REPLACE " && " ";" needles "${caseMustNotAppear}")
        foreach(needle IN LISTS needles)
            string(FIND "${output}" "${needle}" position)
            if(NOT position EQUAL -1)
                list(APPEND failures "${caseName}: did not expect `${needle}` and saw it")
            endif()
        endforeach()
    endif()

    # The verdict itself, separate from the needles, so a needle appearing inside a
    # DIFFERENT failure's text cannot stand in for it.
    string(FIND "${output}" "CMake Error" errorPosition)
    if(caseMustNotAppear MATCHES "CMake Error")
        if(NOT errorPosition EQUAL -1)
            list(APPEND failures "${caseName}: expected the check to pass and it refused")
        endif()
    else()
        if(errorPosition EQUAL -1)
            list(APPEND failures "${caseName}: expected the check to refuse and it passed")
        endif()
    endif()
endforeach()

if(caseCount EQUAL 0)
    message(FATAL_ERROR
        "the case table is empty, so this fixture asserted nothing -- which is exactly "
        "what a green run with no cases looks like")
endif()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`istreambuf-iterator` did not behave as its own documentation says.")
    message("A check nobody has watched refuse is not a check -- and one nobody has")
    message("watched stay QUIET over prose is one that gets deleted.")
    message(FATAL_ERROR
        "istreambuf-iterator-selftest: ${failureCount} of ${caseCount} case(s) disagreed with the check")
endif()

message(STATUS "istreambuf-iterator-selftest: ${caseCount} case(s), every verdict as documented")
