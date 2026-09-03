# SPDX-License-Identifier: Apache-2.0
#
# `bind-failure-seam` must be seen to fail, in each of the four ways it can.
#
# The check it drives is the CODE half of
# [#352](https://github.com/LASTRADA-Software/fastcached/issues/352). The table half
# is a `static_assert` and needs no selftest: a row omitting the column does not
# compile, which is a fact anyone can verify by trying it. The code half is a source
# scan, and a source scan that has never been watched refuse is indistinguishable
# from one that matches nothing -- which is the exact defect it was written to close.
#
# So this drives the real check against SYNTHETIC trees and asserts each verdict
# separately. Not `WILL_FAIL`, which says only "something came back red": a check
# that stopped refusing and a check that started refusing everything are opposite
# defects, and a bare inversion reports them identically. **A classifier that cannot
# be made to say every one of its verdicts has not been shown to classify anything.**
#
# The trees are built here rather than kept in the repository, because a fixture
# containing an opener that bypasses the seam would be a violation of the rule under
# test, and excluding it would put a hole in the thing being proved.
#
# ## Why `cmake -P` and not a shell script
#
# A hygiene script `ctest` runs is constrained to bash 3.2 -- macOS ships a 2007
# `/bin/bash` -- and `producer | grep -q` is a false NEGATIVE under `pipefail` on the
# SUCCESS path. Staying in CMake means neither applies rather than surviving both.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-bind-failure-selftest.cmake
#
# Exit codes: 0 always. The verdict is the presence of `CMake Error` in the output.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
if(NOT DEFINED FASTCACHED_SCRATCH_DIR)
    message(FATAL_ERROR "FASTCACHED_SCRATCH_DIR must be set")
endif()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-bind-failure-seam.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(nodeDirectory "src/apps/fastcache-compile-node")
set(root "${FASTCACHED_SCRATCH_DIR}/bind-failure-selftest")
file(REMOVE_RECURSE "${root}")

set(failures "")

# ---------------------------------------------------------------------------
# A file naming every bind primitive AND the seam.
#
# Every case needs this, because a primitive matching nothing is itself one of the
# verdicts under test: without it each tree would fail for the wrong reason and each
# case would pass while proving nothing.
set(compliantBody [==[
// SPDX-License-Identifier: Apache-2.0
#include "NodeSurfaces.hpp"

namespace FastCache::Node
{

std::expected<void, std::string> OpenEverything(NodeIoLoop& io, NodeConfig const& cfg, ILogger& logger)
{
    auto started = FrameEndpoint::Start(io, NodeSurface::Node, cfg, responder, metrics, logger);
    auto adopted = FrameEndpoint::StartAdopted(io, NodeSurface::Node, 3, host, responder, metrics, logger);
    auto raft = PlatformListener::Bind(reactor, "127.0.0.1", 6675);
    auto admin = BlockingListener::Bind("127.0.0.1", 6676);
    auto beacon = OpenSharedPortUdpSocket("0.0.0.0", 6677, 0);

    if (!started.has_value())
    {
        auto judged = JudgeBindFailure(RowFor(NodeSurface::Node), "cannot bind", logger);
        if (!judged.has_value())
            return std::unexpected { std::move(judged).error() };
    }
    return {};
}

} // namespace FastCache::Node
]==])

# Build a tree: the compliant file, plus an optional extra file.
# @param name Which case; also the directory.
# @param extraPath Where the case's own file goes, or empty for none.
# @param extraBody What that file contains.
function(fastcached_make_tree name extraPath extraBody outVar)
    set(tree "${root}/${name}")
    file(REMOVE_RECURSE "${tree}")
    file(MAKE_DIRECTORY "${tree}/${nodeDirectory}")
    file(WRITE "${tree}/${nodeDirectory}/Compliant.cpp" "${compliantBody}")
    if(NOT extraPath STREQUAL "")
        get_filename_component(directory "${tree}/${extraPath}" DIRECTORY)
        file(MAKE_DIRECTORY "${directory}")
        file(WRITE "${tree}/${extraPath}" "${extraBody}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the check against a tree and record whether it objected.
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
# 1. Clean. Catches a check that refuses everything, which is as useless as one that
#    refuses nothing and much easier to ship.
fastcached_make_tree("clean" "" "" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "clean: the check objected to a tree whose only opener routes through the seam")
endif()

# 2. A NEW opener that binds and decides for itself. THE case: this compiles, links
#    and passes every test in the real tree, and puts the verdict back where #352
#    found it.
set(bypassBody [==[
// SPDX-License-Identifier: Apache-2.0
#include "NodeSurfaces.hpp"

std::expected<void, std::string> StartFifthSurface(ILogger& logger)
{
    auto listener = BlockingListener::Bind("0.0.0.0", 6699);
    if (!listener || !listener->IsBound())
        return std::unexpected { std::format("cannot bind 0.0.0.0:6699: {}", listener->BindError()) };
    return {};
}
]==])
fastcached_make_tree("bypass" "${nodeDirectory}/FifthSurface.cpp" "${bypassBody}" tree)
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "bypass: an opener that binds and refuses on its own did not fail the check -- which is the whole of #352's code half")
else()
    string(FIND "${output}" "FifthSurface.cpp" position)
    if(position EQUAL -1)
        list(APPEND failures "bypass: the check objected but did not name the offending file, so a person reading the failure cannot act on it")
    endif()
endif()

# 3. A primitive named in PROSE. Both of this check's first two findings were doc
#    comments -- `AdminEndpoint.hpp` and `main.cpp` each explain a decision by naming
#    `FrameEndpoint::Start` -- and whole-file matching called both of them violations.
#    A checker that fails on documentation of the thing it checks is one nobody can
#    document, which is the trap `check-target-file-guards` records.
set(proseBody [==[
// SPDX-License-Identifier: Apache-2.0

/// A surface, never an address, for the reason `FrameEndpoint::Start` is: the row
/// decides where a bare port lands. This file binds nothing and must not be read as
/// though it did -- `BlockingListener::Bind` is named here only to contrast with it.
struct SomethingDocumented
{
};
]==])
fastcached_make_tree("prose" "${nodeDirectory}/Documented.hpp" "${proseBody}" tree)
fastcached_run_check("${tree}" objected output)
if(objected)
    list(APPEND failures "prose: a file NAMING a bind primitive in a doc comment was reported as an opener, so the check cannot survive its own documentation")
endif()

# 4. The scan matching nothing. A moved source root reads exactly like a tree with
#    no violations in it, and the difference has to be a failure (#492).
set(tree "${root}/empty")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}/${nodeDirectory}")
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "empty: a tree with no source file at all passed, so a moved source root would report a clean scan")
else()
    string(FIND "${output}" "matched no source file" position)
    if(position EQUAL -1)
        list(APPEND failures "empty: the check objected without saying its scan found nothing, so the failure reads as a violation that is not there")
    endif()
endif()

# 5. A primitive nobody calls. What a RENAME looks like, after which the check keeps
#    passing while guarding a name that no longer exists -- a guard that fires only
#    when nothing is wrong.
set(renamedBody [==[
// SPDX-License-Identifier: Apache-2.0
#include "NodeSurfaces.hpp"

std::expected<void, std::string> OpenSome(NodeIoLoop& io, ILogger& logger)
{
    auto started = FrameEndpoint::Start(io, NodeSurface::Node, cfg, responder, metrics, logger);
    auto adopted = FrameEndpoint::StartAdopted(io, NodeSurface::Node, 3, host, responder, metrics, logger);
    auto raft = PlatformListener::Bind(reactor, "127.0.0.1", 6675);
    auto admin = BlockingListener::Bind("127.0.0.1", 6676);
    if (!started.has_value())
    {
        auto judged = JudgeBindFailure(RowFor(NodeSurface::Node), "cannot bind", logger);
        if (!judged.has_value())
            return std::unexpected { std::move(judged).error() };
    }
    return {};
}
]==])
set(tree "${root}/renamed")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}/${nodeDirectory}")
file(WRITE "${tree}/${nodeDirectory}/OpenSome.cpp" "${renamedBody}")
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "renamed: a tree where OpenSharedPortUdpSocket appears nowhere passed, so a renamed primitive would leave the check guarding a name nobody calls")
else()
    string(FIND "${output}" "OpenSharedPortUdpSocket" position)
    if(position EQUAL -1)
        list(APPEND failures "renamed: the check objected without naming which primitive matched nothing, so nobody can tell which row went stale")
    endif()
endif()

# 6. The seam itself missing. Distinct from case 2: there, ONE opener bypassed it and
#    others still called it. Here nothing does, which is what a rename of the seam
#    looks like, and the check must say so rather than reporting every opener.
set(tree "${root}/no-seam")
file(REMOVE_RECURSE "${tree}")
file(MAKE_DIRECTORY "${tree}/${nodeDirectory}")
string(REPLACE "JudgeBindFailure" "SomeOtherName" seamlessBody "${compliantBody}")
file(WRITE "${tree}/${nodeDirectory}/Compliant.cpp" "${seamlessBody}")
fastcached_run_check("${tree}" objected output)
if(NOT objected)
    list(APPEND failures "no-seam: a tree where nothing calls the seam passed, so renaming JudgeBindFailure would disarm the check silently")
else()
    string(FIND "${output}" "no use of JudgeBindFailure" position)
    if(position EQUAL -1)
        list(APPEND failures "no-seam: the check objected without saying the seam is unused, so a rename reads as every opener having gone wrong at once")
    endif()
endif()

# ---------------------------------------------------------------------------
if(failures)
    message("")
    foreach(entry IN LISTS failures)
        message("  ${entry}")
    endforeach()
    message("")
    list(LENGTH failures failureCount)
    message(FATAL_ERROR "bind-failure selftest: ${failureCount} verdict(s) the check could not be made to reach")
endif()

message("bind-failure selftest: 6 verdicts driven -- clean, bypass, prose, empty scan, renamed primitive, missing seam")
