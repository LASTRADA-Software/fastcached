# SPDX-License-Identifier: Apache-2.0
#
# `std::random_device` is not a uniqueness source, and a temp path may not bet on it.
#
# ## Why a scan and not a type
#
# A unique scratch path comes from `Testing::UniqueScratchPath` (`src/tests/ScratchPath.hpp`):
# pid AND counter, no entropy source that can fail. Nothing in the type system makes a test
# reach for it, so forgetting to must not read the same as deciding not to -- which is this
# repository's standing split between an obligation to DO SOMETHING and one to SAY WHY.
#
# ## What went wrong, because the number is the argument
#
# `StorageTestUtils.hpp`'s `TempFile` named its file
# `std::mt19937_64 { std::random_device {}() }()`. On the WSL2/libstdc++ host
# [#1507](https://github.com/LASTRADA-Software/fastcached/issues/1507) was reproduced on,
# `std::random_device` answers **zero for 57% of draws** -- 1652 of 2880, measured across 32
# concurrent processes -- and `mt19937_64 { 0 }()` is always `2947667278772165694`. So 31 of
# those 32 processes agreed on one filename, and whichever opened it second was refused
# `InUse` by a `flock` doing precisely its job.
#
# That is the shape this check exists for: the symptom named the STORAGE layer, a ticket was
# filed against `CowTreeStorage::Open`, and the defect was four lines in a test helper. The
# unique-path helper had been written five times before with a counter; this was the sixth and
# the first to use something that merely LOOKS stronger than a counter, so none of the
# reasoning recorded against the other five applied to it.
#
# ## The rule
#
# `std::random_device` may be USED in exactly one place -- the production randomness seam,
# `Core/IRandomSource.hpp`, whose own comment owns the 32-bit `result_type` question. Anywhere
# else under `src/` is refused, or carries an exemption row with its reason.
#
# Comments are stripped first (`fastcached_strip_comments`), because the four files this rule
# was written for now EXPLAIN it in prose and a reader that counted those would refuse the
# very tree it had just fixed. Its blind spot is the shared stripper's: an introducer inside a
# string literal. That direction is a false GREEN and is accepted here for a stated reason --
# `std::random_device` inside a string literal is not a call, and a scan broad enough to catch
# one would refuse this file's own documentation.
#
# ## What it does NOT cover, stated rather than left to be discovered
#
#   * It says nothing about a path built from a counter alone, which is the way the first five
#     copies were wrong. That one IS visible to review against the rulebook, and a pattern
#     wide enough to catch every hand-rolled uniqueness scheme would refuse the seam itself.
#   * It reads only `src/`. A script or a fixture outside it that names a temp path is the
#     POSIX helpers' concern (`scripts/lib/e2e-common.sh`).
#   * A NEW legitimate consumer of the platform entropy source is an exemption row, not a
#     reason to widen the pattern: there is one seam that reads the device, and a second one is
#     a design decision somebody should have to write down. `Core/ISecureRandom.hpp` (#1527) is
#     such a decision and reads NO device: it asks the operating system's generator directly,
#     because an engine seeded from this device is exactly what repeated nonces across processes.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "unique-temp-paths: FASTCACHED_SOURCE_DIR must be set")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

# ---------------------------------------------------------------------------
# The one file the platform entropy source may be reached from. Overridable ONLY so the
# self-test can stage a tree; a real run never sets it, and the default is the claim.
if(NOT DEFINED FASTCACHED_RANDOM_SEAM)
    set(FASTCACHED_RANDOM_SEAM "src/FastCache/Core/IRandomSource.hpp")
endif()

# ---------------------------------------------------------------------------
# Sites that may draw from the device anyway, each with the reason it may.
#
# Empty today, and that is the point: the five sites this check was written for all had a
# better answer available. A row here is a claim that a caller needs the platform device
# rather than the seam, and it has to say why.
set(FastCachedRandomDeviceExemptions "")

# ---------------------------------------------------------------------------
# The positive anchor. A scan whose pattern has stopped matching reads exactly like a clean
# tree, so the legitimate use must be FOUND before anything is enumerated.
set(seamPath "${FASTCACHED_SOURCE_DIR}/${FASTCACHED_RANDOM_SEAM}")
if(NOT EXISTS "${seamPath}")
    message("")
    message("  ${FASTCACHED_RANDOM_SEAM} is not there.")
    message("")
    message("That file is where this check's own pattern is proved to match. Without it a")
    message("clean report would mean only that nothing was read.")
    message(FATAL_ERROR "unique-temp-paths: the randomness seam is missing")
endif()

file(READ "${seamPath}" seamWhole)
fastcached_strip_comments("${seamWhole}" seamCode)
string(REGEX MATCHALL "std::random_device" seamHits "${seamCode}")
list(LENGTH seamHits seamCount)
if(seamCount EQUAL 0)
    message("")
    message("  The pattern `std::random_device` matched nothing in ${FASTCACHED_RANDOM_SEAM}.")
    message("")
    message("That file is the one place the platform device is meant to be read, so either it")
    message("has stopped reading it -- in which case this check should be deleted rather than")
    message("left to pass over every file -- or the spelling moved. Either way a clean run")
    message("below would be a report about a pattern nobody looked for.")
    message(FATAL_ERROR "unique-temp-paths: the pattern found no use in the seam")
endif()
message(STATUS "unique-temp-paths: the seam reads the device in ${seamCount} place(s)")

# ---------------------------------------------------------------------------
# The file set: every first-party C++ source and header under src/, asked through the shared
# machinery so the walk fallback, the exclusions and the work-tree probe are not another copy.
fastcached_tracked_files("${FASTCACHED_SOURCE_DIR}"
    PATHSPECS "src"
    GLOBS "src/*.cpp" "src/**/*.cpp" "src/*.hpp" "src/**/*.hpp"
    FILTER "\\.(cpp|hpp)$"
    FILES_OUT sourceFiles
    MODE_OUT scanMode)

if(scanMode STREQUAL "git ls-files")
    message(STATUS "unique-temp-paths: mode: git-index")
else()
    message(STATUS "unique-temp-paths: mode: walk (${scanMode})")
endif()

list(LENGTH sourceFiles fileCount)
if(fileCount EQUAL 0)
    message("")
    message("  No C++ source was enumerated under src/ via ${scanMode}.")
    message("")
    message("This tree has hundreds, so that is an enumeration that broke rather than a tree")
    message("with no code. A scan selecting NOTHING is a refusal.")
    message(FATAL_ERROR "unique-temp-paths: the scan enumerated no source file")
endif()

# ---------------------------------------------------------------------------
# The exemption rows, parsed before the scan so a malformed row fails loudly rather than
# silently exempting nothing.
fastcached_split_rows(FastCachedRandomDeviceExemptions exemptPaths exemptReasons)
list(LENGTH exemptPaths exemptionCount)
set(exemptionsUsed "")

# ---------------------------------------------------------------------------
# The scan.
set(violations "")
set(readCount 0)
foreach(relative IN LISTS sourceFiles)
    if(relative STREQUAL FASTCACHED_RANDOM_SEAM)
        continue()
    endif()

    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)
    fastcached_strip_comments("${wholeFile}" code)
    math(EXPR readCount "${readCount} + 1")

    string(REGEX MATCHALL "std::random_device" hits "${code}")
    list(LENGTH hits hitCount)
    if(hitCount EQUAL 0)
        continue()
    endif()

    list(FIND exemptPaths "${relative}" exemptIndex)
    if(NOT exemptIndex EQUAL -1)
        list(APPEND exemptionsUsed "${relative}")
        continue()
    endif()

    list(APPEND violations "${relative} (${hitCount} use(s))")
endforeach()

# ---------------------------------------------------------------------------
# A row naming no site is refused. An exemption that has stopped describing anything reads as
# coverage of a case that no longer exists, and the next author extends it rather than
# deleting it.
set(staleRows "")
foreach(row IN LISTS exemptPaths)
    list(FIND exemptionsUsed "${row}" usedIndex)
    if(usedIndex EQUAL -1)
        list(APPEND staleRows "${row}")
    endif()
endforeach()

if(staleRows)
    message("")
    message("  These exemption rows name no site that draws from the device:")
    foreach(row IN LISTS staleRows)
        message("    ${row}")
    endforeach()
    message("")
    message("Delete the row. A stale exemption is worse than none: it reads as a decision that")
    message("was made about something, and there is nothing there.")
    message(FATAL_ERROR "unique-temp-paths: ${staleRows} is stale")
endif()

if(violations)
    message("")
    message("  These files read the platform entropy source directly:")
    foreach(violation IN LISTS violations)
        message("    ${violation}")
    endforeach()
    message("")
    message("`std::random_device` is not a uniqueness source. On one host here it answers ZERO")
    message("for 57% of draws, so every process picks the identical `random` name and the")
    message("second one is refused by whatever guard owns that resource -- which is a defect")
    message("that reads as a concurrency bug in the resource rather than in the name (#1507).")
    message("")
    message("For a TEMP PATH:    Testing::UniqueScratchPath(prefix), from src/tests/ScratchPath.hpp")
    message("                    -- pid AND counter, and the pid is what two PROCESSES cannot share.")
    message("For RANDOMNESS:     Core/IRandomSource.hpp, injected, which is also what makes it")
    message("                    testable against a scripted source.")
    message("For bytes that must NEVER REPEAT -- a nonce, a minted id: Core/ISecureRandom.hpp,")
    message("                    the operating system's generator, whose failure is a refusal.")
    message("")
    message("If a caller genuinely needs the platform device, add a row to")
    message("`FastCachedRandomDeviceExemptions` in this file saying why.")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR "unique-temp-paths: ${violationCount} file(s) read std::random_device outside the seam")
endif()

message(STATUS
    "unique-temp-paths: ${readCount} file(s) read, ${exemptionCount} exemption(s), the seam is the only caller")
