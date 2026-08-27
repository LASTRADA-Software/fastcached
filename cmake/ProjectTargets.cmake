# SPDX-License-Identifier: Apache-2.0
# ProjectTargets.cmake - what this project actually built, derived from the build
# system rather than listed by hand.
#
# Two callers want the same answer today and for the same reason. The `coverage`
# target hands llvm-cov every binary the suite runs, and cmake/Utf8CodePage.cmake
# attaches the UTF-8 manifest to every binary that parses a command line. Both
# fail SILENTLY when the list is wrong -- a report of one fewer file still renders,
# and an executable that missed the manifest still runs -- so neither may be
# maintained by hand, and neither may keep a private copy of the walk that would
# drift from the other.

# Collect the executable targets defined under src/.
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
        string(FIND "${subdirectory}" "${CMAKE_SOURCE_DIR}/src" position)
        if(position EQUAL 0)
            fastcached_collect_executables("${subdirectory}" nested)
            list(APPEND found ${nested})
        endif()
    endforeach()

    set(${OUT_VAR} "${found}" PARENT_SCOPE)
endfunction()
