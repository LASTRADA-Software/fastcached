# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy
# the project has not stated, and this one tests strings for substrings.
cmake_minimum_required(VERSION 3.28)
#
# Run `read-slot-guard-canary` and refuse to report green unless the read-slot guard
# was watched REFUSING.
#
# `Detail::ClaimReadSlot` (`src/FastCache/Net/ReadSlot.hpp`) is the tripwire for
# #663: a socket has one read operation, `Read` and `WaitReadable` share it, and
# arming either while the other is parked drops the parked coroutine -- never
# resumed, never freed, and no signal of any kind. Nothing else in this tree can
# observe that, which is exactly why the guard needs watching.
#
# `WILL_FAIL` alone is NOT the mechanism, for the reason `iterator-debug-gate.ps1`
# gives about the same shape: a bare inversion cannot say WHY a green result is
# meaningless, and a non-zero exit is not proof on its own. A segfault, a missing
# shared library and a refused bind all exit non-zero. So the verdict here requires
# the assertion's OWN text, the same way that gate requires the runtime's
# "subscript out of range" and `tsan-gate.sh` requires "data race".
#
# Four outcomes, kept apart on purpose -- skipped, absent, unstarted and failed are
# four states and a count collapses them:
#
#   * the canary never reached the double-arm         -> nothing was watched, FAIL
#   * it reached it and SURVIVED                      -> the guard did not fire, FAIL
#   * it died without the guard's diagnostic          -> it died of something else, FAIL
#   * it died AND named the read-op slot              -> the guard refused, PASS
#
# Reports failure by printing `CMake Error`, because `message(FATAL_ERROR)` in
# script mode exits 0 on CMake 3.28 -- see `src/tests/CMakeLists.txt` for the
# measurement and for the one spelling of the pattern that reads it.
#
# Usage:
#   cmake -DFASTCACHED_CANARY=<path> -P scripts/read-slot-guard-gate.cmake

if(NOT DEFINED FASTCACHED_CANARY)
    message(FATAL_ERROR "read-slot-guard-gate: FASTCACHED_CANARY must be set to the canary executable")
endif()

if(NOT EXISTS "${FASTCACHED_CANARY}")
    message(FATAL_ERROR "read-slot-guard-gate: the canary does not exist: ${FASTCACHED_CANARY}")
endif()

# Merged, because glibc's `assert` writes to stderr while the canary's own progress
# markers go there too, and the order between them is what the report reads.
execute_process(
    COMMAND "${FASTCACHED_CANARY}"
    RESULT_VARIABLE canaryResult
    OUTPUT_VARIABLE canaryOut
    ERROR_VARIABLE canaryErr
    TIMEOUT 90
)
set(canaryText "${canaryOut}${canaryErr}")

message(STATUS "read-slot-guard-gate: canary exited '${canaryResult}'")
message(STATUS "read-slot-guard-gate: canary said:\n${canaryText}")

# Did it get as far as the double-arm at all? A canary that could not bind, or whose
# client never arrived, has told us nothing -- and must not read as the guard working.
if(NOT canaryText MATCHES "arming a Read over a parked WaitReadable")
    message(FATAL_ERROR
        "read-slot-guard-gate: the canary never reached the double-arm, so the guard was not watched at all")
endif()

if(canaryResult STREQUAL "0")
    message(FATAL_ERROR
        "read-slot-guard-gate: the canary SURVIVED arming a Read over a parked WaitReadable. Either this build "
        "has assertions compiled out (the registration is guarded to Debug, so that is itself a defect), or "
        "Detail::ClaimReadSlot no longer guards that arm site -- see issue #663 and FastCache/Net/ReadSlot.hpp")
endif()

# The assertion's own words. `ClaimReadSlot`'s message names the read-op slot, and
# both glibc's and the MSVC runtime's `assert` print the failed expression's string
# literal, so this substring appears on every platform that can run the canary.
if(NOT canaryText MATCHES "read-op slot")
    message(FATAL_ERROR
        "read-slot-guard-gate: the canary died (exit '${canaryResult}') but said nothing about the read-op "
        "slot, so it died of something other than the guard. A non-zero exit is not proof; the diagnostic is")
endif()

message(STATUS "read-slot-guard-gate: the read-slot guard was watched refusing a double-arm")
