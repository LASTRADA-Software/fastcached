# SPDX-License-Identifier: Apache-2.0
#
# A `cmake -P` check that must be seen to fail, proving the FAIL_REGULAR_EXPRESSION
# half of the script-check failure signal.
#
# ## Why this file does not call message(FATAL_ERROR)
#
# It used to, and that made it prove nothing about the mechanism it names (#565).
#
# The reasoning it was built on was that `message(FATAL_ERROR)` in script mode
# exits 0 on CMake 3.28, so the pattern match was the only thing that could fail
# the test. That is false: `FATAL_ERROR` exits **1** on 3.28.3, in every shape --
# bare, inside `if()`, inside a function, inside `foreach()`, after other output,
# and after `cmake_minimum_required()`. Measured, one probe file per shape.
#
# With a nonzero exit the canary was over-determined. ctest failed it on the exit
# code alone, WILL_FAIL inverted that to green, and the FAIL_REGULAR_EXPRESSION
# property could have been deleted outright without the canary noticing. Measured
# on 3.28.3: `FATAL_ERROR` + WILL_FAIL + the property passes, and `FATAL_ERROR` +
# WILL_FAIL with the property REMOVED also passes. A guard that reports green in
# the state it exists to detect is not a guard.
#
# ## The shape that does discriminate
#
# Emit the pattern as ordinary output and exit **0**. Then the property is the
# only thing that can fail the test, WILL_FAIL inverts that to green, and removing
# the property turns the canary red -- which is the whole point. Measured on
# 3.28.3, the pair that matters:
#
#   pattern text + exit 0 + WILL_FAIL + property  -> Passed
#   pattern text + exit 0 + WILL_FAIL, no property -> FAILED
#
# This is the same shape `script-check-warning-canary` already had, which is why
# that one was correct and this one was not.
#
# `message()` rather than `message(FATAL_ERROR)`: the text below is a literal, not
# a diagnostic, and it is the literal the property looks for.
#
# The exit-code half of the signal is proved separately, by
# `scripts/script-check-exit-canary.cmake`. Neither canary covers both, on purpose
# -- a single canary that could pass on either signal is exactly the defect this
# file is the fix for.
message("CMake Error: script-check-canary -- this line is the point. It is deliberately printed rather than raised, so the FAIL_REGULAR_EXPRESSION property is the only thing that can fail this test. If you are reading this as a test FAILURE, that property has stopped being honoured.")
