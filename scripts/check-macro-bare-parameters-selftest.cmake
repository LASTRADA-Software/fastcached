# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy
# the project has not stated.
cmake_minimum_required(VERSION 3.28)
#
# `macro-bare-parameters` is watched refusing AND accepting, against synthesised trees.
#
# ## Why the accepting direction is half the cases here
#
# A guard nobody has watched ACCEPT is not known to work, and this tree has a two-day
# instance of exactly that (#1031: a gate that failed closed and unconditionally, with
# a confidently worded false cause, whose accepting arm had on the evidence never once
# been observed). This check is the shape where that matters most, because the thing
# it must NOT refuse -- the identical line inside a `function()`, where it is correct
# -- is far more common in this tree than the thing it must refuse.
#
# So the cases below are deliberately weighted: one violation shape per construct, and
# five separate arms asserting that correct code is left alone.
#
# ## The case that exists because the census got it wrong first
#
# `elseif` is spelled out. The census that found this defect returned ZERO on its
# first run -- including the instance already in hand -- because its pattern was
# `(el)?if\(`, which spells "elif" and "if" and never "elseif". Both known sites are
# `elseif` lines, so a check with that bug would report a clean tree.
#
# `elseif-only` therefore stages a macro whose ONLY violation is on an `elseif` line,
# with the `if` above it correct. A checker that reads `elseif` as `if` finds nothing
# and this case goes red.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-macro-bare-parameters-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of the failure signal in the
# output.

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-macro-bare-parameters.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
set(failures "")
set(caseCount 0)

# Stage a tree holding one scripts/ directory with one file in it.
#
# @param name Case name; also the directory.
# @param body The subject file's contents.
# @param outVar Set to the staged tree root.
function(StageTree name body outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/scripts")
    file(WRITE "${tree}/scripts/subject.cmake" "${body}")

    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the real check over a staged tree and report whether it OBJECTED.
function(RunCheck tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured
        ERROR_VARIABLE capturedErrors
        RESULT_VARIABLE ignored
        # ENCODING NONE: `execute_process` defaults to AUTO and decodes the child
        # through the console code page, so this would be searching a haystack its own
        # harness had rewritten.
        ENCODING NONE)
    set(combined "${captured}${capturedErrors}")
    # Flattened before matching, because CMake WRAPS its diagnostics at about 74
    # columns: a phrase can exist in the output and in no single LINE of it, and the
    # phrases below are long enough to straddle. A negative test written without this
    # reproduces nothing and reads as a refutation.
    string(REGEX REPLACE "[ \t\r\n]+" " " flattened "${combined}")

    # `CMake Error|CMake Warning`, never `CMake Error` alone: a sub-run that merely
    # WARNS changes meaning silently and would be scored a clean pass, and the ctest
    # registration this harness stands for refuses both. A harness laxer than the
    # registration it models is a fake more permissive than the thing it stands for.
    set(objected FALSE)
    if(flattened MATCHES "CMake Error|CMake Warning")
        set(objected TRUE)
    endif()
    set(${outObjected} ${objected} PARENT_SCOPE)
    set(${outOutput} "${flattened}" PARENT_SCOPE)
endfunction()

# One case. `expectation` is `refuse` or `pass`; `phrase` is a regex the FLATTENED
# output must contain, or "" to assert the verdict alone.
#
# ## A FUNCTION, and this file is the reason
#
# It was a `macro()` first, and that cost exactly what this check exists to catch --
# one level up, and in the harness FOR the rule.
#
# A macro's body is re-parsed with its arguments substituted TEXTUALLY, so a `${...}`
# inside an argument's VALUE is expanded a second time, against the macro's own
# parameters. The staged case bodies here are CMake source containing `${want}` on
# purpose, and this macro had a parameter called `want` -- so
# `if(${want} STREQUAL "yes")` was written to disk as `if(pass STREQUAL "yes")`.
#
# The case still passed. `pass` is not a parameter of the staged macro, so accepting
# it was the right verdict for the text that was actually staged -- and the text that
# was actually staged was not the text the case is named for. Four cases were staging
# a corrupted subject, and every one of them was green.
#
# Nothing found this by reading. It came out of the mutation matrix: deleting the
# `${...}` strip from the check changed NO case's verdict, and a property whose
# removal costs nothing is either dead code or a case that is not testing it.
#
# A function's parameters are variables, so `${body}` here is a value rather than
# text to re-parse, and the `${want}` inside it stays literal. `caseCount` and
# `failures` travel back by PARENT_SCOPE, which is the price and is worth it.
function(ExpectVerdict name body expectation phrase)
    math(EXPR caseCount "${caseCount} + 1")
    StageTree("${name}" "${body}" _tree)
    RunCheck("${_tree}" _objected _output)

    if("${expectation}" STREQUAL "refuse" AND NOT _objected)
        list(APPEND failures "  ${name}: expected a refusal and the check accepted")
        message("${_output}")
    elseif("${expectation}" STREQUAL "pass" AND _objected)
        list(APPEND failures "  ${name}: expected acceptance and the check refused")
        message("${_output}")
    elseif(NOT "${phrase}" STREQUAL "" AND NOT _output MATCHES "${phrase}")
        list(APPEND failures "  ${name}: the verdict was right but did not say '${phrase}', so this case passes for a reason nobody has checked")
        message("${_output}")
    else()
        message(STATUS "  ok    (want-${expectation}) ${name}")
    endif()

    set(caseCount "${caseCount}" PARENT_SCOPE)
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The marker spelled as a VALUE, never as a literal line of this file.
#
# The cases below stage exempt regions as test data. Written literally, those lines
# would sit at column zero in THIS file, inside this file's own exempt region, and
# the check would read them as a nested `data-begin` -- which it refuses, correctly.
# A `set()` line cannot be mistaken for a marker, because the marker test requires
# the marker to be the whole line.
set(dataBegin "# macro-bare-parameter-scan: data-begin")
set(dataEnd "# macro-bare-parameter-scan: data-end")

# ---------------------------------------------------------------------------
# Everything from here to the matching `data-end` is staged SUBJECT TEXT, not code
# this file runs. It is exempt from the very check this file tests: a self-test for
# a scan over CMake source holds violating CMake source as data by construction, and
# a line walk cannot tell the two apart. Exempting the REGION and not the FILE is
# what keeps the scan live over the helper functions above and the assertions below.
# macro-bare-parameter-scan: data-begin

# The control. Every case below is evidence only if a clean tree passes.
ExpectVerdict("clean"
"macro(fastcached_thing want)
    if(\"\${want}\" STREQUAL \"yes\")
        message(STATUS \"yes\")
    endif()
endmacro()
fastcached_thing(yes)
" "pass" "1 macro definition")

# The rule itself, on an `if`.
ExpectVerdict("bare-if"
"macro(fastcached_thing want)
    if(want STREQUAL \"yes\")
        message(STATUS \"yes\")
    endif()
endmacro()
" "refuse" "macro parameter .want. is read as a BARE name")

# The rule on an `elseif`, which is where BOTH real instances were and which a
# checker spelling the pattern `(el)?if` cannot see.
ExpectVerdict("elseif-only"
"macro(fastcached_thing want other)
    if(\"\${other}\" STREQUAL \"a\")
        message(STATUS \"a\")
    elseif(want STREQUAL \"b\")
        message(STATUS \"b\")
    endif()
endmacro()
" "refuse" "macro parameter .want. is read as a BARE name")

# And on a `while`, the third construct that reads a bare name as a variable.
ExpectVerdict("bare-while"
"macro(fastcached_thing want)
    while(want)
        break()
    endwhile()
endmacro()
" "refuse" "macro parameter .want. is read as a BARE name")

# ---------------------------------------------------------------------------
# The accepting direction: five shapes that must be left alone.

# The same line inside a `function()`, where a parameter IS a variable and the code
# is correct. This is the arm that matters most -- it is far more common in this tree
# than the violation, so a check that refused it would be refusing correct code
# everywhere.
ExpectVerdict("function-is-fine"
"function(fastcached_thing want)
    if(want STREQUAL \"yes\")
        message(STATUS \"yes\")
    endif()
endfunction()
macro(fastcached_other thing)
    message(STATUS \"\${thing}\")
endmacro()
" "pass" "2 macro definition|1 macro definition")

# The unquoted expansion, which is correct.
ExpectVerdict("bare-expansion-is-fine"
"macro(fastcached_thing want)
    if(\${want} STREQUAL \"yes\")
        message(STATUS \"yes\")
    endif()
endmacro()
" "pass" "")

# A name that merely CONTAINS a parameter name. The match is on whole words, so a
# checker without boundaries would refuse this.
ExpectVerdict("substring-is-fine"
"macro(fastcached_thing want)
    if(wanted STREQUAL \"yes\")
        message(STATUS \"yes\")
    endif()
    if(unwanted STREQUAL \"yes\")
        message(STATUS \"yes\")
    endif()
endmacro()
" "pass" "")

# `ARGN` is a real variable inside a macro, so reading it bare is correct even though
# it appears in the parameter list position of every variadic macro.
ExpectVerdict("argn-is-fine"
"macro(fastcached_thing want ARGN)
    if(ARGN)
        message(STATUS \"has extra\")
    endif()
    if(\"\${want}\" STREQUAL \"yes\")
        message(STATUS \"yes\")
    endif()
endmacro()
" "pass" "")

# A bare name AFTER the macro has closed is an ordinary variable read at file scope.
ExpectVerdict("outside-macro-is-fine"
"macro(fastcached_thing want)
    message(STATUS \"\${want}\")
endmacro()
if(want STREQUAL \"yes\")
    message(STATUS \"file scope\")
endif()
" "pass" "")

# ---------------------------------------------------------------------------
# The marker, both ways.

ExpectVerdict("marked-with-reason"
"macro(fastcached_thing status)
    execute_process(COMMAND \${CMAKE_COMMAND} -E true RESULT_VARIABLE status)
    # macro-bare-parameter: `status` here is the RESULT_VARIABLE the line above sets,
    # not the parameter of the same name.
    if(status EQUAL 0)
        message(STATUS \"ok\")
    endif()
endmacro()
" "pass" "1 site.s. marked")

ExpectVerdict("marked-without-reason"
"macro(fastcached_thing status)
    # macro-bare-parameter:
    if(status EQUAL 0)
        message(STATUS \"ok\")
    endif()
endmacro()
" "refuse" "with no reason after it")

# ---------------------------------------------------------------------------
# The two controls, which are the arms nobody thinks to stage.

# A macro definition this parser cannot read leaves its whole body unexamined. That
# must be its own outcome and not a quiet skip, or the run reports the same all-clear
# over a body it never looked at.
ExpectVerdict("unreadable-definition"
"macro(fastcached_thing
       want)
    if(want STREQUAL \"yes\")
        message(STATUS \"yes\")
    endif()
endmacro()
" "refuse" "cannot read the macro definition")

# And a tree with no macro at all: the scan has nothing to examine, which is
# indistinguishable from a scan that stopped recognising `macro(` unless it says so.
ExpectVerdict("no-macros-at-all"
"function(fastcached_thing want)
    message(STATUS \"\${want}\")
endfunction()
" "refuse" "found no macro definition at all")

# ---------------------------------------------------------------------------
# The exempt region itself. A self-test's staged bodies are violating source held as
# DATA, so without this the check reports findings in its own fixtures and refuses a
# correct tree -- measured before the region existed, on a two-macro staged body.
#
# The tree carries a real macro alongside the exempt one, because exempting the only
# macro in a tree trips the `no macro definition at all` control instead, and a case
# that passes by way of a DIFFERENT refusal is not evidence for this one.
ExpectVerdict("data-region-is-exempt"
"${dataBegin}
macro(fastcached_staged beta)
    if(beta STREQUAL \"yes\")
        message(STATUS \"b\")
    endif()
endmacro()
${dataEnd}
macro(fastcached_real gamma)
    if(\"\${gamma}\" STREQUAL \"yes\")
        message(STATUS \"ok\")
    endif()
endmacro()
" "pass" "1 region.s. exempt as staged test data")

# An exemption that widens itself is the failure this check exists to prevent, one
# level up: every line below an unclosed `data-begin` is skipped while the run
# reports the same all-clear.
ExpectVerdict("unterminated-data-region"
"macro(fastcached_real gamma)
    if(\"\${gamma}\" STREQUAL \"yes\")
        message(STATUS \"ok\")
    endif()
endmacro()
${dataBegin}
macro(fastcached_staged beta)
    if(beta STREQUAL \"yes\")
        message(STATUS \"b\")
    endif()
endmacro()
" "refuse" "never closed")

# The mirror. A `data-end` closing nothing means the regions do not balance, and
# nobody can then say which lines this scan examined.
ExpectVerdict("stray-data-end"
"macro(fastcached_real gamma)
    if(\"\${gamma}\" STREQUAL \"yes\")
        message(STATUS \"ok\")
    endif()
endmacro()
${dataEnd}
" "refuse" "closes no open")

# macro-bare-parameter-scan: data-end
# ---------------------------------------------------------------------------

# The glob guard: a tree whose scripts/ directory holds no .cmake at all.
math(EXPR caseCount "${caseCount} + 1")
set(emptyTree "${root}/empty")
file(REMOVE_RECURSE "${emptyTree}")
file(MAKE_DIRECTORY "${emptyTree}/scripts")
RunCheck("${emptyTree}" emptyObjected emptyOutput)
if(NOT emptyObjected)
    list(APPEND failures "  empty-file-set: a tree with no scripts/*.cmake was ACCEPTED, so the check vouches for a tree it never read")
elseif(NOT emptyOutput MATCHES "vouch for every macro in the tree while reading none")
    list(APPEND failures "  empty-file-set: refused, but not by the glob guard -- ${emptyOutput}")
else()
    message(STATUS "  ok    (want-refuse) empty-file-set")
endif()

# ---------------------------------------------------------------------------
# Did the cases stage the text they are named for?
#
# ## Why this is not a read-back inside the staging
#
# The first attempt at this compared what `StageTree` was handed against what
# reached the disk. It could not fire, and was caught by arming the control below
# rather than by reading it: the corruption happens in `ExpectVerdict`, UPSTREAM of
# `StageTree`, so the two sides always agree.
#
# It is worse than that, and this is the part worth stating. While `ExpectVerdict`
# was a `macro()`, every `${...}` in a case body was substituted before the body
# reached anything -- and an expectation carried through the SAME parameter is
# rewritten in lockstep with the subject it describes. A pattern argument spelling
# `${want}` becomes `pass`, and then matches the corrupted subject perfectly. So no
# assertion phrased in terms of the harness's own parameters can see this, however
# it is written; the corruption edits the question and the answer together.
#
# What survives is a literal that names no parameter. Under textual substitution
# every `${...}` in a staged body either becomes a parameter's value or evaluates to
# empty, so the two characters `${` disappear from the staged files ENTIRELY. This
# runs at file scope, after the cases, over what is actually on disk -- outside the
# channel that does the corrupting.
file(GLOB_RECURSE stagedSubjects "${root}/*/scripts/*.cmake")
if(NOT stagedSubjects)
    message(FATAL_ERROR
        "staging: no staged subject was found under `${root}`, so the check below "
        "would pass over an empty set -- which is the one way it must not pass")
endif()

set(withExpansion 0)
foreach(subject IN LISTS stagedSubjects)
    file(READ "${subject}" stagedText)
    # `\${` is the two characters, not a variable reference.
    string(FIND "${stagedText}" "\${" expansionAt)
    if(NOT expansionAt EQUAL -1)
        math(EXPR withExpansion "${withExpansion} + 1")
    endif()
endforeach()

list(LENGTH stagedSubjects stagedCount)
if(withExpansion EQUAL 0)
    message(FATAL_ERROR
        "staging: ${stagedCount} staged file(s) and NOT ONE contains the two "
        "characters `\${`, though several cases stage a `\${...}` on purpose. That "
        "is the signature of the harness substituting its arguments textually, which "
        "silently rewrites every case body before it reaches the disk -- those cases "
        "would then be green over text nobody wrote. Is `ExpectVerdict` a "
        "`function()`?")
endif()
message(STATUS
    "  ok    (staging) ${withExpansion} of ${stagedCount} staged file(s) kept a "
    "literal `\${`")

if(failures)
    list(JOIN failures "\n" rendered)
    message("")
    message(FATAL_ERROR
        "macro-bare-parameters selftest: ${caseCount} case(s) ran and these did not "
        "behave as expected:\n${rendered}\n")
endif()

message(STATUS
    "macro-bare-parameters selftest: ${caseCount} synthesised tree(s), every verdict as expected")
