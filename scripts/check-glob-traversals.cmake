# SPDX-License-Identifier: Apache-2.0
#
# A hygiene check traverses each directory once, not once per file pattern.
#
# `file(GLOB_RECURSE var a b c)` traverses the tree **once per pattern**, and the call
# site reads as a single glob. That is what made the cost invisible:
# `check-sccache-backend-caveat` handed it ~19 patterns per root across two calls, and
# on a DrvFs checkout — a supported working environment — it took **78.7 s against a
# 60 s budget**, so the documented local gate could not go green for anyone working
# that way ([#502](https://github.com/LASTRADA-Software/fastcached/issues/502)).
#
# ## The measurement, with its conditions, because the number alone misleads
#
# One `GLOB_RECURSE` traversal of `src/`, DrvFs, warm: **2.09 s** (2102, 2101, 2093,
# 2086, 2091 ms — stable to 16 ms). On a native filesystem the same traversal is
# effectively free, which is why this is invisible on CI and on ext4.
#
#     check                     standalone   single gate   two lanes   budget
#     sccache-backend-caveat        78.7s        60.0s       (timeout)    60s
#     byte-order-qualifier          19.6s        28.6s          60.9s     60s
#     worker-refusals-counted        5.5s         9.7s              -     60s
#     net-boundary                   4.0s         8.6s              -     60s
#
# **Read the conditions, not the number.** A future reader who measures
# `byte-order-qualifier` single-gate gets 28.6 s and concludes it is comfortable. It
# was observed at **60.9 s — over budget — while a second lane's gate was running**,
# and this repository routinely has two and three lanes gating at once. That is the
# only condition under which it has been seen to fail, and dropping it turns a
# reproducible failure into a mystery. This project has already paid for a figure
# quoted without its conditions once.
#
# ## What this check enforces, and what it CANNOT see
#
# **The rule: `file(GLOB_RECURSE)` receives exactly one pattern, and that pattern is
# not an unquoted variable expansion.**
#
# The second clause is load-bearing rather than pedantry. The original defect was
# spelled `file(GLOB_RECURSE rootFiles LIST_DIRECTORIES false ${rootGlobs})` — **one
# argument token, nineteen patterns**. A guard that counted arguments would have passed
# the very code this ticket was opened about.
#
# **This is a PROXY for the real invariant, which is traversals per root, and it is
# worth writing down exactly where the proxy stops:**
#
#   - **A loop defeats it.** Nineteen single-pattern `GLOB_RECURSE` calls inside a
#     `foreach` cost precisely what one nineteen-pattern call costs, and pass this
#     check. Somebody tidying a long argument list into a loop would make this guard
#     greener while reintroducing the cost in full.
#   - It says nothing about how many ROOTS are walked, only about patterns per call.
#   - It says nothing about the size of what is walked; globbing a build tree is a
#     separate mistake, and one this project has also made.
#
# A guard whose limits are written down is worth having. One that reads as complete
# and is not is the shape this repository keeps finding, so the limits are stated here
# rather than discovered later by someone trusting it further than it goes.
#
# Scope is `scripts/*.cmake`: these run in the DEFAULT ctest set, on every developer's
# machine, against a wall-clock budget. A `GLOB_RECURSE` in a `CMakeLists.txt` runs at
# configure time, once, and is a different question.
#
# Runs as `cmake -P`. See `check-script-check-signals.cmake` for why such a check
# reports failure through its OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-glob-traversals.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

# A `cmake -P` script has no project, so every policy starts unset and CMP0057
# (`if(... IN_LIST ...)`) is one of them. Stated as a minimum version so the whole set
# moves together with the project's own.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# Keywords `file(GLOB_RECURSE)` accepts that are not patterns. The two that take a
# value consume the argument after them.
set(valuedKeywords "RELATIVE" "LIST_DIRECTORIES")
set(bareKeywords "CONFIGURE_DEPENDS" "FOLLOW_SYMLINKS")

file(GLOB scriptFiles RELATIVE "${FASTCACHED_SOURCE_DIR}"
     "${FASTCACHED_SOURCE_DIR}/scripts/*.cmake")
list(SORT scriptFiles)

if(NOT scriptFiles)
    message("")
    message("  No scripts/*.cmake was found at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved")
    message("source root, or a FASTCACHED_SOURCE_DIR pointing somewhere else.")
    message(FATAL_ERROR "glob traversals: the scan matched no script and cannot conclude")
endif()

set(violations "")
set(callCount 0)

foreach(relative IN LISTS scriptFiles)
    file(READ "${FASTCACHED_SOURCE_DIR}/${relative}" content)

    # Cheap reject: most scripts contain none of these at all.
    string(FIND "${content}" "GLOB_RECURSE" found)
    if(found EQUAL -1)
        continue()
    endif()

    # Line NUMBERS are what a reader acts on, so the split must preserve line count
    # even where it cannot preserve content. CMake treats `[` ... `]` as list grouping
    # and `;` as a separator, so a stray bracket in a comment merges lines and every
    # number after it is wrong -- measured here: this check first reported a call at
    # line 217 that is in fact at line 307. `check-sccache-backend-caveat.cmake`
    # documents the same failure ("one stray `]` in a comment swallowed 451 lines"),
    # and this is its splitter. Lossy on the four characters it blanks, none of which
    # a `file(GLOB_RECURSE ...)` argument list contains. Fifth copy of this idiom;
    # consolidating them is #495.
    string(REPLACE "\\" " " content "${content}")
    string(REPLACE ";" " " content "${content}")
    string(REPLACE "[" " " content "${content}")
    string(REPLACE "]" " " content "${content}")
    string(REGEX REPLACE "\r?\n" ";" lines "${content}")

    set(lineNumber 0)
    foreach(line IN LISTS lines)
        math(EXPR lineNumber "${lineNumber} + 1")

        # Prose, not a call. This file's own header describes what it checks.
        if(line MATCHES "^[ \t]*(#|//)")
            continue()
        endif()
        if(NOT line MATCHES "^[ \t]*file[ \t]*\\([ \t]*GLOB_RECURSE[ \t]+(.*)$")
            continue()
        endif()

        math(EXPR callCount "${callCount} + 1")
        set(rest "${CMAKE_MATCH_1}")
        string(REGEX REPLACE "\\).*$" "" rest "${rest}")

        # Split into arguments, keeping quoted strings whole.
        string(REGEX MATCHALL "\"[^\"]*\"|[^ \t]+" args "${rest}")

        set(patterns "")
        set(skipNext FALSE)
        set(first TRUE)
        foreach(arg IN LISTS args)
            if(first)
                set(first FALSE)   # the output variable
                continue()
            endif()
            if(skipNext)
                set(skipNext FALSE)
                continue()
            endif()
            string(REPLACE "\"" "" bare "${arg}")
            if("${bare}" IN_LIST valuedKeywords)
                set(skipNext TRUE)
                continue()
            endif()
            if("${bare}" IN_LIST bareKeywords)
                continue()
            endif()
            list(APPEND patterns "${arg}")
        endforeach()

        list(LENGTH patterns patternCount)

        if(patternCount GREATER 1)
            list(APPEND violations
                 "${relative}:${lineNumber}: passes ${patternCount} patterns to GLOB_RECURSE, so it traverses the tree ${patternCount} times")
        endif()

        # One token can still be many patterns. This is the spelling the original
        # defect used, and an argument count alone would have passed it.
        foreach(pattern IN LISTS patterns)
            if(pattern MATCHES "^\\$\\{[A-Za-z_][A-Za-z0-9_]*\\}$")
                list(APPEND violations
                     "${relative}:${lineNumber}: passes the unquoted expansion ${pattern} to GLOB_RECURSE, which is one argument and as many traversals as that list is long")
            endif()
        endforeach()
    endforeach()
endforeach()

if(callCount EQUAL 0)
    message("")
    message("  No `file(GLOB_RECURSE ...)` call was found in scripts/*.cmake.")
    message("")
    message("Several of these checks glob, so zero means this scan is no longer looking")
    message("at what it thinks it is -- a renamed function, a moved directory, a changed")
    message("spelling. It is not evidence that every check traverses once.")
    message(FATAL_ERROR "glob traversals: the scan matched nothing and cannot conclude")
endif()

if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("`file(GLOB_RECURSE var a b c)` traverses once PER PATTERN, and the call site")
    message("reads as a single glob. On a DrvFs checkout one traversal of src/ costs")
    message("2.09 s, which is how a check reached 78.7 s against a 60 s budget (#502).")
    message("")
    message("Walk the directory ONCE and filter the result:")
    message("")
    message("    file(GLOB_RECURSE all LIST_DIRECTORIES false \"\${dir}/*\")")
    message("    list(FILTER all INCLUDE REGEX \"\\\\.(cpp|hpp)$\")")
    message("")
    message("Do NOT raise the test's timeout instead. A budget raised to accommodate a")
    message("redundancy hides the redundancy, and #502 rules it out explicitly.")
    message(FATAL_ERROR "glob traversals: ${violationCount} call(s) traverse more than once")
endif()

message(STATUS "glob traversals: ${callCount} GLOB_RECURSE call(s) in scripts/, each traversing once")
