# SPDX-License-Identifier: Apache-2.0
#
# `check-pedantic-suppressions.cmake` driven against synthetic subject files, so
# every verdict it can reach has been watched.
#
# The check's whole job is to REFUSE, and a check that refuses nothing on a clean
# tree is indistinguishable from one that cannot refuse at all. It was watched
# failing on 0db96dc8 before the fix landed -- naming exactly the four `-Wno-`
# suppressions, twice, once per GNU-style persona -- and that observation is a fact
# about one commit, which stops being reproducible the moment the fix is in. This
# is the part that keeps reproducing.
#
# Each case is a whole synthetic `PedanticCompiler.cmake`, not a patch of the real
# one: the real file is what the check is FOR, and building the negative cases out
# of it would make this test pass or fail for reasons belonging to that file rather
# than to the rule.
#
# Every case is driven by a nested `cmake -P` whose `RESULT_VARIABLE` is READ. An
# unread one exits 0 carrying its child's `CMake Error` on the output, which is the
# shape `script-check-canary` is built out of -- and inside a want-fail assertion
# any way of not running is indistinguishable from the rule firing. The verdict is
# taken from the OUTPUT as well, because that is how ctest hears these checks and a
# check that only WARNS exits 0.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(checkScript "${FASTCACHED_SOURCE_DIR}/scripts/check-pedantic-suppressions.cmake")
set(probeDir "${FASTCACHED_SOURCE_DIR}/scripts/pedantic-flag-probe")

# The verdict ctest reads, passed in by the registration from the single
# definition in src/tests/CMakeLists.txt rather than respelled here. A second
# spelling of a wire constant can drift from the property that actually judges the
# check, and then this file certifies a verdict nobody is using.
if(NOT DEFINED FASTCACHED_SCRIPT_CHECK_FAILED)
    message(FATAL_ERROR
        "FASTCACHED_SCRIPT_CHECK_FAILED must be passed in from src/tests/CMakeLists.txt, which is "
        "where it is defined.\n\n"
        "This used to carry a literal fallback copy of the pattern, which is the very thing the "
        "paragraph above argues against: if the canonical pattern widens and the hand-over is "
        "dropped or misspelled, a fallback takes over SILENTLY and this file goes on certifying a "
        "verdict nobody reads. A refusal is the only version of that safety net that cannot be "
        "wrong quietly.")
endif()

if(NOT EXISTS "${checkScript}")
    message(FATAL_ERROR "the check under test is not there: ${checkScript}")
endif()

if(NOT DEFINED FASTCACHED_SELFTEST_DIR)
    set(FASTCACHED_SELFTEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/pedantic-suppressions-selftest")
endif()
file(REMOVE_RECURSE "${FASTCACHED_SELFTEST_DIR}")
file(MAKE_DIRECTORY "${FASTCACHED_SELFTEST_DIR}")

set(failures "")

# The case NAMES, not a count: a self-test that stopped early must not look like
# one that judged something (#723), and a list says WHICH ran rather than only how
# many -- which is the difference between a truncated run and a complete one.
set(casesRun "")

# The preamble every synthetic subject shares: the same `try_add_compile_options`
# the real file has, and the same TWO-ARM shape -- an MSVC branch and a GNU one --
# so a case differs from the real thing only in WHICH condition a flag sits under,
# which is the one variable under test.
#
# The MSVC arm is not decoration. Every persona needs its anchor flag or the check
# refuses the subject as unreadable, and a fixture with only a GNU arm would make
# every case here exercise a subject the check considers broken -- passing or
# failing for a reason that has nothing to do with the rule.
set(subjectPreamble
"include(CheckCXXCompilerFlag)
function(try_add_compile_options FLAG)
    string(REGEX REPLACE \"^[-/]\" \"\" name \${FLAG})
    string(REGEX REPLACE \":\" \"\" name \${name})
    check_cxx_compiler_flag(\${FLAG} \${name})
    if(\${name})
        add_compile_options($<$<COMPILE_LANGUAGE:CXX>:\${FLAG}>)
    endif()
endfunction()
option(PEDANTIC_COMPILER \"\" OFF)
option(PEDANTIC_COMPILER_WERROR \"\" OFF)
if(\${PEDANTIC_COMPILER})
if(\"\${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}\" STREQUAL \"MSVC\")
")

# What the MSVC arm holds, so a case can vary it. Its own parameter rather than
# part of the preamble, because one case is ABOUT that arm being empty.
set(subjectMsvcArm "    try_add_compile_options(/W4)")

# Run the check once and judge the verdict.
#
# One function, because the predicate was written three times in the first draft
# and had already drifted into two spellings -- the de Morgan'd form in the two
# ad-hoc blocks read the same and is a separate thing to keep in step. Three copies
# of one predicate is what the check under test exists to complain about one file
# over.
# @param 1 Case name, used in the report.
# @param 2 `pass` or `fail` -- what this invocation must make the check say.
# @param 3 A regular expression the output must contain. Empty to assert nothing
#          beyond the verdict; for a `fail` case it is what proves the refusal was
#          the intended one and not some other refusal arriving first.
# @param ARGN The `-D` arguments naming what this invocation drives.
function(pedantic_selftest_run caseName expectation wantedText)
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            "-DFASTCACHED_SOURCE_DIR=${FASTCACHED_SOURCE_DIR}"
            "-DFASTCACHED_PEDANTIC_PROBE_DIR=${probeDir}"
            ${ARGN}
            -P "${checkScript}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE stdoutText
        ERROR_VARIABLE stderrText
    )
    set(combined "${stdoutText}${stderrText}")

    # Matched against the output with its whitespace flattened, never the raw text.
    # `message(FATAL_ERROR ...)` is WORD-WRAPPED by CMake at about 76 columns with
    # indented continuations, so where a line breaks is a function of how long the
    # strings interpolated into it are -- and two of these messages begin with a
    # PATH. Measured before this: with FASTCACHED_SELFTEST_DIR 17-24 characters the
    # absent-probe-stub case failed, at 28-48 the absent-subject case failed, and at
    # 52 or more both passed. `cmake -B /tmp/b` would have failed this test on a
    # tree with nothing wrong, while the repository's own deep
    # `out/build/<preset>/...` path is long enough to pass -- so CI stays green and
    # only some developers ever see the red, and what it prints is "the verdict was
    # right but its wording did not match", which accuses the check of drift when
    # the only variable is $PWD.
    #
    # This is the wrapped-diagnostic hazard (#787), arriving inside a selftest
    # written to hold a rule about instruments that report on the wrong thing.
    string(REGEX REPLACE "[ \t\r\n]+" " " flattened "${combined}")

    # The same verdict ctest reads: a non-zero status OR the failure wording on the
    # output. Reading only the status would miss a check that warns, and reading
    # only the output would miss one that dies before printing.
    set(refused FALSE)
    if(NOT status EQUAL 0 OR combined MATCHES "${FASTCACHED_SCRIPT_CHECK_FAILED}")
        set(refused TRUE)
    endif()

    if(expectation STREQUAL "fail" AND NOT refused)
        list(APPEND failures "  ${caseName}: the check ACCEPTED a subject that violates the rule")
    elseif(expectation STREQUAL "pass" AND refused)
        list(APPEND failures "  ${caseName}: the check REFUSED a subject that obeys the rule -- ${combined}")
    elseif(NOT "${wantedText}" STREQUAL "" AND NOT flattened MATCHES "${wantedText}")
        list(APPEND failures
             "  ${caseName}: the verdict was right but its wording did not match `${wantedText}`, so this case is passing for a reason nobody has checked -- ${combined}")
    endif()
    list(APPEND casesRun "${caseName}")

    set(failures "${failures}" PARENT_SCOPE)
    set(casesRun "${casesRun}" PARENT_SCOPE)
endfunction()

# Write one synthetic subject and drive the check against it.
# @param 1 Case name, which is also the file name.
# @param 2 `pass` or `fail`.
# @param 3 A regular expression the output must contain, or empty.
# @param 4 The body of the GNU arm.
# @param ARGN What the MSVC arm holds, defaulting to `subjectMsvcArm`. Passed only
#        by the case that is about that arm.
function(pedantic_selftest_case caseName expectation wantedText body)
    set(msvcArm "${subjectMsvcArm}")
    if(NOT "${ARGN}" STREQUAL "")
        set(msvcArm "${ARGN}")
    endif()
    set(subjectFile "${FASTCACHED_SELFTEST_DIR}/${caseName}.cmake")
    file(WRITE "${subjectFile}"
         "${subjectPreamble}${msvcArm}\nelse()\n${body}\nendif()\nendif()\n")
    pedantic_selftest_run("${caseName}" "${expectation}" "${wantedText}"
                          "-DFASTCACHED_PEDANTIC_FILE=${subjectFile}")
    set(failures "${failures}" PARENT_SCOPE)
    set(casesRun "${casesRun}" PARENT_SCOPE)
endfunction()

# --- the shape the rule is about ---------------------------------------------
# `-pedantic` outside, its suppression inside. This is 0db96dc8 exactly, and the
# case that must never go quiet.
pedantic_selftest_case(suppression-gated-on-werror fail "-Wno-c2y-extensions is added only when"
"    try_add_compile_options(-pedantic)
    if(\${PEDANTIC_COMPILER_WERROR})
        try_add_compile_options(-Werror)
        try_add_compile_options(-Wno-c2y-extensions)
    endif()")

# --- the fix -----------------------------------------------------------------
pedantic_selftest_case(suppression-beside-pedantic pass ""
"    try_add_compile_options(-pedantic)
    try_add_compile_options(-Wno-c2y-extensions)
    if(\${PEDANTIC_COMPILER_WERROR})
        try_add_compile_options(-Werror)
    endif()")

# --- the repair the ticket names, and the check now refuses it ---------------
# Adding the suppression outside as WELL as inside leaves two conditions naming one
# diagnostic. As SETS the difference is empty and this passed -- permanently and by
# construction -- while three places claimed the guard caught it and the check's own
# trailer said "move the flag, do not copy it". A guard that reads as enforcing
# something it cannot is worse than no guard, because the trailer sends people at
# the shape it does not catch. Counting occurrences rather than testing membership
# is what closed it, and this case is what says so.
pedantic_selftest_case(suppression-copied-not-moved fail "named under two conditions"
"    try_add_compile_options(-pedantic)
    try_add_compile_options(-Wno-c2y-extensions)
    if(\${PEDANTIC_COMPILER_WERROR})
        try_add_compile_options(-Werror)
        try_add_compile_options(-Wno-c2y-extensions)
    endif()")

# --- the same defect wearing the other sign ----------------------------------
# A diagnostic that DISAPPEARS when warnings become fatal is `WERROR` deciding
# which warnings exist just as much as one that appears.
pedantic_selftest_case(diagnostic-dropped-under-werror fail "-Wconversion is DROPPED when"
"    try_add_compile_options(-pedantic)
    if(\${PEDANTIC_COMPILER_WERROR})
        try_add_compile_options(-Werror)
    else()
        try_add_compile_options(-Wconversion)
    endif()")

# --- fatality flags may be gated ---------------------------------------------
# `-Werror` and `-Wno-error=X` are what the `WERROR` block is FOR. A check that
# refused these would be unusable and would be turned off rather than obeyed.
pedantic_selftest_case(fatality-flags-may-be-gated pass ""
"    try_add_compile_options(-pedantic)
    try_add_compile_options(-Wno-c2y-extensions)
    if(\${PEDANTIC_COMPILER_WERROR})
        try_add_compile_options(-Werror)
        try_add_compile_options(-Wno-error=c2y-extensions)
        try_add_compile_options(-Wno-error=missing-declarations)
    endif()")

# --- vacuity -----------------------------------------------------------------
# A subject that stopped adding `-pedantic` cannot exhibit the asymmetry, so a
# check that reported PASS on it would be reporting on nothing. This is the arm
# that would otherwise rot silently: everything else here still passes without it.
pedantic_selftest_case(pedantic-gone-is-not-a-pass fail "-pedantic is not added"
"    try_add_compile_options(-Wall)
    if(\${PEDANTIC_COMPILER_WERROR})
        try_add_compile_options(-Werror)
    endif()")

# A subject that adds nothing at all is the same failure one step further on.
pedantic_selftest_case(empty-subject-is-not-a-pass fail "-pedantic is not added"
"    # nothing at all")

# And the MSVC arm has its own anchor, asked for its own sake. Before the persona
# table carried an anchor COLUMN the vacuity guard was a hardcoded `-pedantic`
# test, so the MSVC personas had no anchor at all: emptying that whole arm on the
# REAL subject still reported success, because the module adds
# `-fdiagnostics-color=always` unconditionally and "recorded something" was true of
# every persona. This case is the half a GNU-only fixture cannot reach.
pedantic_selftest_case(msvc-arm-gone-is-not-a-pass fail "/W4 is not added"
"    try_add_compile_options(-pedantic)
    try_add_compile_options(-Wno-c2y-extensions)
    if(\${PEDANTIC_COMPILER_WERROR})
        try_add_compile_options(-Werror)
    endif()"
"    # the MSVC arm adds nothing")

# --- the check must notice its own inputs are missing -------------------------
# Not synthetic subjects but absent ones: `absent`, `unreadable` and `clean` are
# different states, and only a refusal keeps the third from swallowing the others.
# Through the same helper as every other case, so all nine are judged by one
# predicate.
pedantic_selftest_run(absent-subject fail "the subject file is not there"
    "-DFASTCACHED_PEDANTIC_FILE=${FASTCACHED_SELFTEST_DIR}/no-such-file.cmake")

# The probe stub is part of the instrument: without it every flag goes through a
# real `try_compile`, which script mode does not have. That must be named rather
# than presenting as a clean run.
pedantic_selftest_run(absent-probe-stub fail "the probe stub is not there"
    "-DFASTCACHED_PEDANTIC_PROBE_DIR=${FASTCACHED_SELFTEST_DIR}/no-such-dir")

# A self-test that stopped early must not look like one that judged something, so
# the cases are named whatever the verdict (#723).
list(LENGTH casesRun caseCount)
list(JOIN casesRun ", " caseNames)

if(NOT "${failures}" STREQUAL "")
    list(JOIN failures "\n" body)
    message(FATAL_ERROR
        "check-pedantic-suppressions did not behave as specified (${caseCount} case(s) run: ${caseNames}):\n${body}\n\n"
        "Each case is a synthetic `PedanticCompiler.cmake` differing only in which condition a flag "
        "sits under, or an absent input. A `fail` case that was accepted means the guard has stopped "
        "guarding; a `pass` case that was refused means it now refuses legitimate configurations, "
        "which is how a guard gets turned off rather than obeyed.")
endif()

message("pedantic suppressions selftest: ${caseCount} case(s) run, every verdict as specified\n"
        "  ${caseNames}")
