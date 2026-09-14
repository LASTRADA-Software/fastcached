# SPDX-License-Identifier: Apache-2.0
#
# Every executable a test launches carries the Windows error-popup suppression, or says why
# it does not (#1389).
#
# ## What this reads, and why not CMakeLists.txt
#
# `cmake/ErrorPopups.cmake` attaches `src/tests/ErrorPopupsAtStartup.cpp` to every
# executable by walking the build system. That is a CLAIM about what was attached; the
# artefact is the generated link line. A walk filtered to the wrong roots, or called before
# the `add_subdirectory` that defines a target, leaves an executable that builds, runs, and
# hangs on the one run that asserts -- and every CMakeLists would still read correctly. So
# the set is derived from three generated files, and none of it is listed here:
#
#   * `error-popups-<config>.manifest`: every executable in the source tree, the file ctest
#     would launch, its policy, and a stated reason when it is exempt. Taken at the END of
#     configure over the whole tree, never through the walk's own roots, or a root the walk
#     left out would be absent from the census as well -- measured, and green;
#   * every `CTestTestfile.cmake`: an executable is LAUNCHED when its path appears there --
#     as a test's command or as an argument a script is handed, which is how the e2e
#     fixtures reach the product binaries -- or when a `catch_discover_tests` include file
#     named after it does;
#   * `build.ninja`: a launched executable is COVERED when its link edge for this
#     configuration names the attached object.
#
# And a second property, read from the same registrations and the `_tests.cmake` files
# `catch_discover_tests` generates: EVERY registered test sets VARIABLE. A product binary
# suppresses only when that variable is in its environment, so a test that does not set it
# is a test whose spawned `fastcached` still waits on the dialog -- and the object being
# linked says nothing about that.
#
# ## Outcomes
#
# A launched executable that is neither covered nor exempt is REFUSED, by name. So is
# every way of not being able to ask: no manifest, an empty one, a reason this reader
# cannot carry, and an anchor (`FastCacheTest`) that is not launched-and-covered, since a
# derivation that proves nothing about the one executable certain to be there proves
# nothing at all. A generator that writes no `build.ninja` is a SKIP, naming it -- the
# presets all use Ninja, and a reader claiming to parse a format it does not is worse.
#
# A path is matched with a BOUNDARY after it, never as a bare substring: on POSIX
# `target/fastcache-cc` is a prefix of `target/fastcache-cc-tests`.
#
# Usage: cmake -DBUILD_DIR=<dir> -DCONFIG=<config> -DANCHOR=<target> -DVARIABLE=<name>
#          -P check-error-popups.cmake

cmake_minimum_required(VERSION 3.28)

foreach(required BUILD_DIR CONFIG ANCHOR VARIABLE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "error-popups: ${required} must be set")
    endif()
endforeach()

set(object "ErrorPopupsAtStartup.cpp.o")
set(manifestFile "${BUILD_DIR}/error-popups-${CONFIG}.manifest")
set(ninjaFile "${BUILD_DIR}/build.ninja")
set(problems 0)

# Report one refusal on the output the registration reads, and count it. A FUNCTION, not a
# macro: a macro substitutes its arguments textually and CMake re-parses them.
# @param ARGV the sentence, in pieces that are joined with nothing between them
function(fc_refuse)
    list(JOIN ARGV "" text)
    message("CMake Error: error-popups: ${text}")
    math(EXPR count "${problems} + 1")
    set(problems ${count} PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${manifestFile}")
    message(FATAL_ERROR
        "error-popups: no manifest at ${manifestFile}. It is written at generate time by "
        "cmake/ErrorPopups.cmake, so this build was configured without that walk, or for "
        "another configuration than '${CONFIG}' -- nothing here can say which executables exist.")
endif()
if(NOT EXISTS "${ninjaFile}")
    message("SKIP: error-popups: ${BUILD_DIR} has no build.ninja, so its link lines are in a "
            "format this check does not read and it cannot say what was linked")
    return()
endif()

# Every registration file ctest reads, concatenated. FOLLOWED from the top-level
# `CTestTestfile.cmake` through its `subdirs()` exactly as ctest follows them, never globbed:
# a glob of the build directory also finds files no ctest run reads -- this check's own
# self-test stages whole trees under it, and the first version refused a clean build over
# those. Read whole and searched with FIND and REGEX, never split into a list: these files
# are full of `[==[` brackets.
set(pending "${BUILD_DIR}/CTestTestfile.cmake")
set(ctestFiles "")
set(registrations "")
while(pending)
    list(POP_FRONT pending current)
    if(NOT EXISTS "${current}")
        # ctest skips a listed subdirectory with no testfile, and so does this.
        continue()
    endif()
    list(APPEND ctestFiles "${current}")
    file(READ "${current}" content)
    string(APPEND registrations "${content}\n")
    get_filename_component(currentDir "${current}" DIRECTORY)
    string(REGEX MATCHALL "subdirs\\(\"[^\"]+\"\\)" subdirCalls "${content}")
    foreach(call IN LISTS subdirCalls)
        string(REGEX REPLACE "^subdirs\\(\"(.*)\"\\)$" "\\1" subdir "${call}")
        if(NOT IS_ABSOLUTE "${subdir}")
            set(subdir "${currentDir}/${subdir}")
        endif()
        list(APPEND pending "${subdir}/CTestTestfile.cmake")
    endforeach()
endwhile()
if(NOT ctestFiles)
    fc_refuse("${BUILD_DIR}/CTestTestfile.cmake does not exist, so nothing ctest would run can be read")
endif()
string(REGEX MATCHALL "[^/\"]+-[0-9a-f]+_include\\.cmake" catchIncludes "${registrations}")

file(STRINGS "${ninjaFile}" linkEdges REGEX "_EXECUTABLE_LINKER__")

# Whether PATH occurs in TEXT followed by a boundary, rather than as a prefix of a longer
# name. Sets OUT to TRUE or FALSE.
function(fc_mentions_path text path out)
    string(LENGTH "${path}" pathLength)
    set(rest "${text}")
    while(TRUE)
        string(FIND "${rest}" "${path}" at)
        if(at EQUAL -1)
            set(${out} FALSE PARENT_SCOPE)
            return()
        endif()
        math(EXPR after "${at} + ${pathLength}")
        string(SUBSTRING "${rest}" ${after} 1 next)
        if(next STREQUAL "" OR next MATCHES "[\" \t\r\n)]" OR next STREQUAL "]")
            set(${out} TRUE PARENT_SCOPE)
            return()
        endif()
        string(SUBSTRING "${rest}" ${after} -1 rest)
    endwhile()
endfunction()

file(STRINGS "${manifestFile}" rows)
set(total 0)
set(launched 0)
set(covered 0)
set(exempt 0)
set(anchorProven FALSE)
foreach(row IN LISTS rows)
    if(row STREQUAL "")
        continue()
    endif()
    math(EXPR total "${total} + 1")
    if(row MATCHES "[;\\[]")
        fc_refuse("a manifest row carries a semicolon or an opening bracket, which this reader cannot split safely: ${row}")
        continue()
    endif()
    string(REPLACE "|" ";" fields "${row}")
    list(LENGTH fields fieldCount)
    if(NOT fieldCount EQUAL 4)
        fc_refuse("a manifest row does not have four `|` fields, so a reason carries a `|` or the walk changed shape: ${row}")
        continue()
    endif()
    list(GET fields 0 name)
    list(GET fields 1 file)
    list(GET fields 2 policy)
    list(GET fields 3 reason)

    fc_mentions_path("${registrations}" "${file}" isLaunched)
    foreach(include IN LISTS catchIncludes)
        if(include MATCHES "^(.+)-[0-9a-f]+_include\\.cmake$" AND CMAKE_MATCH_1 STREQUAL name)
            set(isLaunched TRUE)
        endif()
    endforeach()
    if(NOT isLaunched)
        continue()
    endif()
    math(EXPR launched "${launched} + 1")

    set(isLinked FALSE)
    foreach(edge IN LISTS linkEdges)
        string(FIND "${edge}" "_EXECUTABLE_LINKER__${name}_${CONFIG} " atRule)
        string(FIND "${edge}" "${object}" atObject)
        if(NOT atRule EQUAL -1 AND NOT atObject EQUAL -1)
            set(isLinked TRUE)
        endif()
    endforeach()

    if(isLinked)
        math(EXPR covered "${covered} + 1")
        message("ok: ${name} (${policy})")
        if(name STREQUAL ANCHOR)
            set(anchorProven TRUE)
        endif()
    elseif(policy STREQUAL "exempt" AND NOT reason STREQUAL "")
        math(EXPR exempt "${exempt} + 1")
        message("exempt: ${name} -- ${reason}")
    else()
        fc_refuse("${name} is launched by a test and its '${CONFIG}' link line carries no "
                  "${object}, so on Windows an assert in it opens a dialog and hangs the run "
                  "instead of failing it. cmake/ErrorPopups.cmake attaches that object to every "
                  "executable it walks: a target defined after that call, or outside the roots it "
                  "walks, is missed. Move the target or the call so the walk reaches it; only if "
                  "it must never be suppressed, state why with "
                  "set_target_properties(${name} PROPERTIES FASTCACHED_ERROR_POPUPS \"<reason>\").")
    endif()
endforeach()

# ---- every registered test sets the variable --------------------------------------------

# The registration files: every CTestTestfile.cmake, and the `_tests.cmake` each
# `catch_discover_tests` include file names. An include whose tests file was never
# generated is a binary that was not built, and its tests cannot be read -- refused, not
# skipped, since a skip here is exactly the green a missing registration would produce.
set(registrationFiles ${ctestFiles})
string(REGEX MATCHALL "include\\(\"([^\"]+-[0-9a-f]+_include\\.cmake)\"\\)" includeCalls "${registrations}")
foreach(call IN LISTS includeCalls)
    string(REGEX REPLACE "^include\\(\"(.*)\"\\)$" "\\1" includeFile "${call}")
    if(NOT EXISTS "${includeFile}")
        fc_refuse("${includeFile} is named by a registration and does not exist, so the tests it adds cannot be read")
        continue()
    endif()
    file(READ "${includeFile}" includeText)
    if(NOT includeText MATCHES "include\\(\"([^\"]+_tests\\.cmake)\"\\)")
        fc_refuse("${includeFile} names no _tests.cmake, so this reader no longer understands catch_discover_tests' output")
        continue()
    endif()
    set(testsFile "${CMAKE_MATCH_1}")
    if(NOT EXISTS "${testsFile}")
        fc_refuse("${testsFile} was never generated -- its binary is not built -- so whether its tests set ${VARIABLE} cannot be read. Build it, then run this check")
        continue()
    endif()
    list(APPEND registrationFiles "${testsFile}")
endforeach()

# Placeholders for the three characters that make a CMake list unsafe, so each registration
# file can be split into LINES: its brackets are CMake bracket arguments, and a
# `;` inside a quoted property value would split one line into several.
string(ASCII 1 lb)
string(ASCII 2 rb)
string(ASCII 3 sc)
set(testCount 0)
set(unset "")
set(unsetCount 0)
foreach(registrationFile IN LISTS registrationFiles)
    file(READ "${registrationFile}" text)
    string(REPLACE ";" "${sc}" text "${text}")
    string(REPLACE "[" "${lb}" text "${text}")
    string(REPLACE "]" "${rb}" text "${text}")
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    set(added "")
    set(marked "")
    foreach(line IN LISTS lines)
        string(STRIP "${line}" line)
        if(NOT line MATCHES "^(add_test|set_tests_properties)\\( ?(.*)$")
            continue()
        endif()
        set(kind "${CMAKE_MATCH_1}")
        set(rest "${CMAKE_MATCH_2}")
        # The name is the first argument: a bracket argument, whose closing token repeats
        # the opening `=` count and whose body may hold brackets of its own -- measured,
        # `[==[RESP: XRANGE returns entries as [id, [field, value]] arrays]==]` -- or a bare
        # token, which is how catch_discover_tests spells a name with no space in it.
        set(name "")
        if(rest MATCHES "^${lb}(=*)${lb}")
            set(opening "${lb}${CMAKE_MATCH_1}${lb}")
            set(closing "${rb}${CMAKE_MATCH_1}${rb}")
            string(LENGTH "${opening}" openingLength)
            string(SUBSTRING "${rest}" ${openingLength} -1 body)
            string(FIND "${body}" "${closing}" at)
            if(NOT at EQUAL -1)
                string(SUBSTRING "${body}" 0 ${at} name)
            endif()
        elseif(rest MATCHES "^([^ ${lb}${rb}\\)]+)")
            set(name "${CMAKE_MATCH_1}")
        endif()
        if(name STREQUAL "")
            string(REPLACE "${lb}" "[" shown "${line}")
            string(REPLACE "${rb}" "]" shown "${shown}")
            fc_refuse("${registrationFile}: a test registration this reader cannot name, so whether it sets ${VARIABLE} is unknown: ${shown}")
        elseif(kind STREQUAL "add_test")
            list(APPEND added "${name}")
        else()
            string(FIND "${line}" "${VARIABLE}=set:" at)
            if(NOT at EQUAL -1)
                list(APPEND marked "${name}")
            endif()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES added)
    foreach(name IN LISTS added)
        math(EXPR testCount "${testCount} + 1")
        if(NOT name IN_LIST marked)
            math(EXPR unsetCount "${unsetCount} + 1")
            if(unsetCount LESS_EQUAL 10)
                string(REPLACE "${lb}" "[" shownName "${name}")
                string(REPLACE "${rb}" "]" shownName "${shownName}")
                string(APPEND unset "\n    ${shownName}  (${registrationFile})")
            endif()
        endif()
    endforeach()
endforeach()

if(testCount EQUAL 0)
    fc_refuse("no registered test was found under ${BUILD_DIR}, so no test was shown to set ${VARIABLE} and a pass would be vacuous")
elseif(unsetCount GREATER 0)
    fc_refuse("${unsetCount} of ${testCount} registered test(s) do not set ${VARIABLE}, so a product binary "
              "any of them spawns still opens an assert dialog. cmake/ErrorPopups.cmake sets it on every "
              "add_test by a deferred walk, and a catch_discover_tests call must name "
              "ENVIRONMENT_MODIFICATION \"\${FASTCACHED_ERROR_DIALOG_ENVIRONMENT}\" itself. The first of them:${unset}")
endif()
message("error-popups: ${testCount} registered test(s) read, ${unsetCount} without ${VARIABLE}")

if(total EQUAL 0)
    fc_refuse("the manifest ${manifestFile} names no executable, so there was nothing to check "
              "and a pass would be vacuous")
elseif(NOT anchorProven)
    fc_refuse("the anchor '${ANCHOR}' was not found launched AND covered, so the derivation "
              "cannot show it reaches even the one executable certain to be here")
endif()

message("error-popups: ${launched} of ${total} executable(s) are launched by a test; "
        "${covered} carry the suppression, ${exempt} state a reason")
if(problems GREATER 0)
    message(FATAL_ERROR "error-popups: ${problems} problem(s)")
endif()
