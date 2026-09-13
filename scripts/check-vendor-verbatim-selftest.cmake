# SPDX-License-Identifier: Apache-2.0
#
# `vendor-verbatim` must be SEEN to tell a declared local change from an accidental edit.
#
# The check had no selftest. It refused an edit made in place, and a regeneration of the
# manifest silenced that refusal for ANY edit, declared or not (found while landing #1375's
# local change, which is the first one this tree has). So what is pinned here is mostly the
# half that did not exist: a regeneration carries each file's UPSTREAM hash forward rather than
# blessing the new bytes, and a file that differs from upstream must be named by a row of
# VENDOR.md's "Local changes" table.
#
# Which case pins which half:
#
#   clean, declared, declaredWithLink, resyncClears   the ACCEPTING direction
#   editedNotRegenerated                              the original refusal: bytes differ from the manifest
#   blessedByRegeneration                             THE defect: an edit plus a regeneration, undeclared
#   declaredInProse                                   a path in prose declares nothing; only table rows do
#   newFile                                           a file upstream does not have is a local change too
#   staleDeclaration, revertedChange                  a row naming a file that is upstream's is refused
#   editedTwice                                       the upstream hash survives a second regeneration
#   noManifestToCarry, malformed, noHeading, vacuous  every way of having nothing to compare refuses
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-vendor-verbatim-selftest.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-vendor-verbatim.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
set(failures "")
set(caseCount 0)

set(alpha "alpha\n")

# Runs the check over @p tree, optionally in a write mode.
#
# @param tree The synthetic source root.
# @param writeMode Empty to verify, or the value of FASTCACHED_VENDOR_WRITE_MANIFEST.
# @param outObjected Set to TRUE when the output carries a CMake diagnostic.
# @param outOutput Set to the output, whitespace flattened so a wrapped phrase still matches.
function(fastcached_run_check tree writeMode outObjected outOutput)
    set(extra "")
    if(NOT "${writeMode}" STREQUAL "")
        set(extra "-DFASTCACHED_VENDOR_WRITE_MANIFEST=${writeMode}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" ${extra} -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE runResult)
    if(NOT runResult MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "check-vendor-verbatim-selftest: INCONCLUSIVE -- the check could not be RUN (${runResult}). "
            "This is NOT a rule regression: no case was evaluated.")
    endif()
    set(combined "${captured}${capturedErrors}")
    set(sawSignal FALSE)
    if(combined MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    # A `;` becomes `,`: this output is quoted into the failure LIST, where a semicolon would
    # split one failure into two and overstate the count.
    string(REGEX REPLACE "[ \t\r\n]+" " " flattened "${combined}")
    string(REPLACE ";" "," flattened "${flattened}")
    set(${outObjected} ${sawSignal} PARENT_SCOPE)
    set(${outOutput} "${flattened}" PARENT_SCOPE)
endfunction()

# A vendored tree of two files, a VENDOR.md whose "Local changes" section holds @p localChanges
# followed by another heading, and a manifest written by RESYNC -- so both files start as
# upstream's. The RESYNC is asserted to have succeeded, or every case below tests a tree that
# was never set up.
#
# @param name The case name and directory.
# @param localChanges The body of the "Local changes" section.
# @param outVar Set to the tree's path.
function(fastcached_make_tree name localChanges outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(WRITE "${tree}/vendor/endo/a.hpp" "${alpha}")
    file(WRITE "${tree}/vendor/endo/sub/b.hpp" "beta\n")
    file(WRITE "${tree}/vendor/VENDOR.md"
        "# Vendored\n\n## Local changes\n\n${localChanges}\n\n## How to send a change back\n\nProse.\n")
    fastcached_run_check("${tree}" RESYNC objected output)
    if(objected OR NOT EXISTS "${tree}/vendor/MANIFEST")
        message(FATAL_ERROR
            "check-vendor-verbatim-selftest: INCONCLUSIVE -- the setup RESYNC for `${name}` failed, "
            "so no case over it would test anything. Output: ${output}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Judges one verification run.
#
# @param name Case name.
# @param tree The tree to verify.
# @param expect `accept` or `refuse`.
# @param mustSay A phrase the output must contain, which is what says WHICH verdict fired.
# @param why What it means when this case fails.
function(fastcached_judge name tree expect mustSay why)
    math(EXPR caseCount "${caseCount} + 1")
    fastcached_run_check("${tree}" "" objected output)
    if("${expect}" STREQUAL "refuse" AND NOT objected)
        list(APPEND failures "${name}: accepted, and must refuse -- ${why}. Output: ${output}")
    elseif("${expect}" STREQUAL "accept" AND objected)
        list(APPEND failures "${name}: refused, and must accept -- ${why}. Output: ${output}")
    else()
        string(FIND "${output}" "${mustSay}" said)
        if(said EQUAL -1)
            list(APPEND failures "${name}: ${expect}ed without saying `${mustSay}`, so something else decided it -- ${why}. Output: ${output}")
        endif()
    endif()
    set(caseCount ${caseCount} PARENT_SCOPE)
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# Regenerates the manifest over @p tree, refusing the selftest if the regeneration objected.
function(fastcached_regenerate tree)
    fastcached_run_check("${tree}" ON objected output)
    if(objected)
        message(FATAL_ERROR "check-vendor-verbatim-selftest: INCONCLUSIVE -- a regeneration over ${tree} failed: ${output}")
    endif()
endfunction()

set(rowA "| `vendor/endo/a.hpp` | what | why | endo | prepared as a branch |")

# ---------------------------------------------------------------------------
fastcached_make_tree("clean" "None." tree)
fastcached_judge("clean" "${tree}" accept "2 vendored file(s) match vendor/MANIFEST, 0 of them declared"
    "an upstream tree with a fresh manifest -- a check refusing this refuses everything")

fastcached_make_tree("editedNotRegenerated" "None." tree)
file(WRITE "${tree}/vendor/endo/a.hpp" "alpha edited\n")
fastcached_judge("editedNotRegenerated" "${tree}" refuse "EDITED"
    "an edit with no regeneration is the refusal this check always had")

fastcached_make_tree("blessedByRegeneration" "None." tree)
# Hashed from the FILE, never from the string written into it: `file(WRITE)` emits CRLF on
# Windows, so the string's hash is not the bytes' hash there.
file(SHA256 "${tree}/vendor/endo/a.hpp" alphaHash)
file(WRITE "${tree}/vendor/endo/a.hpp" "alpha edited\n")
fastcached_regenerate("${tree}")
file(READ "${tree}/vendor/MANIFEST" manifest)
string(REPLACE ";" "," manifest "${manifest}")
string(FIND "${manifest}" "vendor/endo/a.hpp  local-change-of ${alphaHash}" recorded)
if(recorded EQUAL -1)
    list(APPEND failures "blessedByRegeneration: the regeneration did not record a.hpp's upstream hash -- manifest: ${manifest}")
endif()
fastcached_judge("blessedByRegeneration" "${tree}" refuse "UNDECLARED LOCAL CHANGE"
    "an edit silenced by regenerating the manifest, with no VENDOR.md row, is the defect this selftest exists for")

fastcached_make_tree("declared" "${rowA}" tree)
file(WRITE "${tree}/vendor/endo/a.hpp" "alpha edited\n")
fastcached_regenerate("${tree}")
fastcached_judge("declared" "${tree}" accept "1 of them declared local change(s)"
    "a regenerated edit named by a Local changes row is the legitimate path")

fastcached_make_tree("declaredWithLink"
    "| `vendor/endo/a.hpp` | see [the ticket](https://example.invalid/1) | why; with a semicolon | endo | branch |" tree)
file(WRITE "${tree}/vendor/endo/a.hpp" "alpha edited\n")
fastcached_regenerate("${tree}")
fastcached_judge("declaredWithLink" "${tree}" accept "1 of them declared local change(s)"
    "a row carrying markdown brackets and a semicolon must still declare its path")

fastcached_make_tree("declaredInProse" "`vendor/endo/a.hpp` was changed, as this sentence explains." tree)
file(WRITE "${tree}/vendor/endo/a.hpp" "alpha edited\n")
fastcached_regenerate("${tree}")
fastcached_judge("declaredInProse" "${tree}" refuse "UNDECLARED LOCAL CHANGE"
    "a path mentioned in prose is not a declaration, or explaining a change would declare it")

fastcached_make_tree("newFile" "None." tree)
file(WRITE "${tree}/vendor/endo/c.hpp" "gamma\n")
fastcached_regenerate("${tree}")
fastcached_judge("newFile" "${tree}" refuse "vendor/endo/c.hpp (not in upstream)"
    "a file upstream does not have is a local change and must be declared")

fastcached_make_tree("staleDeclaration" "${rowA}" tree)
fastcached_judge("staleDeclaration" "${tree}" refuse "STALE DECLARATION"
    "a row naming a file that is still upstream's describes a change that does not exist")

fastcached_make_tree("revertedChange" "${rowA}" tree)
file(WRITE "${tree}/vendor/endo/a.hpp" "alpha edited\n")
fastcached_regenerate("${tree}")
file(WRITE "${tree}/vendor/endo/a.hpp" "${alpha}")
fastcached_regenerate("${tree}")
fastcached_judge("revertedChange" "${tree}" refuse "its manifest line says it is upstream's bytes"
    "a change edited back to upstream's bytes is verbatim again, so its row is stale")

fastcached_make_tree("editedTwice" "${rowA}" tree)
file(SHA256 "${tree}/vendor/endo/a.hpp" alphaHash)
file(WRITE "${tree}/vendor/endo/a.hpp" "alpha edited\n")
fastcached_regenerate("${tree}")
file(WRITE "${tree}/vendor/endo/a.hpp" "alpha edited again\n")
fastcached_regenerate("${tree}")
file(READ "${tree}/vendor/MANIFEST" manifest)
string(REPLACE ";" "," manifest "${manifest}")
string(FIND "${manifest}" "local-change-of ${alphaHash}" recorded)
if(recorded EQUAL -1)
    list(APPEND failures "editedTwice: the second regeneration lost a.hpp's UPSTREAM hash, so a revert could never be recognised -- manifest: ${manifest}")
endif()
fastcached_judge("editedTwice" "${tree}" accept "1 of them declared local change(s)"
    "a declared change edited again is still the same declared change")

fastcached_make_tree("resyncClears" "None." tree)
file(WRITE "${tree}/vendor/endo/a.hpp" "alpha from a newer upstream\n")
fastcached_run_check("${tree}" RESYNC objected output)
fastcached_judge("resyncClears" "${tree}" accept "0 of them declared local change(s)"
    "after a re-sync every file is upstream's, which is what RESYNC says")

# ---------------------------------------------------------------------------
fastcached_make_tree("noManifestToCarry" "None." tree)
file(REMOVE "${tree}/vendor/MANIFEST")
math(EXPR caseCount "${caseCount} + 1")
fastcached_run_check("${tree}" ON objected output)
string(FIND "${output}" "RESYNC" said)
if(NOT objected OR said EQUAL -1)
    list(APPEND failures "noManifestToCarry: a regeneration with no manifest to carry upstream hashes from did not refuse by pointing at RESYNC. Output: ${output}")
endif()

fastcached_make_tree("malformed" "None." tree)
file(APPEND "${tree}/vendor/MANIFEST" "not a manifest line\n")
fastcached_judge("malformed" "${tree}" refuse "neither a hash and a path"
    "a manifest line nobody can read describes a file nothing checked")

fastcached_make_tree("noHeading" "None." tree)
file(WRITE "${tree}/vendor/VENDOR.md" "# Vendored\n\nNo local changes heading here.\n")
fastcached_judge("noHeading" "${tree}" refuse "no `## Local changes` section"
    "with the section gone nothing can be declared, and an empty declaration set must not read as none needed")

set(tree "${root}/vacuous")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}/vendor/endo")
file(WRITE "${tree}/vendor/VENDOR.md" "## Local changes\n\nNone.\n")
file(WRITE "${tree}/vendor/MANIFEST" "# empty\n")
fastcached_judge("vacuous" "${tree}" refuse "no files were found"
    "an empty tree and an empty manifest agree perfectly")

# ---------------------------------------------------------------------------
list(LENGTH failures failureCount)
if(failureCount GREATER 0)
    string(REPLACE ";" "\n  " printable "${failures}")
    message(FATAL_ERROR
        "check-vendor-verbatim-selftest: ${failureCount} failure(s) across ${caseCount} case(s), and a case can fail more than one assertion:\n"
        "  ${printable}\n")
endif()

message(STATUS "check-vendor-verbatim-selftest: ${caseCount} case(s), each seen to behave as claimed")
