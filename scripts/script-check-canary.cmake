# SPDX-License-Identifier: Apache-2.0
#
# A `cmake -P` check that MUST be seen to fail.
#
# Every hygiene check in src/tests/CMakeLists.txt reports failure by printing
# `CMake Error`, because `message(FATAL_ERROR)` in script mode exits 0 on CMake
# 3.28 -- this project's declared minimum. That makes the whole failure signal a
# ctest property (FAIL_REGULAR_EXPRESSION), and a property is a thing that can be
# silently wrong: a typo'd pattern, a CTest that stops honouring it, a future
# refactor that drops it. The checks would then pass unconditionally again, which
# is exactly the state this file was written to leave.
#
# So one check exists whose only job is to fail. It is registered WILL_FAIL, so
# ctest reports it green precisely when the mechanism worked. If the pattern ever
# stops matching, this script exits 0, ctest sees a pass, WILL_FAIL inverts it,
# and the canary goes red -- naming the mechanism rather than whichever real
# check happened to have something to say that day.
#
# Same idea as iterator-debug-canary: a program that must die.
message(FATAL_ERROR "script-check-canary: this failure is the point; if you are reading it as a test FAILURE, the FAIL_REGULAR_EXPRESSION mechanism has stopped working")
