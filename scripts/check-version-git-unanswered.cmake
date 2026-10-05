# SPDX-License-Identifier: Apache-2.0
#
# A git query that does not answer is never read as its negative (cmake/Version.cmake).
#
# The version comes from three git queries -- the nearest tag (the triple), the distance
# from it (the string) and the dirty check (the string's suffix). A dirty check that timed
# out once read as "clean" and a distance that timed out as "on the tag", each reported under
# the Exact severity, so `FASTCACHED_REQUIRE_EXACT_VERSION` could not catch either. Now the
# row a missing answer belongs to fails or says `-dirty-unknown`, every unanswered query
# makes the result Provisional, and each warning names what ITS query lost.
#
# This drives the real `GetVersionInformation` against a STUB git: a CMake script behind a
# `.cmd` on Windows and a shell script elsewhere, answering the five queries Version.cmake
# asks with canned output and HOLDING one chosen query past an injected timeout of a few
# seconds. An unknown query is logged and refused, so a query Version.cmake starts asking is
# noticed here rather than answered by accident. Each case asserts the resolved triple, the
# string, the severity of the final report and the phrase of the warning; the all-answer
# cases are the positive control that the stub RAN, since only it can produce their string.
#
# Usage: cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_SCRATCH_DIR=<dir> -P <this>
#   (internally re-entered as the driver with -DFC_ROLE=driver, and as the stub with
#   -DFC_ROLE=stub)

cmake_minimum_required(VERSION 3.28)

# Seconds a query may take here, and how long a HELD query holds. The bound is a few seconds
# rather than one because every answer costs a CMake start-up on a loaded host, and an
# all-answer case must never time out by accident; the hold outlasts it with margin.
set(timeoutSeconds 4)
set(holdSeconds 8)

# ---- the stub: one git invocation ----------------------------------------------------------
if(FC_ROLE STREQUAL "stub")
    # The arguments after `--`.
    set(args "")
    set(seenSeparator FALSE)
    foreach(index RANGE 1 ${CMAKE_ARGC})
        if(seenSeparator AND DEFINED CMAKE_ARGV${index})
            list(APPEND args "${CMAKE_ARGV${index}}")
        elseif("${CMAKE_ARGV${index}}" STREQUAL "--")
            set(seenSeparator TRUE)
        endif()
    endforeach()
    list(JOIN args " " query)

    # One row per query Version.cmake asks: <label>|<query>|<answer>. The label is what a
    # case names to hold that query; `@DIRTY@` is replaced by the dirty answer.
    set(rows
        "tag|describe --tags --abbrev=0 --match v[0-9]*|v1.2.3"
        "distance|describe --tags --match v[0-9]*|v1.2.3-4-gabc1234"
        "dirty|status --porcelain --untracked-files=no|@DIRTY@"
        "inside|rev-parse --is-inside-work-tree|true"
        "commit|rev-parse --short HEAD|abc1234")
    set(label "")
    foreach(row IN LISTS rows)
        string(FIND "${row}" "|" first)
        string(SUBSTRING "${row}" 0 ${first} rowLabel)
        math(EXPR afterFirst "${first} + 1")
        string(SUBSTRING "${row}" ${afterFirst} -1 rest)
        string(FIND "${rest}" "|" second)
        string(SUBSTRING "${rest}" 0 ${second} rowQuery)
        math(EXPR afterSecond "${second} + 1")
        string(SUBSTRING "${rest}" ${afterSecond} -1 rowAnswer)
        if(rowQuery STREQUAL query)
            set(label "${rowLabel}")
            set(answer "${rowAnswer}")
        endif()
    endforeach()
    file(APPEND "${LOG}" "${label}: ${query}\n")
    # Refusals exit non-zero through FATAL_ERROR rather than cmake_language(EXIT), which is
    # newer than this project's declared CMake minimum.
    if(label STREQUAL "")
        file(APPEND "${LOG}" "UNKNOWN QUERY: ${query}\n")
        message(FATAL_ERROR "stub: no row for `git ${query}`")
    endif()
    if(label STREQUAL "$ENV{FC_STUB_HOLD}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${holdSeconds})
    endif()
    # Dying is the WRAPPER's to do, since a CMake script can end itself only with an exit
    # status: this leaves the marker, and the wrapper ends the way a crash does.
    if(label STREQUAL "$ENV{FC_STUB_DIE}")
        file(WRITE "${LOG}.die" "")
        return()
    endif()
    # The ordinary non-zero answers: no tag, and no repository at all -- where every query
    # exits non-zero, as git does outside a work tree.
    if(label STREQUAL "tag" AND "$ENV{FC_STUB_NO_TAG}" STREQUAL "1")
        message(FATAL_ERROR "stub: no names found")
    endif()
    if("$ENV{FC_STUB_NOT_REPO}" STREQUAL "1")
        message(FATAL_ERROR "stub: not a git repository")
    endif()
    set(dirtyAnswer "")
    if("$ENV{FC_STUB_DIRTY}" STREQUAL "1")
        set(dirtyAnswer " M src/x.cpp")
    endif()
    string(REPLACE "@DIRTY@" "${dirtyAnswer}" answer "${answer}")
    if(NOT answer STREQUAL "")
        # To STDOUT, which is what Version.cmake reads; message() writes to stderr.
        execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${answer}")
    endif()
    return()
endif()

# ---- the driver: one version resolution ----------------------------------------------------
if(FC_ROLE STREQUAL "driver")
    set(FastCachedGitTimeoutSeconds ${timeoutSeconds})
    include("${FASTCACHED_SOURCE_DIR}/cmake/Version.cmake")
    GetVersionInformation(FASTCACHED_VERSION FASTCACHED_VERSION_STRING)
    message("RESOLVED triple=${FASTCACHED_VERSION} string=${FASTCACHED_VERSION_STRING}")
    return()
endif()

# ---- the check -----------------------------------------------------------------------------
foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}")
file(MAKE_DIRECTORY "${FASTCACHED_SCRATCH_DIR}")
set(self "${CMAKE_CURRENT_LIST_FILE}")
set(log "${FASTCACHED_SCRATCH_DIR}/queries.log")

# The stub executable. A `.cmd` on Windows, because execute_process starts a file, not a
# script, and a shell script elsewhere; both hand every argument to this file as the stub,
# pass its exit status through, and DIE when it leaves `<log>.die` -- with the NTSTATUS of
# an access violation on Windows and SIGSEGV elsewhere, which CMake reports in words
# ("Access violation", "Segmentation fault") rather than as a status.
set(dieMarker "${log}.die")
if(CMAKE_HOST_WIN32)
    set(stub "${FASTCACHED_SCRATCH_DIR}/git.cmd")
    file(TO_NATIVE_PATH "${CMAKE_COMMAND}" nativeCmake)
    file(TO_NATIVE_PATH "${dieMarker}" nativeDieMarker)
    file(WRITE "${stub}"
        "@\"${nativeCmake}\" -DFC_ROLE=stub \"-DLOG=${log}\" -P \"${self}\" -- %*\r\n"
        "@if exist \"${nativeDieMarker}\" (del \"${nativeDieMarker}\" & exit /b -1073741819)\r\n"
        "@exit /b %ERRORLEVEL%\r\n")
    set(deathWords "Access violation")
else()
    set(stub "${FASTCACHED_SCRATCH_DIR}/git")
    file(WRITE "${stub}"
        "#!/bin/sh\n\"${CMAKE_COMMAND}\" -DFC_ROLE=stub \"-DLOG=${log}\" -P \"${self}\" -- \"$@\"\n"
        "status=$?\n"
        "if [ -f \"${dieMarker}\" ]; then rm -f \"${dieMarker}\"; kill -SEGV $$; fi\n"
        "exit $status\n")
    file(CHMOD "${stub}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    set(deathWords "Segmentation fault")
endif()

set(failures "")
set(cases 0)

# A second tree whose path carries a space and a `(`, as `Program Files (x86)` does: the
# severity is read out of `CMake Warning at <path>:<line> (message):`, and a reader that
# stopped at the first `(` read every such path as no severity at all.
set(parenthesisedTree "${FASTCACHED_SCRATCH_DIR}/tree (x86)")
file(COPY "${FASTCACHED_SOURCE_DIR}/cmake/Version.cmake" DESTINATION "${parenthesisedTree}/cmake")

# Resolve once and assert what came out.
# @param label what the case establishes
# @param hold the query the stub holds past the timeout, or `none`
# @param environment extra `NAME=value` pairs for the stub, or `none`
# @param exact ON to configure as a publishing job does
# @param wantTriple the triple, or `-` when the resolution must refuse
# @param wantString the string, or `-`
# @param wantSeverity STATUS, WARNING or FATAL_ERROR, for the final `[Version]` report
# @param wantPhrase text the output must carry, or `none`
# @param notPhrase text the output must NOT carry, or `none`
# @param SOURCE the tree whose cmake/Version.cmake is driven; the real one when absent
# @param GIT the git to run instead of the stub; the stub when absent
function(fc_case label hold environment exact wantTriple wantString wantSeverity wantPhrase notPhrase)
    cmake_parse_arguments(PARSE_ARGV 9 case "" "SOURCE;GIT" "")
    math(EXPR count "${cases} + 1")
    set(cases ${count} PARENT_SCOPE)
    set(sourceDir "${FASTCACHED_SOURCE_DIR}")
    if(case_SOURCE)
        set(sourceDir "${case_SOURCE}")
    endif()
    set(git "${stub}")
    if(case_GIT)
        set(git "${case_GIT}")
    endif()
    file(REMOVE "${log}")
    set(stubEnvironment "FC_STUB_HOLD=${hold}")
    if(NOT environment STREQUAL "none")
        list(APPEND stubEnvironment ${environment})
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${stubEnvironment}
                "${CMAKE_COMMAND}" -DFC_ROLE=driver "-DFASTCACHED_SOURCE_DIR=${sourceDir}"
                "-DGIT_EXECUTABLE=${git}" "-DFASTCACHED_REQUIRE_EXACT_VERSION=${exact}" -P "${self}"
        WORKING_DIRECTORY "${FASTCACHED_SCRATCH_DIR}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE status)
    set(output "${out}${err}")
    string(REPLACE "\n" " " flat "${output}")
    string(REGEX REPLACE "[ \t]+" " " flat "${flat}")

    set(queries "")
    if(EXISTS "${log}")
        file(READ "${log}" queries)
    endif()
    set(problems "")
    if(queries MATCHES "UNKNOWN QUERY")
        string(APPEND problems "\n    the stub was asked a query it has no row for")
    endif()
    if(queries STREQUAL "" AND NOT case_GIT)
        string(APPEND problems "\n    the stub logged nothing, so git was never the stub")
    endif()

    # The final report's severity, read from how CMake rendered that one message: a STATUS
    # line, or the `CMake Warning at` / `CMake Error at` header NEAREST before the report's
    # `(message): [Version]`. Searched backwards from the report rather than matched across the
    # path, because a path may carry spaces and `(`, and a pattern spanning it could reach back
    # to an EARLIER message's header -- one of the query warnings above the report.
    set(gotSeverity "none")
    string(FIND "${flat}" "(message): [Version] " reportAt)
    if(flat MATCHES "-- \\[Version\\] [^ ]+ from ")
        set(gotSeverity "STATUS")
    elseif(NOT reportAt EQUAL -1)
        string(SUBSTRING "${flat}" 0 ${reportAt} beforeReport)
        string(FIND "${beforeReport}" "CMake Warning at " warningAt REVERSE)
        # verdict-error-only: this finds which ONE header introduces the final `[Version]`
        # report, to name its severity. The WARNING header is searched on the line above and
        # the nearer of the two wins, so a sub-run that merely warns is read as WARNING here,
        # never as a clean pass; and every case states the severity it expects.
        string(FIND "${beforeReport}" "CMake Error at " errorAt REVERSE)
        if(warningAt GREATER errorAt)
            set(gotSeverity "WARNING")
        elseif(errorAt GREATER warningAt)
            set(gotSeverity "FATAL_ERROR")
        endif()
    endif()
    if(NOT gotSeverity STREQUAL wantSeverity)
        string(APPEND problems "\n    final report severity ${gotSeverity}, expected ${wantSeverity}")
    endif()

    if(NOT wantTriple STREQUAL "-")
        if(NOT flat MATCHES "RESOLVED triple=([^ ]*) string=([^ ]*)")
            string(APPEND problems "\n    the driver resolved nothing")
        else()
            if(NOT CMAKE_MATCH_1 STREQUAL wantTriple)
                string(APPEND problems "\n    triple '${CMAKE_MATCH_1}', expected '${wantTriple}'")
            endif()
            if(NOT CMAKE_MATCH_2 STREQUAL wantString)
                string(APPEND problems "\n    string '${CMAKE_MATCH_2}', expected '${wantString}'")
            endif()
        endif()
    elseif(flat MATCHES "RESOLVED ")
        string(APPEND problems "\n    the resolution completed, and was expected to refuse")
    endif()

    if(NOT wantPhrase STREQUAL "none")
        string(FIND "${flat}" "${wantPhrase}" at)
        if(at EQUAL -1)
            string(APPEND problems "\n    the output does not carry '${wantPhrase}'")
        endif()
    endif()
    if(NOT notPhrase STREQUAL "none")
        string(FIND "${flat}" "${notPhrase}" at)
        if(NOT at EQUAL -1)
            string(APPEND problems "\n    the output carries '${notPhrase}', which this case must not")
        endif()
    endif()
    # No warning may still say the version falls back when it did not.
    if(flat MATCHES "; version falls back")
        string(APPEND problems "\n    the output carries the old blanket 'version falls back'")
    endif()

    if(problems STREQUAL "")
        message("  ok   ${label}")
    else()
        set(failures "${failures}\n  FAIL ${label}:${problems}\n  -- queries --\n${queries}  -- output --\n${output}" PARENT_SCOPE)
    endif()
endfunction()

# ---- every query answers: the positive control ---------------------------------------------
# Each also asserts that no git warning was printed at all.
fc_case("every query answers, clean: the tag row is Exact" none none OFF
    1.2.3 1.2.3-4-gabc1234 STATUS "from git tag (v1.2.3)" "fastcached: git")
fc_case("every query answers, dirty: the string says so" none FC_STUB_DIRTY=1 OFF
    1.2.3 1.2.3-4-gabc1234-dirty STATUS "from git tag (v1.2.3)" "fastcached: git")
fc_case("every query answers under REQUIRE_EXACT: nothing to refuse" none none ON
    1.2.3 1.2.3-4-gabc1234 STATUS "from git tag (v1.2.3)" "fastcached: git")

# ---- git answers "no", which is an answer ----------------------------------------------------
# An untagged checkout, a shallow clone without tags, a tarball built where git is installed:
# the most common Provisional paths. A non-zero exit there IS the answer, so no query may be
# reported as unanswered, and nothing may blame the index or git itself.
fc_case("no tag and everything else answers: the commit row, with no git warning" none FC_STUB_NO_TAG=1 OFF
    0.0.0 0.0.0-0-gabc1234 WARNING "from git commit without a matching tag" "fastcached: git")
fc_case("not a git repository: the declared fallback, with no git warning" none FC_STUB_NOT_REPO=1 OFF
    0.0.0 0.0.0-unknown WARNING "from declared fallback" "fastcached: git")

# ---- one query does not answer -------------------------------------------------------------
fc_case("the dirty check does not answer: -dirty-unknown, never clean, and Provisional" dirty none OFF
    1.2.3 1.2.3-4-gabc1234-dirty-unknown WARNING "whether the tree has uncommitted changes is unknown" none)
fc_case("the distance does not answer: the commit row, never the bare tag" distance none OFF
    0.0.0 0.0.0-0-gabc1234 WARNING "the distance from the tag is unknown" none)
fc_case("the tag does not answer: the commit row, and the warning says the version falls back" tag none OFF
    0.0.0 0.0.0-0-gabc1234 WARNING "the release tag is unknown, so the version falls back" none)
fc_case("no tag, and the commit does not answer: the declared fallback" commit FC_STUB_NO_TAG=1 OFF
    0.0.0 0.0.0-unknown WARNING "the commit is unknown" none)

# ---- git cannot run at all: unavailable, not slow ----------------------------------------------
# A cached GIT_EXECUTABLE that has moved. It still fails closed -- the version is Provisional --
# but its words name the binary, and nothing blames the Windows/WSL index re-hash.
fc_case("git cannot be run: the declared fallback, worded as git being unavailable" none none OFF
    0.0.0 0.0.0-unknown WARNING "git could not run `git describe --tags --abbrev=0 --match v[0-9]*`" "re-hashes"
    GIT "${FASTCACHED_SCRATCH_DIR}/no-such-git/git")

# ---- git dies: neither an exit status nor a way of not starting ------------------------------
# A crash or a signal lands in the catch-all row, worded with CMake's own words for the death.
# Before that row every non-timeout was "could not run", which named GIT_EXECUTABLE for a git
# that had started, so this case asserts that wording is ABSENT.
fc_case("git dies on the tag query: the commit row, in CMake's words, never 'could not run'" none FC_STUB_DIE=tag OFF
    0.0.0 0.0.0-0-gabc1234 WARNING "git ended `git describe --tags --abbrev=0 --match v[0-9]*` without an answer (${deathWords})" "GIT_EXECUTABLE")

# ---- under REQUIRE_EXACT, any unanswered query refuses --------------------------------------
fc_case("REQUIRE_EXACT refuses an unanswered dirty check" dirty none ON
    - - FATAL_ERROR "git did not answer `git status --porcelain --untracked-files=no`" none)
fc_case("REQUIRE_EXACT refuses an unanswered distance" distance none ON
    - - FATAL_ERROR "git did not answer `git describe --tags --match v[0-9]*`" none)

# ---- the severity is read from a path carrying `(` ------------------------------------------
fc_case("a source path with `(`: WARNING is still read as WARNING" dirty none OFF
    1.2.3 1.2.3-4-gabc1234-dirty-unknown WARNING "whether the tree has uncommitted changes is unknown" none
    SOURCE "${parenthesisedTree}")
fc_case("a source path with `(`: FATAL_ERROR is still read as FATAL_ERROR" dirty none ON
    - - FATAL_ERROR "git did not answer `git status --porcelain --untracked-files=no`" none
    SOURCE "${parenthesisedTree}")

# Every case this file calls must have run: a refactor that returned early would otherwise
# print a smaller count and pass, having judged less. The number is DERIVED from the calls.
file(STRINGS "${self}" calls REGEX "^fc_case\\(")
list(LENGTH calls called)
message("version-git-unanswered: ${cases} case(s) run")
if(NOT cases EQUAL called)
    message(FATAL_ERROR "version-git-unanswered ran ${cases} of the ${called} case(s) it calls")
endif()
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "version-git-unanswered failed:${failures}")
endif()
