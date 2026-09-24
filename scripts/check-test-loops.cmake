# SPDX-License-Identifier: Apache-2.0
#
# The tests' loops: no C-style `for`, no `while` that polls an atomic, and no coroutine
# `while` that does nothing but park on its reactor until something holds.
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
# The third is the same wait run by a COROUTINE, which cannot block its own reactor thread in
# `WaitUntil` and so parked a turn or a millisecond at a time with no bound at all: four tests
# did (#1453), two of them on a plain `bool` the atomic rule cannot see. Their wait is
# `AwaitUntil`, bounded on the reactor's own clock.
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
#   * a `while (` whose condition calls `.load(` or `->load(`;
#   * a `while (` whose body STARTS with `co_await` of `SleepFor`, `SleepUntil` or `ResumeOn`,
#     or of core-cpp's spellings of the same parks since #1596 -- `core::net::sleepUntil` and a
#     loop's `delay` member, reached through `::`, `->` or `.` -- braced or not, whatever its
#     condition reads. A site both rules match is one site, reported by this rule.
#
# ## What it does NOT cover, said here so nobody over-applies it
#
#   * A `while` that is not polling an atomic -- stepping through `find()` or `getline()`, or
#     reading until a peer is done -- is ordinary and stays. So is a coroutine `while` that
#     WORKS before it parks, as a heartbeat beats and then sleeps, and a `for` that parks a
#     fixed number of turns: neither is waiting for something to hold.
#   * `vendor/` is not scanned: it is another project's code, changed upstream.
#   * `spin` and `poll` do not reach non-test sources; only `loop` does (#1452), per the scope
#     table below.
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
#   * The coroutine rule FAILS OPEN on a wait spelled any other way: a park that is not the
#     body's first statement (`while (!done) { ++turns; co_await ResumeOn { *loop }; }`), a
#     park through a coroutine of another name, a `do ... while`, a coroutine that awaits
#     ITSELF until the condition holds, and a condition nested three parentheses deep or
#     holding a lambda. It reads the one shape #1453's census found, all four times; a wait
#     spelled another way is left to review rather than guessed at by a wider pattern.
#
# ## Exemptions
#
# An exemption's `text` is matched as a SUBSTRING of the loop header **as this check extracts
# it**, and that header STOPS AT THE SECOND `;` -- `loopPattern` is anchored to end there. So for
#
#     for (; it != end; it.increment(ec))
#
# the string being searched is `for (; it != end;` and the INCREMENT CLAUSE IS NOT IN IT. Naming
# `it.increment(ec)` -- by far the most identifying fragment, and the obvious thing to write --
# makes the row match nothing, and the row is then refused as stale with a message saying the
# site was converted or deleted. It was not; it is exactly where it was. Name something from the
# init or the condition instead.
#
# A text may also not contain `;`, for a different reason and with a different symptom: a
# semicolon is what CMake splits a list on, so it breaks the row into pieces when this table is
# DEFINED, and the arity check below reports a field count rather than a character.
#
# Both constraints have the same cause -- the header's two semicolons -- and a `loop` exemption is
# the one kind of row where that is the natural thing to write.
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
# What is scanned, and by WHICH rule: one row per kind of file, as a git pathspec under the
# source root. A file matching several rows is in scope for the UNION of their rules, which is
# how a `*_test.cpp` is also a `*.cpp`.
#
# The rules column is not a refinement of the scope, it is the scope. Widening all three rules
# to `src/` fired `spin` 16 times and `poll` 5 times, every one in a PRODUCTION file: reactor
# shutdown checks, `FrameEndpoint`'s drain, the bench's worker loops. Their remedy is
# `src/tests/BoundedWait.hpp` -- a header a production source cannot include -- so a widened
# `spin` refuses correct code and then tells the author to do something impossible. A guard's
# REMEDY TEXT is part of the guard, and the two rules' rationale in `.agent/rules/testing.md`
# is about waits in TESTS. So `loop` widens to all of `src/` (#1452) and they do not.
#
# pathspec|rules|what it is
set(FastCachedTestLoopScope
    "src/*_test.cpp|loop spin poll|every Catch2 test source (#1446)"
    "src/tests/*.cpp|loop spin poll|the shared test programs and canaries (#1446)"
    "src/tests/*.hpp|loop spin poll|the shared test helpers (#1446)"
    "src/*.cpp|loop|every other C++ source (#1452)"
    "src/*.hpp|loop|every other C++ header (#1452)"
)

# ---------------------------------------------------------------------------
# Sites nobody has decided about yet, and the issue that will decide them: the RATCHET, whose
# rows live in `scripts/check-test-loops-backlog.txt` and whose reasoning lives there with them.
#
# What matters here: a backlog row is a different CLAIM from an exemption row below. An exemption
# says *this site is right and must stay, here is why*. A backlog row says *nobody has decided
# about this yet*, and it is safe only because this check tallies them and prints the total per
# issue on every run -- the argument `RefuseUntriaged` rests on in
# `.agent/rules/metrics-and-observability.md`.

# ---------------------------------------------------------------------------
# Sites that stay, and why.
#
# rule|file|text the matched header contains|reason
#
# `rule` is `loop`, `spin` or `poll`. The text is matched against the header as the scan read
# it, from its keyword to its second `;` (a loop), its `load(` (a spin) or the name of the
# park that opens its body and the character after it (a poll).
#
# **For a loop that means the INIT and CONDITION clauses only -- the increment is not in the
# header and cannot be named here.** Which is a trap rather than a detail, because for the
# shapes most likely to want a row the increment IS the mechanism: a find-walk resumes with
# `at = text.find(tok, at + n)` and a work-stealing loop steps with `index = next.fetch_add(1)`.
# Name the init instead. A row pointing at an increment is reported STALE -- correctly, and with
# a message about matching no site, which reads as the site having been converted. None of the four
# fields may contain `|` or `;`, and the text may not contain `[` or `]`: these rows are a
# CMake list. Checked below rather than left as a comment nothing reads.
set(FastCachedTestLoopExemptions
    "loop|src/tests/TsanCanary.cpp|for (|The ThreadSanitizer canary's loops are its race: a change to this file is judged by a RATE over a few hundred runs (scripts/tsan-canary-rate.sh), never by a green run, and rewriting three loops would re-open that measurement for a spelling."
    "spin|src/FastCache/Protocol/LiveStreamReactors_test.cpp|stopping->load(|Not a wait: a heartbeat coroutine that beats until the case says stop, and whose frame the rig's reactors free when a case ends early."
    "loop|src/apps/fastcache-cc/ToolchainProbe.cpp|it != end|A recursive_directory_iterator advanced by the error-code overload. A range-based for calls ++it, which THROWS on an unreadable directory instead of setting ec -- and surviving that to report a PARTIAL walk is this loop's whole purpose, so converting it would be a behaviour regression rather than a modernisation."
    "loop|src/apps/fastcache-cc/ProcessRunner.cpp|char const* cursor = inherited|GetEnvironmentStringsA returns a double-NUL-terminated block of NUL-separated strings with no size anywhere: the terminator IS the bound, so no range can be formed without first walking the block to find its end."
    "loop|src/apps/fastcache-cc/ProcessRunner.cpp|char** entry = inherited|The POSIX environment array is a NULL-terminated char** with no size. Same reason as the Windows block above, and C++23 has no standard adaptor over a sentinel-terminated C array."
    "loop|src/apps/fastcache-cli/CliCommand.cpp|index < args.size()|The argv walk, and the one option-parser loop of the three #1452 found that does NOT convert. `ApplyOneOption` advances `index` to consume a value, and FIVE `continue` paths -- end-of-options, a refused option while settled, a Stop flow, an operand after the action settled, and a word alias -- advance nothing in the body, so every one of them depends on the head. A `while` therefore needs five duplicated `++index` statements, which is the copy-pasted increment this project forbids elsewhere, or an RAII scope-exit whose only purpose is to satisfy a loop-shape rule. Both are worse than the `for`. Its two clean siblings DID convert in the same change: Cli/Options.hpp ParseOptionsInto has no `continue` at all, and Config/FileOptions.hpp ClearListsNamedOn had one that inverted into an `if`."
    "loop|src/apps/fastcache-cli/SocketExchange.cpp|for (|Five deliberate infinite loops reading framed replies off a socket. A for whose condition clause is empty names no sequence, so there is no range to form -- each one ends on a break or a return decided from bytes that have arrived, which is a fact about the wire rather than an index. Spelling them while (true) would satisfy no guideline, since the rule exists to replace hand-rolled index arithmetic and there is none here. The text is the bare keyword because it has to be: the header such a loop presents to this scan is the keyword and two empty clauses, and no field of this table may contain the character those clauses are separated by, so no narrower fragment can be written. That makes this row FILE-WIDE for C-style loops, and a real counting loop added to this file would be silently covered by it -- the same residual the TsanCanary.cpp row carries."
    "loop|src/apps/fastcache-compile-node/NodeAnnounce.cpp|for (link.BeginRound()|The scheduler dial walk. Its init resets the per-round redirect budget and its condition is absent, so what ends it is a break inside -- an accepted round, an exhausted endpoint list, or the redirect bound that stops two nodes with stale leader memories naming each other forever. The sequence it walks is the endpoint SchedulerLink hands out next, which depends on what the last scheduler answered, so it is not expressible as a view."
    "loop|src/apps/fastcache-cc/ParallelFor.cpp|index = next.fetch_add(1, std::memory_order_relaxed)|A work-stealing queue over a shared atomic, run by every worker thread. The indices ONE thread sees are whichever the others did not claim, so the sequence is not a property of this loop at all and no view can describe it. Converting it would mean giving each thread a fixed slice, which is a different scheduling decision -- and the wrong one here, since the point is that a slow slice does not hold up the others."
    "loop|src/apps/fastcache-compile-node/NodeToolchains.cpp|index = next.fetch_add(1)|The toolchain survey's work-stealing walk, and the same fact as ParallelFor.cpp above: the indices this thread sees are the ones no sibling claimed first. A fingerprint probe spawns a compiler, so the durations are wildly uneven and a fixed slice per thread is measurably worse -- which is why the shape is what it is rather than an oversight."
    "loop|src/FastCache/Cli/UsageDoc.cpp|auto at = out.find(token)|A find-and-replace walk, and it is not merely a scan: the body calls out.replace, so each search runs over a string the previous iteration rewrote. The resume offset is at + value.size() rather than at + token.size() for exactly that reason -- it steps past the REPLACEMENT, so a token whose value contains it does not expand forever. No range over the original text can express a walk whose subject changes underneath it."
    "loop|src/apps/fastcache-cli/DashboardPanel.cpp|at = note.find(DeltaMark)|The same shape as UsageDoc.cpp above, with the mutation on the other side: the body calls note.remove_prefix, so the view being searched shrinks each iteration and the unqualified find always looks from the start of what is left. A range over the original note would walk bytes this loop has already consumed."
    "loop|src/FastCache/Platform/LocalAddresses.cpp|auto const* adapter = head|GetAdaptersAddresses answers with a C linked list of adapters, each holding a C linked list of unicast addresses: no size and no iterator at either level, so each head's Next step IS the sequence and nullptr its end. No standard view walks a nullptr-terminated list -- the gap the ProcessRunner.cpp rows record -- and a `while` would move a constant step from the head to a foot that the next `continue` anybody writes skips."
    "loop|src/FastCache/Platform/LocalAddresses.cpp|auto const* unicast = adapter->FirstUnicastAddress|The inner half of the adapter walk the row above describes: one adapter's unicast addresses, the same nullptr-terminated Next list, exempt for the same reason."
    "loop|src/FastCache/Platform/LocalAddresses.cpp|auto const* entry = head|getifaddrs answers with a C linked list threaded through ifa_next -- the POSIX twin of the adapter walk above, and exempt for the same reason: the head's step is the sequence, and no standard view walks a nullptr-terminated list."
    "loop|src/apps/fastcache-compile-node/ScratchClaim.cpp|auto iterator = std::filesystem::directory_iterator|A directory_iterator advanced by the error-code overload, as ToolchainProbe.cpp's is and for the same reason: a range-based for calls operator++, which THROWS where increment(ec) sets ec, and emptying a claimed scratch root is exactly where an entry vanishing under the walk must stop it quietly rather than end the node."
    "loop|src/apps/fastcached/main.cpp|!ec && entry != end|The same error-code directory_iterator walk, over the shard files a migration converts, and the comment above it says why: a range-based for advances through the throwing operator++, so an entry vanishing mid-scan or an unreadable subdirectory would terminate the process instead of reporting that the directory cannot be listed."
)

# One element of a loop header's clause: an ordinary character, a brace group (a lambda body,
# whose `;` are its own and not the header's), or a parenthesised group -- braces nest one
# level, parentheses two.
set(braceGroup "\\{[^{}]*\\}")
set(innerParens "\\(([^;(){}]|${braceGroup})*\\)")
set(headerElement "([^;(){}]|${braceGroup}|\\(([^;(){}]|${innerParens}|${braceGroup})*\\))")
set(loopPattern "(^|[^A-Za-z0-9_])for[ \t\r\n]*\\(${headerElement}*;${headerElement}*;")
set(whileCondition "([^;(){}]|\\(([^;(){}]|\\([^;(){}]*\\))*\\))*")
set(spinPattern "(^|[^A-Za-z0-9_])while[ \t\r\n]*\\(${whileCondition}(\\.|->)load[ \t\r\n]*\\(")
# The condition, its closing `)`, an optional `{`, then the park -- qualified or not -- and one
# character that ends its name, so `ResumeOnce` is not `ResumeOn`.
# The same three patterns ANCHORED at the start, used to decide WHERE a match is.
# `string(REGEX MATCH)` does not report an offset, and locating the match by searching for
# its own TEXT is wrong whenever that text occurs earlier in the file -- for a `for (;;)` the
# extracted header is ` for (`, six characters, so the search lands on the file's first
# `for (`, which in this tree is very often a RANGE-FOR. Measured: `SocketExchange.cpp`
# reported its range-fors at lines 39 and 51 as C-style loops. The pattern itself never
# matches a range-for, so this was attribution alone -- and a report naming a line that
# holds a range-for is a confidently wrong signal, which is worse than a miscount.
#
# Anchored, without the leading boundary character, which the walk strips from the header.
set(loopPatternAnchored "^for[ \t\r\n]*\\(${headerElement}*;${headerElement}*;")
set(spinPatternAnchored "^while[ \t\r\n]*\\(${whileCondition}(\\.|->)load[ \t\r\n]*\\(")
set(pollPattern
    "(^|[^A-Za-z0-9_])while[ \t\r\n]*\\(${whileCondition}\\)[ \t\r\n]*\\{?[ \t\r\n]*co_await[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*(::|->|\\.)[ \t\r\n]*)*(SleepFor|SleepUntil|ResumeOn|sleepUntil|delay)[^A-Za-z0-9_]")
set(pollPatternAnchored
    "^while[ \t\r\n]*\\(${whileCondition}\\)[ \t\r\n]*\\{?[ \t\r\n]*co_await[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*(::|->|\\.)[ \t\r\n]*)*(SleepFor|SleepUntil|ResumeOn|sleepUntil|delay)[^A-Za-z0-9_]")

# ---------------------------------------------------------------------------
# Exemption rows, checked for shape before anything is decided from them.
set(exemptionIndex 0)
foreach(row IN LISTS FastCachedTestLoopExemptions)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 4)
        message(FATAL_ERROR
            "test-loops: exemption row has ${fieldCount} field(s), expected 4 "
            "(rule|file|text|reason), so it states no reason or cannot be read:\n  ${row}\n"
            "  If the row above looks CUT OFF, it contained a `;`. That is the character "
            "CMake splits a list on, so the row was broken into pieces when this table was "
            "DEFINED -- before any check here could see it whole, which is why this "
            "complains about a count rather than about the character. It is the likeliest "
            "mistake in a `loop` row: the text names a loop HEADER, and a header's defining "
            "feature is two semicolons. The text is matched as a SUBSTRING, so name a "
            "fragment instead -- `it.increment(ec)`, never the whole header.")
    endif()
    list(GET fields 0 exemptRule)
    list(GET fields 1 exemptFile)
    list(GET fields 2 exemptText)
    list(GET fields 3 exemptReason)
    if(NOT exemptRule MATCHES "^(loop|spin|poll)$")
        message(FATAL_ERROR "test-loops: exemption rule `${exemptRule}` is not `loop`, `spin` or `poll`:\n  ${row}")
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

# ---------------------------------------------------------------------------
# The path is overridable so `check-test-loops-selftest.cmake` can stage a backlog beside its
# synthetic tree. That is a seam a self-test takes and CI does not, so the resolved path is part
# of this check's OUTPUT on every run -- a guard whose self-test exercised one mode while CI
# exercised another has already shipped in this tree and passed.
if(NOT DEFINED FASTCACHED_TEST_LOOPS_BACKLOG)
    set(FASTCACHED_TEST_LOOPS_BACKLOG "${CMAKE_CURRENT_LIST_DIR}/check-test-loops-backlog.txt")
endif()
if(NOT EXISTS "${FASTCACHED_TEST_LOOPS_BACKLOG}")
    message("")
    message("  The backlog file is not there: ${FASTCACHED_TEST_LOOPS_BACKLOG}")
    message("")
    message("Refused by name rather than read as an empty backlog. Empty would refuse every")
    message("un-converted site in the tree -- loudly, and with a diagnosis that sends whoever")
    message("meets it to the loops rather than to the missing file.")
    message(FATAL_ERROR "test-loops: the backlog file cannot be read")
endif()
file(STRINGS "${FASTCACHED_TEST_LOOPS_BACKLOG}" backlogLines)
set(FastCachedTestLoopBacklog "")
foreach(backlogLine IN LISTS backlogLines)
    string(STRIP "${backlogLine}" backlogLine)
    if(backlogLine STREQUAL "" OR backlogLine MATCHES "^#")
        continue()
    endif()
    list(APPEND FastCachedTestLoopBacklog "${backlogLine}")
endforeach()
get_filename_component(backlogShown "${FASTCACHED_TEST_LOOPS_BACKLOG}" NAME)
# WHICH backlog, on every run and before any row is judged: the path is overridable so
# the self-test can stage one, and a run judged against the wrong table would otherwise
# look exactly like a run judged against the right one.
message(STATUS "test-loops: backlog file: ${backlogShown}")

# ---------------------------------------------------------------------------
# Backlog rows, checked for shape before anything is decided from them. A malformed row would
# otherwise read as a count of zero, which is the direction that waves a new site through.
set(backlogIndex 0)
set(backlogIssues "")
set(backlogSeen "")
foreach(row IN LISTS FastCachedTestLoopBacklog)
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 4)
        message("")
        message("  FastCachedTestLoopBacklog row has ${fieldCount} field(s), not 4:")
        message("    ${row}")
        message("")
        message("The shape is rule|file|count|issue. A row this reader cannot split would be read")
        message("as a count of zero, which waves through every site in that file.")
        message(FATAL_ERROR "test-loops: a backlog row is malformed")
    endif()
    list(GET fields 0 backlogRule)
    list(GET fields 1 backlogFile)
    list(GET fields 2 backlogCount)
    list(GET fields 3 backlogIssue)
    if(NOT backlogRule MATCHES "^(loop|spin|poll)$")
        message(FATAL_ERROR
            "test-loops: backlog row names rule `${backlogRule}`, which is not loop, spin or poll:\n  ${row}")
    endif()
    if(NOT backlogCount MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR
            "test-loops: backlog row for ${backlogFile} has count `${backlogCount}`. A row exists to "
            "record sites that ARE there, so zero is spelled by deleting the row:\n  ${row}")
    endif()
    if(NOT backlogIssue MATCHES "^#[1-9][0-9]*$")
        message(FATAL_ERROR
            "test-loops: backlog row for ${backlogFile} names `${backlogIssue}` rather than an issue "
            "like `#1452`. A row whose issue nobody can open is an exemption wearing the wrong "
            "word:\n  ${row}")
    endif()
    # One row per (rule, file). Two would be refused anyway -- the scan matches the first and
    # the stale sweep reports the second as a site that was converted or deleted, which is false
    # in every clause and sends whoever reads it looking for a conversion that never happened.
    if("${backlogRule}/${backlogFile}" IN_LIST backlogSeen)
        message("")
        message("  FastCachedTestLoopBacklog has two rows for ${backlogRule} in ${backlogFile}:")
        message("    ${row}")
        message("")
        message("One row per rule per file. With two, the scan matches the first and the second")
        message("reads as a site that was converted or deleted -- so the count that governs the")
        message("file is whichever row comes first, which is not a thing anybody chose.")
        message(FATAL_ERROR "test-loops: a backlog row is duplicated")
    endif()
    list(APPEND backlogSeen "${backlogRule}/${backlogFile}")
    set(backlog_${backlogIndex}_rule "${backlogRule}")
    set(backlog_${backlogIndex}_file "${backlogFile}")
    set(backlog_${backlogIndex}_count "${backlogCount}")
    set(backlog_${backlogIndex}_issue "${backlogIssue}")
    set(backlog_${backlogIndex}_used 0)
    list(APPEND backlogIssues "${backlogIssue}")
    math(EXPR backlogIndex "${backlogIndex} + 1")
endforeach()
set(backlogCount ${backlogIndex})
list(REMOVE_DUPLICATES backlogIssues)

set(sourceFiles "")
set(ruleFiles_loop "")
set(ruleFiles_spin "")
set(ruleFiles_poll "")
foreach(scopeRow IN LISTS FastCachedTestLoopScope)
    string(REPLACE "|" ";" scopeFields "${scopeRow}")
    list(LENGTH scopeFields scopeFieldCount)
    if(scopeFieldCount LESS 3)
        message(FATAL_ERROR
            "test-loops: scope row has ${scopeFieldCount} field(s), not 3. The shape is "
            "pathspec|rules|what it is, and a row missing its rules column says nothing about "
            "which rule it puts those files in scope for:\n  ${scopeRow}")
    endif()
    list(GET scopeFields 0 pathspec)
    list(GET scopeFields 1 scopeRules)
    string(REPLACE " " ";" scopeRules "${scopeRules}")
    if(NOT scopeRules)
        message(FATAL_ERROR
            "test-loops: scope row names no rule, so its files are enumerated and then asked "
            "nothing:\n  ${scopeRow}")
    endif()
    foreach(named IN LISTS scopeRules)
        if(NOT named MATCHES "^(loop|spin|poll)$")
            message(FATAL_ERROR
                "test-loops: scope row names rule `${named}`, which is not loop, spin or "
                "poll:\n  ${scopeRow}")
        endif()
    endforeach()
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
    foreach(named IN LISTS scopeRules)
        list(APPEND ruleFiles_${named} ${rowFiles})
    endforeach()
endforeach()
list(REMOVE_DUPLICATES sourceFiles)
list(SORT sourceFiles)
list(LENGTH sourceFiles fileCount)

# Every rule must be in scope somewhere. A rule no row names is enumerated by nothing and
# refuses nothing, which reads exactly like a rule that found no violations.
foreach(named loop spin poll)
    if(ruleFiles_${named})
        list(REMOVE_DUPLICATES ruleFiles_${named})
    else()
        message("")
        message("  No scope row names the `${named}` rule, so it is asked about no file at all.")
        message("")
        message("A rule nothing enumerates refuses nothing, and a green run then says the tree is")
        message("clean of something nobody looked for.")
        message(FATAL_ERROR "test-loops: the ${named} rule is in scope nowhere")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# The scan.
set(violations "")
set(violationCount 0)
set(loopsSeen 0)
set(exemptedCount 0)
set(staleBacklog "")

foreach(sourceFile IN LISTS sourceFiles)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${sourceFile}")
        continue()
    endif()
    file(READ "${FASTCACHED_SOURCE_DIR}/${sourceFile}" wholeFile)
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

    # `poll` before `spin`, and each `while` it decides is not decided again: a coroutine
    # parking until an atomic flips matches both, and is one site to convert, not two.
    set(pollSites "")
    foreach(rule loop poll spin)
        # This file may not be in scope for this rule -- `spin` and `poll` are test-only.
        if(NOT sourceFile IN_LIST ruleFiles_${rule})
            continue()
        endif()
        set(ruleSites 0)
        set(ruleSiteText "")
        set(rest "${code}")
        set(consumed 0)
        while(TRUE)
            string(REGEX MATCH "${${rule}Pattern}" header "${rest}")
            if(header STREQUAL "")
                break()
            endif()
            # The leading character the pattern needs to see a keyword boundary is not part
            # of the header, and a newline there would put the site one line early.
            if(header MATCHES "^[^fw]")
                string(SUBSTRING "${header}" 1 -1 header)
            endif()
            # WHERE the match is. `string(REGEX MATCH)` reports no offset, and the first place
            # the header's TEXT occurs is at or BEFORE the place the pattern matched -- equal
            # only when that text is distinctive. It is not for a `for (;;)`, whose header is
            # ` for (`: the search then lands on the file's first `for (`, very often a
            # RANGE-FOR. So a candidate is accepted only when the pattern matches ANCHORED
            # there, and otherwise the next occurrence is tried.
            set(at -1)
            set(searchFrom 0)
            string(LENGTH "${rest}" restLength)
            while(searchFrom LESS restLength)
                string(SUBSTRING "${rest}" ${searchFrom} -1 tail)
                string(FIND "${tail}" "${header}" offsetInTail)
                if(offsetInTail EQUAL -1)
                    break()
                endif()
                math(EXPR candidate "${searchFrom} + ${offsetInTail}")
                string(SUBSTRING "${rest}" ${candidate} -1 fromCandidate)
                if(fromCandidate MATCHES "${${rule}PatternAnchored}")
                    set(at ${candidate})
                    break()
                endif()
                math(EXPR searchFrom "${candidate} + 1")
            endwhile()
            if(at EQUAL -1)
                message("")
                message("  In ${sourceFile}, the ${rule} pattern matched a header this scan then could")
                message("  not LOCATE: <${header}>")
                message("")
                message("That is the reader disagreeing with itself -- the unanchored pattern and its")
                message("anchored twin must accept the same text, and a site nobody can place cannot")
                message("be reported, exempted or converted. Refused rather than attributed to a guess.")
                message(FATAL_ERROR "test-loops: a matched site could not be located")
            endif()
            math(EXPR absolute "${consumed} + ${at}")
            string(SUBSTRING "${code}" 0 ${absolute} before)
            string(REGEX MATCHALL "\n" newlines "${before}")
            list(LENGTH newlines newlineCount)
            math(EXPR lineNumber "${newlineCount} + 1")

            set(decided FALSE)
            if(rule STREQUAL "poll")
                list(APPEND pollSites ${absolute})
            elseif(rule STREQUAL "spin" AND absolute IN_LIST pollSites)
                set(decided TRUE)
            endif()

            set(exempted FALSE)
            foreach(index RANGE 0 ${exemptionCount})
                if(index EQUAL exemptionCount)
                    break()
                endif()
                if(exempt_${index}_rule STREQUAL rule AND exempt_${index}_file STREQUAL sourceFile)
                    string(FIND "${header}" "${exempt_${index}_text}" textAt)
                    if(NOT textAt EQUAL -1)
                        math(EXPR exempt_${index}_used "${exempt_${index}_used} + 1")
                        set(exempted TRUE)
                        break()
                    endif()
                endif()
            endforeach()
            if(decided)
            elseif(exempted)
                math(EXPR exemptedCount "${exemptedCount} + 1")
            else()
                # Accumulated per (rule, file) rather than reported here, because whether these
                # sites are a violation depends on the backlog row, which is one question about
                # the whole file. Text, not a list: a loop's header holds its own `;`, which a
                # CMake list would split -- printing the header cut at its first `;` and
                # counting it twice.
                string(REGEX REPLACE "[ \t\r\n]+" " " shown "${header}")
                math(EXPR ruleSites "${ruleSites} + 1")
                if(rule STREQUAL "loop")
                    set(what "a C-style for loop")
                elseif(rule STREQUAL "poll")
                    set(what "a coroutine polling on its reactor")
                else()
                    set(what "a while loop polling an atomic")
                endif()
                string(APPEND ruleSiteText "\n  ${sourceFile}:${lineNumber}: ${what}: ${shown}")
            endif()

            string(LENGTH "${header}" headerLength)
            math(EXPR advance "${at} + ${headerLength}")
            string(SUBSTRING "${rest}" ${advance} -1 rest)
            math(EXPR consumed "${consumed} + ${advance}")
        endwhile()

        # The ratchet, asked once per (rule, file). A row is looked up only when there is
        # something to compare it with; rows nothing matched are found by their `used` flag
        # below, which is the same answer for a file with no sites and a file that is gone.
        set(allowed 0)
        set(allowedIssue "")
        if(ruleSites GREATER 0)
            foreach(index RANGE 0 ${backlogCount})
                if(index EQUAL backlogCount)
                    break()
                endif()
                if(backlog_${index}_rule STREQUAL rule AND backlog_${index}_file STREQUAL sourceFile)
                    set(allowed "${backlog_${index}_count}")
                    set(allowedIssue "${backlog_${index}_issue}")
                    math(EXPR backlog_${index}_used "${backlog_${index}_used} + 1")
                    break()
                endif()
            endforeach()
        endif()
        if(ruleSites GREATER allowed)
            math(EXPR excess "${ruleSites} - ${allowed}")
            math(EXPR violationCount "${violationCount} + ${excess}")
            string(APPEND violations "${ruleSiteText}")
            if(allowed GREATER 0)
                string(APPEND violations
                    "\n      ^ ${ruleSites} ${rule} site(s) here, and FastCachedTestLoopBacklog records ${allowed} for ${allowedIssue}")
            endif()
        elseif(ruleSites LESS allowed)
            math(EXPR fixed "${allowed} - ${ruleSites}")
            string(APPEND staleBacklog
                "\n  ${rule}: ${sourceFile}: the row records ${allowed}, the scan finds ${ruleSites} -- ${fixed} converted, so the row must come down to ${ruleSites}")
        endif()
    endforeach()
endforeach()

if(loopsSeen EQUAL 0)
    message("")
    message("  No `for (` was found in ${fileCount} file(s) via ${scanSource}.")
    message("")
    message("This tree is full of range-for loops, so zero means the comment stripping")
    message("has begun eating code or the scope reads the wrong files -- not that no test loops.")
    message(FATAL_ERROR "test-loops: the scan matched nothing and cannot conclude")
endif()

foreach(index RANGE 0 ${backlogCount})
    if(index EQUAL backlogCount)
        break()
    endif()
    if(backlog_${index}_used EQUAL 0)
        string(APPEND staleBacklog
            "\n  ${backlog_${index}_rule}: ${backlog_${index}_file}: the row records ${backlog_${index}_count}, the scan finds none -- converted, exempted, renamed or deleted")
    endif()
endforeach()

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
    message("says what it waited for when it gives up.")
    message("")
    message("A coroutine parking on its reactor until something holds is the same wait, and it")
    message("cannot block its own reactor thread: `co_await AwaitUntil(reactor, what, reached,")
    message("state, options)` from the same header, bounded on the reactor's clock, its outcome")
    message("kept with `OffThreadWaits::Keep`. Where `Keep` answers false the wait ran out, so the")
    message("coroutine stops there -- `co_return`, or release only what it holds -- rather than")
    message("run on as if what it waited for had happened.")
    message("")
    message("No rule reaches a `while` that does not wait, a coroutine `while` that works before")
    message("it parks, or `vendor/`. The `loop` rule reaches every C++ source under src/ (#1452);")
    message("`spin` and `poll` are test-only, because the wait they ask for is a test helper.")
    message("")
    message("A site that must STAY takes a row in FastCachedTestLoopExemptions, with its reason.")
    message("A site nobody has decided about yet belongs in FastCachedTestLoopBacklog with the")
    message("issue that will decide it -- and if the count for its file is already there, then")
    message("this is a NEW site and the answer is to convert it, not to raise the number. Both")
    message("tables are in ${CMAKE_CURRENT_LIST_FILE}.")
    message("")
    message("Enumerated ${fileCount} file(s) via ${scanSource}.")
    set(refused TRUE)
else()
    set(refused FALSE)
endif()

if(NOT staleBacklog STREQUAL "")
    message("")
    message("FastCachedTestLoopBacklog no longer describes the tree:${staleBacklog}")
    message("")
    message("This is the ratchet, and it only turns one way. A converted site comes OFF the")
    message("backlog in the same change that converts it, or the number it left behind is room")
    message("for the next one to arrive unnoticed.")
    set(refused TRUE)
endif()

if(NOT staleRows STREQUAL "")
    message("")
    message("FastCachedTestLoopExemptions has STALE row(s) matching no site:${staleRows}")
    message("")
    message("The site was converted, moved or deleted -- OR the text names something outside the header as extracted, which ends at the SECOND `;`, so an increment clause cannot be matched. Check that before concluding the site is gone. Remove the row -- a standing exemption")
    message("for a site that is gone would wave the next one of that text through silently.")
    set(refused TRUE)
endif()

# The tally, printed BEFORE any refusal and on every run. This is not decoration: undecided rows
# are allowed at all only because their total is in front of whoever reads this output, which is
# the argument `RefuseUntriaged` rests on in `.agent/rules/metrics-and-observability.md`. Below
# the refusal it printed on exactly the runs nobody needed it on.
#
# Derived from the ROWS, which are the claim. What the scan observed is equal to it on any run
# that passes, and on one that does not the refusal above names every file that disagrees and by
# how much -- so a second accumulator for the same fact would be a second source of truth.
#
# An EMPTY backlog says so rather than printing no tally at all: a line that simply stops
# appearing reads the same as a tally that stopped being computed, and only one of those is
# news. #1452 emptied it, so this is the line the shipped tree prints.
#
# Quoted, because an unquoted operand naming an UNSET variable is read as its own name, which
# is never empty -- and whether `list(REMOVE_DUPLICATES)` leaves an empty list set or unset is
# not something this line should depend on.
if("${backlogIssues}" STREQUAL "")
    message(STATUS "test-loops: backlog: empty -- no loop is waiting on any issue")
endif()
foreach(issue IN LISTS backlogIssues)
    set(issueSites 0)
    set(issueFiles 0)
    foreach(index RANGE 0 ${backlogCount})
        if(index EQUAL backlogCount)
            break()
        endif()
        if(backlog_${index}_issue STREQUAL issue)
            math(EXPR issueSites "${issueSites} + ${backlog_${index}_count}")
            math(EXPR issueFiles "${issueFiles} + 1")
        endif()
    endforeach()
    message(STATUS
        "test-loops: backlog: ${issueSites} site(s) across ${issueFiles} file(s) are waiting on "
        "${issue}, and every one of them is a loop nobody has decided about yet")
endforeach()

# One refusal, after every finding has been printed. Failing at the first of three would report
# a violation and stay silent about a stale row in the same tree, so the next run finds a
# "new" failure that was there all along.
if(refused)
    # Each number DERIVED from what was observed. A refusal that names a cause it did not see
    # sends whoever meets it to the wrong table, and this one can fire for three reasons at once.
    string(REGEX MATCHALL "\n  " staleBacklogRows "${staleBacklog}")
    list(LENGTH staleBacklogRows staleBacklogCount)
    string(REGEX MATCHALL "\n  " staleExemptionRows "${staleRows}")
    list(LENGTH staleExemptionRows staleExemptionCount)
    message(FATAL_ERROR
        "test-loops: ${violationCount} unbacklogged site(s), ${staleBacklogCount} stale backlog "
        "row(s) and ${staleExemptionCount} stale exemption row(s)")
endif()

message(STATUS
    "test-loops: ${loopsSeen} `for (` across ${fileCount} file(s) via ${scanSource}, no new C-style loop, no "
    "atomic-polling while and no coroutine polling on its reactor outside ${exemptedCount} exempted site(s) "
    "in ${exemptionCount} row(s)")
