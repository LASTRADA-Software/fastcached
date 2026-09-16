# SPDX-License-Identifier: Apache-2.0
#
# Drives `check-enumerator-walks.cmake` over staged trees, in both directions.
#
# **Both directions, because only one of them is usually tested.** A self-test that plants a
# violation and watches the check object proves the refusing half and says nothing about the
# accepting half -- and a guard nobody has watched ACCEPT is not known to work either. The
# cases below therefore include the shape the check must NOT refuse: a bounds check on a
# value off a wire, `static_cast<std::size_t>(raw) >= EnumeratorCount<E>`, which appears six
# times in the real tree and is correct every time.
#
# Verdicts are read from the check's OUTPUT and not from its exit status, and the output is
# FLATTENED before matching: CMake word-wraps its diagnostics at a column that depends on the
# scratch path's length, so a phrase grepped for can exist in the output and in no single
# line of it.
#
# **BOTH enumeration modes are exercised, and each case asserts which one it ran in.** A
# staged tree is an untracked scratch directory, so `git ls-files` there returns nothing and
# the check falls through to its directory walk -- which is NOT the mode CI uses. This tree
# has already paid for that exact shape: six green self-test cases exercised a directory-walk
# fallback while CI exercised git, so the mode under test was not the mode in use and the
# guard passed because it was testing something else. A case whose name ends in `Git` is
# `git init`-ed and `git add`-ed so the check takes its git path, and every case's expected
# phrase names the mode, so a case silently changing modes fails rather than passing.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FASTCACHED_SOURCE_DIR OR FASTCACHED_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR OR FASTCACHED_SCRATCH_DIR STREQUAL "")
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-enumerator-walks.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(roots "${FASTCACHED_SOURCE_DIR}/scripts/lib/third-party-roots.txt")
if(NOT EXISTS "${roots}")
    message(FATAL_ERROR "the third-party roots file is missing: ${roots}")
endif()

set(seamBody "
#pragma once
namespace FastCache
{
template <typename Enum>
[[nodiscard]] constexpr auto Enumerators() noexcept
{
    return std::views::iota(std::size_t { 0 }, EnumeratorCount<Enum>);
}
}
")

set(caseCount 0)
set(failureCount 0)

# Run the check over a staged tree.
#
# @param name       The case name, also the scratch subdirectory.
# @param expectFail TRUE when the check must refuse.
# @param phrase     A phrase the flattened output must contain.
# @param seam       The seam header's content (so a case can remove the anchor).
# @param planted    `relative/path.hpp|content` rows for the files to plant.
function(Case name expectFail phrase seam)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Core")
    file(MAKE_DIRECTORY "${tree}/scripts/lib")
    configure_file("${roots}" "${tree}/scripts/lib/third-party-roots.txt" COPYONLY)
    file(WRITE "${tree}/src/FastCache/Core/EnumTable.hpp" "${seam}")

    # Remaining arguments are the planted files.
    math(EXPR last "${ARGC} - 1")
    if(last GREATER_EQUAL 4)
        foreach(index RANGE 4 ${last})
            set(row "${ARGV${index}}")
            string(FIND "${row}" "|" bar)
            if(bar EQUAL -1)
                message(FATAL_ERROR "case `${name}`: planted row has no `|`: ${row}")
            endif()
            string(SUBSTRING "${row}" 0 ${bar} path)
            math(EXPR after "${bar} + 1")
            string(SUBSTRING "${row}" ${after} -1 body)
            get_filename_component(parent "${tree}/${path}" DIRECTORY)
            file(MAKE_DIRECTORY "${parent}")
            file(WRITE "${tree}/${path}" "${body}\n")
        endforeach()
    endif()

    # A case named `...Git` is made a real repository, so the check takes its `git ls-files`
    # path -- the one CI takes. Without at least one such case every verdict below would be
    # about the fallback.
    if(name MATCHES "Git$")
        if(NOT GIT_EXECUTABLE)
            find_package(Git QUIET)
        endif()
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "case `${name}`: git is needed to exercise the git enumeration mode, and a "
                "case that silently ran the fallback instead would be the defect this file "
                "exists to avoid")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}"
                        OUTPUT_QUIET ERROR_QUIET)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A
                        OUTPUT_QUIET ERROR_QUIET)
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        RESULT_VARIABLE status)
    set(combined "${out}${err}")
    # CMake wraps its diagnostics, so a phrase can exist in the output and in no line of it.
    string(REPLACE "\n" " " flat "${combined}")
    string(REGEX REPLACE "[ \t]+" " " flat "${flat}")

    set(failed "${failureCount}")
    math(EXPR caseCount "${caseCount} + 1")

    if(expectFail AND status EQUAL 0)
        message("  ${name}: the check ACCEPTED a tree it must refuse")
        message("${combined}")
        math(EXPR failed "${failed} + 1")
    elseif(NOT expectFail AND NOT status EQUAL 0)
        message("  ${name}: the check REFUSED a tree it must accept")
        message("${combined}")
        math(EXPR failed "${failed} + 1")
    elseif(NOT flat MATCHES "${phrase}")
        message("  ${name}: the right side, but its output does not say `${phrase}`")
        message("${combined}")
        math(EXPR failed "${failed} + 1")
    else()
        message("  ${name}: ok")
    endif()

    set(caseCount "${caseCount}" PARENT_SCOPE)
    set(failureCount "${failed}" PARENT_SCOPE)
endfunction()

# --- the accepting direction ------------------------------------------------------------

# The shape the check must never refuse. Measured in the real tree: six such sites, all
# correct. If this case ever goes red the check has started refusing bounds checks.
#
# Every accepting case's phrase names the enumeration MODE, which is the only place the
# check states it -- a refusing case cannot, since it stops before the status line.
Case(boundsCheckAccepted FALSE "from directory walk" "${seamBody}"
     "src/FastCache/Wire.hpp|inline bool Known(unsigned raw) { return static_cast<std::size_t>(raw) >= EnumeratorCount<Op>; }")

# A walk spelled through the seam is the point of the seam.
Case(seamWalkAccepted FALSE "no hand-spelled enumerator walk" "${seamBody}"
     "src/FastCache/Panel.hpp|inline void Draw() { for (auto const v: Enumerators<Op>()) Use(v); }")

# A COMMENT is not a call site. Without this the check would refuse the prose that explains
# it, including its own rulebook entry quoted into a header.
Case(commentAccepted FALSE "no hand-spelled enumerator walk" "${seamBody}"
     "src/FastCache/Note.hpp|// never write std::views::iota(0, EnumeratorCount<Op>) any more\nint keep = 1;")

# --- and the same two answers in the mode CI actually uses ------------------------------

# A real repository, so the check enumerates with `git ls-files`. Asserting the mode in the
# phrase is what makes this case different from its fallback twin rather than a duplicate of
# it: without the assertion a `git init` that silently failed would leave this passing as a
# second walk-mode case.
Case(boundsCheckAcceptedGit FALSE "from git ls-files" "${seamBody}"
     "src/FastCache/Wire.hpp|inline bool Known(unsigned raw) { return static_cast<std::size_t>(raw) >= EnumeratorCount<Op>; }")

# The refusing side of the git mode, and it asserts the MODE rather than the finding: the
# finding is already covered by its fallback twin, so without this the two cases would be
# indistinguishable and one of them would be decoration. The check names its scan on the
# refusal path for exactly this reason.
Case(iotaRefusedGit TRUE "from git ls-files" "${seamBody}"
     "src/FastCache/Panel.cpp|void Draw() { for (auto const i: std::views::iota(std::size_t { 0 }, EnumeratorCount<Op>)) Use(static_cast<Op>(i)); }")

# --- the refusing direction -------------------------------------------------------------

Case(iotaRefused TRUE "views::iota over EnumeratorCount" "${seamBody}"
     "src/FastCache/Panel.cpp|void Draw() { for (auto const i: std::views::iota(std::size_t { 0 }, EnumeratorCount<Op>)) Use(static_cast<Op>(i)); }")

Case(loopBoundRefused TRUE "a loop bounded by EnumeratorCount" "${seamBody}"
     "src/FastCache/Panel.cpp|void Draw() { for (std::size_t i = 0; i < EnumeratorCount<Op>; ++i) Use(static_cast<Op>(i)); }")

# The refusal names file AND line, because an assertion on the filename alone passes under a
# reader that has lost track of where it is.
Case(refusalNamesTheLine TRUE "Panel.cpp:2:" "${seamBody}"
     "src/FastCache/Panel.cpp|int keep = 1;\nvoid Draw() { for (auto const i: std::views::iota(0, EnumeratorCount<Op>)) Use(i); }")

# --- the scan refusing to conclude ------------------------------------------------------

# The anchor gone. Not a clean tree: the check would be refusing sites for a facility that
# no longer exists, and 'no violations' and 'the rule no longer applies' are different
# answers.
Case(anchorMissingRefused TRUE "the seam anchor is missing" "#pragma once\nint nothing = 0;\n"
     "src/FastCache/Panel.hpp|int keep = 1;")

# --- the exemption ----------------------------------------------------------------------

# The seam header implements the walk, so it is exempt -- and the check asserts the
# enumeration REACHED it, which is what stops the exemption being satisfied by a scan that
# never looked.
Case(seamHeaderExempt FALSE "no hand-spelled enumerator walk" "${seamBody}"
     "src/FastCache/Core/EnumTable_test.cpp|void T() { for (auto const i: std::views::iota(0, EnumeratorCount<Op>)) Use(i); }")

# --- the verdict ------------------------------------------------------------------------

# How many cases RAN, because a self-test that stopped early must not look like one that
# judged something.
if(failureCount GREATER 0)
    message(FATAL_ERROR
        "enumerator-walks-selftest: ${failureCount} finding(s) across ${caseCount} case(s)")
endif()
message(STATUS "enumerator-walks-selftest: ${caseCount} case(s) ran, all as expected")
