# SPDX-License-Identifier: Apache-2.0
#
# The vendored vocabulary stays in vendor/: nothing under `src/` may reach `<coro/...>` or
# `<platform/Clock.hpp>`, by any include, directly or through a vendored header.
#
# Building endo's `runtime/` (#1374) put a second coroutine library and a second clock into
# the link: `coro::Task`, `coro::Cancellation` and `endo::platform::IClock`, beside
# `FastCache::Async::Task`, `Async::Cancellation` and `FastCache::IClock`. That duplicate is
# accepted, because it is the price of keeping vendor/ verbatim. `vendor/VENDOR.md` states
# the rule that makes it safe: the two vocabularies meet in ONE adapter layer, and
# first-party code is written in the first-party one.
#
# The build cannot say that (#1377). The vendored include root is `endo/`, exposed PUBLIC,
# and it has to carry `coro/` and `platform/` beside `tui/`. So anything linking the TUI can
# include the second vocabulary and it compiles cleanly. The leak shows up as two task types
# in one call stack, and nothing reports it.
#
# ## Why the closure, and not only the spelling
#
# A scan for the spelling `<coro/...>` misses the likeliest real violation. A file that
# wants the runtime includes `<tui/runtime/TuiRuntime.hpp>`, which includes `<coro/Task.hpp>`
# itself, and `coro::Task` is then in scope with no forbidden spelling anywhere under src/.
# So every include that RESOLVES into vendor/endo is followed through the vendored headers,
# and a first-party file is refused for what it REACHES. The route is printed, because
# "Foo.cpp reaches coro/Task.hpp" with no route is a finding nobody can act on.
#
# Resolution is the compiler's, narrowed to the one include root that matters. A quoted
# include is tried beside the including file first, then under vendor/endo. An angled one is
# tried under vendor/endo only. Anything resolving nowhere there (the standard library,
# libunicode, `<FastCache/...>`) names nothing in vendor/ and is not followed.
#
# ## What the spelling test still catches
#
# A forbidden SPELLING is refused whether or not it resolves, so `<coro/WhenAll.hpp>`, which
# is not vendored, is refused rather than waved through as naming no file. Backslashes are
# read as the path separators MSVC takes them for, and `..` and `//` are normalised away
# before matching. So `<tui/../coro/Task.hpp>` and `<coro\Task.hpp>` are the same include as
# `<coro/Task.hpp>`.
#
# Every `#include` line is read regardless of the preprocessor. One inside `#if _WIN32` ships
# uncompiled on every leg but one, and one inside a block comment is refused too. Both fail
# closed, which is the direction a boundary check may err in.
#
# Runs as `cmake -P`, following `check-net-boundary.cmake`'s idiom.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-vendor-vocabulary.cmake
#
# The verdict is `CMake Error` in the output, never the exit code
# (`FAIL_REGULAR_EXPRESSION`; see `check-script-check-signals.cmake`).

cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# The ONE legitimate crossing. One row per first-party file allowed to reach the vendored
# vocabulary:
#
#   <path from the source root>|<why this file is the adapter layer>
#
# The adapter layer is the production dashboard event source,
# `src/apps/fastcache-cli/TerminalEvents.*` (#1372). A row for it goes here, with its
# reason, on the day it reaches `coro/` or `platform/Clock.hpp`, and not before: a row for a
# file that reaches neither is refused as stale below. So an exemption cannot be written
# ahead of its reason, and cannot outlive it either.
#
# No row may contain a `;`: these are CMake lists.
set(FastCachedVendorVocabularyExemptions
)

# The positive control. Each row is a vendored header whose closure is KNOWN to reach the
# vocabulary. It is walked by the same reader, resolver and matcher as src/, and the walk
# matches the RESOLVED path rather than the spelling, so the resolver is part of what the
# control proves. A scan of src/ that finds nothing is a finding only once this has been seen
# to find something, because a broken include pattern and a clean tree print the same zero.
#
#   <path under vendor/endo>|<why it must reach the vocabulary>
set(FastCachedVendorVocabularyControls
    "tui/runtime/TuiRuntime.hpp|The runtime's entry point: blockOn and pumpOnce are expressed in coro::Task, so it includes <coro/Task.hpp> itself. If this stops reaching coro/, either upstream moved the runtime off coro and the rule wants re-deriving, or this reader has stopped seeing includes."
)
# @tables-end -- check-vendor-vocabulary-selftest.cmake inserts its own tables below this line.

# ---------------------------------------------------------------------------
# Which files count as source. Wider than this tree uses today, for net-boundary's reason: a
# file this does not scan is a hole that reports green, and `.hpp.in` is a live convention
# here (`Core/Version.hpp.in`). One anchored regex, so src/ is walked once.
set(FastCachedVendorVocabularySourceRegex "[.](hpp|h|hh|hxx|inl|ipp|cpp|cc|cxx|hpp[.]in|h[.]in)$")

# The vocabulary, as RESOLVED paths under vendor/endo and as normalised spellings. Two
# patterns because a spelling can carry a leading directory (`../../vendor/endo/coro/...`)
# while a resolved path cannot. Case-sensitive: `platform/` is endo's, `Platform/` is ours.
set(FastCachedVendorVocabularyResolved "^(coro/|platform/Clock[.]hpp$)")
set(FastCachedVendorVocabularySpelling "(^|/)(coro/|platform/Clock[.]hpp$)")

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

set(sourceRoot "${FASTCACHED_SOURCE_DIR}/src")
set(vendorRoot "${FASTCACHED_SOURCE_DIR}/vendor/endo")
if(NOT IS_DIRECTORY "${sourceRoot}")
    message(FATAL_ERROR "'${sourceRoot}' is not a directory. Is FASTCACHED_SOURCE_DIR the source root?")
endif()
# Refused rather than skipped. Without vendor/endo the control cannot run, and a src/ scan
# with no control behind it reports a zero that means nothing.
if(NOT IS_DIRECTORY "${vendorRoot}")
    message(FATAL_ERROR
        "'${vendorRoot}' is not a directory, so the positive control cannot run and a clean "
        "result for src/ would mean nothing. Is FASTCACHED_SOURCE_DIR the source root?")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

fastcached_split_rows(FastCachedVendorVocabularyExemptions exemptPaths exemptReasons)
fastcached_split_rows(FastCachedVendorVocabularyControls controlPaths controlReasons)

# The names at the top of vendor/endo. An include from src/ can land under vendor/endo only by
# starting with one of them (the include root) or by spelling a path through `vendor/endo/`
# (beside the includer), so every other include -- `<vector>`, `<FastCache/...>`, a sibling
# header -- is decided without asking the filesystem anything.
#
# That is a measured cost, not tidiness. Resolving every include by `EXISTS` took 16-19 s for
# 774 sources under WSL2 over /mnt/d, where each stat crosses 9p, and 3.6-5.8 s with this
# filter (three runs, same tree, an idle host). Natively on Windows the unfiltered scan was
# 1.1 s, which is why the cost is invisible there.
file(GLOB vendorTopLevel RELATIVE "${vendorRoot}" LIST_DIRECTORIES true "${vendorRoot}/*")

# Every include in one file, as `A|<target>` (angled) or `Q|<target>` (quoted), the target
# normalised: backslashes read as `/`, then `..` and `//` collapsed.
#
# Read whole and split through `fastcached_split_lines_tokenised`, never `file(STRINGS)`,
# whose list parser merges elements across an unbalanced bracket and drops every include
# after it (net-boundary measured that as a green run over a real violation). The backslash
# is mapped to `/` BEFORE that split, because the splitter blanks backslashes and
# `<coro\Task.hpp>` would otherwise read as `<coro Task.hpp>` and match nothing.
#
# @param filePath File to read.
# @param includesOut Set to the list of `A|target` / `Q|target` entries, in file order.
function(fastcached_vocabulary_includes filePath includesOut)
    file(READ "${filePath}" content)
    string(REPLACE "\\" "/" content "${content}")
    fastcached_split_lines_tokenised("${content}" lines)
    list(FILTER lines INCLUDE REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]")
    set(includes "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*#[ \t]*include[ \t]*<([^>]+)>")
            set(kind "A")
        elseif(line MATCHES "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\"")
            set(kind "Q")
        else()
            continue()
        endif()
        cmake_path(SET target NORMALIZE "${CMAKE_MATCH_1}")
        list(APPEND includes "${kind}|${target}")
    endforeach()
    set(${includesOut} "${includes}" PARENT_SCOPE)
endfunction()

# Where an include lands in vendor/endo, as a path relative to it, or empty when it does not
# land there. A quoted include is tried beside `fromDir` first; a candidate that exists
# OUTSIDE vendor/endo is a first-party file, which the compiler takes before any include
# root, so it answers empty rather than falling through to vendor/.
#
# @param kind `A` or `Q`, from fastcached_vocabulary_includes.
# @param target The normalised include target.
# @param fromDir Absolute directory of the including file.
# @param resolvedOut Set to the path under vendor/endo, or to empty.
function(fastcached_vocabulary_resolve kind target fromDir resolvedOut)
    set(${resolvedOut} "" PARENT_SCOPE)
    if(kind STREQUAL "Q")
        cmake_path(APPEND fromDir "${target}" OUTPUT_VARIABLE beside)
        cmake_path(NORMAL_PATH beside)
        if(EXISTS "${beside}" AND NOT IS_DIRECTORY "${beside}")
            cmake_path(IS_PREFIX vendorRoot "${beside}" NORMALIZE insideVendor)
            if(insideVendor)
                cmake_path(RELATIVE_PATH beside BASE_DIRECTORY "${vendorRoot}" OUTPUT_VARIABLE relative)
                set(${resolvedOut} "${relative}" PARENT_SCOPE)
            endif()
            return()
        endif()
    endif()
    if(target MATCHES "^[.][.](/|$)" OR IS_ABSOLUTE "${target}")
        return()
    endif()
    if(EXISTS "${vendorRoot}/${target}" AND NOT IS_DIRECTORY "${vendorRoot}/${target}")
        set(${resolvedOut} "${target}" PARENT_SCOPE)
    endif()
endfunction()

# The shortest route from a vendored header to the vocabulary, as `a -> b -> coro/Task.hpp`,
# or empty when its closure reaches none of it. Breadth-first, so the route printed is the
# shortest. Memoised per header in a GLOBAL property, since a `cmake -P` script has no other
# scope that outlives a function call and many first-party files include the same header.
#
# @param entry A path under vendor/endo.
# @param routeOut Set to the route, or to empty.
function(fastcached_vocabulary_route entry routeOut)
    # A digest rather than MAKE_C_IDENTIFIER, which maps `a_b.hpp` and `a/b.hpp` to one name.
    string(MD5 entryId "${entry}")
    get_property(memoised GLOBAL PROPERTY "FastCachedVocabularyRoute_${entryId}" SET)
    if(memoised)
        get_property(route GLOBAL PROPERTY "FastCachedVocabularyRoute_${entryId}")
        set(${routeOut} "${route}" PARENT_SCOPE)
        return()
    endif()

    set(queue "${entry}")
    set(visited "${entry}")
    set(route "")
    while(NOT "${queue}" STREQUAL "")
        list(POP_FRONT queue current)
        if(current MATCHES "${FastCachedVendorVocabularyResolved}")
            set(route "${current}")
            string(MD5 stepId "${current}")
            while(DEFINED parent_${stepId})
                set(step "${parent_${stepId}}")
                set(route "${step} -> ${route}")
                string(MD5 stepId "${step}")
            endwhile()
            break()
        endif()
        cmake_path(GET current PARENT_PATH currentDir)
        fastcached_vocabulary_includes("${vendorRoot}/${current}" currentIncludes)
        foreach(include IN LISTS currentIncludes)
            string(SUBSTRING "${include}" 0 1 kind)
            string(SUBSTRING "${include}" 2 -1 target)
            fastcached_vocabulary_resolve("${kind}" "${target}" "${vendorRoot}/${currentDir}" next)
            if("${next}" STREQUAL "")
                continue()
            endif()
            list(FIND visited "${next}" seen)
            if(seen EQUAL -1)
                list(APPEND visited "${next}")
                list(APPEND queue "${next}")
                string(MD5 nextId "${next}")
                set("parent_${nextId}" "${current}")
            endif()
        endforeach()
    endwhile()

    set_property(GLOBAL PROPERTY "FastCachedVocabularyRoute_${entryId}" "${route}")
    set(${routeOut} "${route}" PARENT_SCOPE)
endfunction()

# What one file reaches: one line per include that is a forbidden spelling or resolves into
# a vendored header whose closure reaches the vocabulary. Empty when it reaches none of it.
#
# @param filePath Absolute path of the file.
# @param reachesOut Set to the list of findings.
# @param entriesOut Set to how many of its includes resolve into vendor/endo at all, so the
#        summary can say whether a clean src/ was clean because nothing includes the TUI.
function(fastcached_vocabulary_reaches filePath reachesOut entriesOut)
    cmake_path(GET filePath PARENT_PATH fileDir)
    fastcached_vocabulary_includes("${filePath}" fileIncludes)
    set(reaches "")
    set(entries 0)
    foreach(include IN LISTS fileIncludes)
        string(SUBSTRING "${include}" 0 1 kind)
        string(SUBSTRING "${include}" 2 -1 target)
        if(kind STREQUAL "A")
            set(spelled "<${target}>")
        else()
            set(spelled "\"${target}\"")
        endif()
        if(target MATCHES "${FastCachedVendorVocabularySpelling}")
            list(APPEND reaches "includes ${spelled}")
            continue()
        endif()
        string(REGEX MATCH "^[^/]+" firstSegment "${target}")
        if(NOT firstSegment IN_LIST vendorTopLevel AND NOT target MATCHES "(^|/)vendor/endo/")
            continue()
        endif()
        fastcached_vocabulary_resolve("${kind}" "${target}" "${fileDir}" resolved)
        if("${resolved}" STREQUAL "")
            continue()
        endif()
        math(EXPR entries "${entries} + 1")
        fastcached_vocabulary_route("${resolved}" route)
        if(NOT "${route}" STREQUAL "")
            list(APPEND reaches "includes ${spelled}, which reaches the vocabulary: ${route}")
        endif()
    endforeach()
    set(${reachesOut} "${reaches}" PARENT_SCOPE)
    set(${entriesOut} ${entries} PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Half one: the control. Each row must be SEEN to reach the vocabulary.
set(controlFailures "")
set(controlRoutes "")
foreach(control IN LISTS controlPaths)
    if(NOT EXISTS "${vendorRoot}/${control}")
        list(APPEND controlFailures "  vendor/endo/${control}\n      is named in the control table but does not exist")
        continue()
    endif()
    fastcached_vocabulary_route("${control}" route)
    if("${route}" STREQUAL "")
        list(APPEND controlFailures "  vendor/endo/${control}\n      is named in the control table and was NOT seen to reach coro/ or platform/Clock.hpp")
    else()
        list(APPEND controlRoutes "${route}")
    endif()
endforeach()
# A control table with no rows proves nothing, which is the failure it exists to rule out.
if("${controlPaths}" STREQUAL "")
    list(APPEND controlFailures "  the control table is empty, so nothing has been seen to find anything")
endif()

# ---------------------------------------------------------------------------
# Half two: src/, all of it, tests included.
#
# Tests are in scope where net-boundary exempts them, and the difference is the rule's
# subject. net-boundary protects what gets LIFTED, and a test is not lifted. This one protects
# which vocabulary first-party code is WRITTEN in, and a test is first-party code: a fixture
# driving `coro::Task` against a `FastCache::Async` subject is exactly the two-task-types call
# stack the rule exists against. An adapter's own test that must reach the runtime gets a row,
# as the adapter does.
file(GLOB_RECURSE sourceAll LIST_DIRECTORIES false "${sourceRoot}/*")
set(sources ${sourceAll})
list(FILTER sources INCLUDE REGEX "${FastCachedVendorVocabularySourceRegex}")

set(violations "")
set(staleExemptions "")
set(exemptReached "")
set(scannedCount 0)
set(vendorEntryCount 0)
# QUOTED: `set(sources ${empty})` UNSETS the variable, and an unquoted `if(sources STREQUAL "")`
# then compares the literal name. net-boundary's `vacuous` case caught exactly that.
if("${sources}" STREQUAL "")
    list(APPEND violations "  src/\n      holds no source this check knows how to read, so it examined nothing")
endif()

foreach(source IN LISTS sources)
    math(EXPR scannedCount "${scannedCount} + 1")
    file(RELATIVE_PATH relativeSource "${FASTCACHED_SOURCE_DIR}" "${source}")
    fastcached_vocabulary_reaches("${source}" reaches entries)
    math(EXPR vendorEntryCount "${vendorEntryCount} + ${entries}")
    list(FIND exemptPaths "${relativeSource}" exemptPosition)
    if(NOT exemptPosition EQUAL -1)
        if("${reaches}" STREQUAL "")
            list(APPEND staleExemptions
                "  ${relativeSource}\n      is exempted and no longer reaches coro/ or platform/Clock.hpp -- delete its row")
        else()
            list(APPEND exemptReached "${relativeSource}")
        endif()
        continue()
    endif()
    foreach(reach IN LISTS reaches)
        list(APPEND violations "  ${relativeSource}\n      ${reach}")
    endforeach()
endforeach()

foreach(exempt IN LISTS exemptPaths)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${exempt}")
        list(APPEND staleExemptions "  ${exempt}\n      is exempted and does not exist -- delete or correct its row")
    elseif(NOT exempt MATCHES "${FastCachedVendorVocabularySourceRegex}" OR NOT exempt MATCHES "^src/")
        list(APPEND staleExemptions "  ${exempt}\n      is exempted but is not a source under src/ this check reads, so the row exempts nothing")
    endif()
endforeach()

# ---------------------------------------------------------------------------
if(NOT "${controlFailures}" STREQUAL "" OR NOT "${violations}" STREQUAL "" OR NOT "${staleExemptions}" STREQUAL "")
    set(report "")
    if(NOT "${controlFailures}" STREQUAL "")
        list(JOIN controlFailures "\n" controlReport)
        string(APPEND report
            "The positive control did not find what it must, so no verdict about src/ below means anything:\n"
            "${controlReport}\n")
    endif()
    if(NOT "${violations}" STREQUAL "")
        list(JOIN violations "\n" violationReport)
        string(APPEND report "First-party source reaches the vendored vocabulary:\n${violationReport}\n")
    endif()
    if(NOT "${staleExemptions}" STREQUAL "")
        list(JOIN staleExemptions "\n" staleReport)
        string(APPEND report "Exemption row(s) no longer describe anything:\n${staleReport}\n")
    endif()

    set(exemptionBook "  (none)\n")
    if(NOT "${exemptPaths}" STREQUAL "")
        set(exemptionBook "")
        set(index 0)
        foreach(exempt IN LISTS exemptPaths)
            list(GET exemptReasons ${index} reason)
            string(APPEND exemptionBook "  ${exempt}\n      ${reason}\n")
            math(EXPR index "${index} + 1")
        endforeach()
    endif()

    message(FATAL_ERROR
        "${report}\n"
        "vendor/endo carries a second coroutine library and a second clock so that the vendored "
        "TUI compiles unmodified. First-party code is written in the first-party vocabulary -- "
        "FastCache::Async::Task, Async::Cancellation, FastCache::IClock -- and reaches the TUI "
        "through ONE adapter layer, the production dashboard event source "
        "(src/apps/fastcache-cli/TerminalEvents.*, #1372). So nothing under src/ may reach "
        "<coro/...> or <platform/Clock.hpp>, by spelling or through a vendored header.\n\n"
        "Fix it by moving the use into the adapter and widening the adapter's interface, "
        "rather than adding a second crossing. Add an exemption row only for a file that IS "
        "that adapter layer, or is its own test, with a reason saying why. The exemptions today:\n"
        "${exemptionBook}\n"
        "What this rule does NOT cover, so it is not over-applied:\n"
        "  * <tui/...> includes are fine. Only the ones whose closure reaches coro/ or "
        "platform/Clock.hpp are refused, and the route above names which header does.\n"
        "  * endo's other platform headers -- <platform/SignalHandler.hpp>, "
        "<platform/Types.hpp>, <platform/Wakeup.hpp> -- are not the vocabulary and are not refused.\n"
        "  * <FastCache/Core/Clock.hpp> and <FastCache/Async/Task.hpp> ARE the first-party "
        "vocabulary. Do not delete them to satisfy this check.\n"
        "  * vendor/ itself is not scanned: the vendored TUI is written in the vendored "
        "vocabulary, and vendor/ stays verbatim.\n"
        "The tables live in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH controlRoutes controlCount)
list(JOIN controlRoutes ", " controlText)
list(LENGTH exemptReached exemptCount)
message("vendor vocabulary: ${scannedCount} source(s) under src/, ${vendorEntryCount} include(s) "
        "resolving into vendor/endo, none reaching coro/ or platform/Clock.hpp outside "
        "${exemptCount} exemption(s), control: ${controlCount} vendored header(s) seen to reach "
        "it (${controlText})")
