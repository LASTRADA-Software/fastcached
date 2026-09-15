# SPDX-License-Identifier: Apache-2.0
#
# The tests' loops: no C-style `for`, and no `while` that polls an atomic.
#
# ## Why a check and not a sentence
#
# `AGENT.md` has said *C-style loops are forbidden* for as long as the tree has had a
# rulebook, and #1446's census still found them in 76 test files -- 198 sites, under the
# pattern `\bfor \([^;{]*;[^;{]*;` over `*_test.cpp` outside `vendor/`. The pattern travels
# with the number because the census that opened the ticket used a narrower one,
# `\bfor \([^:)]*;[^;]*;`, which cannot see a loop whose init names a qualified type
# (`for (std::size_t i = 0; ...)`) and counted 131. A rule stated in the files that obey it
# reaches no file that does not.
#
# The second rule is the ticket's subject. A test that waits for another thread by polling
# an atomic in a `while` either counts iterations, which a loaded host loses, or does not
# bound the wait at all, which turns the failure it exists to catch into a suite timeout
# naming nothing -- or, under a `REQUIRE` that already fired, a join that never returns.
# The census found both shapes (`Clock_test`'s worker start gate and `DashboardSampler_test`'s
# blocking read spun unbounded). The tree's one test wait is `src/tests/BoundedWait.hpp`:
# `WaitUntil` on the case's thread, `OffThreadWaits` on a helper thread.
#
# ## What it matches
#
# Over the file with its comments removed and its newlines kept
# (`fastcached_strip_comments`), so prose explaining a rule is not read as breaking it and a
# header broken across lines is still one header:
#
#   * a `for (` whose header reaches TWO `;` before the `)` that closes it -- a range-for
#     reaches its `)` first, and a range-for with an init-statement
#     (`for (std::size_t i = 0; auto& byte: bytes)`) holds only one. A brace group -- a
#     lambda body -- is read as one element, so the `;` inside it are not the header's: a
#     counting loop whose init or condition holds a lambda is still refused, and a range-for
#     whose init-statement or range holds one is still quiet;
#   * a `while (` whose condition calls `.load(` or `->load(`.
#
# ## What it does NOT cover, said here so nobody over-applies it
#
#   * A `while` that is not polling an atomic -- stepping through `find()` or `getline()`, or
#     reading until a peer is done -- is ordinary and stays. So is a `while` a COROUTINE runs
#     on a reactor; those are refused here only when they poll an atomic, and the four that
#     do are #1453's, bounded on the reactor's own clock rather than through `WaitUntil`.
#   * `vendor/` is not scanned: it is another project's code, changed upstream.
#   * Non-test sources are #1452, whose change adds its row to the scope table below.
#   * A token inside a STRING LITERAL reads as code, as in every regex-shaped reader here.
#
# ## Its residual, in both directions, so the next surprise is diagnosed rather than exempted
#
#   * FAILS OPEN on a header nested deeper than it reads: a lambda whose body holds its own
#     braces, or parentheses three deep. Such a header matches nothing, so a C-style loop
#     spelled that way passes. Deepen `headerElement` rather than exempting the site.
#   * FAILS CLOSED on a `;` inside a string or character literal in a range-for's header
#     (`for (auto const sep = ";"; auto const& item: items)`), because literals read as code:
#     that header holds two `;` as this reader counts them. C++ has no other way to put a
#     second `;` in a range-for's header outside a brace or parenthesis group.
#
# ## Exemptions
#
# A row states its REASON, so an allowed site cannot be spelled the way a forgotten one
# is, and a row that has stopped matching anything is refused as STALE: an exemption
# nobody has to keep true outlives its argument and waves the next site through.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check reports
# failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-test-loops.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# ---------------------------------------------------------------------------
# What is scanned: one row per kind of file, as a git pathspec under the source root.
#
# pathspec|what it is
set(FastCachedTestLoopScope
    "src/*_test.cpp|every Catch2 test source (#1446)"
    "src/tests/*.cpp|the shared test programs and canaries (#1446)"
    "src/tests/*.hpp|the shared test helpers (#1446)"
)

# ---------------------------------------------------------------------------
# Sites that stay, and why.
#
# rule|file|text the matched header contains|reason
#
# `rule` is `loop` or `spin`. The text is matched against the header as the scan read it,
# from its keyword to its second `;` (a loop) or its `load(` (a spin). None of the four
# fields may contain `|` or `;`, and the text may not contain `[` or `]`: these rows are a
# CMake list. Checked below rather than left as a comment nothing reads.
set(FastCachedTestLoopExemptions
    "loop|src/tests/TsanCanary.cpp|for (|The ThreadSanitizer canary's loops are its race: a change to this file is judged by a RATE over a few hundred runs (scripts/tsan-canary-rate.sh), never by a green run, and rewriting three loops would re-open that measurement for a spelling."
    "spin|src/FastCache/Net/CancelRead_test.cpp|out->arming.load(|A coroutine on the reactor waiting for the reader to arm: it cannot block its own reactor thread in WaitUntil, and its reactor-clock bound is #1453."
    "spin|src/apps/fastcache-compile-node/FrameEndpoint_test.cpp|_held.load(|A handler coroutine held until the case releases it, sleeping on its own reactor: its reactor-clock bound is #1453."
    "spin|src/FastCache/Protocol/LiveStreamReactors_test.cpp|stopping->load(|Not a wait: a heartbeat coroutine that beats until the case says stop, and whose frame the rig's reactors free when a case ends early."
)

# One element of a loop header's clause: an ordinary character, a brace group (a lambda body,
# whose `;` are its own and not the header's), or a parenthesised group -- braces nest one
# level, parentheses two.
set(braceGroup "\\{[^{}]*\\}")
set(innerParens "\\(([^;(){}]|${braceGroup})*\\)")
set(headerElement "([^;(){}]|${braceGroup}|\\(([^;(){}]|${innerParens}|${braceGroup})*\\))")
set(loopPattern "(^|[^A-Za-z0-9_])for[ \t\r\n]*\\(${headerElement}*;${headerElement}*;")
set(spinPattern "(^|[^A-Za-z0-9_])while[ \t\r\n]*\\(([^;(){}]|\\(([^;(){}]|\\([^;(){}]*\\))*\\))*(\\.|->)load[ \t\r\n]*\\(")

# ---------------------------------------------------------------------------
# Exemption rows, checked for shape before anything is decided from them.
set(exemptionIndex 0)
foreach(row IN LISTS FastCachedTestLoopExemptions)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 4)
        message(FATAL_ERROR
            "test-loops: exemption row has ${fieldCount} field(s), expected 4 "
            "(rule|file|text|reason), so it states no reason or cannot be read:\n  ${row}")
    endif()
    list(GET fields 0 exemptRule)
    list(GET fields 1 exemptFile)
    list(GET fields 2 exemptText)
    list(GET fields 3 exemptReason)
    if(NOT exemptRule MATCHES "^(loop|spin)$")
        message(FATAL_ERROR "test-loops: exemption rule `${exemptRule}` is neither `loop` nor `spin`:\n  ${row}")
    endif()
    if(exemptText MATCHES "[][]" OR exemptText STREQUAL "" OR exemptReason STREQUAL "")
        message(FATAL_ERROR
            "test-loops: exemption row names no text, no reason, or a text containing a bracket, "
            "which a CMake list cannot carry:\n  ${row}")
    endif()
    set(exempt_${exemptionIndex}_rule "${exemptRule}")
    set(exempt_${exemptionIndex}_file "${exemptFile}")
    set(exempt_${exemptionIndex}_text "${exemptText}")
    set(exempt_${exemptionIndex}_used 0)
    math(EXPR exemptionIndex "${exemptionIndex} + 1")
endforeach()
set(exemptionCount ${exemptionIndex})

# ---------------------------------------------------------------------------
# Which files, per scope row, asked of git -- a dependency cache and a build tree are
# untracked by construction -- with a walk for an export that has no index. Each row must
# match something: a row that matches nothing is a scope that has silently shrunk.
#
# The index is asked only when the source root IS the top of a work tree, never merely
# inside one. A tree nested in another checkout -- the self-test's synthetic trees, staged
# under a build directory in that checkout -- is not what that checkout's index describes:
# asked there, every row matched nothing and every case refused. `--show-prefix` is empty
# exactly at the top, so the question needs no path comparison, which is the part that
# differs between hosts.
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()
set(useGit FALSE)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" rev-parse --show-prefix
        OUTPUT_VARIABLE prefixInWorkTree ERROR_QUIET RESULT_VARIABLE gitStatus
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(gitStatus EQUAL 0 AND prefixInWorkTree STREQUAL "")
        set(useGit TRUE)
    endif()
endif()
if(useGit)
    set(scanMode "git-index")
    set(scanSource "git ls-files")
else()
    set(scanMode "walk")
    set(scanSource "directory walk (no git index)")
endif()
# The mode is part of the OUTPUT on every run, refusals included: a walk that silently
# became an index read is the defect this line exists to make visible.
message(STATUS "test-loops: mode: ${scanMode}")

set(sourceFiles "")
foreach(scopeRow IN LISTS FastCachedTestLoopScope)
    string(FIND "${scopeRow}" "|" bar)
    if(bar EQUAL -1)
        message(FATAL_ERROR "test-loops: scope row carries no '|', so it says nothing about what it is:\n  ${scopeRow}")
    endif()
    string(SUBSTRING "${scopeRow}" 0 ${bar} pathspec)
    set(rowFiles "")
    if(useGit)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" ls-files -- "${pathspec}"
            OUTPUT_VARIABLE tracked RESULT_VARIABLE lsStatus OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT lsStatus EQUAL 0)
            message(FATAL_ERROR "test-loops: `git ls-files -- ${pathspec}` failed (${lsStatus}), so the scope cannot be read")
        endif()
        if(NOT tracked STREQUAL "")
            string(REPLACE "\n" ";" rowFiles "${tracked}")
        endif()
    else()
        file(GLOB_RECURSE rowFiles RELATIVE "${FASTCACHED_SOURCE_DIR}" "${FASTCACHED_SOURCE_DIR}/${pathspec}")
    endif()
    if(NOT rowFiles)
        message("")
        message("  The scope row `${scopeRow}` matched no file via ${scanSource}.")
        message("")
        message("That is a scope that shrank without anybody deciding it should -- a moved")
        message("directory or a renamed suffix -- not a tree with nothing to refuse.")
        message(FATAL_ERROR "test-loops: a scope row matched nothing and cannot conclude")
    endif()
    list(APPEND sourceFiles ${rowFiles})
endforeach()
list(REMOVE_DUPLICATES sourceFiles)
list(SORT sourceFiles)
list(LENGTH sourceFiles fileCount)

# ---------------------------------------------------------------------------
# The scan.
set(violations "")
set(violationCount 0)
set(loopsSeen 0)
set(exemptedCount 0)

foreach(relative IN LISTS sourceFiles)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)
    # Whole-file filters first: the walk below is O(n^2), and a file with no `for` and no
    # `while` has nothing to say.
    string(FIND "${wholeFile}" "for" forAt)
    string(FIND "${wholeFile}" "while" whileAt)
    if(forAt EQUAL -1 AND whileAt EQUAL -1)
        continue()
    endif()
    fastcached_strip_comments("${wholeFile}" code)

    # Every `for (` at all, for the matched-nothing guard: this tree's tests are full of
    # range-fors, so a scan that sees none is not reading what it thinks it is.
    string(REGEX MATCHALL "(^|[^A-Za-z0-9_])for[ \t\r\n]*\\(" anyFor "${code}")
    list(LENGTH anyFor anyForCount)
    math(EXPR loopsSeen "${loopsSeen} + ${anyForCount}")

    foreach(rule loop spin)
        set(rest "${code}")
        set(consumed 0)
        while(TRUE)
            string(REGEX MATCH "${${rule}Pattern}" header "${rest}")
            if(header STREQUAL "")
                break()
            endif()
            string(FIND "${rest}" "${header}" at)
            # The leading character the pattern needs to see a keyword boundary is not part
            # of the header, and a newline there would put the site one line early.
            if(header MATCHES "^[^fw]")
                string(SUBSTRING "${header}" 1 -1 header)
                math(EXPR at "${at} + 1")
            endif()
            math(EXPR absolute "${consumed} + ${at}")
            string(SUBSTRING "${code}" 0 ${absolute} before)
            string(REGEX MATCHALL "\n" newlines "${before}")
            list(LENGTH newlines newlineCount)
            math(EXPR lineNumber "${newlineCount} + 1")

            set(exempted FALSE)
            foreach(index RANGE 0 ${exemptionCount})
                if(index EQUAL exemptionCount)
                    break()
                endif()
                if(exempt_${index}_rule STREQUAL rule AND exempt_${index}_file STREQUAL relative)
                    string(FIND "${header}" "${exempt_${index}_text}" textAt)
                    if(NOT textAt EQUAL -1)
                        math(EXPR exempt_${index}_used "${exempt_${index}_used} + 1")
                        set(exempted TRUE)
                        break()
                    endif()
                endif()
            endforeach()
            if(exempted)
                math(EXPR exemptedCount "${exemptedCount} + 1")
            else()
                # Text, not a list: a loop's header holds its own `;`, which a CMake list would
                # split -- printing the header cut at its first `;` and counting it twice.
                string(REGEX REPLACE "[ \t\r\n]+" " " shown "${header}")
                math(EXPR violationCount "${violationCount} + 1")
                if(rule STREQUAL "loop")
                    string(APPEND violations "\n  ${relative}:${lineNumber}: a C-style for loop: ${shown}")
                else()
                    string(APPEND violations "\n  ${relative}:${lineNumber}: a while loop polling an atomic: ${shown}")
                endif()
            endif()

            string(LENGTH "${header}" headerLength)
            math(EXPR advance "${at} + ${headerLength}")
            string(SUBSTRING "${rest}" ${advance} -1 rest)
            math(EXPR consumed "${consumed} + ${advance}")
        endwhile()
    endforeach()
endforeach()

if(loopsSeen EQUAL 0)
    message("")
    message("  No `for (` was found in ${fileCount} file(s) via ${scanSource}.")
    message("")
    message("This tree's tests are full of range-for loops, so zero means the comment stripping")
    message("has begun eating code or the scope reads the wrong files -- not that no test loops.")
    message(FATAL_ERROR "test-loops: the scan matched nothing and cannot conclude")
endif()

set(staleRows "")
foreach(index RANGE 0 ${exemptionCount})
    if(index EQUAL exemptionCount)
        break()
    endif()
    if(exempt_${index}_used EQUAL 0)
        string(APPEND staleRows "\n  ${exempt_${index}_rule}: ${exempt_${index}_file}: ${exempt_${index}_text}")
    endif()
endforeach()

if(violationCount GREATER 0)
    message("${violations}")
    message("")
    message("A C-style `for` is forbidden in this tree. Count with `std::views::iota`, stop early")
    message("with `std::ranges::any_of` / `all_of` or a `break`, skip a prefix with")
    message("`std::views::drop`, and step through `find()` or `getline()` with a `while`.")
    message("An `iota` whose start is not zero needs a bound that can never sit below it:")
    message("the C loop ran zero times there, and the view's precondition does not.")
    message("")
    message("A `while` polling an atomic is a wait. Wait through src/tests/BoundedWait.hpp:")
    message("`WaitUntil` on the case's thread, `OffThreadWaits` on a helper thread (asserted")
    message("with `AllReached()` after the join). It bounds the wait on a monotonic clock and")
    message("says what it waited for when it gives up. A coroutine waiting on its own reactor")
    message("cannot block that thread: bound it on the reactor's clock instead (#1453).")
    message("")
    message("Neither rule reaches a `while` that does not poll an atomic, `vendor/`, or")
    message("non-test sources (#1452). A site that must stay takes a row in")
    message("FastCachedTestLoopExemptions in ${CMAKE_CURRENT_LIST_FILE}, with its reason.")
    message("")
    message("Enumerated ${fileCount} file(s) via ${scanSource}.")
    message(FATAL_ERROR "test-loops: ${violationCount} site(s) break the loop rules")
endif()

if(NOT staleRows STREQUAL "")
    message("")
    message("FastCachedTestLoopExemptions has row(s) matching no site:${staleRows}")
    message("")
    message("The site was converted, moved or deleted. Remove the row -- a standing exemption")
    message("for a site that is gone would wave the next one of that text through silently.")
    message(FATAL_ERROR "test-loops: ${exemptionCount} exemption row(s), some STALE")
endif()

message(STATUS
    "test-loops: ${loopsSeen} `for (` across ${fileCount} file(s) via ${scanSource}, no C-style loop and no "
    "atomic-polling while outside ${exemptedCount} exempted site(s) in ${exemptionCount} row(s)")
