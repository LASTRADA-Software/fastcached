# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy
# the project has not stated, and this one tests strings for substrings.
cmake_minimum_required(VERSION 3.28)
#
# Run `reactor-teardown-canary` and refuse to report green unless the teardown guard
# was watched REFUSING.
#
# `IReactor::TeardownIsSerialisedWithDispatch()` is the tripwire for #668: an object
# a reactor owns is destroyed on that reactor's worker thread or with the reactor
# stopped, because clearing a pending awaitable anywhere else races the completion
# dispatch. Until #668 that rule was asserted in exactly one translation unit --
# `IocpSocket.cpp`, wholly inside `#if defined(_WIN32)` -- and `Running()` /
# `IsOnWorkerThread()` existed on `IocpReactor` and on no other reactor. So the ONLY
# observer in the CI matrix was `Windows-cl-debug`, which is not a required context
# and which reported the violation intermittently: two greens and one red on
# identical source. This gate is what replaces that observer, and it runs wherever
# `assert` is live rather than only where IOCP is.
#
# `WILL_FAIL` alone is NOT the mechanism, for the reason `read-slot-guard-gate.cmake`
# and `iterator-debug-gate.ps1` both give: a bare inversion cannot say WHY a green
# result is meaningless, and a non-zero exit is not proof on its own. A segfault, a
# missing shared library and a failed thread spawn all exit non-zero.
#
# FIVE outcomes, kept apart on purpose -- skipped, absent, unstarted and failed are
# four states and a count collapses them, and the fifth here was measured rather than
# imagined:
#
#   * the predicate answered SAFE for an unsafe setup  -> the guard is weakened, FAIL
#   * the canary never established the arrangement     -> nothing was watched, FAIL
#   * it established it and SURVIVED                   -> the assert is gone, FAIL
#   * it died without the guard's diagnostic           -> it died of something else, FAIL
#   * it died AND named the completion dispatch        -> the guard refused, PASS
#
# The first two both exit 0 and would collapse into one report. They do not here,
# because weakening `TeardownIsSerialisedWithDispatch()` lands in the FIRST -- the
# canary's own arrangement check catches it before it ever reaches the assertion --
# and calling that "could not establish the arrangement" would send a reader to the
# canary's threading rather than to the guard.
#
# Reports failure by printing `CMake Error`, because that is the contract every
# `cmake -P` check here is registered under -- see `src/tests/CMakeLists.txt` for
# the one spelling of the pattern that reads it, and the measurement and the reasons
# live in `scripts/check-script-check-signals.cmake` and are deliberately not
# restated here (#565).
#
# Usage:
#   cmake -DFASTCACHED_CANARY=<path> -P scripts/reactor-teardown-gate.cmake

if(NOT DEFINED FASTCACHED_CANARY)
    message(FATAL_ERROR "reactor-teardown-gate: FASTCACHED_CANARY must be set to the canary executable")
endif()

if(NOT EXISTS "${FASTCACHED_CANARY}")
    message(FATAL_ERROR "reactor-teardown-gate: the canary does not exist: ${FASTCACHED_CANARY}")
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

message(STATUS "reactor-teardown-gate: canary exited '${canaryResult}'")
message(STATUS "reactor-teardown-gate: canary said:\n${canaryText}")

# The predicate itself answered SAFE for an arrangement that is not safe. Checked
# BEFORE the generic "never got that far" below, because it is a different diagnosis
# reaching the same exit: the canary set the arrangement up correctly and
# `TeardownIsSerialisedWithDispatch()` disagreed. Measured -- weakening that
# predicate lands here, and reporting it as "could not establish the arrangement"
# would send somebody to look at the canary's threading instead of at the guard.
if(canaryText MATCHES "predicate answered SAFE")
    message(FATAL_ERROR
        "reactor-teardown-gate: IReactor::TeardownIsSerialisedWithDispatch() answered SAFE for a reactor that "
        "was running, observed from a thread that is not its worker. The guard has been weakened or its two "
        "queries no longer report -- see issue #668 and FastCache/Async/IReactor.hpp")
endif()

# Did it get as far as the assertion at all? A canary whose reactor never started, or
# whose main thread reported as the worker, has told us nothing -- and must not read
# as the guard working. Every such path in the canary exits 0 and says which it was,
# so this check has to come BEFORE the exit-code check or a self-diagnosed abort
# would be reported as a survival.
if(NOT canaryText MATCHES "arrangement established")
    message(FATAL_ERROR
        "reactor-teardown-gate: the canary never established a running reactor observed from a non-worker "
        "thread, so the guard was not watched at all")
endif()

if(canaryResult STREQUAL "0")
    message(FATAL_ERROR
        "reactor-teardown-gate: the canary SURVIVED destroying against a running reactor from a non-worker "
        "thread. Either this build has assertions compiled out (the registration is guarded to Debug, so that "
        "is itself a defect), or IReactor::TeardownIsSerialisedWithDispatch no longer refuses -- see issue "
        "#668 and FastCache/Async/ReactorTeardown.hpp")
endif()

# The assertion's own words. Both glibc's and the MSVC runtime's `assert` print the
# failed expression's string literal, so this substring appears on every platform
# that can run the canary.
if(NOT canaryText MATCHES "races the completion dispatch")
    message(FATAL_ERROR
        "reactor-teardown-gate: the canary died (exit '${canaryResult}') but said nothing about racing the "
        "completion dispatch, so it died of something other than the guard. A non-zero exit is not proof; "
        "the diagnostic is")
endif()

message(STATUS "reactor-teardown-gate: the teardown guard was watched refusing an off-thread destruction")
