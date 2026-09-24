# SPDX-License-Identifier: Apache-2.0
# ErrorPopups.cmake - no executable a test runs raises a modal error dialog on Windows
# (#1389).
#
# A failed `assert()`, a `_CrtDbgReport` or an iterator-debug check in a Debug CRT opens
# an "Abort/Retry/Ignore" box, and a fault opens Windows Error Reporting's. Neither fails
# a test: the process waits for a click, so ctest reports a TIMEOUT on a developer's
# desktop and a runner reports a slow job. An `endo` test hung 58 minutes that way.
#
# `src/tests/WindowsErrorPopups.hpp` already knew how to switch both off; what it could
# not do was reach a `main` it did not own. Four test executables link
# `Catch2::Catch2WithMain`, so the suppression was in every `main` this project wrote and
# in none of theirs. So this attaches `src/tests/ErrorPopupsAtStartup.cpp` to every
# executable by walking the build system, the argument cmake/Utf8CodePage.cmake makes for
# the UTF-8 manifest: a target that missed a line still builds and still runs, and is
# wrong only on the run that hangs.
#
# ## Two policies, and the environment variable that joins them
#
# A TEST executable suppresses always. A PRODUCT binary -- the target named for its own
# `src/apps/<name>` directory, or one carrying `FASTCACHED_ERROR_POPUPS_POLICY on-request`
# -- suppresses only when `FASTCACHED_ERROR_DIALOG_VARIABLE` is present in its environment.
# A developer running a Debug `fastcached` by hand keeps the dialog and the chance to attach
# a debugger at the assert; a product binary an e2e fixture spawns does not, because every
# registered test carries the variable (`_fastcached_mark_tests_for_error_dialogs` below, and
# `FASTCACHED_ERROR_DIALOG_ENVIRONMENT` on each `catch_discover_tests`) and a child inherits
# it. This is the mechanism `endo` proved; a Debug-only policy was the first draft here and
# took the debugger away from exactly the build that exists to use one.
#
# What proves all of it is not this file's claim but the generated files:
# `ctest -R error-popup-coverage` reads the link line of every executable a test launches,
# and the environment of every registered test.
#
# An executable that must NOT get the object states a reason on itself --
# `set_target_properties(<t> PROPERTIES FASTCACHED_ERROR_POPUPS "<why>")` -- which the walk
# skips and the check prints. Nothing does today.

include(ProjectTargets)

# The variable's NAME, spelled once: the walk hands it to the startup object as a compile
# definition, the registrations set it, and the check and the product canary's gate are
# told it on their command lines.
set(FASTCACHED_ERROR_DIALOG_VARIABLE "FASTCACHED_SUPPRESS_ERROR_DIALOGS")
# The value an `ENVIRONMENT_MODIFICATION` takes to set it. For `catch_discover_tests`,
# whose generated registrations no walk at configure time can reach.
set(FASTCACHED_ERROR_DIALOG_ENVIRONMENT "${FASTCACHED_ERROR_DIALOG_VARIABLE}=set:1")

# Attach the suppression to every executable under src/.
#
# Call once, after every add_subdirectory() -- the walk reads the build system as it
# stands, so a target added later is a target left out. That is not left to this
# comment: the manifest the check reads is NOT taken by this walk.
function(fastcached_suppress_error_popups)
    fastcached_collect_executables("${CMAKE_SOURCE_DIR}" executables src)
    set(source "${CMAKE_SOURCE_DIR}/src/tests/ErrorPopupsAtStartup.cpp")

    set(attached 0)
    foreach(executable IN LISTS executables)
        get_target_property(reason ${executable} FASTCACHED_ERROR_POPUPS)
        if(reason)
            continue()
        endif()
        target_sources(${executable} PRIVATE "${source}")
        math(EXPR attached "${attached} + 1")
        get_target_property(directory ${executable} SOURCE_DIR)
        get_target_property(policy ${executable} FASTCACHED_ERROR_POPUPS_POLICY)
        if(directory STREQUAL "${CMAKE_SOURCE_DIR}/src/apps/${executable}" OR policy STREQUAL "on-request")
            target_compile_definitions(${executable} PRIVATE
                "FASTCACHED_ERROR_POPUPS_ON_REQUEST=\"${FASTCACHED_ERROR_DIALOG_VARIABLE}\"")
        endif()
    endforeach()
    list(LENGTH executables count)
    message(STATUS "[ErrorPopups] suppression attached to ${attached} of ${count} walked executable(s); the rest state a reason")

    # The census runs at the END of the top-level directory, over the WHOLE source tree,
    # rather than here over the walk's roots. Taken here, it would describe exactly what
    # the walk reached: a root left out of the walk and a target defined after this call
    # would both be absent from the manifest and therefore unchecked -- measured, dropping
    # `vendor` from the roots above left the check green with the vendored TUI's test binary
    # unsuppressed, while there was one (#1596 removed that copy, and with it the root).
    #
    # No argument is passed: a deferred call's arguments are evaluated when it RUNS, in the
    # top-level scope, where this function's `source` does not exist.
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _fastcached_write_error_popup_manifest)
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _fastcached_mark_tests_for_error_dialogs "${CMAKE_SOURCE_DIR}")
endfunction()

# Set the variable on every test `add_test` registered in DIR and below.
#
# Deferred to the end of the top-level directory, so a registration added after the call
# is covered too. `catch_discover_tests` registrations do not exist yet at configure time,
# which is why each of those calls names `FASTCACHED_ERROR_DIALOG_ENVIRONMENT` itself, and
# why the check reads the generated files rather than trusting either half.
# @param dir a directory of the source tree
function(_fastcached_mark_tests_for_error_dialogs dir)
    get_property(tests DIRECTORY "${dir}" PROPERTY TESTS)
    foreach(test IN LISTS tests)
        set_property(TEST "${test}" DIRECTORY "${dir}" APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "${FASTCACHED_ERROR_DIALOG_ENVIRONMENT}")
    endforeach()
    get_property(subdirectories DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        string(FIND "${subdirectory}" "${CMAKE_SOURCE_DIR}/" position)
        if(position EQUAL 0)
            _fastcached_mark_tests_for_error_dialogs("${subdirectory}")
        endif()
    endforeach()
endfunction()

# Write the manifest `scripts/check-error-popups.cmake` reads: every executable in the
# source tree, whether the walk reached it or not.
function(_fastcached_write_error_popup_manifest)
    set(source "${CMAKE_SOURCE_DIR}/src/tests/ErrorPopupsAtStartup.cpp")
    fastcached_collect_executables("${CMAKE_SOURCE_DIR}" executables "*")
    set(manifest "")
    foreach(executable IN LISTS executables)
        get_target_property(reason ${executable} FASTCACHED_ERROR_POPUPS)
        get_target_property(sources ${executable} SOURCES)
        get_target_property(definitions ${executable} COMPILE_DEFINITIONS)
        if(reason)
            set(policy "exempt")
        elseif("${source}" IN_LIST sources)
            set(reason "")
            set(policy "always")
            foreach(definition IN LISTS definitions)
                if(definition MATCHES "^FASTCACHED_ERROR_POPUPS_ON_REQUEST=")
                    set(policy "on-request")
                endif()
            endforeach()
        else()
            set(reason "")
            set(policy "not-attached")
        endif()
        # One row per executable: name, the file ctest would launch, policy, reason.
        # `|`-separated, and the check refuses a row it cannot split into exactly four.
        string(APPEND manifest "${executable}|$<TARGET_FILE:${executable}>|${policy}|${reason}\n")
    endforeach()

    # Per configuration, because a multi-config generator puts each one's binaries at a
    # different path, and ctest names the configuration it is running.
    file(GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/error-popups-$<CONFIG>.manifest"
        CONTENT "${manifest}"
    )
endfunction()
