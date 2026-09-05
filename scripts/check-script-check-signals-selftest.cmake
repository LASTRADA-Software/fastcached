# SPDX-License-Identifier: Apache-2.0
#
# `script-check-signals` is watched refusing, against synthesised trees.
#
# ## Why this exists at all, and why it arrives with #680
#
# That check has run green since #497 and had never been seen to REFUSE anything.
# A guard nobody has watched refuse is not a guard -- the same argument
# `script-check-canary` makes for the FAIL_REGULAR_EXPRESSION mechanism, one
# level up and applied to the check's own rules rather than to ctest's plumbing.
#
# #680 widened pass 3 to the two CPack hook scripts, which no ctest registration
# names, and that widening is exactly the kind of change whose failure mode is
# silence: a scan that stops matching reports the same "all clear" as one that
# matched everything. Two empty lists agree perfectly.
#
# ## What it drives, and why these four
#
# One control that must PASS, then four defects that must each be refused with
# ONE finding attributed to the right file:
#
#   strip       a hook loses its `cmake_minimum_required` -- the rule itself
#   rename      the hook VARIABLE is renamed, so the scan matches nothing
#   repoint     the hook variable names a script that is not there
#   semicolon   the hook path contains a `;`
#
# `rename` is the one that matters most and is the least obvious: it is the arm
# that fails CLOSED. Without it a renamed `CPACK_*_BUILD_SCRIPTS` would take the
# whole pass quietly out of service and the check would go on reporting success.
#
# `semicolon` is here because this check found the defect in ITSELF. CMake splits
# a `;` inside a `list(APPEND)` argument into two elements, so a single violation
# printed as two lines and was COUNTED as two -- a check miscounting its own
# output, caught only because an arm reported a number nobody could explain. The
# finding COUNT is therefore asserted, not just the presence of a refusal.
#
# ## The verdict is read from the OUTPUT
#
# This script is registered with FAIL_REGULAR_EXPRESSION like every other, so it
# is subject to the rule it tests -- the same position
# `check-script-check-signals.cmake` takes about itself.
#
# NOT for the reason that was written here first. "message(FATAL_ERROR) in script
# mode exits 0 on CMake 3.28" was the stated ground for the whole mechanism and
# **it does not reproduce** -- #565 measured exit 1 on 3.22.6, 3.25.2, 3.27.9,
# 3.28.3, 3.31.6 and 4.3.0. The property stays because two OTHER things are true
# and were never the stated reason: `message(WARNING)` exits 0 on every version
# while printing `CMake Warning`, which only an output verdict can hear (#517),
# and a script that shells out to another CMake without reading RESULT_VARIABLE
# exits 0 while its child's error is on the output.
#
# That second reason is this file exactly: it runs the check under test through
# `execute_process` and reads the OUTPUT for `CMake Error`. Its own exit status
# says nothing about what it found, which is why the registration cannot rely on
# one -- so the corrected reason is not merely more accurate here, it is the one
# that actually applies.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-script-check-signals-selftest.cmake

cmake_minimum_required(VERSION 3.28)

# CMP0219 -- "macro invocations preserve backslashes in arguments" -- exists only
# on newer CMake than this file's declared minimum, so it is set behind
# `if(POLICY ...)`. Unset, a macro called with an argument containing a backslash
# emits a POLICY WARNING, and `FASTCACHED_SCRIPT_CHECK_FAILED` matches
# `CMake Warning` deliberately (#517: a check that warns is a check nobody reads).
# So the warning alone takes this test red on the platform whose CMake has the
# policy -- measured on `macOS-clang-release`, where all five cases reported
# `every verdict as expected` and ctest failed it anyway.
#
# The policy is belt to the braces below. The real fix was to stop passing a
# FILE'S CONTENTS through a macro at all; this covers the arguments that remain
# (a scratch path, which carries backslashes on Windows) and documents the hazard
# where the next reader will meet it.
if(POLICY CMP0219)
    cmake_policy(SET CMP0219 NEW)
endif()

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(checkScript "${FASTCACHED_SOURCE_DIR}/scripts/check-script-check-signals.cmake")
if(NOT EXISTS "${checkScript}")
    message(FATAL_ERROR "the check under test is missing: ${checkScript}")
endif()

# A COUNT and immediate printing, never a list of messages. The check's output
# carries semicolons, and appending it to a list re-creates the exact defect the
# `semicolon` case below exists to test -- one failure counted as several. The
# first draft of this file did that and reported "5 case(s) did not behave as
# expected" for four. A selftest that miscounts is a selftest nobody can read.
set(failureCount 0)
set(caseCount 0)

macro(fastcached_selftest_failed label detail)
    math(EXPR failureCount "${failureCount} + 1")
    message("")
    message("  FAIL  ${label}")
    message("        ${detail}")
endmacro()

# Stage a tree the check can read: the registrations it walks, the scripts they
# name, and the packaging file pass 3b discovers the CPack hooks from. A COPY of
# the real tree rather than a fabricated one, so a case cannot pass by testing a
# simplified world -- the control asserts that copy is still green.
function(fastcached_stage_tree name outVar)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/tests")
    file(COPY "${FASTCACHED_SOURCE_DIR}/scripts" DESTINATION "${tree}")
    file(COPY "${FASTCACHED_SOURCE_DIR}/cmake" DESTINATION "${tree}")
    file(COPY "${FASTCACHED_SOURCE_DIR}/src/tests/CMakeLists.txt"
         DESTINATION "${tree}/src/tests")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the REAL check against a staged tree. Its verdict is in its output, not its
# status, for the reason this whole area exists.
function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${checkScript}"
        OUTPUT_VARIABLE captured
        ERROR_VARIABLE capturedErr
        RESULT_VARIABLE ignored
    )
    set(all "${captured}${capturedErr}")
    if(all MATCHES "CMake Error")
        set(${outObjected} TRUE PARENT_SCOPE)
    else()
        set(${outObjected} FALSE PARENT_SCOPE)
    endif()
    set(${outOutput} "${all}" PARENT_SCOPE)
endfunction()

# One case: run the staged tree and check the verdict, the attribution and the
# count. `wantFinding` empty means the tree must PASS.
#
# A MACRO and not a function, so `caseCount` and `failureCount` land in the
# caller's scope without a PARENT_SCOPE dance -- and that choice has a cost worth
# naming, because it cost an hour here. A macro substitutes its arguments
# TEXTUALLY and CMake then re-parses them, so a backslash escape inside a pattern
# is consumed a second time: `"\\("` written at the call site arrives at
# `MATCHES` as a bare `(`, which is a regex group opener and fails to compile.
# The patterns below therefore carry NO backslash escapes at all -- a literal
# `(`, `.` or backtick is written as `.`, which is laxer than an escape and is
# the trade for a pattern that means the same thing at the call site and inside.
macro(fastcached_case label tree wantFinding)
    math(EXPR caseCount "${caseCount} + 1")
    fastcached_run_check("${tree}" _objected _output)

    if("${wantFinding}" STREQUAL "")
        if(_objected)
            fastcached_selftest_failed("${label}" "expected a PASS and the check objected")
            message("${_output}")
        else()
            message(STATUS "  ok    (want-pass) ${label}")
        endif()
    elseif(NOT _objected)
        fastcached_selftest_failed("${label}" "expected a refusal and the check passed")
    elseif(NOT _output MATCHES "${wantFinding}")
        fastcached_selftest_failed("${label}" "the refusal never says '${wantFinding}'")
        message("${_output}")
    elseif(NOT _output MATCHES "1 finding\\(s\\)")
        # A defect that reports as several findings is the check miscounting its
        # own output, which is how the semicolon defect was noticed at all.
        fastcached_selftest_failed("${label}" "refused, but not with exactly one finding")
        message("${_output}")
    else()
        message(STATUS "  ok    (want-fail, one finding, attributed) ${label}")
    endif()
endmacro()

# ---------------------------------------------------------------------------
# Each injection is followed by `if(before STREQUAL after)` written out rather
# than wrapped in a helper, and that is deliberate. `if(x STREQUAL y)` with bare
# variable NAMES compares their values without expanding either into an argument
# list -- so a file's contents never become arguments, which is what made the
# first version of this file emit a CMP0219 policy warning and go red on macOS
# with all five verdicts correct. Four short `if` blocks are the price of a
# construct that cannot reintroduce it.
#
# The assertion itself is not optional: an edit that matches no anchor changes
# nothing and reports success, and this file was already bitten by the
# neighbouring failure -- an anchor that matched and landed in a structurally
# invalid position -- which is why the arms assert the finding COUNT too.

# The control. Every negative case below is evidence only if this passes.
fastcached_stage_tree("control" tree)
fastcached_case("the real tree, copied unmodified" "${tree}" "")

# The rule itself: a CPack hook that declares no CMake minimum.
fastcached_stage_tree("strip" tree)
set(hook "${tree}/cmake/MacOSSignBinaries.cmake")
file(READ "${hook}" before)
string(REPLACE "cmake_minimum_required(VERSION 3.28)\n" "" after "${before}")
file(WRITE "${hook}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("strip" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a CPack hook declaring no cmake_minimum_required is refused, and named"
                "${tree}" "MacOSSignBinaries.cmake.*declares no cmake_minimum_required")

# The arm that fails CLOSED: the scan matches nothing.
fastcached_stage_tree("rename" tree)
set(packaging "${tree}/cmake/Packaging.cmake")
file(READ "${packaging}" before)
string(REPLACE "set(CPACK_POST_BUILD_SCRIPTS" "set(CPACK_POSTBUILD_SCRIPTS" after "${before}")
file(WRITE "${packaging}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("rename" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a renamed hook variable is REFUSED, not read as 'nothing to check'"
                "${tree}" "no .set.CPACK_POST_BUILD_SCRIPTS")

# A hook variable naming a script that is not there.
fastcached_stage_tree("repoint" tree)
set(packaging "${tree}/cmake/Packaging.cmake")
file(READ "${packaging}" before)
string(REPLACE "cmake/MacOSNotarizePkg.cmake" "cmake/MacOSGoneAway.cmake" after "${before}")
file(WRITE "${packaging}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("repoint" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a hook variable naming a missing script is refused"
                "${tree}" "MacOSGoneAway.cmake., which is not there")

# And a `;` in the value, which is this check's own defect turned into a case.
fastcached_stage_tree("semicolon" tree)
set(packaging "${tree}/cmake/Packaging.cmake")
file(READ "${packaging}" before)
string(REPLACE "cmake/MacOSNotarizePkg.cmake" "cmake/Gone;Away.cmake" after "${before}")
file(WRITE "${packaging}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("semicolon" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a `;` in a hook path stays ONE finding rather than splitting into two"
                "${tree}" "Gone.;Away.cmake")

# ---------------------------------------------------------------------------
if(failureCount GREATER 0)
    message("")
    message(FATAL_ERROR
        "script-check signals selftest: ${failureCount} of ${caseCount} case(s) did not "
        "behave as expected")
endif()

message(STATUS
    "script-check signals selftest: ${caseCount} synthesised tree(s), every verdict as expected")
