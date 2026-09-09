# SPDX-License-Identifier: Apache-2.0
#
# A Catch2 case that could not RUN its check must not report a PASS.
#
# `SUCCEED("...")` records a passing assertion. Where a case bails out because its
# environment could not be arranged -- no loopback listener, no IPv6 stack, no symlink
# privilege, running as root -- that is a green result for a property nothing
# established ([#685](https://github.com/LASTRADA-Software/fastcached/issues/685)).
#
# It is the converse of [#499](https://github.com/LASTRADA-Software/fastcached/issues/499),
# and the insidious direction. A false RED gets investigated; a false GREEN is believed,
# and it fires exactly where coverage is already thinnest -- a constrained runner, a
# host without IPv6, a Windows agent without the symlink privilege. Those are the runs
# where a green is least justified and most trusted.
#
# ## Why this is a check and not a comment
#
# The idiom spread by IMITATION. `src/apps/fastcache-cc/DirectManifest_test.cpp` carried
# it under a comment that explained, correctly, that the rule being demonstrated is
# unconditional -- and the next test written was copied from it, comment and all. A
# comment that was already right did not stop the copy, so a comment is not the fix.
#
# ## What it can and cannot see
#
# Two signals, and neither is a judgement about whether the case "really ran" -- nothing
# lexical can decide that:
#
#   * **The bail-out shape.** A `SUCCEED` whose next statement is `return;` is leaving
#     the case early -- on the following line, or on its own line, since
#     `if (!ready) { SUCCEED("x"); return; }` is the same defect written in one. Every
#     one of the twenty-one sites #685 converted had it, and no legitimate site in this
#     tree does: a `SUCCEED` that means "the case ran and there was nothing to assert"
#     is the LAST thing in the case and needs no return.
#   * **Skip vocabulary in the message.** A message saying the check could not be
#     performed is the author telling you which state this is. The table below is data,
#     one row per phrase with the reading that makes it a skip.
#
# A `SUCCEED` that bails out silently, with a message in none of that vocabulary and no
# `return`, is invisible here. That is a real limit and is stated rather than papered
# over: this check closes the door the defect actually came through, and
# `.agent/rules/testing.md` carries the rule for the rest.
#
# `SUCCEED()` with no message at all is fine -- `Ranges_test.cpp` uses it after a block
# of `static_assert`s, which is a case that ran and had nothing left to assert.
#
# ## Inconclusive is refused, not passed
#
# A `SUCCEED(` this script cannot read to a closing parenthesis on one line is reported
# as its own outcome. It is not evidence of anything, and the direction this project
# keeps getting wrong is scoring "could not determine" as "fine".
#
# Runs as `cmake -P`: it reads files, compares strings and reports. See
# `check-script-check-signals.cmake` for why such a check reports failure through its
# OUTPUT rather than an exit code.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-succeed-not-skip.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

# A `cmake -P` script has no project, so every policy starts unset. Stated as a minimum
# version so the whole set of checks moves together.
cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

# The skip vocabulary, as data. `<regex>|<reading>` -- the regex is matched against the
# LOWERCASED message, the reading is what the failure tells the author.
#
# Each row is a phrase observed in this tree at a site that was reporting a pass for a
# property nothing established. Adding a case is adding a row.
set(vocabulary
    "skip|says it is skipping"
    "unavailable|says a facility was not available on this host"
    "not available|says a facility was not available on this host"
    "no [a-z0-9_./ -]+ available|says a facility was not available on this host"
    "could not|says the check could not be performed"
    "couldn't|says the check could not be performed"
    "cannot|says the check could not be performed"
    "can not|says the check could not be performed"
    "unable to|says the check could not be performed"
    "not permit|says the host refused the permission the case needs"
    "no permission|says the host refused the permission the case needs"
    "privilege|says the host refused the permission the case needs"
    "unsupported|says the platform does not support the arrangement"
    "not supported|says the platform does not support the arrangement"
    "exercised by|defers the property to another test, so THIS case established nothing"
    "covered by|defers the property to another test, so THIS case established nothing"
    "tested elsewhere|defers the property to another test, so THIS case established nothing"
    "nothing to compare|says there was nothing to check"
    "nothing to assert|says there was nothing to check"
)

# Read and split by hand rather than with `file(STRINGS)`, which returns a LIST: a line
# containing a semicolon becomes two elements and every line number after it drifts.
# Lines are WALKED, never assembled into a CMake list, and that is the whole point.
#
# A list cannot carry a line ending in a backslash. Measured on CMake 3.28.3: the value
# `one\\;two` -- an ESCAPED backslash followed by a separator -- expands to ONE element,
# because a `;` preceded by any backslash is read as escaped whether or not that
# backslash was itself escaped. So no ordering of escapes can place a separator after
# such a line. The reader here used to escape `\` and `;` and split on newlines, and its
# own comment claimed that escaping stopped a line ending in a backslash merging with
# the next -- which is the one case it does not stop.
#
# A merged element begins with whatever the EARLIER line began with, so a `//` swallows
# the real code below it and every line number after it drifts. That is a false GREEN,
# which is the direction nobody investigates.
#
# Live exposure was ZERO when this was repaired: no file in this check's corpus of 229
# test sources ends a line with a backslash. So this closes a latent hazard rather than
# fixing an observed failure -- and it is one file away, since `dist-compile-e2e.sh` has
# 152 such lines and `local-gate.sh` 101. A check reading `scripts/*.sh` through the old
# idiom would have met it on its first file.
#
# `[` and `]` are still replaced with a space. With no list they can no longer merge
# anything, so this is now purely about what the check SEES -- and changing that is a
# different decision from repairing the reader, taken separately or not at all.
# Consolidating the copies of this idiom is #495, not this ticket.
macro(fastcached_take_line contentVar lineVar doneVar)
    string(FIND "${${contentVar}}" "\n" _ftl_newline)
    if(_ftl_newline EQUAL -1)
        set(${lineVar} "${${contentVar}}")
        set(${contentVar} "")
        set(${doneVar} TRUE)
    else()
        string(SUBSTRING "${${contentVar}}" 0 ${_ftl_newline} ${lineVar})
        math(EXPR _ftl_after "${_ftl_newline} + 1")
        string(SUBSTRING "${${contentVar}}" ${_ftl_after} -1 ${contentVar})
        set(${doneVar} FALSE)
    endif()
    string(REGEX REPLACE "\r$" "" ${lineVar} "${${lineVar}}")
endmacro()

# The file, read once, ready to be walked a line at a time.
function(fastcached_read_content path outVar)
    file(READ "${path}" content)
    string(REPLACE "[" " " content "${content}")
    string(REPLACE "]" " " content "${content}")
    string(REPLACE "\r\n" "\n" content "${content}")
    set(${outVar} "${content}" PARENT_SCOPE)
endfunction()

# Which test files this REPOSITORY owns, asked of git rather than inferred from
# directory names. A dependency cache is untracked by construction, whatever a package
# manager decides to call it or wherever it puts it -- Catch2's own self-tests are full
# of `SUCCEED` and nobody here can edit them.
#
# The same idiom, and the same fallback, as `check-catch-skip-return-code.cmake`.
if(NOT GIT_EXECUTABLE)
    find_program(GIT_EXECUTABLE NAMES git)
endif()

set(testFiles "")
set(scanSource "")

if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" rev-parse --is-inside-work-tree
        OUTPUT_VARIABLE insideWorkTree
        ERROR_VARIABLE gitError
        RESULT_VARIABLE gitStatus
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(gitStatus EQUAL 0 AND insideWorkTree STREQUAL "true")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${FASTCACHED_SOURCE_DIR}" ls-files
                    -- "*_test.cpp" "src/tests/*.cpp" "src/tests/*.hpp"
            OUTPUT_VARIABLE tracked
            RESULT_VARIABLE lsStatus
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(lsStatus EQUAL 0 AND NOT tracked STREQUAL "")
            string(REPLACE "\n" ";" testFiles "${tracked}")
            set(scanSource "git ls-files")
        endif()
    endif()
endif()

# No git, or an export with no index. A source export contains no build tree and no
# dependency cache BY CONSTRUCTION -- that is what makes the walk sound here and unsound
# in a working checkout -- so the fallback is a plain recursive glob with the build-tree
# names still excluded, and it says which mode produced the answer.
#
# The fallback is NOT decorative, and it is worth saying where it gets exercised, because
# a check with two enumeration paths of which only one is ever taken has an untested half
# that will be reached exactly once, in anger. Two routes reach it: a release tarball, and
# **inspecting a merge before it lands** -- `git merge-tree --write-tree` plus `git
# archive` materialises a tree with no index, which is how #685 first ran this half. It
# agreed with the git path, and the selftest asserts the MODE on both sides so that
# agreement is not luck.
if(NOT testFiles)
    set(excludeNames "out" "build" "_deps" ".git" ".cache" ".claude")
    file(GLOB_RECURSE walked RELATIVE "${FASTCACHED_SOURCE_DIR}"
         "${FASTCACHED_SOURCE_DIR}/*_test.cpp"
         "${FASTCACHED_SOURCE_DIR}/src/tests/*.cpp"
         "${FASTCACHED_SOURCE_DIR}/src/tests/*.hpp")
    foreach(candidate IN LISTS walked)
        set(excluded FALSE)
        foreach(name IN LISTS excludeNames)
            if(candidate MATCHES "(^|/)${name}/")
                set(excluded TRUE)
                break()
            endif()
        endforeach()
        if(NOT excluded)
            list(APPEND testFiles "${candidate}")
        endif()
    endforeach()
    set(scanSource "directory walk (no git index)")
endif()

list(REMOVE_DUPLICATES testFiles)
list(SORT testFiles)

if(NOT testFiles)
    message("")
    message("  No test source was found in this repository at all.")
    message("")
    message("That is not a clean tree, it is a scan that stopped working -- a moved")
    message("source root, or a FASTCACHED_SOURCE_DIR pointing somewhere else.")
    message(FATAL_ERROR "succeed-not-skip: the scan matched no test sources and cannot conclude")
endif()

set(sites "")
set(violations "")

foreach(relative IN LISTS testFiles)
    # `git ls-files` lists the INDEX, so a test file deleted from the worktree and not
    # yet staged is still named here. Reading it emits `CMake Error ... failed to open
    # for reading`, which the registration's FAIL_REGULAR_EXPRESSION scores as THIS check
    # failing -- for a reason with nothing to do with `SUCCEED`.
    if(NOT EXISTS "${FASTCACHED_SOURCE_DIR}/${relative}")
        continue()
    endif()

    fastcached_read_content("${FASTCACHED_SOURCE_DIR}/${relative}" rest)

    set(lineNumber 0)
    set(inBlockComment FALSE)
    set(atEnd FALSE)
    while(NOT atEnd)
        fastcached_take_line(rest rawLine atEnd)
        math(EXPR lineNumber "${lineNumber} + 1")

        # Prose, not a call. This rule is written out in `.agent/rules/testing.md` and
        # quoted in test comments, so a scan that could not tell those apart would refuse
        # the documentation of its own rule -- with no escape hatch but rewording the
        # comment. A leading-`//` test is not enough: a `/* ... */` block written without
        # leading stars carries whole paragraphs, and its body is what a historic note
        # quoting the idiom looks like.
        #
        # The blind spot, stated rather than papered over: CMake's regex engine is greedy
        # and has no lazy quantifier, so stripping inline `/* ... */` pairs takes
        # everything between the FIRST `/*` and the LAST `*/` on a line. A `SUCCEED`
        # sitting between two block comments on one line is therefore invisible here.
        set(line "${rawLine}")
        if(inBlockComment)
            if(NOT line MATCHES "\\*/")
                continue()
            endif()
            string(REGEX REPLACE "^.*\\*/" "" line "${line}")
            set(inBlockComment FALSE)
        endif()
        string(REGEX REPLACE "/\\*.*\\*/" " " line "${line}")
        if(line MATCHES "/\\*")
            string(REGEX REPLACE "/\\*.*$" "" line "${line}")
            set(inBlockComment TRUE)
        endif()
        string(REGEX REPLACE "//.*$" "" line "${line}")

        if(NOT line MATCHES "SUCCEED[ \t]*\\(")
            continue()
        endif()

        set(where "${relative}:${lineNumber}")
        list(APPEND sites "${where}")

        # `SUCCEED()` -- a case that ran with nothing left to assert. Nothing to read and
        # nothing to object to.
        if(line MATCHES "SUCCEED[ \t]*\\([ \t]*\\)")
            set(message "")
        elseif(line MATCHES "SUCCEED[ \t]*\\([ \t]*\"([^\"]*)\"[ \t]*\\)")
            set(message "${CMAKE_MATCH_1}")
        else()
            # Neither shape. Reported as its own outcome rather than skipped over: a site
            # this scan cannot read is not a site it has cleared.
            list(APPEND violations
                 "${where}: this check cannot read the SUCCEED on one line, so it cannot say whether the case ran -- put the call and its message on one line")
            continue()
        endif()

        # One finding per site, whichever signal saw it. Two lines about one `SUCCEED`
        # read as two defects and inflate the count -- and the count is what a person
        # compares against the tree when deciding whether the check is telling the truth.
        set(reason "")

        # Signal one: the message says so itself. Preferred where both fire, because it
        # quotes the author's own words back and needs no explaining.
        if(NOT message STREQUAL "")
            string(TOLOWER "${message}" lowered)
            foreach(row IN LISTS vocabulary)
                string(REGEX REPLACE "^([^|]*)\\|.*$" "\\1" pattern "${row}")
                string(REGEX REPLACE "^[^|]*\\|(.*)$" "\\1" reading "${row}")
                if(lowered MATCHES "${pattern}")
                    # A semicolon inside the quoted message would split this finding into
                    # two list elements and tear the sentence in half -- which is what it
                    # did before `could not bind test port; skipping` was ever printed.
                    string(REPLACE ";" "\\;" quoted "${message}")
                    set(reason "SUCCEED(\"${quoted}\") ${reading}")
                    break()
                endif()
            endforeach()
        endif()

        # Signal two: the bail-out shape. The next STATEMENT -- not the next line, since a
        # comment may sit between them -- being a bare `return` means the case is leaving
        # early, which is what a case does when its environment could not be arranged.
        #
        # The `return` may also sit on the SUCCEED's OWN line: `if (!ready) { SUCCEED("x");
        # return; }` is the defect in one line, and a scan that starts at the next line
        # walks straight past it. Checked first, since it is the cheaper question.
        if(reason STREQUAL "" AND line MATCHES "SUCCEED[ \t]*\\(.*\\)[ \t]*;.*return[ \t]*;")
            set(reason "SUCCEED is followed by a bare `return`, so the case is bailing out and never reached the property it reports a pass for")
        endif()

        # `lineNumber` is 1-based, so it is already the 0-based index of the NEXT line.
        if(reason STREQUAL "")
            # `rest` is already everything after this line, so the lookahead walks a
            # copy of it rather than indexing. Same lines, same order, no list.
            set(ahead "${rest}")
            set(aheadDone FALSE)
            while(NOT aheadDone)
                fastcached_take_line(ahead following aheadDone)
                string(STRIP "${following}" following)
                if(following STREQUAL "" OR following MATCHES "^(//|\\*|/\\*)")
                    continue()
                endif()
                if(following MATCHES "^return[ \t]*;")
                    set(reason "SUCCEED is followed by a bare `return`, so the case is bailing out and never reached the property it reports a pass for")
                endif()
                break()
            endwhile()
        endif()

        if(NOT reason STREQUAL "")
            list(APPEND violations
                 "${where}: ${reason} -- a case that could not RUN is SKIPPED, not passed. Use SKIP")
        endif()
    endwhile()
endforeach()

# A scan that found NO `SUCCEED` at all is not a clean tree. This project has a dozen
# legitimate ones, so zero means a renamed macro, a moved directory or a changed
# spelling -- reported as its own failure rather than folded into success.
list(LENGTH sites siteCount)
if(siteCount EQUAL 0)
    message("")
    message("  No `SUCCEED` was found in any test source.")
    message("")
    message("This project has legitimate ones -- the Tracy build-mode arms, the")
    message("static_assert-driven Ranges case, the IOCP completion-lifetime cases -- so")
    message("zero means this scan is no longer looking at what it thinks it is. It is")
    message("not evidence that no case reports a pass it did not earn.")
    message(FATAL_ERROR "succeed-not-skip: the scan matched nothing and cannot conclude")
endif()

if(violations)
    list(LENGTH violations violationCount)
    message("")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message("")
    message("`SUCCEED` records a PASSING assertion. Where a case could not run its")
    message("check, that is a green result for a property nothing established -- and it")
    message("fires exactly on the runs where coverage is thinnest and a green is most")
    message("believed (#685).")
    message("")
    message("`SKIP(\"reason\")` is the whole fix. It exits 4, every catch_discover_tests")
    message("registration in this tree carries SKIP_RETURN_CODE 4, and ctest scores it")
    message("as SKIPPED (#499).")
    message("")
    message("`SUCCEED` stays right for a case that RAN and had nothing to assert.")
    message(FATAL_ERROR "succeed-not-skip: ${violationCount} site(s) report a pass for a check that did not run")
endif()

list(LENGTH testFiles fileCount)
message(STATUS
    "succeed-not-skip: ${siteCount} SUCCEED site(s) in ${fileCount} test source(s) via ${scanSource}, none standing in for a skip")
