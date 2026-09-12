# SPDX-License-Identifier: Apache-2.0
#
# Refuses when a file under `vendor/` no longer matches `vendor/MANIFEST`.
#
# `vendor/` holds third-party source copied VERBATIM from upstream, and verbatim is
# load-bearing rather than tidy: `git diff <import>..HEAD -- vendor/endo` is what
# gets sent back upstream, so a vendored file edited in place quietly converts a
# contributable copy into a private fork. Nothing else notices. The build stays
# green, the tests pass, and the discovery comes months later when somebody tries to
# upstream a fix and finds the diff full of unrelated local edits.
#
# `.clang-format-ignore` already stops the MECHANICAL drift -- measured, the
# formatter would rewrite 146 of 165 vendored files on its first run. This covers
# the deliberate kind: a one-line fix applied where it was found rather than where
# it belongs.
#
# WHY A CONTENT MANIFEST rather than "diff against the import commit": a SHA anchor
# does not survive the branch. Every rebase, `/absorb` and autosquash rewrites the
# import commit, orphaning any range recorded against it -- and a range anchored to
# a commit that no longer exists does not fail, it silently widens. Content does not
# move.
#
# WHY SHA-256 AND NOT GIT: this must answer in a source export, which has no index.
# The generator and the checker are the same script for the same reason -- two
# spellings of "how the hash is computed" is one source of truth too many, and they
# would disagree exactly once, in the direction of reporting clean.
#
# Regenerate with -DFASTCACHED_VENDOR_WRITE_MANIFEST=ON, and record the change in
# vendor/VENDOR.md's "Local changes" section in the same commit.
#
# @param FASTCACHED_SOURCE_DIR Repository root.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(vendorDir "${FASTCACHED_SOURCE_DIR}/vendor")
set(manifestPath "${vendorDir}/MANIFEST")

# The tree, walked rather than read from git: a source export has no index, and the
# question -- do these bytes match what was imported -- is the same either way.
# `endo` rather than `vendor` so MANIFEST and VENDOR.md, which are OURS, are not
# asked to describe themselves.
file(GLOB_RECURSE vendorFiles RELATIVE "${FASTCACHED_SOURCE_DIR}" "${vendorDir}/endo/*")
list(SORT vendorFiles)

# A walk that matched nothing is a refusal, never a clean tree. It is also the state
# the whole check is easiest to leave silently broken in: an empty tree and an empty
# manifest agree perfectly, and the run reports success.
list(LENGTH vendorFiles fileCount)
if(fileCount EQUAL 0)
    message(FATAL_ERROR
        "check-vendor-verbatim: no files were found under ${vendorDir}/endo, so nothing was "
        "compared and this run says nothing about the vendored tree. That is the CHECK "
        "failing, not a clean result -- a checkout of this repository cannot have an empty "
        "vendor/endo.")
endif()

# Build the current state as text, in one spelling used for BOTH writing and
# checking, so the two can never disagree about what a line looks like.
set(current "")
foreach(rel IN LISTS vendorFiles)
    file(SHA256 "${FASTCACHED_SOURCE_DIR}/${rel}" hash)
    string(APPEND current "${hash}  ${rel}\n")
endforeach()

if(FASTCACHED_VENDOR_WRITE_MANIFEST)
    file(WRITE "${manifestPath}"
"# Vendored file manifest -- see vendor/VENDOR.md.
#
# SHA-256 of every file under vendor/endo, as imported. scripts/check-vendor-verbatim.cmake
# compares the tree against this list, so a vendored file edited in place is refused
# rather than discovered months later by somebody trying to send the change upstream.
#
# Regenerate ONLY when re-syncing upstream or landing a deliberate local change, and
# record that change in VENDOR.md's \"Local changes\" section in the SAME commit:
#
#     cmake -DFASTCACHED_SOURCE_DIR=<root> -DFASTCACHED_VENDOR_WRITE_MANIFEST=ON \\
#           -P scripts/check-vendor-verbatim.cmake
#
${current}")
    message(STATUS "check-vendor-verbatim: wrote ${fileCount} entries to vendor/MANIFEST")
    return()
endif()

if(NOT EXISTS "${manifestPath}")
    message(FATAL_ERROR
        "check-vendor-verbatim: ${manifestPath} does not exist, so the ${fileCount} vendored "
        "files could not be checked against anything. Regenerate it with "
        "-DFASTCACHED_VENDOR_WRITE_MANIFEST=ON and commit it -- deleting the manifest is not "
        "how a deliberate local change is recorded; VENDOR.md's \"Local changes\" section is.")
endif()

file(READ "${manifestPath}" manifestRaw)

# NOT redundant, and not about the vendored files: CMake's own `file(WRITE)` emits
# CRLF on Windows, so the generator above produces a CRLF manifest there while
# .gitattributes stores it as LF. Without this the check passes for whoever
# regenerated it and fails for everyone who then checks it out -- or the reverse,
# depending on which machine ran last. The comparison below is over text, so the
# line endings have to be settled before it.
string(REPLACE "\r\n" "\n" manifestRaw "${manifestRaw}")

# Strip comments, then compare the whole thing as TEXT before splitting anything.
# The fast path needs no list handling at all, which is where a `cmake -P` reader
# usually goes wrong: `;`, `\` and brackets in content are re-interpreted by
# `foreach(... IN LISTS ...)` and quietly change what is being compared.
#
# NOT `fastcached_split_lines_verbatim` from scripts/lib/CheckCommon.cmake, and this
# is a deliberate non-reuse rather than an oversight. That helper BLANKS `[` and `]`
# and escapes `;`, which is right for its job -- scanning source text for a pattern,
# where a bracket is noise. Here the lines are round-tripped and compared for BYTE
# EQUALITY against a freshly computed set, so any transformation of the content makes
# an unmodified tree report a mismatch. A vendored path containing a bracket would
# fail loudly rather than silently, but it would fail, and the refusal would name the
# wrong cause. CheckCommon's own header records that a reader whose lines must stay
# verbatim is the case it does not serve.
set(expected "")
string(REPLACE "\n" ";" manifestLines "${manifestRaw}")
foreach(line IN LISTS manifestLines)
    if(NOT line MATCHES "^#" AND NOT line STREQUAL "")
        string(APPEND expected "${line}\n")
    endif()
endforeach()

if(current STREQUAL expected)
    message(STATUS
        "vendor verbatim: ${fileCount} vendored file(s) match vendor/MANIFEST")
    return()
endif()

# Only now, on the slow path, work out WHICH -- because a refusal naming no file is
# a refusal nobody can act on.
set(changed "")
set(missing "")
set(added "")
string(REPLACE "\n" ";" expectedLines "${expected}")
string(REPLACE "\n" ";" currentLines "${current}")
foreach(line IN LISTS expectedLines)
    if(line STREQUAL "")
        continue()
    endif()
    if(NOT current MATCHES "(^|\n)${line}\n")
        string(REGEX REPLACE "^[0-9a-fA-F]+  " "" path "${line}")
        if(EXISTS "${FASTCACHED_SOURCE_DIR}/${path}")
            list(APPEND changed "${path}")
        else()
            list(APPEND missing "${path}")
        endif()
    endif()
endforeach()
foreach(line IN LISTS currentLines)
    if(line STREQUAL "")
        continue()
    endif()
    if(NOT expected MATCHES "(^|\n)${line}\n")
        string(REGEX REPLACE "^[0-9a-fA-F]+  " "" path "${line}")
        if(NOT path IN_LIST changed)
            list(APPEND added "${path}")
        endif()
    endif()
endforeach()

set(report "")
if(changed)
    list(JOIN changed "\n    " changedText)
    string(APPEND report "\n  EDITED (bytes differ from what was imported):\n    ${changedText}")
endif()
if(missing)
    list(JOIN missing "\n    " missingText)
    string(APPEND report "\n  MISSING (in the manifest, not in the tree):\n    ${missingText}")
endif()
if(added)
    list(JOIN added "\n    " addedText)
    string(APPEND report "\n  UNRECORDED (in the tree, not in the manifest):\n    ${addedText}")
endif()

message(FATAL_ERROR
    "check-vendor-verbatim: vendor/ no longer matches vendor/MANIFEST.${report}\n\n"
    "vendor/ is third-party source copied verbatim from upstream, and the copy is only "
    "worth having while it stays diffable against upstream: `git diff <import>..HEAD -- "
    "vendor/endo` is the patch that gets contributed back. An edit made in place does not "
    "break anything here, which is exactly why nothing else catches it.\n\n"
    "If the change is a MISTAKE -- a stray formatter run, an edit applied to the copy "
    "instead of to first-party code -- revert the file.\n\n"
    "If the change is DELIBERATE, it needs both halves: add a row to vendor/VENDOR.md's "
    "\"Local changes\" section saying what and why and which upstream it goes to, and "
    "regenerate the manifest with -DFASTCACHED_VENDOR_WRITE_MANIFEST=ON, in the SAME commit. "
    "Regenerating alone silences this check and loses the record, which is the failure it "
    "exists to prevent rather than a shortcut past it.\n\n"
    "This check says nothing about whether the tree still matches UPSTREAM -- only that it "
    "still matches what was imported. Re-syncing is vendor/VENDOR.md's \"How to re-sync\".")
