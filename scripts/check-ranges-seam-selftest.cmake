# SPDX-License-Identifier: Apache-2.0
#
# `ranges-seam` must be SEEN to refuse a direct call, seen to stay QUIET over everything that
# is not one, and seen to refuse every way of not being able to conclude.
#
# ## What each group of cases is for
#
# - THE RED ARM names file AND line and the seam spelling to use instead, in both enumeration
#   modes. A check that refused without saying what to write is half a remedy.
# - THE QUIET ARM is what decides whether a check survives. Prose about the rule, the C++20
#   facilities whose names share a prefix (`std::views::iota`, `std::ranges::iota_view`), the
#   seam spelling itself, and the feature-test macro all appear in a healthy tree.
# - DERIVATION is asserted by adding a selection block to the staged header and planting a
#   call to the facility it wraps: refused, naming the new seam spelling, with no edit to the
#   check. A check that restated its rows would pass every other case here and fail this one.
# - THE EXEMPTION is exact: a file that merely shares the seam test's NAME elsewhere is refused.
# - THIRD-PARTY ROOTS decline a vendored direct call, and the same file is refused once the
#   roots stop naming its directory -- so the decline is the roots file's doing, not an accident
#   of the walk.
# - FAILS CLOSED: a header whose shape the check cannot read, a missing header, a missing test,
#   a seam the enumeration did not list, and a tree with no first-party C++ at all.
#
# The enumeration MODE is part of the output and asserted on both sides, and the git path is
# staged for real (`git init` + `git add`), because a synthetic tree is not a git repository and
# every case would otherwise exercise the directory walk while CI exercises git.
#
# Mutations are applied to a SYNTHESISED tree, never to the tree under test, and each case
# asserts its mutation LANDED before any verdict is drawn from it. The baselines are
# load-bearing: every refusal below is evidence only if the unmutated tree passes.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check reports
# failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         [-DGIT_EXECUTABLE=<git>] -P scripts/check-ranges-seam-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-ranges-seam.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

# ---------------------------------------------------------------------------
# The synthesised ground truth. The seam header carries the two selection blocks in the
# shape the real one does; its test names the standard facilities, as the real one must; one
# source is clean, one explains the rule in prose, and a vendored copy calls a facility
# directly and sits under a third-party root.
set(baseSeamHeader
"// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <version>

namespace FastCache::Ranges
{
// Where the macro is defined this is std::ranges::iota itself.
#if defined(__cpp_lib_ranges_iota) && !defined(FC_RANGES_FORCE_FALLBACK)
inline constexpr auto Iota = std::ranges::iota;
#else
inline constexpr auto Iota = Detail::IotaFallback;
#endif

#if defined(__cpp_lib_ranges_fold) && !defined(FC_RANGES_FORCE_FALLBACK)
inline constexpr auto FoldLeft = std::ranges::fold_left;
#else
inline constexpr auto FoldLeft = Detail::FoldLeftFallback;
#endif
} // namespace FastCache::Ranges
")

set(baseSeamTest
"// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Ranges.hpp>

void Agree(std::vector<int>& a, std::vector<int>& b)
{
    FastCache::Ranges::Detail::IotaFallback(a, 0);
    std::ranges::iota(b, 0);
    (void) std::ranges::fold_left(b, 0, std::plus {});
}
")

set(baseCleanSource
"// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Ranges.hpp>

namespace FastCache
{
std::size_t Total(std::array<std::size_t, 3> const& swept)
{
    return Ranges::FoldLeft(swept, std::size_t { 0 }, std::plus {});
}
} // namespace FastCache
")

set(baseDocumentedSource
"// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Ranges.hpp>

namespace FastCache
{
void Number(std::vector<std::size_t>& order)
{
    // Ranges::Iota rather than std::ranges::iota: Apple's libc++ has no std::ranges::iota.
    Ranges::Iota(order, std::size_t { 0 });
}
} // namespace FastCache
")

set(baseVendoredSource
"// A third-party copy: not ours to edit.
void Upstream(std::vector<int>& v)
{
    std::ranges::iota(v, 1);
}
")

set(baseRoots "# planted\nvendor/upstream\n")

# ---------------------------------------------------------------------------
# Stage a tree, apply one mutation, run the check, return its collapsed output.
function(fastcached_stage_and_run name target from to outOutput outApplied)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")

    # `~n~`, `~lb~`, `~rb~` and `~sc~` are a newline, `[`, `]` and `;`, none of which can
    # appear literally in a case row: `;` splits the CMake list and an unbalanced `[` merges
    # every following row into one element.
    foreach(field from to)
        string(REPLACE "~n~" "\n" ${field} "${${field}}")
        string(REPLACE "~lb~" "[" ${field} "${${field}}")
        string(REPLACE "~rb~" "]" ${field} "${${field}}")
        string(REPLACE "~sc~" ";" ${field} "${${field}}")
    endforeach()

    # A `-git` suffix stages a real git index, so the enumeration CI uses is exercised rather
    # than assumed.
    set(useGit FALSE)
    if(target MATCHES "^(.*)-git$")
        set(target "${CMAKE_MATCH_1}")
        set(useGit TRUE)
    endif()

    set(seamHeaderText "${baseSeamHeader}")
    set(seamTestText "${baseSeamTest}")
    set(rootsText "${baseRoots}")
    set(extraPath "")
    set(extraText "")
    set(writeSeamHeader TRUE)
    set(writeSeamTest TRUE)
    set(untrackSeamTest FALSE)
    set(applied TRUE)

    if(target STREQUAL "none")
        # Nothing to apply.
    elseif(target STREQUAL "newfile")
        # `from` is the path, `to` is the content.
        set(extraPath "${from}")
        set(extraText "${to}")
    elseif(target STREQUAL "seam")
        string(FIND "${seamHeaderText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" seamHeaderText "${seamHeaderText}")
        endif()
    elseif(target MATCHES "^seamrow:(.*)$")
        # A selection block added to the header AND a direct call planted, in one case: the
        # derivation is only proven by the pair. The path is in the target, `from` is the
        # block and `to` is the planted file's content.
        set(extraPath "${CMAKE_MATCH_1}")
        set(extraText "${to}")
        string(FIND "${seamHeaderText}" "} // namespace FastCache::Ranges" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "} // namespace FastCache::Ranges" "${from}\n} // namespace FastCache::Ranges"
                   seamHeaderText "${seamHeaderText}")
        endif()
    elseif(target STREQUAL "roots")
        string(FIND "${rootsText}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" rootsText "${rootsText}")
        endif()
    elseif(target STREQUAL "noseamheader")
        set(writeSeamHeader FALSE)
    elseif(target STREQUAL "noseamtest")
        set(writeSeamTest FALSE)
    elseif(target STREQUAL "untrackseamtest")
        set(untrackSeamTest TRUE)
        if(NOT useGit)
            message(FATAL_ERROR "case `${name}`: untracking needs an index, so the target must end in -git")
        endif()
    else()
        message(FATAL_ERROR "unknown mutation target `${target}` in case `${name}`")
    endif()

    if(writeSeamHeader)
        file(WRITE "${tree}/src/FastCache/Core/Ranges.hpp" "${seamHeaderText}")
    endif()
    if(writeSeamTest)
        file(WRITE "${tree}/src/FastCache/Core/Ranges_test.cpp" "${seamTestText}")
    endif()
    file(WRITE "${tree}/src/FastCache/Core/Clean.cpp" "${baseCleanSource}")
    file(WRITE "${tree}/src/apps/fastcache-cli/Documented.cpp" "${baseDocumentedSource}")
    file(WRITE "${tree}/vendor/upstream/src/Upstream.cpp" "${baseVendoredSource}")
    file(WRITE "${tree}/scripts/lib/third-party-roots.txt" "${rootsText}")
    if(NOT extraPath STREQUAL "")
        file(WRITE "${tree}/${extraPath}" "${extraText}")
    endif()

    if(useGit)
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "case `${name}` asks for the git enumeration path and no git was found. "
                "Skipping it silently would leave the mode CI actually takes untested")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}"
                        OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE initStatus)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A
                        OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE addStatus)
        set(rmStatus 0)
        if(untrackSeamTest)
            execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" rm -q --cached src/FastCache/Core/Ranges_test.cpp
                            OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE rmStatus)
        endif()
        if(NOT initStatus EQUAL 0 OR NOT addStatus EQUAL 0 OR NOT rmStatus EQUAL 0)
            message(FATAL_ERROR
                "case `${name}` could not stage its git index (init=${initStatus} add=${addStatus} rm=${rmStatus}), "
                "so it would silently have tested something else")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors)
    set(combined "${captured}${capturedErrors}")

    # `message(FATAL_ERROR)` word-wraps at a column that depends on the scratch path's length,
    # so whitespace is collapsed before any needle is looked for -- most of all for the
    # NEGATIVE needles, which a wrapped line would satisfy for free.
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")

    set(${outOutput} "${combined}" PARENT_SCOPE)
    set(${outApplied} "${applied}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases.
#
#   <name>|<target>|<from>|<to>|<must ALL appear>|<must NONE appear>
#
# Needle fields are ' && '-separated; `-` means none. `CMake Error` in the last field means the
# case expects the check to PASS. The field count is asserted per row below.
set(FastCachedRangesSeamCases
    # The baselines, in both enumeration modes. The vendored direct call is DECLINED here.
    "baseline via walk|none|-|-|none called directly && directory walk (no git index) && 1 declined under third-party roots && std::ranges::iota -> Ranges::Iota && std::ranges::fold_left -> Ranges::FoldLeft|CMake Error"
    "baseline via git|none-git|-|-|none called directly && git ls-files && 1 declined under third-party roots|CMake Error"

    # --- THE RED ARM ---
    "planted iota|newfile|src/FastCache/Core/Fresh.cpp|#include <vector>~n~void Fresh(std::vector<int>& v)~n~{~n~    std::ranges::iota(v, 0)~sc~~n~}~n~|src/FastCache/Core/Fresh.cpp:4: calls `std::ranges::iota` directly -- write `Ranges::Iota` (gated by __cpp_lib_ranges_iota) && 1 site(s) call a wrapped range algorithm directly|-"
    "planted fold_left via git|newfile-git|src/apps/fastcache-compile-node/Fresh.cpp|int Fresh(std::array<int, 3> const& a)~n~{~n~    return std::ranges::fold_left(a, 0, std::plus {})~sc~~n~}~n~|src/apps/fastcache-compile-node/Fresh.cpp:3: calls `std::ranges::fold_left` directly -- write `Ranges::FoldLeft` && via git ls-files|-"
    "bare ranges qualifier under using namespace std|newfile|src/FastCache/Core/Bare.cpp|using namespace std~sc~~n~void Bare(vector<int>& v) { ranges::iota(v, 0)~sc~ }~n~|src/FastCache/Core/Bare.cpp:2: calls `std::ranges::iota` directly|-"
    "a test is first-party too|newfile|src/FastCache/Cache/Fresh_test.cpp|TEST_CASE(\"x\", \"~lb~cache~rb~\")~n~{~n~    std::vector<int> order(3)~sc~~n~    std::ranges::iota(order, 0)~sc~~n~}~n~|src/FastCache/Cache/Fresh_test.cpp:4: calls|-"
    "a header extension other than hpp|newfile|src/apps/fastcache-bench/Inline.inl|inline int Sum(std::span<int const> s) { return std::ranges::fold_left(s, 0, std::plus {})~sc~ }~n~|src/apps/fastcache-bench/Inline.inl:1: calls `std::ranges::fold_left`|-"
    "two calls on two lines are both named|newfile|src/FastCache/Core/Two.cpp|void Two(std::vector<int>& v)~n~{~n~    std::ranges::iota(v, 0)~sc~~n~    (void) std::ranges::fold_left(v, 0, std::plus {})~sc~~n~}~n~|Two.cpp:3: calls `std::ranges::iota` && Two.cpp:4: calls `std::ranges::fold_left` && 2 site(s)|-"
    "line number survives brackets semicolons and a trailing backslash|newfile|src/FastCache/Core/Drift.cpp|int a~lb~4~rb~ = {1, 2, 3, 4}~sc~~n~auto b = a~lb~0~rb~~sc~ auto c = a~lb~1~rb~~sc~~n~// a trailing backslash \\~n~void Drift(std::vector<int>& v) { std::ranges::iota(v, 0)~sc~ }~n~|src/FastCache/Core/Drift.cpp:4: calls|-"

    # --- THE QUIET ARM ---
    "C++20 names sharing the prefix are not wrapped facilities|newfile|src/FastCache/Core/Views.cpp|auto Indices(int n) { return std::views::iota(0, n)~sc~ }~n~using Span = std::ranges::iota_view<int, int>~sc~~n~auto Tail = std::ranges::fold_left_first~sc~~n~|none called directly|CMake Error"
    "the seam spelling and the macro are not direct calls|newfile|src/FastCache/Core/Seam.cpp|#if defined(__cpp_lib_ranges_iota)~n~#endif~n~void Seam(std::vector<int>& v) { FastCache::Ranges::Iota(v, 0)~sc~ (void) Ranges::FoldLeft(v, 0, std::plus {})~sc~ }~n~|none called directly|CMake Error"
    "comments are not calls|newfile|src/FastCache/Core/Note.cpp|// Never std::ranges::iota(v, 0) here.~n~/// Nor `std::ranges::fold_left(v, 0, op)`.~n~/*~n~ std::ranges::iota(v, 0) is missing from Apple's libc++.~n~*/~n~int Note() { return 0~sc~ }~n~|none called directly|CMake Error"

    # --- DERIVATION: a block added to the header is enforced with no edit to the check ---
    "a selection block added to the header is enforced|seamrow:src/FastCache/Core/Right.cpp|#if defined(__cpp_lib_ranges_fold) && !defined(FC_RANGES_FORCE_FALLBACK)~n~inline constexpr auto FoldRight = std::ranges::fold_right~sc~~n~#else~n~inline constexpr auto FoldRight = Detail::FoldRightFallback~sc~~n~#endif|int Right(std::vector<int> const& v) { return std::ranges::fold_right(v, 0, std::minus {})~sc~ }~n~|Right.cpp:1: calls `std::ranges::fold_right` directly -- write `Ranges::FoldRight` && std::ranges::fold_right -> Ranges::FoldRight|-"
    # ...and its CONTROL: the same call with no block in the header is not this check's business.
    "a facility the header does not wrap is not refused|newfile|src/FastCache/Core/Right.cpp|int Right(std::vector<int> const& v) { return std::ranges::fold_right(v, 0, std::minus {})~sc~ }~n~|none called directly|CMake Error"

    # --- THE EXEMPTION IS EXACT ---
    "a file sharing the seam test name elsewhere is not exempt|newfile|src/apps/fastcache-cli/Ranges_test.cpp|void Other(std::vector<int>& v) { std::ranges::iota(v, 0)~sc~ }~n~|src/apps/fastcache-cli/Ranges_test.cpp:1: calls|-"

    # --- THIRD-PARTY ROOTS ---
    "a vendored call is refused once the roots stop naming it|roots|vendor/upstream|vendor/elsewhere|vendor/upstream/src/Upstream.cpp:4: calls `std::ranges::iota` directly|-"

    # --- FAILS CLOSED ---
    "a header shape the check cannot read|seam|inline constexpr auto |inline constexpr auto const& |derived no wrapped facility from src/FastCache/Core/Ranges.hpp|-"
    "no seam header|noseamheader|-|-|the seam anchor src/FastCache/Core/Ranges.hpp is missing|-"
    "no seam test|noseamtest|-|-|the seam anchor src/FastCache/Core/Ranges_test.cpp is missing|-"
    "a seam the enumeration did not list|untrackseamtest-git|-|-|listed 1 of the 2 exempt files && the enumeration missed the seam it is anchored on|-"
    "no first-party C++ at all|roots|vendor/upstream|src~n~vendor/upstream|matched no first-party C++ sources|-"
)

# ---------------------------------------------------------------------------
set(failures "")
set(caseCount 0)

foreach(row IN LISTS FastCachedRangesSeamCases)
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

    # The verdict itself, separate from the needles, so a needle appearing inside a DIFFERENT
    # failure's text cannot stand in for it. `CMake Error|CMake Warning`, never the error word
    # alone: a sub-run that merely WARNS is not a clean pass (#672).
    set(sawSignal FALSE)
    if(output MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    # verdict-error-only: a read of the CASE TABLE, not of the sub-run. The table spells
    # `CMake Error` in the `must not appear` field to mean "this case expects acceptance".
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
        "the case table is empty, so this fixture asserted nothing -- which is exactly what a "
        "green run with no cases looks like")
endif()

# `caseCount` counts rows; `failures` counts MESSAGES, several per case at most -- hence
# findings ACROSS cases, never findings OF cases.
if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`ranges-seam` did not behave as its own documentation says. A check nobody has")
    message("watched refuse is not a check -- and one nobody has watched stay QUIET is one that")
    message("gets deleted.")
    message(FATAL_ERROR "ranges-seam-selftest: ${failureCount} finding(s) across ${caseCount} case(s)")
endif()

message(STATUS "ranges-seam-selftest: ${caseCount} case(s), every verdict as documented")
