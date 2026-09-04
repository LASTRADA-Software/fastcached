# SPDX-License-Identifier: Apache-2.0
#
# The cluster pre-shared key has exactly one signing construction, and this is
# what makes that a fact rather than a convention.
#
# `Cluster/ClusterSigning.hpp` takes the domain label as a required
# `SigningDomain` parameter, so nothing that goes THROUGH the seam can omit one.
# That is two thirds of #402 and it does nothing at all about the third: a fourth
# signer can call `HmacSha256` directly and produce an unlabelled tag under the
# same key, which is precisely the state discovery was in before that ticket --
# a message whose separation from the lease token was a coincidence of field
# arity rather than a property of either construction.
#
# There is no compile-time guard available for that. `HmacSha256` is a public
# function in `Core/`, a new caller is an ordinary call, and nothing in the build
# would say a word. So the guard is a scan: the primitive may be NAMED in the
# file that declares it, the file that implements it, and the one seam that wraps
# it. Anywhere else in `src/` is a second construction, and the fix is to add a
# row to `SigningDomainTable` and go through `SignFields`.
#
# Runs as `cmake -P`, for the reason check-net-boundary.cmake states: this
# compares strings and reports, so a .sh + .ps1 pair would be two implementations
# of one rule differing only in syntax, and cmake is the one tool guaranteed
# present. Its registration therefore carries `FAIL_REGULAR_EXPRESSION`:
# `message(FATAL_ERROR)` prints `CMake Error` and exits **0** on CMake 3.28, this
# project's declared minimum, so the exit code cannot be the verdict.
#
# **It fails when its own scan matches nothing.** A check that counted zero
# callers and reported success would pass vacuously the day somebody renames the
# function, moves it, or changes its spelling -- and it would go on passing
# forever, which is worse than not having it. Each allowed row must be seen to
# match, and the total across the tree must be non-zero. Absence of the negative
# is not the positive.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-psk-signing-seam.cmake
#
# Exit codes: 0 = one construction. 1 = a second one has appeared. (But read the
# OUTPUT, not the code -- see above.)

# ---------------------------------------------------------------------------
# What the primitive is called, and what counts as naming it.
#
# The paren is load-bearing: it matches a call and a declaration and does NOT
# match prose, so a comment or a rule document that mentions `HmacSha256` in
# backticks is not a violation. A check that failed on the word would make the
# reasoning unwritable, which is the reliable way to get a guard deleted.

cmake_minimum_required(VERSION 3.28)

set(FastCachedPskPrimitive "HmacSha256")
set(FastCachedPskCallRegex "HmacSha256[ \t]*\\(")

# ---------------------------------------------------------------------------
# Where the primitive may be named. One row per file:
#
#   <path under src/>|<why this file may name it>
#
# Adding a row here adds a signing construction to the cluster key, which is the
# decision #402 exists to make deliberate. It is meant to be argued in review,
# not reached by writing a call.
#
# No row may contain a ';' -- these are CMake lists, and a semicolon inside a row
# would split it into two.
set(FastCachedPskSigners
    "FastCache/Core/Sha256.hpp|Declares it. The primitive has to live somewhere, and Core/ is where the algorithm is -- implemented in-tree because FASTCACHED_ENABLE_TLS is off by default and a cluster that could only authenticate its members when OpenSSL happened to be compiled in would accept anybody in the common configuration."
    "FastCache/Core/Sha256.cpp|Implements it, against FIPS 180-4 and RFC 4231 vectors."
    "FastCache/Cluster/ClusterSigning.hpp|The one construction. SignFields folds the domain label in ahead of every field and VerifyFields is the only comparison exposed, so a caller can neither omit a label nor reach for a non-constant-time ==."
)

# ---------------------------------------------------------------------------
# Test sources are out of scope, and deliberately so rather than by omission.
# `Core/Sha256_test.cpp` spells the primitive to check it against the published
# vectors, and `Cluster/ClusterSigning_test.cpp` spells it to pin the seam's
# message shape as BYTES -- writing out `HmacSha256(WireFields::Encode({label,
# fields...}))` the long way, so the construction is asserted against something
# other than the code that produces it. Both are the check working, not a hole:
# what this rule is about is what SIGNS on the wire.
set(FastCachedPskTestRegex "_test[.]cpp$")

# Which files count as source. Wider than the extensions this tree happens to use,
# for check-net-boundary.cmake's reason: a file this does not scan is a hole that
# reports green.
set(FastCachedPskSourceGlobs
    "*.hpp" "*.h" "*.hh" "*.hxx" "*.inl" "*.ipp"
    "*.cpp" "*.cc" "*.cxx" "*.hpp.in" "*.h.in"
)

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

set(sourceRoot "${FASTCACHED_SOURCE_DIR}/src")
if(NOT IS_DIRECTORY "${sourceRoot}")
    message(FATAL_ERROR "'${sourceRoot}' is not a directory. Is FASTCACHED_SOURCE_DIR the source root?")
endif()

# Split a "path|reason" table into two parallel lists, so the reason can be
# printed beside the rule it explains rather than being a comment nobody reads.
#
# Copied verbatim from the sibling checks rather than varied, and that is the
# point: consolidating these into a shared module is #495, deliberately not
# pre-empted here, and #495's validation compares the copies as TEXT. A copy that
# rewrote an escape or a spelling would be equivalent and non-identical, which is
# exactly the divergence that consolidation cannot detect. Keep this byte-for-byte
# with `check-net-boundary.cmake`, and count this file in when #495 lands.
# @param rows Name of the list variable holding the rows.
# @param pathsOut Set to the paths.
# @param reasonsOut Set to the reasons, in the same order.
function(fastcached_split_rows rows pathsOut reasonsOut)
    set(paths "")
    set(reasons "")
    foreach(row IN LISTS ${rows})
        string(FIND "${row}" "|" separator)
        if(separator EQUAL -1)
            message(FATAL_ERROR "Malformed row (no '|'): ${row}")
        endif()
        string(SUBSTRING "${row}" 0 ${separator} rowPath)
        math(EXPR reasonStart "${separator} + 1")
        string(SUBSTRING "${row}" ${reasonStart} -1 rowReason)
        list(APPEND paths "${rowPath}")
        list(APPEND reasons "${rowReason}")
    endforeach()
    set(${pathsOut} "${paths}" PARENT_SCOPE)
    set(${reasonsOut} "${reasons}" PARENT_SCOPE)
endfunction()

# Turn a list of shell globs into one anchored regex, so the tree can be walked
# ONCE and the results filtered in memory. `file(GLOB_RECURSE var a b c)`
# traverses once PER PATTERN, and one traversal of `src/` costs 2.09 s on DrvFs
# (#502).
#
# Verbatim from the sibling checks; consolidating them into a shared module is
# #495, deliberately not pre-empted here. See `fastcached_split_rows` above for
# why the copy must stay byte-identical rather than merely equivalent.
# @param globs The shell globs, each like `*.hpp` or `*.hpp.in`.
# @param outVar Set to an anchored alternation regex.
function(fastcached_globs_to_regex globs outVar)
    set(parts "")
    foreach(glob IN LISTS globs)
        string(REPLACE "." "PLACEHOLDERDOT" one "${glob}")
        string(REPLACE "*" ".*" one "${one}")
        string(REPLACE "PLACEHOLDERDOT" "\\." one "${one}")
        list(APPEND parts "${one}")
    endforeach()
    string(REPLACE ";" "|" joined "${parts}")
    set(${outVar} "(${joined})$" PARENT_SCOPE)
endfunction()

fastcached_split_rows(FastCachedPskSigners allowedPaths allowedReasons)
fastcached_globs_to_regex("${FastCachedPskSourceGlobs}" sourceRegex)

# ---------------------------------------------------------------------------
# ONE traversal, filtered afterwards.
file(GLOB_RECURSE treeAll LIST_DIRECTORIES false "${sourceRoot}/*")
set(sources ${treeAll})
list(FILTER sources INCLUDE REGEX "${sourceRegex}")

# `list(LENGTH)` rather than `if(sources STREQUAL "")`, and this is the exact
# trap the check exists to avoid rather than a style preference. The cause is the
# COPY on the line above, not the glob: `file(GLOB)` matching nothing DEFINES an
# empty list and compares correctly, while `set(sources ${treeAll})` from an empty
# list leaves `sources` UNDEFINED -- and CMake's `if()` then compares an undefined
# name against the literal string `sources`, which is never empty. So the guard
# against scanning nothing was itself silently false in the one case it is for,
# and the check fell through to a later, vaguer refusal. Measured, not reasoned:
# a synthetic tree holding no source reached the wrong message.
#
# The distinction is worth stating because the obvious next move is to go looking
# for `if(x STREQUAL "")` elsewhere in scripts/ and call each one a bug. Most are
# reading a glob directly and are correct. `list(LENGTH)` is still the better
# spelling everywhere, because it does not depend on the reader knowing which of
# the two mechanisms produced the empty value -- but that is robustness, not a
# defect, and a claim that something else is broken needs its own measurement.
list(LENGTH sources scannedCount)
if(scannedCount EQUAL 0)
    message(FATAL_ERROR
        "This check walked '${sourceRoot}' and found no source file it knows how to read. "
        "That is the check being broken, not the tree being clean -- and it would report "
        "success on every run from here on. Fix the glob table in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

# Which allowed rows were actually seen, so a stale row cannot sit in the table
# vouching for a file that no longer signs anything.
# Left unset rather than initialised to `""`, which CMake reads as a one-element
# list holding an empty string: `list(APPEND)` onto it produces a leading empty
# element and every joined report starts with a stray separator.
set(seenAllowed)
set(violations)

# Counted apart rather than summed, because they answer different questions and a
# total answers neither. `Core/Sha256_test.cpp` alone spells the primitive a dozen
# times against the RFC 4231 vectors, so a combined figure would stay comfortably
# non-zero with every production signer gone -- which is the exact thing the
# emptiness guard below exists to notice.
set(signerCalls 0)
set(testCalls 0)


foreach(source IN LISTS sources)
    file(RELATIVE_PATH relativeSource "${sourceRoot}" "${source}")

    # A whole-file prefilter before any per-line work: essentially every file in
    # this tree contains none of this, and splitting them all into lines cost a
    # default-set check seconds on every platform (#492's measurement, same shape).
    file(READ "${source}" content)
    string(FIND "${content}" "${FastCachedPskPrimitive}" primitiveAt)
    if(primitiveAt EQUAL -1)
        continue()
    endif()

    # Read whole and split by hand, never `file(STRINGS)`: that call returns a CMake
    # LIST, and an UNBALANCED `[` or `]` on a KEPT line merges that element with the
    # ones after it, so everything past the bracket vanishes from the scan. It takes
    # a PATH, so there is nothing to neutralise beforehand.
    #
    # Blanking the brackets is safe HERE and is not safe everywhere: this reader
    # matches `HmacSha256[ \t]*\(`, which contains none. Where brackets are the data --
    # `check-tsan-scope`, whose rows are Catch2 tags like `[async]` -- the remedy is
    # a list-free offset walk instead, and blanking them makes that check refuse on a
    # good tree. Measured, in this branch.
    #
    # Fifth-and-counting copy of this idiom; consolidating them is
    # [#495](https://github.com/LASTRADA-Software/fastcached/issues/495) and is
    # deliberately not done here.
    #
    # MEASURED, and this one is SILENT rather than loud: injecting one `]`
    # into a comment on the FIRST of two matching lines in
    # `Cluster/DiscoveryWire_test.cpp` took the reported count from 15 calls
    # in test sources to 14, while the check still PASSED. A call to the PSK
    # signing seam simply stopped being counted. Injecting on a file with only
    # ONE matching line changes nothing, because there is no second element to
    # merge with -- which is why exposure is a property of (reader, file,
    # surviving lines) and never of the script.
    file(READ "${source}" namingText)
    string(REPLACE "\\" " " namingText "${namingText}")
    string(REPLACE ";" " " namingText "${namingText}")
    string(REPLACE "[" " " namingText "${namingText}")
    string(REPLACE "]" " " namingText "${namingText}")
    string(REGEX REPLACE "\r?\n" ";" namingLines "${namingText}")
    list(FILTER namingLines INCLUDE REGEX "${FastCachedPskCallRegex}")
    if(namingLines STREQUAL "")
        # The name appears only in prose here. Not a violation, and not a match.
        continue()
    endif()

    list(LENGTH namingLines namingCount)

    list(FIND allowedPaths "${relativeSource}" allowedPosition)
    if(NOT allowedPosition EQUAL -1)
        list(APPEND seenAllowed "${relativeSource}")
        math(EXPR signerCalls "${signerCalls} + ${namingCount}")
        continue()
    endif()

    # Tests may spell the primitive; see the table above.
    if(relativeSource MATCHES "${FastCachedPskTestRegex}")
        math(EXPR testCalls "${testCalls} + ${namingCount}")
        continue()
    endif()

    list(JOIN namingLines "\n        " namingText)
    list(APPEND violations "  ${relativeSource}\n      calls the MAC primitive directly:\n        ${namingText}")
endforeach()

# ---------------------------------------------------------------------------
# The vacuous-pass guards. Both directions, because they fail differently: a
# total of zero is the function having been renamed out from under this check,
# and a row that never matched is a table entry describing a file that has
# stopped doing what the row vouches for.
#
# The stale rows are the set difference, so they are written as one:
# `REMOVE_ITEM` on an empty `seenAllowed` is a no-op rather than an error, which
# is the case that matters -- nothing matched at all.
set(staleRows ${allowedPaths})
if(seenAllowed)
    list(REMOVE_ITEM staleRows ${seenAllowed})
endif()

if(signerCalls EQUAL 0)
    message(FATAL_ERROR
        "This check scanned the tree for calls to ${FastCachedPskPrimitive}() and found NONE in "
        "any of the files allowed to make them -- including the one that declares it. That is not "
        "a clean tree, it is a check that has stopped looking at anything, and it would report "
        "success on every run from here on. The primitive has most likely been renamed or moved; "
        "update ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH staleRows staleCount)
if(staleCount GREATER 0)
    list(JOIN staleRows "\n  " staleReport)
    message(FATAL_ERROR
        "File(s) named in the signer table no longer call ${FastCachedPskPrimitive}():\n  "
        "${staleReport}\n\n"
        "A row that matches nothing vouches for a file that has stopped signing, and leaves the "
        "table looking complete while covering less than it says. If the seam moved, move the "
        "row; if a signer is gone, delete its row. The table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH violations violationCount)
if(violationCount GREATER 0)
    list(JOIN violations "\n" violationReport)

    set(rulebook "")
    set(index 0)
    foreach(allowed IN LISTS allowedPaths)
        list(GET allowedReasons ${index} reason)
        string(APPEND rulebook "  ${allowed}\n      ${reason}\n")
        math(EXPR index "${index} + 1")
    endforeach()

    message(FATAL_ERROR
        "A second MAC construction has appeared under the cluster's pre-shared key:\n"
        "${violationReport}\n\n"
        "That key already signs a discovery proof and a lease token, and one key serving two "
        "hand-rolled constructions is how a tag produced for one purpose comes to be accepted "
        "for the other (#402). Only these may name it:\n\n"
        "${rulebook}\n"
        "Sign through FastCache::Cluster::SignFields instead: add a row to SigningDomainTable "
        "in src/FastCache/Cluster/ClusterSigning.hpp giving the new construction a domain label "
        "of its own, and pass that domain. The label is a required parameter precisely so this "
        "cannot be skipped, and the table's static_asserts refuse an empty or duplicated one.\n"
        "The signer table lives in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH allowedPaths signerCount)
message("psk signing seam: ${scannedCount} source(s) scanned; ${signerCalls} call(s) to "
        "${FastCachedPskPrimitive}() in the ${signerCount} file(s) allowed to make them, "
        "${testCalls} in test sources, none anywhere else")
