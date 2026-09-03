# SPDX-License-Identifier: Apache-2.0
#
# A `cmake -P` check that MUST be seen to WARN.
#
# Sibling of `script-check-canary.cmake`, which covers the `CMake Error` half of
# `FASTCACHED_SCRIPT_CHECK_FAILED`. This covers the `CMake Warning` half, added by
# #517 so that no script check may emit CMake warnings.
#
# That guard is silent forever on a clean tree, which is exactly the shape of a
# property that can rot unnoticed: a typo in the alternation, a CTest that stops
# honouring FAIL_REGULAR_EXPRESSION, a future edit that drops the second branch.
# The checks would then be free to warn again and nothing would say so.
#
# So one check exists whose only job is to warn. It is registered WILL_FAIL, so
# ctest reports it green precisely when the mechanism worked. If the pattern ever
# stops matching `CMake Warning`, this script's output no longer trips it, ctest
# sees a pass, WILL_FAIL inverts it, and this canary goes red -- naming the
# mechanism rather than whichever real check happened to start warning that day.
#
# It deliberately does NOT also fail. A `message(FATAL_ERROR)` here would print
# `CMake Error` and trip the other half of the alternation, so the canary would
# stay green even if the warning branch were deleted -- proving nothing and
# looking like proof.

cmake_minimum_required(VERSION 3.28)

message(WARNING "script-check-warning-canary: this warning is the point; if you are reading it as a test FAILURE, the CMake Warning half of FAIL_REGULAR_EXPRESSION has stopped working")
