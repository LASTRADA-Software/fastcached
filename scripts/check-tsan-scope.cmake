# SPDX-License-Identifier: Apache-2.0
#
# ThreadSanitizer scope hygiene: fail if a test in a concurrency-bearing
# location carries no tag the TSan gate selects on.
#
# `scripts/tsan-gate.sh` runs a SUBSET of the suite, chosen by Catch2 tag
# expression, because a whole-tree sanitized run costs more than it finds. That
# makes the tag list a load-bearing scope declaration -- and a scope declared by
# convention is a scope that rots silently.
#
# It had already rotted before the gate was ever merged. The first version of
# that table selected `[async],[consensus],[distributed]`, on the strength of a
# grep that matched only the FIRST tag in each string. Six of ten `Async/` test
# files are tagged `[reactor]`, `[task]`, `[epoll][reactor]` or
# `[reactor][iocp]` and carry no `[async]` at all -- so the reactor and coroutine
# layer, which is the most thread-bearing code in the tree, was excluded from the
# job written to cover it. Nothing could have noticed: the filter matched 511
# cases, they passed, and every signal in the run said clean.
#
# That is the same failure this project keeps a list about -- the tool ran, the
# artefact was fine, and the thing somebody was told is covered was not -- so the
# fix is not a wider grep. It is this check: the tag convention must be
# ENFORCED, so that a new test file in one of these directories either joins the
# sanitized scope or fails the build.
#
# ## The tag list is READ from the gate, never restated here
#
# An earlier draft of this file kept its own copy of the tags and claimed the two
# "are checked against each other". They were not, and the hole was the same
# shape as the bug above: delete `[task]` from the gate's `TARGETS` row and leave
# a copy here alone, and `Async/Task_test.cpp` still matches the copy, still
# reports covered, and six coroutine cases leave the sanitized scope with every
# signal green. A second list is not a cross-check; it is a second thing to be
# wrong. So the expression is parsed out of `scripts/tsan-gate.sh`, which is the
# text that actually reaches Catch2, and a `TARGETS` table this cannot parse is a
# FATAL_ERROR rather than an empty scope.
#
# ## What this does NOT check, stated so nobody reads more into a green run
#
# - **That the tags appear only in these directories.** A handful of files
#   elsewhere carry them and are swept in as a result. Running MORE than the
#   scope is harmless; running less is the defect.
# - **That a case's tag string is the one Catch2 will read.** The unit here is
#   the CASE since #317 -- it used to be the FILE, so one selected tag anywhere in
#   a file covered every case in it, and a 28th case tagged `[fleetchart]` in a
#   file of 27 `[distributed]` ones left the sanitized scope with this check
#   reporting covered. What is read is the LAST string literal of the header, by
#   a parenthesis-depth walk with the literals removed first; what Catch2 reads is
#   the second macro ARGUMENT. Those agree on every shape in this tree and on
#   every shape the self-test drives, and they are not the same statement.
# - **That the scope table is COMPLETE.** It is a stated claim about where the
#   threads are, checked by nobody. #316 is what that costs: the table named three
#   directories, and a census of `std::thread`/`std::jthread`/`std::async` across
#   `FastCacheTest`'s own sources finds nineteen threaded files, ELEVEN of which
#   were in no scope row and selected by no gate tag. A grep for those spellings
#   is NOT promoted into a check here, deliberately: it is a proxy -- a file that
#   reaches threads through a helper spawns none of its own, and a file naming
#   `std::thread` in a comment spawns none at all -- so a check built on it would
#   refuse correct files and miss incorrect ones, which is worse than a stated
#   table somebody has to argue with. Re-run the census when adding a threaded
#   test; the residue below is what the table currently claims nothing about.
# - **Anything in a directory a FILE row names.** `Cache/`, `Core/`, `Protocol/`
#   and `Server/` are in scope one file at a time, because most of what is in them
#   is single-threaded and a directory row would demand a scope tag on every case
#   there. So a NEW threaded test file next to `ShardedStorage_test.cpp` joins the
#   sanitized scope only when somebody adds its row. That is a real hole and it is
#   narrower than the one it replaces, which was four whole directories.
#
# A tag is matched where Catch2 would see one: directly after the opening `"` of
# the tag string, or after a preceding `]`. Catch2 also accepts space-separated
# tags (`"[slow] [async]"`); nothing in this tree writes them that way, and the
# failure if something does is a loud false refusal naming the file, never a
# silent pass.
#
# Runs as `cmake -P`, for the reason check-repository-hygiene.cmake gives at
# length: this reads files, compares strings and reports, so a .sh + .ps1 pair
# would be two implementations of one rule differing only in syntax.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-tsan-scope.cmake
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_TSAN_SCOPE_SELFTEST=ON \
#         -DFASTCACHED_SELFTEST_DIR=<scratch> -P scripts/check-tsan-scope.cmake
#
# Exit codes: 0 = every test case in scope is selectable. 1 = at least one is
# not, or this check could not answer the question -- which it refuses rather
# than reports clean, every time, because those two are the same green.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# ---------------------------------------------------------------------------
# The tests that must be reachable by the gate. One row per location, and a row
# is either a DIRECTORY (every `*_test.cpp` under it, recursively) or a single
# FILE. This list is the one thing stated here rather than derived: it is the
# claim "this is where the threads are", and widening it is a deliberate act.
#
# **Why a row may be a file.** Until #316 every row was a directory, and that
# shape is what kept the threaded tests in `Cache/`, `Core/`, `Protocol/` and
# `Server/` out: those directories are mostly single-threaded, so a directory row
# would have demanded a gate tag on every case in them -- and a check that forces
# an unrelated tag onto a case just to satisfy itself is worse than the gap it
# closes. A file row states the narrower claim the tree actually supports.
#
# It is also the exemption mechanism, which is why there is no separate one: a
# genuinely single-threaded FILE inside a scoped directory is taken out by
# replacing that directory row with the file rows that belong -- not by an
# opt-out somebody can spell without saying what they are opting out of.
#
# Each row and what it is here for. `Net/` is a directory because every file in
# it carries `[net]`, because six of its tests spawn threads, and because it is
# where `BlockingListener` lives -- the class the tree's one observed race (#260)
# was in, which this gate reached only through the node binary being run whole.
set(FastCachedTsanScope
    # Directories: everything here is concurrency-bearing.
    "src/FastCache/Async"
    "src/FastCache/Consensus"
    "src/FastCache/Distributed"
    "src/FastCache/Net"
    # Files: one threaded test in a directory that is otherwise not.
    "src/FastCache/Cache/ExpiryReaper_test.cpp"
    "src/FastCache/Cache/ShardedStorage_test.cpp"
    "src/FastCache/Core/Clock_test.cpp"
    "src/FastCache/Protocol/RedisRespSocket_test.cpp"
    "src/FastCache/Server/ReactorServerLoop_test.cpp"
    "src/FastCache/Server/ReadinessAnnouncer_test.cpp"
)

# The gate whose scope this enforces. Its `TARGETS` table is the source of truth
# for which tags are selected; see the header for why nothing is copied out of it.
set(FastCachedTsanGate "${FASTCACHED_SOURCE_DIR}/scripts/tsan-gate.sh")

# ---------------------------------------------------------------------------
# Parse the tag expressions out of the gate's TARGETS table.
#
# A row is "name|tagExpression"; an empty expression means the binary is run
# whole, which selects everything and so contributes no constraint. Every failure
# to parse is fatal, because a scope this cannot read must not read as an empty
# scope -- the whole file exists because "nothing was checked" and "everything is
# fine" look identical otherwise.

if(NOT EXISTS "${FastCachedTsanGate}")
    message(FATAL_ERROR
        "check-tsan-scope: ${FastCachedTsanGate} does not exist.\n"
        "This check derives the sanitized scope from that script's TARGETS "
        "table; it cannot substitute a list of its own.")
endif()

# Read the table LINE BY LINE, between `TARGETS=(` and the `)` that closes it in
# column zero -- not with a `[^)]*` block match. A Catch2 tag expression may
# legally contain parentheses (`[a]&&([b]||[c])`), which is exactly the edit the
# messages below invite; a block match would stop at the first of those, silently
# drop every row after it, and then fail on unrelated files for "carrying no tag".
# Walked with `FIND`/`SUBSTRING` and never turned into a CMake list -- not
# `file(STRINGS)`, and NOT the neutralising split the sibling checks use.
#
# `file(STRINGS)` returns a LIST, and an UNBALANCED `[` or `]` on a KEPT line
# merges elements. MEASURED here: one `]` added to a comment on the `TARGETS=(`
# line took this check from a clean pass to a hard refusal, because it could no
# longer find the table.
#
# **But the usual remedy is wrong for this reader, and trying it is how the
# stronger rule gets learned twice.** Blanking `[` and `]` before splitting is
# what the other checks do, and here the brackets ARE THE DATA: a Catch2 tag is
# spelled `[async]`, so neutralising them makes the table name no tags at all and
# the check refuses on a perfectly good tree. Measured -- the first attempt at
# this fix did exactly that. The rulebook already says so: *where brackets must
# survive, do not neutralise them -- read the lines without ever building a CMake
# list, which is immune by construction rather than by convention*, and it
# records the same over-broad fix taking `check-worker-refusals-counted` from
# three refusal spellings to zero.
#
# So this walks the content by offset. Nothing here is ever a list, so there is
# nothing for CMake's list parser to group, and every bracket reaches the tag
# matcher untouched. `check-test-names.cmake` reads its sources the same way and
# for the same reason.
file(READ "${FastCachedTsanGate}" gateContent)
string(REPLACE "\r\n" "\n" gateContent "${gateContent}")
set(inTable FALSE)
set(targetRows "")
set(sawTable FALSE)
while(NOT gateContent STREQUAL "")
    string(FIND "${gateContent}" "\n" gateNewline)
    if(gateNewline EQUAL -1)
        set(line "${gateContent}")
        set(gateContent "")
    else()
        string(SUBSTRING "${gateContent}" 0 ${gateNewline} line)
        math(EXPR gateNewline "${gateNewline} + 1")
        string(SUBSTRING "${gateContent}" ${gateNewline} -1 gateContent)
    endif()

    if(inTable)
        if(line MATCHES "^\\)")
            set(inTable FALSE)
        else()
            string(REGEX MATCHALL "\"[^\"]*\"" lineRows "${line}")
            list(APPEND targetRows ${lineRows})
        endif()
    elseif(line MATCHES "^TARGETS=\\(")
        set(inTable TRUE)
        set(sawTable TRUE)
    endif()
endwhile()

if(NOT sawTable OR inTable)
    message(FATAL_ERROR
        "check-tsan-scope: could not read the TARGETS=( ... ) table in "
        "${FastCachedTsanGate}.\n"
        "It must open with `TARGETS=(` and close with `)` in column zero. If "
        "that table changed shape, this parser changes with it -- do not restore "
        "a copy of the tag list here. The rule lives in "
        "${CMAKE_CURRENT_LIST_FILE}.")
endif()

set(FastCachedTsanScopeTags "")
foreach(row IN LISTS targetRows)
    # "name|[a],[b]" -> [a],[b] -> a;b
    string(REGEX REPLACE "^\"[^|]*\\|" "" rowTags "${row}")
    string(REGEX REPLACE "\"$" "" rowTags "${rowTags}")
    string(REGEX MATCHALL "\\[([A-Za-z0-9_-]+)\\]" rowTagMatches "${rowTags}")
    foreach(tagMatch IN LISTS rowTagMatches)
        string(REGEX REPLACE "^\\[|\\]$" "" tag "${tagMatch}")
        list(APPEND FastCachedTsanScopeTags "${tag}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES FastCachedTsanScopeTags)

if(NOT FastCachedTsanScopeTags)
    message(FATAL_ERROR
        "check-tsan-scope: the TARGETS table in ${FastCachedTsanGate} names no "
        "Catch2 tags at all.\n"
        "Either every target is now run whole -- in which case this check has "
        "nothing to enforce and should be removed deliberately -- or the table "
        "was mis-edited. It is not treated as an empty scope. The rule lives in "
        "${CMAKE_CURRENT_LIST_FILE}.")
endif()

# ---------------------------------------------------------------------------
# `-DFASTCACHED_TSAN_SCOPE_SELFTEST=ON`: drive this check over synthetic trees.
#
# A guard nobody has watched refuse is not a guard, and a guard nobody has
# watched ACCEPT is not known to work -- #1031 is two days of a gate failing
# CLOSED and unconditionally with a confidently worded false cause. So every case
# below states the direction it drives, and the first one is the accepting one.
#
# **The synthetic trees are staged from `FastCachedTsanScope` and the REAL gate,
# never from a copy of either.** Each tree is a `src/` mirroring every row of that
# table plus a copy of `scripts/tsan-gate.sh`, so the child invocation runs the
# shipped scope table and the shipped tag expression against files this function
# wrote. A self-test that carried its own two-row scope would be a second thing
# to be wrong, which is the defect the header of this file already records once.
#
# The verdict is read from the child's OUTPUT and never from its exit code alone:
# `message(WARNING)` exits 0 while printing a diagnostic, so a check that only
# warned would pass an exit-code test. **And CMake WRAPS a diagnostic at about 74
# columns**, so every expectation is matched against the FLATTENED output --
# measured in this repository at zero matches for a `FATAL_ERROR` phrase that
# crosses the column, with a mutation harness once calling all three of its arms
# green while the thing it drove was red.
if(FASTCACHED_TSAN_SCOPE_SELFTEST)
    list(GET FastCachedTsanScopeTags 0 selftestTag)

    if(NOT DEFINED FASTCACHED_SELFTEST_DIR)
        set(FASTCACHED_SELFTEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/tsan-scope-selftest")
    endif()

    set(selftestRan 0)
    set(selftestFailed 0)

    # Stage a tree in which every row of the real scope table exists and is
    # covered. Returns the directory. `which` names the case, so a failure names
    # a directory somebody can go and look at rather than one shared path that
    # the next case has already overwritten.
    function(FastCachedStageTree which out)
        set(tree "${FASTCACHED_SELFTEST_DIR}/${which}")
        file(REMOVE_RECURSE "${tree}")
        file(MAKE_DIRECTORY "${tree}/scripts")
        file(COPY "${FastCachedTsanGate}" DESTINATION "${tree}/scripts")
        foreach(row IN LISTS FastCachedTsanScope)
            if(row MATCHES "_test\\.cpp$")
                set(staged "${tree}/${row}")
            else()
                set(staged "${tree}/${row}/Baseline_test.cpp")
            endif()
            file(WRITE "${staged}"
                "TEST_CASE(\"baseline\", \"[${selftestTag}]\")\n{\n}\n")
        endforeach()
        set("${out}" "${tree}" PARENT_SCOPE)
    endfunction()

    # One case. `expect` is `pass` or `refuse`; `needle` is a phrase that must
    # appear in the flattened output either way -- so the accepting direction
    # asserts something POSITIVE was reported and not merely that nothing was.
    function(FastCachedSelftestCase which tree expect needle)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}"
                    -P "${CMAKE_CURRENT_LIST_FILE}"
            OUTPUT_VARIABLE childOut
            ERROR_VARIABLE childErr
            RESULT_VARIABLE childStatus
            ENCODING NONE)
        string(REGEX REPLACE "[\r\n]+" " " flat "${childOut} ${childErr}")
        string(REGEX REPLACE " +" " " flat "${flat}")

        set(verdict "pass")
        if(NOT childStatus EQUAL 0 OR flat MATCHES "CMake Error")
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

    # The file every mutating case reaches for: a covered file inside a scoped
    # DIRECTORY, so an added case can be shown to leave the scope while the file
    # around it stays covered. That arrangement IS #317 -- a per-file check
    # passes on it.
    set(victim "src/FastCache/Async/Baseline_test.cpp")

    message("== check-tsan-scope --self-test")

    # -- the accepting direction, first, and asserting a positive report -------
    FastCachedStageTree("baseline" tree)
    FastCachedSelftestCase("baseline-tree-is-accepted" "${tree}" pass "case.s. in")

    FastCachedStageTree("multiline" tree)
    file(WRITE "${tree}/${victim}"
        "TEST_CASE(\"a name long enough to push the tag string onto its own line\",\n"
        "          \"[${selftestTag}]\")\n{\n}\n")
    FastCachedSelftestCase("tag-string-on-the-next-line" "${tree}" pass "case.s. in")

    FastCachedStageTree("adjacent" tree)
    file(WRITE "${tree}/${victim}"
        "TEST_CASE(\"first half of a name \"\n"
        "          \"second half of a name\",\n"
        "          \"[${selftestTag}]\")\n{\n}\n")
    FastCachedSelftestCase("a-name-spelled-as-two-adjacent-literals" "${tree}" pass "case.s. in")

    # An UNMATCHED parenthesis, not a matched pair. A reader that counted
    # parentheses without removing the string literals first would still close a
    # matched pair correctly and pass such a case for the wrong reason; one `(`
    # leaves it inside the header, swallowing every case after it. So this case
    # is driven in the REFUSING direction, with an unselected case behind the
    # tricky one: the finding proves the reader got past it.
    FastCachedStageTree("parens" tree)
    file(WRITE "${tree}/${victim}"
        "TEST_CASE(\"a name with an unmatched ( in it\", \"[${selftestTag}]\")\n{\n}\n"
        "TEST_CASE(\"the case behind it\", \"[somethingelse]\")\n{\n}\n")
    FastCachedSelftestCase("an-unmatched-parenthesis-inside-a-case-name" "${tree}" refuse "the case behind it")

    FastCachedStageTree("brackets-only" tree)
    file(WRITE "${tree}/${victim}"
        "// a comment carrying an unbalanced ] bracket\n"
        "TEST_CASE(\"ordinary\", \"[${selftestTag}]\")\n{\n}\n")
    FastCachedSelftestCase("an-unbalanced-bracket-alone-changes-nothing" "${tree}" pass "case.s. in")

    # -- the refusing direction ------------------------------------------------
    FastCachedStageTree("added-case" tree)
    file(APPEND "${tree}/${victim}"
        "TEST_CASE(\"added later\", \"[somethingelse]\")\n{\n}\n")
    FastCachedSelftestCase("an-unselected-case-in-a-covered-file" "${tree}" refuse "added later")

    FastCachedStageTree("brackets-and-violation" tree)
    file(APPEND "${tree}/${victim}"
        "// a comment carrying an unbalanced ] bracket\n"
        "TEST_CASE(\"added later\", \"[somethingelse]\")\n{\n}\n")
    FastCachedSelftestCase("a-violation-behind-an-unbalanced-bracket" "${tree}" refuse "added later")

    # The REPORT is text out of a source file, so it has the hazard this file is
    # about. Two case names in the current scope contain a `;`, which splits a
    # `list(APPEND)` element in two; a `[` would merge two findings into one and
    # a refusal would then name fewer cases than it found. Both characters, in
    # one name, asserted to come back WHOLE.
    FastCachedStageTree("punctuated-name" tree)
    file(APPEND "${tree}/${victim}"
        "TEST_CASE(\"a name with a ; and an unbalanced [ in it\", \"[somethingelse]\")\n{\n}\n")
    FastCachedSelftestCase("a-finding-whose-name-holds-a-semicolon-and-a-bracket"
        "${tree}" refuse "a name with a ; and an unbalanced . in it")

    FastCachedStageTree("no-tag-string" tree)
    file(APPEND "${tree}/${victim}" "TEST_CASE(\"untagged\")\n{\n}\n")
    FastCachedSelftestCase("a-case-with-no-tag-string" "${tree}" refuse "no tag string at all")

    FastCachedStageTree("name-carries-a-tag" tree)
    file(APPEND "${tree}/${victim}"
        "TEST_CASE(\"prose about [${selftestTag}] behaviour\", \"[somethingelse]\")\n{\n}\n")
    FastCachedSelftestCase("a-name-cannot-talk-a-case-into-the-scope" "${tree}" refuse "prose about")

    FastCachedStageTree("scenario" tree)
    file(APPEND "${tree}/${victim}" "SCENARIO(\"a scenario\", \"[somethingelse]\")\n{\n}\n")
    FastCachedSelftestCase("SCENARIO-is-read-too" "${tree}" refuse "a scenario")

    FastCachedStageTree("test-case-method" tree)
    file(APPEND "${tree}/${victim}"
        "TEST_CASE_METHOD(Fixture, \"a fixtured case\", \"[somethingelse]\")\n{\n}\n")
    FastCachedSelftestCase("TEST_CASE_METHOD-is-read-too" "${tree}" refuse "a fixtured case")

    FastCachedStageTree("runaway" tree)
    file(APPEND "${tree}/${victim}" "TEST_CASE(\"never closed\",\n")
    FastCachedSelftestCase("a-header-that-never-closes" "${tree}" refuse "still open at end of file")

    # The same fault far enough from the end of the file to hit the runaway bound
    # instead of the end-of-file arm. Two arms, because they are reported by two
    # different messages and a reader meeting one of them must not be told the
    # other one's story.
    FastCachedStageTree("runaway-bound" tree)
    file(APPEND "${tree}/${victim}" "TEST_CASE(\"never closed\",
")
    foreach(filler RANGE 1 70)
        file(APPEND "${tree}/${victim}" "// filler line ${filler}
")
    endforeach()
    FastCachedSelftestCase("a-header-that-outruns-the-line-bound" "${tree}" refuse "has no closing")

    FastCachedStageTree("no-cases" tree)
    foreach(row IN LISTS FastCachedTsanScope)
        if(row MATCHES "_test\\.cpp$")
            file(WRITE "${tree}/${row}" "// no cases here\n")
        else()
            file(WRITE "${tree}/${row}/Baseline_test.cpp" "// no cases here\n")
        endif()
    endforeach()
    FastCachedSelftestCase("a-scope-that-holds-no-case-at-all" "${tree}" refuse "found no Catch2 case")

    FastCachedStageTree("empty-directory" tree)
    file(REMOVE "${tree}/src/FastCache/Async/Baseline_test.cpp")
    FastCachedSelftestCase("a-directory-row-with-no-test-files" "${tree}" refuse "contains no ..test.cpp files")

    FastCachedStageTree("missing-file-row" tree)
    file(REMOVE "${tree}/src/FastCache/Core/Clock_test.cpp")
    FastCachedSelftestCase("a-file-row-that-does-not-exist" "${tree}" refuse "names neither a directory nor a file")

    # The count is printed because a self-test that STOPPED early must not look
    # like one that judged everything: `set -e`'s CMake equivalent is a
    # FATAL_ERROR anywhere above, and eight cases reported green is what that
    # looks like from the outside.
    message("check-tsan-scope --self-test: ${selftestRan} case(s) ran, ${selftestFailed} failed")
    if(NOT selftestFailed EQUAL 0)
        message(FATAL_ERROR
            "check-tsan-scope --self-test: ${selftestFailed} of ${selftestRan} "
            "case(s) failed. The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()
    return()
endif()

# A tag counts only where Catch2 would see one: inside the quoted tag string of a
# case, so immediately after the opening `"` or after a preceding `]`. A bare
# `[reactor]` in a comment -- or in a test NAME -- is prose, and matching it would
# let a file talk its way into the scope without joining it.
string(REPLACE ";" "|" tagAlternation "${FastCachedTsanScopeTags}")
set(tagPattern "[\"]\\[(${tagAlternation})\\]|\\]\\[(${tagAlternation})\\]")

# ---------------------------------------------------------------------------
# Reading a case out of a source file.
#
# Every Catch2 macro that names a case, taken verbatim from
# `check-test-names.cmake` and for its reason: all four spellings, not the one
# this tree happens to use today, because a guard that covers the macro in front
# of it and not the neighbouring one is a guard somebody walks around without
# meaning to.
set(FastCachedCaseMacroPattern "^[ \t]*(TEST_CASE|TEST_CASE_METHOD|TEMPLATE_TEST_CASE|SCENARIO)[ \t]*\\(")

# A case header may span lines, and 28 of the ones in scope do. This is the
# runaway bound: a header still open after this many lines is a PARSE FAILURE and
# is refused by name, never quietly abandoned -- an abandoned header is a case
# nobody checked, which is this file's entire subject.
set(FastCachedCaseHeaderLineBound 60)

# Split one line into its quoted string literals and the text between them,
# WITHOUT ever building a CMake list.
#
# `string(REGEX MATCHALL "\"[^\"]*\"")` is the obvious spelling and is the hazard
# the header of this file spends a paragraph on: MATCHALL returns a LIST, and an
# unbalanced `[` or `]` on the line merges elements. Case names in this tree
# contain brackets by the dozen. So the quotes are found by offset, exactly as
# the TARGETS reader above walks newlines.
#
# Out-parameters: `<out>_last` is the LAST literal, quotes included; `<out>_count`
# is how many there were; `<out>_first` is the first, for reporting; `<out>_bare`
# is the line with every literal removed, which is what parentheses are counted
# on -- a `(` inside a case name must not open anything.
#
# Escaped quotes are not modelled. Measured across all 220 `*_test.cpp` files in
# this tree: zero case headers contain one. If one appears, the name splits into
# two literals and the LAST is still the tag string, so the classification
# survives it; only the reported name would be short.
function(FastCachedSplitLiterals line out)
    set(rest "${line}")
    set(bare "")
    set(count 0)
    set(lastLit "")
    set(firstLit "")
    while(TRUE)
        string(FIND "${rest}" "\"" openPos)
        if(openPos EQUAL -1)
            string(APPEND bare "${rest}")
            break()
        endif()
        string(SUBSTRING "${rest}" 0 ${openPos} before)
        string(APPEND bare "${before}")
        math(EXPR afterOpen "${openPos} + 1")
        string(SUBSTRING "${rest}" ${afterOpen} -1 tail)
        string(FIND "${tail}" "\"" closeOffset)
        if(closeOffset EQUAL -1)
            # An unterminated literal. Catch2 headers do not contain one, and
            # treating the remainder as ordinary text would let its parentheses
            # close a header early -- so stop reading this line rather than
            # guessing, and let the runaway bound report it if it matters.
            break()
        endif()
        math(EXPR litLength "${closeOffset} + 2")
        string(SUBSTRING "${rest}" ${openPos} ${litLength} literal)
        math(EXPR count "${count} + 1")
        if(count EQUAL 1)
            set(firstLit "${literal}")
        endif()
        set(lastLit "${literal}")
        math(EXPR consumed "${openPos} + ${litLength}")
        string(SUBSTRING "${rest}" ${consumed} -1 rest)
    endwhile()
    set("${out}_last" "${lastLit}" PARENT_SCOPE)
    set("${out}_first" "${firstLit}" PARENT_SCOPE)
    set("${out}_count" "${count}" PARENT_SCOPE)
    set("${out}_bare" "${bare}" PARENT_SCOPE)
endfunction()

# How many times `needle` occurs in `text`. By length difference rather than by
# `MATCHALL` + `list(LENGTH)`, for the reason above: no list is built, so nothing
# in the text can be grouped by CMake's list parser.
function(FastCachedCountChar text needle out)
    string(REPLACE "${needle}" "" without "${text}")
    string(LENGTH "${text}" withLength)
    string(LENGTH "${without}" withoutLength)
    math(EXPR occurrences "${withLength} - ${withoutLength}")
    set("${out}" "${occurrences}" PARENT_SCOPE)
endfunction()

# Walk one test file and append `file:line: <case>` for every CASE whose tag
# string carries no selected tag. Sets `<out>_cases` to how many cases it read,
# because a file that suddenly holds none is a reader that stopped working and
# must not read as a file with nothing wrong.
#
# **The tag string is the LAST string literal in the header, never the second.**
# Two measurements say so, and only one of them is obvious. Three cases in this
# tree spell a long name as two adjacent literals -- `"first half " "second
# half", "[tags]"` -- where the second literal is half a NAME. And 489 cases
# tree-wide carry no tag string at all, where the first literal is the whole
# name; those are counted by ARITY (`< 2` literals) rather than by pattern, since
# a name is free to contain `[async]` and must not be able to talk its way in.
function(FastCachedScanTestFile testFile relativeFile tagPattern out)
    file(READ "${testFile}" sourceRest)
    string(REPLACE "\r\n" "\n" sourceRest "${sourceRest}")

    set(findings "")
    set(caseCount 0)
    set(lineNumber 0)
    set(inHeader FALSE)
    while(NOT sourceRest STREQUAL "")
        string(FIND "${sourceRest}" "\n" sourceNewline)
        if(sourceNewline EQUAL -1)
            set(line "${sourceRest}")
            set(sourceRest "")
        else()
            string(SUBSTRING "${sourceRest}" 0 ${sourceNewline} line)
            math(EXPR sourceNewline "${sourceNewline} + 1")
            string(SUBSTRING "${sourceRest}" ${sourceNewline} -1 sourceRest)
        endif()
        math(EXPR lineNumber "${lineNumber} + 1")

        if(NOT inHeader)
            if(NOT line MATCHES "${FastCachedCaseMacroPattern}")
                continue()
            endif()
            set(inHeader TRUE)
            set(headerLine "${lineNumber}")
            set(headerDepth 0)
            set(headerEntered FALSE)
            set(headerCount 0)
            set(headerFirst "")
            set(headerLast "")
        endif()

        FastCachedSplitLiterals("${line}" lit)
        if(lit_count GREATER 0)
            if(headerCount EQUAL 0)
                set(headerFirst "${lit_first}")
            endif()
            set(headerLast "${lit_last}")
            math(EXPR headerCount "${headerCount} + ${lit_count}")
        endif()
        FastCachedCountChar("${lit_bare}" "(" opens)
        FastCachedCountChar("${lit_bare}" ")" closes)
        if(opens GREATER 0)
            set(headerEntered TRUE)
        endif()
        math(EXPR headerDepth "${headerDepth} + ${opens} - ${closes}")

        math(EXPR headerSpan "${lineNumber} - ${headerLine} + 1")
        if(headerSpan GREATER "${FastCachedCaseHeaderLineBound}")
            message(FATAL_ERROR
                "check-tsan-scope: the case macro at ${relativeFile}:${headerLine} "
                "has no closing `)` within ${FastCachedCaseHeaderLineBound} lines.\n"
                "This reader tracks parenthesis depth with string literals "
                "removed, so it is reporting that it lost its place rather than "
                "that the source is wrong. A header it cannot read is a case it "
                "cannot check, and a case it cannot check must not pass. The "
                "rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
        endif()

        if(NOT headerEntered OR headerDepth GREATER 0)
            continue()
        endif()

        set(inHeader FALSE)
        math(EXPR caseCount "${caseCount} + 1")
        # `string(APPEND)`, never `list(APPEND)`. The report carries a case NAME,
        # which is text out of a source file: two names in the current scope
        # contain a `;`, which would split one finding into two list elements, and
        # a name containing an unbalanced `[` would MERGE two findings into one --
        # so a refusal would name fewer cases than it found. That is this file's
        # own doctrine arriving in its report path, where it is easy to miss
        # because the verdict stays right and only the evidence goes wrong.
        if(headerCount LESS 2)
            string(APPEND findings
                "    ${relativeFile}:${headerLine}: ${headerFirst} -- no tag string at all\n")
        elseif(NOT headerLast MATCHES "${tagPattern}")
            string(APPEND findings
                "    ${relativeFile}:${headerLine}: ${headerFirst} -- tagged ${headerLast}\n")
        endif()
    endwhile()

    if(inHeader)
        message(FATAL_ERROR
            "check-tsan-scope: the case macro at ${relativeFile}:${headerLine} "
            "is still open at end of file.\n"
            "A header this reader cannot close is a case it did not check. The "
            "rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()

    set("${out}" "${findings}" PARENT_SCOPE)
    set("${out}_cases" "${caseCount}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------

set(uncovered "")
set(scannedCount 0)
set(caseCount 0)
set(scopeDirCount 0)
set(scopeFileCount 0)

foreach(scopeRow IN LISTS FastCachedTsanScope)
    set(absoluteRow "${FASTCACHED_SOURCE_DIR}/${scopeRow}")
    if(IS_DIRECTORY "${absoluteRow}")
        math(EXPR scopeDirCount "${scopeDirCount} + 1")
        # GLOB_RECURSE, for the reason check-net-boundary.cmake states: a file
        # this does not scan is a hole that reports green. `Async/` is flat today
        # and a subdirectory added tomorrow must not walk out of the scope
        # unnoticed.
        file(GLOB_RECURSE testFiles "${absoluteRow}/*_test.cpp")
        if(NOT testFiles)
            message(FATAL_ERROR
                "check-tsan-scope: ${scopeRow} contains no *_test.cpp files.\n"
                "That is either a directory that lost its tests or a glob that "
                "stopped matching; both make the sanitized scope smaller than it "
                "reads.")
        endif()
    elseif(EXISTS "${absoluteRow}")
        math(EXPR scopeFileCount "${scopeFileCount} + 1")
        # A file row that has stopped being a test file is a row that stopped
        # meaning anything: `contents MATCHES` would still run over a renamed
        # header and report covered or uncovered for a file no test binary holds.
        if(NOT scopeRow MATCHES "_test\\.cpp$")
            message(FATAL_ERROR
                "check-tsan-scope: the scope row ${scopeRow} is not a "
                "*_test.cpp file.\n"
                "A file row names one Catch2 test source. If that test moved, "
                "move the row; if it became a directory's worth of tests, make "
                "the row a directory. The rule lives in "
                "${CMAKE_CURRENT_LIST_FILE}.")
        endif()
        set(testFiles "${absoluteRow}")
    else()
        # A renamed or removed row must not silently shrink the scope: the whole
        # point of this file is that "nothing to check" and "everything is fine"
        # have to look different. Reported as ONE refusal covering both row
        # kinds, because at this point the row names neither -- claiming it was
        # "the directory" would send a reader looking for a directory that a
        # deleted test file never was.
        message(FATAL_ERROR
            "check-tsan-scope: the scope row ${scopeRow} names neither a "
            "directory nor a file that exists.\n"
            "If it moved, update FastCachedTsanScope here and the TARGETS "
            "table in scripts/tsan-gate.sh together.")
    endif()

    foreach(testFile IN LISTS testFiles)
        math(EXPR scannedCount "${scannedCount} + 1")
        file(RELATIVE_PATH relativeFile "${FASTCACHED_SOURCE_DIR}" "${testFile}")
        FastCachedScanTestFile("${testFile}" "${relativeFile}" "${tagPattern}" fileFindings)
        math(EXPR caseCount "${caseCount} + ${fileFindings_cases}")
        # A STRING throughout, and `STREQUAL ""` rather than `if(fileFindings)`:
        # the report holds case names, and a name that happened to be `NO` or
        # `OFF` would make a real finding read as nothing to report.
        if(NOT fileFindings STREQUAL "")
            string(APPEND uncovered "${fileFindings}")
        endif()
    endforeach()
endforeach()

# Two empty lists agree perfectly, and this check has two ways to become one: a
# scope that scans no file, and a reader that finds no case in the files it does
# scan. The first is refused per row above; this is the second.
if(caseCount EQUAL 0)
    message(FATAL_ERROR
        "check-tsan-scope: scanned ${scannedCount} file(s) and found no Catch2 "
        "case in any of them.\n"
        "That is this reader having stopped working, not a tree with nothing to "
        "check -- and the two produce the same clean run otherwise. The rule "
        "lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

if(NOT uncovered STREQUAL "")
    string(REPLACE ";" "]\n    [" printableTags "[${FastCachedTsanScopeTags}]")
    message(FATAL_ERROR
        "These test CASES are in the ThreadSanitizer scope but carry no tag the "
        "gate selects on, so they are NOT run under "
        "TSan:\n${uncovered}\n"
        "Give each of them one of these tags:\n    ${printableTags}\n\n"
        "If a file genuinely does not belong in the sanitized scope, the fix is "
        "to narrow FastCachedTsanScope here -- replace the directory row with "
        "the file rows that do belong -- because widening the tag list would "
        "pull the file IN, not let it out. To widen the scope instead, edit the "
        "TARGETS table in scripts/tsan-gate.sh; this check reads its tags from "
        "there and needs no edit of its own.\n"
        "The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

string(REPLACE ";" "],[" renderedTags "[${FastCachedTsanScopeTags}]")
message("tsan scope: ${caseCount} case(s) in ${scannedCount} test file(s) from "
        "${scopeDirCount} directory row(s) and ${scopeFileCount} file row(s) are "
        "selected by ${renderedTags}")
