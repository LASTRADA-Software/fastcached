# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy
# the project has not stated -- CMP0057, where `if(... IN_LIST ...)` silently did
# nothing in a check in this tree.
cmake_minimum_required(VERSION 3.28)
#
# A `macro()` parameter read as a BARE name inside `if()` is a literal string, so the
# branch is dead (#1216).
#
# ## The mechanism, in one line
#
# A macro's parameters are TEXT. The body is substituted before it runs, so `${want}`
# becomes the argument and a bare `want` is left standing as the four characters
# `want`. `if(want STREQUAL "want-pass")` therefore compares `want` against
# `want-pass` and is false forever. Inside a `function()` the identical line WORKS,
# because a function's parameters are real variables.
#
# ## Why it is worth a check rather than a rule
#
# Three properties, and the third is what makes a check the only remedy:
#
#   * It is silent in both directions. CMake does not warn, and the dead branch does
#     not fail -- it falls through to whatever comes next, which in a test harness is
#     the weaker assertion.
#   * It reads as correct. The two constructs are one keyword apart.
#   * It CONCENTRATES in harnesses. A macro is chosen over a function precisely when
#     the body must set variables in the caller's scope, which is what a per-case test
#     harness does -- so the construct lands disproportionately in the code whose job
#     is to decide verdicts.
#
# The instance that produced this check cost exactly that: `fastcached_case` in
# `check-corrupt-store-diagnostics-selftest.cmake` had BOTH verdict arms dead, so nine
# cases fell through to a substring check and the two `want-pass` cases -- which pass
# an empty substring -- asserted nothing at all.
#
# ## What it refuses, and what it must NOT
#
# Refused: a bare parameter name in the condition of an `if()`, `elseif()` or
# `while()` inside the `macro()` that declares it.
#
# Not refused: `${param}` and `"${param}"`, which are correct and are what the fix
# looks like; the same shape inside a `function()`, where it works; and a name that
# merely CONTAINS a parameter name, since the match is on whole words.
#
# ## The one legitimate shape this cannot tell apart, and the way out
#
# A macro parameter whose name is ALSO a variable the body sets -- `macro(f status)`
# whose body runs an `execute_process(... RESULT_VARIABLE status)` and then reads
# `if(status EQUAL 0)` -- is reading the VARIABLE, correctly, and this check cannot
# see the difference: at the point of the `if()` both spellings are the same six
# characters. No such site exists in this tree today, and a checker that refused one
# would be refusing correct code.
#
# So there is a marker, and it carries a REASON:
#
#     # macro-bare-parameter: <why this name is a variable here, not the parameter>
#
# in the comment block immediately above the line. The count of markers is PRINTED on
# every run, because a marker nobody reads would spell "forgot" in the vocabulary of
# "decided" -- and a marker with no reason after it is refused outright.
#
# ## Scope, stated rather than left to be inferred
#
# `scripts/` and everything under it. The other two candidate scopes were MEASURED at
# d911b33e rather than assumed, by this check's own rule and again by
# `git grep -c '^macro('` as a second construction: `cmake/**/*.cmake` holds 13 files
# and **0 macro definitions**, and the 13 tracked `CMakeLists.txt` files hold **0**.
# So the shape cannot occur there today -- which is a stronger answer than "no
# violations found", because there is nothing there to violate. Widening the glob when
# a macro first appears in either is a one-line change.
#
# ## Why this file cannot report itself
#
# It defines no `macro()`, so its own walk never enters a macro body and the example
# text in these comments is never examined. That is immunity by construction rather
# than an exclusion, which is the same reason the walk below builds no CMake list: a
# list-free `FIND`/`SUBSTRING` walk cannot have an unbalanced `[` in a comment merge
# two lines and move a reported line number, and the shell dispatch shapes some of
# these files carry ARE brackets.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-macro-bare-parameters.cmake
#
# Exit codes: whatever CMake gives it. The verdict is read from the OUTPUT, because
# that is the rule every check here is registered under.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

file(GLOB_RECURSE cmakeScripts "${FASTCACHED_SOURCE_DIR}/scripts/*.cmake")

# An empty file set would report success having read nothing, which is the vacuous
# pass this whole family of checks exists to leave behind. Two empty lists agree
# perfectly.
if(NOT cmakeScripts)
    message(FATAL_ERROR
        "no scripts/*.cmake was found under ${FASTCACHED_SOURCE_DIR}; this check would "
        "vouch for every macro in the tree while reading none of them")
endif()

set(violations "")
set(fileCount 0)
set(macroCount 0)
set(dataRegions 0)
set(markedCount 0)

foreach(scriptFile IN LISTS cmakeScripts)
    math(EXPR fileCount "${fileCount} + 1")
    file(READ "${scriptFile}" scriptText)

    # Whole-file test first: most of these files define no macro at all, and reading
    # them line by line to learn that is the cost this repository has already paid.
    #
    # `(^|\n)`, never a bare `\n`. A file whose FIRST line is the definition has no
    # newline before it and was skipped ENTIRELY -- its whole body unexamined, with
    # the run reporting the same all-clear over it. Invisible on this tree, where
    # every macro has a licence header above it, and caught on the self-test's first
    # run by a staged file that begins with the definition.
    if(NOT scriptText MATCHES "(^|\n)macro\\(")
        continue()
    endif()
    file(RELATIVE_PATH shownScript "${FASTCACHED_SOURCE_DIR}" "${scriptFile}")

    set(rest "${scriptText}")
    set(lineNumber 0)
    set(macroArgs "")
    set(inMacro FALSE)
    set(pendingReason "")
    set(sawMarker FALSE)
    set(inData FALSE)
    set(dataOpenedAt 0)

    while(NOT rest STREQUAL "")
        string(FIND "${rest}" "\n" newlineAt)
        if(newlineAt EQUAL -1)
            set(line "${rest}")
            set(rest "")
        else()
            string(SUBSTRING "${rest}" 0 ${newlineAt} line)
            math(EXPR afterNewline "${newlineAt} + 1")
            string(SUBSTRING "${rest}" ${afterNewline} -1 rest)
        endif()
        math(EXPR lineNumber "${lineNumber} + 1")

        # ## A macro in a STRING is not a macro
        #
        # This is a line walk over CMake source, so it cannot tell code from a
        # multi-line string literal holding code -- and a self-test's whole job is to
        # hold violating source as DATA. The definition test is anchored at column
        # zero, which shields a body written as `"macro(...)` (the quote is in the
        # way) and does NOT shield a second macro on a continuation line, so the
        # discrimination this check would otherwise rely on is an accident of where a
        # test author put a quote. Measured: a staged two-macro body makes this check
        # report a finding in its own self-test and refuse a correct tree.
        #
        # So a file that matches its own scan by construction exempts a REGION, never
        # ITSELF -- a whole-file exemption would blind the scan to the rest of a file
        # that also holds real code, which is the trade `check-e2e-helpers.sh` already
        # settled for the bash-3.2 scan.
        #
        # The region is BALANCED or the file is refused. An unterminated `data-begin`
        # would otherwise silence every remaining line of the file while the run
        # reports the same all-clear -- an exemption that widens itself is the failure
        # this check exists to prevent, one level up.
        if(line MATCHES "^[ \t]*#[ \t]*macro-bare-parameter-scan:[ \t]*data-begin[ \t]*$")
            if(inData)
                list(APPEND violations
                     "${shownScript}:${lineNumber}: a `data-begin` is already open from line ${dataOpenedAt} -- nested exempt regions are refused, because the `data-end` that closes them is then ambiguous")
            endif()
            set(inData TRUE)
            set(dataOpenedAt ${lineNumber})
            math(EXPR dataRegions "${dataRegions} + 1")
            continue()
        endif()
        if(line MATCHES "^[ \t]*#[ \t]*macro-bare-parameter-scan:[ \t]*data-end[ \t]*$")
            if(NOT inData)
                list(APPEND violations
                     "${shownScript}:${lineNumber}: a `data-end` closes no open `data-begin`, so this scan's exempt regions do not balance and nobody can say which lines were examined")
            endif()
            set(inData FALSE)
            continue()
        endif()
        if(inData)
            continue()
        endif()

        # A COMMENT is not a call site. It is where a marker is DECLARED, though, so
        # one is collected here and spent on the next line of code -- which is what
        # makes "the comment block immediately above" the only place it can go.
        if(line MATCHES "^[ \t]*#")
            if(line MATCHES "macro-bare-parameter:[ \t]*(.*)$")
                string(STRIP "${CMAKE_MATCH_1}" pendingReason)
                set(sawMarker TRUE)
            endif()
            continue()
        endif()

        # `endmacro` before `macro`, so `endmacro(` cannot be read as a definition.
        if(line MATCHES "^[ \t]*endmacro\\(")
            set(inMacro FALSE)
            set(macroArgs "")
            set(pendingReason "")
            set(sawMarker FALSE)
            continue()
        endif()

        if(line MATCHES "^macro\\(")
            math(EXPR macroCount "${macroCount} + 1")
            # A definition this parser cannot read is its own outcome, never a file
            # quietly skipped: a macro whose parameter list runs over several lines
            # would otherwise leave its whole body unexamined while the run reports
            # the same "all clear".
            # A plain group, not `(?:...)`: CMake's regex engine has no
            # non-capturing group and fails to COMPILE on one.
            if(NOT line MATCHES "^macro\\([ \t]*([A-Za-z_0-9]+)(([ \t]+[A-Za-z_0-9]+)*)[ \t]*\\)[ \t]*$")
                list(APPEND violations
                     "${shownScript}:${lineNumber}: this scan cannot read the macro definition `${line}` -- its body is therefore unexamined, and a verdict drawn from a scan that lost its subject is worth nothing")
                set(inMacro FALSE)
                set(macroArgs "")
                continue()
            endif()
            set(inMacro TRUE)
            # ARGN is a real variable inside a macro, so it is not a parameter this
            # rule is about and reading it bare is correct.
            set(macroArgs "")
            string(REGEX MATCHALL "[A-Za-z_0-9]+" declaredArgs "${CMAKE_MATCH_2}")
            foreach(argName IN LISTS declaredArgs)
                if(NOT argName STREQUAL "ARGN")
                    list(APPEND macroArgs "${argName}")
                endif()
            endforeach()
            set(pendingReason "")
            set(sawMarker FALSE)
            continue()
        endif()

        if(inMacro AND line MATCHES "^[ \t]*(else)?if\\((.*)$|^[ \t]*while\\((.*)$")
            set(conditionBody "${CMAKE_MATCH_2}${CMAKE_MATCH_3}")

            # Quoted spans go first, then `${...}` expansions: what is left is the
            # text CMake would read as variable names, which is exactly the question.
            string(REGEX REPLACE "\"[^\"]*\"" " " bareText "${conditionBody}")
            string(REGEX REPLACE "[$][{][^}]*[}]" " " bareText "${bareText}")
            # CMake's regex engine has no word-boundary escape, so the operand is
            # padded and the boundaries are spelled as character classes.
            set(paddedText " ${bareText} ")

            foreach(argName IN LISTS macroArgs)
                if(paddedText MATCHES "[^A-Za-z_0-9]${argName}[^A-Za-z_0-9]")
                    if(sawMarker AND NOT pendingReason STREQUAL "")
                        math(EXPR markedCount "${markedCount} + 1")
                    elseif(sawMarker)
                        list(APPEND violations
                             "${shownScript}:${lineNumber}: `macro-bare-parameter:` with no reason after it -- a marker nobody can read spells `forgot` in the vocabulary of `decided`")
                    else()
                        list(APPEND violations
                             "${shownScript}:${lineNumber}: macro parameter `${argName}` is read as a BARE name, so this compares the literal string `${argName}` and the branch is dead")
                    endif()
                endif()
            endforeach()
        endif()

        set(pendingReason "")
        set(sawMarker FALSE)
    endwhile()

    # End of file with a region still open: every line after `dataOpenedAt` was
    # skipped, so this file's all-clear covers a body nobody read.
    if(inData)
        list(APPEND violations
             "${shownScript}:${dataOpenedAt}: this `data-begin` is never closed, so every line below it was skipped and this file's clean verdict describes only the lines above it")
    endif()
endforeach()

# A count of BAD things says nothing about whether the good things exist. If this
# scan ever stops recognising `macro(` -- a reformat, a definition style it cannot
# read -- every file passes while nothing is examined, and the summary below reads
# exactly the same.
if(macroCount EQUAL 0)
    message(FATAL_ERROR
        "read ${fileCount} scripts/*.cmake file(s) and found no macro definition at all; "
        "this tree has several, so this scan has stopped recognising them and has "
        "vouched for every one without reading it")
endif()

if(violations)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("In CMake a macro's parameters are TEXT, not variables: the body is")
    message("substituted before it runs, so a bare name is left standing as itself.")
    message("Write the expansion:")
    message("")
    message("    if(\"\${param}\" STREQUAL \"value\")")
    message("")
    message("A `function()` needs no change -- its parameters ARE variables, which is why")
    message("the same line is correct there and dead here.")
    message("")
    message("Where the name really is a VARIABLE the body sets, and not the parameter,")
    message("say so in the comment block immediately above the line:")
    message("")
    message("    # macro-bare-parameter: <why this name is a variable here>")
    message("")
    list(LENGTH violations violationCount)
    message(FATAL_ERROR
        "macro bare parameters: ${violationCount} finding(s) across ${fileCount} file(s)")
endif()

message(STATUS
    "macro bare parameters: ${fileCount} scripts/*.cmake file(s), ${macroCount} macro "
    "definition(s), no parameter read as a bare name; ${markedCount} site(s) marked as "
    "reading a variable of the same name for a stated reason; ${dataRegions} region(s) "
    "exempt as staged test data")
