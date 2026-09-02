# SPDX-License-Identifier: Apache-2.0
#
# `target-file-guards` must be SEEN to refuse, on each thing it claims and on nothing
# else.
#
# This check had never been watched refuse anything. #510 exists because a DELIBERATE
# canary -- a stray `]` somebody left in a comment -- caught it going blind, not
# because the check reported a problem. By this tree's own standard a guard nobody has
# watched refuse is not a guard, and this one had been carrying a silent defect for
# long enough to be fixed twice: #509 repaired one of its two readers, and #510 the
# other.
#
# The case that matters most here is `bracket`. Before #510, appending one `]` to an
# app table row took this check from
#
#     25 reference(s) to 5 optional target(s), all guarded
#
# to
#
#     10 reference(s) to 1 optional target(s), all guarded    -- and it PASSED
#
# Four targets and fifteen references silently unchecked. The old vacuity refusal could
# not catch it because it is a FLOOR OF ZERO and 1 is not 0, which is why `incomplete`
# is here too: it drives the completeness assertion that replaced the floor.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-target-file-guards-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-target-file-guards.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
set(failures "")

# Build a tree with an app table and one registration file under src/tests/.
#
# The case supplies the WHOLE table, because where a stray bracket sits decides
# whether it reproduces anything: `file(STRINGS ... REGEX)` discards lines that do
# not match BEFORE building the list, so a bracket on a discarded line is invisible.
function(fastcached_make_tree name tableBody registrations outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/apps")
    file(MAKE_DIRECTORY "${tree}/src/tests")
    file(WRITE "${tree}/src/apps/CMakeLists.txt" "${tableBody}")
    file(WRITE "${tree}/src/tests/CMakeLists.txt" "${registrations}")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# The ordinary two-row table every case starts from.
set(plainTable
"# The app table.
set(FastCachedApps
    \"alpha|FASTCACHED_BUILD_ALPHA|ON|The alpha app\"
    \"beta|FASTCACHED_BUILD_BETA|OFF|The beta app\"
)
")

function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")
    string(FIND "${combined}" "CMake Error" position)
    if(position EQUAL -1)
        set(${outObjected} FALSE PARENT_SCOPE)
    else()
        set(${outObjected} TRUE PARENT_SCOPE)
    endif()
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# 1. A guarded reference passes. Without this the check could refuse everything,
#    which is exactly as useless as refusing nothing and looks like rigour.
fastcached_make_tree("guarded" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "guarded: a properly guarded reference was refused -- the check refuses everything")
endif()

# 2. An unguarded reference is refused, and NAMES the file and line. A guard whose
#    file:line is missing or wrong sends the reader to the wrong place.
fastcached_make_tree("unguarded" "${plainTable}"
"add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "unguarded: a reference with no enclosing `TARGET alpha` was accepted -- that is a generate-time hard error, not a skipped test")
else()
    string(FIND "${output}" "src/tests/CMakeLists.txt:1" position)
    if(position EQUAL -1)
        list(APPEND failures "unguarded: the refusal did not name file:line, so it cannot be acted on")
    endif()
endif()

# 3. No reference at all. The check must refuse as VACUOUS rather than report success
#    over a scan that examined nothing. #510 turns on this refusal existing, and
#    fixing the readers must make it rare rather than remove it.
fastcached_make_tree("vacuous" "${plainTable}" "# no registrations here at all\n" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "vacuous: a tree with no reference to any optional target passed -- two empty lists agree perfectly, which is the defect this check exists to refuse")
else()
    string(FIND "${output}" "pass vacuously" position)
    if(position EQUAL -1)
        list(APPEND failures "vacuous: it refused, but not as a vacuous pass, so the reason will be misread")
    endif()
endif()

# 4. THE REGRESSION CASE. An unbalanced `]` on an app table ROW must not change the
#    verdict. Before #510 this took the real check from 5 optional targets to 1 while
#    still passing.
#
#    It sits on the FIRST row deliberately. A REGEX-filtered `file(STRINGS)` discards
#    non-matching lines BEFORE building the list, so a bracket in a COMMENT reaches
#    nothing -- the first version of this case put it there, and PASSED against the
#    unfixed check. A regression test that does not reproduce the regression is worse
#    than none, because it reads as coverage.
set(bracketTable
"# The app table.
set(FastCachedApps
    \"alpha|FASTCACHED_BUILD_ALPHA|ON|The alpha app ] with a stray bracket\"
    \"beta|FASTCACHED_BUILD_BETA|OFF|The beta app\"
)
")
fastcached_make_tree("bracket" "${bracketTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
if(TARGET beta)
    add_test(NAME b COMMAND $<TARGET_FILE:beta>)
endif()
" tree)
fastcached_run_check("${tree}" objected bracketOutput)
if(objected)
    list(APPEND failures "bracket: a stray `]` in the app table made the check refuse a correct tree")
else()
    string(FIND "${bracketOutput}" "2 optional target(s)" position)
    if(position EQUAL -1)
        list(APPEND failures "bracket: the check no longer saw BOTH optional targets -- one unbalanced bracket still truncates the app table, which is #510 unfixed")
    endif()
endif()

# 5. COMPLETENESS. A row the reader cannot see must be refused, not silently dropped.
#    The old guard was a floor of zero and could not catch this: some targets were
#    still found, and some is not all.
set(incompleteTable
"# The app table.
set(FastCachedApps
    \"alpha|FASTCACHED_BUILD_ALPHA|ON|The alpha app\"
    \"beta|FASTCACHED_BUILD_BETA|OFF|The beta app\"
)
# A third option the row filter cannot see: FASTCACHED_BUILD_GAMMA
")
fastcached_make_tree("incomplete" "${incompleteTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "incomplete: the app table declared a build option the reader never turned into a target, and the check passed anyway -- that is the 5-to-1 truncation going unreported")
else()
    string(FIND "${output}" "lost rows" position)
    if(position EQUAL -1)
        list(APPEND failures "incomplete: it refused, but not as a reader that lost rows, so the cause will be hunted in the wrong place")
    endif()
endif()

# ---------------------------------------------------------------------------
if(failures)
    list(LENGTH failures failureCount)
    string(REPLACE ";" "\n  " rendered "${failures}")
    message(FATAL_ERROR
        "target-file-guards selftest: ${failureCount} case(s) wrong\n  ${rendered}")
endif()

message(STATUS
    "target-file guards selftest: 5 synthetic tree(s), every verdict as expected")
