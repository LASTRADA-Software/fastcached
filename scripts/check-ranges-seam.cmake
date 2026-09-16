# SPDX-License-Identifier: Apache-2.0
#
# No first-party C++ calls a range algorithm that `src/FastCache/Core/Ranges.hpp` wraps
# directly: `std::ranges::iota` is spelled `FastCache::Ranges::Iota`, `std::ranges::fold_left`
# is spelled `FastCache::Ranges::FoldLeft`, and so on for every facility that header wraps.
#
# ## Why the seam exists
#
# `Package (macOS .pkg)` compiles with AppleClang over Apple's own libc++, which has no
# `std::ranges::iota`. PR #1392 went red there on `DashboardPanel.cpp` with every REQUIRED
# context green -- libstdc++, the MSVC STL and the Homebrew LLVM libc++ of `macOS-clang-release`
# all ship it. The owner kept the C++23 spelling rather than retreat to `std::iota` and
# `std::accumulate`, so `Ranges.hpp` selects the standard function object where its
# feature-test macro says the library ships it, and a transcription of the standard where it
# does not. The header carries the argument in full.
#
# ## Why a check and not only the header
#
# Because the failure lands on a job nobody is required to wait for. A direct call builds on
# every leg a developer runs locally and on every required context, so the first place it can
# fail is a packaging job that runs after the pull request looks finished -- or at release
# time. And a build stops at its first failure: the `fold_left` in `FrameEndpoint.cpp` sat
# behind the `iota` and was never reached, so fixing only the reported site would have moved
# the red one file along. This runs in the DEFAULT ctest set, where every developer meets it.
#
# ## What is scanned
#
# First-party C++, DERIVED rather than listed: every tracked C++ source (`git ls-files`, or a
# directory walk where there is no index), minus the third-party roots read from
# `scripts/lib/third-party-roots.txt` -- vendored code is not ours to edit, and is counted as
# declined rather than dropped silently. Tests, the benchmarks and the test client count:
# they are first-party C++ and the macOS legs that build them use a libc++ too.
#
# ## What is refused, derived from the header
#
# The set of wrapped facilities is READ from `Ranges.hpp`, never restated here: every selection
# block of the shape
#
#     #if defined(__cpp_lib_<feature>) ...
#     inline constexpr auto <Name> = std::ranges::<name>;
#
# contributes one row -- the standard name refused, the seam name offered, the macro that gates
# it. A restated list would catch a facility going away and be blind to one ARRIVING, which is
# the direction that matters: a row added to the header is enforced here with no edit. A header
# this check derives no row from is REFUSED, never read as "nothing to enforce".
#
# A USE is `ranges::<name>` followed by something that cannot continue an identifier, in code
# with comments stripped (`fastcached_scan_code_lines`, `scripts/lib/CheckCommon.cmake`). So
# `std::ranges::iota` and a bare `ranges::iota` under a `using namespace std;` are refused, and
# `std::views::iota` and `std::ranges::iota_view` -- C++20, shipped everywhere -- are not.
# The seam spelling differs by CASE (`Ranges::Iota`), which is what lets one case-sensitive
# pattern tell the two apart.
#
# Exempt, and derived from the one path below rather than listed: the seam header itself, and
# its test beside it by the house `Foo.hpp` / `Foo_test.cpp` convention, which has to name the
# standard facility to assert the fallback agrees with it. Both must EXIST, or the exemption
# describes nothing.
#
# ## Blind spots, stated
#
# - A namespace alias (`namespace sr = std::ranges; sr::iota(...)`) is not seen. Nothing in
#   this tree aliases `std::ranges`; the census below says so.
# - The token inside a STRING LITERAL reads as code and would be refused, as in every
#   regex-shaped reader here; reword rather than widen.
# - This enforces the seam, not the portability of C++23 in general. A range algorithm the
#   header does not wrap is not refused -- `std::ranges::contains`, used across the tree,
#   builds on every leg today.
#
# The census this was written against, with its pattern: `git grep -nE
# 'ranges::(iota|fold_left|fold_left_first|fold_right|fold_right_last|fold_left_with_iter|fold_left_first_with_iter)\b'`
# over the whole repository at `8b8a9d39` found THREE direct calls, all under `src/`:
# `DashboardPanel.cpp` and `FrameEndpoint.cpp` (new on that branch) and `CowTreeStorage_test.cpp`
# (on master already, unseen by the package job because it builds no tests). `git grep -nE
# 'namespace [A-Za-z_]+ *= *std::ranges|using namespace std::ranges|using std::ranges::'`
# found none.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check reports
# failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> [-DGIT_EXECUTABLE=<git>] -P scripts/check-ranges-seam.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# The one path this check is anchored on. Its test is DERIVED from it.
set(seamHeader "src/FastCache/Core/Ranges.hpp")
string(REGEX REPLACE "\\.hpp$" "_test.cpp" seamTest "${seamHeader}")

foreach(anchor IN ITEMS "${seamHeader}" "${seamTest}")
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${anchor}")
        message("")
        message("  ${anchor} does not exist.")
        message("")
        message("This check refuses direct calls to the range algorithms that header wraps, and")
        message("exempts the header and its test. With either gone it can neither say what to")
        message("refuse nor what it exempts -- a moved or renamed seam, not a clean tree. Point")
        message("`seamHeader` in scripts/check-ranges-seam.cmake at where it went.")
        message(FATAL_ERROR "ranges-seam: the seam anchor ${anchor} is missing and the scan cannot conclude")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The rows, read from the header. Matched over the whole content, and the match stops before
# the `;` that ends the declaration, so no element of the result list is split by one.
file(READ "${FASTCACHED_SOURCE_DIR}/${seamHeader}" seamContent)
string(REPLACE "\r\n" "\n" seamContent "${seamContent}")
set(selectionShape "#if defined\\((__cpp_lib_[a-z0-9_]+)\\)[^\n]*\ninline constexpr auto ([A-Z][A-Za-z0-9]*) = std::ranges::([a-z_][a-z0-9_]*)")
string(REGEX MATCHALL "${selectionShape}" selections "${seamContent}")

set(rowNames "")
set(rowSummaries "")
foreach(selection IN LISTS selections)
    if(NOT selection MATCHES "^${selectionShape}$")
        message(FATAL_ERROR "ranges-seam: a selection matched as a whole and not by its own groups: ${selection}")
    endif()
    set(macro "${CMAKE_MATCH_1}")
    set(seamName "${CMAKE_MATCH_2}")
    set(standardName "${CMAKE_MATCH_3}")
    list(APPEND rowNames "${standardName}")
    set("seamNameOf_${standardName}" "${seamName}")
    set("macroOf_${standardName}" "${macro}")
    list(APPEND rowSummaries "std::ranges::${standardName} -> Ranges::${seamName}")
endforeach()

if(NOT rowNames)
    message("")
    message("  No selection block was read from ${seamHeader}.")
    message("")
    message("Each wrapped facility is expected as two consecutive lines:")
    message("")
    message("  #if defined(__cpp_lib_<feature>) && !defined(FC_RANGES_FORCE_FALLBACK)")
    message("  inline constexpr auto <Name> = std::ranges::<name>;")
    message("")
    message("Reading none is not \"nothing to enforce\": it is a header whose shape changed")
    message("under this check, and every direct call would pass unseen. Restore the shape, or")
    message("teach `selectionShape` in scripts/check-ranges-seam.cmake the new one.")
    message(FATAL_ERROR "ranges-seam: derived no wrapped facility from ${seamHeader} and cannot conclude")
endif()

list(REMOVE_DUPLICATES rowNames)
string(REPLACE ";" ", " rowSummaryText "${rowSummaries}")

# ---------------------------------------------------------------------------
# Which C++ this repository holds, asked of git, then narrowed to what it OWNS by the
# third-party roots -- through `fastcached_first_party_cxx`, which is the ONE answer to that
# question (#1485). The extension set that used to live here included `.ipp` and `.inl`, so
# it moved INTO the helper rather than the helper's narrower set being adopted here: this
# tree tracks none of either today, so a verbatim migration would have selected the identical
# files and silently stopped covering them the day somebody adds the first one.
#
# The MODE travels with the answer and is stated in the status line and the refusal below. It
# has three values, not two, and the walk's fallback exists for a source export with no index
# -- which holds no build tree and no dependency cache by construction, and that is what makes
# the walk sound there and unsound in a working checkout.
fastcached_first_party_cxx("${FASTCACHED_SOURCE_DIR}" sourceFiles declinedFiles scanSource)
list(LENGTH declinedFiles declinedCount)

if(NOT sourceFiles)
    message("")
    message("  No first-party C++ source was found in this repository at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved source")
    message("root, a FASTCACHED_SOURCE_DIR pointing somewhere else, or a third-party root")
    message("that swallowed the whole tree.")
    message(FATAL_ERROR "ranges-seam: the scan matched no first-party C++ sources and cannot conclude")
endif()

# Counted once, here, because the refusal names it too.
list(LENGTH sourceFiles fileCount)

# ---------------------------------------------------------------------------
set(violations "")
set(exemptSeen 0)

foreach(relative IN LISTS sourceFiles)
    if(relative STREQUAL seamHeader OR relative STREQUAL seamTest)
        math(EXPR exemptSeen "${exemptSeen} + 1")
        continue()
    endif()
    # The index can name a file deleted from the worktree and not yet staged; reading it
    # would print `CMake Error`, which would score as THIS check failing for another reason.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()

    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)
    foreach(standardName IN LISTS rowNames)
        # A whole-file test first; the line walk is quadratic and almost no file needs it.
        string(FIND "${wholeFile}" "ranges::${standardName}" tokenPosition)
        if(tokenPosition EQUAL -1)
            continue()
        endif()
        fastcached_scan_code_lines("${wholeFile}" "ranges::${standardName}([^A-Za-z0-9_]|$)" hits)
        foreach(hit IN LISTS hits)
            if(hit MATCHES "^use:([0-9]+)$")
                list(APPEND violations "${relative}:${CMAKE_MATCH_1}|${standardName}")
            endif()
        endforeach()
    endforeach()
endforeach()

# The exemption must have matched both files it names. The anchors exist on disk (asserted
# above); not seeing them HERE means the enumeration missed them -- an untracked seam, or a
# walk excluding their directory -- and then nothing says it saw the rest of the tree either.
if(NOT exemptSeen EQUAL 2)
    message("")
    message("  The enumeration (${scanSource}) listed ${exemptSeen} of the 2 exempt files:")
    message("  ${seamHeader} and ${seamTest}.")
    message("")
    message("Both exist on disk, so a scan that did not list them is not scanning the tree it")
    message("was pointed at. Stage the files, or find which exclusion dropped them.")
    message(FATAL_ERROR "ranges-seam: the enumeration missed the seam it is anchored on and cannot conclude")
endif()

if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        string(REPLACE "|" ";" fields "${violation}")
        list(GET fields 0 where)
        list(GET fields 1 standardName)
        message("  ${where}: calls `std::ranges::${standardName}` directly -- "
                "write `Ranges::${seamNameOf_${standardName}}` (gated by ${macroOf_${standardName}})")
    endforeach()
    message("")
    message("Each of these names a standard facility that ${seamHeader} wraps because a")
    message("standard library this project builds with does not ship it on every leg:")
    message("`Package (macOS .pkg)` builds with AppleClang over Apple's own libc++, which had no")
    message("`std::ranges::iota` when this check was written. That job is not a required")
    message("context, so a direct call merges green and breaks packaging afterwards.")
    message("")
    message("The fix at each site:")
    message("")
    message("  1. #include <FastCache/Core/Ranges.hpp>")
    message("  2. replace `std::ranges::<name>(...)` with `FastCache::Ranges::<Name>(...)`")
    message("     (`Ranges::<Name>` inside namespace FastCache), same arguments.")
    message("")
    message("Wherever the library ships the facility the seam is an object of the standard")
    message("function object's own type, so nothing changes where it compiled before, and")
    message("its fallback is the standard's wording transcribed. Do not guard the call site with the")
    message("feature-test macro instead: that re-derives the selection at every site, and the")
    message("arm a local build never compiles is the one that breaks.")
    message("")
    message("NOT covered, and not to be rewritten: `std::views::iota` and `std::ranges::iota_view`")
    message("(C++20, shipped everywhere), range algorithms the header does not wrap, and the")
    message("header and its test, which must name the standard facility.")
    message("")
    message("Wrapped facilities, read from ${seamHeader}: ${rowSummaryText}.")
    message("Enumerated ${fileCount} first-party C++ source(s) via ${scanSource}; declined ${declinedCount} under third-party roots.")
    message(FATAL_ERROR "ranges-seam: ${violationCount} site(s) call a wrapped range algorithm directly")
endif()

message(STATUS
    "ranges-seam: ${fileCount} first-party C++ source(s) via ${scanSource}, ${declinedCount} declined under "
    "third-party roots; wrapped facilities read from ${seamHeader}: ${rowSummaryText}; none called directly")
