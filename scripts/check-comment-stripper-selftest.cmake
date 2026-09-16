# SPDX-License-Identifier: Apache-2.0
#
# Drives `fastcached_strip_comment_line` in `scripts/lib/CheckCommon.cmake` over single lines.
#
# ## Why this file exists at all
#
# Eight checks read C++ through that one function -- `check-test-loops`, `check-cli-text-cell`,
# `check-enumerator-walks`, `check-istreambuf-iterator`, `check-markup-entities`,
# `check-ranges-seam`, `check-selftest-registered`, `check-target-pragmas` -- and a defect in it
# is a defect in all of them at once, silent in the direction that matters. It had one.
#
# A comment introducer inside a STRING LITERAL was read as a real introducer. On this tree
# `src/apps/fastcache-cli/SocketExchange.cpp:410` carries `"...Accept: */*\r\n..."`, whose three
# characters `*` `/` `*` contain a `/*`; nothing closed it, and the remaining 51 lines of that
# file -- a `for (;;)` among them -- were blanked while the caller printed a clean count over
# lines it had never read. The sibling shape, a `//` inside a literal, truncated 21 lines across
# 16 files, losing text like `//") == 0)`: the rest of a `REQUIRE`.
#
# The function's own header had NAMED the blind spot for as long as it existed. It named it in
# the wrong direction -- "blind to either introducer inside a STRING LITERAL, as every
# regex-shaped reader here is" reads as a false-POSITIVE risk, and being blind to a MENTION
# costs nothing. Being blind to a USE is the whole job. That is why this file asserts what
# SURVIVES stripping and not only what is removed: a self-test written for the removal half
# passes under the entire defect.
#
# ## One case table, run against the shipped library and against six neuters
#
# A pure function over a line has no environment to stage, so the cases run in process. What
# they cannot do by themselves is fail: every one of them passes over a function that returns
# its input unchanged for the SURVIVES half, and over one that returns the empty string for the
# GONE half. So the table is ALSO run against six copies of the library with one decision
# removed from each, and a neuter that provokes NOTHING is reported as this file's own failure
# rather than as a pass -- three times in #1456 the fixture was the hole rather than the code.
#
# The neuter runs are subprocesses, because a neutered copy cannot be included beside the real
# one in a single CMake scope, and their verdict is read from their OUTPUT: a child that could
# not START would otherwise be indistinguishable from one that found nothing. Each child prints
# `CASES-RAN:` before any verdict is believed.
#
# ## What is deliberately NOT asserted as exact text
#
# Most cases name a substring that must survive and one that must be gone, rather than the whole
# stripped line. Whitespace here is a consequence of where a comment sat, and pinning it would
# make every case fail on a change that alters no verdict -- a self-test people then edit rather
# than read. Where the exact text IS the subject, `CaseExact` says so, and the neuters are what
# prove a substring case can fail at all.
cmake_minimum_required(VERSION 3.28)

set(FASTCACHED_STRIPPER_SOURCE "${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

# ---------------------------------------------------------------------------
# Verdicts. A failing assertion prints and is counted; it does not end the run, or the cases
# after the first failure would be reported as neither passed nor failed.

set(caseCount 0)
set(failureCount 0)
set(failedNames "")

## Record one expectation.
## @param name The case name.
## @param what What was being asserted, for the diagnostic.
## @param ok TRUE when the expectation held.
## @param detail Text describing what was observed.
function(Expect name what ok detail)
    math(EXPR caseCount "${caseCount} + 1")
    set(caseCount "${caseCount}" PARENT_SCOPE)
    if(ok)
        return()
    endif()
    message(STATUS "  ${name} / ${what}: FAILED")
    message(STATUS "      ${detail}")
    math(EXPR failureCount "${failureCount} + 1")
    set(failureCount "${failureCount}" PARENT_SCOPE)
    list(APPEND failedNames "${name}")
    set(failedNames "${failedNames}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases.
#
# `SURVIVES` and `GONE` are matched with `string(FIND)` and never as regexes: the needles are
# C++ fragments full of `(`, `)`, `*` and `.`, and a needle silently read as a pattern is a
# case that passes for a reason nobody chose.

## One case, asserted by substring.
## Keywords: NAME, LINE; options IN_BLOCK, SKIP, OPEN; multi-value SURVIVES, GONE.
function(Case)
    cmake_parse_arguments(PARSE_ARGV 0 arg "IN_BLOCK;SKIP;OPEN" "NAME;LINE" "SURVIVES;GONE")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Case: unrecognised argument(s) '${arg_UNPARSED_ARGUMENTS}'")
    endif()
    if(arg_IN_BLOCK)
        set(entering TRUE)
    else()
        set(entering FALSE)
    endif()
    fastcached_strip_comment_line("${arg_LINE}" "${entering}" code skip open)

    foreach(needle IN LISTS arg_SURVIVES)
        string(FIND "${code}" "${needle}" at)
        if(at EQUAL -1)
            set(held FALSE)
        else()
            set(held TRUE)
        endif()
        Expect("${arg_NAME}" "survives <${needle}>" "${held}" "stripped to: [${code}]")
    endforeach()
    foreach(needle IN LISTS arg_GONE)
        string(FIND "${code}" "${needle}" at)
        if(at EQUAL -1)
            set(held TRUE)
        else()
            set(held FALSE)
        endif()
        Expect("${arg_NAME}" "removed <${needle}>" "${held}" "stripped to: [${code}]")
    endforeach()

    if(arg_SKIP)
        set(wantSkip TRUE)
    else()
        set(wantSkip FALSE)
    endif()
    if(skip)
        set(gotSkip TRUE)
    else()
        set(gotSkip FALSE)
    endif()
    string(COMPARE EQUAL "${gotSkip}" "${wantSkip}" skipHeld)
    Expect("${arg_NAME}" "skip is ${wantSkip}" "${skipHeld}" "skip came back ${gotSkip}")

    if(arg_OPEN)
        set(wantOpen TRUE)
    else()
        set(wantOpen FALSE)
    endif()
    if(open)
        set(gotOpen TRUE)
    else()
        set(gotOpen FALSE)
    endif()
    string(COMPARE EQUAL "${gotOpen}" "${wantOpen}" openHeld)
    Expect("${arg_NAME}" "block open is ${wantOpen}" "${openHeld}" "open came back ${gotOpen}")

    set(caseCount "${caseCount}" PARENT_SCOPE)
    set(failureCount "${failureCount}" PARENT_SCOPE)
    set(failedNames "${failedNames}" PARENT_SCOPE)
endfunction()

## One case whose exact stripped text is the subject.
## @param name The case name.
## @param entering Whether a block comment was open when the line began.
## @param line The line.
## @param expected The required stripped text.
function(CaseExact name entering line expected)
    fastcached_strip_comment_line("${line}" "${entering}" code skip open)
    string(COMPARE EQUAL "${code}" "${expected}" held)
    Expect("${name}" "exact text" "${held}" "expected [${expected}] got [${code}]")
    set(caseCount "${caseCount}" PARENT_SCOPE)
    set(failureCount "${failureCount}" PARENT_SCOPE)
    set(failedNames "${failedNames}" PARENT_SCOPE)
endfunction()

## Every case, in one place, so the shipped library and each neuter are asked the same thing.
function(RunEveryCase)
    # -- Lines with nothing to strip, and the ordinary comment shapes.
    CaseExact("plainCode" FALSE "int total = Sum(values);" "int total = Sum(values);")
    CaseExact("lineComment" FALSE "int total = Sum(values); // add them up"
              "int total = Sum(values); ")
    CaseExact("wholeLineComment" FALSE "// just prose" "")
    Case(NAME "inlineBlock" LINE "int total = /* why */ Sum(values);"
         SURVIVES "int total =" "Sum(values);" GONE "why")

    # A comment must not JOIN the tokens it sat between: `inty` is an identifier this reader
    # would have invented, and a match on an invented token is worse than a missed one.
    Case(NAME "joinedTokens" LINE "int/*x*/y = 1;" SURVIVES "int y" GONE "inty")

    # Code between two block comments on ONE line. CMake's regex engine is greedy with no lazy
    # quantifier, so the old `REGEX REPLACE "/\\*.*\\*/"` took everything from the FIRST `/*` to
    # the LAST `*/` and this middle vanished. 113 lines of this tree have the shape.
    Case(NAME "twoBlocksOneLine" LINE "int a = /*xx*/ 1 + /*yy*/ 2;"
         SURVIVES "1 +" "2;" GONE "xx" "yy")

    Case(NAME "blockOpens" LINE "Foo(); /* opening" SURVIVES "Foo()" GONE "opening" OPEN)
    CaseExact("blockContinues" TRUE "   still inside the comment" "")
    Case(NAME "blockContinuesFlags" IN_BLOCK LINE "   still inside the comment"
         SKIP OPEN)
    Case(NAME "blockCloses" IN_BLOCK LINE "   done */ Resume();"
         SURVIVES "Resume()" GONE "done")
    Case(NAME "blockMentionsLineComment" LINE "/* a // inside a block */ Go();"
         SURVIVES "Go()" GONE "inside")

    # The historical defect, and the reason the two introducers are decided POSITIONALLY: a
    # LINE comment mentioning `/*` must not open a block comment nothing ever closes.
    Case(NAME "lineCommentMentionsBlock" LINE "Go(); // see /* elsewhere"
         SURVIVES "Go()" GONE "elsewhere")

    # -- A string literal's contents are CODE. This is the half the defect lived in.
    #
    # The subject verbatim: `SocketExchange.cpp:410`. Before the fix this opened a block
    # comment, nothing closed it, and the 51 lines after it -- including a `for (;;)` -- were
    # invisible to a scan that reported clean.
    Case(NAME "slashStarInsideALiteral"
         LINE "    auto request = std::format(\"GET / HTTP/1.1 Accept: */* close\");"
         SURVIVES "std::format" "Accept: */*" "close\");")
    Case(NAME "doubleSlashInsideALiteral" LINE "REQUIRE(Fold(\"a//b\") == 0);"
         SURVIVES "a//b" "== 0);")
    Case(NAME "urlInALiteral" LINE "auto url = std::string(\"https://host/x\"); Go();"
         SURVIVES "https://host/x" "Go()")
    Case(NAME "escapedQuoteInALiteral" LINE "auto s = \"a\\\"b//c\"; Go();"
         SURVIVES "b//c" "Go()")
    Case(NAME "escapedBackslashThenClose" LINE "auto p = \"C:\\\\\"; // trailing"
         SURVIVES "C:\\\\" GONE "trailing")
    Case(NAME "commentAfterALiteral" LINE "Log(\"done\"); // note"
         SURVIVES "Log(\"done\");" GONE "note")
    Case(NAME "blockAfterALiteral" LINE "Log(\"done\"); /* note"
         SURVIVES "Log(\"done\");" GONE "note" OPEN)
    Case(NAME "literalThenBlockThenCode" LINE "Log(\"a\"); /*note*/ More();"
         SURVIVES "More()" GONE "note")

    # -- The two limits that remain, pinned so they are decisions rather than discoveries.
    #
    # `'` is NOT an opener: 262 lines of this tree are digit separators, and treating a
    # quote-shaped character as a char literal would blank the tail of every one of them.
    Case(NAME "digitSeparators" LINE "constexpr auto n = 1'000'000; // count"
         SURVIVES "1'000'000;" GONE "count")

    # Which means a `'\"'` opens a literal that usually does not close on its line. The tail is
    # then KEPT as code -- so this comment survives, and that is the fail-CLOSED direction: a
    # finding somebody can see and exempt beats a clean report over text that was dropped.
    Case(NAME "charLiteralHoldingAQuote" LINE "if (c == '\"' || c == '\\\\') // escape"
         SURVIVES "escape")
    Case(NAME "unterminatedLiteralKeepsTheTail" LINE "auto s = \"unclosed // tail"
         SURVIVES "tail")

    # A raw string's first line has an unterminated literal by construction. 10 here.
    Case(NAME "rawStringFirstLine" LINE "constexpr std::string_view Css = R\"CSS("
         SURVIVES "R\"CSS(")

    set(caseCount "${caseCount}" PARENT_SCOPE)
    set(failureCount "${failureCount}" PARENT_SCOPE)
    set(failedNames "${failedNames}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Child mode: run the table against the library named on the command line and report per case.
# The verdict is the OUTPUT, and `CASES-RAN:` is printed before it so a parent can tell a child
# that found nothing from one that never started.

if(DEFINED FASTCACHED_STRIPPER_LIBRARY)
    include("${FASTCACHED_STRIPPER_LIBRARY}")
    RunEveryCase()
    list(REMOVE_DUPLICATES failedNames)
    message("CASES-RAN: ${caseCount}")
    foreach(name IN LISTS failedNames)
        message("CASE-FAILED: ${name}")
    endforeach()
    message("CHILD-DONE")
else()
    include("${FASTCACHED_STRIPPER_SOURCE}")
    RunEveryCase()
    set(shippedFailures "${failureCount}")
    set(shippedCases "${caseCount}")

    if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
        message(FATAL_ERROR
            "comment-stripper-selftest: FASTCACHED_SCRATCH_DIR is required -- the neuter runs "
            "need somewhere to write a modified copy of the library.")
    endif()
    file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}")
    file(MAKE_DIRECTORY "${FASTCACHED_SCRATCH_DIR}")
    file(READ "${FASTCACHED_STRIPPER_SOURCE}" pristine)

    set(neuterCount 0)
    set(inertNeuters "")

    ## Remove one decision from the library and require the table to notice.
    ##
    ## Three ways this can go wrong and all three are reported rather than passed: the
    ## replacement matching nothing (the neuter never happened -- #1450 shipped one that
    ## matched a COMMENT naming the script and reported itself inert), the child failing to
    ## start, and the child running the whole table with nothing to say.
    ##
    ## @param name What was removed.
    ## @param needle The text to replace.
    ## @param replacement What to put in its place.
    ## @param expected A case name that MUST be among the failures, or "" for any failure.
    function(Neuter name needle replacement expected)
        math(EXPR neuterCount "${neuterCount} + 1")
        set(neuterCount "${neuterCount}" PARENT_SCOPE)

        string(FIND "${pristine}" "${needle}" at)
        if(at EQUAL -1)
            message(STATUS "  neuter ${name}: FAILED -- its anchor is not in the library")
            message(STATUS "      <${needle}>")
            math(EXPR failureCount "${failureCount} + 1")
            set(failureCount "${failureCount}" PARENT_SCOPE)
            return()
        endif()
        string(REPLACE "${needle}" "${replacement}" broken "${pristine}")
        string(COMPARE EQUAL "${broken}" "${pristine}" unchanged)
        if(unchanged)
            message(STATUS "  neuter ${name}: FAILED -- the replacement changed nothing")
            math(EXPR failureCount "${failureCount} + 1")
            set(failureCount "${failureCount}" PARENT_SCOPE)
            return()
        endif()

        set(copy "${FASTCACHED_SCRATCH_DIR}/${name}.cmake")
        file(WRITE "${copy}" "${broken}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}"
                    "-DFASTCACHED_STRIPPER_LIBRARY=${copy}"
                    -P "${CMAKE_CURRENT_LIST_FILE}"
            OUTPUT_VARIABLE childOut
            ERROR_VARIABLE childErr
            RESULT_VARIABLE childStatus)
        set(childText "${childOut}${childErr}")

        string(FIND "${childText}" "CASES-RAN:" ranAt)
        string(FIND "${childText}" "CHILD-DONE" doneAt)
        if(ranAt EQUAL -1 OR doneAt EQUAL -1)
            # Not "no failures" -- INCONCLUSIVE, which is its own outcome. A child that could
            # not start looks exactly like a neuter that provoked nothing.
            message(STATUS "  neuter ${name}: FAILED -- the child did not run the table")
            message(STATUS "      status ${childStatus}, output: ${childText}")
            math(EXPR failureCount "${failureCount} + 1")
            set(failureCount "${failureCount}" PARENT_SCOPE)
            return()
        endif()

        string(REGEX MATCHALL "CASE-FAILED: [A-Za-z]+" broke "${childText}")
        list(LENGTH broke brokeCount)
        if(brokeCount EQUAL 0)
            message(STATUS "  neuter ${name}: FAILED -- INERT, the table noticed nothing")
            list(APPEND inertNeuters "${name}")
            set(inertNeuters "${inertNeuters}" PARENT_SCOPE)
            math(EXPR failureCount "${failureCount} + 1")
            set(failureCount "${failureCount}" PARENT_SCOPE)
            return()
        endif()
        if(NOT expected STREQUAL "")
            string(FIND "${childText}" "CASE-FAILED: ${expected}" wantedAt)
            if(wantedAt EQUAL -1)
                message(STATUS
                    "  neuter ${name}: FAILED -- it broke ${brokeCount} case(s) but not "
                    "<${expected}>, which is the one it exists to break")
                message(STATUS "      broke: ${broke}")
                math(EXPR failureCount "${failureCount} + 1")
                set(failureCount "${failureCount}" PARENT_SCOPE)
                return()
            endif()
        endif()
        message(STATUS "  neuter ${name}: ${brokeCount} case(s) failed, as required")
    endfunction()

    # (1) The defect itself: take the QUOTE out of the race and a literal's contents become
    #     comment introducers again.
    Neuter("quoteOutOfTheRace"
"        if(NOT quoteAt EQUAL -1)
            set(first \${quoteAt})
            set(kind \"quote\")
        endif()
"
""
        "slashStarInsideALiteral")

    # (2) Escapes inside a literal. `\"` must not close it.
    Neuter("escapesIgnored"
        "math(EXPR escaped \"\${slashes} % 2\")"
        "set(escaped 0)"
        "escapedQuoteInALiteral")

    # (3) The space a consumed comment leaves behind, without which `int/*x*/y` is one token.
    Neuter("noSpaceForAComment"
        "            string(APPEND code \" \")"
        ""
        "joinedTokens")

    # (4) Positional order between `//` and `/*`. Let `/*` win unconditionally and a line
    #     comment mentioning one opens a block nothing closes.
    Neuter("blockBeatsLine"
        "if(NOT blockAt EQUAL -1 AND (first EQUAL -1 OR blockAt LESS first))"
        "if(NOT blockAt EQUAL -1)"
        "lineCommentMentionsBlock")

    # (5) An unterminated literal must keep its tail rather than drop it.
    Neuter("unterminatedLiteralDropped"
"            string(SUBSTRING \"\${rest}\" \${first} -1 tail)
            string(APPEND code \"\${tail}\")
"
""
        "unterminatedLiteralKeepsTheTail")

    # (6) The fast path. Widened to every line outside a block comment, nothing is stripped at
    #     all -- the broad neuter, which must break most of the table rather than one case.
    Neuter("fastPathTooWide"
        "if(NOT inBlockComment AND NOT line MATCHES \"/[/*]\")"
        "if(NOT inBlockComment)"
        "lineComment")

    # -----------------------------------------------------------------------
    # The counts are printed whatever the verdict: a self-test that stopped early must not look
    # like one that judged something.
    if(failureCount GREATER 0)
        list(REMOVE_DUPLICATES failedNames)
        if(failedNames)
            message(STATUS "failing case(s): ${failedNames}")
        endif()
        if(inertNeuters)
            message(STATUS "inert neuter(s): ${inertNeuters}")
        endif()
        message(FATAL_ERROR
            "comment-stripper-selftest: ${failureCount} failure(s) across ${shippedCases} "
            "assertion(s) on the shipped library and ${neuterCount} neuter(s)")
    endif()
    message(STATUS
        "comment-stripper-selftest: ${shippedCases} assertion(s) ran on the shipped library "
        "with ${shippedFailures} failure(s), and ${neuterCount} neuter(s) each provoked at "
        "least the failure they exist to provoke")
endif()
