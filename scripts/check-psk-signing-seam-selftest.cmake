# SPDX-License-Identifier: Apache-2.0
#
# `psk-signing-seam` must be SEEN to refuse, on each thing it claims and on
# nothing else.
#
# That check is the enforcement half of #402: the seam takes its domain label as
# a required parameter, which binds whoever goes THROUGH it and says nothing at
# all about a fourth signer calling `HmacSha256` itself. So the rule the
# rulebook states -- one key, one construction -- rests on a scan, and a scan is
# the shape of guard this tree keeps catching going blind: #510's reader lost
# four of five targets to one stray `]` in a comment and reported success, and
# `check-target-file-guards-selftest.cmake` exists because that was found by a
# deliberate canary rather than by the check.
#
# The two cases that matter most here are the ones a passing check cannot be
# distinguished from:
#
#   `renamed`    -- the primitive is spelled something else everywhere. Every
#                   scan then matches nothing, every file is clean, and a check
#                   without a vacuity refusal reports success forever.
#   `stalerow`   -- the seam stops signing while its row stays in the table. The
#                   table then vouches for a file that has stopped doing the
#                   thing, and covers less than it says while looking complete.
#
# And `prose`, which is the opposite direction: a check that failed on the WORD
# rather than the call would make its own reasoning unwritable, which is the
# reliable way to get a guard deleted rather than fixed.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-psk-signing-seam-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-psk-signing-seam.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")

set(failures)

# ---------------------------------------------------------------------------
# The three files the signer table allows, written as stand-ins rather than
# copied from the tree: a selftest that copied the real ones would start
# failing for reasons that belong to those files rather than to this check.
#
# The path and the body are two parameters rather than one `path|body` row, and
# that is not a style preference: every interesting body here is a C++ statement,
# so it contains a `;`, and CMake would split one row into two list elements. The
# table idiom is right where a row is data and wrong where a row IS code.
#
# @param name Which synthetic tree.
# @param seamBody What Cluster/ClusterSigning.hpp contains.
# @param extraPath One further file, relative to src/; empty for none.
# @param extraBody What that file contains.
# @param outVar Set to the tree's root.
function(fastcached_make_tree name seamBody extraPath extraBody outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")

    file(WRITE "${tree}/src/FastCache/Core/Sha256.hpp"
         "namespace FastCache { Digest HmacSha256(Key key, Message message); }\n")
    file(WRITE "${tree}/src/FastCache/Core/Sha256.cpp"
         "Digest HmacSha256(Key key, Message message) { return Compute(key, message); }\n")
    file(WRITE "${tree}/src/FastCache/Cluster/ClusterSigning.hpp" "${seamBody}")

    if(NOT extraPath STREQUAL "")
        file(WRITE "${tree}/src/${extraPath}" "${extraBody}\n")
    endif()

    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# The seam as it actually is: one call, and one mention in prose.
set(signingSeam
"// One construction, built from HmacSha256 and the field grammar.\nDigest SignFields(Key key, Domain domain, Fields fields) { return HmacSha256(key, Encode(domain, fields)); }\n")

# @param tree Which synthetic tree to run the check against.
# @param outObjected Set TRUE when the check reported `CMake Error`.
# @param outOutput Set to everything the check printed, with every run of
#        whitespace collapsed to one space.
function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")

    # Collapsed before anything is matched against it, because `message()`
    # WORD-WRAPS at a fixed width and every needle below is a phrase rather than a
    # token. Measured, not guessed: the `nosources` refusal embeds the tree's own
    # path, so at a scratch directory of one length it reads `found no source file
    # it knows how to read` and at another `found no source\n  file it knows how to
    # read` -- and `string(FIND)` then reports the phrase absent. That made this
    # selftest fail on the tree it was passing on, decided by how long
    # `CMAKE_CURRENT_BINARY_DIR` happens to be on the machine running it, which
    # differs per preset, per checkout and per CI runner. A guard whose verdict
    # depends on a path length is a guard that reports on something other than its
    # subject. Single tokens are never broken by the wrapper, so this leaves the
    # file-path needles alone and repairs every phrase at once.
    string(REGEX REPLACE "[ \t\r\n]+" " " combined "${combined}")

    string(FIND "${combined}" "CMake Error" position)
    if(position EQUAL -1)
        set(${outObjected} FALSE PARENT_SCOPE)
    else()
        set(${outObjected} TRUE PARENT_SCOPE)
    endif()
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# 1. The three allowed signers and nothing else. Without this the check could
#    refuse everything, which is exactly as useless as refusing nothing and
#    looks a great deal more like rigour.
fastcached_make_tree("clean" "${signingSeam}" "" "" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "clean: a tree with exactly the three allowed signers was refused -- the check refuses everything")
endif()

# 2. A fourth signer. The whole point: a new caller of the primitive is an
#    ordinary call to a public function in Core/ that no compiler remarks on.
fastcached_make_tree("newsigner" "${signingSeam}"
    "apps/node/Announce.cpp" "Digest Tag(Key k, Message m) { return HmacSha256(k, m); }" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "newsigner: a second MAC construction under the cluster key was accepted -- which is the entire rule this check carries")
else()
    string(FIND "${output}" "apps/node/Announce.cpp" position)
    if(position EQUAL -1)
        list(APPEND failures "newsigner: the refusal did not name the offending file, so it cannot be acted on")
    endif()
    string(FIND "${output}" "SignFields" position)
    if(position EQUAL -1)
        list(APPEND failures "newsigner: the refusal did not say what to do instead, so the likely response is to widen the table")
    endif()
endif()

# 3. A test source may spell the primitive. `Core/Sha256_test.cpp` checks it
#    against the published vectors and `Cluster/ClusterSigning_test.cpp` pins
#    the seam's message shape by writing the construction out the long way --
#    both are the rule working, since what it governs is what SIGNS on the wire.
fastcached_make_tree("testsigner" "${signingSeam}"
    "FastCache/Cluster/ClusterSigning_test.cpp" "auto expected = HmacSha256(key, Encode(label, fields));" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "testsigner: a _test.cpp naming the primitive was refused -- that forbids pinning the construction against anything but the code that produces it")
endif()

# 4. Prose is not a call. A check that failed on the word would make the
#    reasoning above unwritable, and an unwritable rule gets deleted.
fastcached_make_tree("prose" "${signingSeam}"
    "FastCache/Distributed/LeaseToken.hpp" "// Signed through SignFields -- see HmacSha256 for the primitive it wraps." tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "prose: a comment naming the primitive without calling it was refused -- the rule cannot then be explained in the files it governs")
endif()

# 5. The seam stops signing while its row stays. The table then vouches for a
#    file that has stopped doing the thing, and covers less than it says.
fastcached_make_tree("stalerow"
    "Digest SignFields(Key key, Domain domain, Fields fields) { return Placeholder(key, fields); }\n" "" "" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "stalerow: the seam no longer signs anything and the check passed -- a table entry that matches nothing looks exactly like one that is satisfied")
else()
    string(FIND "${output}" "no longer call" position)
    if(position EQUAL -1)
        list(APPEND failures "stalerow: it refused, but not as a stale table row, so the cause will be hunted in the wrong place")
    endif()
endif()

# 6. The primitive is renamed. Every scan then matches nothing and every file is
#    clean -- the vacuous pass, which would hold forever.
fastcached_make_tree("renamed" "${signingSeam}" "" "" tree)
file(WRITE "${tree}/src/FastCache/Core/Sha256.hpp"
     "namespace FastCache { Digest MacIt(Key key, Message message); }\n")
file(WRITE "${tree}/src/FastCache/Core/Sha256.cpp"
     "Digest MacIt(Key key, Message message) { return Compute(key, message); }\n")
file(WRITE "${tree}/src/FastCache/Cluster/ClusterSigning.hpp"
     "Digest SignFields(Key key, Domain domain, Fields fields) { return MacIt(key, Encode(domain, fields)); }\n")
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "renamed: the primitive was spelled something else everywhere and the check reported success -- it had stopped looking at anything and would have gone on passing")
else()
    string(FIND "${output}" "found NONE" position)
    if(position EQUAL -1)
        list(APPEND failures "renamed: it refused, but not as a scan that matched nothing, so the reader is not told the check itself is broken")
    endif()
endif()

# 7. A tree the glob table cannot read. The same vacuity one level up, and the
#    one an emptiness test written as `if(x STREQUAL "")` can silently miss --
#    though only for one of the two ways a CMake variable comes to be empty, and
#    the two are opposites. `file(GLOB)` matching nothing DEFINES an empty list,
#    and such a guard fires correctly. Copying that empty list on with
#    `set(other ${glob})` leaves `other` UNDEFINED, and `if()` then compares the
#    undefined name against its own spelling, which is never empty -- so the
#    guard silently does nothing. The check under test copies, which is how it
#    reached a later and vaguer refusal here before `list(LENGTH)` replaced the
#    comparison. Both mechanisms measured rather than one inferred from the
#    other: a mechanism that fits is not a mechanism that was observed.
set(tree "${root}/nosources")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}/src/FastCache")
file(WRITE "${tree}/src/FastCache/notes.txt" "no source here\n")
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "nosources: a tree holding no source this check can read was accepted -- the scan examined nothing and said so was fine")
else()
    string(FIND "${output}" "no source file" position)
    if(position EQUAL -1)
        list(APPEND failures "nosources: it refused, but not as a walk that found no source, so the glob table will not be the first place anybody looks")
    endif()
endif()

# ---------------------------------------------------------------------------
if(failures)
    list(LENGTH failures failureCount)
    string(REPLACE ";" "\n  " rendered "${failures}")
    message(FATAL_ERROR
        "psk-signing-seam selftest: ${failureCount} case(s) wrong\n  ${rendered}")
endif()

message(STATUS "psk signing seam selftest: 7 synthetic tree(s), every verdict as expected")
