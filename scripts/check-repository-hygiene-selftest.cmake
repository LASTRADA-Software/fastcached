# SPDX-License-Identifier: Apache-2.0
#
# `check-repository-hygiene.cmake`'s four ways of not having an index, driven apart.
#
# The check printed two different sentences for two of them and returned 0 from both,
# so ctest scored them identically: `SKIP: git could not answer here` and
# `SKIP: ... is not a git work tree` were one verdict wearing two words (#1565).
#
# ## The distinction the seam cannot draw
#
# `fastcached_work_tree_state` answers in three words, and `no-git` is three situations:
#
#   * git is not installed, or cannot run at all;
#   * this directory is in no repository -- an exported source tarball;
#   * this directory IS a checkout and git cannot follow its `.git`.
#
# `git rev-parse --is-inside-work-tree` exits 128 for the last two alike and says "not a
# git repository" to both, so no amount of reading its answer separates them. What does
# is a fact the directory carries and the seam never asked for: whether `.git` is there.
#
# The third is the one that must not skip. A tarball has no index and never will; a
# checkout git cannot read is a broken host, and a check with no directory-walk fallback
# that skips there has reported NOTHING about a tree it was pointed straight at.
#
# ## Each case arranges its state, and asserts WHICH one it got
#
# A case that merely asserted "refused" or "skipped" would pass on the neighbouring
# state, which is the whole defect: the two sentences were already different and the
# VERDICTS were not. So every case matches the sentence as well.
#
# ## Read from the OUTPUT, never from the exit code
#
# `message(WARNING)` exits 0 on every CMake while printing `CMake Warning`, and this
# check's own SKIP path exits 0 while printing `SKIP: `. The exit status can carry
# neither distinction. Flattened first, because CMake wraps its diagnostics at a column
# that depends on the scratch path's length.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-repository-hygiene.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

# The staged trees must not be INSIDE a repository, and by default they are.
#
# `FASTCACHED_SCRATCH_DIR` is under the build directory, which for this project's own
# presets is `out/build/...` -- inside the checkout. So `git rev-parse` started in a
# staged "tarball" tree walks UP, finds the real repository, and answers `work-tree`:
# the state this fixture meant to arrange never existed, and state 2 failed against a
# correct check. It passed when first written only because that run used a scratch
# directory outside the tree, which is the "the tree you measured is not the tree in
# question" trap in its cheapest form.
#
# `GIT_CEILING_DIRECTORIES` stops the upward walk. It is exported to this process, so
# every `git` below -- the ones this file runs and the ones the check runs -- inherits
# it.
set(ENV{GIT_CEILING_DIRECTORIES} "${FASTCACHED_SCRATCH_DIR}")

find_program(GIT_EXECUTABLE NAMES git)
if(NOT GIT_EXECUTABLE)
    # Not a skip: three of the four cases need git to ARRANGE their state, so a run
    # without it would report on one case and look like a run that judged four.
    message(FATAL_ERROR
        "no git executable found, so three of this self-test's four states cannot be "
        "arranged. That is not a clean run and must not be scored as one.")
endif()

set(ran 0)
set(failures "")

# Run the check over `tree`, with `gitExecutable` as the git it is handed.
#
# @param name The case name.
# @param tree The directory to point the check at.
# @param gitExecutable The git to pass, or `-` to let the check find one.
# @param outFlat Set to the combined output, flattened onto one line.
function(RunCheck name tree gitExecutable outFlat)
    set(gitArg "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}")
    if(NOT gitExecutable STREQUAL "-")
        set(gitArg "-DGIT_EXECUTABLE=${gitExecutable}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" "${gitArg}"
                -P "${check}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(combined "${out}${err}")
    string(REGEX REPLACE "[\r\n]+" " " combined "${combined}")
    string(REGEX REPLACE " +" " " combined "${combined}")
    set(${outFlat} "${combined}" PARENT_SCOPE)
endfunction()

# Assert a case's verdict and the words it reached the verdict with.
#
# @param name The case name.
# @param flat The flattened output.
# @param expect `pass`, `skip` or `refuse`.
# @param phrase A phrase the output must contain.
function(ExpectVerdict name flat expect phrase)
    math(EXPR ran "${ran} + 1")
    set(ran "${ran}" PARENT_SCOPE)

    # Order matters: a refusal that also printed `SKIP: ` would be scored a skip by
    # ctest on some orderings and a failure on others, so the check must never print
    # both. Reading the refusal first here is what would catch it.
    if(flat MATCHES "CMake Error|CMake Warning")
        set(verdict "refuse")
    elseif(flat MATCHES "SKIP: ")
        set(verdict "skip")
    else()
        set(verdict "pass")
    endif()

    set(problem "")
    if(NOT verdict STREQUAL expect)
        set(problem "expected ${expect}, got ${verdict}")
    elseif(NOT flat MATCHES "${phrase}")
        set(problem "${verdict} was right but the words were not: expected `${phrase}`")
    elseif(verdict STREQUAL "refuse" AND flat MATCHES "SKIP: ")
        # A refusal carrying the skip marker is scored by ctest's SKIP_REGULAR_EXPRESSION
        # as well, and the two properties do not have a stated precedence. The check must
        # not put a reader's verdict in that position.
        set(problem "the refusal also printed `SKIP: `, which ctest may score as a skip")
    endif()

    if(problem STREQUAL "")
        message(STATUS "ok   ${name}")
    else()
        message(STATUS "FAIL ${name}: ${problem}")
        message(STATUS "     output: ${flat}")
        list(APPEND failures "${name}")
        set(failures "${failures}" PARENT_SCOPE)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# State 1 -- a real checkout. The accepting direction, and it is not decoration: every
# case below is a way of NOT running the rule, so without this one a check that refused
# or skipped unconditionally would pass the lot.
set(tree "${FASTCACHED_SCRATCH_DIR}/work-tree")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}/scripts")
file(WRITE "${tree}/scripts/ordinary.sh" "#!/usr/bin/env bash\necho hello\n")
execute_process(COMMAND "${GIT_EXECUTABLE}" init -q "${tree}" OUTPUT_QUIET ERROR_QUIET)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${tree}" add -A OUTPUT_QUIET ERROR_QUIET)
RunCheck("work-tree" "${tree}" "-" flat)
ExpectVerdict("state 1: a checkout is inspected and accepted" "${flat}" "pass" "")

# ---------------------------------------------------------------------------
# State 2 -- no `.git` and no repository: an exported source tarball. Ordinary, and it
# must stay a skip. The registration in `src/tests/CMakeLists.txt` is written around
# this case, so turning `no-git` into a refusal outright -- which is the obvious reading
# of "git could not answer is an environment fault" -- would have failed every tarball.
set(tarball "${FASTCACHED_SCRATCH_DIR}/tarball")
file(REMOVE_RECURSE "${tarball}")
file(MAKE_DIRECTORY "${tarball}/scripts")
file(WRITE "${tarball}/scripts/ordinary.sh" "#!/usr/bin/env bash\necho hello\n")
# The arrangement is ASSERTED, not assumed. This is the one state whose absence is
# silent: a tarball tree that IS inside a repository looks like an ordinary checkout,
# so the case fails reporting a defect in the CHECK when the fixture is what broke.
# Refused here instead, naming the cause, because that is a different finding.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${tarball}" rev-parse --is-inside-work-tree
    OUTPUT_VARIABLE tarballInsideTree ERROR_QUIET RESULT_VARIABLE tarballStatus
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(tarballStatus EQUAL 0 AND tarballInsideTree STREQUAL "true")
    message(FATAL_ERROR
        "the staged tarball tree at ${tarball} is INSIDE a git work tree, so the state "
        "this case exists to drive cannot be arranged here and the case would report a "
        "defect in the check rather than in this fixture. GIT_CEILING_DIRECTORIES was "
        "set to ${FASTCACHED_SCRATCH_DIR} and did not stop git's upward walk -- check "
        "that the scratch path is absolute and unsymlinked.")
endif()
RunCheck("tarball" "${tarball}" "-" flat)
ExpectVerdict("state 2: a tree with no `.git` at all is skipped" "${flat}" "skip" "no `.git` entry")

# ---------------------------------------------------------------------------
# State 3 -- a `.git` that git cannot follow. THE case: a worktree pointer naming a
# gitdir this host cannot reach, which is what `git worktree add` leaves behind when the
# path is written for one of Windows and WSL and read from the other.
#
# Byte-identical to state 2 as far as git is concerned -- both answer `no-git`, both from
# a `rev-parse` exiting 128 with "not a git repository" -- which is why this needs the
# `.git` entry to be told apart and why it skipped for years.
set(broken "${FASTCACHED_SCRATCH_DIR}/broken-pointer")
file(REMOVE_RECURSE "${broken}")
file(MAKE_DIRECTORY "${broken}/scripts")
file(WRITE "${broken}/scripts/ordinary.sh" "#!/usr/bin/env bash\necho hello\n")
file(WRITE "${broken}/.git" "gitdir: ${FASTCACHED_SCRATCH_DIR}/no-such-gitdir-anywhere\n")
RunCheck("broken-pointer" "${broken}" "-" flat)
ExpectVerdict("state 3: a checkout git cannot read is REFUSED, not skipped"
    "${flat}" "refuse" "repair-worktree-pointers.sh")

# ---------------------------------------------------------------------------
# State 4 -- inside a repository and outside its work tree. `rev-parse` SUCCEEDS here
# and prints `false`, so this is the only situation that reaches `not-a-work-tree`, and
# it is a different sentence from state 2's. Arranged by asking about a repository's own
# `.git` directory, which is the state's own meaning rather than a trick.
RunCheck("inside-dot-git" "${tree}/.git" "-" flat)
ExpectVerdict("state 4: inside a repository, outside its work tree, is skipped"
    "${flat}" "skip" "outside its work tree")

# ---------------------------------------------------------------------------
# State 5 -- no git at all. Ordinary: nothing about the tree is claimed either way, and
# a machine without git is not a machine with a broken checkout. Kept distinct from
# state 3 on purpose -- state 3's whole argument is that git IS installed and still
# cannot read this tree.
RunCheck("no-git-binary" "${tree}" "" flat)
ExpectVerdict("state 5: no git executable is skipped, not refused"
    "${flat}" "skip" "no git executable")

# ---------------------------------------------------------------------------
if(ran EQUAL 0)
    message(FATAL_ERROR
        "check-repository-hygiene self-test ran no cases, so it asserted nothing.")
endif()
if(failures)
    list(LENGTH failures failureCount)
    string(REPLACE ";" ", " failureList "${failures}")
    message(FATAL_ERROR
        "check-repository-hygiene-selftest: ${ran} case(s) ran, "
        "${failureCount} failed: ${failureList}")
endif()
message(STATUS
    "check-repository-hygiene-selftest: ${ran} case(s) ran, all passed")
