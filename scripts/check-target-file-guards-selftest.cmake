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
# #1369 gave the check a SECOND source of optional targets -- the directories the root adds
# under a `FASTCACHED_BUILD_*` option outside `src/`, which is `vendor/` -- with its own
# completeness argument. So every synthetic tree now carries a root `CMakeLists.txt` and one
# such directory, `thirdparty/`, and the cases from 6 on drive that source: a planted
# violation in EACH source is refused and names which source made the target optional, the
# app table's equality still refuses a lost row with gated targets present, and each of the
# second reader's own assertions is watched refuse.
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

# The root and the gated directory every tree has unless a case replaces them: the root adds
# `thirdparty/` only under `FASTCACHED_BUILD_THIRDPARTY`, and `thirdparty/` declares one
# target. The same shape as `vendor/` under `FASTCACHED_BUILD_TUI`.
set(defaultRootBody
"option(FASTCACHED_BUILD_THIRDPARTY \"Build the third-party tree\" ON)
add_library(Library STATIC)
if(FASTCACHED_BUILD_THIRDPARTY)
    add_subdirectory(thirdparty)
endif()
add_subdirectory(src/apps)
")
set(defaultGatedBody
"add_executable(thirdparty-probe probe.cpp)
")

# Build a tree with a root, a gated `thirdparty/`, an app table and one registration file
# under src/tests/.
#
# The case supplies the WHOLE table, because where a stray bracket sits decides
# whether it reproduces anything: `file(STRINGS ... REGEX)` discards lines that do
# not match BEFORE building the list, so a bracket on a discarded line is invisible.
# A case replaces the root or the gated file by setting `treeRootBody` or
# `treeGatedBody` before calling, and unsetting it after.
function(fastcached_make_tree name tableBody registrations outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/apps")
    file(MAKE_DIRECTORY "${tree}/src/tests")
    file(MAKE_DIRECTORY "${tree}/thirdparty")
    set(rootBody "${defaultRootBody}")
    if(DEFINED treeRootBody)
        set(rootBody "${treeRootBody}")
    endif()
    set(gatedBody "${defaultGatedBody}")
    if(DEFINED treeGatedBody)
        set(gatedBody "${treeGatedBody}")
    endif()
    file(WRITE "${tree}/CMakeLists.txt" "${rootBody}")
    file(WRITE "${tree}/thirdparty/CMakeLists.txt" "${gatedBody}")
    file(WRITE "${tree}/src/apps/CMakeLists.txt" "${tableBody}")
    file(WRITE "${tree}/src/tests/CMakeLists.txt" "${registrations}")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Every case counts itself here, so the closing line states how many RAN rather than how many
# somebody once wrote down.
set(caseCount 0)

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
    # `CMake Error|CMake Warning`, never `CMake Error` alone: a sub-run that merely WARNS
    # changes meaning silently and, read for the error word alone, is scored a clean pass
    # (#672). Stated in full -- and enforced -- in `scripts/check-script-check-signals.cmake`.
    set(sawSignal FALSE)
    if(combined MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    set(${outObjected} ${sawSignal} PARENT_SCOPE)
    # FLATTENED before any case reads it: CMake wraps a diagnostic's text at about 74
    # columns, so a phrase a refusal really printed can exist in the output and in no single
    # line of it, and a FIND for it reports a refusal that did not happen as a wrong one.
    string(REGEX REPLACE "[ \t\r\n]+" " " flattened "${combined}")
    set(${outOutput} "${flattened}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# 1. A guarded reference passes. Without this the check could refuse everything,
#    which is exactly as useless as refusing nothing and looks like rigour.
math(EXPR caseCount "${caseCount} + 1")
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
math(EXPR caseCount "${caseCount} + 1")
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
    string(FIND "${output}" "optional because it is a row of the app table" position)
    if(position EQUAL -1)
        list(APPEND failures "unguarded: the refusal did not say the target is optional because of the app table, so its source cannot be told from the second one")
    endif()
endif()

# 3. No reference at all. The check must refuse as VACUOUS rather than report success
#    over a scan that examined nothing. #510 turns on this refusal existing, and
#    fixing the readers must make it rare rather than remove it.
math(EXPR caseCount "${caseCount} + 1")
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
math(EXPR caseCount "${caseCount} + 1")
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
    list(APPEND failures "bracket: a stray closing bracket in the app table made the check refuse a correct tree")
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
math(EXPR caseCount "${caseCount} + 1")
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
# The second source (#1369).

# 6. A gated target referenced WITH its guard passes, and the pass says the second source was
#    read: one target from one gated file. Without the count, a reader that found nothing
#    passes this case too.
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("gated-guarded" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
if(TARGET thirdparty-probe)
    add_test(NAME t COMMAND $<TARGET_FILE:thirdparty-probe>)
endif()
" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "gated-guarded: a guarded reference to a gated target was refused")
else()
    string(FIND "${output}" "and 1 from 1 gated file(s) outside src/ (thirdparty/CMakeLists.txt)" position)
    if(position EQUAL -1)
        list(APPEND failures "gated-guarded: the pass did not report the one target of thirdparty/CMakeLists.txt, so the second source may have read nothing")
    endif()
endif()

# 7. THE #1369 CASE. An unguarded reference to a gated target is refused and names BOTH its
#    file:line and the source that made the target optional. Before #1369 this passed: the
#    check never knew the target existed.
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("gated-unguarded" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
add_test(NAME t COMMAND $<TARGET_FILE:thirdparty-probe>)
" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "gated-unguarded: an unguarded reference to a target only a gated directory declares was accepted -- #1369 unfixed")
else()
    string(FIND "${output}" "src/tests/CMakeLists.txt:4" position)
    if(position EQUAL -1)
        list(APPEND failures "gated-unguarded: the refusal did not name file:line")
    endif()
    string(FIND "${output}" "declared in thirdparty/CMakeLists.txt, which CMakeLists.txt adds only under FASTCACHED_BUILD_THIRDPARTY" position)
    if(position EQUAL -1)
        list(APPEND failures "gated-unguarded: the refusal did not name the gated file and the option as the target's source")
    endif()
endif()

# 8. The gated directory's own reference to its own target is not a violation: that file is
#    read only when its option is on. The control on 7 -- a check that refused every gated
#    reference would pass 7 for the wrong reason.
set(treeGatedBody
"add_executable(thirdparty-probe probe.cpp)
add_custom_target(run-probe COMMAND $<TARGET_FILE:thirdparty-probe>)
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("gated-self" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
" tree)
unset(treeGatedBody)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "gated-self: a gated file naming its own target was refused, though that target exists wherever the file is read")
endif()

# 9. The app table's equality holds ON ITS OWN TERMS with gated targets present. A row the
#    reader cannot turn into a target is refused as lost rows -- a merged pair of counts would
#    let the gated target stand in for it. Before `thirdparty-probe` could mask anything, the
#    refusal must come from the app table's own assertion.
set(droppedRowTable
"# The app table.
set(FastCachedApps
    \"alpha|FASTCACHED_BUILD_ALPHA|ON|The alpha app\"
    \"beta app|FASTCACHED_BUILD_BETA|OFF|A row whose target field the reader cannot read\"
)
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("app-row-dropped" "${droppedRowTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
if(TARGET thirdparty-probe)
    add_test(NAME t COMMAND $<TARGET_FILE:thirdparty-probe>)
endif()
" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "app-row-dropped: an app table row the reader dropped was not refused while a gated target was present -- the two sources' counts are standing in for each other")
else()
    string(FIND "${output}" "declares 2 build option(s) but only 1 optional target(s) were read" position)
    if(position EQUAL -1)
        list(APPEND failures "app-row-dropped: it refused, but not with the app table's own count of 2 options against 1 target")
    endif()
endif()

# 10. The root reader's completeness: every `option(FASTCACHED_BUILD_...)` counted in the root's
#     raw bytes must be seen naming an `if()`. One the reader never saw is refused, naming the
#     option: either the reader lost the lines that gate a directory, or the option gates nothing
#     this check can see. (A condition spelled across lines used to be how this was reached; the
#     walker follows those now, which case 18 pins.)
set(treeRootBody
"option(FASTCACHED_BUILD_THIRDPARTY \"Build the third-party tree\" ON)
option(FASTCACHED_BUILD_UNSEEN \"An option no if() names\" ON)
set(SOMETHING \${FASTCACHED_BUILD_UNSEEN})
if(FASTCACHED_BUILD_THIRDPARTY)
    add_subdirectory(thirdparty)
endif()
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("root-option-unseen" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
" tree)
unset(treeRootBody)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "root-option-unseen: a root option no if() was read naming was accepted, so whatever it gates dropped out of the check silently")
else()
    string(FIND "${output}" "declares option(FASTCACHED_BUILD_UNSEEN) but no if() naming it was read" position)
    if(position EQUAL -1)
        list(APPEND failures "root-option-unseen: it refused, but did not name the option the reader never saw")
    endif()
endif()

# 11. The gated file's completeness: a declaration whose target name is on a later line is
#     counted by the raw count and missed by the line reader, and is refused naming the file.
set(treeGatedBody
"add_executable(
    thirdparty-probe probe.cpp)
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("gated-declaration-unread" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
" tree)
unset(treeGatedBody)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "gated-declaration-unread: a gated declaration the reader could not name was accepted")
else()
    string(FIND "${output}" "thirdparty/CMakeLists.txt holds 1 add_library/add_executable call(s) but only 0 were read" position)
    if(position EQUAL -1)
        list(APPEND failures "gated-declaration-unread: it refused, but not as thirdparty/CMakeLists.txt's reader losing a declaration")
    endif()
endif()

# 12. The floor: a root that gates no directory outside src/ gives the second source nothing,
#     and is refused as vacuous rather than reported clean.
set(treeRootBody
"option(FASTCACHED_BUILD_THIRDPARTY \"Build the third-party tree\" ON)
if(FASTCACHED_BUILD_THIRDPARTY)
    add_subdirectory(src/elsewhere)
endif()
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("no-gated-directory" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
" tree)
unset(treeRootBody)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "no-gated-directory: a second source that read nothing passed")
else()
    string(FIND "${output}" "the second source of optional targets read nothing" position)
    if(position EQUAL -1)
        list(APPEND failures "no-gated-directory: it refused, but not as the second source reading nothing")
    endif()
endif()

# 13. A stray `]` in a gated file's COMMENT, above its declaration, changes nothing -- the
#     bracket that blinded the app table's reader (#510), aimed at the new reader.
set(treeGatedBody
"# a stray ] bracket in a comment
add_executable(thirdparty-probe probe.cpp)
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("gated-bracket" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
add_test(NAME t COMMAND $<TARGET_FILE:thirdparty-probe>)
" tree)
unset(treeGatedBody)
fastcached_run_check("${tree}" objected output)
string(FIND "${output}" "$<TARGET_FILE:thirdparty-probe> is reached without" position)
if(NOT objected OR position EQUAL -1)
    list(APPEND failures "gated-bracket: a stray closing bracket in thirdparty/CMakeLists.txt blinded the second reader -- the unguarded reference to its target was not refused by name")
endif()

# ---------------------------------------------------------------------------
# The review of #1369's first version found four ways the second reader went blind or lied.
# One case each; every one failed against that version.

# 14. A target the ROOT declares inside `if(FASTCACHED_BUILD_...)` is not exempt in the root:
#     unlike a gated directory's file, the root runs whether the option is on or not, so an
#     unguarded reference later in the root is a generate-time error with the option OFF.
set(treeRootBody
"option(FASTCACHED_BUILD_THIRDPARTY \"Build the third-party tree\" ON)
if(FASTCACHED_BUILD_THIRDPARTY)
    add_subdirectory(thirdparty)
    add_executable(rootprobe probe.cpp)
endif()
add_test(NAME r COMMAND $<TARGET_FILE:rootprobe>)
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("root-gated-reference" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
" tree)
unset(treeRootBody)
fastcached_run_check("${tree}" objected output)
string(FIND "${output}" "CMakeLists.txt:6: $<TARGET_FILE:rootprobe> is reached without" position)
if(NOT objected OR position EQUAL -1)
    list(APPEND failures "root-gated-reference: an unguarded reference in the root to a target the root declares only under an option was not refused at CMakeLists.txt:6 -- the root runs with the option OFF too")
endif()

# 15. A gated directory spelled with quotes, or through `${CMAKE_CURRENT_SOURCE_DIR}`, is still
#     a gated directory. A second one spelled so lost ALL its targets while the first satisfied
#     the floor, so the unguarded reference to its target must still be refused by name.
set(treeRootBody
"option(FASTCACHED_BUILD_THIRDPARTY \"Build the third-party tree\" ON)
option(FASTCACHED_BUILD_OTHER \"Build the other tree\" ON)
if(FASTCACHED_BUILD_THIRDPARTY)
    add_subdirectory(\"thirdparty\")
endif()
if(FASTCACHED_BUILD_OTHER)
    add_subdirectory(\${CMAKE_CURRENT_SOURCE_DIR}/other)
endif()
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("subdirectory-spellings" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
if(TARGET thirdparty-probe)
    add_test(NAME t COMMAND $<TARGET_FILE:thirdparty-probe>)
endif()
add_test(NAME o COMMAND $<TARGET_FILE:other-probe>)
" tree)
unset(treeRootBody)
file(MAKE_DIRECTORY "${tree}/other")
file(WRITE "${tree}/other/CMakeLists.txt" "add_executable(other-probe probe.cpp)\n")
fastcached_run_check("${tree}" objected output)
string(FIND "${output}" "$<TARGET_FILE:other-probe> is reached without" position)
if(NOT objected OR position EQUAL -1)
    list(APPEND failures "subdirectory-spellings: a gated directory added through \${CMAKE_CURRENT_SOURCE_DIR} lost its targets, and the unguarded reference to one was not refused by name")
endif()

# 16. A gated `add_subdirectory` whose directory cannot be resolved at all is refused, naming the
#     spelling, rather than silently not being a gated directory.
set(treeRootBody
"option(FASTCACHED_BUILD_THIRDPARTY \"Build the third-party tree\" ON)
if(FASTCACHED_BUILD_THIRDPARTY)
    add_subdirectory(thirdparty)
    add_subdirectory(\${SOMEWHERE_ELSE})
endif()
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("subdirectory-unresolved" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
" tree)
unset(treeRootBody)
fastcached_run_check("${tree}" objected output)
string(FIND "${output}" "add_subdirectory(\${SOMEWHERE_ELSE})" position)
if(NOT objected OR position EQUAL -1)
    list(APPEND failures "subdirectory-unresolved: a gated add_subdirectory naming a directory this check cannot resolve was not refused by its spelling")
endif()

# 17. An `if()` carrying a trailing comment is still a frame: the gated directory behind it keeps
#     its targets, and the unguarded reference to one is refused by name -- not the floor firing
#     for the wrong reason because the directory silently stopped being gated.
set(treeRootBody
"option(FASTCACHED_BUILD_THIRDPARTY \"Build the third-party tree\" ON)
if(FASTCACHED_BUILD_THIRDPARTY)  # the third-party tree
    add_subdirectory(thirdparty)
endif()
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("if-with-comment" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
add_test(NAME t COMMAND $<TARGET_FILE:thirdparty-probe>)
" tree)
unset(treeRootBody)
fastcached_run_check("${tree}" objected output)
string(FIND "${output}" "$<TARGET_FILE:thirdparty-probe> is reached without" position)
if(NOT objected OR position EQUAL -1)
    list(APPEND failures "if-with-comment: an if() with a trailing comment was not read as a frame, so its gated directory lost its targets")
endif()

# 18. A condition spelled across lines is still a frame, in the reference scan too: a guarded
#     reference inside it passes, and an `endif()` does not pop the frame ENCLOSING it.
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("multi-line-guard" "${plainTable}"
"if(TARGET alpha AND
   NOT SOMETHING_ELSE)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
if(TARGET beta)
    if(SOMETHING
       OR SOMETHING_MORE)
        set(x 1)
    endif()
    add_test(NAME b COMMAND $<TARGET_FILE:beta>)
endif()
" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "multi-line-guard: a condition spelled across lines was not followed, so a guarded reference was refused or an enclosing guard was popped early")
endif()

# 19. CMake commands are case-insensitive, and so is this reader: `IF`, `ADD_SUBDIRECTORY` and
#     `ADD_EXECUTABLE` are read, and the unguarded reference to the target is refused by name.
#     Both counts folding case is what keeps 0 == 0 from passing.
set(treeRootBody
"OPTION(FASTCACHED_BUILD_THIRDPARTY \"Build the third-party tree\" ON)
IF(FASTCACHED_BUILD_THIRDPARTY)
    ADD_SUBDIRECTORY(thirdparty)
ENDIF()
")
set(treeGatedBody
"ADD_EXECUTABLE(thirdparty-probe probe.cpp)
")
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("upper-case-commands" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
add_test(NAME t COMMAND $<TARGET_FILE:thirdparty-probe>)
" tree)
unset(treeRootBody)
unset(treeGatedBody)
fastcached_run_check("${tree}" objected output)
string(FIND "${output}" "$<TARGET_FILE:thirdparty-probe> is reached without" position)
if(NOT objected OR position EQUAL -1)
    list(APPEND failures "upper-case-commands: upper-case IF/ADD_SUBDIRECTORY/ADD_EXECUTABLE went unread, so the gated target's unguarded reference was not refused by name")
endif()

# 20. A walk that loses track SAYS so, at the line it happened. An `endif()` with no open `if()`
#     used to pop nothing and carry on, every later verdict in the file drawn from the wrong
#     frames. The reference is guarded, so the refusal can only be the lost track.
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("stray-endif" "${plainTable}"
"if(TARGET alpha)
    add_test(NAME a COMMAND $<TARGET_FILE:alpha>)
endif()
endif()
" tree)
fastcached_run_check("${tree}" objected output)
string(FIND "${output}" "src/tests/CMakeLists.txt:4: an if()/elseif()/endif() this checker could not match to its chain" position)
if(NOT objected OR position EQUAL -1)
    list(APPEND failures "stray-endif: an endif() with no open if() was not reported at src/tests/CMakeLists.txt:4")
endif()

# ---------------------------------------------------------------------------
# No failure text above may contain a bracket. `failures` is a CMake list, and one unbalanced
# `]` merges its element with the next, so `list(LENGTH)` undercounts: with two such texts
# failing, this line once read "7 case(s) wrong" above fourteen listed failures.
if(failures)
    list(LENGTH failures failureCount)
    string(REPLACE ";" "\n  " rendered "${failures}")
    message(FATAL_ERROR
        "target-file-guards selftest: ${failureCount} case(s) wrong\n  ${rendered}")
endif()

message(STATUS
    "target-file guards selftest: ${caseCount} synthetic tree(s), every verdict as expected")
