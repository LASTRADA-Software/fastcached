# SPDX-License-Identifier: Apache-2.0
#
# A staged POSIX-shell stub that a NON-SHELL spawns is refused unless the test is
# registered off those platforms for a SUBJECT reason (#871).
#
# THE RULE HAD THREE PROSE HOMES AND NO EXECUTOR. `src/tests/CMakeLists.txt` applies
# it BY HAND at the `tidy-blind-spots-selftest` registration,
# `.agent/rules/testing.md` restates it and `.agent/rules/build-and-toolchain.md`
# restates it again. Nothing enforced it, so it held exactly as far as whoever wrote
# the next fixture had read.
#
# THE MECHANISM. A `#!/bin/sh` file is started by the KERNEL reading its shebang, and
# only a shell asks the kernel to do that. CMake's `execute_process`, Python's
# `subprocess` and C++'s `CreateProcess` are `CreateProcess` on Windows, which reads
# no shebang and fails ENOEXEC -- "inappropriate file type or format". Git Bash CAN
# start such a stub, so a fixture that runs happily by hand is one the check cannot
# run at all. That exact failure has landed here before (#668's gate self-test).
#
# WHY THE REASON TEXT IS THE DISCRIMINATOR, and this is the whole design. Both of
# these register a fixture off Windows, and they are different claims:
#
#   "Windows cannot start a shebang script"     -- a PORTABILITY excuse. The fixture
#                                                  could be made to work with a
#                                                  `.cmd` stub, and skipping instead
#                                                  silently drops coverage.
#   "the launcher sandbox this needs has no     -- a SUBJECT reason. There is nothing
#    Windows equivalent"                           on this platform to answer, so the
#                                                  skip stands in for no pass.
#
# A check that accepted either would accept the first, which is the one that costs
# coverage. So a `guarded` row's reason is matched against a banned vocabulary and
# refused when it only says the platform cannot start the file.
#
# THREE CLASSES, and the middle one is the remedy the refusal points at:
#
#   data      the stub is never executed -- bytes for a scan to read, or a file a
#             probe merely has to FIND. Not the hazard.
#   adapted   the fixture stages a form the spawner CAN start (a `.cmd` beside the
#             `#!/bin/sh`), so it runs everywhere instead of skipping. The best
#             outcome, and what `reactor-teardown-gate-selftest.sh` already does.
#   guarded   genuinely kept off a platform. Needs a SUBJECT reason.
#
# Collapsing `data` into the hazard would be an over-report: loud, misattributed,
# and it sends somebody to guard a fixture that is already correct.
# `ToolchainProbe_test.cpp` stages FOURTEEN stubs and is deliberately NOT a
# candidate -- it spawns them through a FAKE `IProcessRunner`, so no process is ever
# started and the mechanism list does not match it. That is the scan being narrow on
# purpose rather than missing something.
#
# So the POPULATION is derived and the CLASSIFICATION is stated. A candidate with no
# row is refused, which is what catches a new one ARRIVING; a row whose file is no
# longer a candidate is refused as stale.
#
# RUNS ON WINDOWS, which is where the defect fires, and `cmake -P` is the portable
# vehicle. It stages nothing executable, so it cannot trip its own rule.
cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "check-staged-stub-spawns: FASTCACHED_SOURCE_DIR is required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

set(failures 0)
macro(Refuse reason)
    message(SEND_ERROR "STAGED STUB SPAWNS: ${reason}")
    math(EXPR failures "${failures} + 1")
endmacro()

# --- the classification table -------------------------------------------------
#
# Rows are `<path>|<class>|<guard>|<reason>`.
#
#   class `data`      the stub is never executed. The reason names what reads it.
#   class `adapted`   the fixture stages an executable form for the spawner. `guard`
#                     is the token that selects it, and it must appear in the file.
#   class `guarded`   something spawns it and the fixture is kept off the platforms
#                     that cannot start it. `guard` must appear in the file, and
#                     `reason` must be about the SUBJECT rather than the platform.
#
# TOTAL: 8 rows, which is the derived candidate count on this tree and is asserted
# against it below rather than restated as a number nobody re-derives.
#
# NO SEMICOLONS in a reason. CMake turns `;` into a list separator, so a reason
# carrying one splits the row and `list(GET parts 3 ...)` reads past the end. The
# first version of this table had three, and the check died on `list index: 1 out
# of range` -- the table format being wrong rather than the tree.
set(StagedStubSpawns
    "scripts/check-compile-cache-install.cmake|guarded|CMAKE_HOST_WIN32|the launcher sandbox this needs has no Windows equivalent, so there is nothing here for that platform to answer"
    "scripts/check-compile-cache-daemon-staging.cmake|guarded|CMAKE_HOST_WIN32|the launcher sandbox this needs has no Windows equivalent, so there is nothing here for that platform to answer"
    "scripts/reactor-teardown-gate-selftest.sh|adapted|MINGW|stages a .cmd stub where the spawner cannot start a shebang, so the fixture runs everywhere rather than skipping"
    "scripts/check-control-bytes-selftest.cmake|data|-|staged as bytes for the control-byte reader to scan -- nothing executes it"
    "scripts/check-repository-hygiene-selftest.cmake|data|-|staged so a hygiene scan and git have a tracked file to look at -- nothing executes it"
    "scripts/check-tracked-files-selftest.cmake|data|-|staged so the tracked-file enumeration has a path to return -- nothing executes it"
    "scripts/check-staged-stub-spawns.cmake|data|-|this scan's OWN pattern literals. It carries every shebang it matches on and names every spawn mechanism, so it is a candidate by construction -- and it stages nothing"
    "scripts/check-staged-stub-spawns-selftest.cmake|data|-|the self-test's staged tree bodies, written as strings into synthetic trees under a scratch directory and never executed"
)

# The vocabulary that makes a reason a portability excuse rather than a subject
# reason. Lower-cased before matching.
set(PortabilityOnly
    "cannot start a shebang"
    "cannot run a shebang"
    "enoexec"
    "windows cannot"
    "does not work on windows"
    "not portable"
    "shebang is not supported"
)

# --- the derived population ----------------------------------------------------
fastcached_tracked_files("${FASTCACHED_SOURCE_DIR}"
    PATHSPECS "*.sh" "*.py" "*.cmake" "*.cpp" "*.hpp" "*.ps1"
    GLOBS "*.sh" "*.py" "*.cmake" "*.cpp" "*.hpp" "*.ps1"
    FILES_OUT allFiles
    MODE_OUT scanMode)
fastcached_decline_third_party("${FASTCACHED_SOURCE_DIR}" allFiles declined)

list(LENGTH allFiles fileCount)
message(STATUS "staged-stub-spawns: reading ${fileCount} first-party file(s) (${scanMode})")
if(fileCount EQUAL 0)
    Refuse("no first-party file was enumerated at all. An empty scan agrees with every claim, so this is a refusal rather than a clean run.")
endif()

set(candidates "")
foreach(relative IN LISTS allFiles)
    set(absolute "${FASTCACHED_SOURCE_DIR}/${relative}")
    if(NOT EXISTS "${absolute}")
        continue()
    endif()
    file(READ "${absolute}" text)
    # Line 1 is the file's OWN shebang and is not a staged stub. Dropped by
    # removing everything up to the first newline.
    string(FIND "${text}" "\n" firstBreak)
    if(firstBreak GREATER -1)
        math(EXPR afterFirst "${firstBreak} + 1")
        string(SUBSTRING "${text}" ${afterFirst} -1 body)
    else()
        set(body "")
    endif()
    set(hasStub FALSE)
    foreach(shebang "#!/bin/sh" "#!/bin/bash" "#!/usr/bin/env bash" "#!/usr/bin/env sh")
        string(FIND "${body}" "${shebang}" at)
        if(at GREATER -1)
            set(hasStub TRUE)
        endif()
    endforeach()
    if(NOT hasStub)
        continue()
    endif()
    # A non-shell spawn mechanism named anywhere in the same file. This is a
    # CANDIDATE test, not a verdict -- whether that mechanism starts THIS stub is
    # what the table's class column states.
    set(hasSpawn FALSE)
    foreach(mechanism "execute_process" "subprocess." "CreateProcess" "posix_spawn" "Popen")
        string(FIND "${text}" "${mechanism}" at)
        if(at GREATER -1)
            set(hasSpawn TRUE)
        endif()
    endforeach()
    if(hasSpawn)
        list(APPEND candidates "${relative}")
    endif()
endforeach()

list(LENGTH candidates candidateCount)
message(STATUS "staged-stub-spawns: ${candidateCount} file(s) stage a POSIX stub and name a non-shell spawn")
if(candidateCount EQUAL 0)
    Refuse("no candidate was derived. This tree is known to carry several, so a zero here is the scan having stopped matching rather than the hazard having gone away.")
endif()

# --- reconcile -----------------------------------------------------------------
set(rowPaths "")
foreach(row IN LISTS StagedStubSpawns)
    string(REPLACE "|" ";" parts "${row}")
    list(GET parts 0 rowPath)
    list(GET parts 1 rowClass)
    list(GET parts 2 rowGuard)
    list(GET parts 3 rowReason)
    list(APPEND rowPaths "${rowPath}")

    list(FIND candidates "${rowPath}" found)
    if(found EQUAL -1)
        Refuse("the row for `${rowPath}` no longer describes a candidate -- the file has stopped staging a stub, stopped naming a non-shell spawn, or moved. Delete the row; a table entry that describes nothing is a claim nobody can check.")
        continue()
    endif()

    if(rowClass STREQUAL "data")
        message(STATUS "  ok (data)     ${rowPath} -- ${rowReason}")
        continue()
    endif()
    if(rowClass STREQUAL "adapted")
        # The BEST outcome and the one the refusal below points at: the fixture
        # stages a form the spawner can start, so it runs on every platform
        # instead of skipping. Its guard token selects that form.
        file(READ "${FASTCACHED_SOURCE_DIR}/${rowPath}" adaptedText)
        string(FIND "${adaptedText}" "${rowGuard}" adaptedAt)
        if(adaptedAt EQUAL -1)
            Refuse("`${rowPath}` is classed `adapted` and its row names `${rowGuard}` as the token selecting the executable form -- and that token does not appear in the file. Either the adaptation was removed, in which case the fixture now fails ENOEXEC where the spawner is not a shell, or the row names the wrong token.")
        else()
            message(STATUS "  ok (adapted)  ${rowPath} -- ${rowReason}")
        endif()
        continue()
    endif()
    if(NOT rowClass STREQUAL "guarded")
        Refuse("the row for `${rowPath}` has class `${rowClass}`, which is none of `data`, `adapted` or `guarded`. A class nobody enumerated is not a pass.")
        continue()
    endif()

    # An `executed` row must actually carry its guard.
    file(READ "${FASTCACHED_SOURCE_DIR}/${rowPath}" guardText)
    string(FIND "${guardText}" "${rowGuard}" guardAt)
    if(guardAt EQUAL -1)
        Refuse("`${rowPath}` is classed `guarded` and its row names `${rowGuard}` as the guard that keeps it off the platforms that cannot start a shebang -- and that token does not appear in the file. Either the guard was removed, in which case the fixture now fails ENOEXEC there, or the row is naming the wrong token.")
        continue()
    endif()

    # And its reason must be about the SUBJECT.
    string(TOLOWER "${rowReason}" loweredReason)
    set(excuseHit "")
    foreach(phrase IN LISTS PortabilityOnly)
        string(FIND "${loweredReason}" "${phrase}" phraseAt)
        if(phraseAt GREATER -1)
            set(excuseHit "${phrase}")
        endif()
    endforeach()
    if(NOT excuseHit STREQUAL "")
        Refuse("`${rowPath}` is classed `guarded` and its reason says `${excuseHit}`, which is a PORTABILITY excuse rather than a subject reason. \"Windows cannot start a shebang\" and \"this subject never runs on Windows\" are different claims: the first describes a fixture that could be made to work with a `.cmd` stub, and skipping it silently drops coverage. State what there is on that platform for the fixture to answer -- or stage a form it can execute.")
        continue()
    endif()
    message(STATUS "  ok (guarded)  ${rowPath} -- guarded by ${rowGuard}: ${rowReason}")
endforeach()

foreach(candidate IN LISTS candidates)
    list(FIND rowPaths "${candidate}" found)
    if(found EQUAL -1)
        Refuse("`${candidate}` stages a POSIX-shell stub AND names a non-shell spawn mechanism, and no row in `StagedStubSpawns` says which it is. Add one. If the stub is only ever READ -- bytes for a scan, or a file a probe merely has to find -- class it `data` and say what reads it. If something SPAWNS it, prefer `adapted`: stage a `.cmd` form where the spawner is not a shell, so the fixture RUNS everywhere instead of skipping, and name the token that selects the form. Only where there is genuinely nothing for the other platform to answer, class it `guarded`, name the token that keeps it off, and give a reason about the SUBJECT rather than about the platform.")
    endif()
endforeach()

# The table's stated length, asserted rather than written in prose.
list(LENGTH StagedStubSpawns rowCount)
if(NOT rowCount EQUAL 8)
    Refuse("the table has ${rowCount} rows; the comment above says 8. Update both or neither -- a total stated beside a table is derived from it or it is a second claim.")
endif()

if(failures GREATER 0)
    message(FATAL_ERROR "staged-stub-spawns: ${failures} unclassified or wrongly classified site(s)")
endif()
message(STATUS "staged-stub-spawns: ${candidateCount} candidate(s), all classified")
