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
# ADDING AN EXEMPTION ROW TO THE CHECK IS TWO EDITS, AND THIS FILE IS THE SECOND. A row with
# no staged site here is reported STALE -- correctly -- which turns every baseline case red at
# once, so the failure names this file rather than the row. Stage a file carrying that row's
# site, add it to `files`/`texts` (positional, so both), and move the exempted-site tally in the
# two baseline rows. Measured: missed twice in one session, both times by whoever added the row.
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
set(liveFile "void Beat(std::atomic<bool>* stopping) { while (!stopping->load(std::memory_order_acquire)) Sleep(); }\n")

# `src/apps/fastcache-cc/ToolchainProbe.cpp`'s row: a recursive_directory_iterator advanced by
# the error-code overload. ONLY the exempted loop, so this file contributes no unbacklogged site.
set(probeFile
"void Walk(std::filesystem::recursive_directory_iterator it, std::error_code& ec)
{
    std::filesystem::recursive_directory_iterator const end;
    for (; it != end; it.increment(ec))
        ;
}
")
# `src/apps/fastcache-cc/ProcessRunner.cpp` carries TWO rows, so its file carries both sites:
# the Windows double-NUL environment block and the POSIX NULL-terminated `char**` array.
set(runnerFile
"void Windows(char const* inherited)
{
    for (char const* cursor = inherited; *cursor != '\\0';)
        cursor += 1;
}
void Posix(char** inherited)
{
    for (char** entry = inherited; entry != nullptr && *entry != nullptr; ++entry)
        ;
}
")

# `src/apps/fastcache-cli/CliCommand.cpp`'s row: the argv walk whose five `continue` paths all
# depend on the head, so a `while` would need five duplicated increments. ONLY the exempted loop,
# so this file contributes no unbacklogged site.
set(cliCommandFile
"void Parse(std::span<char const* const> args, Command& command)
{
    for (std::size_t index = 0; index < args.size(); ++index)
    {
        if (IsOption(args, index))
            continue;
        command.operands.emplace_back(args, index);
    }
}
")

# `src/apps/fastcache-cli/SocketExchange.cpp`'s row: FIVE deliberate infinite loops reading
# framed replies. Five, because the tally counts SITES and that one file-wide row covers all
# of them -- a synthetic file with one would leave the baseline four short and read as a
# miscount in the check rather than in this fixture.
set(socketExchangeFile
"void ReadReply(Socket& socket)
{
    for (;;)
    {
        if (!socket.Read())
            return;
    }
}
void ReadStatus(Socket& socket)
{
    for (;;)
    {
        if (socket.Done())
            break;
    }
}
void ReadFrame(Socket& socket)
{
    for (;;)
        if (socket.Done())
            return;
}
void ReadPush(Socket& socket)
{
    for (;;)
        if (socket.Done())
            return;
}
void Drain(Socket& socket)
{
    for (;;)
        if (socket.Done())
            return;
}
")

# `src/apps/fastcache-compile-node/NodeAnnounce.cpp`'s row: an init that resets the per-round
# redirect budget and no condition at all, so what ends it is a break inside.
set(nodeAnnounceFile
"std::size_t DialAndAnnounce(SchedulerLink& link, IEndpointDialer& dialer)
{
    for (link.BeginRound();;)
    {
        if (!link.Next())
            return 0;
    }
}
")

# `src/apps/fastcache-cc/ParallelFor.cpp`'s row: a work-stealing queue over a shared atomic.
set(parallelForFile
"void Worker(std::atomic<std::size_t>& next, std::size_t count, Slice slice)
{
    for (auto index = next.fetch_add(1, std::memory_order_relaxed); index < count;
         index = next.fetch_add(1, std::memory_order_relaxed))
    {
        slice(index);
    }
}
")

# `src/apps/fastcache-compile-node/NodeToolchains.cpp`'s row: the same work-stealing shape over
# the survey's entries.
set(nodeToolchainsFile
"void Survey(std::atomic<std::size_t>& next, Entries const& entries)
{
    for (auto index = next.fetch_add(1); index < entries.size(); index = next.fetch_add(1))
    {
        Fingerprint(entries, index);
    }
}
")

# `src/FastCache/Cli/UsageDoc.cpp`'s row: a find-and-REPLACE walk, so each search runs over a
# string the previous iteration rewrote. Its text names the INIT, because the header this table
# matches against stops at the second semicolon and the resume is in the increment.
set(usageDocFile
"std::string Expand(std::string out, std::string_view token, std::string_view value)
{
    for (auto at = out.find(token); at != std::string::npos; at = out.find(token, at + value.size()))
        out.replace(at, token.size(), value);
    return out;
}
")

# `src/apps/fastcache-cli/DashboardPanel.cpp`'s row: the mutation is on the other side -- the
# body shortens the view being searched, so the unqualified find always looks from the start of
# what is left.
set(dashboardPanelFile
"std::string Glyphs(std::string_view note, Glyphs const& glyphs)
{
    auto text = std::string {};
    for (auto at = note.find(DeltaMark); at != std::string_view::npos; at = note.find(DeltaMark))
    {
        text += note.substr(0, at);
        text += glyphs.delta;
        note.remove_prefix(at + DeltaMark.size());
    }
    return text;
}
")

# ---------------------------------------------------------------------------
# Stage a tree, apply one mutation, run the check, return its collapsed output.
function(fastcached_stage_and_run name target from to backlog outOutput outApplied)
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
    foreach(var from to backlog)
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
        "src/FastCache/Protocol/LiveStreamReactors_test.cpp"
        "src/apps/fastcache-cc/ToolchainProbe.cpp"
        "src/apps/fastcache-cc/ProcessRunner.cpp"
        "src/apps/fastcache-cli/CliCommand.cpp"
        # APPENDED, never inserted: `file<N>` mutation targets are positional, so a new entry
        # in the middle would silently re-point an existing case at a different file.
        "src/apps/fastcache-cli/SocketExchange.cpp"
        "src/apps/fastcache-compile-node/NodeAnnounce.cpp"
        "src/apps/fastcache-cc/ParallelFor.cpp"
        "src/apps/fastcache-compile-node/NodeToolchains.cpp"
        "src/FastCache/Cli/UsageDoc.cpp"
        "src/apps/fastcache-cli/DashboardPanel.cpp")
    set(texts cleanFile helperHeader canaryFile liveFile probeFile runnerFile cliCommandFile
              socketExchangeFile nodeAnnounceFile parallelForFile nodeToolchainsFile
              usageDocFile dashboardPanelFile)
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

    # The backlog, beside the tree rather than the shipped one, which names 64 production
    # files this tree does not have. `absent` writes nothing, for the missing-file case; `-` is
    # an empty table, which is still a FILE -- the two are different refusals.
    #
    # A comment and a blank line ride along on every case, so the reader's skipping of them is
    # exercised by every row rather than by one case that could rot.
    set(backlogPath "${tree}/backlog.txt")
    if(backlog STREQUAL "absent")
        file(REMOVE "${backlogPath}")
    elseif(backlog STREQUAL "-")
        file(WRITE "${backlogPath}" "# rule|file|count|issue\n\n")
    else()
        file(WRITE "${backlogPath}" "# rule|file|count|issue\n\n${backlog}\n")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}"
                "-DFASTCACHED_TEST_LOOPS_BACKLOG=${backlogPath}" -P "${check}"
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
#   <name>|<target>|<from>|<to>|<must ALL appear>|<must NONE appear>|<backlog>
#
# Needle fields are ' && '-separated; `-` means none. `CMake Error` in the must-NONE field means
# the case expects the check to PASS.
#
# The backlog field is the rows to stage, `~bar~`-separated and `~n~` between rows; `-` is an
# empty table and `absent` is no file at all. It is the last field because almost every case
# wants none, and a default that reads as `-` is one a row cannot forget to state.
set(FastCachedTestLoopCases
    # The baseline, in both enumeration modes. Every refusal below is evidence only if
    # these pass -- and they pass only while every exemption row still matches its site.
    "baseline via walk|none|-|-|no new C-style loop && directory walk (no git index) && 18 exempted site(s) in 12 row(s)|CMake Error|-"
    "baseline via git|none-git|-|-|no new C-style loop && git ls-files|CMake Error|-"
    # A tree inside another checkout is walked, not read from that checkout's index, which
    # knows none of it. Asking "inside a work tree" instead of "at its top" refused every
    # case when ctest staged them under the gate's build directory.
    "baseline nested in another checkout|none-nested|-|-|no new C-style loop && directory walk (no git index) && 18 exempted site(s) in 12 row(s)|CMake Error|-"
    "a counting loop nested in another checkout|newfile-nested|src/FastCache/Core/Fresh_test.cpp|int F()~n~{~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop && directory walk (no git index)|-|-"

    # THE RED ARM.
    "a counting loop|newfile|src/FastCache/Core/Fresh_test.cpp|int F()~n~{~n~    int n = 0~sc~~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        n += i~sc~~n~    return n~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:4: a C-style for loop: for (int i = 0~sc~ i < 3 && test-loops: 1 unbacklogged site(s)|-|-"
    "a counting loop via git|newfile-git|src/FastCache/Core/Fresh_test.cpp|int F()~n~{~n~    for (std::size_t i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop && git ls-files|-|-"
    "an empty header|newfile|src/FastCache/Core/Fresh_test.cpp|void F()~n~{~n~    for (~sc~~sc~)~n~        break~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-|-"
    "a header broken across lines|newfile|src/FastCache/Core/Fresh_test.cpp|void F()~n~{~n~    for (auto at = text.find(needle)~sc~~n~         at != npos~sc~~n~         at = text.find(needle, at + 1))~n~        Use(at)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-|-"
    # ...and broken BEFORE its first `;`, inside a call: the shape a long init takes, and the only one
    # a reader matching line by line cannot see at all.
    "a header broken before its first semicolon|newfile|src/FastCache/Core/Fresh_test.cpp|void F()~n~{~n~    for (auto at = text.find(~n~             needle)~sc~ at != npos~sc~ at = Next(at))~n~        Use(at)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-|-"
    "a loop in a shared helper header|file1|inline int Helper() { return 0~sc~ }|inline int Helper() { int n = 0~sc~ for (int i = 0~sc~ i < 2~sc~ ++i) n += i~sc~ return n~sc~ }|src/tests/Helper.hpp:3: a C-style for loop|-|-"
    "an atomic polled in a while|newfile|src/FastCache/Core/Spin_test.cpp|void F(std::atomic<bool> const& go)~n~{~n~    while (!go.load(std::memory_order_acquire))~n~        std::this_thread::yield()~sc~~n~}~n~|src/FastCache/Core/Spin_test.cpp:3: a while loop polling an atomic|-|-"
    "an atomic polled through a pointer|newfile|src/FastCache/Core/Spin_test.cpp|void F(std::atomic<bool>* done)~n~{~n~    while (Count() < 3 && !done->load())~n~        Step()~sc~~n~}~n~|src/FastCache/Core/Spin_test.cpp:3: a while loop polling an atomic|-|-"
    # A coroutine parking until a plain flag holds: no atomic, so only this rule sees it (#1453's
    # EpollConnector and IocpConnector shape).
    "a coroutine parking a turn at a time|newfile|src/FastCache/Net/Park_test.cpp|DetachedTask F(IReactor* loop, bool const* done)~n~{~n~    while (!*done)~n~        co_await FastCache::ResumeOn { *loop }~sc~~n~}~n~|src/FastCache/Net/Park_test.cpp:3: a coroutine polling on its reactor && test-loops: 1 unbacklogged site(s)|-|-"
    "a coroutine sleeping in a braced body|newfile|src/FastCache/Net/Park_test.cpp|Task<int> F(IReactor* reactor, Held const* held)~n~{~n~    while (held->Is(Order::Acquire))~n~    {~n~        co_await SleepFor(*reactor, std::chrono::milliseconds { 1 })~sc~~n~        Note()~sc~~n~    }~n~    co_return 0~sc~~n~}~n~|src/FastCache/Net/Park_test.cpp:3: a coroutine polling on its reactor|-|-"
    # Both rules match a coroutine parking until an atomic flips (#1453's CancelRead and FrameEndpoint
    # shape), and it is ONE site: counted twice, the refusal would overstate what is wrong.
    "a coroutine parking until an atomic flips is one site|newfile|src/FastCache/Net/Park_test.cpp|DetachedTask F(IReactor* reactor, std::atomic<bool> const* armed)~n~{~n~    while (!armed->load(std::memory_order_acquire))~n~        co_await SleepUntil { .reactor = reactor, .deadline = reactor->Clock().Now() + Step }~sc~~n~}~n~|src/FastCache/Net/Park_test.cpp:3: a coroutine polling on its reactor && test-loops: 1 unbacklogged site(s)|a while loop polling an atomic|-"

    # A violation BEHIND a comment: prose above it and a trailing comment on its own line
    # must not hide it, and the line number must survive list structure planted above it.
    "a loop below prose and brackets|newfile|src/FastCache/Core/Drift_test.cpp|// for (int i = 0~sc~ i < n~sc~ ++i) is what this file must not do~n~int a~lb~4~rb~ = { 1, 2, 3, 4 }~sc~~n~// a trailing backslash ~bs~~n~void F() { for (int i = 0~sc~ i < 4~sc~ ++i) Use(a~lb~i~rb~)~sc~ } // and a trailing comment~n~|src/FastCache/Core/Drift_test.cpp:4: a C-style for loop|Drift_test.cpp:1|-"

    # THE QUIET ARM.
    "a commented-out loop|newfile|src/FastCache/Core/Note_test.cpp|// for (int i = 0~sc~ i < 3~sc~ ++i)~n~/* for (int j = 0~sc~ j < 3~sc~ ++j)~n~   while (!go.load()) */~n~int Note() { return 0~sc~ }~n~|no new C-style loop|CMake Error|-"
    "a range-for whose range holds a lambda with semicolons|newfile|src/FastCache/Core/Range_test.cpp|int F(std::vector<int> const& v)~n~{~n~    int n = 0~sc~~n~    for (auto const x: v ~bar~ std::views::transform(~lb~~rb~(int y) { int z = y~sc~ return z~sc~ }))~n~        n += x~sc~~n~    return n~sc~~n~}~n~|no new C-style loop|CMake Error|-"
    "a range-for followed by two statements|newfile|src/FastCache/Core/Range_test.cpp|void F(std::vector<int> const& v)~n~{~n~    for (auto const x: v)~n~        Use(x)~sc~~n~    Next()~sc~~n~}~n~|no new C-style loop|CMake Error|-"
    # A C++20 range-for with an init-statement holds ONE `;`. master's RaftWire and Nonce tests
    # carry three, and the first version of this check -- one `;` was enough -- refused them.
    "a range-for with an init-statement|newfile|src/FastCache/Core/Init_test.cpp|void F(std::span<std::byte> tag)~n~{~n~    for (std::size_t index = 0~sc~ auto& byte: tag)~n~        Use(byte, index++)~sc~~n~    Next()~sc~~n~}~n~|no new C-style loop|CMake Error|-"
    "a range-for whose init-statement breaks the line|newfile|src/FastCache/Core/Init_test.cpp|void F(std::span<std::byte const> nonce)~n~{~n~    for (std::size_t index = 0~sc~~n~         auto const byte: nonce)~n~        Use(byte, index++)~sc~~n~}~n~|no new C-style loop|CMake Error|-"
    # A lambda's body is one element of the header, so its `;` are its own: the first version
    # stopped reading at a `{` and passed a counting loop that held a lambda.
    "a range-for whose init-statement holds a lambda|newfile|src/FastCache/Core/Init_test.cpp|void F(std::vector<int> const& values)~n~{~n~    for (auto f = ~lb~~rb~(int x) { Note(x)~sc~ return x~sc~ }~sc~ auto v: values)~n~        Use(f(v))~sc~~n~}~n~|no new C-style loop|CMake Error|-"
    "a counting loop whose init holds a lambda|newfile|src/FastCache/Core/Fresh_test.cpp|void F()~n~{~n~    for (auto f = ~lb~~rb~ { return 0~sc~ }~sc~ f() < 3~sc~)~n~        Use()~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-|-"
    "a counting loop whose condition calls a lambda|newfile|src/FastCache/Core/Fresh_test.cpp|void F(int n)~n~{~n~    for (int i = 0~sc~ i < Pick(n, ~lb~~rb~ { return 1~sc~ })~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|src/FastCache/Core/Fresh_test.cpp:3: a C-style for loop|-|-"
    "a while that polls no atomic|newfile|src/FastCache/Core/While_test.cpp|void F(std::istream& in)~n~{~n~    std::string line~sc~~n~    while (std::getline(in, line))~n~        Use(line)~sc~~n~}~n~|no new C-style loop|CMake Error|-"
    "a coroutine that works before it parks|newfile|src/FastCache/Net/Beat_test.cpp|DetachedTask Beat(IReactor* reactor, Beats* beats)~n~{~n~    while (!beats->stopping)~n~    {~n~        auto const deadline = reactor->Clock().Now() + Step~sc~~n~        co_await SleepUntil { .reactor = reactor, .deadline = deadline }~sc~~n~    }~n~}~n~|no new C-style loop|CMake Error|-"
    "a coroutine reading until its peer is done|newfile|src/FastCache/Net/Read_test.cpp|Task<int> Drain(ISocket* socket, std::span<std::byte> buffer)~n~{~n~    auto eof = false~sc~~n~    while (!eof)~n~        eof = (co_await socket->Read(buffer)).value_or(0) == 0~sc~~n~    co_return 0~sc~~n~}~n~|no new C-style loop|CMake Error|-"
    "a coroutine parking a fixed number of turns|newfile|src/FastCache/Net/Turns_test.cpp|DetachedTask F(IReactor* loop, int turns)~n~{~n~    for (auto const turn: std::views::iota(0, turns))~n~        co_await ResumeOn { *loop }~sc~~n~}~n~|no new C-style loop|CMake Error|-"
    "a coroutine awaiting a name that only begins like a park|newfile|src/FastCache/Net/Name_test.cpp|DetachedTask F(IReactor* loop, bool const* done)~n~{~n~    while (!*done)~n~        co_await ResumeOnce(loop)~sc~~n~}~n~|no new C-style loop|CMake Error|-"
    "a function merely named for|newfile|src/FastCache/Core/Name_test.cpp|int transform_for(int a)~sc~~n~int platform(int b)~sc~~n~int x = transform_for(1)~sc~~n~|no new C-style loop|CMake Error|-"

    # EXEMPTIONS, both directions.
    "the exempted text in another file is not exempt|newfile|src/FastCache/Core/Copy_test.cpp|void Beat(std::atomic<bool>* stopping)~n~{~n~    while (!stopping->load(std::memory_order_acquire))~n~        Sleep()~sc~~n~}~n~|src/FastCache/Core/Copy_test.cpp:3: a while loop polling an atomic|-|-"
    "a second site in an exempted file needs its own row|file3|Sleep()~sc~ }|Sleep()~sc~ }~n~void Other(std::atomic<bool>& ready) { while (!ready.load()) Sleep()~sc~ }|src/FastCache/Protocol/LiveStreamReactors_test.cpp:2: a while loop polling an atomic|-|-"
    "an exemption whose site is gone is STALE|file3|while (!stopping->load(std::memory_order_acquire)) Sleep()~sc~|BeatUntilStopped()~sc~|matching no site && stopping->load( && STALE|-|-"

    # --- fails CLOSED ---
    "a scope row that matches nothing|noheader|-|-|matched no file && src/tests/*.hpp|-|-"
    "no for anywhere|nofor|-|-|the scan matched nothing and cannot conclude|-|-"
    # ---- #1452: the `loop` rule reaches production sources, and a ratchet is what lets it.
    #
    # The scope rows for `src/*.cpp` and `src/*.hpp` are what these exercise. Before #1452 a
    # production source was in no scope row at all, so every one of these passed vacuously.
    "a production loop with no backlog row|newfile|src/FastCache/Core/Prod.cpp|void F()~n~{~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|src/FastCache/Core/Prod.cpp:3: a C-style for loop && A site nobody has decided about yet belongs in FastCachedTestLoopBacklog|-|-"
    # The ACCEPTING direction. A ratchet that refused its own rows would look like a working
    # one on any tree that still has sites, which is every tree until #1452 closes.
    "a production loop its backlog row records|newfile|src/FastCache/Core/Prod.cpp|void F()~n~{~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|no new C-style loop && backlog: 1 site(s) across 1 file(s) are waiting on #1452|CMake Error|loop~bar~src/FastCache/Core/Prod.cpp~bar~1~bar~#1452"
    # One MORE than the row records: a new site arriving in a file that already has some, which
    # is the likeliest way one arrives at all.
    "one more loop than the backlog records|newfile|src/FastCache/Core/Prod.cpp|void F()~n~{~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|2 loop site(s) here, and FastCachedTestLoopBacklog records 1 for #1452 && this is a NEW site|-|loop~bar~src/FastCache/Core/Prod.cpp~bar~1~bar~#1452"
    # One FEWER: a conversion that did not come off the backlog. The non-obvious direction, and
    # without it the number a fixed site left behind is room for the next arrival.
    "one fewer loop than the backlog records|newfile|src/FastCache/Core/Prod.cpp|void F()~n~{~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~}~n~|the row records 2, the scan finds 1 && the ratchet, and it only turns one way|-|loop~bar~src/FastCache/Core/Prod.cpp~bar~2~bar~#1452"
    "a backlog row whose file is not there|none|-|-|the row records 1, the scan finds none|-|loop~bar~src/FastCache/Core/Absent.cpp~bar~1~bar~#1452"

    # ---- The per-rule scope: `spin` and `poll` are TEST-only, because the wait they ask for is
    # `src/tests/BoundedWait.hpp` and a production source cannot include it. Both of these are
    # refused when they sit in a test file (cases above), and must NOT be here.
    "an atomic polled in a production source|newfile|src/FastCache/Core/Prod.cpp|void F(std::atomic<bool> const& go)~n~{~n~    while (!go.load(std::memory_order_acquire))~n~        std::this_thread::yield()~sc~~n~}~n~|no new C-style loop|CMake Error && a while loop polling an atomic|-"
    "a coroutine parking in a production source|newfile|src/FastCache/Core/Prod.cpp|DetachedTask F(IReactor* loop, bool const* done)~n~{~n~    while (!*done)~n~        co_await FastCache::ResumeOn { *loop }~sc~~n~}~n~|no new C-style loop|CMake Error && a coroutine polling on its reactor|-"
    # The must-NOT-catch half, now asked of a production source: the scope went from ~90 files
    # to 915 and a rule that had only ever read tests now reads every header in the tree.
    "a range-for in a production source|newfile|src/FastCache/Core/Prod.cpp|void F(std::span<int const> values)~n~{~n~    for (auto const value: values)~n~        Use(value)~sc~~n~}~n~|no new C-style loop|CMake Error && a C-style for loop|-"

    # ---- The backlog FILE's own shape. Each refused by NAME: a row read as a count of zero
    # waves through every site in its file, and that is the direction nobody notices.
    "a backlog row with three fields|none|-|-|Backlog row has 3 field(s), not 4|-|loop~bar~src/FastCache/Core/Prod.cpp~bar~1"
    "a backlog row with a count of zero|none|-|-|so zero is spelled by deleting the row|-|loop~bar~src/FastCache/Core/Prod.cpp~bar~0~bar~#1452"
    "a backlog row naming no issue|none|-|-|rather than an issue like|-|loop~bar~src/FastCache/Core/Prod.cpp~bar~1~bar~1452"
    "a backlog row naming an unknown rule|none|-|-|which is not loop, spin or poll|-|lint~bar~src/FastCache/Core/Prod.cpp~bar~1~bar~#1452"
    # Two rows for one file: refused anyway, but the stale sweep would have blamed a conversion
    # that never happened, and the count governing the file would be whichever row came first.
    "two backlog rows for one file|none|-|-|has two rows for loop in src/FastCache/Core/Prod.cpp|-|loop~bar~src/FastCache/Core/Prod.cpp~bar~1~bar~#1452~n~loop~bar~src/FastCache/Core/Prod.cpp~bar~2~bar~#1452"
    # The tally is the whole reason an undecided row is allowed, so it must print on a run that
    # REFUSES too -- below the refusal it printed on exactly the runs nobody needs it on.
    "the backlog tally prints on a refusing run|newfile|src/FastCache/Core/Prod.cpp|void F()~n~{~n~    for (int i = 0~sc~ i < 3~sc~ ++i)~n~        Use(i)~sc~~n~    for (int j = 0~sc~ j < 3~sc~ ++j)~n~        Use(j)~sc~~n~}~n~|2 loop site(s) here && backlog: 1 site(s) across 1 file(s) are waiting on #1452|-|loop~bar~src/FastCache/Core/Prod.cpp~bar~1~bar~#1452"
    # Absent is not empty. Read as empty it refuses every un-converted site in the tree, loudly,
    # and sends whoever meets it to the loops rather than to the missing file.
    "a missing backlog file|none|-|-|the backlog file cannot be read|-|absent"
)

# ---------------------------------------------------------------------------
set(failures "")
set(caseCount 0)

foreach(row IN LISTS FastCachedTestLoopCases)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 7)
        message(FATAL_ERROR
            "case row has ${fieldCount} fields, expected 7 -- a row that does not parse would run as a "
            "different case than it reads as: [${row}]")
    endif()
    list(GET fields 0 caseName)
    list(GET fields 1 caseTarget)
    list(GET fields 2 caseFrom)
    list(GET fields 3 caseTo)
    list(GET fields 4 caseMustAppear)
    list(GET fields 5 caseMustNotAppear)
    list(GET fields 6 caseBacklog)

    math(EXPR caseCount "${caseCount} + 1")
    fastcached_stage_and_run("${caseCount}" "${caseTarget}" "${caseFrom}" "${caseTo}"
                             "${caseBacklog}" output applied)

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

    # WHICH backlog the check read, asserted on every case that got far enough to read one. The
    # path is overridable so this file can stage one, and a case that silently read the SHIPPED
    # 64-row table would refuse for a reason that is not what it is about -- while looking, in
    # the rows that expect a refusal, exactly like a case that worked.
    if(NOT caseBacklog STREQUAL "absent")
        string(FIND "${output}" "test-loops: backlog file: backlog.txt" position)
        if(position EQUAL -1)
            list(APPEND failures
                 "${caseName}: the check did not say it read the STAGED backlog, so this case may have been judged against the shipped one")
        endif()
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
