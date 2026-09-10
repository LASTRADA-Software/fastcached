# SPDX-License-Identifier: Apache-2.0
#
# `read-buffer-guard` must be SEEN to refuse, on each thing it claims and on nothing
# else. By this tree's own standard a guard nobody has watched refuse is not a guard,
# and this one exists precisely because another guard -- `empty-read-buffer-canary` --
# was watched refusing ONE site and read as covering six.
#
# Six cases, and none of them is decoration:
#
#   1. compliant   -- a tree that complies PASSES. A check that refuses everything is
#                     exactly as useless as one that refuses nothing, and looks like
#                     rigour.
#   2. missing     -- a transport with no guard is refused, NAMING the file and the
#                     class. This is the real defect: measured on the real tree,
#                     deleting the call from `EpollSocket::Read` left the canary and
#                     all 3316 tests green.
#   3. late        -- a guard that stands AFTER the body's first `return` is refused,
#                     and refused for THAT reason rather than as missing. Every
#                     transport opens with `if (_closed) return ...`, so this is the
#                     shape a well-meaning edit produces, and a presence-only scan
#                     reads it as green.
#   4. commented   -- a guard that appears only inside a `//` comment is refused. A
#                     COMMENT IS NOT A CALL SITE (#720), and the check strips comments
#                     before it measures anything.
#   5. nosources   -- a tree with no `Net/*.cpp` is refused as the CHECK being broken.
#   6. nosignature -- a tree full of sources in which the signature matches nothing is
#                     refused too, and that is a DIFFERENT question from case 5: a
#                     renamed or reformatted signature leaves the file count healthy
#                     and the implementation count at zero, and "no violations" over an
#                     empty set is the most confident wrong answer available.
#
# Cases 5 and 6 are the ones a reader is most likely to think redundant. They are not:
# each is reachable without the other, and each would otherwise report that every
# transport complies, forever.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-read-buffer-guard-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-read-buffer-guard.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(root "${FASTCACHED_SCRATCH_DIR}")
file(REMOVE_RECURSE "${root}")
set(failures "")
set(caseCount 0)

# Build a tree holding one `Net/` source with the given body.
# @param name     Sub-directory under the scratch root.
# @param fileName What to call the source, or "" for no source at all.
# @param contents The source text.
# @param outVar   Set to the tree root.
function(fastcached_make_tree name fileName contents outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Net")
    if(NOT fileName STREQUAL "")
        file(WRITE "${tree}/src/FastCache/Net/${fileName}" "${contents}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# @param tree        Tree to run the check against.
# @param outObjected TRUE when the check printed a CMake Error.
# @param outOutput   Everything it printed.
function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured ERROR_VARIABLE capturedErrors RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")
    # **This copy of `fastcached_run_check` DIVERGES from its three siblings, and the
    # divergence is deliberate.** `check-target-file-guards-selftest`,
    # `check-worker-refusals-selftest` and `check-succeed-not-skip-selftest` carry the
    # same function byte-identically and do NOT flatten. This one must: CMake wraps its
    # diagnostics at ~74 columns, so a phrase can exist in the output and in no single
    # LINE of it, and the phrases this selftest matches on
    # (`ExampleSocket::Read does not call`, `only AFTER its first return`) are long
    # enough to straddle. A negative test written without it reads as a refutation.
    #
    # Said out loud because a silently non-identical copy is worse than either a
    # faithful one or a shared helper -- it is the case a later consolidation cannot
    # detect. The siblings match shorter phrases and are correct today; whether they
    # should flatten too is their question, not this one.
    string(REGEX REPLACE "[\r\n]+" " " combined "${combined}")
    string(REGEX REPLACE " +" " " combined "${combined}")
    # `CMake Error|CMake Warning`, never `CMake Error` alone: a sub-run that merely WARNS
    # changes meaning silently and, read for the error word alone, is scored a clean pass
    # (#672). Stated in full -- and enforced -- in `scripts/check-script-check-signals.cmake`.
    set(sawSignal FALSE)
    if(combined MATCHES "CMake Error|CMake Warning")
        set(sawSignal TRUE)
    endif()
    set(${outObjected} ${sawSignal} PARENT_SCOPE)
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# A transport that complies: the guard is the first statement, ahead of the `_closed`
# early return every real one opens with.
set(compliantSource
"IoAwaitable ExampleSocket::Read(std::span<std::byte> buffer)
{
    Detail::RequireReadBuffer(buffer);
    if (_closed)
        return IoAwaitable { std::unexpected(NetError {}) };
    return IoAwaitable { IoResult { buffer.size() } };
}
")

# ---------------------------------------------------------------------------
# 1. A compliant tree passes, and the report says how much it looked at.
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("compliant" "ExampleSocket.cpp" "${compliantSource}" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "compliant: a transport that guards correctly was refused -- the check refuses everything")
else()
    string(FIND "${output}" "1 ISocket::Read implementation(s)" position)
    if(position EQUAL -1)
        list(APPEND failures "compliant: it passed without saying how many implementations it saw, so a shrinking set would be invisible")
    endif()
endif()

# ---------------------------------------------------------------------------
# 2. THE REGRESSION CASE. A transport with no guard is refused and NAMED.
math(EXPR caseCount "${caseCount} + 1")
set(missingSource
"IoAwaitable ExampleSocket::Read(std::span<std::byte> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(NetError {}) };
    return IoAwaitable { IoResult { buffer.size() } };
}
")
fastcached_make_tree("missing" "ExampleSocket.cpp" "${missingSource}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "missing: a transport with no guard passed -- which is exactly what the canary does, and the whole reason this check exists")
else()
    string(FIND "${output}" "ExampleSocket::Read does not call" position)
    if(position EQUAL -1)
        list(APPEND failures "missing: it refused without naming the transport, so the reader cannot act on it")
    endif()
endif()

# ---------------------------------------------------------------------------
# 3. A guard below the first return is refused, and for THAT reason.
math(EXPR caseCount "${caseCount} + 1")
set(lateSource
"IoAwaitable ExampleSocket::Read(std::span<std::byte> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(NetError {}) };
    Detail::RequireReadBuffer(buffer);
    return IoAwaitable { IoResult { buffer.size() } };
}
")
fastcached_make_tree("late" "ExampleSocket.cpp" "${lateSource}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "late: a guard standing after the body's first return passed -- a presence-only scan, which is the weaker check this one replaced")
else()
    string(FIND "${output}" "only AFTER its first return" position)
    if(position EQUAL -1)
        list(APPEND failures "late: it refused, but not as a MISPLACED guard, so the cause will be hunted as a missing one")
    endif()
endif()

# ---------------------------------------------------------------------------
# 4. A guard that exists only in a comment is refused. A comment is not a call site.
math(EXPR caseCount "${caseCount} + 1")
set(commentedSource
"IoAwaitable ExampleSocket::Read(std::span<std::byte> buffer)
{
    // Detail::RequireReadBuffer(buffer);
    if (_closed)
        return IoAwaitable { std::unexpected(NetError {}) };
    return IoAwaitable { IoResult { buffer.size() } };
}
")
fastcached_make_tree("commented" "ExampleSocket.cpp" "${commentedSource}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "commented: a guard that is only a COMMENT passed -- two checks in this tree have already matched their own headers (#720)")
endif()

# ---------------------------------------------------------------------------
# 5. No source at all: the CHECK is broken, not the tree clean.
math(EXPR caseCount "${caseCount} + 1")
fastcached_make_tree("nosources" "" "" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "nosources: a Net/ directory with no source passed -- an empty scan and a compliant tree are not the same answer")
else()
    string(FIND "${output}" "found no non-test source" position)
    if(position EQUAL -1)
        list(APPEND failures "nosources: it refused, but not as an empty scan, so the reason will be misread")
    endif()
endif()

# ---------------------------------------------------------------------------
# 6. Sources present, signature matching none of them. Distinct from case 5, and the
#    one a reader is most likely to delete as redundant: the file count is healthy and
#    the implementation count is zero.
math(EXPR caseCount "${caseCount} + 1")
set(renamedSource
"IoAwaitable ExampleSocket::ReadSome(std::span<std::byte> into)
{
    return IoAwaitable { IoResult { into.size() } };
}
")
fastcached_make_tree("nosignature" "ExampleSocket.cpp" "${renamedSource}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "nosignature: a tree in which the signature matches NOTHING passed -- every transport would 'comply' from here on")
else()
    string(FIND "${output}" "found NO implementation" position)
    if(position EQUAL -1)
        list(APPEND failures "nosignature: it refused, but not as a signature that matches nothing, so a rename would be hunted as a real violation")
    endif()
endif()

# ---------------------------------------------------------------------------
# 7. TWO implementations in ONE file, the second unguarded, with real padding ahead of
#    the first. This is the case the other six structurally cannot reach: every one of
#    them is a single implementation at offset zero, so a cursor that over-advances
#    still lands past the only match and the verdict is unchanged.
#
#    It exists because the check DID over-advance -- by a whole signature -- and
#    reported `1 implementation(s) ... all refusing` over a file whose second transport
#    had no guard. A false pass in the instrument built to prevent one. The padding is
#    load-bearing rather than realism: with both functions at the top of the file the
#    over-advance is small enough to land inside the second signature and the check
#    finds it anyway, which is a regression test that does not reproduce the regression.
#    Do not "tidy" the filler away.
#
#    It asserts BOTH that the refusal names the second class AND that both
#    implementations were seen, because **a refusal counting one of two is right by
#    accident** -- and a failure message saying so is read by somebody debugging a red,
#    not by somebody about to delete the padding.
math(EXPR caseCount "${caseCount} + 1")
set(paddedPair "")
foreach(i RANGE 1 200)
    string(APPEND paddedPair "// filler line ${i}, to push the first definition well into the file\n")
endforeach()
string(APPEND paddedPair
"
IoAwaitable AlphaSocket::Read(std::span<std::byte> buffer)
{
    Detail::RequireReadBuffer(buffer);
    if (_closed)
        return IoAwaitable { std::unexpected(NetError {}) };
    return IoAwaitable { IoResult { buffer.size() } };
}

IoAwaitable BetaSocket::Read(std::span<std::byte> buffer)
{
    if (_closed)
        return IoAwaitable { std::unexpected(NetError {}) };
    return IoAwaitable { IoResult { buffer.size() } };
}
")
fastcached_make_tree("twoinone" "TwoSockets.cpp" "${paddedPair}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "twoinone: a file whose SECOND implementation has no guard passed -- the walk skipped it, which is a false pass in the instrument built to prevent one")
else()
    string(FIND "${output}" "BetaSocket::Read does not call" position)
    if(position EQUAL -1)
        list(APPEND failures "twoinone: it refused, but did not name the SECOND implementation, so the walk is still not reaching it")
    endif()
    string(FIND "${output}" "2 ISocket::Read implementation(s)" position)
    if(position EQUAL -1)
        list(APPEND failures "twoinone: it refused without having SEEN both implementations -- a refusal that counted one of two is right by accident")
    endif()
endif()

# ---------------------------------------------------------------------------
# How many cases RAN is part of the output, because a self-test that stops early must
# not look like one that judged something (#720).
list(LENGTH failures failureCount)
if(NOT failureCount EQUAL 0)
    string(REPLACE ";" "\n  " rendered "${failures}")
    message(FATAL_ERROR
        "read-buffer-guard-selftest: ${failureCount} of ${caseCount} case(s) failed:\n  ${rendered}")
endif()

message(STATUS "read-buffer-guard-selftest: ${caseCount} case(s) ran; the check refuses each thing it claims and nothing else")
