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
#
# Its own decisions are driven over synthetic trees by
# `scripts/check-tsan-scope-selftest.cmake`, which `include()`s this file for the
# scope table and the parsed tags rather than restating either.
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

# The row's NAME, beside its tags. `check-tsan-binaries.cmake` needs the set of
# binaries the gate RUNS and must not keep a copy of it: this is the one place
# the table is parsed, and it is parsed above the definitions-only return so an
# includer gets both halves from one reader. A second parser is a second thing to
# be wrong, and one that had drifted would agree with itself perfectly every run.
#
# Sliced by POSITION rather than matched with a regex naming a quote character,
# for the same reason the table is walked with FIND/SUBSTRING above: this reader
# stays free of the escaping the surrounding file argues against, and a row is
# `"name|tagExpression"` by construction -- the parser refuses the table outright
# if it is not.
set(FastCachedTsanGateTargets "")
foreach(row IN LISTS targetRows)
    string(LENGTH "${row}" rowLength)
    if(rowLength LESS 3)
        message(FATAL_ERROR
            "check-tsan-scope: the TARGETS table in ${FastCachedTsanGate} holds "
            "an empty row.\n"
            "Every row is `\"name|tagExpression\"`; an empty one is a row that "
            "names no binary, which would silently shrink the sanitized scope. "
            "The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()
    math(EXPR rowInnerLength "${rowLength} - 2")
    string(SUBSTRING "${row}" 1 ${rowInnerLength} rowName)
    string(FIND "${rowName}" "|" rowBar)
    if(rowBar EQUAL -1)
        message(FATAL_ERROR
            "check-tsan-scope: the TARGETS row ${row} carries no `|`.\n"
            "A row is `\"name|tagExpression\"` and the separator is what tells a "
            "binary name from a tag expression -- a row without one would be read "
            "as a target called `${rowName}` run with no tags, which is a "
            "different and much wider scope than whoever wrote it meant. The rule "
            "lives in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()
    string(SUBSTRING "${rowName}" 0 ${rowBar} rowName)
    if(rowName STREQUAL "")
        message(FATAL_ERROR
            "check-tsan-scope: the TARGETS row ${row} names no binary before its "
            "`|`.\n"
            "The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
    endif()
    list(APPEND FastCachedTsanGateTargets "${rowName}")
endforeach()

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
# The reason this scope is a Catch2 tag expression at all, wired so that whoever
# retires the reason is told (#312).
#
# `catch_discover_tests` registers cases by NAME. Catch2 gained
# `ADD_TAGS_AS_LABELS`, which exports each case's tags to CTest as labels, in
# **3.8.0** -- so on a tree carrying an older Catch2 there is no `ctest -L`
# selection to express the sanitized scope with, and the scope has to be a tag
# expression in a bash table plus this check to enforce it.
#
# Two halves, and they were established differently, which is worth keeping
# separate because only one of them can be re-derived from this repository:
#
#   * MEASURED here, 2026-09-10, against the Catch2 3.6.0 source CPM fetched for
#     this tree: `extras/Catch.cmake` contains **zero** occurrences of
#     `ADD_TAGS_AS_LABELS`, with `catch_discover_tests` at 11 occurrences in the
#     same file as the positive control -- so the zero is an absence rather than
#     a grep that stopped working.
#   * SOURCED, not measured: that 3.8.0 is the release that added it, from
#     Catch2's own `docs/release-notes.md`. Nothing in this tree can check that,
#     which is why the tripwire below fires on ANY bump past the watermark rather
#     than on `>= 3.8.0`. A threshold resting on the half nobody here can verify
#     would be a second thing to be wrong.
#
# **The watermark is a pinned CONDITION, not a copy of the current version, and
# must NOT be made to track `CMakeLists.txt`.** It records the version this
# workaround was reasoned about. Point it at the CPM block and the day somebody
# bumps Catch2 the comparison becomes `x > x`, which is false forever: the
# tripwire silently stops existing, which is the failure mode of every other
# thing in this file.
set(FastCachedCatch2TagLabelWatermark "3.6.0")

set(FastCachedProjectCMakeLists "${FASTCACHED_SOURCE_DIR}/CMakeLists.txt")

# Everything ABOVE this line is definition; everything below it SCANS.
# `check-tsan-scope-selftest.cmake` `include()`s this file with the guard set, so
# it stages its synthetic trees from `FastCachedTsanScope` and drives them with
# the tags parsed out of the real gate -- rather than keeping copies of either.
# That is the same rule the header states about the tag list: a second copy is
# not a cross-check, it is a second thing to be wrong, and a self-test whose
# scope had drifted from the check's would agree with itself perfectly.
#
# Nothing else may use this. It is not a mode this check offers to an operator --
# there is no argument that reaches it and `cmake -P` never sets it -- so the
# scanning path cannot be skipped from a command line.
if(FastCachedTsanScopeDefinitionsOnly)
    return()
endif()

if(NOT EXISTS "${FastCachedProjectCMakeLists}")
    message(FATAL_ERROR
        "check-tsan-scope: ${FastCachedProjectCMakeLists} does not exist.\n"
        "The Catch2 version is read from there to decide whether this scope "
        "mechanism is still necessary; a reading it cannot take must not read as "
        "one that found nothing to say. The rule lives in "
        "${CMAKE_CURRENT_LIST_FILE}.")
endif()

# Read the declared version WITHOUT building a list, for the reason the TARGETS
# reader above gives at length: `CMakeLists.txt` is full of brackets, and
# `file(STRINGS)` would merge elements around an unbalanced one.
file(READ "${FastCachedProjectCMakeLists}" projectCMakeLists)
string(REPLACE "\r\n" "\n" projectCMakeLists "${projectCMakeLists}")
string(FIND "${projectCMakeLists}" "NAME Catch2" catch2NamePos)
set(declaredCatch2Version "")
if(NOT catch2NamePos EQUAL -1)
    string(SUBSTRING "${projectCMakeLists}" ${catch2NamePos} 400 catch2Block)
    if(catch2Block MATCHES "VERSION[ \t]+([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(declaredCatch2Version "${CMAKE_MATCH_1}")
    endif()
endif()

if(declaredCatch2Version STREQUAL "")
    message(FATAL_ERROR
        "check-tsan-scope: could not read a `NAME Catch2 ... VERSION x.y.z` "
        "declaration out of ${FastCachedProjectCMakeLists}.\n"
        "That declaration is what says whether the Catch2 in this tree can "
        "express the sanitized scope as CTest labels. If the dependency moved, "
        "this reader moves with it -- do not delete the question, because a "
        "question nobody asks reads exactly like an answer of no. The rule lives "
        "in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

if(declaredCatch2Version VERSION_GREATER "${FastCachedCatch2TagLabelWatermark}")
    message(FATAL_ERROR
        "Catch2 has moved to ${declaredCatch2Version}, past the "
        "${FastCachedCatch2TagLabelWatermark} this scope mechanism was reasoned "
        "about (#312).\n\n"
        "Re-evaluate expressing the ThreadSanitizer scope as CTest labels: "
        "`catch_discover_tests(... ADD_TAGS_AS_LABELS)` exports each case's tags "
        "as labels from Catch2 3.8.0 on, which would let `scripts/tsan-gate.sh` "
        "select with `ctest -L` instead of a Catch2 tag expression, and would "
        "make this check unnecessary.\n\n"
        "WHAT MUST SURVIVE THAT MOVE, wherever the scope ends up being "
        "expressed: the guard that a scope selecting NOTHING is a refusal. A "
        "typo runs zero cases while every other signal in the run says clean, "
        "and that is what the tag expression's `tested NOTHING` refusal and this "
        "check between them exist for. Moving the scope without moving the guard "
        "trades a workaround for a hole.\n\n"
        "If the move is not being made now, that is a legitimate answer: raise "
        "FastCachedCatch2TagLabelWatermark to ${declaredCatch2Version} in "
        "${CMAKE_CURRENT_LIST_FILE} and say in the commit why the scope stayed a "
        "tag expression. Do NOT make the watermark read the version out of "
        "CMakeLists.txt -- a watermark that tracks its subject is one that can "
        "never fire again.")
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
