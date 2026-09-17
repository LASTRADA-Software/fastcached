# SPDX-License-Identifier: Apache-2.0
#
# A test file does not define its own `IMembershipOracle`: the fakes live in
# `src/tests/MembershipFakes.hpp`.
#
# ## Why a check and not a sentence
#
# The rulebook already says it -- *"A test FAKE is a shared helper too:
# `src/tests/ScriptedSocket.hpp`. A fake nothing exercises does not report its own bugs."*
# That sentence reached the files that obey it and no file that did not: when #1497 was filed,
# six test files carried their own copy of one of two membership fakes, four of them
# byte-identical.
#
# What makes a duplicated FAKE worse than duplicated production code is the direction its bugs
# point. A fake that is wrong makes its cases pass -- it cannot make them fail -- so an
# assertion review finds nothing, a green suite says nothing, and the copy is discovered only
# when somebody happens to read it. That is precisely the population a check exists for.
#
# And the cost is not fixed: it is the number of FACTS each copy has to get right. That number
# was zero until #1471 gave the seam an attribution, and is now one -- each copy must name
# which admission route it stands for, and each is a separate chance to name the wrong one,
# which would make a case assert attribution it never established.
#
# ## What it matches
#
# Over each `*_test.cpp` with its comments removed (`fastcached_strip_comments`), so prose
# about the interface is not read as implementing it:
#
#   a `class` or `struct` whose base clause names `IMembershipOracle`, in any spelling
#   (`IMembershipOracle`, `Distributed::IMembershipOracle`, `FastCache::Distributed::...`),
#   with or without `final`, and with or without an access specifier.
#
# The base clause is bounded by `[^;{]*`, which cannot cross a `;` or a `{` -- so the match is
# the declaration and nothing after it, and a declaration WRAPPED across lines is still one
# match. That is why this reads the whole file rather than walking lines: a wrapped base clause
# is exactly what a formatter produces once a class name grows, and a line-based reader would
# fail OPEN on it.
#
# It reports the class NAME rather than a line number, deliberately: a name is what you search
# for, and it does not drift when the file above it changes.
#
# ## What it does NOT cover, said here so nobody over-applies it
#
#   * **Production oracles.** `OpenMembership`, `AnyOfMembership`, `HostSetMembership` and
#     `NodeMembership` are the real implementations and are not test files. Only `*_test.cpp`
#     is scanned.
#   * **Using the interface.** A test that takes an `IMembershipOracle const&` parameter or
#     holds a pointer to one is the ORDINARY case and is untouched -- `NodeReload_test.cpp`
#     does both, on purpose. Only a DERIVATION is refused.
#   * **A fake of a different seam.** This says nothing about `ILiveStatsSources`,
#     `IClusterAdmin` or any other interface test files stub. Each would be its own decision,
#     and its own row if one is ever wanted here.
#   * **`vendor/`**, which is another project's code.
#   * A token inside a STRING LITERAL reads as code, as in every regex-shaped reader here.
#
# ## Its residual, so the next surprise is diagnosed rather than exempted
#
#   * FAILS OPEN on a base clause holding a `{` or a `;` before the interface name -- a
#     braced initializer or a nested template argument spelled that way. No such declaration
#     exists in C++ that this check would meet; widen the bound rather than exempting a site.
#   * It cannot tell a fake that SHOULD be shared from one that genuinely belongs to one file.
#     That is what the exemption table is for, and a row has to say which.

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "membership-fakes: FASTCACHED_SOURCE_DIR must be set")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

# ---------------------------------------------------------------------------
# Where the shared fakes live. Overridable ONLY so the self-test can stage a tree; a real run
# never sets it, and the default is the claim.
if(NOT DEFINED FASTCACHED_MEMBERSHIP_FAKES)
    set(FASTCACHED_MEMBERSHIP_FAKES "src/tests/MembershipFakes.hpp")
endif()

# ---------------------------------------------------------------------------
# Sites that may keep their own fake, each with the reason it may.
#
# EMPTY today, and that is the strongest state rather than a missing feature: every fake of
# this seam is shared, so there is nothing to wave through. The table exists because the
# alternative to an escape hatch is somebody deleting the check, and because a row nobody can
# write is a rule nobody can be held to. A row is `<path>|<reason>`; the reason is a forcing
# function, so a row without one does not parse.
set(FastCachedMembershipFakeExemptions)

# The one derivation pattern. `[^;{]*` bounds the base clause, so the match is the declaration
# and a wrapped one is still one match.
set(derivationPattern
    "(class|struct)[ \t\r\n]+[A-Za-z_][A-Za-z0-9_]*[^;{]*IMembershipOracle")

# ---------------------------------------------------------------------------
# The positive anchor, asked BEFORE anything is concluded from what was not found.
#
# A scan whose pattern has stopped matching reports exactly the same clean as a tree with
# nothing to refuse, and this check's whole subject is a shape nobody noticed. So it must first
# find the shared fakes themselves: the same pattern, over the header it is pointing everybody
# at. If that comes back short, the pattern or the header moved and no verdict is available.
set(fakesPath "${FASTCACHED_SOURCE_DIR}/${FASTCACHED_MEMBERSHIP_FAKES}")
if(NOT EXISTS "${fakesPath}")
    message("")
    message("  The shared fakes header is missing: ${FASTCACHED_MEMBERSHIP_FAKES}")
    message("")
    message("This check tells every test file to use that header, so it cannot conclude anything")
    message("while the header is not there -- a clean run would be advice to include a file that")
    message("does not exist.")
    message(FATAL_ERROR "membership-fakes: the shared fakes header is missing")
endif()

file(READ "${fakesPath}" fakesWhole)
fastcached_strip_comments("${fakesWhole}" fakesCode)
string(REGEX MATCHALL "${derivationPattern}" fakesFound "${fakesCode}")
list(LENGTH fakesFound fakesCount)
if(fakesCount LESS 2)
    message("")
    message("  ${FASTCACHED_MEMBERSHIP_FAKES} defines ${fakesCount} `IMembershipOracle` implementation(s),")
    message("  and this check's own pattern is what found that number. Two are expected:")
    message("  `ListedMembership` (admits from a host list) and `FixedMembership` (one verdict).")
    message("")
    message("So either the header stopped carrying the shared fakes, or the pattern stopped")
    message("matching a declaration. Both make every result below meaningless in the same")
    message("direction -- a scan that matches nothing reports the tree is clean of something")
    message("nobody looked for -- so this is a refusal rather than a pass.")
    message(FATAL_ERROR "membership-fakes: the pattern found ${fakesCount} shared fake(s), expected 2")
endif()

# ---------------------------------------------------------------------------
# The file set. The question is this check's own -- every first-party Catch2 test source -- and
# it is asked through the shared machinery so the walk fallback, the exclusions and the
# work-tree probe are not a seventh copy.
fastcached_tracked_files("${FASTCACHED_SOURCE_DIR}"
    PATHSPECS "src"
    GLOBS "src/*_test.cpp" "src/**/*_test.cpp"
    FILTER "_test\\.cpp$"
    FILES_OUT testFiles
    MODE_OUT scanMode)

if(scanMode STREQUAL "git ls-files")
    message(STATUS "membership-fakes: mode: git-index")
else()
    message(STATUS "membership-fakes: mode: walk (${scanMode})")
endif()

list(LENGTH testFiles fileCount)
if(fileCount EQUAL 0)
    message("")
    message("  No `*_test.cpp` was enumerated under src/ via ${scanMode}.")
    message("")
    message("This tree has hundreds, so that is an enumeration that broke rather than a tree with")
    message("no tests. A scan selecting NOTHING is a refusal.")
    message(FATAL_ERROR "membership-fakes: the scan enumerated no test file")
endif()

# ---------------------------------------------------------------------------
# The exemption rows, parsed before the scan so a malformed row fails loudly rather than
# silently exempting nothing.
fastcached_split_rows(FastCachedMembershipFakeExemptions exemptPaths exemptReasons)
list(LENGTH exemptPaths exemptionCount)
set(exemptionsUsed "")

# ---------------------------------------------------------------------------
# The scan.
set(violations "")
set(violationCount 0)

foreach(testFile IN LISTS testFiles)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${testFile}")
        continue()
    endif()
    file(READ "${FASTCACHED_SOURCE_DIR}/${testFile}" wholeFile)
    # Whole-file filter first: a file that never names the interface has nothing to say, and
    # stripping comments over every test source is the expensive part.
    string(FIND "${wholeFile}" "IMembershipOracle" mentionAt)
    if(mentionAt EQUAL -1)
        continue()
    endif()

    fastcached_strip_comments("${wholeFile}" code)
    string(REGEX MATCHALL "${derivationPattern}" found "${code}")
    if(NOT found)
        continue()
    endif()

    if(testFile IN_LIST exemptPaths)
        list(APPEND exemptionsUsed "${testFile}")
        continue()
    endif()

    foreach(match IN LISTS found)
        # The declared name, which is what a reader searches for.
        string(REGEX MATCH "(class|struct)[ \t\r\n]+([A-Za-z_][A-Za-z0-9_]*)" declaration "${match}")
        set(declaredName "${CMAKE_MATCH_2}")
        string(APPEND violations "\n  ${testFile}: class ${declaredName}")
        math(EXPR violationCount "${violationCount} + 1")
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# A row that no longer describes a site is refused rather than ignored: a standing exemption
# for a fake that is gone would wave the next one of that path through in silence.
set(staleRows "")
foreach(exemptPath IN LISTS exemptPaths)
    if(NOT exemptPath IN_LIST exemptionsUsed)
        string(APPEND staleRows "\n  ${exemptPath}")
    endif()
endforeach()

set(refused FALSE)

if(NOT violations STREQUAL "")
    message("")
    message("A test file defines its own `IMembershipOracle`:${violations}")
    message("")
    message("Use the shared fakes in ${FASTCACHED_MEMBERSHIP_FAKES}:")
    message("")
    message("  * `Testing::ListedMembership { { hosts... }, <participant> }` -- admits the hosts it")
    message("    lists, and `Remove(host)` stops admitting one mid-case;")
    message("  * `Testing::FixedMembership { <verdict>, <participant> }` -- answers one verdict")
    message("    whatever it is asked about, which is the only way to spell `Forgotten` (no host")
    message("    list can).")
    message("")
    message("Both take the participant as a REQUIRED argument, and that is the point rather than")
    message("ceremony: which admission route decided is a fact each CASE states (#1471), so a")
    message("shared fake may not default it. Name the route at the construction site, and keep")
    message("whatever your fixture's own reason was for using a list rather than an open oracle --")
    message("that reason is about your arrangement, not about the fake, so it stays with you.")
    message("")
    message("A fake that genuinely belongs to one file takes a row in")
    message("FastCachedMembershipFakeExemptions with the reason it does, in")
    message("${CMAKE_CURRENT_LIST_FILE}. The table is empty today, so a first row is a decision")
    message("somebody should read rather than a formality.")
    message("")
    message("Enumerated ${fileCount} test file(s) via ${scanMode}.")
    set(refused TRUE)
endif()

if(NOT staleRows STREQUAL "")
    message("")
    message("FastCachedMembershipFakeExemptions has STALE row(s) matching no fake:${staleRows}")
    message("")
    message("The fake was shared, moved or deleted. Remove the row -- a standing exemption for a")
    message("site that is gone would wave the next one of that path through silently.")
    set(refused TRUE)
endif()

if(refused)
    string(REGEX MATCHALL "\n  " staleRowMarks "${staleRows}")
    list(LENGTH staleRowMarks staleCount)
    message(FATAL_ERROR
        "membership-fakes: ${violationCount} duplicated fake(s) and ${staleCount} stale exemption row(s)")
endif()

message(STATUS
    "membership-fakes: ${fakesCount} shared fake(s) in ${FASTCACHED_MEMBERSHIP_FAKES}, no test file "
    "defines its own across ${fileCount} file(s) via ${scanMode}, ${exemptionCount} exemption row(s)")
