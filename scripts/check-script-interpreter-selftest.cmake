# SPDX-License-Identifier: Apache-2.0
#
# `script-interpreter` watched refusing, against synthesised registration files.
#
# ## Why both directions, and why the control is not decoration
#
# #379's acceptance asks for the check shown refusing a bare-`bash` registration
# AND shown passing on the converted tree. The second half is the one that is
# easy to skip and easy to fake: a check that refuses everything also refuses a
# bare `bash`, and only a control that must PASS separates a guard from a
# tripwire.
#
# The control is the REAL `src/tests/CMakeLists.txt`, copied unmodified. A
# synthesised minimal file would let a case pass by testing a simplified world --
# and the site this check exists for, `set(_smoke_driver ...)`, is exactly the
# kind of thing a hand-written fixture would not have thought to include.
#
# ## The five refusals, and why each is here
#
#   bare        a `COMMAND bash` registration -- #379's own subject
#   literal     a `COMMAND /bin/bash` -- correct today, broken on Windows the day
#               a row moves, so the check pins the VARIABLE and not the value
#   nointerp    `COMMAND "x.sh"` with no interpreter token at all. It must say
#               THAT rather than report `COMMAND` as the interpreter: naming the
#               wrong half of a real defect is #791, and it sends a maintainer to
#               look for something that is not there
#   nodef       the `FASTCACHED_BASH` definition removed. Every site then expands
#               to no interpreter, the scripts still RUN from their shebang, and
#               every test stays green while the thing this pins is unpinned --
#               a green suite proving nothing
#   empty       no script registrations match at all. Two empty lists agree
#               perfectly, so an emptiness refusal is what keeps a scan that
#               stopped matching from reporting success
#
# **Every arm asserts the finding COUNT.** A defect that reports as several
# findings is the check miscounting its own output (#796), and this file's subject
# has already done it once: a raw `;` in a violation string made one finding print
# and count as two.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-script-interpreter-selftest.cmake

cmake_minimum_required(VERSION 3.28)

# See `check-script-check-signals-selftest.cmake` for why this is guarded: the
# policy exists only on newer CMake, and unset it WARNS on a macro argument
# containing a backslash -- which the registration's FAIL_REGULAR_EXPRESSION
# matches on purpose, so the warning alone would take this red with nothing wrong.
if(POLICY CMP0219)
    cmake_policy(SET CMP0219 NEW)
endif()

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(checkScript "${FASTCACHED_SOURCE_DIR}/scripts/check-script-interpreter.cmake")
set(realRegistrations "${FASTCACHED_SOURCE_DIR}/src/tests/CMakeLists.txt")
foreach(needed checkScript realRegistrations)
    if(NOT EXISTS "${${needed}}")
        message(FATAL_ERROR "missing input: ${${needed}}")
    endif()
endforeach()

set(failureCount 0)
set(caseCount 0)

macro(fastcached_selftest_failed label detail)
    math(EXPR failureCount "${failureCount} + 1")
    message("")
    message("  FAIL  ${label}")
    message("        ${detail}")
endmacro()

# Stage the one file the check reads. `tree` is a scratch source root.
function(fastcached_stage name outVar)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/tests")
    file(COPY "${realRegistrations}" DESTINATION "${tree}/src/tests")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

function(fastcached_run tree outObjected outOutput)
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

# `wantSaying` empty means the tree must PASS. Patterns carry NO backslash
# escapes: a macro re-parses its arguments, so an escape is consumed twice and a
# `\(` arrives at MATCHES as a group opener that fails to compile. A literal is
# written as `.`, which is laxer and means the same thing at both ends.
macro(fastcached_case label tree wantSaying)
    math(EXPR caseCount "${caseCount} + 1")
    fastcached_run("${tree}" _objected _output)

    if("${wantSaying}" STREQUAL "")
        if(_objected)
            fastcached_selftest_failed("${label}" "expected a PASS and the check objected")
            message("${_output}")
        else()
            message(STATUS "  ok    (want-pass) ${label}")
        endif()
    elseif(NOT _objected)
        fastcached_selftest_failed("${label}" "expected a refusal and the check passed")
    elseif(NOT _output MATCHES "${wantSaying}")
        fastcached_selftest_failed("${label}" "the refusal never says '${wantSaying}'")
        message("${_output}")
    elseif(NOT _output MATCHES "1 finding.s. across")
        fastcached_selftest_failed("${label}" "refused, but not with exactly one finding")
        message("${_output}")
    else()
        message(STATUS "  ok    (want-fail, one finding, attributed) ${label}")
    endif()
endmacro()

# One injection, asserted to have staged something. An edit that matches no
# anchor changes nothing and reports success.
function(fastcached_inject tree from to outOk)
    set(file "${tree}/src/tests/CMakeLists.txt")
    file(READ "${file}" before)
    string(REPLACE "${from}" "${to}" after "${before}")
    file(WRITE "${file}" "${after}")
    if(before STREQUAL after)
        set(${outOk} FALSE PARENT_SCOPE)
    else()
        set(${outOk} TRUE PARENT_SCOPE)
    endif()
endfunction()

set(oneSite "COMMAND \${FASTCACHED_BASH} \"\${CMAKE_SOURCE_DIR}/scripts/ci-scope-test.sh\"")

# ---------------------------------------------------------------------------
# The control: the real registration file, unmodified, must PASS.
fastcached_stage("control" tree)
fastcached_case("the real registration file, copied unmodified" "${tree}" "")

fastcached_stage("bare" tree)
fastcached_inject("${tree}" "${oneSite}"
                  "COMMAND bash \"\${CMAKE_SOURCE_DIR}/scripts/ci-scope-test.sh\"" staged)
if(NOT staged)
    fastcached_selftest_failed("bare" "the injection changed nothing, so the case stages no defect")
else()
    fastcached_case("a bare `bash` registration is refused, and named" "${tree}"
                    "runs a shell script as .bash. rather than")
endif()

fastcached_stage("literal" tree)
fastcached_inject("${tree}" "${oneSite}"
                  "COMMAND /bin/bash \"\${CMAKE_SOURCE_DIR}/scripts/ci-scope-test.sh\"" staged)
if(NOT staged)
    fastcached_selftest_failed("literal" "the injection changed nothing, so the case stages no defect")
else()
    fastcached_case("a literal `/bin/bash` is refused too -- the VARIABLE is what is pinned" "${tree}"
                    "runs a shell script as ./bin/bash. rather than")
endif()

fastcached_stage("nointerp" tree)
fastcached_inject("${tree}" "${oneSite}"
                  "COMMAND \"\${CMAKE_SOURCE_DIR}/scripts/ci-scope-test.sh\"" staged)
if(NOT staged)
    fastcached_selftest_failed("nointerp" "the injection changed nothing, so the case stages no defect")
else()
    fastcached_case("a site with NO interpreter token says that, not that `COMMAND` is one" "${tree}"
                    "NO interpreter token before it")
endif()

fastcached_stage("nodef" tree)
fastcached_inject("${tree}" "set(FASTCACHED_BASH " "set(FASTCACHED_BASH_RENAMED " staged)
if(NOT staged)
    fastcached_selftest_failed("nodef" "the injection changed nothing, so the case stages no defect")
else()
    fastcached_case("the definition removed is refused -- every site would expand to no interpreter" "${tree}"
                    "defines no .FASTCACHED_BASH")
endif()

fastcached_stage("empty" tree)
fastcached_inject("${tree}" "/scripts/" "/no-such-dir/" staged)
if(NOT staged)
    fastcached_selftest_failed("empty" "the injection changed nothing, so the case stages no defect")
else()
    fastcached_case("a scan matching nothing is REFUSED, not read as 'all clear'" "${tree}"
                    "found no script-driven test registrations")
endif()

# ---------------------------------------------------------------------------
# A finding whose LINE contains a semicolon is ONE finding.
#
# Every other want-fail case here asserts "one finding" over a line with no `;`, so none
# of them can see the #796 miscount -- and it came back: converting the accumulator from
# a macro to a function (to fix a message that could not name its own token) expanded the
# stored list unquoted, which consumed the escapes the same function had just added, and
# one finding was counted as two. Measured: 3 findings where 2 were staged.
#
# The check already escapes on the way in; what this pins is that the escape and the
# EXPANSION agree, which is the half that was not being thought about.
# TWO defects, and the first carries the `;`. One is not enough and that is the whole
# subtlety: the corruption damages elements ALREADY in the list when a later one is
# appended, so a single finding is stored and read back intact under the bug. The first
# version of this case staged one defect, asserted "one finding", and passed with the
# mutation installed -- a regression test that could not reproduce its regression, found
# by neutering rather than by reading it.
#
# So this case does not go through `fastcached_case`, whose count assertion is hard-wired
# to one. It asserts TWO.
fastcached_stage("semicolon" tree)
fastcached_inject("${tree}" "${oneSite}"
                  "COMMAND bash \"\${CMAKE_SOURCE_DIR}/scripts/ci-scope-test.sh\" \; tail" stagedA)
fastcached_inject("${tree}" "COMMAND \${FASTCACHED_BASH} \"\${CMAKE_SOURCE_DIR}/scripts/flake-rate.sh\""
                  "COMMAND bash \"\${CMAKE_SOURCE_DIR}/scripts/flake-rate.sh\"" stagedB)
math(EXPR caseCount "${caseCount} + 1")
if(NOT stagedA OR NOT stagedB)
    fastcached_selftest_failed("semicolon"
        "an injection changed nothing (A=${stagedA} B=${stagedB}), so the case stages fewer than the two defects it needs")
else()
    fastcached_run("${tree}" _semiObjected _semiOutput)
    if(NOT _semiObjected)
        fastcached_selftest_failed("semicolon" "expected a refusal and the check passed")
    elseif(NOT _semiOutput MATCHES "2 finding.s. across")
        fastcached_selftest_failed("semicolon"
            "two defects were staged, one of them on a line carrying a `;`, and the check did not report exactly two findings -- the accumulator is re-splitting stored elements (#796)")
        message("${_semiOutput}")
    else()
        message(STATUS "  ok    (want-fail, TWO findings) a `;` in one finding does not split another")
    endif()
endif()

# ---------------------------------------------------------------------------
# The EXEMPTION, both directions (#598).
#
# An exemption is a hole unless it is narrow, and "narrow" here means it permits exactly
# ONE substitute token for exactly one script. The control above already covers the
# accepting direction -- the real file carries the exempt site and must pass -- so what
# is left is the direction that would make it a hole: the exempt script run as something
# the row does not name. Without this case an exemption keyed on the FILE alone passes
# every test in this file while readmitting a literal `/bin/bash` or a PATH-resolved
# `bash`, which is the whole of what this check refuses.
fastcached_stage("exempt-wrong-token" tree)
fastcached_inject("${tree}"
    "COMMAND \${FASTCACHED_BASH44} \"\${CMAKE_SOURCE_DIR}/scripts/tidy-sweep.sh\""
    "COMMAND /bin/bash \"\${CMAKE_SOURCE_DIR}/scripts/tidy-sweep.sh\"" staged)
if(NOT staged)
    fastcached_selftest_failed("exempt-wrong-token"
        "the injection changed nothing -- the exempt registration is not spelled as this case expects, so it stages no defect")
else()
    fastcached_case("an exempt script run as a token its row does not name is refused" "${tree}"
                    "exemption names ONE permitted token")
endif()

# And the exemption must not leak to a script it does not name. A row keyed loosely --
# on a substring, say -- would exempt every neighbour whose path contains the name.
#
# Injected at `oneSite`, which is registered exactly once, because the harness asserts a
# single finding. The first version of this case rewrote `run-check.sh` and produced many
# -- a correct refusal that failed the case for arithmetic rather than for behaviour.
fastcached_stage("exempt-does-not-leak" tree)
fastcached_inject("${tree}" "${oneSite}"
    "COMMAND \${FASTCACHED_BASH44} \"\${CMAKE_SOURCE_DIR}/scripts/ci-scope-test.sh\"" staged)
if(NOT staged)
    fastcached_selftest_failed("exempt-does-not-leak"
        "the injection changed nothing, so the case stages no defect")
else()
    fastcached_case("a NON-exempt script may not use the exempt token either" "${tree}"
                    "rather than")
endif()

# ---------------------------------------------------------------------------
if(failureCount GREATER 0)
    message("")
    message(FATAL_ERROR
        "script interpreter selftest: ${failureCount} of ${caseCount} case(s) did not "
        "behave as expected")
endif()

message(STATUS
    "script interpreter selftest: ${caseCount} synthesised tree(s), every verdict as expected")
