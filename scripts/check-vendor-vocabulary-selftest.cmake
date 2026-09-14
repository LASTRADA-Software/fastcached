# SPDX-License-Identifier: Apache-2.0
#
# `vendor-vocabulary` must be SEEN to refuse each thing it claims to refuse, and seen to
# ACCEPT what it claims to leave alone (#1377).
#
# The shipped tree reaches none of the vocabulary today, so the real run can only ever show
# the accepting direction. Everything about refusing is shown here, over synthesised trees.
#
# ## How the tables get into a synthetic tree
#
# The check's exemption and control tables name files in the REAL tree, and a synthetic tree
# has none of them. So each case copies the check and `lib/CheckCommon.cmake` into the tree it
# runs in and inserts its own tables after the check's `@tables-end` marker, which re-`set`s both
# tables and so overrides whatever rows the real ones carry. The copy is otherwise
# byte-identical, so the thing tested is the thing that ships. The marker is asserted to occur
# exactly ONCE: missing and duplicated both make the injection mean something else, and a
# case run against an uninjected copy would refuse on the real control rows and read as a
# rule regression.
#
# ## Which case pins which half
#
#   clean, neighbours, shadow, bracketAlone   the ACCEPTING direction, each on a shape that
#                                             a plausible broken matcher would refuse
#   angled, quoted, backslash, dotdot,        the forbidden SPELLING, one per way of writing it
#   conditional, testFile
#   unvendored                                a forbidden spelling naming NO vendored file, the
#                                             one shape only the spelling arm can refuse
#   direct, deep                              the CLOSURE: one hop, and two hops through a
#                                             quoted include resolved beside a VENDORED file
#   besideIntoVendor                          a quoted path walking from src/ into vendor/endo,
#                                             which the cheap first-segment filter must not skip
#   memoCollision                             two headers whose names flatten alike keep their
#                                             own memoised answers
#   bracket                                   a stray `]` before a violation does not hide it
#   exempt, staleReach, staleMissing,         the exemption table, and each way a row describes
#   staleNotSource                            nothing
#   controlDead, controlMissing, controlEmpty a control that finds nothing is itself refused
#   vacuous, noVendor                         a scan of nothing refuses rather than passes
#   remedyText                                the refusal says what the rule does not cover
#
# `deep` is the only case that can see the resolver follow a quoted include inside vendor/,
# and no real vendored header reaches the vocabulary in two hops today (measured on the
# fork point: five reach it directly, none only transitively). So without it the transitive
# half would be proven by nothing.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-vendor-vocabulary-selftest.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-vendor-vocabulary.cmake")
set(checkCommon "${FASTCACHED_SOURCE_DIR}/scripts/lib/CheckCommon.cmake")
foreach(required IN ITEMS "${check}" "${checkCommon}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "the check under test is missing: ${required}")
    endif()
endforeach()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
set(failures "")
set(caseCount 0)

set(defaultControls "\"tui/runtime/TuiRuntime.hpp|synthetic control: includes coro/Task.hpp directly\"")

# A vendored tree shaped like the real one where it matters: the vocabulary itself, a clean
# platform header and a clean tui header, a runtime header reaching coro/ in ONE hop, and a
# tui header reaching platform/Clock.hpp in TWO, the second through a quoted include that
# only resolves beside the vendored file including it.
#
# @param name Case name, which is also the tree's directory.
# @param probeBody Content of `src/apps/probe/Probe.cpp`.
# @param exemptions The exemption rows to inject, as quoted CMake arguments, or empty.
# @param controls The control rows to inject, as quoted CMake arguments.
# @param outVar Set to the tree's path.
function(fastcached_make_tree name probeBody exemptions controls outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    fastcached_stage_vendor("${tree}")
    file(WRITE "${tree}/src/apps/probe/Probe.cpp" "${probeBody}")
    fastcached_write_check("${tree}" "${exemptions}" "${controls}")
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# The one tree every `fastcached_case` shares, rewritten only where a case differs.
#
# **The cost of this selftest is creating files, and on the local gate that is a DrvFs cost.**
# MEASURED on 09714dc9, WSL2 over a Windows volume, host load 3-4 from another lane's build, three
# interleaved rounds: 10.4-17.8 s with the scratch tree on DrvFs, 0.83-0.88 s with the same sources
# and the scratch tree on ext4, 0.17-0.18 s with both native -- and a gate's `ctest --parallel 32`
# killed it at 60 s. So the cases that only swap the probe source and the tables no longer each
# stage, delete and re-stage a whole tree: they share one, write the probe, and rewrite the check
# only when the injected tables change. The cases that ADD or REMOVE a file keep a tree of their own
# (`fastcached_make_tree`), so nothing one of them leaves behind reaches another.
#
# MEASURED after, same host, scratch on DrvFs, the per-case version and this one interleaved, all 26
# cases passing in every run: at load 2.8, 10.4-19.3 s against 6.2-8.7 s over four rounds; at load
# 13-34, 21.7-34.2 s against 9.2-17.5 s over three. The residue is the check's own run per case,
# which reads its tree over the same filesystem. A reference, not a budget: the ctest TIMEOUT stays
# where it was.
function(fastcached_shared_tree probeBody exemptions controls outVar)
    set(tree "${root}/shared")
    get_property(staged GLOBAL PROPERTY FASTCACHED_VOCABULARY_SHARED_STAGED SET)
    if(NOT staged)
        file(REMOVE_RECURSE "${tree}")
        fastcached_stage_vendor("${tree}")
        set_property(GLOBAL PROPERTY FASTCACHED_VOCABULARY_SHARED_STAGED TRUE)
    endif()
    file(WRITE "${tree}/src/apps/probe/Probe.cpp" "${probeBody}")
    get_property(tables GLOBAL PROPERTY FASTCACHED_VOCABULARY_SHARED_TABLES)
    if(NOT "${tables}" STREQUAL "${exemptions}|${controls}")
        fastcached_write_check("${tree}" "${exemptions}" "${controls}")
        set_property(GLOBAL PROPERTY FASTCACHED_VOCABULARY_SHARED_TABLES "${exemptions}|${controls}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# The vendored tree the cases are read against.
# @param tree Where to stage it.
function(fastcached_stage_vendor tree)
    file(WRITE "${tree}/vendor/endo/coro/Task.hpp" "#pragma once\n")
    file(WRITE "${tree}/vendor/endo/platform/Clock.hpp" "#pragma once\n")
    file(WRITE "${tree}/vendor/endo/platform/Types.hpp" "#pragma once\n#include <cstdint>\n")
    file(WRITE "${tree}/vendor/endo/tui/Screen.hpp" "#pragma once\n#include <platform/Types.hpp>\n#include <vector>\n")
    file(WRITE "${tree}/vendor/endo/tui/runtime/TuiRuntime.hpp" "#pragma once\n#include <tui/Screen.hpp>\n#include <coro/Task.hpp>\n")
    file(WRITE "${tree}/vendor/endo/tui/Widget.hpp" "#pragma once\n#include <tui/Screen.hpp>\n#include \"runtime/Flow.hpp\"\n")
    file(WRITE "${tree}/vendor/endo/tui/runtime/Flow.hpp" "#pragma once\n#include <platform/Clock.hpp>\n")
    file(COPY "${checkCommon}" DESTINATION "${tree}/scripts/lib")
endfunction()

# The check under test, with the case's tables injected after its `# @tables-end` marker.
# @param tree The tree to write it into.
# @param exemptions The exemption rows, as quoted CMake arguments, or empty.
# @param controls The control rows, as quoted CMake arguments.
function(fastcached_write_check tree exemptions controls)
    file(READ "${check}" checkText)
    string(REGEX MATCHALL "# @tables-end[^\n]*\n" markers "${checkText}")
    list(LENGTH markers markerCount)
    if(NOT markerCount EQUAL 1)
        message(FATAL_ERROR
            "check-vendor-vocabulary-selftest: INCONCLUSIVE -- the `# @tables-end` marker occurs "
            "${markerCount} time(s) in ${check}, and the injection needs exactly one. No case was "
            "evaluated, so no verdict here means anything. Restore the single marker line after the "
            "check's tables.")
    endif()
    string(FIND "${checkText}" "# @tables-end" markerAt)
    string(SUBSTRING "${checkText}" ${markerAt} -1 tail)
    string(FIND "${tail}" "\n" tailLineEnd)
    math(EXPR splitAt "${markerAt} + ${tailLineEnd} + 1")
    string(SUBSTRING "${checkText}" 0 ${splitAt} head)
    string(SUBSTRING "${checkText}" ${splitAt} -1 rest)
    file(WRITE "${tree}/scripts/check-vendor-vocabulary.cmake"
        "${head}"
        "set(FastCachedVendorVocabularyExemptions ${exemptions})\n"
        "set(FastCachedVendorVocabularyControls ${controls})\n"
        "${rest}")
endfunction()

function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}"
                -P "${tree}/scripts/check-vendor-vocabulary.cmake"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE runResult)
    set(combined "${captured}${capturedErrors}")
    # A spawn that never RAN is not a verdict (#747): a launch failure comes back as a string.
    if(NOT runResult MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "check-vendor-vocabulary-selftest: INCONCLUSIVE -- the check could not be RUN (${runResult}). "
            "This is NOT a rule regression: no case was evaluated.")
    endif()
    # `CMake Error|CMake Warning`, never the error word alone (#672).
    set(sawSignal FALSE)
    if(combined MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    # CMake wraps diagnostics, so a phrase can straddle a line break. Flatten before matching.
    # A `;` becomes `,`: this output is quoted into the failure LIST, where a semicolon would
    # split one failure into two and overstate the count.
    string(REGEX REPLACE "[ \t\r\n]+" " " flattened "${combined}")
    string(REPLACE ";" "," flattened "${flattened}")
    set(${outObjected} ${sawSignal} PARENT_SCOPE)
    set(${outOutput} "${flattened}" PARENT_SCOPE)
endfunction()

# Run one case and judge it.
#
# @param name Case name.
# @param probeBody Content of the probe source.
# @param exemptions Injected exemption rows, or empty.
# @param controls Injected control rows.
# @param expect `accept` or `refuse`.
# @param mustSay A phrase the (flattened) output must contain, or empty. For a refusal this is
#        what says WHICH refusal fired, since a case that refuses for a neighbouring reason is
#        green and testing nothing.
# @param why What it means when this case fails.
#
# A function and not a macro: a macro substitutes its arguments TEXTUALLY and re-parses them,
# so `backslash`'s probe body would lose its backslash a second time and arrive as an invalid
# escape rather than as the include under test.
function(fastcached_case name probeBody exemptions controls expect mustSay why)
    math(EXPR caseCount "${caseCount} + 1")
    fastcached_shared_tree("${probeBody}" "${exemptions}" "${controls}" caseTree)
    fastcached_run_check("${caseTree}" caseObjected caseOutput)
    if("${expect}" STREQUAL "refuse" AND NOT caseObjected)
        list(APPEND failures "${name}: accepted, and must refuse -- ${why}")
    elseif("${expect}" STREQUAL "accept" AND caseObjected)
        list(APPEND failures "${name}: refused, and must accept -- ${why}. Output: ${caseOutput}")
    elseif(NOT "${mustSay}" STREQUAL "")
        string(FIND "${caseOutput}" "${mustSay}" caseSaid)
        if(caseSaid EQUAL -1)
            list(APPEND failures "${name}: ${expect}ed without saying `${mustSay}`, so something else decided it -- ${why}. Output: ${caseOutput}")
        endif()
    endif()
    set(caseCount ${caseCount} PARENT_SCOPE)
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

set(legal
"#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <tui/Screen.hpp>
#include <vector>
")

# --- accepting ---------------------------------------------------------------------------
fastcached_case("clean" "${legal}" "" "${defaultControls}" accept
    "1 include(s) resolving into vendor/endo"
    "only legal includes, one resolving into vendor/ and reaching nothing forbidden through it -- a check refusing this refuses everything, and the count proves the resolver was reached")

fastcached_case("neighbours"
"${legal}#include <platform/Types.hpp>
#include <FastCache/Platform/Clock.hpp>
#include \"coroutine/Helper.hpp\"
#include <mycoro/Helper.hpp>
" "" "${defaultControls}" accept ""
    "endo's other platform headers, our own Platform/Clock.hpp, and directories merely NAMED like coro are not the vocabulary -- the refusal text promises exactly that")

# A first-party file beside the includer shadows vendor/: the compiler takes it first.
fastcached_make_tree("shadow"
"#include \"tui/runtime/TuiRuntime.hpp\"
" "" "${defaultControls}" shadowTree)
file(WRITE "${shadowTree}/src/apps/probe/tui/runtime/TuiRuntime.hpp" "#pragma once\n#include <vector>\n")
math(EXPR caseCount "${caseCount} + 1")
fastcached_run_check("${shadowTree}" objected output)
if(objected)
    list(APPEND failures "shadow: a quoted include resolving BESIDE the includer, to a first-party file, was followed into vendor/ instead -- the compiler would never read the vendored one. Output: ${output}")
endif()

fastcached_case("bracketAlone"
"#include <tui/Screen.hpp>   // a stray ] in a comment
#include <vector>
" "" "${defaultControls}" accept ""
    "a stray bracket with no violation behind it -- without this, a check refusing any bracket passes `bracket` for the wrong reason")

# --- the spelling --------------------------------------------------------------------------
fastcached_case("angled" "${legal}#include <coro/Task.hpp>\n" "" "${defaultControls}" refuse
    "includes <coro/Task.hpp>" "the rule this check carries, in its plainest spelling")
fastcached_case("quoted" "${legal}#include \"platform/Clock.hpp\"\n" "" "${defaultControls}" refuse
    "includes \"platform/Clock.hpp\"" "a quoted spelling reaches vendor/ through the include root exactly as an angled one does")
fastcached_case("backslash" "${legal}#include <coro\\Task.hpp>\n" "" "${defaultControls}" refuse
    "includes <coro/Task.hpp>" "MSVC reads a backslash as a separator -- the tokenising splitter blanks it, so it must be mapped first")
fastcached_case("dotdot" "${legal}#include <tui/../coro/Task.hpp>\n" "" "${defaultControls}" refuse
    "includes <coro/Task.hpp>" "a `..` walk into coro/ is normalised and refused under its real name")
fastcached_case("conditional"
"${legal}#if defined(_WIN32)
#include <coro/Task.hpp>
#endif
" "" "${defaultControls}" refuse "includes <coro/Task.hpp>"
    "an include behind #if ships uncompiled on every leg but one, which is why this reads every include line")
fastcached_make_tree("testFile" "${legal}" "" "${defaultControls}" testTree)
file(WRITE "${testTree}/src/apps/probe/Probe_test.cpp" "#include <coro/Task.hpp>\n")
math(EXPR caseCount "${caseCount} + 1")
fastcached_run_check("${testTree}" objected output)
string(FIND "${output}" "Probe_test.cpp includes <coro/Task.hpp>" said)
if(NOT objected OR said EQUAL -1)
    list(APPEND failures "testFile: a *_test.cpp including coro/ was not refused by name -- tests are first-party code and in scope here, unlike net-boundary. Output: ${output}")
endif()

# Every other spelling case also RESOLVES into vendor/, so the closure would refuse it with
# the spelling arm deleted. This one names a file vendor/ does not carry.
fastcached_case("unvendored" "${legal}#include <coro/WhenAll.hpp>\n" "" "${defaultControls}" refuse
    "includes <coro/WhenAll.hpp>"
    "a forbidden spelling is refused whether or not it names a vendored file")

# --- the closure ---------------------------------------------------------------------------
fastcached_case("direct" "${legal}#include <tui/runtime/TuiRuntime.hpp>\n" "" "${defaultControls}" refuse
    "tui/runtime/TuiRuntime.hpp -> coro/Task.hpp"
    "the likeliest real violation: no forbidden spelling under src/, coro::Task in scope anyway")
fastcached_case("deep" "${legal}#include <tui/Widget.hpp>\n" "" "${defaultControls}" refuse
    "tui/Widget.hpp -> tui/runtime/Flow.hpp -> platform/Clock.hpp"
    "two hops, the second a quoted include resolved beside the VENDORED file -- no real vendored header has this shape today, so nothing else proves it")

fastcached_case("besideIntoVendor" "${legal}#include \"../../../vendor/endo/tui/runtime/TuiRuntime.hpp\"\n" ""
    "${defaultControls}" refuse "tui/runtime/TuiRuntime.hpp -> coro/Task.hpp"
    "a quoted include spelling its way into vendor/endo starts with `..`, not a vendored directory, and still reaches coro/")

# `tui/a_b.hpp` reaches nothing and `tui/a/b.hpp` reaches coro/. A memo keyed on
# MAKE_C_IDENTIFIER files both under `tui_a_b_hpp`, so whichever is asked first answers for
# the other -- here the clean one first, so the violation is waved through.
fastcached_make_tree("memoCollision" "${legal}#include <tui/a_b.hpp>\n#include <tui/a/b.hpp>\n" ""
    "${defaultControls}" memoTree)
file(WRITE "${memoTree}/vendor/endo/tui/a_b.hpp" "#pragma once\n")
file(WRITE "${memoTree}/vendor/endo/tui/a/b.hpp" "#pragma once\n#include <coro/Task.hpp>\n")
math(EXPR caseCount "${caseCount} + 1")
fastcached_run_check("${memoTree}" objected output)
string(FIND "${output}" "tui/a/b.hpp -> coro/Task.hpp" said)
if(NOT objected OR said EQUAL -1)
    list(APPEND failures "memoCollision: tui/a/b.hpp reaches coro/ and was not refused by its route -- the memo answered for it with tui/a_b.hpp's result. Output: ${output}")
endif()

# --- a bracket does not hide a violation ---------------------------------------------------
fastcached_case("bracket"
"#include <tui/Screen.hpp>   // a stray ] in a comment
#include <coro/Task.hpp>
" "" "${defaultControls}" refuse "includes <coro/Task.hpp>"
    "a stray `]` on a kept line merged every later include away under file(STRINGS), which net-boundary measured as a green run over a violation")

# --- the exemption table -------------------------------------------------------------------
fastcached_case("exempt" "${legal}#include <tui/runtime/TuiRuntime.hpp>\n"
    "\"src/apps/probe/Probe.cpp|synthetic adapter\"" "${defaultControls}" accept
    "outside 1 exemption(s)"
    "an exempted file reaching the vocabulary is the one legitimate crossing, and the summary must count it")
fastcached_case("staleReach" "${legal}"
    "\"src/apps/probe/Probe.cpp|synthetic adapter\"" "${defaultControls}" refuse
    "no longer reaches"
    "an exemption for a file that reaches nothing has outlived its reason")
fastcached_case("staleMissing" "${legal}"
    "\"src/apps/probe/Gone.cpp|synthetic adapter\"" "${defaultControls}" refuse
    "does not exist"
    "an exemption for a file that is not there exempts nothing")
fastcached_case("staleNotSource" "${legal}"
    "\"vendor/endo/tui/Screen.hpp|synthetic misplaced adapter\"" "${defaultControls}" refuse
    "exempts nothing"
    "a row naming a file this check never reads as first-party source exempts nothing")

# --- the control ---------------------------------------------------------------------------
fastcached_case("controlDead" "${legal}" "" "\"tui/Screen.hpp|synthetic dead control\"" refuse
    "was NOT seen to reach"
    "a control that finds nothing means the zero for src/ is not a finding")
fastcached_case("controlMissing" "${legal}" "" "\"tui/Gone.hpp|synthetic missing control\"" refuse
    "is named in the control table but does not exist"
    "a control naming no file proves nothing")
fastcached_case("controlEmpty" "${legal}" "" "" refuse
    "the control table is empty"
    "a control table with no rows has found nothing, which is what a control exists to rule out")

# --- a scan of nothing ---------------------------------------------------------------------
fastcached_make_tree("vacuous" "" "" "${defaultControls}" vacuousTree)
file(REMOVE "${vacuousTree}/src/apps/probe/Probe.cpp")
math(EXPR caseCount "${caseCount} + 1")
fastcached_run_check("${vacuousTree}" objected output)
if(NOT objected)
    list(APPEND failures "vacuous: a src/ with no sources was reported clean -- an empty scan agrees with every rule")
else()
    string(FIND "${output}" "examined nothing" said)
    if(said EQUAL -1)
        list(APPEND failures "vacuous: refused without saying it examined nothing, so another refusal decided it. Output: ${output}")
    endif()
endif()

fastcached_make_tree("noVendor" "${legal}" "" "${defaultControls}" noVendorTree)
file(REMOVE_RECURSE "${noVendorTree}/vendor")
math(EXPR caseCount "${caseCount} + 1")
fastcached_run_check("${noVendorTree}" objected output)
string(FIND "${output}" "positive control cannot run" said)
if(NOT objected OR said EQUAL -1)
    list(APPEND failures "noVendor: with no vendor/endo the check did not refuse for THAT reason, so a missing vendor tree is caught, if at all, by a neighbouring refusal. Output: ${output}")
endif()

# --- the refusal text ----------------------------------------------------------------------
# A guard's remedy text is the part most people read and the part nothing tests. #1377's
# acceptance asks that it say what the rule does NOT cover, so it is asserted here.
# Nothing is added or removed, so it shares the tree.
fastcached_shared_tree("${legal}#include <coro/Task.hpp>\n" "" "${defaultControls}" remedyTree)
math(EXPR caseCount "${caseCount} + 1")
fastcached_run_check("${remedyTree}" objected output)
foreach(phrase IN ITEMS "does NOT cover" "<tui/...> includes are fine" "<platform/SignalHandler.hpp>"
                        "ARE the first-party vocabulary" "vendor/ itself is not scanned")
    string(FIND "${output}" "${phrase}" said)
    if(said EQUAL -1)
        list(APPEND failures "remedyText: the refusal no longer says `${phrase}`, so a reader acting on it may over-apply the rule")
    endif()
endforeach()

# ---------------------------------------------------------------------------
list(LENGTH failures failureCount)
if(failureCount GREATER 0)
    string(REPLACE ";" "\n  " printable "${failures}")
    message(FATAL_ERROR
        "check-vendor-vocabulary-selftest: ${failureCount} failure(s) across ${caseCount} case(s), and a case can fail more than one assertion:\n"
        "  ${printable}\n")
endif()

message(STATUS "check-vendor-vocabulary-selftest: ${caseCount} case(s), each seen to behave as claimed")
