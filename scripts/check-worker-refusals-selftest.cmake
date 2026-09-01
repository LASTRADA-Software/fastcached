# SPDX-License-Identifier: Apache-2.0
#
# `worker-refusals-counted` must be seen to FAIL, on a file nobody listed.
#
# The check it drives replaced a hand-maintained list of source files with a glob,
# because the list was exact about the files it knew and silent about the ones it did
# not -- and silence reads identically to complete coverage
# ([#492](https://github.com/LASTRADA-Software/fastcached/issues/492)). A glob is only
# worth more than the list if it actually bites on a file that was never named, and
# **a guard nobody has seen fire is not a guard**.
#
# So this drives the real check against SYNTHETIC source trees and asserts each
# verdict. The trees are built here rather than in the repository: a fixture file
# containing a bare `EncodeErrorReply` inside `src/` would be a violation of the very
# rule under test, and excluding it would put a hole in the thing being proved.
#
# ## Why it inverts here rather than with `WILL_FAIL`
#
# `WILL_FAIL` on a whole test says only "something came back red". This asserts each
# direction separately and names which one broke -- a check that stopped refusing and
# a check that started refusing everything are opposite defects, and a bare inversion
# reports them identically. That is the same distinction
# `node-scratch-isolation-e2e-selftest` draws: **a classifier that cannot be made to
# say every one of its verdicts has not been shown to classify anything.**
#
# ## Why `cmake -P` and not a shell script
#
# Two reasons, both of which this project has paid for. A hygiene script `ctest` runs
# is constrained to bash 3.2, because macOS ships a 2007 `/bin/bash`. And
# `producer | grep -q` is a false NEGATIVE under `set -o pipefail`, on the SUCCESS
# path, because `grep -q` exits at the first match and the producer dies of SIGPIPE.
# Staying in CMake means neither applies rather than surviving both: `execute_process`
# hands back the output as a variable and `string(FIND)` matches it afterwards, which
# is the shape those two rules argue for anyway.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-worker-refusals-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-worker-refusals-counted.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(refusalHeader "src/FastCache/Protocol/SurfaceRefusal.hpp")
set(wireHeader "src/FastCache/Protocol/CompileCacheWire.hpp")
set(root "${FASTCACHED_SCRATCH_DIR}/worker-refusals-selftest")
file(REMOVE_RECURSE "${root}")

set(failures "")

# ---------------------------------------------------------------------------
# Build one synthetic tree.
#
# The two real headers are COPIED rather than stubbed, so a rename or a signature
# change in either shows up here as a failing selftest rather than as a fixture that
# has quietly stopped resembling the thing it stands in for.
# @param name Which case; also the directory.
# @param extraPath Where the case's own file goes, or empty for none.
# @param extraBody What that file contains.
function(fastcached_make_tree name extraPath extraBody outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/src/FastCache/Protocol")
    configure_file("${FASTCACHED_SOURCE_DIR}/${refusalHeader}" "${tree}/${refusalHeader}" COPYONLY)
    configure_file("${FASTCACHED_SOURCE_DIR}/${wireHeader}" "${tree}/${wireHeader}" COPYONLY)
    if(NOT extraPath STREQUAL "")
        get_filename_component(directory "${tree}/${extraPath}" DIRECTORY)
        file(MAKE_DIRECTORY "${directory}")
        file(WRITE "${tree}/${extraPath}" "${extraBody}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the check against a tree and record whether it objected.
# @param tree The synthetic source directory.
function(fastcached_run_check tree outObjected outOutput)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${tree}" -P "${check}"
        OUTPUT_VARIABLE captured
        ERROR_VARIABLE capturedErrors
        RESULT_VARIABLE ignored)
    set(combined "${captured}${capturedErrors}")

    # The verdict is read from the OUTPUT, never from the exit code: a `cmake -P`
    # script's `message(FATAL_ERROR)` exits 0 on CMake 3.28, this project's declared
    # minimum, and 1 on 4.x. Reading the status here would make this selftest agree
    # with the check on one host and disagree on the other.
    string(FIND "${combined}" "CMake Error" position)
    if(position EQUAL -1)
        set(${outObjected} FALSE PARENT_SCOPE)
    else()
        set(${outObjected} TRUE PARENT_SCOPE)
    endif()
    set(${outOutput} "${combined}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# The cases.
#
# A clean tree, so an always-failing check is caught. Then the three ways the old
# list-driven check was blind, each of which must now bite.

# 1. Clean: one ordinary surface that refuses through the counted spelling.
set(cleanBody [==[
// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/SurfaceRefusal.hpp>

std::vector<std::byte> AnswerSomething(FastCache::IMetricsSink& metrics)
{
    return FastCache::Cc::Refuse(metrics,
                                 FastCache::Cc::SurfaceRefusal { .code = CompileCacheWire::ErrorCode::MalformedFrame,
                                                                 .counter = FastCache::IMetricsSink::Counter::Nothing },
                                 "synthetic");
}
]==])
fastcached_make_tree("clean" "src/apps/some-new-surface/NewSurface.cpp" "${cleanBody}" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures
         "clean: the check objected to a tree whose only refusal is routed -- it now refuses everything, which is as useless as refusing nothing")
endif()
string(FIND "${output}" "none awaiting triage" position)
if(position EQUAL -1)
    list(APPEND failures
         "clean: the check did not report an empty triage backlog, so the tally is not being computed from the tree it scanned")
endif()

# 2. A NEW `.cpp` nobody listed. This is the whole ticket: under the old check this
#    file was invisible by construction, and five real sites accumulated that way.
set(bareCallBody [==[
// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/CompileCacheWire.hpp>

std::vector<std::byte> AnswerSomething()
{
    return CompileCacheWire::EncodeErrorReply(CompileCacheWire::ErrorCode::MalformedFrame, "synthetic");
}
]==])
fastcached_make_tree("new-source" "src/apps/some-new-surface/NewSurface.cpp" "${bareCallBody}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures
         "new-source: a NEW .cpp answering a refusal with a bare EncodeErrorReply did not fail the check -- which is exactly the hole #492 is about")
else()
    string(FIND "${output}" "src/apps/some-new-surface/NewSurface.cpp" position)
    if(position EQUAL -1)
        list(APPEND failures
             "new-source: the check objected but did not name the offending file, so a person reading the failure cannot act on it")
    endif()
endif()

# 3. The same call in a HEADER. #447 put two security counters in `Responders.hpp`
#    and the old scan covered `.cpp` files only, so it could not see them at all.
fastcached_make_tree("new-header" "src/apps/some-new-surface/NewSurface.hpp" "${bareCallBody}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures
         "new-header: a bare EncodeErrorReply in a HEADER did not fail the check -- headers are where refusal rows now legitimately live")
endif()

# 4. An untriaged refusal passes, and is COUNTED. The third spelling is only safe
#    because the backlog it creates is visible; a tally that printed a total it never
#    computed would be the same silence in a different font.
set(untriagedBody [==[
// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/SurfaceRefusal.hpp>

std::vector<std::byte> AnswerSomething()
{
    return FastCache::Cc::RefuseUntriaged(
        FastCache::Cc::UntriagedRefusal { .code = CompileCacheWire::ErrorCode::MalformedFrame, .issue = 4242 });
}
]==])
fastcached_make_tree("untriaged" "src/apps/some-new-surface/NewSurface.cpp" "${untriagedBody}" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures
         "untriaged: the check refused a site marked RefuseUntriaged, which is a legitimate answer and must pass while being counted")
endif()
string(FIND "${output}" "1 refusal site(s) awaiting triage" position)
if(position EQUAL -1)
    list(APPEND failures "untriaged: the backlog tally did not report the one untriaged site it was given")
endif()
string(FIND "${output}" "awaiting #4242" position)
if(position EQUAL -1)
    list(APPEND failures
         "untriaged: the tally did not resolve the site to its issue, so neither issue gets the completion test the count is for")
endif()

# 5. An untriaged call that names no issue. The backlog is read twice -- from the
#    calls and from the issues they name -- and reporting one of two disagreeing
#    readings as the total would put the same silence back under a number.
set(unnamedIssueBody [==[
// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/SurfaceRefusal.hpp>

std::vector<std::byte> AnswerSomething()
{
    return FastCache::Cc::RefuseUntriaged(someRowFromSomewhereElse);
}
]==])
fastcached_make_tree("issue-unnamed" "src/apps/some-new-surface/NewSurface.cpp" "${unnamedIssueBody}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures
         "issue-unnamed: an untriaged refusal naming no issue was folded into the backlog total instead of being reported as a tally that cannot conclude")
endif()

# 6. A tree where the spellings have gone. Zero findings must be reported as a scan
#    that stopped working, never as a clean tree -- the direction this project keeps
#    getting wrong, and the one a passing check hides.
set(tree "${root}/scan-broken")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}/src/FastCache/Protocol")
configure_file("${FASTCACHED_SOURCE_DIR}/${wireHeader}" "${tree}/${wireHeader}" COPYONLY)
file(WRITE "${tree}/${refusalHeader}" [==[
// SPDX-License-Identifier: Apache-2.0
#pragma once
// A SurfaceRefusal.hpp that refuses nothing: the spellings were renamed away.
]==])
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures
         "scan-broken: a SurfaceRefusal.hpp with no refusal spelling in it was reported as a clean tree, so a rename would silence this check without saying so")
endif()

# ---------------------------------------------------------------------------
if(failures)
    list(LENGTH failures failureCount)
    message("")
    foreach(failure IN LISTS failures)
        message("  ${failure}")
    endforeach()
    message("")
    message("`worker-refusals-counted` is the only thing standing between a new surface")
    message("and the accumulation #327 and #447 both record. Each case above drives it")
    message("against a synthetic tree and asserts ONE verdict, so a failure here names")
    message("the direction that broke rather than only that something did.")
    message(FATAL_ERROR "worker refusals selftest: ${failureCount} verdict(s) wrong")
endif()

message(STATUS "worker refusals selftest: 6 synthetic tree(s), every verdict as expected")
