# SPDX-License-Identifier: Apache-2.0
#
# Policies are pinned because a `cmake -P` script gets OLD defaults for every policy the
# project has not stated. CMP0057 (`if(... IN_LIST ...)`) has already silently done
# nothing in a check in this tree.
cmake_minimum_required(VERSION 3.28)
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
# Those three cover every offering script here. A file carrying the token in no dispatch
# position is reported as NOT offering, and the count is printed, so a spelling this table
# cannot read shows up as the offering count falling rather than as silence.
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
string(REPLACE ";" "\\;" registrationSplit "${registrationRaw}")
string(REPLACE "[" " " registrationSplit "${registrationSplit}")
string(REPLACE "]" " " registrationSplit "${registrationSplit}")
string(REPLACE "\r\n" "\n" registrationSplit "${registrationSplit}")
string(REPLACE "\n" ";" registrationLines "${registrationSplit}")
set(registrationContent "")
foreach(registrationLine IN LISTS registrationLines)
    if(registrationLine MATCHES "^[ \t]*#")
        continue()
    endif()
    string(APPEND registrationContent "${registrationLine}\n")
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
set(allScripts ${shScripts} ${ps1Scripts})

list(LENGTH allScripts scriptCount)
if(scriptCount EQUAL 0)
    message(FATAL_ERROR
        "no scripts were found under ${FASTCACHED_SOURCE_DIR}/scripts; this check would "
        "pass vacuously. Either the tree moved or the glob stopped matching -- and a "
        "check reporting clean over an empty set is the defect it exists to prevent.")
endif()

set(offeringCount 0)
set(carryingCount 0)
set(unregistered "")

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
    # MEASURED, on this tree (Windows, CMake 4.3.1, 57 scripts / 1.59 MB), because the
    # walk copies the remainder of the file per line and the cost is therefore a
    # property of what a file CONTAINS rather than of what it declares:
    #
    #                                      without reject   with reject
    #     as the tree stands                    201 ms          201 ms
    #     + ONE comment line naming the token
    #       in the 3,658-line check-e2e-helpers.sh
    #                                           499 ms          200 ms
    #
    # So it buys IMMUNITY, not speed, and the distinction is the whole reason it is
    # here. The steady-state total does not move -- the 29 files that really do offer
    # are still walked, and they are where the time goes. What it removes is a check
    # whose cost TRIPLES because somebody wrote a sentence in an unrelated file, which
    # is a trap laid for whoever writes the sentence rather than a cost anybody chose.
    #
    # 201 ms is unremarkable beside its neighbours (`check-script-check-signals` runs at
    # ~962 ms). Getting the steady state down means not walking line-by-line at all --
    # a token-jump walk measures ~69 ms and is #1168, kept out of this branch because
    # bounding a line to a window changes what "the line" means for a very long one.
    if(scriptExt STREQUAL ".ps1")
        if(NOT scriptContent MATCHES "\\[switch\\][ \t]*\\$SelfTest")
            continue()
        endif()
    elseif(NOT scriptContent MATCHES "--self-test\\)"
            AND NOT scriptContent MATCHES "[\"']--self-test[\"']")
        continue()
    endif()

    set(offerShape "")
    set(rest "${scriptContent}")
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

        string(FIND "${line}" "${token}" lineTokenAt)
        if(lineTokenAt EQUAL -1)
            continue()
        endif()
        if(line MATCHES "^[ \t]*#")
            continue()
        endif()

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
        # The shape is all that is wanted, so stop at the first one rather than walking
        # the rest of a 3,600-line script to learn nothing further.
        if(NOT offerShape STREQUAL "")
            break()
        endif()
    endwhile()

    if(offerShape STREQUAL "")
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

message(STATUS
    "check-selftest-registered: ${scriptCount} script(s) scanned, ${carryingCount} "
    "carrying the token, ${offeringCount} offering a self-test")

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
