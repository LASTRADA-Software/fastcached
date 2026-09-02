# SPDX-License-Identifier: Apache-2.0
#
# `::htonl(...)` is a syntax error on macOS and an ordinary call everywhere else.
#
# The byte-order conversions are function-like MACROS on every platform this
# project builds on -- that is not the difference. The difference is what they
# expand to:
#
#   glibc    `# define htonl(x) __bswap_32 (x)`, guarded by `#ifdef __OPTIMIZE__`.
#            The expansion is a bare global identifier, so `::__bswap_32 (x)` is
#            well-formed. Measured on glibc 2.39/x86-64: `::htonl(1u)` compiles at
#            -O0 (macro not defined) and at -O2 (macro defined and expanded).
#   Darwin   `#define htonl(x) __DARWIN_OSSwapInt32(x)`, always defined. That it
#            FAILS is measured -- CI's macOS leg reports `expected unqualified-id`
#            at the `::`, with a `note: expanded from macro 'htonl'`. WHY is an
#            inference from that error and not a reading: nobody here has a macOS
#            machine, so "the expansion is not a bare identifier, and the `::` has
#            nothing to qualify" is the best explanation of the diagnostic rather
#            than something observed. The rule below does not depend on it.
#
# The earlier explanation in this tree -- "they are macros on macOS and functions
# on glibc" -- was wrong, and wrong in the direction that matters: it suggests the
# rule is about which platform has a macro, which would make this check need to
# know about platforms and `#if` guards. It does not. `::` before these names is
# never necessary and never correct here, so the rule is flat and lexical.
#
# ---------------------------------------------------------------------------
# Why a check and not a comment.
#
# It has happened three times. The second and third were different lanes,
# different files, hours apart, each costing a CI cycle on a branch that was
# otherwise green. The rule was already written down IN THE SAME FILE, three
# lines above the fixture that violated it -- the same shape AGENT.md records for
# `node-scratch-isolation-e2e`, where a correct paragraph sat three lines above
# the wait that had stopped implementing it. A comment is what already failed
# here; see issue #469.
#
# Nothing else can catch it:
#
#   - Local builds cannot. Nobody here builds macOS locally, and Linux and
#     Windows compile it without complaint.
#   - clang-tidy cannot. The files it appears in are behind `#if defined(__APPLE__)`
#     or `#if defined(_WIN32)`, so on the analyser's Linux host they are EMPTY
#     translation units that report clean -- see issue #466.
#   - CI can, on the macOS leg, one cycle at a time, last.
#
# ---------------------------------------------------------------------------
# No exemption table, deliberately.
#
# Every other scan in this directory has one, because every other scan judges
# something that can legitimately look like a violation. This one cannot: writing
# `::` before a global byte-order call is never required and never correct, so a
# row excusing one would be a hole rather than a decision. A qualified MEMBER --
# `Foo::htonl(x)` -- is legitimate and is not matched; see the pattern below.
#
# Runs as `cmake -P` for the reason check-test-names.cmake gives: it reads files,
# compares strings and reports, so a .sh + .ps1 pair would be two implementations
# of one rule differing only in syntax, and cmake is the one tool guaranteed
# present.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-byte-order-qualifier.cmake
#
# Exit codes: 0 always -- `message(FATAL_ERROR)` exits 0 on CMake 3.28, this
# project's declared minimum. The verdict is the presence of `CMake Error` in the
# output, which is what the FAIL_REGULAR_EXPRESSION on the registration reads.

# Under CMP0007 OLD, `list()` DISCARDS empty elements -- every blank line in every
# file scanned here -- so a reported line number would be off by however many
# blank lines precede it. Guarded by `if(POLICY ...)` so a CMake that has retired
# the policy still runs this.
if(POLICY CMP0007)
    cmake_policy(SET CMP0007 NEW)
endif()

# ---------------------------------------------------------------------------
# The names. `htonll`/`ntohll` are included although this tree does not use them
# today: they are the same family from the same headers, and the cost of listing
# a name nobody writes is nothing while the cost of omitting one is a fourth
# occurrence.
#
#   <name>|<what it converts>
#
# No row may contain a ';' -- these are CMake lists, and a semicolon inside a row
# would split it into two.
set(FastCachedByteOrderNames
    "htonl|32-bit host to network."
    "htons|16-bit host to network."
    "ntohl|32-bit network to host."
    "ntohs|16-bit network to host."
    "htonll|64-bit host to network, where the platform provides it."
    "ntohll|64-bit network to host, where the platform provides it."
)

# Where to look.
#
#   <path relative to the source root>|<why this root is scanned>
set(FastCachedByteOrderScanRoots
    "src|Every first-party translation unit. The three occurrences so far were all under here -- two in Net/, one in an app test."
)

# Which files are judged. Wider than the extensions this tree uses today, because
# a file this does not scan is a hole that reports green.
set(FastCachedByteOrderScanGlobs
    "*.cpp" "*.cc" "*.cxx" "*.hpp" "*.h" "*.hh" "*.inl" "*.ipp"
)

# ---------------------------------------------------------------------------

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

if(NOT IS_DIRECTORY "${FASTCACHED_SOURCE_DIR}")
    message(FATAL_ERROR "'${FASTCACHED_SOURCE_DIR}' is not a directory. Is it the source root?")
endif()

# Split one '|'-separated row into the variables named in ARGN, the last of which
# takes whatever remains -- so only the final field may contain a '|', which is
# what lets a reason be written in ordinary prose.
#
# @param row The '|'-separated row.
# @param ARGN Output variable names, in field order.
function(fastcached_row_fields row)
    list(LENGTH ARGN fieldCount)
    math(EXPR lastField "${fieldCount} - 1")
    set(rest "${row}")
    foreach(field RANGE 0 ${lastField})
        list(GET ARGN ${field} outVar)
        if(field EQUAL lastField)
            set(value "${rest}")
        else()
            string(FIND "${rest}" "|" separator)
            if(separator EQUAL -1)
                message(FATAL_ERROR "Malformed row (wanted ${fieldCount} '|'-separated fields): ${row}")
            endif()
            string(SUBSTRING "${rest}" 0 ${separator} value)
            math(EXPR restStart "${separator} + 1")
            string(SUBSTRING "${rest}" ${restStart} -1 rest)
        endif()
        set(${outVar} "${value}" PARENT_SCOPE)
    endforeach()
endfunction()

# Split file content into a list of lines, one element per line.
#
# `file(STRINGS)` cannot be used: it returns a CMake list, so a line containing a
# ';' becomes several elements and every line number after it is wrong. The four
# characters CMake's list syntax reserves are replaced by a space rather than
# escaped -- escaping does not survive a line ending in a backslash, which every
# shell continuation in this repository is. Nothing is lost: none of them can
# appear inside `::htonl(`, and no line's text is ever printed, only its number.
#
# Tabs become spaces in the same pass, so the pattern below can spell optional
# whitespace as a plain space class -- CMake's regex engine does not read `\t`
# inside a bracket expression.
#
# @param content File content.
# @param linesOut Set to the content's lines, in order.
function(fastcached_split_lines content linesOut)
    string(REPLACE "\\" " " content "${content}")
    string(REPLACE ";" " " content "${content}")
    string(REPLACE "[" " " content "${content}")
    string(REPLACE "]" " " content "${content}")
    string(REPLACE "\t" " " content "${content}")
    string(REGEX REPLACE "\r?\n" ";" lines "${content}")
    set(${linesOut} "${lines}" PARENT_SCOPE)
endfunction()

# Turn a list of shell globs into one anchored regex, so a root can be walked ONCE
# and the results filtered in memory.
#
# `file(GLOB_RECURSE var a b c)` traverses the tree once PER PATTERN. On DrvFs one
# traversal of `src/` costs 2.09 s, so a list of N patterns is N x that -- and the call
# site reads as a single glob, which is what made the cost invisible (#502).
#
# This is the fourth copy of this idiom across the hygiene checks; consolidating them
# into a shared module is #495, deliberately not pre-empted here.
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

fastcached_globs_to_regex("${FastCachedByteOrderScanGlobs}" scanRegex)

# ---------------------------------------------------------------------------
# Collect the files to judge.
set(scanFiles "")
set(missingRoots "")
foreach(row IN LISTS FastCachedByteOrderScanRoots)
    fastcached_row_fields("${row}" scanRoot scanRootReason)
    set(rootPath "${FASTCACHED_SOURCE_DIR}/${scanRoot}")
    if(IS_DIRECTORY "${rootPath}")
        # ONE traversal per root, filtered afterwards. See fastcached_globs_to_regex.
        file(GLOB_RECURSE rootAll LIST_DIRECTORIES false "${rootPath}/*")
        set(rootFiles ${rootAll})
        list(FILTER rootFiles INCLUDE REGEX "${scanRegex}")
        list(APPEND scanFiles ${rootFiles})
    else()
        # A renamed or mistyped root would otherwise take a whole surface out of
        # this check's view while it went on reporting success.
        list(APPEND missingRoots
            "  ${scanRoot}\n      is named in the scan table but is not a directory. It is scanned because: ${scanRootReason}")
    endif()
endforeach()
list(REMOVE_DUPLICATES scanFiles)
list(SORT scanFiles)

set(byteOrderNames "")
foreach(row IN LISTS FastCachedByteOrderNames)
    fastcached_row_fields("${row}" byteOrderName byteOrderReason)
    list(APPEND byteOrderNames "${byteOrderName}")
endforeach()

# One alternation built from the table, rather than one pass per name.
#
# Six names is six whole-file `string(FIND)` passes over every scanned file and,
# on any file that mentions one, six regexes per line. Folded into one
# alternation that becomes one and two. The alternation is derived from the table
# so the two cannot disagree, and `CMAKE_MATCH_2` still names which one matched.
#
# Cost, over the 622 files this tree has today. Two rows because the two differ by
# forty-fold and quoting either alone would misdescribe the check:
#
#   native filesystem    ~0.5 s  (Windows, same 622 files)
#   /mnt/... under WSL   ~21 s   (was ~31 s before this fold)
#
# What the fold bought is the difference between those two 21/31 figures; the rest
# of the WSL number is `file(READ)` over a DrvFs mount and is a property of that
# mount, not of this check. CI runs on a native filesystem, so the first row is the
# one that describes it there.
list(JOIN byteOrderNames "|" byteOrderAlternation)

# The same table spelled for a human, derived rather than restated. The success
# line at the bottom of this file listed FOUR names while the table already held
# six -- "a second copy is not a cross-check, it is a second thing to be wrong",
# arriving inside a check written to enforce a rule.
list(JOIN byteOrderNames "/" byteOrderDisplay)
set(byteOrderQualified "(^|[^A-Za-z0-9_:])::[ ]*(${byteOrderAlternation})[ ]*\\(")
set(byteOrderBare "(^|[^A-Za-z0-9_:.>])(${byteOrderAlternation})[ ]*\\(")

# ---------------------------------------------------------------------------
# The scan.
#
# The pattern requires the character before `::` to be neither an identifier
# character nor a colon, which is what distinguishes GLOBAL qualification -- the
# defect -- from a qualified member such as `Foo::htonl(x)` or `A::B::htonl(x)`,
# which are legitimate and are left alone.
#
# It also requires the opening parenthesis. Without it the scan matches prose:
# the comment in KqueueSocket_test.cpp that states this very rule names the
# construct it forbids, and a check that fires on its own documentation is one
# people delete. Every real occurrence is a call.
set(violations "")
set(bareUses 0)

foreach(scanFile IN LISTS scanFiles)
    file(RELATIVE_PATH relativeFile "${FASTCACHED_SOURCE_DIR}" "${scanFile}")
    file(READ "${scanFile}" content)

    # Cheap whole-file test first: almost no file names any of them, and
    # splitting into lines is the expensive part. One regex over the whole
    # content rather than one `string(FIND)` per name -- six passes over every
    # one of 600-odd files is most of what this check costs.
    if(NOT content MATCHES "(${byteOrderAlternation})")
        continue()
    endif()

    fastcached_split_lines("${content}" fileLines)
    list(LENGTH fileLines lineCount)

    # A split that merged lines does not report a missing line, it reports the
    # WRONG line number -- silently. Checked against newlines counted before the
    # split, the one number the split cannot influence.
    string(REGEX MATCHALL "\n" newlines "${content}")
    list(LENGTH newlines newlineCount)
    math(EXPR expectedLines "${newlineCount} + 1")
    if(NOT lineCount EQUAL expectedLines)
        message(FATAL_ERROR
            "Splitting ${relativeFile} into lines produced ${lineCount} where the file has "
            "${newlineCount} newline(s), so this check cannot trust its own line numbers. "
            "A character CMake's list syntax reserves has reached the split unneutralised; "
            "fastcached_split_lines in ${CMAKE_CURRENT_LIST_FILE} says which those are.")
    endif()

    set(lineIndex -1)
    foreach(line IN LISTS fileLines)
        math(EXPR lineIndex "${lineIndex} + 1")
        # Two regexes per line rather than twelve. `CMAKE_MATCH_2` is the name
        # the alternation matched, which is what makes the message specific
        # without a per-name pass.
        if(line MATCHES "${byteOrderQualified}")
            set(matchedName "${CMAKE_MATCH_2}")
            math(EXPR humanLine "${lineIndex} + 1")
            list(APPEND violations
                "  ${relativeFile}:${humanLine}\n      writes ::${matchedName}( -- drop the ::")
        elseif(line MATCHES "${byteOrderBare}")
            # An unqualified call. Counted only to prove this scan still reaches
            # the code that uses these names -- see the vacuity guard below.
            # Excluding '.' and '>' keeps a member call such as `x.htons(...)`
            # out of the corpus count.
            math(EXPR bareUses "${bareUses} + 1")
        endif()
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# A banned-pattern scan passes by finding nothing, which is also what it does
# when it has stopped looking at anything. The two are indistinguishable from the
# result alone, so the corpus is asserted instead: these files must exist, and
# they must still contain unqualified calls to this family. A restructure that
# moved the network code out from under the scan roots lands here rather than
# turning this into a check that passes forever.
#
# Family-level rather than per-name: a single name falling out of use is ordinary
# churn, while ALL of them vanishing is the scan having lost its corpus.
set(vacuous "")
list(LENGTH scanFiles scanFileCount)
if(scanFileCount EQUAL 0)
    string(APPEND vacuous
        "  No file matched the scan globs under any scan root, so this check judged nothing.\n")
elseif(bareUses EQUAL 0)
    string(APPEND vacuous
        "  ${scanFileCount} file(s) were scanned and not one unqualified call to any of "
        "${byteOrderDisplay} was found.\n"
        "  Either this project has stopped converting byte order -- in which case delete this\n"
        "  check -- or the scan roots no longer reach the code that does, and this check has\n"
        "  been passing by looking at the wrong files.\n")
endif()

# ---------------------------------------------------------------------------
#   <variable holding the report lines>|<heading>
set(byteOrderReportSections
    "vacuous|This check has stopped checking anything"
    "missingRoots|The scan table names somewhere that is not there"
    "violations|Globally-qualified byte-order call(s)"
)

set(report "")
foreach(row IN LISTS byteOrderReportSections)
    fastcached_row_fields("${row}" sectionVariable sectionHeading)
    if(NOT "${${sectionVariable}}" STREQUAL "")
        list(JOIN ${sectionVariable} "\n" sectionBody)
        string(APPEND report "${sectionHeading}:\n${sectionBody}\n")
    endif()
endforeach()

if(NOT report STREQUAL "")
    message(FATAL_ERROR
        "${report}"
        "\nThese names are function-like macros on every platform here. The `::` is what "
        "breaks: on Darwin `htonl(x)` expands to `__DARWIN_OSSwapInt32(x)`, which is not a "
        "bare identifier, so the qualifier has nothing to attach to and clang reports "
        "`expected unqualified-id`. On glibc the expansion IS a bare identifier "
        "(`__bswap_32`), so the same line compiles -- which is why this reaches CI's macOS "
        "leg and nothing before it.\n\n"
        "Write them unqualified. There is no case in this tree where the `::` is needed, and "
        "no exemption table here on purpose -- a row excusing one would be a hole rather than "
        "a decision. A qualified MEMBER (`Foo::htonl(x)`) is not matched and needs nothing.\n\n"
        "Issue #469 has the history: three occurrences, two of them hours apart in different "
        "lanes, with the rule already written down three lines above the third.\n"
        "The tables live in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH scanFiles scanFileCount)
message("byte-order qualifier: ${scanFileCount} source file(s) scanned, ${bareUses} unqualified "
        "call(s) to ${byteOrderDisplay} found, 0 globally qualified")
