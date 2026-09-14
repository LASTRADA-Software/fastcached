# SPDX-License-Identifier: Apache-2.0
#
# `fastcached_vendor_link_roots_verdict` must be SEEN to accept every home a link checks and to refuse
# every other (#1376).
#
# The configure calls it over the real project's targets, and that call can only ever show the verdict
# the tree happens to earn -- accepting, on a good tree. So each case here hands it synthetic records:
#
#   probeLibrary        a vendored .cpp in an object library the probe links       accepts, counted in the probe
#   ownExecutable       a vendored .cpp in an executable that builds by default    accepts, counted as its own link
#   staticLibrary       a vendored .cpp in a static library                        refuses: an archive resolves nothing
#   sharedLibrary       a vendored .cpp in a shared library                        refuses: the loader may resolve it
#   unlinkedObjects     a vendored .cpp in an object library the probe does not    refuses
#   excludedExecutable  a vendored .cpp in an EXCLUDE_FROM_ALL executable          refuses: its link runs never
#   generatorExpression a source genex naming the vendored tree                    refuses: unresolvable here
#   interfaceSources    a vendored .cpp a target hands to whatever links it        refuses: the walk cannot follow it
#   unknownCarrier      a carrier the verdict has no row for                       refuses, saying so
#   ignored             tests, headers, first-party sources, $<TARGET_OBJECTS:>    accepted without being counted
#   emptyProbe          nothing in the probe's own libraries                      refuses: the walk stopped working
#   malformed           a record missing a field                                   refuses
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<dir> -P scripts/check-vendor-link-roots-selftest.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED FASTCACHED_SOURCE_DIR)
    message(FATAL_ERROR "FASTCACHED_SOURCE_DIR must be set")
endif()
set(module "${FASTCACHED_SOURCE_DIR}/cmake/VendorLinkRoots.cmake")
if(NOT EXISTS "${module}")
    message(FATAL_ERROR "the module under test is missing: ${module}")
endif()
include("${module}")

set(root "/tree/vendor/endo")
set(probe "fastcache-tui-objects")
set(inProbeRecord "fastcache-tui-objects|OBJECT_LIBRARY|0|${root}/tui/Box.cpp")

set(ran 0)
set(mismatches "")

# Requires the verdict over @p records to refuse exactly when @p refuses, a problem sentence to contain
# @p expected when it does, and the two counts to be @p wantProbe and @p wantExecutables when it does not.
function(fastcached_case name refuses expected wantProbe wantExecutables)
    fastcached_vendor_link_roots_verdict(problems inProbe inExecutables
        VENDOR_ROOT "${root}" PROBE_LIBRARIES ${probe} RECORDS ${ARGN})
    list(JOIN problems " / " said)
    set(problem "")
    if(refuses AND NOT problems)
        set(problem "accepted, and must refuse")
    elseif(NOT refuses AND problems)
        set(problem "refused, and must accept: ${said}")
    elseif(refuses)
        string(FIND "${said}" "${expected}" found)
        if(found EQUAL -1)
            set(problem "refused for the wrong reason, without `${expected}`: ${said}")
        endif()
    elseif(NOT inProbe EQUAL wantProbe OR NOT inExecutables EQUAL wantExecutables)
        set(problem "counted ${inProbe} in the probe and ${inExecutables} in executables, not ${wantProbe} and ${wantExecutables}")
    endif()
    math(EXPR count "${ran} + 1")
    set(ran ${count} PARENT_SCOPE)
    if(NOT problem STREQUAL "")
        set(mismatches "${mismatches}\n  ${name}: ${problem}" PARENT_SCOPE)
    endif()
endfunction()

fastcached_case(probeLibrary OFF "" 1 0 "${inProbeRecord}")
fastcached_case(ownExecutable OFF "" 1 1
    "${inProbeRecord}" "fastcache-tui-tests|EXECUTABLE|0|${root}/platform/SystemPipe.cpp")
fastcached_case(staticLibrary ON "an archive whose members are resolved only when something links it" 0 0
    "${inProbeRecord}" "fastcache-tui|STATIC_LIBRARY|0|${root}/platform/SignalHandler.cpp")
fastcached_case(sharedLibrary ON "a shared library, whose link may leave symbols for the loader" 0 0
    "${inProbeRecord}" "tui-dll|SHARED_LIBRARY|0|${root}/platform/SignalHandler.cpp")
fastcached_case(unlinkedObjects ON "an object library fastcache-tui-linkprobe does not link" 0 0
    "${inProbeRecord}" "more-objects|OBJECT_LIBRARY|0|${root}/platform/SignalHandler.cpp")
fastcached_case(excludedExecutable ON "excluded from the default build" 0 0
    "${inProbeRecord}" "a-canary|EXECUTABLE|1|${root}/platform/SystemPipe.cpp")
fastcached_case(generatorExpression ON "a generator expression naming the vendored tree" 0 0
    "${inProbeRecord}" "fastcache-cli|EXECUTABLE|0|$<$<BOOL:ON>:vendor/endo/tui/Box.cpp>")
fastcached_case(interfaceSources ON "handing it to whatever links the target" 0 0
    "${inProbeRecord}" "tui-sources|INTERFACE_SOURCES|0|${root}/platform/SignalHandler.cpp")
fastcached_case(unknownCarrier ON "a carrier this check has no row for" 0 0
    "${inProbeRecord}" "odd|UNKNOWN_LIBRARY|0|${root}/platform/SignalHandler.cpp")
fastcached_case(ignored OFF "" 1 0
    "${inProbeRecord}"
    "fastcache-tui-tests|EXECUTABLE|0|${root}/tui/runtime/TuiRuntime_test.cpp"
    "fastcache-tui|STATIC_LIBRARY|0|${root}/tui/Box.hpp"
    "fastcache-tui|STATIC_LIBRARY|0|$<TARGET_OBJECTS:fastcache-tui-objects>"
    "FastCache|STATIC_LIBRARY|0|/tree/src/FastCache/Core/Clock.cpp"
    "neighbour|STATIC_LIBRARY|0|/tree/vendor/endo-other/X.cpp")
fastcached_case(emptyProbe ON "no vendored translation unit was found in the probe's object libraries" 0 0
    "fastcache-tui-tests|EXECUTABLE|0|${root}/platform/SystemPipe.cpp")
fastcached_case(malformed ON "is not `<target>|<type>|<0 or 1>|<source>`" 0 0
    "${inProbeRecord}" "fastcache-tui|STATIC_LIBRARY|${root}/tui/Box.cpp")

if(NOT mismatches STREQUAL "")
    message(FATAL_ERROR "vendor-link-roots-selftest: ${ran} case(s) ran, and these did not judge as they must:${mismatches}")
endif()
message(STATUS "vendor-link-roots-selftest: ${ran} case(s) ran, every verdict as it must be")
