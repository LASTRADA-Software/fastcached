# SPDX-License-Identifier: Apache-2.0
#
# A `cmake -P` check that MUST be seen to fail, and that can only fail one way.
#
# ## What the mechanism is, and why it needs a canary at all
#
# Every hygiene check in src/tests/CMakeLists.txt reports failure by printing
# `CMake Error` or `CMake Warning`, and ctest hears that through a per-test
# property, `FAIL_REGULAR_EXPRESSION "${FASTCACHED_SCRIPT_CHECK_FAILED}"`. A
# property is a thing that can be silently wrong: a typo'd pattern, a CTest that
# stops honouring it, a refactor that drops it. The checks would then pass
# unconditionally, which is the state this whole area exists to leave behind.
#
# So one check exists whose only job is to fail. Registered WILL_FAIL, it is
# green precisely when the mechanism worked; if the pattern ever stops matching,
# this script's run reports a pass, WILL_FAIL inverts it, and the canary goes red
# -- naming the mechanism rather than whichever real check happened to have
# something to say that day.
#
# ## Why it exits 0, which is the whole design (#565)
#
# It used to be a bare `message(FATAL_ERROR)`, on the stated ground that in
# script mode that prints `CMake Error ...` and exits **0** on CMake 3.28, this
# project's declared minimum. That is not true anywhere it has been looked for:
# it exits 1 on 3.22.6, 3.25.2, 3.27.9, 3.28.3, 3.31.6 and 4.3.0, for six
# different script shapes, with controls proving the harness could read a zero.
#
# And the correction is not cosmetic, because **the note being wrong made this
# canary vacuous.** A test fails under ctest when its exit code is non-zero OR
# the pattern matches, and WILL_FAIL inverts that whole verdict. A canary exiting
# 1 therefore passes on its exit code alone: delete the FAIL_REGULAR_EXPRESSION
# from every registration in the tree and this file would still report green,
# having proved nothing about the property it exists to prove. Measured under
# ctest with a four-case fixture -- a script exiting 1 while printing nothing the
# pattern matches PASSES under WILL_FAIL.
#
# So the canary now exits **0** and prints a real `CMake Error`, which leaves the
# pattern as the only thing that can fail it. The way it does that is the one
# condition that genuinely produces `CMake Error` on the output of a process that
# exits 0: a nested `cmake -P` whose `RESULT_VARIABLE` is not read. That is a
# real hazard for any check that shells out to another CMake, so the canary is
# now shaped like the failure it guards against rather than like a version claim
# that did not hold.
#
# The child is this same file, re-entered with `-D`, rather than a second script
# beside it: two files can drift, and one of them would be a `.cmake` in
# `scripts/` that no registration mentions.
#
# Same idea as iterator-debug-canary and tsan-canary: a program that must die.

cmake_minimum_required(VERSION 3.28)

if(DEFINED SCRIPT_CHECK_CANARY_INNER)
    message(FATAL_ERROR "script-check-canary: this failure is the point; if you are reading it as a test FAILURE, the FAIL_REGULAR_EXPRESSION mechanism has stopped working")
endif()

# RESULT_VARIABLE is deliberately absent, and OUTPUT_/ERROR_VARIABLE with it: the
# child's diagnostic has to reach ctest's captured output, and this process has
# to end successfully so that the pattern is the only remaining verdict. Adding a
# RESULT_VARIABLE here and acting on it would restore the vacuous shape.
execute_process(COMMAND "${CMAKE_COMMAND}" -DSCRIPT_CHECK_CANARY_INNER=1 -P "${CMAKE_CURRENT_LIST_FILE}")
