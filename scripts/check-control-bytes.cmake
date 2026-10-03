# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy the
# project has not stated. CMP0057 (`if(... IN_LIST ...)`) has already silently done
# nothing in a check in this tree.
cmake_minimum_required(VERSION 3.28)
#
# No tracked text file carries a C0 control byte other than TAB and LF (#595).
#
# ## What this is about
#
# A `printf '\a'` that was meant to be `printf '\\a'`, a paste out of a terminal, an
# editor that inserted a literal escape: the byte lands in the file, and every
# instrument in this repository reads straight past it. It is not whitespace, so no
# formatter normalises it; it is not a comment introducer, so no comment stripper sees
# it; it is valid UTF-8, so the text gates pass it. #593 was one BEL in one shell
# script, and what made it expensive was not the byte -- it was that nothing anywhere
# could have found it. The instance is gone; the class had no guard at all, which is
# what this is.
#
# A C0 byte is invisible in a diff, invisible in review, and changes what a script
# DOES: inside a shell word it is an ordinary character, so a name, a path or a
# comparison silently stops matching the one everybody can see on screen.
#
# ## The file set is an ALLOW-LIST of extensions
#
# Not a denylist of paths. An exclusion list bets on how the tree is laid out and is
# silent about anything that arrives outside it; an inclusion list states what this
# check is for and says so in its own output. A file whose extension is not below is
# NOT scanned and NOT claimed about, and the run prints how many those were.
#
# It is deliberately not "every text file", because that question cannot be answered
# from a path: whether a given blob is text is a property of its bytes, and a check
# that guessed would refuse a binary the day somebody committed one.
#
# ## The bytes are never read as TEXT
#
# `file(READ)` is not a byte read, and every way it differs reports clean. Measured: it
# drops the CR of a CRLF on Windows and on Linux alike (`a\r\nb\n`, five bytes, reads as
# four), a 0x1A ends a text-mode read on Windows (`a\x1ab\r\nc\rd\n`, nine bytes, reads as
# one), and while the string HOLDS a NUL, the regex engine stops at one -- so a NUL, and any
# byte behind one, was never matched. This check missed every NUL until that was measured;
# its header used to say a NUL truncates the read.
#
# So the question is put to the file set as a question about bytes:
# `fastcached_tracked_files(CONTAINING_BYTES ...)` answers which files hold any forbidden
# byte -- in git mode one `git grep -a -P` over the whole set, in a walk by reading each file
# as HEX -- and only those files are read here, as HEX, to say which byte and where. A file
# the answer does not name holds none; nothing about it is read, which is what keeps this
# check from paying a read and a size per file over a DrvFs checkout.
#
# The answer and the byte pass are two readings of one file, so they can disagree -- the
# file changed between them, or the answer was wrong -- and a file the answer named that the
# byte pass finds clean is refused, by name, rather than taken as clean.
#
# ## Offsets and lines come from the hex, with the bytes separated first
#
# A two-character token searched in a raw hex string matches at odd offsets too --
# `a0 7b` contains `07` -- so every hit would need its alignment checked and a
# mis-aligned one would be a byte that is not there. Inserting a space after each pair
# removes the question: a two-character token can then only match at a multiple of
# three, which is a whole byte, so the offset and the newline count are both exact.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

# ---------------------------------------------------------------------------
# The extensions this check is FOR: text this repository authors or configures with.
# A row is an extension without its dot, or a whole basename for the extensionless
# files (`.clang-tidy`, `Dockerfile`, `LICENSE`), which `get_filename_component(EXT)`
# reads as an extension of `.clang-tidy` and which are named here instead.
set(FastCachedControlByteExtensions
    awk c cmake cpp crt h hpp html in inc json key manifest md ps1 py rtf
    service sh socket sysusers tmpfiles txt xml yaml yml)

set(FastCachedControlByteNames
    .clang-format .clang-format-ignore .clang-format-version .clang-tidy
    .clang-tidy-version .dockerignore .gitattributes .gitignore .tsan-suppressions
    Dockerfile LICENSE MANIFEST
    # `packaging/macos/paths.d.fastcached` is an `/etc/paths.d` entry -- first-party
    # text whose `.fastcached` suffix is a product name rather than a file type, so it
    # is named here rather than widening the extension list. Found by PRINTING the
    # unread count and then asking what those files were, which is the whole reason
    # that figure is in the output.
    paths.d.fastcached)

# ---------------------------------------------------------------------------
# The bytes. TAB (9) and LF (10) are the only C0 codes a file here may carry.
#
# CR (13) is in the refused set on purpose and is the one most likely to be met: this
# repository is LF-everywhere by `.gitattributes`, and a CRLF `*.sh` does not misbehave,
# it fails to START. So the refusal below names it separately -- a CR is a line-ending
# problem with a different remedy from an escape that got eaten, and one message covering
# both would send half its readers to the wrong fix.
#
# DEL (127) is not a C0 code and is included anyway, for the reason the C0 codes are: it
# is invisible, it is valid UTF-8, and it survives every other gate here.
#
# NUL is a byte like the others here. It is asked of git as `\x00` and read as `00`, and never
# looked for in text, which cannot see one (see the header).
set(forbiddenCodes 0)
foreach(code RANGE 1 31)
    if(code EQUAL 9 OR code EQUAL 10)
        continue()
    endif()
    list(APPEND forbiddenCodes ${code})
endforeach()
list(APPEND forbiddenCodes 127)

# The two-digit lowercase hex token for each forbidden code, in the same order. Built from a
# digit table rather than `OUTPUT_FORMAT HEXADECIMAL`, which renders 7 as `0x7` -- one digit,
# and a one-digit token would match half of some other byte in the hex.
set(hexDigits 0 1 2 3 4 5 6 7 8 9 a b c d e f)
set(forbiddenTokens "")
foreach(code IN LISTS forbiddenCodes)
    math(EXPR high "${code} / 16")
    math(EXPR low "${code} % 16")
    list(GET hexDigits ${high} highDigit)
    list(GET hexDigits ${low} lowDigit)
    list(APPEND forbiddenTokens "${highDigit}${lowDigit}")
endforeach()

list(LENGTH forbiddenCodes forbiddenCount)
list(LENGTH forbiddenTokens tokenCount)
if(NOT forbiddenCount EQUAL 31 OR NOT tokenCount EQUAL 31)
    # A set one byte short is invisible in the output: the check would go on reporting a
    # file count and a clean verdict over a set it no longer covers.
    message(FATAL_ERROR
        "control-bytes: the forbidden set was built as ${tokenCount} token(s) from "
        "${forbiddenCount} code(s). It must be 31 bytes (0, 1-8, 11-31, 127). The set this "
        "check enforces is not the set it was written for.")
endif()

# ---------------------------------------------------------------------------
# The files. Through `fastcached_tracked_files`, which is the one answer to HOW a check
# finds its file set, and then declined against the third-party roots -- vendored source
# is not this repository's to hold to its byte rules, and an enumerator that had not
# asked would refuse `vendor/` on somebody else's behalf.
list(JOIN FastCachedControlByteExtensions "|" extensionAlternatives)
set(escapedNames ${FastCachedControlByteNames})
list(TRANSFORM escapedNames REPLACE "\\." "\\\\.")
list(JOIN escapedNames "|" nameAlternatives)
set(fileFilter "(\\.(${extensionAlternatives})|(^|/)(${nameAlternatives}))$")

# Asked WITHOUT the filter, and partitioned here, so the files the allow-list turned
# away can be COUNTED. `fastcached_tracked_files` applies `FILTER` internally and
# discards what it rejects, so a caller that passes one gets no denominator -- and the
# header above promises that number, because it is the single thing that makes an
# inclusion list defensible against its own silence: a new extension arriving in the
# tree is invisible except as a figure that moved. Claiming it and not printing it was
# worse than either.
fastcached_tracked_files("${FASTCACHED_SOURCE_DIR}"
    GLOBS "*" FILES_OUT tracked MODE_OUT mode
    CONTAINING_BYTES ${forbiddenTokens} CONTAINING_OUT holding
    MISSING_OUT missing)
fastcached_decline_third_party("${FASTCACHED_SOURCE_DIR}" tracked declined)

set(candidates "")
set(unlisted "")
foreach(trackedPath IN LISTS tracked)
    if(trackedPath MATCHES "${fileFilter}")
        list(APPEND candidates "${trackedPath}")
    else()
        list(APPEND unlisted "${trackedPath}")
    endif()
endforeach()

list(LENGTH candidates candidateCount)
list(LENGTH declined declinedCount)
list(LENGTH unlisted unlistedCount)


set(findings "")
set(scannedCount 0)

foreach(relative IN LISTS candidates)
    set(absolute "${FASTCACHED_SOURCE_DIR}/${relative}")
    # Asked of the file set once rather than of the filesystem per file: over DrvFs a
    # per-file `EXISTS` cost more than every other step of this check together.
    if(relative IN_LIST missing)
        # The index names it and the tree does not have it. Not a finding about bytes and
        # not silence either: it is a file this run did not read, and `scannedCount` is
        # what a reader compares, so it must not count one.
        list(APPEND findings "  ${relative}: named by the file set (${mode}) and not present here, so it was NOT scanned")
        continue()
    endif()

    math(EXPR scannedCount "${scannedCount} + 1")

    # Named by the answer or not read at all -- see the header. A file here that holds
    # nothing forbidden costs this comparison and nothing else.
    if(NOT relative IN_LIST holding)
        continue()
    endif()

    # True bytes, with a space after each so a two-character token can only match a whole
    # one.
    file(READ "${absolute}" hex HEX)
    string(REGEX REPLACE "(..)" "\\1 " spaced "${hex}")

    set(hits "")
    foreach(token IN LISTS forbiddenTokens)
        # The decimal is a function of the token -- `math(EXPR)` reads a hex literal --
        # so there is no second list to walk in lockstep and no index to keep.
        math(EXPR code "0x${token}")
        string(FIND "${spaced}" "${token}" at)
        if(at EQUAL -1)
            continue()
        endif()
        math(EXPR byteOffset "${at} / 3")

        # The LINE, because a byte offset alone sends a reader to a hex editor. Counted
        # over the bytes BEFORE the hit, in the same separated form, so a `0a` inside
        # some other byte pair cannot be counted as a newline.
        string(SUBSTRING "${spaced}" 0 ${at} before)
        string(REGEX MATCHALL "0a" newlines "${before}")
        list(LENGTH newlines newlineCount)
        math(EXPR lineNumber "${newlineCount} + 1")

        # How MANY, because the site alone reads as the whole finding and is not: this
        # reports the FIRST occurrence of each byte value, so a reader who fixes it and
        # re-runs meets the same byte again. Deliberately a count rather than a list of
        # sites -- the commonest real case is a CRLF file, where every line carries a
        # CR and listing them would bury the other findings under thousands of lines.
        string(REGEX MATCHALL "${token}" occurrences "${spaced}")
        list(LENGTH occurrences occurrenceCount)
        set(occurrenceNote "")
        if(occurrenceCount GREATER 1)
            set(occurrenceNote ", first of ${occurrenceCount} in this file")
        endif()

        if(code EQUAL 0)
            list(APPEND hits "    line ${lineNumber}, byte offset ${byteOffset}${occurrenceNote}: NUL (0x00) -- this is not a text file, or something wrote into it")
        elseif(code EQUAL 13)
            list(APPEND hits "    line ${lineNumber}, byte offset ${byteOffset}${occurrenceNote}: CR (0x0d) -- this tree is LF-only, by .gitattributes")
        else()
            list(APPEND hits "    line ${lineNumber}, byte offset ${byteOffset}${occurrenceNote}: control byte 0x${token} (decimal ${code})")
        endif()
    endforeach()

    if(NOT hits)
        # The file set said this file holds a forbidden byte and its true bytes hold none.
        # One of the two readings is wrong, and taking the clean one would be this check
        # choosing the answer that passes.
        list(APPEND findings
             "  ${relative}: the file set (${mode}) named it as holding a forbidden byte and its bytes, read as hex, hold none -- the two readings disagree, so it is NOT reported clean")
        continue()
    endif()
    # No element above contains a `;`, which is what lets these be joined as a list at
    # all -- one did, and it split the sentence across two lines in the refusal.
    list(JOIN hits "\n" hitReport)
    list(APPEND findings "  ${relative}:\n${hitReport}")
endforeach()

# ---------------------------------------------------------------------------
# The scan found nothing, which is not the same as finding nothing wrong.
#
# This check reports by ACCUMULATING findings, so a run that read no file and a tree with
# no control byte produce byte-identical output. The count is printed on every run for
# the same reason: a filter that stopped matching shows up as a number that moved, and
# there is nothing else in a clean run that could show it.
if(scannedCount EQUAL 0)
    message(FATAL_ERROR
        "control-bytes scanned 0 file(s) of ${candidateCount} candidate(s) under "
        "${FASTCACHED_SOURCE_DIR} (file set from ${mode}), so it is reporting on "
        "nothing. Either the file set is empty, or the extension allow-list in "
        "${CMAKE_CURRENT_LIST_FILE} no longer matches anything this tree contains. Both "
        "make this check pass vacuously.")
endif()

if(findings)
    list(LENGTH findings findingCount)
    list(JOIN findings "\n" findingReport)
    message(FATAL_ERROR
        "${findingCount} tracked file(s) carry a byte no text file here may (#595):\n"
        "${findingReport}\n\n"
        "       TAB and LF are the only C0 codes allowed. Everything else -- BEL, ESC, "
        "CR, DEL -- is invisible in a diff, invisible in review, valid UTF-8, and "
        "survives every formatter and text gate in this repository, while changing what "
        "a script DOES: inside a shell word it is an ordinary character, so a name or a "
        "comparison stops matching the one on screen.\n"
        "       The usual cause is an escape that got EATEN: `printf '\\a'` where "
        "`printf '\\\\a'` was meant, or a paste out of a terminal. Write the escape so "
        "the shell passes it through, or use the character's name.\n"
        "       For a CR, the cause is line endings rather than an escape: this tree is "
        "LF everywhere by `.gitattributes`, and a CRLF `*.sh` does not misbehave, it "
        "fails to start.\n"
        "       The rule lives in ${CMAKE_CURRENT_LIST_FILE}, which also carries the "
        "extension allow-list -- a file type it does not name is not scanned.")
endif()

message(STATUS
    "control-bytes: ${scannedCount} tracked text file(s) scanned (file set from ${mode}), "
    "none carries a C0 byte other than TAB or LF; ${declinedCount} third-party file(s) "
    "declined and ${unlistedCount} file(s) left unread by the extension allow-list, "
    "which is the figure to watch -- a file type arriving in this tree is invisible "
    "here except as that number moving")
