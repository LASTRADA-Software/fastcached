# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every
# policy the project has not stated.
cmake_minimum_required(VERSION 3.28)
#
# `tsan-binaries` must be SEEN to refuse, on each thing it claims and on nothing
# else -- and to ACCEPT, which is the direction a guard is not known to work in
# until somebody has watched it. #1031 is two days of a gate failing CLOSED and
# unconditionally with a confidently worded false cause, and its `none` arm had
# on the evidence never once been observed answering.
#
# `tsan-binaries` runs the check against THIS tree, where a green run is equally
# consistent with the check working and with it having quietly stopped reading:
# the whole subject is a binary nobody noticed, so a reader that matches nothing
# reports exactly the same clean line as a tree with nothing to report.
#
# ## The trees are staged FROM the check, never from a copy of it
#
# This file `include()`s `check-tsan-binaries.cmake` with
# `FastCachedTsanBinariesDefinitionsOnly` set, which returns before the scan, and
# stages one registration per row of the gate's own `TARGETS` table and one per
# row of the check's own exemption list. So a row added to either is a row this
# file stages, and no list here can drift from the shipped one -- a second list
# is not a cross-check, it is a second thing to be wrong, and one that had drifted
# would agree with itself perfectly on every run.
#
# ## The verdict is the OUTPUT, and it is FLATTENED first
#
# `message(WARNING)` exits 0 on every CMake while printing a diagnostic, so a
# check that merely warned would pass an exit-code test. And CMake WRAPS a
# diagnostic at about 74 columns, so a phrase can exist in the output and in no
# single LINE of it. Every expectation here is matched against flattened text.
#
# ## A spawn that never RAN is not a verdict
#
# `execute_process` reports a failure to LAUNCH as a STRING rather than a number,
# and a discarded `RESULT_VARIABLE` reads that as the check having answered --
# every `refuse` arm then passes and every `pass` arm fails, which presents as a
# rule regression and sends the next reader hunting a defect that is not there
# (#747, on `check-net-boundary-selftest`). It is INCONCLUSIVE here and says so.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-tsan-binaries-selftest.cmake
#
# Exit: the verdict is the OUTPUT, not the status.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(FastCachedBinariesCheck "${FASTCACHED_SOURCE_DIR}/scripts/check-tsan-binaries.cmake")
set(FastCachedScopeCheckPath "${FASTCACHED_SOURCE_DIR}/scripts/check-tsan-scope.cmake")
set(FastCachedGatePath "${FASTCACHED_SOURCE_DIR}/scripts/tsan-gate.sh")
foreach(required
        "${FastCachedBinariesCheck}" "${FastCachedScopeCheckPath}" "${FastCachedGatePath}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "the subject is missing: ${required}")
    endif()
endforeach()

# Definitions only: this returns before the scan, so nothing here has to restate
# the gate's target list or the check's exemption rows.
set(FastCachedTsanBinariesDefinitionsOnly TRUE)
include("${FastCachedBinariesCheck}")
unset(FastCachedTsanBinariesDefinitionsOnly)

if(NOT FastCachedTsanGateTargets OR NOT FastCachedTsanBinaryExemptions)
    message(FATAL_ERROR
        "check-tsan-binaries-selftest: including ${FastCachedBinariesCheck} "
        "yielded no gate targets or no exemption rows.\n"
        "Every case below stages from those, so an empty one would run the whole "
        "file against nothing and report it all green -- which is this file's own "
        "subject, one level up.")
endif()

# Every tree starts with `file(REMOVE_RECURSE)` on a caller-supplied path, so the
# path is checked before anything is deleted. Absolute and at least two segments
# deep: a relative one would be resolved against whatever directory ctest ran
# this in, and `/` or `/x` is nothing a scratch tree should ever be.
if(NOT IS_ABSOLUTE "${FASTCACHED_SCRATCH_DIR}"
   OR NOT FASTCACHED_SCRATCH_DIR MATCHES "[^/\\]+[/\\][^/\\]+")
    message(FATAL_ERROR
        "check-tsan-binaries-selftest: FASTCACHED_SCRATCH_DIR "
        "(${FASTCACHED_SCRATCH_DIR}) must be an absolute path at least two "
        "segments deep; this file removes it recursively.")
endif()

set(selftestRan 0)
set(selftestFailed 0)

# The exemption targets and the directories their claims are about, derived from
# the shipped rows rather than listed here.
set(FastCachedExemptTargets "")
set(FastCachedExemptDirectories "")
set(FastCachedShippedExemptionLines "")
foreach(exemptionRow IN LISTS FastCachedTsanBinaryExemptions)
    string(APPEND FastCachedShippedExemptionLines "    \"${exemptionRow}\"\n")
    string(FIND "${exemptionRow}" "|" bar)
    if(NOT bar EQUAL -1)
        string(SUBSTRING "${exemptionRow}" 0 ${bar} exemptTarget)
        list(APPEND FastCachedExemptTargets "${exemptTarget}")
        math(EXPR restStart "${bar} + 1")
        string(SUBSTRING "${exemptionRow}" ${restStart} -1 rest)
        string(FIND "${rest}" "|" secondBar)
        if(NOT secondBar EQUAL -1)
            string(SUBSTRING "${rest}" 0 ${secondBar} exemptDirectory)
            list(APPEND FastCachedExemptDirectories "${exemptDirectory}")
        endif()
    endif()
endforeach()
list(LENGTH FastCachedExemptTargets exemptTargetCount)
list(LENGTH FastCachedExemptDirectories exemptDirectoryCount)
if(NOT exemptTargetCount EQUAL exemptDirectoryCount)
    message(FATAL_ERROR
        "check-tsan-binaries-selftest: the shipped exemption rows yielded "
        "${exemptTargetCount} target(s) but ${exemptDirectoryCount} directory(ies). "
        "Every row is `target|directory|reason`, and the staged trees give each "
        "directory a source, so a row without one would stage a baseline that "
        "refuses for a reason no case names.")
endif()

# The shipped table plus @p extraLines, as the text a staged check carries. A case
# about ONE planted row must keep the shipped rows too: replacing the table would
# leave every shipped binary registered and unexempt, and a case expected to PASS
# would then refuse for that instead.
function(FastCachedExemptionsPlus extraLines out)
    set("${out}"
        "set(FastCachedTsanBinaryExemptions\n${FastCachedShippedExemptionLines}${extraLines})\n"
        PARENT_SCOPE)
endfunction()

if(NOT FastCachedExemptTargets)
    message(FATAL_ERROR
        "check-tsan-binaries-selftest: the shipped exemption rows yielded no "
        "target names, so the accepting case would stage a tree with nothing "
        "exempt and pass for the wrong reason.")
endif()

# ---------------------------------------------------------------------------
# Staging.
#
# A tree is `scripts/` holding the three real files plus a `src/` whose one
# CMakeLists.txt registers every binary the shipped configuration accounts for.
# `extraRegistrations` and `exemptionsOverride` are what each case varies.
# ---------------------------------------------------------------------------
function(FastCachedStageBinariesTree which extraRegistrations exemptionsOverride out)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${which}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/scripts")
    file(MAKE_DIRECTORY "${tree}/src/staged")
    file(COPY "${FastCachedGatePath}" DESTINATION "${tree}/scripts")
    file(COPY "${FastCachedScopeCheckPath}" DESTINATION "${tree}/scripts")

    file(READ "${FastCachedBinariesCheck}" checkBody)
    if(NOT exemptionsOverride STREQUAL "")
        # Replace the shipped table by POSITION. A regex would have to stop at the
        # closing paren, and the shipped reasons contain parentheses of their own
        # -- `(measured 2026-09-11 ...)` -- so `[^)]*` would cut mid-row and leave
        # a file that fails to parse, which every arm would then read as the rule
        # firing.
        set(tableMarker "set(FastCachedTsanBinaryExemptions")
        string(FIND "${checkBody}" "${tableMarker}" tableStart)
        if(tableStart EQUAL -1)
            message(FATAL_ERROR
                "check-tsan-binaries-selftest: could not find `${tableMarker}` in "
                "${FastCachedBinariesCheck}. This file rewrites that table to "
                "drive its cases; if the table was renamed, rename it here too.")
        endif()
        string(SUBSTRING "${checkBody}" ${tableStart} -1 tableTail)
        string(FIND "${tableTail}" "\n)\n" tableEnd)
        if(tableEnd EQUAL -1)
            message(FATAL_ERROR
                "check-tsan-binaries-selftest: the exemption table in "
                "${FastCachedBinariesCheck} does not close with `)` in column "
                "zero. This file finds its end that way.")
        endif()
        math(EXPR tableEnd "${tableStart} + ${tableEnd} + 3")
        string(SUBSTRING "${checkBody}" 0 ${tableStart} beforeTable)
        string(SUBSTRING "${checkBody}" ${tableEnd} -1 afterTable)
        set(checkBody "${beforeTable}${exemptionsOverride}${afterTable}")
    endif()
    file(WRITE "${tree}/scripts/check-tsan-binaries.cmake" "${checkBody}")

    # One registration per gate row and one per exemption row, so the baseline is
    # exactly the shipped configuration and nothing else.
    set(registrations "")
    foreach(target IN LISTS FastCachedTsanGateTargets FastCachedExemptTargets)
        string(APPEND registrations
            "catch_discover_tests(${target} PROPERTIES SKIP_RETURN_CODE 4)\n")
    endforeach()
    string(APPEND registrations "${extraRegistrations}")
    file(WRITE "${tree}/src/staged/CMakeLists.txt" "${registrations}")

    # Each shipped exemption's directory, holding one source that names nothing, so
    # the baseline's claims are scanned and hold rather than refusing as empty.
    foreach(exemptDirectory IN LISTS FastCachedExemptDirectories)
        file(WRITE "${tree}/${exemptDirectory}/clean.cpp" "int CleanSource() { return 0; }\n")
    endforeach()

    set("${out}" "${tree}" PARENT_SCOPE)
endfunction()

# One case. `expect` is `pass` or `refuse`; `needle` is a phrase that must appear
# in the flattened output either way -- so the accepting direction asserts
# something POSITIVE was reported and not merely that nothing was.
function(FastCachedBinariesCase which tree expect needle)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}"
                -P "${tree}/scripts/check-tsan-binaries.cmake"
        OUTPUT_VARIABLE childOut
        ERROR_VARIABLE childErr
        RESULT_VARIABLE childStatus
        ENCODING NONE)

    if(NOT childStatus MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "check-tsan-binaries-selftest: INCONCLUSIVE -- the check could not be "
            "RUN for case ${which} (${childStatus}). No arm was evaluated, so no "
            "arm's verdict means anything. This is NOT a rule regression; re-run, "
            "and if it persists the spawn is the subject.")
    endif()

    string(REGEX REPLACE "[\r\n]+" " " flat "${childOut} ${childErr}")
    string(REGEX REPLACE " +" " " flat "${flat}")

    # The failure signal is TWO words, and this harness reads the sub-run's output
    # ITSELF -- ctest never sees it -- so it spells the whole pattern. Matching
    # `CMake Error` alone would score a sub-run that merely WARNS as a clean pass
    # here while ctest's FAIL_REGULAR_EXPRESSION refuses it, making this harness
    # MORE PERMISSIVE than the thing it stands for.
    set(verdict "pass")
    if(NOT childStatus EQUAL 0 OR flat MATCHES "CMake Error|CMake Warning")
        set(verdict "refuse")
    endif()

    set(problem "")
    if(NOT verdict STREQUAL expect)
        set(problem "expected to ${expect}, ${verdict}d")
    elseif(NOT flat MATCHES "${needle}")
        set(problem "did not report `${needle}`")
    endif()

    math(EXPR selftestRan "${selftestRan} + 1")
    set(selftestRan "${selftestRan}" PARENT_SCOPE)
    if(problem STREQUAL "")
        message("  ok   ${which}")
    else()
        message("  FAIL ${which}: ${problem}")
        message("       ${flat}")
        math(EXPR selftestFailed "${selftestFailed} + 1")
        set(selftestFailed "${selftestFailed}" PARENT_SCOPE)
    endif()
endfunction()

message("== check-tsan-binaries-selftest")

# -- the accepting direction, first, and asserting a positive report -------
FastCachedStageBinariesTree("baseline" "" "" tree)
FastCachedBinariesCase("baseline-tree-is-accepted" "${tree}" pass "whose [0-9]+ source")

# -- the defect the check exists for: a binary nobody decided about ---------
FastCachedStageBinariesTree("uncovered"
    "catch_discover_tests(newcomer-tests PROPERTIES SKIP_RETURN_CODE 4)\n" "" tree)
FastCachedBinariesCase("an-unlisted-binary-is-refused" "${tree}" refuse "newcomer-tests")

# The DISCRIMINATION for comment stripping, and it needs both halves: the same
# registration commented out must be accepted. Without this pair, a reader that
# stripped nothing and a reader that stripped everything both pass the case above.
FastCachedStageBinariesTree("commented"
    "# catch_discover_tests(newcomer-tests PROPERTIES SKIP_RETURN_CODE 4)\n" "" tree)
FastCachedBinariesCase("a-commented-registration-is-not-a-call-site" "${tree}" pass "Catch2 binary")

FastCachedStageBinariesTree("indented-comment"
    "    #   catch_discover_tests(newcomer-tests)\n" "" tree)
FastCachedBinariesCase("an-indented-comment-is-not-a-call-site" "${tree}" pass "Catch2 binary")

# A backticked mention with no `(` is the other half of the prose problem, and it
# is what the second filter is for -- the tree really does hold sixteen of these.
FastCachedStageBinariesTree("prose"
    "set(note \"see catch_discover_tests for how cases register\")\n" "" tree)
FastCachedBinariesCase("a-mention-with-no-paren-is-not-a-call-site" "${tree}" pass "Catch2 binary")

# -- the exemption table's own failure modes --------------------------------
FastCachedStageBinariesTree("empty-reason" ""
    "set(FastCachedTsanBinaryExemptions\n    \"lonely-tests|src/lonely|\"\n)\n" tree)
FastCachedBinariesCase("an-exemption-with-no-reason-is-refused" "${tree}" refuse "EMPTY reason")

FastCachedStageBinariesTree("stale" ""
    "set(FastCachedTsanBinaryExemptions\n    \"gone-tests|src/gone|it was here once\"\n)\n" tree)
FastCachedBinariesCase("a-stale-exemption-is-refused" "${tree}" refuse "STALE exemption")

# An exemption naming a binary the gate ALSO runs is a contradiction, and it is
# the shape a widening leaves behind: somebody adds the TARGETS row and forgets
# to delete the exemption, after which the row could be removed again with the
# check still green.
list(GET FastCachedTsanGateTargets 0 firstGateTarget)
FastCachedStageBinariesTree("contradiction" ""
    "set(FastCachedTsanBinaryExemptions\n    \"${firstGateTarget}|src/staged|it is also a row\"\n)\n" tree)
FastCachedBinariesCase("an-exemption-that-is-also-a-row-is-refused" "${tree}" refuse "BOTH a TARGETS row")

# A semicolon splits a CMake list element, so this row arrives as two and the
# tail carries no `|`. It is a real defect this table shipped in its first draft,
# and the refusal has to name the CAUSE -- `carries no |` is a true description
# of a symptom that is somewhere else entirely.
FastCachedStageBinariesTree("semicolon" ""
    "set(FastCachedTsanBinaryExemptions\n    \"lonely-tests|src/lonely|one reason; and another\"\n)\n" tree)
FastCachedBinariesCase("a-semicolon-in-a-reason-is-refused-by-cause" "${tree}" refuse "SEMICOLON in a")

# -- an exemption's claim is re-measured, not recorded ----------------------
#
# Each case plants one exempt binary, `planted-tests`, whose claim is about
# `src/planted`, beside the shipped rows. The refusing cases are driven once per
# row of the check's own primitive table, so a primitive added there is a
# primitive this file plants.
set(plantedRegistration "catch_discover_tests(planted-tests PROPERTIES SKIP_RETURN_CODE 4)\n")
FastCachedExemptionsPlus("    \"planted-tests|src/planted|its sources name no thread primitive\"\n" plantedTable)

foreach(primitive IN LISTS FastCachedTsanThreadPrimitives)
    string(MAKE_C_IDENTIFIER "${primitive}" primitiveSlug)
    FastCachedStageBinariesTree("claim-${primitiveSlug}" "${plantedRegistration}" "${plantedTable}" tree)
    file(WRITE "${tree}/src/planted/worker.cpp"
        "#include <cstddef>\nvoid Spawn() { auto unused = ${primitive}; }\n")
    FastCachedBinariesCase("an-exempt-directory-naming-${primitiveSlug}-is-refused"
        "${tree}" refuse "src/planted/worker.cpp:2 names ${primitive}")
endforeach()

# THE FLOOR, and it is a deliberate SECOND SPELLING. Do not fold it into the loop
# above. That loop is generated from the check's own list, so it notices a primitive
# ARRIVING and is blind to one LEAVING: delete `ThreadPoolExecutor` from
# `FastCachedTsanThreadPrimitives` and the loop simply plants one case fewer, all
# green. These two are the primitives this rule exists for -- `std::thread` is what
# every exemption reason claimed, and `ThreadPoolExecutor` is how the stale
# `fastcache-cli-tests` row went false without anybody spelling a thread -- so they
# are written out here independently of the list. A floor, not a mirror: the list
# may grow past it, and it may not shrink below it.
foreach(pinnedPrimitive IN ITEMS "std::thread" "ThreadPoolExecutor")
    string(MAKE_C_IDENTIFIER "${pinnedPrimitive}" pinnedSlug)
    FastCachedStageBinariesTree("floor-${pinnedSlug}" "${plantedRegistration}" "${plantedTable}" tree)
    file(WRITE "${tree}/src/planted/pinned.cpp"
        "#include <cstddef>\nvoid Spawn() { auto unused = ${pinnedPrimitive}; }\n")
    FastCachedBinariesCase("the-floor-still-refuses-${pinnedSlug}"
        "${tree}" refuse "src/planted/pinned.cpp:2 names ${pinnedPrimitive}")
endforeach()

# The accepting twins. Without the comment case, a scan that refused every file
# mentioning a primitive anywhere passes the loop above; without the near-miss
# case, one matching substrings does.
FastCachedStageBinariesTree("claim-comment" "${plantedRegistration}" "${plantedTable}" tree)
file(WRITE "${tree}/src/planted/worker.cpp"
    "// std::thread would be wrong here\n/// ThreadPoolExecutor is not used\n/* std::jthread\n * std::async and pthread_create\n */\nint Quiet() { return 0; }\n")
FastCachedBinariesCase("a-primitive-in-a-comment-is-not-a-thread" "${tree}" pass "whose [0-9]+ source")

FastCachedStageBinariesTree("claim-near-miss" "${plantedRegistration}" "${plantedTable}" tree)
file(WRITE "${tree}/src/planted/worker.cpp"
    "void Yield() { std::this_thread::yield(); }\nstruct ThreadPoolExecutorLike {};\nvoid my_pthread_create_wrapper();\nint std_async_count = 0;\n")
FastCachedBinariesCase("a-longer-name-is-not-a-primitive" "${tree}" pass "whose [0-9]+ source")

# Where the scan looks, and how it reports.
FastCachedStageBinariesTree("claim-deep" "${plantedRegistration}" "${plantedTable}" tree)
file(WRITE "${tree}/src/planted/clean.cpp" "int Clean() { return 0; }\n")
file(WRITE "${tree}/src/planted/deep/inner.hpp" "std::jthread worker;\n")
FastCachedBinariesCase("a-primitive-in-a-subdirectory-is-found" "${tree}" refuse "src/planted/deep/inner.hpp:1 names std::jthread")

FastCachedStageBinariesTree("claim-many" "${plantedRegistration}" "${plantedTable}" tree)
string(REPEAT "std::thread worker;\n" 10 manyHits)
file(WRITE "${tree}/src/planted/worker.cpp" "${manyHits}")
math(EXPR hiddenHits "10 - ${FastCachedTsanClaimHitsShown}")
FastCachedBinariesCase("hits-past-the-cap-are-counted-not-dropped" "${tree}" refuse "and ${hiddenHits} more of 10")

# A claim about nothing verifies nothing.
FastCachedStageBinariesTree("claim-missing" "${plantedRegistration}" "${plantedTable}" tree)
FastCachedBinariesCase("an-exempt-directory-that-does-not-exist-is-refused" "${tree}" refuse "which does not exist")

FastCachedStageBinariesTree("claim-empty" "${plantedRegistration}" "${plantedTable}" tree)
file(WRITE "${tree}/src/planted/NOTES.md" "std::thread lives in prose only\n")
FastCachedBinariesCase("an-exempt-directory-with-no-source-is-refused" "${tree}" refuse "A claim about an empty directory")

FastCachedExemptionsPlus("    \"planted-tests|its sources name no thread primitive\"\n" oneBarTable)
FastCachedStageBinariesTree("claim-no-directory" "${plantedRegistration}" "${oneBarTable}" tree)
FastCachedBinariesCase("a-row-naming-no-directory-is-refused" "${tree}" refuse "carries one")

FastCachedExemptionsPlus("    \"planted-tests|src/../elsewhere|its sources name no thread primitive\"\n" outsideTable)
FastCachedStageBinariesTree("claim-outside" "${plantedRegistration}" "${outsideTable}" tree)
FastCachedBinariesCase("a-directory-outside-src-is-refused" "${tree}" refuse "not a path under")

# -- the ways this check can stop being able to answer ----------------------
FastCachedStageBinariesTree("no-registrations" "" "" tree)
file(WRITE "${tree}/src/staged/CMakeLists.txt" "# nothing registers here\n")
FastCachedBinariesCase("a-tree-with-no-registration-is-refused" "${tree}" refuse "registration at all")

FastCachedStageBinariesTree("no-cmakelists" "" "" tree)
file(REMOVE_RECURSE "${tree}/src")
FastCachedBinariesCase("a-tree-with-no-CMakeLists-is-refused" "${tree}" refuse "found no CMakeLists.txt to scan")

FastCachedStageBinariesTree("no-gate" "" "" tree)
file(REMOVE "${tree}/scripts/tsan-gate.sh")
FastCachedBinariesCase("a-tree-with-no-gate-script-is-refused" "${tree}" refuse "CMake Error")

FastCachedStageBinariesTree("no-scope-check" "" "" tree)
file(REMOVE "${tree}/scripts/check-tsan-scope.cmake")
FastCachedBinariesCase("a-tree-with-no-scope-check-is-refused" "${tree}" refuse "does not exist")

# -- the mutation that proves the check reads the GATE and not a copy -------
#
# Deleting the third row from the staged gate must make the binary it named
# uncovered. If this passed, the check would be carrying its own idea of what the
# gate runs, which is the one defect the include exists to prevent.
list(LENGTH FastCachedTsanGateTargets gateTargetCount)
if(gateTargetCount GREATER 1)
    list(GET FastCachedTsanGateTargets 1 secondGateTarget)
    FastCachedStageBinariesTree("gate-row-removed" "" "" tree)
    file(READ "${tree}/scripts/tsan-gate.sh" gateBody)
    string(REPLACE "\n    \"${secondGateTarget}|" "\n    \"REMOVED-${secondGateTarget}|"
           gateBody "${gateBody}")
    file(WRITE "${tree}/scripts/tsan-gate.sh" "${gateBody}")
    FastCachedBinariesCase("removing-a-gate-row-uncovers-its-binary" "${tree}" refuse "${secondGateTarget}")
else()
    message(FATAL_ERROR
        "check-tsan-binaries-selftest: the gate names ${gateTargetCount} target(s), "
        "so the row-removal case cannot be staged. That case is what proves this "
        "check reads the gate rather than a copy, and skipping it silently is how "
        "a self-test stops testing the thing it is named for.")
endif()

# A self-test that stopped early must not look like one that judged something:
# `set -e`'s CMake analogue is a `message(FATAL_ERROR)` in a helper, and a run
# truncated at eight cases with no case named reads exactly like a clean one.
message("== ${selftestRan} case(s) run, ${selftestFailed} failed")

if(NOT selftestFailed EQUAL 0)
    message(FATAL_ERROR
        "check-tsan-binaries-selftest: ${selftestFailed} of ${selftestRan} case(s) "
        "failed. Each names the direction it drives, so a failure says whether "
        "the check stopped refusing something or started refusing something "
        "correct.")
endif()
