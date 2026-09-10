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
# - **Per test CASE.** The unit here is the FILE: one selected tag anywhere in it
#   and the file counts as covered, so a new case added to a covered file with an
#   unselected tag leaves the sanitized scope unnoticed. That is the same shape
#   as the bug above, one level down, and closing it means parsing each case's
#   tag string -- issue #317.
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
# Exit codes: 0 = every test file in scope is selectable. 1 = at least one is not.

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

# A tag counts only where Catch2 would see one: inside the quoted tag string of a
# case, so immediately after the opening `"` or after a preceding `]`. A bare
# `[reactor]` in a comment -- or in a test NAME -- is prose, and matching it would
# let a file talk its way into the scope without joining it.
string(REPLACE ";" "|" tagAlternation "${FastCachedTsanScopeTags}")
set(tagPattern "[\"]\\[(${tagAlternation})\\]|\\]\\[(${tagAlternation})\\]")

# ---------------------------------------------------------------------------

set(uncovered "")
set(scannedCount 0)
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
        file(READ "${testFile}" contents)
        if(NOT contents MATCHES "${tagPattern}")
            file(RELATIVE_PATH relativeFile "${FASTCACHED_SOURCE_DIR}" "${testFile}")
            list(APPEND uncovered "    ${relativeFile}")
        endif()
    endforeach()
endforeach()

if(uncovered)
    list(JOIN uncovered "\n" uncoveredReport)
    string(REPLACE ";" "]\n    [" printableTags "[${FastCachedTsanScopeTags}]")
    message(FATAL_ERROR
        "These test files are in the ThreadSanitizer scope but carry no tag the "
        "gate selects on, so they are NOT run under "
        "TSan:\n${uncoveredReport}\n\n"
        "Give each case one of these tags:\n    ${printableTags}\n\n"
        "If a file genuinely does not belong in the sanitized scope, the fix is "
        "to narrow FastCachedTsanScope here -- replace the directory row with "
        "the file rows that do belong -- because widening the tag list would "
        "pull the file IN, not let it out. To widen the scope instead, edit the "
        "TARGETS table in scripts/tsan-gate.sh; this check reads its tags from "
        "there and needs no edit of its own.\n"
        "The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

string(REPLACE ";" "],[" renderedTags "[${FastCachedTsanScopeTags}]")
message("tsan scope: ${scannedCount} test file(s) from ${scopeDirCount} "
        "directory row(s) and ${scopeFileCount} file row(s) are selected by "
        "${renderedTags}")
