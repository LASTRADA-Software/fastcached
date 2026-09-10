# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy the
# project has not stated. CMP0057 (`if(... IN_LIST ...)`) has already silently done
# nothing in a check in this tree.
cmake_minimum_required(VERSION 3.28)

# The two line-splitting idioms, defined once (#495). They are TWO -- tokenised and
# verbatim -- with opposite intent, and the module says which one a site wants and
# why merging them would break whichever family it did not choose, silently.
include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")
#
# A script that offers `--self-test` must be REGISTERED to run it (#596).
#
# An unrun self-test is the most expensive kind of green there is. It reads as coverage
# from every direction: the cases exist, they are written carefully, they are cited in
# review, and nothing anywhere says that no target invokes them. The suite passes because
# the suite never asks.
#
# This is not hypothetical and it is not old. `scripts/ci-apt-update.sh` grew a twelve-case
# self-test covering the exact decision that had just broken CI, and no `add_test` named
# it. The allowlist then shipped with the runner's real layout untested and took every
# Linux job down at `Install build tools` (#1160). Twelve cases, none of them ever
# executed, on the one file whose correctness the outage turned on.
#
# ## What counts as OFFERING a self-test
#
# The token is not the offer. `--self-test` appears in this tree inside comments, inside
# usage banners naming ANOTHER script, and inside `printf` strings that stage a fake
# workflow file for a fixture -- `check-tidy-blind-spots-selftest.sh` writes
# `run: bash scripts/tidy-sweep.sh --self-test` into a YAML fixture, and requiring THAT
# file to register a self-test it does not have would be a false refusal on a correct
# tree.
#
# A COMMENT is not a call site, and neither is a STRING LITERAL staging somebody else's
# command line. So the offer is recognised by ARGUMENT DISPATCH, not by the token:
#
#   * a `case` arm  --  `--self-test)`
#   * a comparison against a POSITIONAL PARAMETER on the same line --
#     `[ "${1:-}" = "--self-test" ]`, `[[ "${1:-}" == "--self-test" ]]`
#   * PowerShell  --  `[switch]$SelfTest` in a param block
#
# Those three cover every offering script here.
#
# ## A spelling this table cannot read REFUSES
#
# Reporting the count was not enough, and the reason is the direction of failure. A new
# dispatch spelling makes the offering count fall by one, in a two-digit number nobody has
# memorised, printed on a line that says the run PASSED. So the file is silently
# not-counted, and "not counted" and "does not offer" are one number. (The figure is not
# restated here: the run prints it, and a second copy of a live number is a second source
# of truth that drifts while both claim to be current.)
#
# A file whose token appears on a line that is not a comment, and in which no shape is
# recognised, is therefore REFUSED. That predicate is asked of the whole content rather
# than by walking, so the immunity the pre-filter buys below is kept: a token that appears
# only in comments is documentation and says nothing.
#
# There are TWO states in which that happens, and they are separate reachability cases
# rather than one rule with two examples:
#
#   * the whole-file pre-filter rejects the file, so no line is ever examined
#   * the pre-filter accepts and every line then fails to match, so the two filters
#     DISAGREE -- which is the state a genuinely new spelling arrives in
#
# Both are refused, at their own site. The self-test drives one case at each, and it took
# the mutation matrix to establish that: deleting the second refusal outright changed no
# verdict, because the case written for it was reaching the FIRST site. Its staged body
# spelled the flag `\"--self-test\"`, so the character after the token on disk is a
# backslash rather than a quote and the pre-filter's `["\']--self-test["\']` never
# matched. A case can be green, correctly named, and testing the neighbouring branch.
#
# The one legitimate case in this tree is a fixture STAGING somebody else's command line --
# `check-tidy-blind-spots-selftest.sh` writes `bash scripts/tidy-sweep.sh --self-test` into
# a YAML fixture. That is not an offer, and it cannot be told from one by any pattern, so
# it says so in its own file:
#
#   # selftest-offer: <why this file's token is not an offer>
#
# An EMPTY reason is refused. A placeholder would spell "forgot" in the vocabulary of
# "decided", which is the distinction `RefuseWithoutCounter` exists to keep.
#
# ## `.cmake` self-tests offer by their NAME
#
# A `cmake -P` self-test has no `--self-test` flag to offer: the FILE is the mode, and
# `scripts/check-<x>-selftest.cmake` is invoked as its own script. So for `.cmake` the
# offer is the NAME, and the registration must invoke it -- `-P` and the file name on one
# line -- which is the exact analogue of requiring the flag to be PASSED for a shell
# script. A registration that merely mentions the name runs nothing.
#
# This scanned only `*.sh` and `*.ps1` until #1220, so 23 `*-selftest.cmake` files were
# outside it entirely -- including this check's own self-test. All 23 are registered
# today, so widening changes no verdict now; it is the direction of failure that decides
# it, exactly as for the recursive glob below. Over-broad fails CLOSED.
#
# ## Why the MODE must be registered, and not merely the script
#
# #596 asks that an offering script "appear in an `add_test` COMMAND". That is weaker than
# the defect requires, and the tree contains the counterexample: `check-mkdocs-validation.sh`
# offers `--self-test` and was registered ONCE, in its plain mode. It appears in the table,
# it satisfies the weaker rule, and its self-test had still never run. So what is required
# here is a registration that PASSES the flag.
#
# ## Why the walk never builds a CMake list
#
# `file(STRINGS)` and every `string(REPLACE "\n" ";")` idiom hand the content to CMake's
# list parser, which reads an unbalanced `[` or `]` as structure and merges elements. The
# standing remedy in this tree is to blank the brackets before splitting -- and it is
# WRONG here, because the brackets ARE the data: `[ "${1:-}" != "--self-test" ]` is the
# shell test this check recognises, and blanking it would delete the syntax being matched.
# `check-tsan-scope.cmake` hit the same wall (a Catch2 tag IS `[async]`) and answered it
# the same way. This walks with FIND/SUBSTRING and builds no list at all, so the question
# does not arise.
#
# It is the EIGHTH copy of that walk under `scripts/` -- `check-tsan-scope`,
# `check-test-names`, `check-corrupt-store-diagnostics`, `check-bind-failure-seam`,
# `check-worker-refusals-counted`, `check-istreambuf-iterator` (wrapped as
# `fastcached_scan_lines`) and `check-tools-page-installed-set` (wrapped as
# `fc_capture_lines`) already carry one, and neither wrapper is a drop-in here.
# **#495 is the ticket for consolidating them and this is not it.** The citation is what
# keeps this copy inside that ticket's audit rather than hiding from it -- an
# uncited copy is one #495 will not find.
#
# ## Cost
#
# Conditions, stated rather than pointed at, because a measurement is a quantity UNDER
# conditions and the citation is where they get lost: Windows 11, Git Bash, native NTFS,
# CMake 4.3.1-msvc1, 67 scanned scripts, minimum of seven runs. Taken back to back --
# an earlier session measured a 90 ms process floor where these runs measure 47 ms, on
# the same machine, so figures from different sittings are not comparable and only the
# rows within one block are.
#
#     empty `cmake -P` (process floor)         47 ms
#     line walk, before #1168                 330 ms   (283 ms of work)
#     token jump                              155 ms   (108 ms of work)
#
# The second property is the one worth having, and it is measured on a COPY of the tree
# so that another lane's file is not edited to take a reading. Those figures are slower
# throughout than the block above -- a different path and a cold cache -- and are to be
# read only against each other:
#
#                                          line walk    token jump
#     the copied tree as it stands            538 ms       225 ms
#     + ONE comment line carrying a QUOTED
#       token in the 4,103-line
#       check-e2e-helpers.sh                  917 ms       207 ms
#
# So a line walk's cost is a property of a file's LENGTH, and one sentence written in an
# unrelated file cost it 379 ms. That was a trap laid for whoever wrote the sentence
# rather than a cost anybody chose, and the whole-file pre-filter below existed to buy
# immunity from it. The token-jump walk has that immunity intrinsically: cost is a
# property of how often a file MENTIONS the flag, which is what the question is about.
# The pre-filter stays anyway -- it is what makes the two-filter DISAGREEMENT detectable,
# which is #1220's refusal.
#
# Both walks were compared as SETS and not as counts, because two totals agreeing is
# weak evidence -- a file gained and a file lost cancel exactly. Instrumented to print
# every file read as offering and the shape recognised, the two listings are identical:
# 36 files, same shapes. The comparison carries its own controls, since a `diff` of two
# empty listings agrees perfectly: the listing must be non-empty, and removing one row
# must make it compare unequal.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-selftest-registered.cmake
#
# Exit: the verdict is the OUTPUT, not the status -- `message(FATAL_ERROR)` is read
# through `FAIL_REGULAR_EXPRESSION`, because a `-P` script that merely WARNS exits 0.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()

set(registrationFile "${FASTCACHED_SOURCE_DIR}/src/tests/CMakeLists.txt")
if(NOT EXISTS "${registrationFile}")
    message(FATAL_ERROR "the registration table is missing: ${registrationFile}")
endif()
file(READ "${registrationFile}" registrationRaw)

# Comment lines are removed before anything is matched against this file.
#
# The offering side goes to real trouble to reject comments and string literals, and the
# registration side did not -- so a registration COMMENTED OUT still read as present, and
# the check reported clean. That is the REMOVAL direction, the one #596 explicitly asks
# for, and it fails OPEN: adding an unregistered self-test is refused loudly, while
# commenting a registration out is silent. Measured on this tree before the fix, by
# commenting one `--self-test` registration: `30 offering` and no refusal.
#
# Split with the bracket-blanking idiom rather than the list-free walk used for the
# scripts. It is safe HERE and not there, and the difference is the data: what gets
# matched against this content is a script NAME and a FLAG, neither of which can contain
# a bracket, whereas the shell dispatch shapes the walk recognises ARE brackets.
fastcached_split_lines_verbatim("${registrationRaw}" registrationLines)
set(registrationContent "")
set(registeredCmakeNames "")
foreach(registrationLine IN LISTS registrationLines)
    if(registrationLine MATCHES "^[ \t]*#")
        continue()
    endif()
    string(APPEND registrationContent "${registrationLine}\n")

    # A `-P` line NAMES the script it runs. Collected here, once, rather than asked of
    # the whole content per file: the question is "`-P` and the name on ONE line", and a
    # match against the joined content cannot express "same line" -- it would accept a
    # `-P` in one registration and the name in a different one, which is a registration
    # that runs a different script.
    if(registrationLine MATCHES "-P.*/([A-Za-z0-9_.+-]+\\.cmake)")
        list(APPEND registeredCmakeNames "${CMAKE_MATCH_1}")
    endif()
endforeach()
if(registrationContent STREQUAL "")
    message(FATAL_ERROR
        "every line of ${registrationFile} was read as a comment, so no registration "
        "could be found and every offering script would be reported unregistered. The "
        "reader is broken, not the tree.")
endif()

# RECURSIVE, because a glob one directory deep is a file list wearing a glob's clothes.
#
# `scripts/` has three subdirectories -- `lib/`, `probes/`, `pedantic-flag-probe/` -- and
# `scripts/lib/e2e-common.sh` is the shared library every POSIX fixture sources, which is
# the one file here most likely to grow a self-test. A non-recursive glob would never
# require it to register and would report clean, which is this check's own thesis failing
# one level down: the argument for a glob over a hand-kept list is that a list is exact
# about what it knows and silent about what it does not, and a glob that stops at the top
# directory is silent in exactly the same way (#492).
#
# None of those files carries the token today, so this changes no verdict now. It is the
# direction of failure that decides it: over-broad fails CLOSED, and the scan reports its
# denominator so a shrink is visible.
file(GLOB_RECURSE shScripts "${FASTCACHED_SOURCE_DIR}/scripts/*.sh")
file(GLOB_RECURSE ps1Scripts "${FASTCACHED_SOURCE_DIR}/scripts/*.ps1")
file(GLOB_RECURSE cmakeScripts "${FASTCACHED_SOURCE_DIR}/scripts/*.cmake")
set(allScripts ${shScripts} ${ps1Scripts})

list(LENGTH allScripts scriptCount)
if(scriptCount EQUAL 0)
    message(FATAL_ERROR
        "no scripts were found under ${FASTCACHED_SOURCE_DIR}/scripts; this check would "
        "pass vacuously. Either the tree moved or the glob stopped matching -- and a "
        "check reporting clean over an empty set is the defect it exists to prevent.")
endif()

# A class going to zero on its own is the same defect one population down, and a TOTAL
# cannot see it: a `.sh` glob that stopped matching is invisible behind a healthy
# `.cmake` one. What it must NOT be is a claim about how this tree is composed -- the
# first version of this guard required all three populations to be non-empty, which is
# true of the repository and false of every synthetic tree in this check's own
# self-test, and it refused all nine of them.
#
# So it is a CROSS-CHECK against an independent source instead: the registration table
# names what it runs, so a table naming files of a class the glob did not find means the
# GLOB broke. That holds for any tree -- a fixture staging only shell scripts registers
# only shell scripts and is untouched -- and it is the same shape as `check-tsan-scope`
# reading its scope out of `tsan-gate.sh` rather than restating it.
foreach(pair "shScripts;--self-test;shell" "ps1Scripts;-SelfTest;PowerShell")
    list(GET pair 0 globVar)
    list(GET pair 1 needle)
    list(GET pair 2 label)
    list(LENGTH ${globVar} classCount)
    if(classCount EQUAL 0 AND registrationContent MATCHES "${needle}")
        message(FATAL_ERROR
            "the registration table names ${label} self-tests and the `${globVar}` glob "
            "matched nothing under ${FASTCACHED_SOURCE_DIR}/scripts. Two independent "
            "sources disagree about whether this tree has any, so the glob has stopped "
            "matching and this check would report clean over a population it never read.")
    endif()
endforeach()

set(offeringCount 0)
set(carryingCount 0)
set(unregistered "")
set(unreadable "")
set(cmakeSelftestCount 0)

# --- `.cmake` self-tests: the FILE is the mode, so the NAME is the offer ------
foreach(script IN LISTS cmakeScripts)
    get_filename_component(scriptName "${script}" NAME)
    if(NOT scriptName MATCHES "-selftest\\.cmake$")
        continue()
    endif()
    math(EXPR cmakeSelftestCount "${cmakeSelftestCount} + 1")
    if(NOT scriptName IN_LIST registeredCmakeNames)
        list(APPEND unregistered "${scriptName}")
    endif()
endforeach()

# The same cross-check, and NOT a bare `cmakeSelftestCount EQUAL 0`: a tree with no
# `.cmake` self-test is an ordinary tree, and refusing it would refuse every fixture.
# What cannot be true is the TABLE naming `*-selftest.cmake` registrations while the
# glob finds none of them.
set(registeredCmakeSelftests "")
foreach(registeredName IN LISTS registeredCmakeNames)
    if(registeredName MATCHES "-selftest\\.cmake$")
        list(APPEND registeredCmakeSelftests "${registeredName}")
    endif()
endforeach()
if(cmakeSelftestCount EQUAL 0 AND registeredCmakeSelftests)
    list(LENGTH registeredCmakeSelftests registeredCmakeSelftestCount)
    message(FATAL_ERROR
        "the registration table runs ${registeredCmakeSelftestCount} "
        "`*-selftest.cmake` file(s) with `-P`, and the `*.cmake` glob found none under "
        "${FASTCACHED_SOURCE_DIR}/scripts. Two independent sources disagree about "
        "whether this tree has any, so the glob has stopped matching and every "
        "unregistered `.cmake` self-test would pass unseen.")
endif()

foreach(script IN LISTS allScripts)
    file(READ "${script}" scriptContent)
    string(REPLACE "\r\n" "\n" scriptContent "${scriptContent}")
    get_filename_component(scriptName "${script}" NAME)
    get_filename_component(scriptExt "${script}" EXT)

    # Cheap reject over the RAW bytes. 400-odd scripts do not carry the token at all, and
    # walking every line of each to discover that is the cost this avoids.
    if(scriptExt STREQUAL ".ps1")
        set(token "SelfTest")
    else()
        set(token "--self-test")
    endif()
    string(FIND "${scriptContent}" "${token}" tokenAt)
    if(tokenAt EQUAL -1)
        continue()
    endif()
    math(EXPR carryingCount "${carryingCount} + 1")

    # The `selftest-offer:` marker is read HERE, in the pass that already holds this
    # file, rather than in a second loop that re-read every script to ask one question.
    # Only a file CARRYING the token can have a marker that means anything -- the marker
    # explains a token, so on a file with none it describes nothing.
    set(hasOfferMarker FALSE)
    if(scriptContent MATCHES "#[ \t]*selftest-offer:[ \t]*([^\n]*)")
        set(hasOfferMarker TRUE)
        string(STRIP "${CMAKE_MATCH_1}" offerMarkerReason)
        if(offerMarkerReason STREQUAL "")
            list(APPEND unreadable
                 "${scriptName} (its `selftest-offer:` marker states no reason)")
        endif()
    endif()

    # Second reject, over the whole content, before any walking: can a dispatch shape
    # exist in this file AT ALL?
    #
    # Exact rather than approximate, and that is what makes it safe: every per-line
    # pattern below is a strict specialisation of one of these, so a file matching
    # neither has no line that could match either, and skipping it cannot change a
    # verdict. It is the same argument `worker-refusals-counted` makes for filtering
    # whole-file before splitting -- each needle is a strict prefix of the regex that
    # would have matched it.
    #
    # It buys IMMUNITY, not speed, and that distinction is the whole reason it is here:
    # what it removes is a check whose cost jumps because somebody wrote a SENTENCE in an
    # unrelated file, which is a trap laid for whoever writes the sentence rather than a
    # cost anybody chose. The figures are in the `## Cost` section of the header, pinned
    # to the conditions they were taken under.
    set(prefilterPassed TRUE)
    if(scriptExt STREQUAL ".ps1")
        if(NOT scriptContent MATCHES "\\[switch\\][ \t]*\\$SelfTest")
            set(prefilterPassed FALSE)
        endif()
    elseif(NOT scriptContent MATCHES "--self-test\\)"
            AND NOT scriptContent MATCHES "[\"']--self-test[\"']")
        set(prefilterPassed FALSE)
    endif()

    # Does the token appear anywhere OUTSIDE a comment? Asked of the whole content, in
    # one regex, so the pre-filter's immunity survives: a file carrying the token only in
    # prose is documentation and is dropped here without being walked.
    #
    # `[^#\n]*` is the whole predicate -- from the start of a line, no `#` before the
    # token. It reads a code line that happens to contain an earlier `#` as prose, which
    # is the pre-existing behaviour and fails toward NOT refusing, so it can add no false
    # refusal to a tree that passes today.
    if(scriptContent MATCHES "(^|\n)[ \t]*[^#\n]*${token}")
        set(tokenOnCodeLine TRUE)
    else()
        set(tokenOnCodeLine FALSE)
    endif()

    # An offer this table cannot read is not an absence of an offer.
    if(NOT prefilterPassed)
        if(tokenOnCodeLine AND NOT hasOfferMarker)
            list(APPEND unreadable "${scriptName}")
        endif()
        continue()
    endif()

    set(offerShape "")
    # ## Jump to the token; do not walk the lines (#1168)
    #
    # The line walk this replaces did `string(SUBSTRING "${rest}" ${afterNewline} -1 rest)`
    # once PER LINE, and each of those copies the whole remainder of the file -- so
    # reading a 3,658-line script cost thousands of copies averaging half its size, and
    # the price was paid on every line whether or not it had anything to do with the
    # token. Cost was a property of a file's LENGTH.
    #
    # This jumps from token to token instead. The number of copies is the number of
    # OCCURRENCES, which is a handful per file, so cost becomes a property of how often
    # a file mentions the flag -- which is what the question is actually about.
    #
    # ## There is NO window bound, deliberately
    #
    # #1168 anticipated one, and the note this replaces said a token-jump walk was kept
    # out "because bounding a line to a window changes what `the line` means for a very
    # long one". That is a real hazard and this design does not have it: the line is
    # recovered by a REVERSE find for the preceding newline over the exact text between
    # the last line consumed and the token, and a forward find for the next one. Both
    # are exact, so the line handed to the patterns below is the WHOLE line, byte for
    # byte the one the line walk produced, at any length.
    #
    # A bound would have been the weaker answer twice over: it would make the verdict
    # depend on a constant nobody could derive, and #1168 requires the verdict to be
    # IDENTICAL. The corpus would not have told anyone it was wrong, either -- 38,188
    # lines across the scanned scripts, longest 950 characters, six over 500 and none
    # over 1,000 -- so any window above about 1 KB would have looked perfect and been a
    # silent cliff for whoever first wrote a longer line. `selftest-registered-selftest`
    # stages a 4,000-character line for exactly that reason.
    #
    # `string(FIND ... REVERSE)` answers -1 when the token is on the first line, and
    # `-1 + 1` is 0, which is the right line start -- so that case needs no branch and
    # gets none.
    string(LENGTH "${scriptContent}" contentLength)
    set(searchFrom 0)
    while(searchFrom LESS contentLength)
        string(SUBSTRING "${scriptContent}" ${searchFrom} -1 tail)
        string(FIND "${tail}" "${token}" tokenAt)
        if(tokenAt EQUAL -1)
            break()
        endif()

        string(SUBSTRING "${tail}" 0 ${tokenAt} beforeToken)
        string(FIND "${beforeToken}" "\n" lastNewline REVERSE)
        math(EXPR lineStart "${lastNewline} + 1")

        string(SUBSTRING "${tail}" ${tokenAt} -1 fromToken)
        string(FIND "${fromToken}" "\n" nextNewline)
        if(nextNewline EQUAL -1)
            math(EXPR lineEnd "${contentLength} - ${searchFrom}")
        else()
            math(EXPR lineEnd "${tokenAt} + ${nextNewline}")
        endif()
        math(EXPR lineLength "${lineEnd} - ${lineStart}")
        string(SUBSTRING "${tail}" ${lineStart} ${lineLength} line)

        # Advance past the LINE rather than past the token, so a line mentioning the
        # flag twice is examined once.
        #
        # This is a COST property and not a correctness one, and the distinction is
        # written down because the first version of this comment claimed the opposite --
        # that advancing by the token would "count the same offer twice". It would not:
        # `offeringCount` rises once per FILE, and the loop breaks at the first shape, so
        # re-examining a line yields the same answer and changes no verdict. The
        # mutation matrix is what said so, by advancing with the token and staying
        # GREEN. Left as-is it would have been a plausible sentence nothing could
        # falsify, sitting in a comment, vouching for the line beneath it.
        math(EXPR searchFrom "${searchFrom} + ${lineEnd} + 1")

        if(NOT line MATCHES "^[ \t]*#")
            if(scriptExt STREQUAL ".ps1")
                if(line MATCHES "\\[switch\\][ \t]*\\$SelfTest")
                    set(offerShape "param-switch")
                endif()
            else()
                if(line MATCHES "(^|[ \t(|])--self-test\\)")
                    set(offerShape "case-arm")
                elseif(line MATCHES "[\"']--self-test[\"']"
                        AND line MATCHES "(\\$1|\\$\\{1|\\$@|\\$\\*)")
                    set(offerShape "positional-comparison")
                endif()
            endif()
            # The shape is all that is wanted, so stop at the first one.
            if(NOT offerShape STREQUAL "")
                break()
            endif()
        endif()
    endwhile()

    if(offerShape STREQUAL "")
        # The two filters now DISAGREE: a dispatch shape exists somewhere in this file
        # and no line carries one. That is the state a new spelling arrives in.
        if(tokenOnCodeLine AND NOT hasOfferMarker)
            list(APPEND unreadable "${scriptName}")
        endif()
        continue()
    endif()
    math(EXPR offeringCount "${offeringCount} + 1")

    # The registration has to pass the FLAG. A plain registration of the same script
    # satisfies "appears in an add_test COMMAND" and still never runs a case.
    string(REPLACE "." "\\." scriptNameRegex "${scriptName}")
    if(scriptExt STREQUAL ".ps1")
        set(flagRegex "-SelfTest")
    else()
        set(flagRegex "--self-test")
    endif()
    if(NOT registrationContent MATCHES "${scriptNameRegex}\"?[ \t]+${flagRegex}")
        list(APPEND unregistered "${scriptName}")
    endif()
endforeach()

if(offeringCount EQUAL 0)
    message(FATAL_ERROR
        "not one of the ${scriptCount} script(s) scanned was read as offering a "
        "self-test, though ${carryingCount} carry the token. The dispatch table above "
        "has stopped matching this tree's spellings, so this check would pass over "
        "every unregistered self-test in it.")
endif()

if(unreadable)
    list(REMOVE_DUPLICATES unreadable)
    list(LENGTH unreadable unreadableCount)
    string(REPLACE ";" "\n         " unreadableList "${unreadable}")
    message(FATAL_ERROR
        "${unreadableCount} script(s) carry the self-test token on a line that is not a "
        "comment, and this check's dispatch table reads no offer in them:\n"
        "         ${unreadableList}\n"
        "       Either the script offers a self-test in a spelling this table cannot "
        "read -- in which case add the shape here, or the file goes on being reported as "
        "NOT offering and its cases never run -- or the token is not an offer at all, in "
        "which case say so in the file:\n"
        "         # selftest-offer: <why this file's token is not an offer>\n"
        "       A count alone cannot carry this: a new spelling makes the offering total "
        "fall by one, in a number nobody has memorised, on a run that reports PASSED.")
endif()

message(STATUS
    "check-selftest-registered: ${scriptCount} shell/PowerShell script(s) scanned, "
    "${carryingCount} carrying the token, ${offeringCount} offering a self-test; "
    "${cmakeSelftestCount} `*-selftest.cmake` file(s), each of which must be named by a "
    "`-P` registration")

if(unregistered)
    list(LENGTH unregistered unregisteredCount)
    string(REPLACE ";" "\n         " unregisteredList "${unregistered}")
    message(FATAL_ERROR
        "${unregisteredCount} script(s) offer a self-test that no add_test runs:\n"
        "         ${unregisteredList}\n"
        "       Register each in src/tests/CMakeLists.txt with the flag PASSED, not "
        "merely with the script named -- a registration in the script's plain mode "
        "satisfies neither the ticket nor the reader. An unrun self-test reads as "
        "coverage from every direction and is silent about being unrun, which is how "
        "twelve cases guarding the apt sweep never executed once (#1160).")
endif()
