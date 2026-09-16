# SPDX-License-Identifier: Apache-2.0
#
# No first-party C++ walks an enum's enumerators by hand now that
# `src/FastCache/Core/EnumTable.hpp` offers `Enumerators<Enum>()`.
#
# ## Why the seam exists
#
# Ten sites spelled the same walk before #1441 -- `std::views::iota` from zero to
# `EnumeratorCount<Enum>` and a `static_cast<Enum>(index)` in the body -- which is an index
# where the code wanted an enumerator. One of them even compared
# `static_cast<std::size_t>(route.match) != kind`, casting the enumerator back out again to
# meet the index it had been given. `Enumerators<Enum>()` is a view of the enumerators
# themselves, and `Enumerators(Enum from)` the tail of them, so the cast disappears.
#
# ## Why a check and not only the seam
#
# Because nothing refused the hand-spelled form, which is the whole reason there were ten.
# The guidelines forbid C-style loops and say to prefer range views, and every one of those
# ten obeyed both -- `views::iota` IS a range view. The defect is not the loop shape, it is
# walking indices to reach enumerators, and no existing check can see that.
#
# ## What is scanned
#
# First-party C++, DERIVED rather than listed: every tracked C++ source (`git ls-files`, or a
# directory walk where there is no index), minus the third-party roots read from
# `scripts/lib/third-party-roots.txt` -- vendored code is not ours to edit, and is counted as
# declined rather than dropped silently. Tests count: the seam is as available to them.
#
# ## What is refused, and what is deliberately NOT
#
# Two line-local shapes, in code with comments stripped, so prose explaining the rule is not
# read as breaking it:
#
#   1. `views::iota` on the same line as `EnumeratorCount<` -- the #1441 shape exactly.
#   2. a `for` or `while` whose bound is `EnumeratorCount<`, on one line.
#
# **A `static_cast<...>(x)` near an `EnumeratorCount<` is NOT refused, and that was measured
# rather than assumed.** The obvious third pattern matches six sites in this tree and every
# one of them is legitimate -- `static_cast<std::size_t>(raw) >= EnumeratorCount<E>` in
# `Consensus/RaftTypes.hpp` and `apps/fastcache-cc/Stats.cpp`, which are BOUNDS CHECKS on a
# value arriving off a wire, not walks. A check on that pattern would refuse correct code,
# which is why shape 2 requires `for`/`while` on the line: `>=` is a range test and `<` in an
# `if` may be one too.
#
# Also not covered, said here so the next reader does not over-apply the rule:
#
# - a walk split across lines, with the bound on its own continuation line. This is
#   line-local like every reader in this tree; `check-ranges-seam.cmake` has the same limit.
# - the C-style three-clause `for` over an enumerator count. That belongs to #1452, which
#   widens `check-test-loops` from the test tree to production, and duplicating it here would
#   report one defect twice.
#
# ## Why the seam's own existence is derived
#
# The anchor is `Enumerators` in the seam header. If that declaration goes away the check is
# refusing sites for a facility that no longer exists, so a missing anchor is a REFUSAL and
# never read as "nothing to enforce" -- the direction a restated list fails in silently.
#
# Exempt, derived from the one path rather than listed: the seam header itself, which
# implements the walk, and its test beside it by the house `Foo.hpp` / `Foo_test.cpp`
# convention. The enumeration is asserted to have REACHED the header, so an exemption cannot
# be satisfied by a scan that never looked.

cmake_minimum_required(VERSION 3.20)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED FASTCACHED_SOURCE_DIR OR FASTCACHED_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(seamHeader "src/FastCache/Core/EnumTable.hpp")
set(seamTest "src/FastCache/Core/EnumTable_test.cpp")

# --------------------------------------------------------------------------
# The anchor: the seam this check exists to enforce must still be there.
# --------------------------------------------------------------------------

set(seamPath "${FASTCACHED_SOURCE_DIR}/${seamHeader}")
if(NOT EXISTS "${seamPath}")
    message(FATAL_ERROR
        "enumerator-walks: the seam header ${seamHeader} is missing, so this check would be "
        "refusing sites for a facility that no longer exists and cannot conclude")
endif()
file(READ "${seamPath}" seamContent)
if(NOT seamContent MATCHES "Enumerators")
    message("")
    message("  ${seamHeader} no longer declares Enumerators.")
    message("")
    message("This check refuses hand-spelled enumerator walks because a seam offers the")
    message("walk instead. With the seam gone it has nothing to offer in their place, so")
    message("it refuses rather than reporting a clean tree -- 'no violations' and 'the rule")
    message("no longer applies' are different answers and only one of them is good news.")
    message(FATAL_ERROR "enumerator-walks: the seam anchor is missing and the scan cannot conclude")
endif()

# --------------------------------------------------------------------------
# What to scan: first-party C++, derived.
# --------------------------------------------------------------------------

# One answer, from `fastcached_first_party_cxx`, which nine other checks each had their own
# copy of before it was extracted. The MODE comes back with the files because there are two
# of them, they cover different sets, and CI takes the git one -- so every verdict this check
# prints names it, and a self-test case asserts it.
fastcached_first_party_cxx("${FASTCACHED_SOURCE_DIR}" sourceFiles declinedFiles scanSource)
list(LENGTH declinedFiles declinedCount)

if(NOT sourceFiles)
    message("")
    message("  No first-party C++ source was found in this repository at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved source")
    message("root, a FASTCACHED_SOURCE_DIR pointing somewhere else, or a third-party root")
    message("that swallowed the whole tree.")
    message(FATAL_ERROR "enumerator-walks: the scan matched no first-party C++ and cannot conclude")
endif()
list(LENGTH sourceFiles fileCount)

# The enumeration must have REACHED the file the exemption is about. Without this, an
# exemption is satisfied by a scan that never looked -- which is the same green as a clean
# tree and tells the reader nothing.
list(FIND sourceFiles "${seamHeader}" seamIndex)
if(seamIndex EQUAL -1)
    message(FATAL_ERROR
        "enumerator-walks: the enumeration (${scanSource}) did not reach ${seamHeader}, which "
        "this check exempts -- so the exemption is untested and the scan cannot conclude")
endif()

# --------------------------------------------------------------------------
# The scan.
# --------------------------------------------------------------------------

# Cheap whole-file gate first: the line walk is O(n^2) in a file's length, so only files
# naming the count at all are walked.
set(countToken "EnumeratorCount<")

# Shape 1: the #1441 spelling. Shape 2: a loop whose bound is the count.
set(iotaPattern "views::iota.*EnumeratorCount<")
set(loopPattern "(for|while)[ \t]*\\(.*[<][ \t]*EnumeratorCount<")

set(violations "")
set(walkedCount 0)

foreach(relative IN LISTS sourceFiles)
    if(relative STREQUAL "${seamHeader}" OR relative STREQUAL "${seamTest}")
        continue()
    endif()
    set(absolute "${FASTCACHED_SOURCE_DIR}/${relative}")
    if(NOT EXISTS "${absolute}")
        continue()
    endif()
    file(READ "${absolute}" content)
    string(FIND "${content}" "${countToken}" mentioned)
    if(mentioned EQUAL -1)
        continue()
    endif()
    math(EXPR walkedCount "${walkedCount} + 1")

    fastcached_scan_code_lines("${content}" "${iotaPattern}" iotaHits)
    foreach(hit IN LISTS iotaHits)
        if(hit MATCHES "^use:(.+)$")
            list(APPEND violations "${relative}:${CMAKE_MATCH_1}: views::iota over EnumeratorCount")
        endif()
    endforeach()

    fastcached_scan_code_lines("${content}" "${loopPattern}" loopHits)
    foreach(hit IN LISTS loopHits)
        if(hit MATCHES "^use:(.+)$")
            list(APPEND violations "${relative}:${CMAKE_MATCH_1}: a loop bounded by EnumeratorCount")
        endif()
    endforeach()
endforeach()

if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("Each of these walks INDICES to reach ENUMERATORS. Spell the walk with the seam:")
    message("")
    message("    for (auto const value: Enumerators<Enum>())        // all of them")
    message("    for (auto const value: Enumerators(Enum::Third))   // from one onwards")
    message("")
    message("both from FastCache/Core/EnumTable.hpp, and drop the static_cast the index")
    message("forced on the body. Enumerators(from) | std::views::reverse walks backwards.")
    message("")
    message("This does NOT say every EnumeratorCount use is wrong. A bounds check on a value")
    message("off a wire -- static_cast<std::size_t>(raw) >= EnumeratorCount<E> -- is right and")
    message("is not matched here. Nor is a C-style three-clause for, which is #1452's scan.")
    message("")
    # The refusal says what it SCANNED, not only what it found. A verdict carries its own
    # conditions, and this one has two enumeration modes: a reader who cannot tell which ran
    # cannot tell a real finding from a scan that read a different set of files. It is also
    # what lets a self-test case assert the mode on this path, which the status line below
    # can only do for an accepting run.
    message("Scanned ${fileCount} first-party C++ file(s) from ${scanSource} "
            "(${declinedCount} third-party declined), ${walkedCount} naming EnumeratorCount.")
    message(FATAL_ERROR
        "enumerator-walks: ${violationCount} site(s) walk an enum's enumerators by hand")
endif()

message(STATUS
    "enumerator-walks: ${fileCount} first-party C++ file(s) from ${scanSource} "
    "(${declinedCount} third-party declined), ${walkedCount} naming EnumeratorCount walked; "
    "no hand-spelled enumerator walk outside the seam")
