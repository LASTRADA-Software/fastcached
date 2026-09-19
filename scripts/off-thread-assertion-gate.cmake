# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy
# the project has not stated, and this one tests strings for substrings.
cmake_minimum_required(VERSION 3.28)
#
# Read `off-thread-assertion-canary`'s run and report green exactly while
# `OffThreadAssertionGuard` (#1211) was watched BOTH accepting an assertion on the case's
# thread AND ending the process at one made on a helper thread.
#
# The guard exists because an off-thread Catch2 assertion damaged the heap a few runs in a
# hundred, and the crash then landed somewhere else. A guard that never fires would bring
# exactly that back, silently -- and so would one that is no longer attached, since it
# arrives through the Catch2 link and not through a list. The canary names no guard of
# its own, so this is also the check that the link still carries it.
#
# **Not a bare `WILL_FAIL`**, for the reason `write-slot-guard-gate.cmake` gives: a
# non-zero exit is not proof. It would accept a segfault, a missing shared library and a
# Catch2 that refused its command line alike. The gate requires the guard's own words, and
# keeps "never got that far", "survived" and "died of something else" apart from "the
# guard ended it".
#
# **And it requires the ACCEPT marker first.** A guard that ended the process at EVERY
# assertion would pass a refusal-only gate and break every test binary in the tree.
#
# Usage:
#   cmake -DFASTCACHED_CANARY=<path> -P scripts/off-thread-assertion-gate.cmake
if(NOT DEFINED FASTCACHED_CANARY)
    message(FATAL_ERROR "off-thread-assertion-gate: FASTCACHED_CANARY must be set to the canary executable")
endif()
if(NOT EXISTS "${FASTCACHED_CANARY}")
    message(FATAL_ERROR "off-thread-assertion-gate: the canary does not exist: ${FASTCACHED_CANARY}")
endif()

# Merged, because the canary's progress markers and the guard's message both go to stderr
# and the ORDER between them is what this gate reads.
execute_process(
    COMMAND "${FASTCACHED_CANARY}"
    RESULT_VARIABLE canaryResult
    OUTPUT_VARIABLE canaryOut
    ERROR_VARIABLE canaryErr
    TIMEOUT 90
)
set(canaryText "${canaryOut}${canaryErr}")
message(STATUS "off-thread-assertion-gate: canary exited '${canaryResult}'")
message(STATUS "off-thread-assertion-gate: canary said:\n${canaryText}")

# THE POSITIVE CONTROL FIRST. A guard nobody has watched accept is not known to work
# (#1031): one that objected to every assertion would pass everything below.
if(NOT canaryText MATCHES "an assertion on the case's thread was ACCEPTED")
    message(FATAL_ERROR
        "off-thread-assertion-gate: the canary never reported an assertion on its own thread being accepted, so "
        "the guard was never watched ACCEPTING. Either it now ends the process at every assertion -- which "
        "would fail every test binary in the tree -- or the canary never got that far. Read its output above")
endif()

# Did it get as far as the helper thread at all? A canary that stopped before it has told
# us nothing, and must not read as the guard working.
if(NOT canaryText MATCHES "asserting from a helper thread")
    message(FATAL_ERROR
        "off-thread-assertion-gate: the canary never reached its helper thread, so the guard was not watched "
        "refusing")
endif()

if(canaryText MATCHES "SURVIVED an assertion off the case's thread" OR canaryResult STREQUAL "0")
    message(FATAL_ERROR
        "off-thread-assertion-gate: the canary SURVIVED an assertion on a helper thread (exit '${canaryResult}'). "
        "Either OffThreadAssertionGuard no longer ends the process there, or it no longer reaches this binary: "
        "it arrives as an INTERFACE source of the Catch2 target (the top-level CMakeLists.txt), and "
        "`ctest -R off-thread-assertion-guard-coverage` says which executables carry it. See issue #1211")
endif()

# The guard's own words. A non-zero exit is not proof; the diagnostic is.
if(NOT canaryText MATCHES "ran on a thread that is not the one running test case")
    message(FATAL_ERROR
        "off-thread-assertion-gate: the canary died (exit '${canaryResult}') but said nothing the guard says, so "
        "it died of something other than the guard. A non-zero exit is not proof; the diagnostic is")
endif()

message(STATUS
    "off-thread-assertion-gate: the guard was watched accepting an assertion on the case's thread AND ending "
    "the process at one on a helper thread")
