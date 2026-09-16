# SPDX-License-Identifier: Apache-2.0
#
# Nothing under `src/apps/fastcache-cli/` asks the environment or the terminal a
# question directly (#134 §9.11).
#
# `live-stats` draws from a model and a rung, both handed to it: what the terminal can
# draw is `TerminalCapabilities`, decided once before the loop, and the environment
# reaches the binary through `Platform/Environment` from `main`. A `getenv`, `isatty`,
# `ioctl` or `GetConsoleMode` in a panel, the loop or a verb is the ambient read that
# design exists to exclude.
#
# ## Why a scan
#
# Because no behavioural test can see this. A panel that called `isatty` to decide its
# glyphs draws the right frame on the machine it was tested on, and the determinism
# property -- one event list renders byte-identical frames -- still holds wherever the
# answer happens to be stable. The loop's own tests are the behavioural half of this
# clause; this is the half that reads the source.
#
# ## What counts
#
# A token is matched as a SUBSTRING of the code, case-sensitively, after comments are
# stripped: `std::getenv`, `_isatty`, `secure_getenv`, `_wgetenv` and `ioctlsocket` are
# the same question asked through a different door, and a word-boundary match would let
# every one of them through. The prose documenting the rule -- `DashboardLoop.hpp` names
# all four tokens to explain this very check -- is a comment and is not a read.
#
# Blind spots, stated rather than papered over:
#
#   * a token inside a STRING LITERAL reads as code, as it does in every regex-shaped
#     reader here; a help text naming `isatty` would need an exemption row.
#   * a read reached through a first-party seam -- `Platform/Terminal`,
#     `Platform/Environment` -- is not a token here, deliberately: those seams are where
#     the question belongs, and the rule is that the CLI is HANDED their answers.
#
# ## Exemptions are rows with reasons, and a stale row is refused
#
# `scripts/cli-ambient-read-exemptions.txt` holds one `<path>|<token>|<reason>` row per
# permitted read. A missing file is a refusal, not an empty table -- two empty answers
# agree perfectly. A row with no reason, an unknown token, or a path this scan does not
# cover is refused; so is a row whose file no longer names its token, because a
# permission that has stopped describing the tree is what lets the next read through.
#
# ## Fails closed
#
# The same stripping and matching run over `src/FastCache/Platform/`, where these calls
# LIVE, and must recognise at least one there. A scanner that recognises none of the
# tokens where they are known to be would report every CLI file clean, and that clean
# report would look exactly like this one. The control is found by scanning, not by a
# listed path, so moving a seam's file does not leave the check excusing a path that no
# longer exists.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> [-DGIT_EXECUTABLE=<git>] \
#         -P scripts/check-cli-ambient-reads.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# Spelled once each: what is matched, what the refusals name, and what the exemption
# rows are validated against.
set(FastCachedAmbientTokens "getenv" "isatty" "ioctl" "GetConsoleMode")
set(subjectRoot "src/apps/fastcache-cli")
set(controlRoot "src/FastCache/Platform")
set(exemptionTable "scripts/cli-ambient-read-exemptions.txt")

# Walk a file line by line WITHOUT building a CMake list of lines, strip comments, and
# report `<line>:<token>` for every token left in code.
#
# `;`, `\`, `[` and `]` are CMake list structure and C++ is full of all four, so the walk
# is `FIND`/`SUBSTRING` rather than a split -- immune to them by construction, the reason
# `check-cli-text-cell.cmake` walks the same way. Consolidating the copies of this idiom
# is [#495](https://github.com/LASTRADA-Software/fastcached/issues/495), not this ticket.
function(fastcached_ambient_hits content outHits)
    set(hits "")
    set(rest "${content}")
    set(lineNumber 0)
    set(inBlockComment FALSE)
    while(TRUE)
        string(FIND "${rest}" "\n" newline)
        if(newline EQUAL -1)
            set(line "${rest}")
        else()
            string(SUBSTRING "${rest}" 0 ${newline} line)
        endif()
        string(REGEX REPLACE "\r$" "" line "${line}")
        math(EXPR lineNumber "${lineNumber} + 1")

        set(stripped "${line}")
        set(skipLine FALSE)
        if(inBlockComment)
            string(FIND "${stripped}" "*/" closeAt)
            if(closeAt EQUAL -1)
                set(skipLine TRUE)
            else()
                math(EXPR afterClose "${closeAt} + 2")
                string(SUBSTRING "${stripped}" ${afterClose} -1 stripped)
                set(inBlockComment FALSE)
            endif()
        endif()
        if(NOT skipLine)
            # An inline `/* ... */` first; CMake's regex is greedy, so a read BETWEEN two
            # block comments on one line is invisible. Nothing here writes that.
            string(REGEX REPLACE "/\\*.*\\*/" " " stripped "${stripped}")
            # Which introducer comes first decides -- a `//` comment naming `/*` must not
            # open a block no `*/` closes, which would skip the rest of the file while a
            # clean count was printed over lines never read.
            string(FIND "${stripped}" "/*" blockAt)
            string(FIND "${stripped}" "//" lineAt)
            if(NOT blockAt EQUAL -1 AND (lineAt EQUAL -1 OR blockAt LESS lineAt))
                string(SUBSTRING "${stripped}" 0 ${blockAt} stripped)
                set(inBlockComment TRUE)
            elseif(NOT lineAt EQUAL -1)
                string(SUBSTRING "${stripped}" 0 ${lineAt} stripped)
            endif()

            foreach(token IN LISTS FastCachedAmbientTokens)
                string(FIND "${stripped}" "${token}" tokenAt)
                if(NOT tokenAt EQUAL -1)
                    list(APPEND hits "${lineNumber}:${token}")
                endif()
            endforeach()
        endif()

        if(newline EQUAL -1)
            break()
        endif()
        math(EXPR skip "${newline} + 1")
        string(LENGTH "${rest}" restLength)
        if(skip GREATER_EQUAL restLength)
            break()
        endif()
        string(SUBSTRING "${rest}" ${skip} -1 rest)
    endwhile()
    set(${outHits} "${hits}" PARENT_SCOPE)
endfunction()

# Whether @p content names any token at all: the cheap whole-file test that keeps the
# line walk off the files that cannot match. Walking every file line by line to find
# that out costs seconds on every platform for nothing.
function(fastcached_mentions_any content outMentions)
    set(mentions FALSE)
    foreach(token IN LISTS FastCachedAmbientTokens)
        string(FIND "${content}" "${token}" at)
        if(NOT at EQUAL -1)
            set(mentions TRUE)
            break()
        endif()
    endforeach()
    set(${outMentions} "${mentions}" PARENT_SCOPE)
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

# The C++ files under one root, asked of git rather than inferred from directory names, with a
# directory walk when there is no index -- and the mode is part of the verdict, because a
# fixture staging synthetic trees takes the walk while CI takes git.
#
# The ROOT is this check's question and stays here; how a question is answered is
# `fastcached_tracked_files` (#1485), which this wrapper keeps parameterising by root. The
# pathspec and the glob are the same question twice, or the two modes cover two sets.
function(fastcached_sources_under root outFiles outMode)
    fastcached_tracked_files("${FASTCACHED_SOURCE_DIR}"
        PATHSPECS "${root}/*.cpp" "${root}/*.hpp"
        GLOBS "${root}/*.cpp" "${root}/*.hpp"
        FILES_OUT files
        MODE_OUT mode)
    set(${outFiles} "${files}" PARENT_SCOPE)
    set(${outMode} "${mode}" PARENT_SCOPE)
endfunction()

list(JOIN FastCachedAmbientTokens ", " tokenList)

# ---------------------------------------------------------------------------
# The subject.
fastcached_sources_under("${subjectRoot}" subjectFiles scanSource)
list(LENGTH subjectFiles subjectCount)
if(subjectCount EQUAL 0)
    message("")
    message("  No C++ source was found under ${subjectRoot}/.")
    message("")
    message("That is not a clean binary, it is a scan with no subject -- a moved directory,")
    message("or a FASTCACHED_SOURCE_DIR pointing somewhere else.")
    message(FATAL_ERROR "cli-ambient-reads: the scan matched no sources under ${subjectRoot} and cannot conclude")
endif()

set(subjectHits "")
foreach(relative IN LISTS subjectFiles)
    # `git ls-files` lists the INDEX, so a file deleted and not yet staged is still named.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)
    fastcached_mentions_any("${wholeFile}" mentions)
    if(NOT mentions)
        continue()
    endif()
    fastcached_ambient_hits("${wholeFile}" hits)
    foreach(hit IN LISTS hits)
        list(APPEND subjectHits "${relative}:${hit}")
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# THE FAIL-CLOSED GUARD: the scanner must recognise a token where the platform seams
# read them. Asked BEFORE any verdict about the subject.
fastcached_sources_under("${controlRoot}" controlFiles controlSource)
set(controlCount 0)
set(controlFile "")
foreach(relative IN LISTS controlFiles)
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" wholeFile)
    fastcached_mentions_any("${wholeFile}" mentions)
    if(NOT mentions)
        continue()
    endif()
    fastcached_ambient_hits("${wholeFile}" hits)
    list(LENGTH hits controlCount)
    # One recognising file is the whole question, so the walk stops there rather than
    # paying for the rest of the platform tree on every run.
    if(controlCount GREATER 0)
        set(controlFile "${relative}")
        break()
    endif()
endforeach()

if(controlCount EQUAL 0)
    message("")
    message("  Not one of ${tokenList} was recognised under ${controlRoot}/.")
    message("")
    message("That is where these calls live -- the terminal and environment seams the CLI")
    message("is handed answers from -- so this scan is its own positive control there. If it")
    message("recognises nothing where the reads are known to be, a clean result under")
    message("${subjectRoot}/ means nothing. Either the seams moved, in which case point the")
    message("control at their new home, or the comment stripping has begun eating code.")
    message("")
    message("Enumerated the control via ${controlSource}.")
    message(FATAL_ERROR "cli-ambient-reads: the scanner recognised no read in the platform seams, so it cannot conclude")
endif()

# ---------------------------------------------------------------------------
# The exemption table: required, validated row by row, and every row must still
# describe the tree.
if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${exemptionTable}")
    message("")
    message("  ${exemptionTable} is missing.")
    message("")
    message("A missing table is not an empty one. An empty table is a file with no rows,")
    message("which says nothing is exempt; a missing file says nothing at all, and reading it")
    message("as empty would turn a deleted table into a silent permission change.")
    message(FATAL_ERROR "cli-ambient-reads: the exemption table ${exemptionTable} is missing")
endif()

file(READ "${FASTCACHED_SOURCE_DIR}/${exemptionTable}" tableText)
set(tableProblems "")
set(exempted "")
set(rowCount 0)
set(rest "${tableText}")
set(tableLine 0)
while(NOT rest STREQUAL "")
    string(FIND "${rest}" "\n" newline)
    if(newline EQUAL -1)
        set(row "${rest}")
        set(rest "")
    else()
        string(SUBSTRING "${rest}" 0 ${newline} row)
        math(EXPR after "${newline} + 1")
        string(SUBSTRING "${rest}" ${after} -1 rest)
    endif()
    string(REGEX REPLACE "\r$" "" row "${row}")
    math(EXPR tableLine "${tableLine} + 1")
    string(STRIP "${row}" trimmed)
    if(trimmed STREQUAL "" OR trimmed MATCHES "^#")
        continue()
    endif()
    math(EXPR rowCount "${rowCount} + 1")

    # Fields by the first two `|` only, so a reason may contain one.
    string(FIND "${trimmed}" "|" firstBar)
    if(firstBar EQUAL -1)
        list(APPEND tableProblems "${exemptionTable}:${tableLine}: not a <path>|<token>|<reason> row")
        continue()
    endif()
    string(SUBSTRING "${trimmed}" 0 ${firstBar} rowPath)
    math(EXPR afterFirst "${firstBar} + 1")
    string(SUBSTRING "${trimmed}" ${afterFirst} -1 remainder)
    string(FIND "${remainder}" "|" secondBar)
    if(secondBar EQUAL -1)
        list(APPEND tableProblems "${exemptionTable}:${tableLine}: not a <path>|<token>|<reason> row")
        continue()
    endif()
    string(SUBSTRING "${remainder}" 0 ${secondBar} rowToken)
    math(EXPR afterSecond "${secondBar} + 1")
    string(SUBSTRING "${remainder}" ${afterSecond} -1 rowReason)
    string(STRIP "${rowPath}" rowPath)
    string(STRIP "${rowToken}" rowToken)
    string(STRIP "${rowReason}" rowReason)

    if(rowReason STREQUAL "")
        list(APPEND tableProblems "${exemptionTable}:${tableLine}: ${rowPath} may name ${rowToken}, but the row gives no reason")
        continue()
    endif()
    list(FIND FastCachedAmbientTokens "${rowToken}" tokenIndex)
    if(tokenIndex EQUAL -1)
        list(APPEND tableProblems "${exemptionTable}:${tableLine}: `${rowToken}` is not a token this scan matches (${tokenList})")
        continue()
    endif()
    list(FIND subjectFiles "${rowPath}" pathIndex)
    if(pathIndex EQUAL -1)
        list(APPEND tableProblems "${exemptionTable}:${tableLine}: STALE -- ${rowPath} is not a source under ${subjectRoot}/")
        continue()
    endif()

    set(stillReads FALSE)
    foreach(hit IN LISTS subjectHits)
        # Nested rather than one `AND`, so the match groups are read only after the match
        # that sets them has run.
        if(hit MATCHES "^(.*):[0-9]+:(.*)$")
            if("${CMAKE_MATCH_1}" STREQUAL "${rowPath}" AND "${CMAKE_MATCH_2}" STREQUAL "${rowToken}")
                set(stillReads TRUE)
            endif()
        endif()
    endforeach()
    if(NOT stillReads)
        list(APPEND tableProblems "${exemptionTable}:${tableLine}: STALE -- ${rowPath} no longer names ${rowToken}")
        continue()
    endif()
    list(APPEND exempted "${rowPath}|${rowToken}")
endwhile()

if(tableProblems)
    list(LENGTH tableProblems problemCount)
    message("")
    foreach(problem IN LISTS tableProblems)
        message("  ${problem}")
    endforeach()
    message("")
    message("Every exemption row names a file, a token and the reason that file reads it")
    message("directly. A STALE row is one that no longer describes the tree: delete it. It is")
    message("not harmless -- it is a standing permission for the next read in that file.")
    message("")
    message("Enumerated ${subjectCount} source(s) under ${subjectRoot}/ via ${scanSource}.")
    message(FATAL_ERROR "cli-ambient-reads: ${problemCount} problem(s) in ${exemptionTable}")
endif()

# ---------------------------------------------------------------------------
# The verdict.
set(violations "")
foreach(hit IN LISTS subjectHits)
    if(hit MATCHES "^(.*):([0-9]+):(.*)$")
        set(hitPath "${CMAKE_MATCH_1}")
        set(hitLine "${CMAKE_MATCH_2}")
        set(hitToken "${CMAKE_MATCH_3}")
        list(FIND exempted "${hitPath}|${hitToken}" exemptIndex)
        if(exemptIndex EQUAL -1)
            list(APPEND violations "${hitPath}:${hitLine}: ${hitToken}")
        endif()
    endif()
endforeach()

if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("fastcache-cli asks neither the environment nor the terminal anything itself: it is")
    message("HANDED the answers (#134 §9.11). A read here draws the right frame on the machine")
    message("it was tested on and passes every behavioural test, which is why this is a scan.")
    message("")
    message("Take the answer as a value instead:")
    message("")
    message("  * what the terminal can draw is `TerminalCapabilities`, decided once and passed in;")
    message("  * the environment is read by `Platform/Environment`, from `main`, and handed down.")
    message("")
    message("If a file here genuinely must ask, add a row to ${exemptionTable} naming the file,")
    message("the token and the reason.")
    message("")
    message("WHAT THIS DOES NOT SAY: nothing here objects to `Platform/Terminal` or")
    message("`Platform/Environment` making these calls -- that is where they belong, and this")
    message("scan uses them as its own positive control. Do not move a read into a seam's file")
    message("to satisfy this unless the CLI then receives its answer as a value.")
    message("")
    message("Enumerated ${subjectCount} source(s) under ${subjectRoot}/ via ${scanSource}.")
    message(FATAL_ERROR "cli-ambient-reads: ${violationCount} ambient read(s) under ${subjectRoot}")
endif()

list(LENGTH exempted exemptCount)
message(STATUS
    "cli-ambient-reads: no ${tokenList} across ${subjectCount} source(s) under ${subjectRoot}/ "
    "via ${scanSource}; ${exemptCount} exemption row(s); the control recognised ${controlCount} "
    "read(s) in ${controlFile}")
