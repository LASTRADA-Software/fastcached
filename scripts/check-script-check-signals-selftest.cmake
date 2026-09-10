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
# ONE finding attributed to the right file, then the control AGAIN over the same
# tree -- see `fastcached_stage_tree` for why there is one tree and what the
# closing control is for:
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
#
# ONE tree for every case, mutated and restored, rather than one copy per case
# (#1137). The copy is 158 files, and on a Windows checkout reached over DrvFs it
# is what this test COSTS: five copies were 81% of a 63 s run, against 8% for the
# five nested `cmake -P` invocations the ticket's hypothesis named. Measured
# 2026-09-10 on WSL2 / Ubuntu-24.04 / 32 CPUs, load average 0.08 at the start,
# min of three, timed with CLOCK_MONOTONIC because CLOCK_REALTIME is stepped in
# both directions on that host:
#
#   whole selftest, five copies    DrvFs 63.26 s     ext4  3.01 s
#   ONE staging (file COPY x158)   DrvFs 10.24 s     ext4  0.01 s
#   the check under test, once     DrvFs  1.03 s     ext4  0.56 s
#   `cmake -P` process floor       DrvFs  0.017 s    ext4  0.004 s
#
# Those conditions are PINNED rather than pointed at: they are the state of one
# host at one instant and must not silently start describing a different one.
#
# What one tree costs is that a case which fails to restore leaves the next case
# reading a tree nobody described. Two independent guards, because the restore is
# the whole risk: every case asserts its own file came back byte-identical, and
# the control is run AGAIN at the end over the same tree -- which is strictly
# more than the per-copy arrangement could say, since that one never checked that
# an injection had been undone at all.
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
    # `CMake Error|CMake Warning`, never `CMake Error` alone: a sub-run that merely WARNS
    # changes meaning silently and, read for the error word alone, is scored a clean pass
    # (#672). Stated in full -- and enforced -- in `scripts/check-script-check-signals.cmake`.
    if(all MATCHES "CMake Error|CMake Warning")
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
# @param ARGN `no-count` when the expected refusal is a positive CONTROL firing rather
#        than a finding. A control reports no findings at all, so asserting `1 finding(s)`
#        on one would fail it for being exactly what it is.
macro(fastcached_case label tree wantFinding)
    set(_wantCount TRUE)
    if("${ARGN}" STREQUAL "no-count")
        set(_wantCount FALSE)
    endif()
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
    elseif(_wantCount AND NOT _output MATCHES "1 finding\\(s\\)")
        # A defect that reports as several findings is the check miscounting its
        # own output, which is how the semicolon defect was noticed at all.
        fastcached_selftest_failed("${label}" "refused, but not with exactly one finding")
        message("${_output}")
    else()
        set(_shape "one finding, attributed")
        if(NOT _wantCount)
            set(_shape "a control firing, attributed")
        endif()
        message(STATUS "  ok    (want-fail, ${_shape}) ${label}")
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
#
# The RESTORE carries an assertion of its own, and for a different reason than
# the injection's. An injection that changes nothing stages no defect and the
# case passes vacuously; a restore that does not land leaves the NEXT case
# reading a tree the file does not describe, and its verdict then belongs to no
# stated arrangement. Both are read back with bare variable NAMES -- `if(a
# STREQUAL b)` compares two values without expanding either into an argument
# list, which is what keeps a file's contents out of a macro call and out of
# CMP0219's way.

# ONE tree, for the reason `fastcached_stage_tree` gives.
fastcached_stage_tree("tree" tree)

# The control. Every negative case below is evidence only if this passes.
fastcached_case("the real tree, copied unmodified" "${tree}" "")

# The rule itself: a CPack hook that declares no CMake minimum.
set(hook "${tree}/cmake/MacOSSignBinaries.cmake")
file(READ "${hook}" before)
string(REPLACE "cmake_minimum_required(VERSION 3.28)\n" "" after "${before}")
file(WRITE "${hook}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("strip" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a CPack hook declaring no cmake_minimum_required is refused, and named"
                "${tree}" "MacOSSignBinaries.cmake.*declares no cmake_minimum_required")
file(WRITE "${hook}" "${before}")
file(READ "${hook}" restored)
if(NOT restored STREQUAL before)
    fastcached_selftest_failed("strip" "the tree was not restored, so every later case reads an undescribed tree")
endif()

# The arm that fails CLOSED: the scan matches nothing.
set(packaging "${tree}/cmake/Packaging.cmake")
file(READ "${packaging}" before)
string(REPLACE "set(CPACK_POST_BUILD_SCRIPTS" "set(CPACK_POSTBUILD_SCRIPTS" after "${before}")
file(WRITE "${packaging}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("rename" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a renamed hook variable is REFUSED, not read as 'nothing to check'"
                "${tree}" "no .set.CPACK_POST_BUILD_SCRIPTS")
file(WRITE "${packaging}" "${before}")
file(READ "${packaging}" restored)
if(NOT restored STREQUAL before)
    fastcached_selftest_failed("rename" "the tree was not restored, so every later case reads an undescribed tree")
endif()

# A hook variable naming a script that is not there.
file(READ "${packaging}" before)
string(REPLACE "cmake/MacOSNotarizePkg.cmake" "cmake/MacOSGoneAway.cmake" after "${before}")
file(WRITE "${packaging}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("repoint" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a hook variable naming a missing script is refused"
                "${tree}" "MacOSGoneAway.cmake., which is not there")
file(WRITE "${packaging}" "${before}")
file(READ "${packaging}" restored)
if(NOT restored STREQUAL before)
    fastcached_selftest_failed("repoint" "the tree was not restored, so every later case reads an undescribed tree")
endif()

# And a `;` in the value, which is this check's own defect turned into a case.
file(READ "${packaging}" before)
string(REPLACE "cmake/MacOSNotarizePkg.cmake" "cmake/Gone;Away.cmake" after "${before}")
file(WRITE "${packaging}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("semicolon" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a `;` in a hook path stays ONE finding rather than splitting into two"
                "${tree}" "Gone.;Away.cmake")
file(WRITE "${packaging}" "${before}")
file(READ "${packaging}" restored)
if(NOT restored STREQUAL before)
    fastcached_selftest_failed("semicolon" "the tree was not restored, so every later case reads an undescribed tree")
endif()

# ---------------------------------------------------------------------------
# Pass 5 (#672): the three ways it has to be seen behaving.
#
# It is the pass whose subject is a SPELLING, so all three failures are silent by
# construction -- a harness reading half the signal passes everything it is given, a
# marker with no reason still looks marked, and a detector that has stopped
# recognising the shape prints the same summary as one that read every file.

# The rule itself: a harness that reads only the error word.
set(harness "${tree}/scripts/check-glob-traversals-selftest.cmake")
file(READ "${harness}" before)
string(REPLACE "MATCHES \"CMake Error|CMake Warning\"" "MATCHES \"CMake Error\"" after "${before}")
file(WRITE "${harness}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("half-read" "the injection changed nothing, so the case stages no defect")
endif()
# The label spells the word as `CMake` + `Error` deliberately. It is printed on the
# SUCCESS path, and this test is registered with
# FAIL_REGULAR_EXPRESSION "${FASTCACHED_SCRIPT_CHECK_FAILED}" -- so a label carrying
# the literal would take the test red on a tree with nothing wrong, on ctest only,
# which is not where this file is usually run from. Same family as a checker
# reporting its own remediation example.
fastcached_case("a harness reading only the error half of the signal is refused, and named"
                "${tree}" "check-glob-traversals-selftest.cmake:.*not for .CMake Warning")
file(WRITE "${harness}" "${before}")
file(READ "${harness}" restored)
if(NOT restored STREQUAL before)
    fastcached_selftest_failed("half-read" "the tree was not restored, so every later case reads an undescribed tree")
endif()

# An exception marker carrying no reason. That is how "forgot" would come to be spelled
# in the vocabulary of "decided", so it is refused rather than counted.
set(marked "${tree}/scripts/check-fatal-error-exit.cmake")
file(READ "${marked}" before)
string(REPLACE
       "# verdict-error-only: this asks whether the `fatal` arm's own `message(FATAL_ERROR)`"
       "# verdict-error-only:" after "${before}")
file(WRITE "${marked}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("bare-marker" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a `verdict-error-only:` marker with no reason is refused"
                "${tree}" "check-fatal-error-exit.cmake:.*no reason after it")
file(WRITE "${marked}" "${before}")
file(READ "${marked}" restored)
if(NOT restored STREQUAL before)
    fastcached_selftest_failed("bare-marker" "the tree was not restored, so every later case reads an undescribed tree")
endif()

# And the positive control, which is the arm nobody would think to stage: a tree the
# scan reads to the end and finds NO verdict reader in. That is what a detector which
# has stopped recognising the shape looks like, and it reports zero findings -- so it
# is indistinguishable from a clean tree by every other assertion in this file.
#
# A SYNTHESISED tree rather than a mutated copy, and the reason is cost: blinding the
# real copy means rewriting every file under scripts/ that carries the word, and this
# fixture already measures its staging as the thing it costs on DrvFs. Six small files
# satisfy passes 1 through 4 and carry the word nowhere.
set(blind "${FASTCACHED_SCRATCH_DIR}/no-verdict-readers")
file(REMOVE_RECURSE "${blind}")
file(MAKE_DIRECTORY "${blind}/scripts")
file(MAKE_DIRECTORY "${blind}/src/tests")
file(MAKE_DIRECTORY "${blind}/cmake")
file(WRITE "${blind}/scripts/subject.cmake" "cmake_minimum_required(VERSION 3.28)\n")
file(WRITE "${blind}/scripts/canary.cmake"
     "cmake_minimum_required(VERSION 3.28)\nmessage(STATUS \"a canary that exits 0\")\n")
file(WRITE "${blind}/cmake/pre.cmake" "cmake_minimum_required(VERSION 3.28)\n")
file(WRITE "${blind}/cmake/post.cmake" "cmake_minimum_required(VERSION 3.28)\n")
file(WRITE "${blind}/cmake/Packaging.cmake"
     "set(CPACK_PRE_BUILD_SCRIPTS \"\${CMAKE_SOURCE_DIR}/cmake/pre.cmake\")\n"
     "set(CPACK_POST_BUILD_SCRIPTS \"\${CMAKE_SOURCE_DIR}/cmake/post.cmake\")\n")
file(WRITE "${blind}/src/tests/CMakeLists.txt"
     "add_test(\n    NAME \"subject\"\n    COMMAND \${CMAKE_COMMAND} -P \"\${CMAKE_SOURCE_DIR}/scripts/subject.cmake\"\n)\n"
     "set_tests_properties(\"subject\" PROPERTIES\n    FAIL_REGULAR_EXPRESSION \"\${FASTCACHED_SCRIPT_CHECK_FAILED}\"\n)\n"
     "add_test(\n    NAME \"canary\"\n    COMMAND \${CMAKE_COMMAND} -P \"\${CMAKE_SOURCE_DIR}/scripts/canary.cmake\"\n)\n"
     "set_tests_properties(\"canary\" PROPERTIES\n    FAIL_REGULAR_EXPRESSION \"\${FASTCACHED_SCRIPT_CHECK_FAILED}\"\n    WILL_FAIL TRUE\n)\n")
fastcached_case("a tree the scan reads to the end with no verdict reader in it fires the control"
                "${blind}" "no compliant verdict reader" no-count)

# ---------------------------------------------------------------------------
# Pass 2 (#679): the rule this check was written for, and the walk that now answers
# it in one pass.
#
# Neither was covered here before. The cases above drive passes 3, 3b and 5, and
# pass 2 -- a registration that cannot report failure, which is the whole reason
# this file exists -- had never been watched refusing anything. A rewrite of an
# unwatched function is the shape worth staging a case for.

# The rule. ONE registration loses its signal, and the finding must name that one
# and only that one: a lookup that blurred two registrations together would report a
# violation against a check that has nothing wrong with it, and nothing in the
# message would say so.
set(registrations "${tree}/src/tests/CMakeLists.txt")
file(READ "${registrations}" before)
string(REPLACE
       "set_tests_properties(\"glob-traversals-selftest\" PROPERTIES\n    FAIL_REGULAR_EXPRESSION \"\${FASTCACHED_SCRIPT_CHECK_FAILED}\"\n"
       "set_tests_properties(\"glob-traversals-selftest\" PROPERTIES\n" after "${before}")
file(WRITE "${registrations}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("unsignalled" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a registration with no FAIL_REGULAR_EXPRESSION is refused, and named"
                "${tree}" ".glob-traversals-selftest. runs a .cmake -P. script but has no FAIL_REGULAR_EXPRESSION")
file(WRITE "${registrations}" "${before}")
file(READ "${registrations}" restored)
if(NOT restored STREQUAL before)
    fastcached_selftest_failed("unsignalled" "the tree was not restored, so every later case reads an undescribed tree")
endif()

# And the walk itself losing the shape it reads. The finding has to name the WALK: a
# check that answers an instrument fault with 71 findings against 71 innocent
# registrations is an instrument fault wearing the findings' clothes.
file(READ "${registrations}" before)
string(REPLACE "set_tests_properties(" "set_test_properties(" after "${before}")
file(WRITE "${registrations}" "${after}")
if(before STREQUAL after)
    fastcached_selftest_failed("blocks-unreadable" "the injection changed nothing, so the case stages no defect")
endif()
fastcached_case("a walk that stops recognising property blocks names ITSELF, not 71 innocent checks"
                "${tree}" "has stopped recognising the shape" no-count)
file(WRITE "${registrations}" "${before}")
file(READ "${registrations}" restored)
if(NOT restored STREQUAL before)
    fastcached_selftest_failed("blocks-unreadable" "the tree was not restored, so every later case reads an undescribed tree")
endif()

# The control AGAIN, over the tree every case above has now written to. The
# per-case restores each assert their own file; this asserts the WHOLE tree is
# back where the first control found it, which is the assertion that survives a
# restore written against the wrong path -- one that would compare a file nobody
# mutated against itself and agree. It is also the half the one-copy-per-case
# arrangement could not have: five pristine trees can say nothing about whether
# an injection was undone.
fastcached_case("the same tree, after every injection was reverted" "${tree}" "")

# ---------------------------------------------------------------------------
if(failureCount GREATER 0)
    message("")
    message(FATAL_ERROR
        "script-check signals selftest: ${failureCount} of ${caseCount} case(s) did not "
        "behave as expected")
endif()

message(STATUS
    "script-check signals selftest: ${caseCount} synthesised tree(s), every verdict as expected")
