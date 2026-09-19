# SPDX-License-Identifier: Apache-2.0
#
# `check-control-bytes.cmake` is driven against staged trees, in BOTH directions.
#
# The accepting direction is the one that matters most here, and it is not decoration.
# This repository carries NO control byte today -- the check reports clean on the real
# tree -- so every case below that expects a refusal has to PLANT one, and a check that
# had silently stopped reading any file at all would pass the accepting case perfectly.
# The clean cases therefore assert the file COUNT as well as the verdict.
#
# ## Why a planted byte and not a fixture committed with one
#
# A file carrying a BEL cannot be committed to this repository -- the check under test
# refuses it, which is the point -- so the fixture has to write the byte at run time.
# `string(ASCII 7 ...)` is what produces it, the same call the check uses, and that is
# a shared premise rather than a coincidence: if `string(ASCII)` stopped producing the
# byte, the check would stop refusing and this self-test would stop planting, together
# and in silence -- every case would go green over nothing. So one case asserts that
# call produces exactly one byte.
#
# ## The fixtures are NOT written with `file(WRITE)`
#
# Measured: `file(WRITE)` writes `\n` as CRLF on this Windows host, so every staged file
# carried a CR -- which the check correctly refuses, so all four accepting cases failed
# on a correct check. The fixture was the violation.
#
# `execute_process(COMMAND ${CMAKE_COMMAND} -E echo "${body}" OUTPUT_FILE ...)` writes
# the bytes: measured, an argument carrying embedded LF, BEL and TAB comes back out of
# the file unchanged, with one LF appended. That is the whole reason the write goes
# through a subprocess, and it is worth the awkwardness -- a fixture for a check about
# BYTES cannot be written through a path that rewrites them.
#
# ## Every tree carries one file the fixture did not ask for
#
# `scripts/lib/third-party-roots.txt` has to exist, because the helper refuses a tree
# without it -- and `.txt` is in the allow-list, so it is SCANNED. Every count below
# therefore includes it, and that is stated rather than silently absorbed: a baseline of
# one is the difference between "the filter works" and "the filter matched my file".
#
# ## Read from the OUTPUT, never from the exit code
#
# `message(WARNING)` exits 0 on every CMake while printing `CMake Warning`, so an exit
# status cannot tell a refusal from a remark. Matched FLATTENED, because CMake wraps its
# diagnostics at a column that depends on the scratch path's length.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-control-bytes.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()
# The helper is NOT copied into the staged trees: the check includes it from its own
# directory, so a copy would be a second one nobody reads -- and, being a `.cmake`, it
# would be scanned and would move every count here.

string(ASCII 7 BEL)
string(ASCII 27 ESC)
string(ASCII 13 CR)
string(ASCII 9 TAB)

set(ran 0)
set(failures "")

# Write `body` to `path` as BYTES. See the header: `file(WRITE)` is not byte-faithful
# here, and this fixture's subject is bytes.
function(WriteBytes path body)
    get_filename_component(parent "${path}" DIRECTORY)
    file(MAKE_DIRECTORY "${parent}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${body}" OUTPUT_FILE "${path}")
endfunction()

# Stage a tree and run the check over it.
#
# The check reads `scripts/lib/third-party-roots.txt` through the helper, so every tree
# here plants one: a roots file that cannot be read is a refusal, and a case expecting a
# refusal would then pass for a reason that has nothing to do with control bytes.
#
# @param name The case name, which names the directory.
# @param files `path=content` pairs joined by `|`, or `-` for no staged file.
# @param extensions A replacement extension allow-list for a STAGED copy of the check,
#        or `-` to run the shipped one.
# @param outFlat Set to the combined output, flattened onto one line.
# @param outStaged Set to TRUE when the run used a staged copy of the check.
function(StageAndRun name files extensions outFlat outStaged)
    set(tree "${FASTCACHED_SCRATCH_DIR}/case-${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/scripts/lib")
    WriteBytes("${tree}/scripts/lib/third-party-roots.txt" "# planted\nvendor/upstream")

    if(NOT files STREQUAL "-")
        string(REPLACE "|" ";" entries "${files}")
        foreach(entry IN LISTS entries)
            string(FIND "${entry}" "=" at)
            if(at EQUAL -1)
                message(FATAL_ERROR "case `${name}`: staged entry has no '=': ${entry}")
            endif()
            string(SUBSTRING "${entry}" 0 ${at} relative)
            math(EXPR at "${at} + 1")
            string(SUBSTRING "${entry}" ${at} -1 body)
            WriteBytes("${tree}/${relative}" "${body}")
        endforeach()
    endif()

    set(runCheck "${check}")
    if(NOT extensions STREQUAL "-")
        file(READ "${check}" checkText)
        set(anchor "set(FastCachedControlByteExtensions")
        string(FIND "${checkText}" "${anchor}" tableAt)
        if(tableAt EQUAL -1)
            message(FATAL_ERROR
                "case `${name}`: the extension table was not found in the check, so the "
                "allow-list cannot be staged -- this case would run the SHIPPED list and "
                "pass while testing nothing.")
        endif()
        string(SUBSTRING "${checkText}" 0 ${tableAt} beforeTable)
        string(SUBSTRING "${checkText}" ${tableAt} -1 tail)
        string(FIND "${tail}" ")" closeAt)
        math(EXPR closeAt "${closeAt} + 1")
        string(SUBSTRING "${tail}" ${closeAt} -1 afterTable)
        # In `scripts/`, with the helper beside it: the check includes the helper by
        # `CMAKE_CURRENT_LIST_DIR`, so a copy anywhere else would fail to include and
        # this case would refuse for a reason that is not its own.
        file(COPY "${FASTCACHED_SOURCE_DIR}/scripts/lib/CheckCommon.cmake"
             DESTINATION "${tree}/scripts/lib")
        file(WRITE "${tree}/scripts/staged-check.cmake"
             "${beforeTable}set(FastCachedControlByteExtensions ${extensions})\n"
             "message(STATUS \"control-bytes: STAGED-CHECK\")\n"
             "${afterTable}")
        set(runCheck "${tree}/scripts/staged-check.cmake")
    endif()

    # `ENCODING NONE` because this fixture's whole subject is BYTES: without it the
    # child's output is decoded through the console code page, and a check searching
    # for a control byte would be searching a haystack its own harness had rewritten.
    # The status is deliberately not captured -- see the header, the verdict is the
    # output.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${runCheck}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err ENCODING NONE)
    set(combined "${out}${err}")
    string(REGEX REPLACE "[\r\n]+" " " combined "${combined}")
    string(REGEX REPLACE " +" " " combined "${combined}")
    set(${outFlat} "${combined}" PARENT_SCOPE)
    if(combined MATCHES "STAGED-CHECK")
        set(${outStaged} TRUE PARENT_SCOPE)
    else()
        set(${outStaged} FALSE PARENT_SCOPE)
    endif()
endfunction()

# @param name The case name.
# @param flat The flattened output.
# @param expect `accept` or `refuse`.
# @param phrase A phrase the output must contain.
# @param scanned The file count the check must report, or `-`.
function(ExpectVerdict name flat expect phrase scanned)
    math(EXPR ran "${ran} + 1")
    set(ran "${ran}" PARENT_SCOPE)

    if(flat MATCHES "CMake Error|CMake Warning")
        set(verdict "refuse")
    else()
        set(verdict "accept")
    endif()

    set(problem "")
    if(NOT verdict STREQUAL expect)
        set(problem "expected ${expect}, got ${verdict}")
    elseif(NOT flat MATCHES "${phrase}")
        set(problem "${verdict} was right but the words were not: expected `${phrase}`")
    elseif(NOT scanned STREQUAL "-")
        # The half a verdict cannot carry. A reader that stopped reading files produces
        # a perfectly clean accept, which is this check's own thesis applied to itself.
        if(NOT flat MATCHES "control-bytes: ([0-9]+) tracked text file")
            set(problem "the run reported no scanned count")
        elseif(NOT CMAKE_MATCH_1 STREQUAL "${scanned}")
            set(problem "scanned ${CMAKE_MATCH_1} file(s), expected ${scanned}")
        endif()
    endif()

    if(problem STREQUAL "")
        message(STATUS "ok   ${name}")
    else()
        message(STATUS "FAIL ${name}: ${problem}")
        message(STATUS "     output: ${flat}")
        list(APPEND failures "${name}")
        set(failures "${failures}" PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Case 1 -- the ACCEPTING direction, with its count. Two staged files plus the roots
# file every tree carries (see the header), so three.
StageAndRun("clean" "src/Alpha.cpp=int alpha()\n{\n${TAB}return 1\n}|scripts/fine.sh=#!/usr/bin/env bash\necho hello" "-" flat staged)
ExpectVerdict("case 1: a tree with no control byte is accepted" "${flat}"
    "accept" "none carries a C0 byte" "3")

# Case 2 -- TAB is not a violation. Case 1 already staged one, so this asserts the thing
# case 1 would be silent about: that the ALLOWED codes are allowed for a reason rather
# than because nothing is being read.
StageAndRun("tabs" "src/Alpha.cpp=int${TAB}alpha()${TAB}{${TAB}return${TAB}1${TAB}}" "-" flat staged)
ExpectVerdict("case 2: TAB is allowed" "${flat}" "accept" "none carries a C0 byte" "2")

# Case 3 -- a BEL. #593's byte, and the class this check exists for.
StageAndRun("bel" "src/Alpha.cpp=int alpha()|scripts/rings.sh=#!/usr/bin/env bash\nbell=\"${BEL}\"" "-" flat staged)
ExpectVerdict("case 3: a BEL is refused and the file NAMED" "${flat}"
    "refuse" "scripts/rings\\.sh" "-")

# Case 4 -- and it says WHERE, in a line and a byte offset. A refusal naming only the
# file sends a reader to a grep for a character that does not print.
ExpectVerdict("case 4: the refusal gives a line and a byte offset" "${flat}"
    "refuse" "line 2, byte offset 26: control byte 0x07" "-")

# Case 5 -- ESC, so the class is not one byte with a guard around it.
StageAndRun("esc" "src/Alpha.cpp=int alpha()|docs/note.md=a ${ESC}[1m bold escape" "-" flat staged)
ExpectVerdict("case 5: an ESC is refused" "${flat}" "refuse" "docs/note\\.md" "-")

# Case 6 -- CR gets its OWN sentence. It is the likeliest hit here and the remedy is a
# different one: line endings rather than an eaten escape. A refusal that said "an
# escape got eaten" would send every reader of it to the wrong fix.
#
# It is also the case that cannot be reached through the text read on this host, so it
# is the one proving the hex arm exists at all.
StageAndRun("cr" "src/Alpha.cpp=int alpha()|scripts/dos.sh=#!/usr/bin/env bash${CR}\necho hi${CR}" "-" flat staged)
ExpectVerdict("case 6: a CR is refused and named as a line-ending problem" "${flat}"
    "refuse" "CR \\(0x0d\\) -- this tree is LF-only" "-")

# Case 7 -- a file type the allow-list does not name is not scanned, and the tree is
# still accepted. This is what makes the list an ALLOW-list rather than a claim that
# every file in the tree is text.
StageAndRun("unlisted" "src/Alpha.cpp=int alpha()|docs/image.png=fake ${BEL} bytes" "-" flat staged)
ExpectVerdict("case 7: a BEL in an unlisted file type is not scanned" "${flat}"
    "accept" "none carries a C0 byte" "2")

# Case 8 -- third-party source is DECLINED, not held to this rule. The planted file
# would refuse the tree if it were read, so acceptance is the proof and the declined
# count is what says why.
StageAndRun("vendored" "src/Alpha.cpp=int alpha()|vendor/upstream/Theirs.cpp=int theirs() ${BEL}" "-" flat staged)
ExpectVerdict("case 8: a control byte in vendored source is declined" "${flat}"
    "accept" "1 third-party file\\(s\\) declined" "2")

# Case 9 -- an empty scan is a REFUSAL, not a clean run. Two empty lists agree
# perfectly, and this check accumulates findings, so a file set that matched nothing
# produces byte-identical output to a tree with no violation.
#
# Reached through a STAGED allow-list rather than an empty tree, because an empty tree
# cannot reach it: the roots file the helper requires is itself a scanned `.txt`, so a
# valid tree always has at least one file. Narrowing the list in a copy is the same
# move `check-selftest-registered-selftest` makes for its exemption table, and the run
# is asserted to have used the copy -- a case that silently ran the shipped list would
# scan the tree and pass while testing nothing.
StageAndRun("nothing" "src/Alpha.cpp=int alpha()" "zzz-no-such-extension" flat staged)
ExpectVerdict("case 9: a scan that reads no file is refused, not reported clean"
    "${flat}" "refuse" "reporting on nothing" "-")
if(NOT staged)
    message(STATUS "FAIL case 9: the staged allow-list was not used, so the shipped one was read")
    list(APPEND failures "case 9 staging")
endif()

# Case 10 -- a clean file beside a dirty one. The findings ACCUMULATE, so a check that
# stopped at the first clean file would pass every case above and this one is what says
# so.
StageAndRun("mixed" "src/Alpha.cpp=int alpha() ${BEL}|scripts/fine.sh=echo hi" "-" flat staged)
ExpectVerdict("case 10: a clean file beside a dirty one does not hide it" "${flat}"
    "refuse" "src/Alpha\\.cpp" "-")

# ---------------------------------------------------------------------------
# The shared premise, asserted rather than assumed: `string(ASCII 7 ...)` really does
# produce one byte. The check and this fixture both depend on it, so if it stopped
# working the check would stop refusing and every planted case would stop planting --
# together, and every case above would go green.
math(EXPR ran "${ran} + 1")
string(LENGTH "${BEL}" bellLength)
if(bellLength EQUAL 1)
    message(STATUS "ok   case 11: string(ASCII 7) produces one byte, so the planted cases plant something")
else()
    message(STATUS "FAIL case 11: string(ASCII 7) produced ${bellLength} byte(s), not 1")
    list(APPEND failures "case 11")
endif()

# ---------------------------------------------------------------------------
if(ran EQUAL 0)
    message(FATAL_ERROR "check-control-bytes self-test ran no cases, so it asserted nothing.")
endif()
if(failures)
    list(LENGTH failures failureCount)
    list(JOIN failures ", " failureList)
    message(FATAL_ERROR
        "check-control-bytes-selftest: ${ran} case(s) ran, ${failureCount} failed: ${failureList}")
endif()
message(STATUS "check-control-bytes-selftest: ${ran} case(s) ran, all passed")
