# SPDX-License-Identifier: Apache-2.0
#
# `test-loops` must be SEEN to refuse each shape it names, and seen to stay QUIET over the
# shapes it does not.
#
# Both arms, or the check is only half-tested. The quiet arm decides whether a check
# survives: this tree's tests hold over a thousand range-for loops and plenty of prose about
# the loops it forbids, and a check that refused those would be deleted rather than obeyed.
#
# ## What the staged tree carries
#
# One file per scope row, and one file per exemption row carrying the site that row exempts
# -- so every case also proves the exemption rows still match what they claim to, and a case
# that removes one of those sites proves STALE is refused. The mode (git or directory walk)
# is part of the output and asserted on both sides, because a synthetic tree is not a git
# repository and a fixture that only ever exercised the walk would be testing the mode CI
# does not use.
#
# ## Line numbers are asserted, not just filenames
#
# `;`, `[`, `]` and `\` are CMake list structure and C++ is full of them. One case plants all
# of them ABOVE a violation and asserts the exact `file:line`, because an assertion on the
# filename alone passes under a reader that merges lines.
#
# The mutations are applied to a SYNTHESISED tree, never to the tree under test. Each case
# asserts its mutation actually LANDED before any verdict is drawn from it.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-test-loops-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-test-loops.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

# ---------------------------------------------------------------------------
# The staged tree. `CleanFile` is the file most cases mutate: range-fors, a find loop, and
# prose naming both forbidden shapes, none of which may be refused.
set(cleanFile
"// SPDX-License-Identifier: Apache-2.0
// Never write for (int i = 0; i < n; ++i) here, and never while (!flag.load()) either.
#include <ranges>
#include <string_view>

int Clean(std::string_view text)
{
    auto count = 0;
    for (auto const i: std::views::iota(0, 3))
        count += i;
    for (auto const c: text | std::views::filter([](char x) { return x != ';'; }))
        count += c;
    auto at = text.find('a');
    while (at != std::string_view::npos)
        at = text.find('a', at + 1);
    return count;
}
")
set(helperHeader
"// SPDX-License-Identifier: Apache-2.0
#pragma once
inline int Helper() { return 0; }
")
# One file per exemption row in the check, each carrying the site its row names.
set(canaryFile
"int main()
{
    for (auto r = 0; r < 3; ++r)
        for (auto i = 0; i < 3; ++i)
            ;
    for (auto i = 0; i < 3; ++i)
        ;
}
")
set(cancelFile "void Wait(Out* out) { while (!out->arming.load(std::memory_order_acquire)) Sleep(); }\n")
set(frameFile "void Hold() { while (_held.load(std::memory_order_acquire)) Sleep(); }\n")
set(liveFile "void Beat(std::atomic<bool>* stopping) { while (!stopping->load(std::memory_order_acquire)) Sleep(); }\n")

# ---------------------------------------------------------------------------
# Stage a tree, apply one mutation, run the check, return its collapsed output.
function(fastcached_stage_and_run name target from to outOutput outApplied)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${tree}")
    # `-nested`: the tree sits one level inside a git repository whose index knows none of
    # it -- how ctest stages every walk case, under a build directory in the checkout.
    set(nestedIn "")
    if(target MATCHES "^(.*)-nested$")
        set(target "${CMAKE_MATCH_1}")
        set(nestedIn "${tree}")
        set(tree "${nestedIn}/inner")
    endif()

    # `~n~`, `~lb~`, `~rb~`, `~sc~`, `~bar~` and `~bs~` are a newline, `[`, `]`, `;`, `|` and a backslash:
    # none of them can sit literally in a case row, which is a CMake list.
    foreach(var from to)
        string(REPLACE "~n~" "\n" ${var} "${${var}}")
        string(REPLACE "~lb~" "[" ${var} "${${var}}")
        string(REPLACE "~rb~" "]" ${var} "${${var}}")
        string(REPLACE "~sc~" ";" ${var} "${${var}}")
        string(REPLACE "~bar~" "|" ${var} "${${var}}")
        string(REPLACE "~bs~" "\\" ${var} "${${var}}")
    endforeach()

    set(useGit FALSE)
    if(target MATCHES "^(.*)-git$")
        set(target "${CMAKE_MATCH_1}")
        set(useGit TRUE)
    endif()

    set(files
        "src/FastCache/Core/Clean_test.cpp"
        "src/tests/Helper.hpp"
        "src/tests/TsanCanary.cpp"
        "src/FastCache/Net/CancelRead_test.cpp"
        "src/apps/fastcache-compile-node/FrameEndpoint_test.cpp"
        "src/FastCache/Protocol/LiveStreamReactors_test.cpp")
    set(texts cleanFile helperHeader canaryFile cancelFile frameFile liveFile)
    list(LENGTH files stagedCount)
    math(EXPR lastStaged "${stagedCount} - 1")
    foreach(index RANGE 0 ${lastStaged})
        list(GET texts ${index} textVar)
        set(content_${index} "${${textVar}}")
    endforeach()
    set(extraPath "")
    set(applied TRUE)

    if(target STREQUAL "none")
    elseif(target STREQUAL "newfile")
        set(extraPath "${from}")
        set(extraText "${to}")
    elseif(target MATCHES "^file([0-9])$" AND CMAKE_MATCH_1 LESS_EQUAL lastStaged)
        set(index ${CMAKE_MATCH_1})
        string(FIND "${content_${index}}" "${from}" position)
        if(position EQUAL -1)
            set(applied FALSE)
        else()
            string(REPLACE "${from}" "${to}" content_${index} "${content_${index}}")
        endif()
    elseif(target STREQUAL "noheader")
        set(dropHeader TRUE)
    elseif(target STREQUAL "nofor")
        foreach(index RANGE 0 ${lastStaged})
            set(content_${index} "int Nothing() { return 0; }\n")
        endforeach()
    else()
        message(FATAL_ERROR "unknown mutation target `${target}` in case `${name}`")
    endif()

    foreach(index RANGE 0 ${lastStaged})
        list(GET files ${index} path)
        if(dropHeader AND path STREQUAL "src/tests/Helper.hpp")
            continue()
        endif()
        file(WRITE "${tree}/${path}" "${content_${index}}")
    endforeach()
    if(NOT extraPath STREQUAL "")
        file(WRITE "${tree}/${extraPath}" "${extraText}")
    endif()

    if(NOT nestedIn STREQUAL "")
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "case `${name}` needs a git repository around its tree and no git was found. Skipping "
                "it would leave the arrangement ctest actually runs every walk case in untested")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${nestedIn}" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE outerStatus)
        if(NOT outerStatus EQUAL 0)
            message(FATAL_ERROR "case `${name}` could not create the repository around its tree (init=${outerStatus})")
        endif()
    endif()

    if(useGit)
        if(NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "case `${name}` asks for the git enumeration path and no git was found. Skipping it "
                "would leave the mode CI actually takes untested")
        endif()
        execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE initStatus)
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE addStatus)
        if(NOT initStatus EQUAL 0 OR NOT addStatus EQUAL 0)
            message(FATAL_ERROR
                "case `${name}` could not stage a git index (init=${initStatus} add=${addStatus}), so it "
                "would silently have tested the directory walk instead")
        endif()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors)
    set(combined "${captured}${capturedErrors}")
    # `message(FATAL_ERROR)` word-wraps at a column that depends on the scratch path's
    # length, so a needle can break across a line on one machine and not another.
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")
    set(${outOutput} "${combined}" PARENT_SCOPE)
    set(${outApplied} "${applied}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases.
#
#   <name>|<target>|<from>|<to>|<must ALL appear>|<must NONE appear>
#
# Needle fields are ' && '-separated; `-` means none. `CMake Error` in the last field means
# the case expects the check to PASS.
set(FastCachedTestLoopCases
    # The baseline, in both enumeration modes. Every refusal below is evidence only if
    # these pass -- and they pass only while every exemption row still matches its site.
    "baseline via walk|none|-|-|no C-style loop && directory walk (no git index) && 6 exempted site(s) in 4 row(s)|CMake Error"
    "baseline via git|none-git|-|-|no C-style loop && git ls-files|CMake Error"
    # A tree inside another checkout is walked, not read from that checkout's index, which
    # knows none of it. Asking "inside a work tree" instead of "at its top" refused every
    # case when ctest staged them under the gate's build directory.
    "baseline nested in another checkout|none-nested|-|-|no C-style loop && directory walk (no git index) && 6 exempted site(s) in 4 row(s)|CMake Error"
    "a counting loop nested in another checkout|newfile-nested|src/FastCache/Core/Fresh_test.cpp|int F()~n~{~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop && directory walk (no git index)|-"

    # THE RED ARM.
    "a counting loop|newfile|src/FastCache/Core/Fresh_test.cpp|int F()~n~{~n~    int n = 0~sc~~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        n += i~sc~~n~    return n~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:4: a C-style for loop: for (int i = 0~sc~ i < 3 && test-loops: 1 site(s) break the loop rules|-"
    "a counting loop via git|newfile-git|src/FastCache/Core/Fresh_test.cpp|int F()~n~{~n~    for (std::size_t i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop && git ls-files|-"
    "an empty header|newfile|src/FastCache/Core/Fresh_test.cpp|void F()~n~{~n~    for (~sc~~sc~)~n~        break~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-"
    "a header broken across lines|newfile|src/FastCache/Core/Fresh_test.cpp|void F()~n~{~n~    for (auto at = text.find(needle)~sc~~n~         at != npos~sc~~n~         at = text.find(needle, at + 1))~n~        Use(at)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-"
    # ...and broken BEFORE its first `;`, inside a call: the shape a long init takes, and the only one
    # a reader matching line by line cannot see at all.
    "a header broken before its first semicolon|newfile|src/FastCache/Core/Fresh_test.cpp|void F()~n~{~n~    for (auto at = text.find(~n~             needle)~sc~ at != npos~sc~ at = Next(at))~n~        Use(at)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-"
    "a loop in a shared helper header|file1|inline int Helper() { return 0~sc~ }|inline int Helper() { int n = 0~sc~ for (int i = 0~sc~ i < 2~sc~ ++i) n += i~sc~ return n~sc~ }|src/tests/Helper.hpp:3: a C-style for loop|-"
    "an atomic polled in a while|newfile|src/FastCache/Core/Spin_test.cpp|void F(std::atomic<bool> const& go)~n~{~n~    while (!go.load(std::memory_order_acquire))~n~        std::this_thread::yield()~sc~~n~}~n~|src/FastCache/Core/Spin_test.cpp:3: a while loop polling an atomic|-"
    "an atomic polled through a pointer|newfile|src/FastCache/Core/Spin_test.cpp|void F(std::atomic<bool>* done)~n~{~n~    while (Count() < 3 && !done->load())~n~        Step()~sc~~n~}~n~|src/FastCache/Core/Spin_test.cpp:3: a while loop polling an atomic|-"

    # A violation BEHIND a comment: prose above it and a trailing comment on its own line
    # must not hide it, and the line number must survive list structure planted above it.
    "a loop below prose and brackets|newfile|src/FastCache/Core/Drift_test.cpp|// for (int i = 0~sc~ i < n~sc~ ++i) is what this file must not do~n~int a~lb~4~rb~ = { 1, 2, 3, 4 }~sc~~n~// a trailing backslash ~bs~~n~void F() { for (int i = 0~sc~ i < 4~sc~ ++i) Use(a~lb~i~rb~)~sc~ } // and a trailing comment~n~|src/FastCache/Core/Drift_test.cpp:4: a C-style for loop|Drift_test.cpp:1"

    # THE QUIET ARM.
    "a commented-out loop|newfile|src/FastCache/Core/Note_test.cpp|// for (int i = 0~sc~ i < 3~sc~ ++i)~n~/* for (int j = 0~sc~ j < 3~sc~ ++j)~n~   while (!go.load()) */~n~int Note() { return 0~sc~ }~n~|no C-style loop|CMake Error"
    "a range-for whose range holds a lambda with semicolons|newfile|src/FastCache/Core/Range_test.cpp|int F(std::vector<int> const& v)~n~{~n~    int n = 0~sc~~n~    for (auto const x: v ~bar~ std::views::transform(~lb~~rb~(int y) { int z = y~sc~ return z~sc~ }))~n~        n += x~sc~~n~    return n~sc~~n~}~n~|no C-style loop|CMake Error"
    "a range-for followed by two statements|newfile|src/FastCache/Core/Range_test.cpp|void F(std::vector<int> const& v)~n~{~n~    for (auto const x: v)~n~        Use(x)~sc~~n~    Next()~sc~~n~}~n~|no C-style loop|CMake Error"
    # A C++20 range-for with an init-statement holds ONE `;`. master's RaftWire and Nonce tests
    # carry three, and the first version of this check -- one `;` was enough -- refused them.
    "a range-for with an init-statement|newfile|src/FastCache/Core/Init_test.cpp|void F(std::span<std::byte> tag)~n~{~n~    for (std::size_t index = 0~sc~ auto& byte: tag)~n~        Use(byte, index++)~sc~~n~    Next()~sc~~n~}~n~|no C-style loop|CMake Error"
    "a range-for whose init-statement breaks the line|newfile|src/FastCache/Core/Init_test.cpp|void F(std::span<std::byte const> nonce)~n~{~n~    for (std::size_t index = 0~sc~~n~         auto const byte: nonce)~n~        Use(byte, index++)~sc~~n~}~n~|no C-style loop|CMake Error"
    # A lambda's body is one element of the header, so its `;` are its own: the first version
    # stopped reading at a `{` and passed a counting loop that held a lambda.
    "a range-for whose init-statement holds a lambda|newfile|src/FastCache/Core/Init_test.cpp|void F(std::vector<int> const& values)~n~{~n~    for (auto f = ~lb~~rb~(int x) { Note(x)~sc~ return x~sc~ }~sc~ auto v: values)~n~        Use(f(v))~sc~~n~}~n~|no C-style loop|CMake Error"
    "a counting loop whose init holds a lambda|newfile|src/FastCache/Core/Fresh_test.cpp|void F()~n~{~n~    for (auto f = ~lb~~rb~ { return 0~sc~ }~sc~ f() < 3~sc~)~n~        Use()~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-"
    "a counting loop whose condition calls a lambda|newfile|src/FastCache/Core/Fresh_test.cpp|void F(int n)~n~{~n~    for (int i = 0~sc~ i < Pick(n, ~lb~~rb~ { return 1~sc~ })~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-"
    "a while that polls no atomic|newfile|src/FastCache/Core/While_test.cpp|void F(std::istream& in)~n~{~n~    std::string line~sc~~n~    while (std::getline(in, line))~n~        Use(line)~sc~~n~}~n~|no C-style loop|CMake Error"
    "a function merely named for|newfile|src/FastCache/Core/Name_test.cpp|int transform_for(int a)~sc~~n~int platform(int b)~sc~~n~int x = transform_for(1)~sc~~n~|no C-style loop|CMake Error"

    # EXEMPTIONS, both directions.
    "the exempted text in another file is not exempt|newfile|src/FastCache/Core/Copy_test.cpp|void Wait(Out* out)~n~{~n~    while (!out->arming.load(std::memory_order_acquire))~n~        Sleep()~sc~~n~}~n~|src/FastCache/Core/Copy_test.cpp:3: a while loop polling an atomic|-"
    "a second site in an exempted file needs its own row|file3|Sleep()~sc~ }|Sleep()~sc~ }~n~void Other(std::atomic<bool>& ready) { while (!ready.load()) Sleep()~sc~ }|src/FastCache/Net/CancelRead_test.cpp:2: a while loop polling an atomic|-"
    "an exemption whose site is gone is STALE|file3|while (!out->arming.load(std::memory_order_acquire)) Sleep()~sc~|WaitUntilArmed()~sc~|matching no site && out->arming.load( && STALE|-"

    # --- fails CLOSED ---
    "a scope row that matches nothing|noheader|-|-|matched no file && src/tests/*.hpp|-"
    "no for anywhere|nofor|-|-|the scan matched nothing and cannot conclude|-"
)

# ---------------------------------------------------------------------------
set(failures "")
set(caseCount 0)

foreach(row IN LISTS FastCachedTestLoopCases)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 6)
        message(FATAL_ERROR
            "case row has ${fieldCount} fields, expected 6 -- a row that does not parse would run as a "
            "different case than it reads as: [${row}]")
    endif()
    list(GET fields 0 caseName)
    list(GET fields 1 caseTarget)
    list(GET fields 2 caseFrom)
    list(GET fields 3 caseTo)
    list(GET fields 4 caseMustAppear)
    list(GET fields 5 caseMustNotAppear)

    math(EXPR caseCount "${caseCount} + 1")
    fastcached_stage_and_run("${caseCount}" "${caseTarget}" "${caseFrom}" "${caseTo}" output applied)

    if(NOT applied)
        list(APPEND failures
             "${caseName}: the mutation did not apply -- `${caseFrom}` was not found in the staged tree, so this case asserted nothing")
        continue()
    endif()

    # Which PATH the case exercised, asserted on every case and derived from its target
    # rather than restated per row: `-git` stages an index at the tree's top, and every other
    # tree -- `-nested` ones included -- must be walked. A walk that silently read another
    # checkout's index is how every case once refused under ctest.
    if(caseTarget MATCHES "-git$")
        set(expectedMode "test-loops: mode: git-index")
        set(otherMode "test-loops: mode: walk")
    else()
        set(expectedMode "test-loops: mode: walk")
        set(otherMode "test-loops: mode: git-index")
    endif()
    string(FIND "${output}" "${expectedMode}" position)
    if(position EQUAL -1)
        list(APPEND failures "${caseName}: expected `${expectedMode}` and did not see it")
    endif()
    string(FIND "${output}" "${otherMode}" position)
    if(NOT position EQUAL -1)
        list(APPEND failures "${caseName}: ran as `${otherMode}`, which is not the path this case is about")
    endif()

    if(NOT caseMustAppear STREQUAL "-")
        string(REPLACE " && " ";" needles "${caseMustAppear}")
        foreach(needle IN LISTS needles)
            # A needle may name a `;` as `~sc~`, as the staged text does: this list cannot hold one.
            string(REPLACE "~sc~" ";" needle "${needle}")
            string(FIND "${output}" "${needle}" position)
            if(position EQUAL -1)
                list(APPEND failures "${caseName}: expected to see `${needle}` and did not")
            endif()
        endforeach()
    endif()
    if(NOT caseMustNotAppear STREQUAL "-")
        string(REPLACE " && " ";" needles "${caseMustNotAppear}")
        foreach(needle IN LISTS needles)
            string(FIND "${output}" "${needle}" position)
            if(NOT position EQUAL -1)
                list(APPEND failures "${caseName}: did not expect `${needle}` and saw it")
            endif()
        endforeach()
    endif()

    # The verdict itself, separate from the needles. `CMake Error|CMake Warning`, never the
    # error word alone (#672); see `scripts/check-script-check-signals.cmake`.
    set(sawSignal FALSE)
    if(output MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    # verdict-error-only: a read of the CASE TABLE, where `CMake Error` in the last field
    # means "this case expects acceptance" -- a question about this file, not the sub-run.
    if(caseMustNotAppear MATCHES "CMake Error")
        if(sawSignal)
            list(APPEND failures "${caseName}: expected the check to pass and it refused")
        endif()
    else()
        if(NOT sawSignal)
            list(APPEND failures "${caseName}: expected the check to refuse and it passed")
        endif()
    endif()
endforeach()

if(caseCount EQUAL 0)
    message(FATAL_ERROR
        "the case table is empty, so this fixture asserted nothing -- which is exactly what a green "
        "run with no cases looks like")
endif()

if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`test-loops` did not behave as its own documentation says. A check nobody has watched")
    message("refuse is not a check -- and one nobody has watched stay QUIET over a range-for is one")
    message("that gets deleted.")
    message(FATAL_ERROR "test-loops-selftest: ${failureCount} finding(s) across ${caseCount} case(s)")
endif()

message(STATUS "test-loops-selftest: ${caseCount} case(s), every verdict as documented")
