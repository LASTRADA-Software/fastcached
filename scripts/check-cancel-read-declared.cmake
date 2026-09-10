# SPDX-License-Identifier: Apache-2.0
#
# Every transport the LIBRARY hands out answers `CancelRead` in its own file, and this
# is what stops a seventh inheriting silence.
#
# `ISocket::CancelRead` ships with a **default no-op**, which is the right shape --
# `.agent/rules/wire-and-protocol.md` records why a pure virtual is the wrong answer
# here: it compels seven fakes to write `{}` with no reason beside it, which is *forgot*
# spelled in the vocabulary of *decided*. Reach for the type system when the obligation
# is DO SOMETHING and for a scan when it is SAY WHY (#892).
#
# **What that default cost, measured rather than argued.** On #710's branch TWO of six
# transports inherited it and BOTH park: `TlsSocket`, whose `WaitReadable` parks on a
# raw read whenever OpenSSL wants more bytes, and `InMemorySocket`, which is asymmetric
# -- `WaitReadable` never parks and `Read` does, which is exactly why the default looked
# safe. A socket that inherits the no-op cannot retire a parked read at all: the
# awaiting coroutine is never resumed and its frame is never freed, with no signal
# anywhere (#663, #710).
#
# **This does not detect parking, and nothing textual can.** It asks the narrower
# question the rule is actually about: did this transport ANSWER, or did it inherit an
# answer it never gave. A `CancelRead` that is a written no-op passes, and should --
# `BlockingSocket`'s is exactly that, with its reason beside it.
#
# **It DERIVES the set rather than tabulating it**, for the reason
# `check-read-buffer-guard.cmake` states next door: a hand-kept list of six transports
# is the same defect one level up, since a seventh joins no list, asserts nothing and
# passes.
#
# **Test doubles are out of scope, and that is a decision.** `src/tests/ScriptedSocket`,
# `SocketDecorator` and the per-app scripted peers implement `ISocket` too, and a fake
# that never parks owes no cancellation. What this rule is about is what the LIBRARY
# hands out, which is the same scope `read-buffer-guard` takes and states. `Net/`'s own
# `*_test.cpp` is excluded on that ground rather than by accident.
#
# **A class's region ends at the next top-level `class`/`struct`, and that boundary is
# load-bearing.** Without it a header holding two transports would let the SECOND one's
# declaration satisfy the FIRST -- a violation hidden by its neighbour, which is the
# case a clean tree cannot exhibit and case 4 of the self-test plants.
#
# Runs as `cmake -P`, so its registration carries `FAIL_REGULAR_EXPRESSION` and THAT
# rather than the exit code is the verdict; the measurement and the reasons live in
# `scripts/check-script-check-signals.cmake` and are deliberately not restated (#565).
#
# **It fails CLOSED on an empty scan, twice.** No header found, or no class deriving
# from `ISocket` matched, is the check being broken rather than the tree being clean --
# a renamed base, a moved directory or a mistyped needle all produce zero, and every one
# of them would otherwise report that every transport answers, forever.
#
# **It never splits a list**, so a bracket cannot merge two elements: it reads whole
# files with `file(READ)` and walks them with `string(FIND)`/`string(SUBSTRING)`, immune
# by construction rather than by escaping (#518).
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-cancel-read-declared.cmake
#
# Exit codes: 0 = every transport answers. 1 = one does not. (But read the OUTPUT, not
# the code -- see above.)

cmake_minimum_required(VERSION 3.28)

# What deriving from the interface looks like, and what answering looks like. The
# needle omits the `final:` / `:` in front so both spellings in this tree match one
# string, and it is matched literally rather than as a regex.
set(FastCachedSocketBase "public ISocket")
set(FastCachedCancelName "CancelRead")

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SOURCE_DIR is not set. Invoke this script as: cmake "
        "-DFASTCACHED_SOURCE_DIR=<source root> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

set(netRoot "${FASTCACHED_SOURCE_DIR}/src/FastCache/Net")
if(NOT IS_DIRECTORY "${netRoot}")
    message(FATAL_ERROR "'${netRoot}' is not a directory. Is FASTCACHED_SOURCE_DIR the source root?")
endif()

file(GLOB netHeaders LIST_DIRECTORIES false "${netRoot}/*.hpp")
list(FILTER netHeaders EXCLUDE REGEX "_test\\.hpp$")

# `list(LENGTH)` and not `if(netHeaders STREQUAL "")`: an unset variable compares
# against its own NAME, so the guard against scanning nothing would be false in exactly
# the case it is for (#518, and `check-net-boundary.cmake`'s own instance).
list(LENGTH netHeaders scannedCount)
if(scannedCount EQUAL 0)
    message(FATAL_ERROR
        "This check walked '${netRoot}' and found no non-test header at all. That is the check "
        "being broken, not the tree being clean -- and it would report success on every run from "
        "here on. Fix the glob in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

set(violations "")
set(transportCount 0)
string(LENGTH "${FastCachedSocketBase}" baseLength)

foreach(header IN LISTS netHeaders)
    file(READ "${header}" text)

    # Comments are stripped before anything is measured, because a COMMENT IS NOT A
    # DECLARATION (#720) -- and this file's own prose names both the base and the
    # method. Stripped to end-of-line rather than only whole-line, so a trailing
    # `// ... CancelRead ...` beside real code cannot answer for a class either.
    #
    # BOTH spellings, and the second is not decoration: stripping only `//` left
    # `/* CancelRead */` and any `/** ... */` Doxygen block satisfying the check --
    # measured, a class whose ONLY mention was inside a block comment was reported
    # compliant. A rule enforced for one spelling of a thing and blind to the other
    # reads exactly like complete coverage, and the self-test's `commented` case could
    # not see it because it plants a `//`.
    #
    # Line comments go FIRST so a `// ... /* ...` cannot open a block that swallows the
    # real code after it. Every way of getting the order wrong deletes a real
    # declaration and so refuses LOUDLY; none of them can make a comment answer.
    string(REGEX REPLACE "//[^\n]*" "" text "${text}")
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" "" text "${text}")

    file(RELATIVE_PATH shownPath "${FASTCACHED_SOURCE_DIR}" "${header}")

    set(cursor 0)
    while(TRUE)
        string(SUBSTRING "${text}" ${cursor} -1 rest)
        string(FIND "${rest}" "${FastCachedSocketBase}" baseAt)
        if(baseAt EQUAL -1)
            break()
        endif()

        # The class name, for the report and for nothing else. Recovered from a bounded
        # window rather than the whole file, so a long header cannot make the regex
        # quadratic.
        set(className "<unknown>")
        if(baseAt GREATER 0)
            math(EXPR windowStart "${baseAt} - 96")
            if(windowStart LESS 0)
                set(windowStart 0)
            endif()
            math(EXPR windowLength "${baseAt} - ${windowStart}")
            string(SUBSTRING "${rest}" ${windowStart} ${windowLength} window)
            if(window MATCHES "class[ \t]+([A-Za-z_][A-Za-z0-9_]*)[^;{]*$")
                set(className "${CMAKE_MATCH_1}")
            endif()
        endif()

        # A forward declaration cannot carry a member, and refusing one would make the
        # rule unstatable. Every offset below stays in ONE coordinate system --
        # positions within `rest` -- which is where an off-by-one hid the last time a
        # walk in this directory used three (#712's cleanup, recorded in
        # `.agent/rules/wire-and-protocol.md`).
        math(EXPR afterBase "${baseAt} + ${baseLength}")
        string(SUBSTRING "${rest}" ${afterBase} -1 tail)
        string(FIND "${tail}" "{" braceAt)
        string(FIND "${tail}" ";" semicolonAt)
        if(braceAt EQUAL -1)
            break()
        endif()
        if(NOT semicolonAt EQUAL -1 AND semicolonAt LESS braceAt)
            math(EXPR cursor "${cursor} + ${afterBase}")
            continue()
        endif()

        # The region: from the opening brace to whichever TOP-LEVEL marker comes first
        # -- this class's own closing `};` at column zero, or the next `class`/`struct`
        # -- or to the end of the file. Not a brace-counting walk, which would have to
        # know about braces inside strings and initializers to answer the same question;
        # and not the whole file, which is what lets one class's answer cover its
        # neighbour's silence.
        #
        # **The closing `};` is what bounds the LAST class in a header**, and without it
        # that region ran to EOF: measured, an ordinary trailing helper --
        # `inline void RetireIt(ISocket& s) { s.CancelRead(); }` -- answered for a
        # transport that declared nothing, and an out-of-line definition of some OTHER
        # class's `CancelRead` does the same. The `class`/`struct` boundary only ever
        # protected a class with a NEIGHBOUR, which is the case a clean tree does have;
        # the unbounded tail is the one it does not.
        #
        # Markers are matched at column zero, as everything top-level in this tree is
        # written; a member's own `};` is indented and so is not one. A boundary this
        # misses can only make the region LONGER, never shorter, so it cannot invent a
        # violation -- and one it finds early can, which is why nothing weaker than a
        # column-zero anchor is used.
        math(EXPR regionStart "${afterBase} + ${braceAt} + 1")
        string(SUBSTRING "${rest}" ${regionStart} -1 regionRest)
        set(regionEnd -1)
        foreach(boundary IN ITEMS "\n};" "\nclass " "\nstruct ")
            string(FIND "${regionRest}" "${boundary}" boundaryAt)
            if(NOT boundaryAt EQUAL -1 AND (regionEnd EQUAL -1 OR boundaryAt LESS regionEnd))
                set(regionEnd ${boundaryAt})
            endif()
        endforeach()
        if(regionEnd EQUAL -1)
            set(region "${regionRest}")
        else()
            string(SUBSTRING "${regionRest}" 0 ${regionEnd} region)
        endif()

        math(EXPR transportCount "${transportCount} + 1")

        string(FIND "${region}" "${FastCachedCancelName}" cancelAt)
        if(cancelAt EQUAL -1)
            list(APPEND violations
                 "${shownPath}: ${className} derives from ISocket and does not declare ${FastCachedCancelName}, so it INHERITS the default no-op -- an answer it never gave")
        endif()

        math(EXPR cursor "${cursor} + ${regionStart}")
    endwhile()
endforeach()

# The second empty-scan refusal, and it is a DIFFERENT question from the first: a
# renamed base leaves the header count healthy and the transport count at zero, and "no
# violations" over an empty set is the most confident wrong answer available (#492).
if(transportCount EQUAL 0)
    message(FATAL_ERROR
        "This check read ${scannedCount} header(s) under '${netRoot}' and found no class deriving "
        "from '${FastCachedSocketBase}'. That is the check being broken, not the tree being clean: "
        "the base may have been renamed or respelled. Fix the needle in ${CMAKE_CURRENT_LIST_FILE}.")
endif()

list(LENGTH violations violationCount)
if(violationCount GREATER 0)
    set(rendered "")
    foreach(violation IN LISTS violations)
        string(APPEND rendered "\n  - ${violation}")
    endforeach()
    message(FATAL_ERROR
        "cancel-read-declared: ${violationCount} transport(s) inherit ISocket::CancelRead's default "
        "no-op:${rendered}\n\n"
        "A socket that inherits it cannot retire a parked read: the awaiting coroutine is never "
        "resumed and its frame is never freed, with no signal anywhere (#663, #710). Two of six "
        "transports inherited it and both parked. Declare it -- a written no-op WITH ITS REASON is "
        "a complete answer, and `BlockingSocket` is the example.")
endif()

message(STATUS
    "cancel-read-declared: ${transportCount} transport(s) across ${scannedCount} header(s) declare "
    "${FastCachedCancelName}; none inherits the default")
