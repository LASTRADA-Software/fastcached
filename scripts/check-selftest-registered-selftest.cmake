# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28)
#
# `check-selftest-registered.cmake` shown REFUSING, and shown ACCEPTING (#596).
#
# Both directions, because they are different edits with the same consequence and a check
# that only refuses is half-measured. #1031 is the scar for the accepting half: a guard
# that fails CLOSED and unconditionally reads as "my branch is bad", not as "the gate is
# broken", and can stand for days. A guard nobody has watched ACCEPT is not known to work.
#
# Each case stages a whole tiny tree -- `scripts/` plus `src/tests/CMakeLists.txt` -- and
# drives the REAL check over it through `FASTCACHED_SOURCE_DIR`. A fixture reimplementing
# the classifier would be a second thing to be wrong rather than a test of this one.
#
# The verdict is read from OUTPUT, never from the exit status, and the output is FLATTENED
# first: CMake wraps `message(FATAL_ERROR)` text at about 74 columns, so a phrase that
# straddles the wrap exists in the output and in no single line of it. Measured in this
# tree: a phrase crossing column 74 matches 0 times unwrapped under FATAL_ERROR and 1 time
# after flattening. A negative case written without the flatten reports a refutation.
#
# Usage:
#   cmake -DFASTCACHED_CHECK=<path to check-selftest-registered.cmake> \
#         -P scripts/check-selftest-registered-selftest.cmake

if(NOT DEFINED FASTCACHED_CHECK)
    message(FATAL_ERROR "FASTCACHED_CHECK must be set")
endif()
if(NOT EXISTS "${FASTCACHED_CHECK}")
    message(FATAL_ERROR "the check under test is missing: ${FASTCACHED_CHECK}")
endif()

# The scratch root is REQUIRED, not defaulted to `CMAKE_CURRENT_BINARY_DIR`.
#
# Under `cmake -P` there is no build tree, so that variable is the CURRENT DIRECTORY --
# and running this by hand from the checkout therefore staged eight synthetic trees
# inside the source tree. It is invisible from ctest, which runs in the build directory,
# so it would have littered only the tree of whoever ran it directly. `check-script-
# interpreter-selftest.cmake` already takes `FASTCACHED_SCRATCH_DIR` for this reason;
# this now matches it rather than inventing a second convention.
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR
        "FASTCACHED_SCRATCH_DIR must be set -- this selftest stages trees on disk and "
        "will not guess a location, because under `cmake -P` the guess is the caller's "
        "current directory")
endif()
set(stageRoot "${FASTCACHED_SCRATCH_DIR}/selftest-registered-selftest")
file(REMOVE_RECURSE "${stageRoot}")

set(caseCount 0)
set(failures "")

# Stage an empty tree and hand back its root.
#
# Script BODIES are written by the caller with `file(WRITE)` rather than passed in, and
# that is not a style choice: a `case` arm ends in `;;`, and any body carrying one is torn
# into list elements the moment it is passed through a CMake list. The first draft of this
# harness did exactly that and staged files with empty names. The bodies under test are
# shell, so semicolons are data here for the same reason brackets are data in the check
# itself.
function(NewTree name outDir)
    set(dir "${stageRoot}/${name}")
    file(MAKE_DIRECTORY "${dir}/scripts")
    file(MAKE_DIRECTORY "${dir}/src/tests")
    set(${outDir} "${dir}" PARENT_SCOPE)
endfunction()

# Run the check over a staged tree and assert the verdict.
#   want   -- "pass" or "refuse"
#   phrase -- text the FLATTENED output must contain ("" to assert nothing further)
function(ExpectVerdict name dir want phrase)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${dir}" -P "${FASTCACHED_CHECK}"
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        RESULT_VARIABLE rc
        # ENCODING NONE: `execute_process` defaults to AUTO and decodes the child through
        # the console code page, so a check would be searching a haystack its own harness
        # had rewritten.
        ENCODING NONE
    )
    set(combined "${out}${err}")
    string(REPLACE "\n" " " flat "${combined}")
    string(REGEX REPLACE "  +" " " flat "${flat}")

    # `CMake Error|CMake Warning`, not `CMake Error` alone (#672). A construct that WARNS
    # rather than errors changes behaviour inside a sub-run and would be scored a clean
    # pass -- six selftest harnesses in this tree had exactly that blind spot, and the
    # ctest registration this drives already uses the two-alternative pattern through
    # `FASTCACHED_SCRIPT_CHECK_FAILED`. A harness laxer than the registration it stands
    # for is a fake more permissive than the thing it models.
    set(refused FALSE)
    if(flat MATCHES "CMake Error|CMake Warning")
        set(refused TRUE)
    endif()

    set(problem "")
    if(want STREQUAL "refuse" AND NOT refused)
        set(problem "expected a refusal, got none")
    elseif(want STREQUAL "pass" AND refused)
        set(problem "expected acceptance, got a refusal")
    elseif(NOT phrase STREQUAL "")
        string(FIND "${flat}" "${phrase}" phraseAt)
        if(phraseAt EQUAL -1)
            set(problem "verdict was right but did not say '${phrase}'")
        endif()
    endif()

    math(EXPR caseCount "${caseCount} + 1")
    set(caseCount "${caseCount}" PARENT_SCOPE)
    if(problem STREQUAL "")
        message(STATUS "ok   ${name}")
    else()
        message(STATUS "FAIL ${name}: ${problem}")
        message(STATUS "     output: ${flat}")
        list(APPEND failures "${name}")
        set(failures "${failures}" PARENT_SCOPE)
    endif()
endfunction()

# Bodies of the staged scripts. Written per case with `file(WRITE)`; see `NewTree`.
set(ARM_BODY "case \"\${1:-}\" in
    --self-test) selfTest=1 \;\;
esac
")
set(CMP_BODY "[ \"\${1:-}\" != \"--self-test\" ] || selfTest=1
")
set(PLAIN_BODY "echo no self-test here
")
set(REG_ARM "add_test(NAME \"arm\" COMMAND bash \"scripts/check-arm.sh\" --self-test)
")
set(REG_CMP "add_test(NAME \"cmp\" COMMAND bash \"scripts/check-cmp.sh\" --self-test)
")

# ---------------------------------------------------------------------------
# Case 1 -- ACCEPTS a tree where every offering script is registered with the flag.
NewTree("accept" acceptDir)
file(WRITE "${acceptDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${acceptDir}/scripts/check-cmp.sh" "${CMP_BODY}")
file(WRITE "${acceptDir}/scripts/check-plain.sh" "${PLAIN_BODY}")
file(WRITE "${acceptDir}/src/tests/CMakeLists.txt" "${REG_ARM}${REG_CMP}")
ExpectVerdict("case 1: a tree with every self-test registered is accepted" "${acceptDir}" "pass" "")

# ---------------------------------------------------------------------------
# Case 2 -- REFUSES a script that offers a self-test and is registered nowhere. #596's
# first direction: somebody adds `--self-test` and forgets the registration.
NewTree("added" addedDir)
file(WRITE "${addedDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${addedDir}/scripts/check-cmp.sh" "${CMP_BODY}")
file(WRITE "${addedDir}/src/tests/CMakeLists.txt" "${REG_ARM}")
ExpectVerdict("case 2: an unregistered self-test is refused and NAMED" "${addedDir}" "refuse" "check-cmp.sh")

# ---------------------------------------------------------------------------
# Case 3 -- REFUSES when an existing registration is REMOVED. Same consequence as case 2,
# a different edit, and #596 asks for both because nothing makes one imply the other.
NewTree("removed" removedDir)
file(WRITE "${removedDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${removedDir}/scripts/check-cmp.sh" "${CMP_BODY}")
file(WRITE "${removedDir}/src/tests/CMakeLists.txt" "# every registration deleted
")
ExpectVerdict("case 3: a deleted registration is refused and NAMED" "${removedDir}" "refuse" "check-arm.sh")

# ---------------------------------------------------------------------------
# Case 4 -- the MODE, not merely the name. A registration running the script WITHOUT the
# flag satisfies "appears in an add_test COMMAND" and still runs no case. This is the live
# shape `check-mkdocs-validation.sh` was in, and why this check is stricter than #596's
# literal wording.
NewTree("plainmode" plainModeDir)
file(WRITE "${plainModeDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${plainModeDir}/src/tests/CMakeLists.txt"
    "add_test(NAME \"arm\" COMMAND bash \"scripts/check-arm.sh\")
")
ExpectVerdict("case 4: a registration without the flag does not count" "${plainModeDir}" "refuse" "check-arm.sh")

# ---------------------------------------------------------------------------
# Case 5 -- a COMMENT is not an offer. Without this the check refuses a correct tree the
# moment somebody documents a self-test dispatch in a header or leaves one commented out.
#
# The fixture is a commented-out CASE ARM, and that is the whole point of it. The first
# version used a prose mention -- `# see scripts/other.sh --self-test for the harness` --
# which no dispatch shape matches whether comments are skipped or not, so deleting the
# comment skip from the check left this case GREEN. It was a duplicate of case 6 wearing
# case 5's name, and reading it could not show that: only neutering the check did. The
# line below matches the arm shape exactly, so it is read as an offer the moment the skip
# goes.
NewTree("comment" commentDir)
file(WRITE "${commentDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${commentDir}/scripts/check-note.sh"
    "# The dispatch this script used to have, kept for reference:
#     --self-test) selfTest=1 \;\;
echo hi
")
file(WRITE "${commentDir}/src/tests/CMakeLists.txt" "${REG_ARM}")
ExpectVerdict("case 5: a comment mentioning the flag is not an offer" "${commentDir}" "pass" "")

# ---------------------------------------------------------------------------
# Case 6 -- a STRING LITERAL staging somebody else's command line is not an offer, and
# until #1220 it was silently NOT COUNTED, which is the same number as "does not offer".
# It is now REFUSED and named. Not hypothetical: `check-tidy-blind-spots-selftest.sh`
# printf's a fake workflow containing `bash scripts/tidy-sweep.sh --self-test`. Case 5
# does not cover it -- that line is a comment, and this one is code.
NewTree("fixture" fixtureDir)
file(WRITE "${fixtureDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${fixtureDir}/scripts/check-stage.sh"
    "printf 'jobs:\n  - run: bash scripts/other.sh --self-test\n' > fixture.yml
")
file(WRITE "${fixtureDir}/src/tests/CMakeLists.txt" "${REG_ARM}")
ExpectVerdict("case 6: an unexplained staged command line is REFUSED, not ignored"
    "${fixtureDir}" "refuse" "check-stage.sh")

# ---------------------------------------------------------------------------
# Case 6b -- and the same file is ACCEPTED once it says why. This is the direction that
# goes unwatched: a refusal with no accepting counterpart is a rule nobody can satisfy,
# and #1031 stood for two days because a guard's passing direction had never been seen.
NewTree("fixture-marked" markedDir)
file(WRITE "${markedDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${markedDir}/scripts/check-stage.sh"
    "# selftest-offer: this stages ANOTHER script's command line into a fixture
printf 'jobs:\n  - run: bash scripts/other.sh --self-test\n' > fixture.yml
")
file(WRITE "${markedDir}/src/tests/CMakeLists.txt" "${REG_ARM}")
ExpectVerdict("case 6b: a staged command line with a stated reason is accepted"
    "${markedDir}" "pass" "")

# ---------------------------------------------------------------------------
# Case 6c -- a marker with no reason is refused. The reason is the forcing function:
# without one the marker is a way to spell "forgot" that reads as "decided", which is
# the distinction `RefuseWithoutCounter` exists to keep.
NewTree("fixture-blank" blankDir)
file(WRITE "${blankDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${blankDir}/scripts/check-stage.sh"
    "# selftest-offer:
printf 'jobs:\n  - run: bash scripts/other.sh --self-test\n' > fixture.yml
")
file(WRITE "${blankDir}/src/tests/CMakeLists.txt" "${REG_ARM}")
ExpectVerdict("case 6c: a marker stating no reason is refused"
    "${blankDir}" "refuse" "states no reason")

# ---------------------------------------------------------------------------
# Case 9 -- a registration that has been COMMENTED OUT does not count.
#
# The removal direction, and the one that fails OPEN: adding an unregistered self-test is
# refused loudly, while commenting a registration out was silent, because the offering
# side rejected comments and the registration side did not. Case 3 does not cover it --
# it DELETES the registration, and a deleted line and a commented line are different
# edits with the same consequence, which is the same argument #596 makes for having both
# case 2 and case 3 at all.
NewTree("commented" commentedDir)
file(WRITE "${commentedDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${commentedDir}/src/tests/CMakeLists.txt" "# ${REG_ARM}")
ExpectVerdict("case 9: a commented-out registration does not count as registered" "${commentedDir}" "refuse" "check-arm.sh")

# ---------------------------------------------------------------------------
# Case 7 -- the vacuity floor. A tree whose scripts offer nothing must REFUSE rather than
# report clean: that is the state the check is in if the dispatch table stops matching this
# tree's spellings, which is #596's own failure one level up.
NewTree("vacuous" vacuousDir)
file(WRITE "${vacuousDir}/scripts/check-plain.sh" "${PLAIN_BODY}")
file(WRITE "${vacuousDir}/src/tests/CMakeLists.txt" "# nothing registered
")
ExpectVerdict("case 7: a tree with no self-test at all is refused, not passed" "${vacuousDir}" "refuse" "dispatch table")

# ---------------------------------------------------------------------------
# Case 8 -- an empty scripts directory is refused too. Distinct from case 7: there the glob
# matched and nothing offered, here the glob matched nothing. Both report clean under a
# naive implementation and they are different breakages.
NewTree("empty" emptyDir)
file(WRITE "${emptyDir}/src/tests/CMakeLists.txt" "# nothing
")
ExpectVerdict("case 8: an empty scripts directory is refused" "${emptyDir}" "refuse" "pass vacuously")

# ---------------------------------------------------------------------------
# Case 6d -- the SECOND way a spelling goes unread, and it is a different reachability
# state from case 6 rather than a second example of it.
#
# Case 6's staged line is rejected by the whole-file pre-filter, which never sees a
# dispatch shape in that file at all. This one PASSES the pre-filter -- a usage banner
# quotes the flag, so `"--self-test"` is present -- and then no line matches, because
# nothing compares it against a positional parameter. The two filters disagree, which is
# exactly the state a NEW dispatch spelling arrives in.
#
# It exists because the mutation matrix found the branch unreached: deleting that whole
# refusal changed no verdict, and a property whose removal costs nothing is either dead
# code or a case nobody wrote. It was the second.
NewTree("usage-banner" bannerDir)
file(WRITE "${bannerDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${bannerDir}/scripts/check-usage.sh"
    "echo \"usage: pass '--self-test' to drive the cases\"
")
file(WRITE "${bannerDir}/src/tests/CMakeLists.txt" "${REG_ARM}")
ExpectVerdict("case 6d: a quoted flag with no dispatch is REFUSED, not ignored"
    "${bannerDir}" "refuse" "check-usage.sh")

# ---------------------------------------------------------------------------
# Case 10 -- a `.cmake` self-test offers by its NAME, and a `-P` registration runs it.
# This whole family was outside the scan until #1220: it globbed `*.sh` and `*.ps1`
# only, so 23 `*-selftest.cmake` files -- including this check's own -- were never asked
# to register. All 23 happened to be registered, which is exactly why the gap was
# invisible: the check reported clean and was right by luck rather than by reading.
NewTree("cmake-ok" cmakeOkDir)
file(WRITE "${cmakeOkDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${cmakeOkDir}/scripts/check-thing-selftest.cmake" "message(STATUS \"x\")
")
file(WRITE "${cmakeOkDir}/src/tests/CMakeLists.txt" "${REG_ARM}add_test(NAME \"thing\" COMMAND cmake -P \"scripts/check-thing-selftest.cmake\")
")
ExpectVerdict("case 10: a registered .cmake self-test is accepted" "${cmakeOkDir}" "pass" "")

# ---------------------------------------------------------------------------
# Case 11 -- and an unregistered one is refused AND NAMED. This is the case the check
# could not see at all before #1220.
NewTree("cmake-missing" cmakeMissingDir)
file(WRITE "${cmakeMissingDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${cmakeMissingDir}/scripts/check-thing-selftest.cmake" "message(STATUS \"x\")
")
file(WRITE "${cmakeMissingDir}/src/tests/CMakeLists.txt" "${REG_ARM}")
ExpectVerdict("case 11: an unregistered .cmake self-test is refused and NAMED"
    "${cmakeMissingDir}" "refuse" "check-thing-selftest.cmake")

# ---------------------------------------------------------------------------
# Case 12 -- naming the file is not running it. The `-P` is the `.cmake` analogue of
# requiring a shell registration to PASS the flag (case 4): a registration that merely
# mentions the script satisfies #596's weaker wording and executes nothing.
NewTree("cmake-noflag" cmakeNoFlagDir)
file(WRITE "${cmakeNoFlagDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${cmakeNoFlagDir}/scripts/check-thing-selftest.cmake" "message(STATUS \"x\")
")
file(WRITE "${cmakeNoFlagDir}/src/tests/CMakeLists.txt" "${REG_ARM}add_test(NAME \"thing\" COMMAND cmake \"scripts/check-thing-selftest.cmake\")
")
ExpectVerdict("case 12: a .cmake self-test named without -P does not count"
    "${cmakeNoFlagDir}" "refuse" "check-thing-selftest.cmake")

# ---------------------------------------------------------------------------
# Case 13 -- the glob-breakage cross-check. The table runs a `*-selftest.cmake` with
# `-P` and the glob finds none, so two independent sources disagree about whether this
# tree has any and the GLOB is what broke.
#
# It is a cross-check and not a bare "no .cmake self-tests found", because a tree with
# none is an ordinary tree -- every other case here is one. The first version of this
# guard required all three populations to be non-empty, which is true of the repository
# and false of every fixture in this file, and it refused all nine of them.
NewTree("cmake-glob-broke" cmakeGlobDir)
file(WRITE "${cmakeGlobDir}/scripts/check-arm.sh" "${ARM_BODY}")
file(WRITE "${cmakeGlobDir}/src/tests/CMakeLists.txt" "${REG_ARM}add_test(NAME \"gone\" COMMAND cmake -P \"scripts/check-gone-selftest.cmake\")
")
ExpectVerdict("case 13: a table naming .cmake self-tests the glob cannot find is refused"
    "${cmakeGlobDir}" "refuse" "disagree")

# ---------------------------------------------------------------------------
if(failures)
    list(LENGTH failures failureCount)
    string(REPLACE ";" ", " failureList "${failures}")
    message(FATAL_ERROR
        "check-selftest-registered-selftest: ${caseCount} case(s) ran, "
        "${failureCount} failed: ${failureList}")
endif()
message(STATUS "check-selftest-registered-selftest: ${caseCount} case(s) ran, all passed")
