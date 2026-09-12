# SPDX-License-Identifier: Apache-2.0
#
# `markup-entities` must be SEEN to refuse a second escaper, and seen to ACCEPT the
# comments and assertions that legitimately spell the same bytes.
#
# The accepting arm is the load-bearing one here. This check's whole subject is a
# handful of string literals that appear, correctly, in a comment explaining the rule,
# in four test files asserting what the escaper emits, and in one page of hand-escaped
# documentation prose. A version that refused those would be satisfiable only by
# deleting a comment, an assertion or a paragraph — so it would be deleted instead,
# which is the failure mode rather than a cosmetic complaint.
#
# ## Two defects this fixture's subject had before it existed
#
# Both were caught by the check's own fail-closed guards rather than by review, and
# both are cases below so they cannot come back:
#
#  * `&lt;` and `&gt;` each contain a `;`, which is CMake's list separator. A list of
#    the full entity spellings is therefore silently ragged, and `string(FIND "$line"
#    "")` matches at position 0 on every line. The check holds the BODIES and rebuilds
#    `&<body>;` at the point of use.
#  * the table is indented inside `Detail`, so a home signature anchored at column 0
#    found nothing and reported it as *the table was renamed or removed*.
#
# ## The mode is part of the output, and asserted on both sides
#
# A synthetic tree is not a git repository, so every case would exercise the DIRECTORY
# WALK while CI exercises GIT — which is how a sibling check came to have six green
# self-test cases and a red CI run with no contradiction between them. The git path is
# staged for real with `git init` plus `git add`; an index is all `git ls-files` needs.
#
# Mutations are applied to a SYNTHESISED tree, never to the tree under test, and each
# case asserts its mutation LANDED before any verdict is drawn from it. The BASELINE is
# load-bearing: every refusal below is evidence only if the unmutated tree passes.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-markup-entities-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-markup-entities.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

# Placeholders, because an entity spelling cannot appear literally in a case row: `&lt;`
# and `&gt;` contain `;`, which splits the CMake list, and `|` is the field separator.
# That is the same hazard the check itself had, which is why the rows below spell the
# entities through here rather than directly.
function(fastcached_expand var)
    set(text "${${var}}")
    string(REPLACE "~n~" "\n" text "${text}")
    string(REPLACE "~lb~" "[" text "${text}")
    string(REPLACE "~rb~" "]" text "${text}")
    string(REPLACE "~bar~" "|" text "${text}")
    string(REPLACE "~AMP~" "&amp@" text "${text}")
    string(REPLACE "~LT~" "&lt@" text "${text}")
    string(REPLACE "~GT~" "&gt@" text "${text}")
    string(REPLACE "~QUOT~" "&quot@" text "${text}")
    string(REPLACE "~A39~" "&#39@" text "${text}")
    string(REPLACE "~APOS~" "&apos@" text "${text}")
    # `@` stands in for the entity's own `;` right up to the last moment, so no list
    # operation above ever sees one.
    string(REPLACE "@" ";" text "${text}")
    string(REPLACE "~sc~" ";" text "${text}")
    set(${var} "${text}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The synthesised ground truth, mirroring the real tree's shape: one home holding the
# table, one production consumer that CALLS it, one test that asserts what it emits,
# and one page of hand-escaped documentation prose.
set(baseHome
"// SPDX-License-Identifier: Apache-2.0
#pragma once
namespace FastCache
{
namespace Detail
{
    inline constexpr std::array MarkupEscapes {
        TextEscape { .byte = '&', .spelling = \"~AMP~\" },
        TextEscape { .byte = '<', .spelling = \"~LT~\" },
        TextEscape { .byte = '>', .spelling = \"~GT~\" },
        TextEscape { .byte = '\\\"', .spelling = \"~QUOT~\" },
        TextEscape { .byte = '\\\\'', .spelling = \"~A39~\" },
    };
} // namespace Detail
inline std::string EscapeMarkup(std::string_view text) { return std::string { text }; }
} // namespace FastCache
")

set(baseConsumer
"// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Markup.hpp>
namespace FastCache::Distributed
{
// An ampersand becomes ~AMP~, so a label a peer chose cannot break the document.
std::string Row(std::string_view label)
{
    return EscapeMarkup(label);
}
} // namespace FastCache::Distributed
")

set(baseTest
"// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Markup.hpp>
TEST_CASE(\"markup escapes what it must\")
{
    CHECK(EscapeMarkup(\"a&b\") == \"a~AMP~b\");
    CHECK(EscapeMarkup(\"<x>\") == \"~LT~x~GT~\");
}
")

set(baseProse
"// SPDX-License-Identifier: Apache-2.0
#include \"AdminEndpoint.hpp\"
namespace FastCache::Node
{
void Help(std::string& out)
{
    out += \"<code>Authorization: Bearer ~LT~token~GT~</code>\"~sc~
}
} // namespace FastCache::Node
")

# ---------------------------------------------------------------------------
function(fastcached_stage_and_run name target from to outOutput outApplied)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")

    fastcached_expand(from)
    fastcached_expand(to)

    set(useGit FALSE)
    if(target MATCHES "^(.*)-git$")
        set(target "${CMAKE_MATCH_1}")
        set(useGit TRUE)
    endif()

    set(homeText "${baseHome}")
    set(consumerText "${baseConsumer}")
    set(testText "${baseTest}")
    set(proseText "${baseProse}")
    foreach(v homeText consumerText testText proseText)
        fastcached_expand(${v})
    endforeach()

    set(extraPath "")
    set(extraText "")
    set(writeSources TRUE)
    set(writeProse TRUE)
    set(applied TRUE)

    if(target STREQUAL "none")
    elseif(target STREQUAL "newfile")
        set(extraPath "${from}")
        set(extraText "${to}")
    elseif(target STREQUAL "home")
        string(FIND "${homeText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" homeText "${homeText}")
        endif()
    elseif(target STREQUAL "test")
        string(FIND "${testText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" testText "${testText}")
        endif()
    elseif(target STREQUAL "noprose")
        # The exemption's subject removed, so the row describes nothing.
        set(writeProse FALSE)
    elseif(target STREQUAL "nosources")
        set(writeSources FALSE)
        set(writeProse FALSE)
    else()
        message(FATAL_ERROR "unknown mutation target `${target}` in case `${name}`")
    endif()

    if(writeSources)
        file(WRITE "${tree}/src/FastCache/Core/Markup.hpp" "${homeText}")
        file(WRITE "${tree}/src/FastCache/Distributed/FleetView.cpp" "${consumerText}")
        file(WRITE "${tree}/src/FastCache/Distributed/FleetText_test.cpp" "${testText}")
    else()
        file(MAKE_DIRECTORY "${tree}/src")
        file(WRITE "${tree}/README.md" "not a source\n")
    endif()
    if(writeProse)
        file(WRITE "${tree}/src/apps/fastcache-compile-node/AdminEndpoint.cpp" "${proseText}")
    endif()

    if(NOT extraPath STREQUAL "")
        file(WRITE "${tree}/${extraPath}" "${extraText}")
    endif()

    if(useGit)
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "case `${name}` asks for the git enumeration path and no git was found. "
                "Skipping it silently would leave the mode CI actually takes untested.")
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

    # `message(FATAL_ERROR)` word-wraps at a column that depends on the scratch path's
    # length, so a multi-word needle can break across a line on one machine and not
    # another. Collapsing whitespace matters most for the NEGATIVE assertions, where a
    # wrapped line makes a `must not appear` needle pass for free.
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")

    set(${outOutput} "${combined}" PARENT_SCOPE)
    set(${outApplied} "${applied}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
#   <name>|<target>|<from>|<to>|<must ALL appear>|<must NONE appear>
set(FastCachedMarkupCases
    # THE ACCEPTING ARM, both enumeration modes.
    "baseline via walk|none|-|-|no duplicate escaper across && directory walk (no git index)|CMake Error"
    "baseline via git|none-git|-|-|no duplicate escaper across && git ls-files|CMake Error"

    # THE RED ARM: a second escaper, named by file and line.
    "hand-rolled escaper|newfile|src/FastCache/Platform/ServiceControl.cpp|std::string XmlEscape(std::string_view t)~n~{~n~    std::string o~sc~~n~    for (char c : t) { if (c == '&') o += \"~AMP~\"~sc~ }~n~    return o~sc~~n~}~n~|src/FastCache/Platform/ServiceControl.cpp:4|-"
    "hand-rolled escaper via git|newfile-git|src/FastCache/Platform/ServiceControl.cpp|std::string XmlEscape(std::string_view t)~n~{~n~    return t == \"<\" ? \"~LT~\" : \"\"~sc~~n~}~n~|src/FastCache/Platform/ServiceControl.cpp:3 && git ls-files|-"

    # THE RETIRED SPELLING, refused BY NAME rather than as an ordinary duplicate.
    "the retired entity returns|newfile|src/FastCache/Platform/ServiceControl.cpp|std::string Q() { return \"~APOS~\"~sc~ }~n~|spell the retired && src/FastCache/Platform/ServiceControl.cpp:1|-"

    # THE ACCEPTING ARM PROPER. Each of these is a real shape from the tree, and each
    # differs from a violation only by context.
    "a comment is not a use|newfile|src/FastCache/Distributed/FleetChart.cpp|// The label is escaped, so ~AMP~ and ~LT~ cannot break the SVG.~n~int Chart() { return 0~sc~ }~n~|no duplicate escaper across|CMake Error"
    "a block comment is not a use|newfile|src/FastCache/Distributed/FleetChart.cpp|/*~n~ A paragraph naming ~AMP~ and ~QUOT~ with no leading stars.~n~*/~n~int Chart() { return 0~sc~ }~n~|no duplicate escaper across|CMake Error"

    # COMMENT ORDERING, the same false green this check carried: a line comment
    # NAMING a block opener opened one that never closed, and the rest of the file
    # went unread behind a clean count.
    "a line comment naming a block opener does not open one|newfile|src/FastCache/Platform/Order.cpp|// Call EscapeMarkup rather than ~AMP~, and not inside a /* block either.~n~std::string E() { return \"~AMP~\"~sc~ }~n~|src/FastCache/Platform/Order.cpp:2|-"

    # ...and the CONTROL: the `//` inside the open block stays inside it (line 2 is
    # never named) and the file RESUMES after `*/` (line 4 is).
    "a block comment survives a line marker inside it|newfile|src/FastCache/Platform/Order.cpp|/*~n~ Prose with a // in it, naming ~AMP~ to argue against writing it.~n~*/~n~std::string E() { return \"~AMP~\"~sc~ }~n~|src/FastCache/Platform/Order.cpp:4|Order.cpp:2"
    "a test may assert the output|newfile|src/FastCache/Distributed/FleetView_test.cpp|CHECK(EscapeMarkup(\"&\") == \"~AMP~\")~sc~~n~|no duplicate escaper across|CMake Error"

    # THE PARTIAL-FIX SHAPE: a file that explains the rule AND breaks it.
    "prose and a use in one file|newfile|src/FastCache/Platform/Both.cpp|// Always call EscapeMarkup rather than writing ~AMP~ by hand.~n~std::string E() { return \"~AMP~\"~sc~ }~n~|src/FastCache/Platform/Both.cpp:2|-"

    # LINE NUMBERS, with the list structure that makes them drift planted above.
    "line number survives brackets|newfile|src/FastCache/Platform/Drift.cpp|int a~lb~4~rb~ = {1,2,3,4}~sc~~n~auto b = a~lb~0~rb~~sc~~n~// a trailing backslash \\\\~n~std::string E() { return \"~AMP~\"~sc~ }~n~|src/FastCache/Platform/Drift.cpp:4|-"

    # --- fails CLOSED ---

    # The home renamed: the rule has no subject.
    "no home at all|home|inline constexpr std::array MarkupEscapes|inline constexpr std::array MarkupRows|expected exactly one markup table, found 0|-"

    # The home spells nothing this scan recognises -- the positive control ON the check.
    # This is the case the `;`-in-the-list defect would have shown up as.
    "home spells no entity|home|TextEscape { .byte = '&', .spelling = \"~AMP~\" },~n~        TextEscape { .byte = '<', .spelling = \"~LT~\" },~n~        TextEscape { .byte = '>', .spelling = \"~GT~\" },~n~        TextEscape { .byte = '\\\"', .spelling = \"~QUOT~\" },~n~        TextEscape { .byte = '\\\\'', .spelling = \"~A39~\" },|TextEscape { .byte = '&', .spelling = Amp },|the home spells no entity|-"

    # No test spells one: comment stripping or the suffix rule has eaten the corpus.
    "no test spells an entity|test|CHECK(EscapeMarkup(\"a&b\") == \"a~AMP~b\")~sc~~n~    CHECK(EscapeMarkup(\"<x>\") == \"~LT~x~GT~\")~sc~|CHECK(true)~sc~|no test spells an entity|-"

    # A STALE exemption: the row is there and its subject is not.
    "stale exemption|noprose|-|-|is exempted and needs no exemption|-"

    # No C++ under src/ at all.
    "no sources at all|nosources|-|-|matched no C++ sources|-"
)

# ---------------------------------------------------------------------------
set(failures "")
set(caseCount 0)

foreach(row IN LISTS FastCachedMarkupCases)
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
    fastcached_stage_and_run("${caseCount}" "${caseTarget}" "${caseFrom}" "${caseTo}" output applied)

    if(NOT applied)
        list(APPEND failures
             "${caseName}: the mutation did not apply -- its `from` text was not found in the staged tree, so this case asserted nothing")
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

    # The verdict, separate from the needles, so a needle appearing inside a DIFFERENT
    # failure's text cannot stand in for it. `CMake Error|CMake Warning`, never the
    # error word alone: a sub-run that merely WARNS changes meaning silently and, read
    # for the error word alone, is scored a clean pass (#672).
    set(sawSignal FALSE)
    if(output MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    # verdict-error-only: a read of the CASE TABLE, not of the sub-run.
    if(caseMustNotAppear MATCHES "CMake Error")
        if(sawSignal)
            list(APPEND failures "${caseName}: expected the check to pass and it refused")
        endif()
    else()
        if(NOT sawSignal)
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
    message("`markup-entities` did not behave as its own documentation says.")
    message("A check nobody has watched refuse is not a check -- and one nobody has")
    message("watched ACCEPT a comment or a test assertion is one that gets deleted,")
    message("because satisfying it would mean deleting the assertion.")
    # FINDINGS against CASES, and they are named separately because one case can
    # contribute several -- a missing needle, an unexpected needle and a wrong
    # verdict are three. Written as "N of M" this printed `22 of 15`, a number
    # nobody can explain, which is the tell for a count that overstates.
    message(FATAL_ERROR
        "markup-entities-selftest: ${failureCount} finding(s) across ${caseCount} case(s)")
endif()

message(STATUS "markup-entities-selftest: ${caseCount} case(s), every verdict as documented")
