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
# What this deliberately does NOT check: that the tags appear only in these
# directories. Three files elsewhere carry them and are swept in as a result.
# Running MORE than the scope is harmless; running less is the defect.
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
# directory; adding a concurrency-bearing directory is adding a row here AND a
# tag to the list below, which is the point -- the two facts are checked against
# each other rather than each being trusted.
set(FastCachedTsanScopeDirs
    "src/FastCache/Async"
    "src/FastCache/Consensus"
    "src/FastCache/Distributed"
)

# ---------------------------------------------------------------------------
# The tags `scripts/tsan-gate.sh` selects on. This list and the tag expression in
# that script's TARGETS table are the same fact written twice, and this check is
# what keeps them honest: a tag added here and not there widens nothing, and a
# tag removed there and not here fails this check on the next file that uses it.
#
# Kept as bare names without brackets so the message below can print them the way
# a Catch2 tag is spelled.
set(FastCachedTsanScopeTags
    async
    consensus
    distributed
    reactor
    task
)

# ---------------------------------------------------------------------------

set(uncovered "")

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

    file(GLOB testFiles "${absoluteDir}/*_test.cpp")
    if(testFiles STREQUAL "")
        message(FATAL_ERROR
            "check-tsan-scope: ${scopeDir} contains no *_test.cpp files.\n"
            "That is either a directory that lost its tests or a glob that "
            "stopped matching; both make the sanitized scope smaller than it "
            "reads.")
    endif()

    foreach(testFile IN LISTS testFiles)
        file(READ "${testFile}" contents)
        set(covered FALSE)
        foreach(tag IN LISTS FastCachedTsanScopeTags)
            string(FIND "${contents}" "[${tag}]" tagPosition)
            if(NOT tagPosition EQUAL -1)
                set(covered TRUE)
                break()
            endif()
        endforeach()
        if(NOT covered)
            file(RELATIVE_PATH relativeFile "${FASTCACHED_SOURCE_DIR}" "${testFile}")
            list(APPEND uncovered "${relativeFile}")
        endif()
    endforeach()
endforeach()

if(NOT uncovered STREQUAL "")
    string(REPLACE ";" "]\n    [" printableTags "[${FastCachedTsanScopeTags}]")
    message("")
    message("These test files sit in a concurrency-bearing directory but carry no tag")
    message("the ThreadSanitizer gate selects on, so they are NOT run under TSan:")
    message("")
    foreach(file IN LISTS uncovered)
        message("    ${file}")
    endforeach()
    message("")
    message("Give each case one of these tags:")
    message("    ${printableTags}")
    message("")
    message("or, if it genuinely does not belong in the sanitized scope, widen")
    message("FastCachedTsanScopeTags here and the TARGETS table in")
    message("scripts/tsan-gate.sh together -- they are one fact written twice and")
    message("this check is what keeps them agreeing.")
    message("")
    message(FATAL_ERROR "check-tsan-scope: ${uncovered} not reachable by the TSan gate")
endif()
