# SPDX-License-Identifier: Apache-2.0
# ProjectTargets.cmake - what this project actually built, derived from the build
# system rather than listed by hand.
#
# Three callers want the same answer and for the same reason. The `coverage`
# target hands llvm-cov every binary the suite runs, cmake/Utf8CodePage.cmake
# attaches the UTF-8 manifest to every binary that parses a command line, and
# cmake/ErrorPopups.cmake attaches the Windows error-popup suppression. All
# fail SILENTLY when the list is wrong -- a report of one fewer file still renders,
# and an executable that missed the manifest still runs -- so neither may be
# maintained by hand, and neither may keep a private copy of the walk that would
# drift from the other.

# Collect the executable targets defined under src/, or under the source-relative
# roots named after OUT_VAR.
#
# The roots are a parameter because the error-popup suppression must reach
# `vendor/`, where upstream's own test binary is defined and run by ctest, while
# coverage and the code page have no reason to (#1389). The root `*` names the whole
# source tree, which is what a check of that walk must enumerate: a census taken
# through the walk's own filter cannot see what the filter left out.
#
# A new app under src/apps/ is a new row in that directory's app table and nothing
# else, so nothing here needs editing when one appears.
#
# The subdirectory walk is filtered to src/ because CPM adds each dependency's
# source tree as a subdirectory too, and those carry executables (Catch2's own
# self-tests, for one) that neither caller wants. Compared with string(FIND)
# rather than a regex: a checkout path is arbitrary text, and this repository
# routinely has worktrees with a `+` in the name.
#
# Reads the build system as it stands, so every caller must run AFTER the
# add_subdirectory() calls that define the targets.
function(fastcached_collect_executables DIR OUT_VAR)
    set(roots ${ARGN})
    if(NOT roots)
        set(roots src)
    endif()
    set(found "")

    get_property(targets DIRECTORY "${DIR}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "EXECUTABLE")
            list(APPEND found ${target})
        endif()
    endforeach()

    get_property(subdirectories DIRECTORY "${DIR}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        set(inside FALSE)
        foreach(root IN LISTS roots)
            set(prefix "${CMAKE_SOURCE_DIR}/${root}")
            if(root STREQUAL "*")
                set(prefix "${CMAKE_SOURCE_DIR}/")
            endif()
            string(FIND "${subdirectory}" "${prefix}" position)
            if(position EQUAL 0)
                set(inside TRUE)
            endif()
        endforeach()
        if(inside)
            fastcached_collect_executables("${subdirectory}" nested ${roots})
            list(APPEND found ${nested})
        endif()
    endforeach()

    set(${OUT_VAR} "${found}" PARENT_SCOPE)
endfunction()
