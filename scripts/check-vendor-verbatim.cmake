# SPDX-License-Identifier: Apache-2.0
#
# Refuses when a file under `vendor/` no longer matches `vendor/MANIFEST`, or differs from
# upstream without being declared as a local change in `vendor/VENDOR.md`.
#
# `vendor/` holds third-party source copied VERBATIM from upstream, and verbatim is
# load-bearing rather than tidy: the diff against the UPSTREAM fork points in
# vendor/VENDOR.md is what gets sent back, so a vendored file edited in place quietly
# converts a contributable copy into a private fork. Nothing else notices. The build stays
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
# ## A declared local change and an accidental edit are told apart
#
# Until #1375 they were not. The manifest held one hash per file, and regenerating it made
# the tree match again whatever had been edited. So the "record it in VENDOR.md in the same
# commit" instruction was the only thing standing between a deliberate local change and a
# stray edit silenced by a regeneration, and nothing checked it.
#
# So each manifest line also says whether its bytes are still UPSTREAM's:
#
#   <sha256>  <path>                               verbatim: upstream's bytes at the fork point
#   <sha256>  <path>  local-change-of <sha256>     differs from upstream, whose hash is recorded
#   <sha256>  <path>  local-change-new             not in upstream at all
#
# Regenerating never decides that a file is upstream's. It carries the upstream hash forward
# from the manifest it replaces: a verbatim file whose bytes changed becomes
# `local-change-of <its previous hash>`, a local change edited back to its upstream hash
# becomes verbatim again, and a file the old manifest did not list is `local-change-new`.
# Then every `local-change-*` line must be declared by a row of VENDOR.md's "Local changes"
# table naming its path, and every path such a row names must be a local change. So a
# regeneration can no longer silence an edit. It turns an undeclared edit into a refusal
# that names the file.
#
# `RESYNC` is the one spelling that marks files upstream's, because after a re-sync every
# file of that copy IS: the import reads upstream at a pinned version and compares each one
# (VENDOR.md, "How to re-sync"). It is a claim this script cannot verify, which is why it
# is a separate word rather than the default.
#
# ## Every third-party root, and a re-sync names ONE of them
#
# The roots are `scripts/lib/third-party-roots.txt`, read here rather than restated, so a new
# upstream copy is hashed, compared and declared by the same rules the day its row is added
# (#178 added the second, `vendor/monocypher`). Each root must hold files: a root that walks to
# nothing is refused by name, since the whole tree being non-empty says nothing about one copy.
#
# A re-sync is of ONE upstream, so `RESYNC` takes the root it is about in
# `FASTCACHED_VENDOR_RESYNC_ROOT` and marks only that root's files upstream's; every other root
# is carried forward exactly as `ON` carries it. A bare RESYNC that blessed every root would
# silence each OTHER copy's local changes -- the regeneration defect above, reached through the
# one word that is allowed to bless. The first import of a root is a re-sync of it.
#
# Not covered: a vendored file DELETED locally. The manifest describes the tree, so a
# deletion regenerates away with no declaration. The vendor build's partition assertion
# refuses a missing source, so this is covered for translation units and not for headers.
#
# Regenerate with -DFASTCACHED_VENDOR_WRITE_MANIFEST=ON, or with =RESYNC plus
# -DFASTCACHED_VENDOR_RESYNC_ROOT=<root> after a re-sync or the first import of that root.
#
# @param FASTCACHED_SOURCE_DIR Repository root.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(vendorDir "${FASTCACHED_SOURCE_DIR}/vendor")
set(manifestPath "${vendorDir}/MANIFEST")
set(vendorDocPath "${vendorDir}/VENDOR.md")

# The roots, from the one answer to "which directories are third-party" rather than a path
# written here: every root is a verbatim copy, and a copy this check was never told about is a
# copy an in-place edit reaches silently.
fastcached_third_party_roots("${FASTCACHED_SOURCE_DIR}" thirdPartyRoots)

# The tree, walked rather than read from git: a source export has no index, and the
# question -- do these bytes match what was imported -- is the same either way. Each ROOT
# rather than `vendor/`, so MANIFEST and VENDOR.md, which are OURS, are not asked to describe
# themselves.
#
# A walk that matched nothing is a refusal, never a clean tree, and it is asked PER ROOT: an
# empty copy and a manifest without its lines agree perfectly, and the other roots' files would
# make the total look healthy.
set(vendorFiles "")
foreach(root IN LISTS thirdPartyRoots)
    file(GLOB_RECURSE rootFiles RELATIVE "${FASTCACHED_SOURCE_DIR}" "${FASTCACHED_SOURCE_DIR}/${root}/*")
    list(LENGTH rootFiles rootFileCount)
    if(rootFileCount EQUAL 0)
        message(FATAL_ERROR
            "check-vendor-verbatim: no files were found under ${FASTCACHED_SOURCE_DIR}/${root}, a root "
            "scripts/lib/third-party-roots.txt names, so nothing of it was compared and this run says "
            "nothing about that copy. That is the CHECK failing, not a clean result -- restore the "
            "copy, or remove its row if the import is gone.")
    endif()
    list(APPEND vendorFiles ${rootFiles})
endforeach()
list(SORT vendorFiles)
list(LENGTH vendorFiles fileCount)
list(LENGTH thirdPartyRoots rootCount)

# The root a path lies under, or empty. A prefix test on `<path>/`, never a regex: a root is a
# path name, and a `.` or `+` in one is a character, not a pattern.
#
# @param path A repository-relative path.
# @param outVar Set to the root, or to empty.
function(fastcached_root_of path outVar)
    set(found "")
    foreach(candidate IN LISTS thirdPartyRoots)
        string(FIND "${path}/" "${candidate}/" position)
        if(position EQUAL 0)
            set(found "${candidate}")
            break()
        endif()
    endforeach()
    set(${outVar} "${found}" PARENT_SCOPE)
endfunction()

# Build the current state as text, in one spelling used for BOTH writing and
# checking, so the two can never disagree about what a line looks like.
set(current "")
foreach(rel IN LISTS vendorFiles)
    file(SHA256 "${FASTCACHED_SOURCE_DIR}/${rel}" hash)
    string(APPEND current "${hash}  ${rel}\n")
    string(MD5 key "${rel}")
    set("currentHash_${key}" "${hash}")
endforeach()

# The manifest's lines, comments dropped, as `expected` (the first two columns, compared as
# text) plus, per path, the upstream state its third column records. Read as text and
# walked by hand, never through a CMake list of the raw content, for the reason given at the
# comparison below.
#
# A function, not a macro: a macro substitutes `${raw}` TEXTUALLY and re-parses it, and the
# manifest's own header ends a line in a backslash, which CMake would then read as a line
# continuation.
#
# @param raw The manifest's content.
# Sets in the caller: `expected`, `manifestPaths`, `localPaths`, `malformedLines`, and for each
# path `upstream_<md5 of path>` = `verbatim`, `new` or the upstream hash, and
# `manifestHash_<md5 of path>` = the hash the manifest records for it.
function(fastcached_read_manifest raw)
    set(expected "")
    set(manifestPaths "")
    set(localPaths "")
    set(malformedLines "")
    string(REPLACE "\r\n" "\n" manifestText "${raw}")
    # A `;` would split one line into two list elements. Hash-and-path lines hold none, and a
    # comment that does is only a comment, so it is blanked rather than escaped.
    string(REPLACE ";" " " manifestText "${manifestText}")
    string(REPLACE "\n" ";" manifestLines "${manifestText}")
    foreach(line IN LISTS manifestLines)
        if(line MATCHES "^#" OR line STREQUAL "")
            continue()
        endif()
        if(line MATCHES "^([0-9a-f]+)  ([^ ]+)$")
            set(upstream "verbatim")
        elseif(line MATCHES "^([0-9a-f]+)  ([^ ]+)  local-change-of ([0-9a-f]+)$")
            set(upstream "${CMAKE_MATCH_3}")
            list(APPEND localPaths "${CMAKE_MATCH_2}")
        elseif(line MATCHES "^([0-9a-f]+)  ([^ ]+)  local-change-new$")
            set(upstream "new")
            list(APPEND localPaths "${CMAKE_MATCH_2}")
        else()
            list(APPEND malformedLines "${line}")
            continue()
        endif()
        string(APPEND expected "${CMAKE_MATCH_1}  ${CMAKE_MATCH_2}\n")
        list(APPEND manifestPaths "${CMAKE_MATCH_2}")
        string(MD5 key "${CMAKE_MATCH_2}")
        set("upstream_${key}" "${upstream}" PARENT_SCOPE)
        set("manifestHash_${key}" "${CMAKE_MATCH_1}" PARENT_SCOPE)
    endforeach()
    set(expected "${expected}" PARENT_SCOPE)
    set(manifestPaths "${manifestPaths}" PARENT_SCOPE)
    set(localPaths "${localPaths}" PARENT_SCOPE)
    set(malformedLines "${malformedLines}" PARENT_SCOPE)
endfunction()

if(FASTCACHED_VENDOR_WRITE_MANIFEST)
    set(resyncRoot "")
    if("${FASTCACHED_VENDOR_WRITE_MANIFEST}" STREQUAL "RESYNC")
        string(REPLACE ";" ", " rootsText "${thirdPartyRoots}")
        if("${FASTCACHED_VENDOR_RESYNC_ROOT}" STREQUAL "")
            message(FATAL_ERROR
                "check-vendor-verbatim: RESYNC names no root. A re-sync is of ONE upstream copy, so say "
                "which with -DFASTCACHED_VENDOR_RESYNC_ROOT=<root>, one of: ${rootsText}. Blessing every "
                "root at once would mark each OTHER copy's local changes upstream's, silently.")
        endif()
        if(NOT "${FASTCACHED_VENDOR_RESYNC_ROOT}" IN_LIST thirdPartyRoots)
            message(FATAL_ERROR
                "check-vendor-verbatim: FASTCACHED_VENDOR_RESYNC_ROOT is `${FASTCACHED_VENDOR_RESYNC_ROOT}`, "
                "which scripts/lib/third-party-roots.txt does not name. The roots are: ${rootsText}. A new "
                "copy is a row there first, then a RESYNC of it.")
        endif()
        set(resyncRoot "${FASTCACHED_VENDOR_RESYNC_ROOT}")
    elseif(NOT EXISTS "${manifestPath}")
        message(FATAL_ERROR
            "check-vendor-verbatim: there is no ${manifestPath} to carry upstream hashes forward "
            "from, so a regeneration cannot tell a local change from an upstream file. A first "
            "import, whose files are all upstream's, is -DFASTCACHED_VENDOR_WRITE_MANIFEST=RESYNC "
            "with -DFASTCACHED_VENDOR_RESYNC_ROOT=<root>.")
    endif()
    # Carried forward in BOTH modes: a RESYNC of one root still carries every other root's
    # upstream hashes, which is the point of naming the root.
    if(EXISTS "${manifestPath}")
        file(READ "${manifestPath}" previousRaw)
        fastcached_read_manifest("${previousRaw}")
        if(NOT "${malformedLines}" STREQUAL "")
            message(FATAL_ERROR
                "check-vendor-verbatim: the manifest being replaced has line(s) this script cannot "
                "read, so their upstream state cannot be carried forward:\n  ${malformedLines}")
        endif()
    endif()

    set(lines "")
    set(localCount 0)
    foreach(rel IN LISTS vendorFiles)
        string(MD5 key "${rel}")
        set(hash "${currentHash_${key}}")
        fastcached_root_of("${rel}" relRoot)
        if(NOT "${resyncRoot}" STREQUAL "" AND "${relRoot}" STREQUAL "${resyncRoot}")
            set(upstream "${hash}")
        elseif(NOT DEFINED "upstream_${key}")
            set(upstream "new")
        elseif("${upstream_${key}}" STREQUAL "verbatim")
            # The previous manifest recorded upstream's bytes for this path, so its previous
            # hash IS the upstream hash, whatever the file holds now.
            set(upstream "${manifestHash_${key}}")
        else()
            set(upstream "${upstream_${key}}")
        endif()

        if("${upstream}" STREQUAL "${hash}")
            string(APPEND lines "${hash}  ${rel}\n")
        elseif("${upstream}" STREQUAL "new")
            string(APPEND lines "${hash}  ${rel}  local-change-new\n")
            math(EXPR localCount "${localCount} + 1")
        else()
            string(APPEND lines "${hash}  ${rel}  local-change-of ${upstream}\n")
            math(EXPR localCount "${localCount} + 1")
        endif()
    endforeach()

    file(WRITE "${manifestPath}"
"# Vendored file manifest -- see vendor/VENDOR.md.
#
# SHA-256 of every file under every third-party root in scripts/lib/third-party-roots.txt.
# scripts/check-vendor-verbatim.cmake compares the tree against this list, so a vendored file
# edited in place is refused rather than discovered months later by somebody trying to send the
# change upstream.
#
# A line ending `local-change-of <sha256>` differs from upstream, whose hash it records, and one
# ending `local-change-new` is not in upstream. Every such path must be named by a row of
# VENDOR.md's \"Local changes\" table, and regenerating never marks a file upstream's, so a
# regeneration cannot silence an undeclared edit:
#
#     cmake -DFASTCACHED_SOURCE_DIR=<root> -DFASTCACHED_VENDOR_WRITE_MANIFEST=ON \\
#           -P scripts/check-vendor-verbatim.cmake
#
# After a re-sync of ONE root from upstream, and only then, =RESYNC with
# -DFASTCACHED_VENDOR_RESYNC_ROOT=<root> marks that root's files upstream's.
#
${lines}")
    set(resyncText "")
    if(NOT "${resyncRoot}" STREQUAL "")
        set(resyncText ", ${resyncRoot} re-synced")
    endif()
    message(STATUS "check-vendor-verbatim: wrote ${fileCount} entries across ${rootCount} root(s) to "
                   "vendor/MANIFEST, ${localCount} of them local change(s)${resyncText}")
    return()
endif()

if(NOT EXISTS "${manifestPath}")
    message(FATAL_ERROR
        "check-vendor-verbatim: ${manifestPath} does not exist, so the ${fileCount} vendored "
        "files could not be checked against anything. Regenerate it with "
        "-DFASTCACHED_VENDOR_WRITE_MANIFEST=ON and commit it -- deleting the manifest is not "
        "how a deliberate local change is recorded -- VENDOR.md's \"Local changes\" section is.")
endif()

file(READ "${manifestPath}" manifestRaw)

# CRLF is settled inside the reader and NOT redundant: CMake's own `file(WRITE)` emits CRLF
# on Windows, so the generator above produces a CRLF manifest there while .gitattributes
# stores it as LF. Without it the check passes for whoever regenerated and fails for everyone
# who then checks it out -- or the reverse, depending on which machine ran last.
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
fastcached_read_manifest("${manifestRaw}")
if(NOT "${malformedLines}" STREQUAL "")
    list(JOIN malformedLines "\n    " malformedText)
    message(FATAL_ERROR
        "check-vendor-verbatim: vendor/MANIFEST has line(s) that are neither a hash and a path "
        "nor one of the two local-change forms, so the files they name were not checked:\n"
        "    ${malformedText}\n"
        "Regenerate the manifest with -DFASTCACHED_VENDOR_WRITE_MANIFEST=ON rather than editing it.")
endif()

# ---------------------------------------------------------------------------
# Half one: the bytes are what the manifest says.
set(report "")
if(NOT current STREQUAL expected)
    # Only now, on the slow path, work out WHICH -- because a refusal naming no file is
    # a refusal nobody can act on.
    set(changed "")
    set(missing "")
    set(added "")
    set(outside "")
    string(REPLACE "\n" ";" expectedLines "${expected}")
    string(REPLACE "\n" ";" currentLines "${current}")
    foreach(line IN LISTS expectedLines)
        if(line STREQUAL "")
            continue()
        endif()
        if(NOT current MATCHES "(^|\n)${line}\n")
            string(REGEX REPLACE "^[0-9a-fA-F]+  " "" path "${line}")
            fastcached_root_of("${path}" pathRoot)
            if("${pathRoot}" STREQUAL "")
                list(APPEND outside "${path}")
            elseif(EXISTS "${FASTCACHED_SOURCE_DIR}/${path}")
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

    if(changed)
        list(JOIN changed "\n    " changedText)
        string(APPEND report "\n  EDITED (bytes differ from vendor/MANIFEST):\n    ${changedText}")
    endif()
    if(missing)
        list(JOIN missing "\n    " missingText)
        string(APPEND report "\n  MISSING (in the manifest, not in the tree):\n    ${missingText}")
    endif()
    if(added)
        list(JOIN added "\n    " addedText)
        string(APPEND report "\n  UNRECORDED (in the tree, not in the manifest):\n    ${addedText}")
    endif()
    if(outside)
        list(JOIN outside "\n    " outsideText)
        string(APPEND report
            "\n  OUTSIDE EVERY ROOT (in the manifest, under no root of scripts/lib/third-party-roots.txt, "
            "so nothing compared them -- a root was dropped from that file, or the manifest names a path "
            "that was never vendored):\n    ${outsideText}")
    endif()
endif()

# ---------------------------------------------------------------------------
# Half two: every file that is not upstream's is declared, and every declaration describes one.
#
# A declaration is a backticked path under a third-party root on a TABLE ROW (a line starting `|`) of
# VENDOR.md's "## Local changes" section. Rows only: prose in that section explains, and a path
# mentioned while explaining must not declare anything. Brackets and semicolons are blanked
# before the section is split into lines, since VENDOR.md is markdown and a declared path holds
# neither.
set(declaredPaths "")
if(EXISTS "${vendorDocPath}")
    file(READ "${vendorDocPath}" vendorDoc)
    string(REPLACE "\r\n" "\n" vendorDoc "${vendorDoc}")
    string(FIND "${vendorDoc}" "\n## Local changes\n" sectionStart)
    if(sectionStart EQUAL -1)
        message(FATAL_ERROR
            "check-vendor-verbatim: ${vendorDocPath} has no `## Local changes` section, so no local "
            "change can be declared and none can be checked. Restore the heading.")
    endif()
    string(SUBSTRING "${vendorDoc}" ${sectionStart} -1 section)
    string(SUBSTRING "${section}" 1 -1 section)
    string(FIND "${section}" "\n## " sectionEnd)
    if(NOT sectionEnd EQUAL -1)
        string(SUBSTRING "${section}" 0 ${sectionEnd} section)
    endif()
    string(REPLACE "[" " " section "${section}")
    string(REPLACE "]" " " section "${section}")
    string(REPLACE ";" " " section "${section}")
    string(REPLACE "\n" ";" sectionLines "${section}")
    foreach(line IN LISTS sectionLines)
        if(NOT line MATCHES "^\\|")
            continue()
        endif()
        string(REGEX MATCHALL "`[^`]+`" rowPaths "${line}")
        foreach(rowPath IN LISTS rowPaths)
            string(REPLACE "`" "" rowPath "${rowPath}")
            fastcached_root_of("${rowPath}" rowRoot)
            if(NOT "${rowRoot}" STREQUAL "")
                list(APPEND declaredPaths "${rowPath}")
            endif()
        endforeach()
    endforeach()
else()
    message(FATAL_ERROR "check-vendor-verbatim: ${vendorDocPath} does not exist, so no local change can be declared.")
endif()

set(undeclared "")
foreach(path IN LISTS localPaths)
    if(NOT path IN_LIST declaredPaths)
        string(MD5 key "${path}")
        if("${upstream_${key}}" STREQUAL "new")
            list(APPEND undeclared "${path} (not in upstream)")
        else()
            list(APPEND undeclared "${path} (upstream sha256 ${upstream_${key}})")
        endif()
    endif()
endforeach()
set(staleDeclarations "")
foreach(path IN LISTS declaredPaths)
    if(NOT path IN_LIST localPaths)
        if(path IN_LIST manifestPaths)
            list(APPEND staleDeclarations "${path} (its manifest line says it is upstream's bytes)")
        else()
            list(APPEND staleDeclarations "${path} (not in vendor/MANIFEST)")
        endif()
    endif()
endforeach()
if(NOT "${undeclared}" STREQUAL "")
    list(JOIN undeclared "\n    " undeclaredText)
    string(APPEND report
        "\n  UNDECLARED LOCAL CHANGE (differs from upstream, named by no row of VENDOR.md's \"Local changes\" table):\n"
        "    ${undeclaredText}")
endif()
if(NOT "${staleDeclarations}" STREQUAL "")
    list(JOIN staleDeclarations "\n    " staleText)
    string(APPEND report
        "\n  STALE DECLARATION (a \"Local changes\" row names a file that is not a local change):\n"
        "    ${staleText}")
endif()

if("${report}" STREQUAL "")
    list(LENGTH localPaths localCount)
    string(REPLACE ";" ", " rootsText "${thirdPartyRoots}")
    message(STATUS
        "vendor verbatim: ${fileCount} vendored file(s) under ${rootCount} root(s) (${rootsText}) "
        "match vendor/MANIFEST, ${localCount} of them declared local change(s)")
    return()
endif()

message(FATAL_ERROR
    "check-vendor-verbatim: vendor/ does not match what vendor/MANIFEST and vendor/VENDOR.md say.${report}\n\n"
    "vendor/ is third-party source copied verbatim from upstream, and the copy is only "
    "worth having while it stays diffable against the UPSTREAM fork points recorded in "
    "vendor/VENDOR.md -- that diff is the patch that gets contributed back. An edit made "
    "in place does not break anything here, which is exactly why nothing else catches "
    "it.\n\n"
    "If the change is a MISTAKE -- a stray formatter run, an edit applied to the copy "
    "instead of to first-party code -- revert the file, and regenerate the manifest if it "
    "was regenerated over the edit.\n\n"
    "If the change is DELIBERATE, it is made UPSTREAM first and re-vendored (VENDOR.md, \"How "
    "to send a change back\"), then needs both halves in the SAME commit: regenerate the "
    "manifest with -DFASTCACHED_VENDOR_WRITE_MANIFEST=ON, which records each changed file's "
    "upstream hash, and add a row to VENDOR.md's \"Local changes\" table naming every such "
    "file in backticks, saying what and why and which upstream it goes to. Regenerating "
    "alone does not silence this check, and that is the point of it.\n\n"
    "A STALE DECLARATION means the change was upstreamed, reverted or never made: delete "
    "the row, or the path from it.\n\n"
    "This check says nothing about whether the tree still matches UPSTREAM beyond the "
    "hashes the manifest carries -- `=RESYNC` is trusted, not verified. Re-syncing is "
    "vendor/VENDOR.md's \"How to re-sync\", one root at a time.")
