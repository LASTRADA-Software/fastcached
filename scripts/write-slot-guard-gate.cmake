# SPDX-License-Identifier: Apache-2.0
#
# Read `write-slot-guard-canary`'s run and report green exactly while the write-slot
# guard was watched BOTH accepting an ordinary pair of writes AND refusing a
# double-arm.
#
# `Detail::ClaimWriteSlot` (`FastCache/Net/WriteSlot.hpp`) is the write-side tripwire
# for #893. It shipped with its own header claiming it was watched refusing by a
# `ctest -R write-slot-guard-canary` that did not exist anywhere in the tree -- so the
# claim was the only instance of the name. This is the check that makes the sentence
# true (#1218).
#
# **Not a bare `WILL_FAIL`**, for the reason `read-slot-guard-gate.cmake` gives at
# length: an inversion cannot say WHY a green result is meaningless, and a non-zero
# exit is not proof on its own. It would accept a segfault, a missing shared library
# and a refused bind alike. The gate requires the assertion's own words, and keeps
# "never got that far", "survived" and "died of something else" apart from "the guard
# refused".
#
# **And it requires the ACCEPT marker before the abort**, which the read-side gate has
# no equivalent of. A guard that refused every write would pass a refusal-only gate
# while breaking every honest caller; the failure would surface days later somewhere
# else and read as a different defect. Ordering is what makes the two observations one
# run rather than two claims: the accept is printed by the same process, before the
# abort it did not survive.
#
# Usage:
#   cmake -DFASTCACHED_CANARY=<path> -P scripts/write-slot-guard-gate.cmake

if(NOT DEFINED FASTCACHED_CANARY)
    message(FATAL_ERROR "write-slot-guard-gate: FASTCACHED_CANARY must be set to the canary executable")
endif()

if(NOT EXISTS "${FASTCACHED_CANARY}")
    message(FATAL_ERROR "write-slot-guard-gate: the canary does not exist: ${FASTCACHED_CANARY}")
endif()

# Merged, because glibc's `assert` writes to stderr while the canary's own progress
# markers go there too, and the ORDER between them is what this gate reads.
execute_process(
    COMMAND "${FASTCACHED_CANARY}"
    RESULT_VARIABLE canaryResult
    OUTPUT_VARIABLE canaryOut
    ERROR_VARIABLE canaryErr
    TIMEOUT 90
)
set(canaryText "${canaryOut}${canaryErr}")

message(STATUS "write-slot-guard-gate: canary exited '${canaryResult}'")
message(STATUS "write-slot-guard-gate: canary said:\n${canaryText}")

# THE POSITIVE CONTROL FIRST. A guard nobody has watched accept is not known to work
# (#1031), and this is the arm that would catch a `ClaimWriteSlot` that objected to
# every write rather than only to a double-arm.
if(NOT canaryText MATCHES "two sequential writes were ACCEPTED")
    message(FATAL_ERROR
        "write-slot-guard-gate: the canary never reported an ordinary pair of writes being accepted, so the "
        "guard was never watched ACCEPTING. Either it now refuses honest sequential writes -- which would "
        "break every caller -- or the connection never got that far. Read the canary output above; a refusal "
        "arm passing on its own says nothing (#1031)")
endif()

# Did it get as far as the double-arm at all? A canary that could not bind, or whose
# client never arrived, has told us nothing -- and must not read as the guard working.
if(NOT canaryText MATCHES "arming a Write over a parked Write")
    message(FATAL_ERROR
        "write-slot-guard-gate: the canary never reached the double-arm, so the guard was not watched refusing")
endif()

if(canaryResult STREQUAL "0")
    message(FATAL_ERROR
        "write-slot-guard-gate: the canary SURVIVED arming a Write over a parked Write. Either this build has "
        "assertions compiled out (the registration is guarded to Debug, so that is itself a defect), or "
        "Detail::ClaimWriteSlot no longer guards that arm site -- see issue #893 and FastCache/Net/WriteSlot.hpp")
endif()

# The assertion's own words. `ClaimWriteSlot`'s message names the write-op slot, and
# both glibc's and the MSVC runtime's `assert` print the failed expression's string
# literal, so this substring appears on every platform that can run the canary.
if(NOT canaryText MATCHES "write-op slot")
    message(FATAL_ERROR
        "write-slot-guard-gate: the canary died (exit '${canaryResult}') but said nothing about the write-op "
        "slot, so it died of something other than the guard. A non-zero exit is not proof; the diagnostic is")
endif()

message(STATUS "write-slot-guard-gate: the write-slot guard was watched accepting a sequential pair AND refusing a double-arm")
