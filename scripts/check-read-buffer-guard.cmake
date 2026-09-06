# SPDX-License-Identifier: Apache-2.0
#
# Every `ISocket::Read` in `Net/` refuses an empty buffer, and this is what makes
# that a fact rather than six people having remembered.
#
# `Detail::RequireReadBuffer` (`FastCache/Net/ISocket.hpp`) is #838's guard: `0` on
# that interface means *the peer has finished sending*, and every transport's receive
# primitive answers `0` for a zero-length request, so an empty span is answered with a
# graceful close that never happened.
#
# **The guard is purely ADDITIVE, and that is the whole reason this check exists.**
# `Detail::ClaimReadSlot` next door needs no such scan because it IS the claim -- the
# arm site has to clear the slot, and clearing it is the call -- so an arm site cannot
# omit the guard without omitting the operation. `RequireReadBuffer` is a line a
# transport may simply not have. Measured rather than argued: with the call deleted
# from `EpollSocket::Read`, the transport every Linux deployment reads through,
# `empty-read-buffer-canary` PASSES and the whole suite reports 3316 of 3316. The
# canary aborts at the first violation, so it can only ever watch ONE site whatever it
# picks, and it watches `InMemorySocket`. That is #492's shape: exact about the site it
# knows, silent about the five it does not, and silence reads identically to complete
# coverage.
#
# **It DERIVES the set rather than tabulating it.** A hand-kept list of six paths is
# the same defect one level up: a seventh transport joins no list, asserts nothing and
# passes. So it walks `src/FastCache/Net/` and finds every definition of the signature.
#
# **TWO clauses per site, and the second is not decoration.** The body must call the
# guard, AND that call must stand before the body's first `return`. All six open with
# `if (_closed) return ...`, so a guard that drifts below that arm is skipped for
# exactly the socket a caller is most likely to be confused about -- while a
# presence-only scan goes on reading green.
#
# **Test fakes are out of scope, and that is a decision.** `src/tests/`'s
# `ScriptedSocket` and `SocketDecorator` implement `Read` too, and a fixture handing
# one an empty span is the fixture's business rather than a broken transport. What
# this rule is about is what the LIBRARY hands out. `Net/InMemoryTransport.cpp` is in
# scope because it is in `Net/` and is the transport the whole suite reads through.
#
# Runs as `cmake -P`, for the reason `check-net-boundary.cmake` states: this compares
# strings and reports, so a .sh + .ps1 pair would be two implementations of one rule
# differing only in syntax. Its registration therefore carries
# `FAIL_REGULAR_EXPRESSION`, and that property rather than the exit code is the
# verdict -- the measurement and the reasons live in
# `scripts/check-script-check-signals.cmake` and are deliberately not restated (#565).
#
# **It fails CLOSED on an empty scan, twice.** No source file found, or no
# implementation matched, is the check being broken rather than the tree being clean --
# a renamed signature, a moved directory or a mistyped pattern all produce zero, and
# every one of them would otherwise report that every transport complies, forever.
#
# **It never splits a list, so a bracket cannot merge two elements.** #518 measured
# `file(STRINGS)` readers silently mis-reporting on one stray `]`, and C++ is full of
# `[[nodiscard]]`. This reads whole files with `file(READ)` and walks them with
# `string(FIND)`/`string(SUBSTRING)`, which is immune by construction rather than by
# escaping -- the same answer `check-tsan-scope.cmake` arrived at, and for the same
# reason its remedy could not be the usual one.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-read-buffer-guard.cmake
#
# Exit codes: 0 = every transport guards. 1 = one does not. (But read the OUTPUT, not
# the code -- see above.)

cmake_minimum_required(VERSION 3.28)

# What a transport's read entry point looks like, and what the guard is spelled as.
# The signature is matched WITHOUT its return type so a future `[[nodiscard]]` or a
# reformat cannot silently shrink the set; the class name is recovered from the text
# in front of it, purely so a refusal can name the transport.
set(FastCachedReadSignature "::Read(std::span<std::byte> buffer)")
set(FastCachedReadGuardCall "Detail::RequireReadBuffer(buffer)")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

set(netRoot "${FASTCACHED_SOURCE_DIR}/src/FastCache/Net")
if(NOT IS_DIRECTORY "${netRoot}")
    message(FATAL_ERROR "'${netRoot}' is not a directory. Is FASTCACHED_SOURCE_DIR the source root?")
endif()

file(GLOB netSources LIST_DIRECTORIES false "${netRoot}/*.cpp")
list(FILTER netSources EXCLUDE REGEX "_test\\.cpp$")

# `list(LENGTH)` and not `if(netSources STREQUAL "")`: an unset variable compares
# against its own NAME, so the guard against scanning nothing would be false in
# exactly the case it is for (#518, and `check-net-boundary.cmake`'s own instance).
list(LENGTH netSources scannedCount)
if(scannedCount EQUAL 0)
    message(FATAL_ERROR
        "This check walked '${netRoot}' and found no non-test source at all. That is the check "
        "being broken, not the tree being clean -- and it would report success on every run from "
        "here on. Fix the glob in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

set(violations "")
set(implementationCount 0)

# DERIVED, never written out. An earlier draft hardcoded `34` here for a signature that
# is 35 characters -- benign, because the extra character landed on the closing `)`
# which matches neither of the two things the walk looks for next, and because the
# cursor advance was wrong by the same one in the other direction. Two errors
# cancelling is not a reason to keep either: it is a second source of truth for a
# constant this file already holds as data, in the check whose own header argues it
# DERIVES its set rather than tabulating it. Nothing in the selftest could have caught
# it, which is the part worth recording.
string(LENGTH "${FastCachedReadSignature}" signatureLength)

foreach(source IN LISTS netSources)
    file(READ "${source}" text)

    # Comments are stripped before anything is measured, because a COMMENT IS NOT A
    # CALL SITE and this file's own prose names both the guard and `return`. Stripped
    # to end-of-line rather than only whole-line, so a trailing `// ... return ...`
    # beside real code cannot move the first-exit position either.
    string(REGEX REPLACE "//[^\n]*" "" text "${text}")

    file(RELATIVE_PATH shownPath "${FASTCACHED_SOURCE_DIR}" "${source}")

    set(cursor 0)
    string(LENGTH "${text}" textLength)
    while(TRUE)
        string(SUBSTRING "${text}" ${cursor} -1 rest)
        string(FIND "${rest}" "${FastCachedReadSignature}" signatureAt)
        if(signatureAt EQUAL -1)
            break()
        endif()

        # The class name, purely for the report: the identifier immediately before the
        # `::`. Recovered from a bounded window rather than the whole file, so a long
        # source cannot make the regex quadratic.
        set(className "<unknown>")
        if(signatureAt GREATER 0)
            math(EXPR windowStart "${signatureAt} - 64")
            if(windowStart LESS 0)
                set(windowStart 0)
            endif()
            math(EXPR windowLength "${signatureAt} - ${windowStart}")
            string(SUBSTRING "${rest}" ${windowStart} ${windowLength} window)
            if(window MATCHES "([A-Za-z_][A-Za-z0-9_]*)$")
                set(className "${CMAKE_MATCH_1}")
            endif()
        endif()

        # A DECLARATION ends in `;` and has no body; only a definition can carry the
        # guard, and refusing a header declaration would make the rule unstatable.
        #
        # Every offset below is kept in ONE coordinate system -- positions within
        # `rest` -- rather than the three the first draft used (`rest`, then a `tail`
        # slice, then a `bodyRest` slice, summed back together at the advance). That is
        # where the hardcoded length above hid: with three origins, an error in one is
        # cancelled by an error in another and the walk still lands correctly.
        math(EXPR afterSignature "${signatureAt} + ${signatureLength}")
        string(SUBSTRING "${rest}" ${afterSignature} -1 tail)
        string(FIND "${tail}" "{" braceAt)
        string(FIND "${tail}" ";" semicolonAt)
        if(braceAt EQUAL -1)
            break()
        endif()
        if(NOT semicolonAt EQUAL -1 AND semicolonAt LESS braceAt)
            math(EXPR cursor "${cursor} + ${afterSignature}")
            continue()
        endif()

        # The body: from the opening brace to the first `}` at column 0. Every one of
        # these is a top-level function definition, so that terminator is exact; a
        # brace-counting walk would be a second, weaker way to answer the same
        # question.
        math(EXPR bodyStart "${afterSignature} + ${braceAt} + 1")
        string(SUBSTRING "${rest}" ${bodyStart} -1 bodyRest)
        string(FIND "${bodyRest}" "\n}" bodyEnd)
        if(bodyEnd EQUAL -1)
            list(APPEND violations
                 "${shownPath}: ${className}::Read has no terminating '}' at column 0, so this check cannot read its body")
            break()
        endif()
        string(SUBSTRING "${bodyRest}" 0 ${bodyEnd} body)

        math(EXPR implementationCount "${implementationCount} + 1")

        string(FIND "${body}" "${FastCachedReadGuardCall}" guardAt)
        # `return` rather than `return ` or `co_return`: the substring is inside both
        # spellings, and what is wanted is the FIRST way out of the body whatever it
        # is called.
        string(FIND "${body}" "return" returnAt)

        if(guardAt EQUAL -1)
            list(APPEND violations
                 "${shownPath}: ${className}::Read does not call ${FastCachedReadGuardCall}, so an empty span is answered EOF there (#838)")
        elseif(NOT returnAt EQUAL -1 AND guardAt GREATER returnAt)
            list(APPEND violations
                 "${shownPath}: ${className}::Read calls ${FastCachedReadGuardCall} only AFTER its first return, so every early exit skips it (#838)")
        endif()

        # `bodyStart` is ALREADY in `rest` coordinates, so this is one term and not two.
        #
        # It was two, briefly, and that is worth the space: the change that moved every
        # offset into one coordinate system -- made precisely to remove where an
        # off-by-one had been hiding -- left this line summing `afterSignature` a second
        # time. The cursor then over-advanced by the whole signature, so the walk SKIPPED
        # any second `Read` definition in a file whenever the first sat more than a few
        # hundred bytes in, which is every real source here. Measured on a synthetic
        # tree: a guarded implementation followed by an UNGUARDED one, 200 lines of
        # filler ahead of them, reported `1 implementation(s) ... all refusing` and
        # PASSED.
        #
        # A false pass in the instrument built to stop a false pass -- #492's shape one
        # level up, in the check whose own header argues against it. The six original
        # self-test cases could not see it because every one is a single implementation
        # at offset zero; case 7 exists for exactly this and pads its file deliberately.
        math(EXPR cursor "${cursor} + ${bodyStart}")
        if(cursor GREATER_EQUAL textLength)
            break()
        endif()
    endwhile()
endforeach()

# The second emptiness guard, and it answers a different question from the first. The
# tree can be full of sources while the SIGNATURE pattern matches none of them --
# renamed, reformatted, moved -- and "no violations" over an empty set is the most
# confident wrong answer this check could give.
if(implementationCount EQUAL 0)
    message(FATAL_ERROR
        "This check read ${scannedCount} source(s) under '${netRoot}' and found NO implementation of "
        "'${FastCachedReadSignature}'. Every transport would 'comply' from here on. The signature has "
        "moved or been reformatted; fix the pattern in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH violations violationCount)
if(NOT violationCount EQUAL 0)
    string(REPLACE ";" "\n  " rendered "${violations}")
    message(FATAL_ERROR
        "read-buffer-guard: ${violationCount} of ${implementationCount} ISocket::Read implementation(s) do not "
        "refuse an empty buffer:\n  ${rendered}\n\n"
        "Every transport calls Detail::RequireReadBuffer(buffer) as the FIRST statement of Read. `0` on this "
        "interface means the peer has finished sending, so a zero-length read hands the caller a graceful close "
        "that never happened -- see FastCache/Net/ISocket.hpp and issue #838. The canary watches one site only "
        "and cannot see this.")
endif()

message(STATUS
    "read-buffer-guard: ${implementationCount} ISocket::Read implementation(s) across ${scannedCount} "
    "source(s), all refusing an empty buffer before their first return")
