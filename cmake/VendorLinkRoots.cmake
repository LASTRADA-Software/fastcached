# SPDX-License-Identifier: Apache-2.0
# VendorLinkRoots.cmake - whether every vendored translation unit the build compiles is checked by a link (#1376).
#
# The vendored TUI's completeness criterion is a LINK: every vendored translation unit any target
# compiles is linked into an executable with nothing unresolved. `fastcache-tui-linkprobe` is that link
# for the object libraries it names, and an executable that builds by default is it for its own sources.
# A vendored `.cpp` anywhere else -- a static library, an object library the probe does not link, an
# executable nothing builds -- is compiled and never checked, and a missing implementation it references
# stays invisible until something links it. That is how all three of the import's misses stayed hidden.
#
# This file is the DECISION, as a pure function over records, so `cmake -P` can drive every verdict
# (`ctest -R vendor-link-roots-selftest`). Acquisition -- walking the project's targets and resolving
# each source to an absolute path -- stays in `vendor/CMakeLists.txt`, where the targets exist.

# Judge which vendored translation units a link checks.
#
# Each record is `<target>|<carrier>|<excluded-from-all>|<source>`. The carrier is the target's TYPE when
# the source is one of its SOURCES, and `INTERFACE_SOURCES` when the target hands the source to whatever
# links it; the third field is `1` when the target is EXCLUDE_FROM_ALL and `0` otherwise; the source is as
# the target lists it -- an absolute path, or a generator expression left unexpanded.
#
#   fastcached_vendor_link_roots_verdict(<problems-var> <in-probe-var> <in-executables-var>
#       VENDOR_ROOT <dir> PROBE_LIBRARIES <target>... RECORDS <record>...)
#
# <problems-var> receives one sentence per unchecked translation unit, and one when the probe's own
# object libraries hold none at all -- an empty root accepts every tree, so it is the walk having
# stopped working rather than a clean result.
function(fastcached_vendor_link_roots_verdict problemsVar inProbeVar inExecutablesVar)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "VENDOR_ROOT" "PROBE_LIBRARIES;RECORDS")
    if(NOT arg_VENDOR_ROOT)
        message(FATAL_ERROR "fastcached_vendor_link_roots_verdict: VENDOR_ROOT is required")
    endif()
    # Exactly one trailing slash, so `endo` never prefixes `endo-other`. `/+$` and never `/*$`: CMake before
    # 4.x refuses a REGEX REPLACE whose pattern can match the empty string, and 3.28 is this tree's minimum.
    string(REGEX REPLACE "/+$" "" vendorRoot "${arg_VENDOR_ROOT}")
    string(APPEND vendorRoot "/")

    # Why each unchecked carrier checks nothing, one row each, because the reasons are not one reason: an
    # archive resolves nothing, a shared library MAY leave symbols to the loader (the ELF default, not the
    # PE one), and a carrier with no row is refused as exactly that rather than given a neighbour's sentence.
    set(whyEXECUTABLE "an executable excluded from the default build, whose link runs only when somebody names it")
    set(whyOBJECT_LIBRARY "an object library fastcache-tui-linkprobe does not link")
    set(whySTATIC_LIBRARY "a static library, an archive whose members are resolved only when something links it")
    set(whySHARED_LIBRARY "a shared library, whose link may leave symbols for the loader to resolve")
    set(whyMODULE_LIBRARY "a module library, whose link may leave symbols for the loader to resolve")
    set(whyINTERFACE_SOURCES "handing it to whatever links the target, which a walk over each target's own sources cannot follow")

    set(problems "")
    set(inProbe 0)
    set(inExecutables 0)
    foreach(record IN LISTS arg_RECORDS)
        # The source is everything after the third bar, so a generator expression may carry one.
        if(NOT record MATCHES "^([^|]+)\\|([^|]+)\\|([01])\\|(.+)$")
            list(APPEND problems "a target record is not `<target>|<type>|<0 or 1>|<source>`: ${record}")
            continue()
        endif()
        set(target "${CMAKE_MATCH_1}")
        set(type "${CMAKE_MATCH_2}")
        set(excluded "${CMAKE_MATCH_3}")
        set(source "${CMAKE_MATCH_4}")

        # A generator expression is not expanded at configure time, so which translation unit it names
        # cannot be known here. Refused when it could name the vendored tree, ignored otherwise -- which is
        # where `$<TARGET_OBJECTS:x>` lands: it names a target, never a path, and x is judged where it is.
        if(source MATCHES "^\\$<")
            string(FIND "${source}" "endo/" namesVendored)
            if(namesVendored GREATER -1)
                list(APPEND problems
                    "target `${target}` adds `${source}`, a generator expression naming the vendored tree, which "
                    "no configure-time walk can resolve to a translation unit. Name the source plainly.")
            endif()
            continue()
        endif()

        string(FIND "${source}" "${vendorRoot}" atVendor)
        if(NOT atVendor EQUAL 0 OR NOT source MATCHES "\\.cpp$" OR source MATCHES "(_test|test_main)\\.cpp$")
            continue()
        endif()

        if(type STREQUAL "EXECUTABLE" AND NOT excluded)
            math(EXPR inExecutables "${inExecutables} + 1")
        elseif(type STREQUAL "OBJECT_LIBRARY" AND target IN_LIST arg_PROBE_LIBRARIES)
            math(EXPR inProbe "${inProbe} + 1")
        else()
            if(DEFINED why${type})
                set(why "${why${type}}")
            else()
                set(why "a `${type}`, a carrier this check has no row for, so it cannot say what links it")
            endif()
            list(APPEND problems
                "target `${target}` carries ${source}, and no link checks it: `${target}` is ${why}. A vendored "
                "translation unit must be linked by an executable that builds by default, or sit in an object "
                "library named by FASTCACHED_TUI_LINKPROBE_OBJECT_LIBRARIES (vendor/CMakeLists.txt).")
        endif()
    endforeach()

    if(inProbe EQUAL 0)
        list(APPEND problems
            "no vendored translation unit was found in the probe's object libraries (${arg_PROBE_LIBRARIES}). "
            "The walk has stopped reading targets, and an empty root accepts every tree.")
    endif()

    set(${problemsVar} "${problems}" PARENT_SCOPE)
    set(${inProbeVar} ${inProbe} PARENT_SCOPE)
    set(${inExecutablesVar} ${inExecutables} PARENT_SCOPE)
endfunction()
