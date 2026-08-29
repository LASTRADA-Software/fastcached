# SPDX-License-Identifier: Apache-2.0
#
# ThreadSanitizer scope hygiene: fail if a test in a concurrency-bearing
# directory carries no tag the TSan gate selects on.
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
#   tag string. Tracked in .agent/rules/build-and-toolchain.md's Open work.
# - **That the scope directories are the right ones.** `Net/` and `Cache/` also
#   spawn threads, and the one race this gate suppresses (#260) is a `Net/`
#   class, reached only because the node binary is run whole. Also tracked there.
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

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# ---------------------------------------------------------------------------
# The directories whose tests must be reachable by the gate. One row per
# directory. This list is the one thing stated here rather than derived: it is
# the claim "this is where the threads are", and widening it is a deliberate act.
set(FastCachedTsanScopeDirs
    "src/FastCache/Async"
    "src/FastCache/Consensus"
    "src/FastCache/Distributed"
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
file(STRINGS "${FastCachedTsanGate}" gateLines)
set(inTable FALSE)
set(targetRows "")
set(sawTable FALSE)
foreach(line IN LISTS gateLines)
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
endforeach()

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

foreach(scopeDir IN LISTS FastCachedTsanScopeDirs)
    set(absoluteDir "${FASTCACHED_SOURCE_DIR}/${scopeDir}")
    if(NOT IS_DIRECTORY "${absoluteDir}")
        # A renamed or removed directory must not silently empty the scope: the
        # whole point of this file is that "nothing to check" and "everything is
        # fine" have to look different.
        message(FATAL_ERROR
            "check-tsan-scope: ${scopeDir} does not exist.\n"
            "If it moved, update FastCachedTsanScopeDirs here and the TARGETS "
            "table in scripts/tsan-gate.sh together.")
    endif()

    # GLOB_RECURSE, for the reason check-net-boundary.cmake states: a file this
    # does not scan is a hole that reports green. `Async/` is flat today and a
    # subdirectory added tomorrow must not walk out of the scope unnoticed.
    file(GLOB_RECURSE testFiles "${absoluteDir}/*_test.cpp")
    if(NOT testFiles)
        message(FATAL_ERROR
            "check-tsan-scope: ${scopeDir} contains no *_test.cpp files.\n"
            "That is either a directory that lost its tests or a glob that "
            "stopped matching; both make the sanitized scope smaller than it "
            "reads.")
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
        "These test files sit in a concurrency-bearing directory but carry no "
        "tag the ThreadSanitizer gate selects on, so they are NOT run under "
        "TSan:\n${uncoveredReport}\n\n"
        "Give each case one of these tags:\n    ${printableTags}\n\n"
        "If a file genuinely does not belong in the sanitized scope, the fix is "
        "to narrow FastCachedTsanScopeDirs here -- widening the tag list would "
        "pull the file IN, not let it out. To widen the scope instead, edit the "
        "TARGETS table in scripts/tsan-gate.sh; this check reads its tags from "
        "there and needs no edit of its own.\n"
        "The rule lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

string(REPLACE ";" "],[" renderedTags "[${FastCachedTsanScopeTags}]")
list(LENGTH FastCachedTsanScopeDirs scopeDirCount)
message("tsan scope: ${scannedCount} test file(s) across ${scopeDirCount} "
        "directory/directories are selected by ${renderedTags}")
