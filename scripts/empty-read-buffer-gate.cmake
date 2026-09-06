# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy
# the project has not stated, and this one tests strings for substrings.
cmake_minimum_required(VERSION 3.28)
#
# Run `empty-read-buffer-canary` and refuse to report green unless the empty-buffer
# guard was watched REFUSING.
#
# `Detail::RequireReadBuffer` (`src/FastCache/Net/ISocket.hpp`) is the tripwire for
# #838: `ISocket::Read` documents that its destination span must be non-empty, and
# nothing enforced it -- so a caller handed `Read` an empty span was answered `0`,
# which on this interface means the peer has finished sending. A graceful close that
# never happened, produced by the one call the type cannot express an answer for.
#
# `WILL_FAIL` alone is NOT the mechanism, for the reason `read-slot-guard-gate.cmake`
# and `iterator-debug-gate.ps1` both give about the same shape: a bare inversion cannot
# say WHY a green result is meaningless, and a non-zero exit is not proof on its own. A
# segfault, a missing shared library and a failed pair construction all exit non-zero.
# So the verdict here requires the assertion's OWN text, the same way those gates
# require "subscript out of range" and "data race".
#
# Four outcomes, kept apart on purpose -- skipped, absent, unstarted and failed are
# four states and a count collapses them:
#
#   * the canary never reached the empty read        -> nothing was watched, FAIL
#   * it reached it and SURVIVED                     -> the guard did not fire, FAIL
#   * it died without the guard's diagnostic         -> it died of something else, FAIL
#   * it died AND named the empty buffer             -> the guard refused, PASS
#
# Reports failure by printing `CMake Error`, because that is the contract every
# `cmake -P` check here is registered under -- see `src/tests/CMakeLists.txt` for the
# one spelling of the pattern that reads it, and `scripts/check-script-check-signals.cmake`
# for the measurement and the reasons, deliberately not restated here (#565).
#
# **This is the THIRD copy of the canary-gate driver, and that is stated rather than
# left to be discovered.** `scripts/read-slot-guard-gate.cmake` and
# `scripts/reactor-teardown-gate.cmake` are the other two; measured, the executable
# lines differ only in the two string literals this gate matches on. Keep it
# byte-for-byte with them apart from those, for the reason `check-psk-signing-seam.cmake`
# gives about its own copies: a copy that rewrites an escape or a spelling is equivalent
# and non-identical, which is exactly the divergence a later consolidation cannot detect.
# Consolidating the three into one parameterised gate is worth doing and is deliberately
# NOT pre-empted here -- it would touch two guards this change has no other business in,
# and a canary gate is the last thing to refactor in a hurry.
#
# One outcome none of the three distinguishes: an `execute_process` TIMEOUT produces no
# `RESULT_VARIABLE` a reader can tell from an ordinary non-zero exit. It would present
# as "died of something else", which is at least a refusal rather than a pass.
#
# Usage:
#   cmake -DFASTCACHED_CANARY=<path> -P scripts/empty-read-buffer-gate.cmake

if(NOT DEFINED FASTCACHED_CANARY)
    message(FATAL_ERROR "empty-read-buffer-gate: FASTCACHED_CANARY must be set to the canary executable")
endif()

if(NOT EXISTS "${FASTCACHED_CANARY}")
    message(FATAL_ERROR "empty-read-buffer-gate: the canary does not exist: ${FASTCACHED_CANARY}")
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

message(STATUS "empty-read-buffer-gate: canary exited '${canaryResult}'")
message(STATUS "empty-read-buffer-gate: canary said:\n${canaryText}")

# Did it get as far as the empty read at all? A canary that could not construct its
# pair, or could not stage the pending bytes, has told us nothing -- and must not read
# as the guard working.
if(NOT canaryText MATCHES "reading into an empty span")
    message(FATAL_ERROR
        "empty-read-buffer-gate: the canary never reached the empty read, so the guard was not watched at all")
endif()

if(canaryResult STREQUAL "0")
    message(FATAL_ERROR
        "empty-read-buffer-gate: the canary SURVIVED reading into an empty span. Either this build has "
        "assertions compiled out (the registration is guarded to Debug, so that is itself a defect), or "
        "Detail::RequireReadBuffer no longer guards that call site -- see issue #838 and "
        "FastCache/Net/ISocket.hpp")
endif()

# The assertion's own words. `RequireReadBuffer`'s message names the empty buffer, and
# both glibc's and the MSVC runtime's `assert` print the failed expression's string
# literal, so this substring appears on every platform that can run the canary.
if(NOT canaryText MATCHES "empty buffer")
    message(FATAL_ERROR
        "empty-read-buffer-gate: the canary died (exit '${canaryResult}') but said nothing about an empty "
        "buffer, so it died of something other than the guard. A non-zero exit is not proof; the diagnostic is")
endif()

message(STATUS "empty-read-buffer-gate: the empty-read-buffer guard was watched refusing a zero-length read")
