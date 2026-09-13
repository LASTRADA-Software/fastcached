# SPDX-License-Identifier: Apache-2.0
#
# Drives `check-error-popups.cmake` against staged build trees, in both directions (#1389).
#
# Each tree holds the files the check reads, in the shapes CMake generates: a manifest, a
# `CTestTestfile.cmake` (one nested, as a real build nests them) with bracket-named
# registrations, a `catch_discover_tests` include file and the `_tests.cmake` it names, and
# the `build.ninja` link edges. A refusal asserts WHICH refusal, by a phrase only that
# refusal carries: every refusal prints `CMake Error`, so asserting that alone would pass
# with the arms exchanged.
#
# Usage: cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_SCRATCH_DIR=<dir> -P <this>

cmake_minimum_required(VERSION 3.28)

foreach(required FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-error-popups.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

file(REMOVE_RECURSE "${FASTCACHED_SCRATCH_DIR}")
set(failures "")
set(cases 0)
set(variable "FC_SELFTEST_SUPPRESS")

# One CTestTestfile registration, spelled as CMake writes it.
# @param outVar receives the two lines
# @param name the test name
# @param command the rest of the add_test line
# @param marked TRUE when the test sets the variable
function(fc_registration outVar name command marked)
    set(text "add_test([=[${name}]=] ${command})\n")
    if(marked)
        string(APPEND text "set_tests_properties([=[${name}]=] PROPERTIES ENVIRONMENT_MODIFICATION \"A=unset:;${variable}=set:1\" _BACKTRACE_TRIPLES \"x;1;y\")\n")
    else()
        string(APPEND text "set_tests_properties([=[${name}]=] PROPERTIES TIMEOUT \"60\")\n")
    endif()
    set(${outVar} "${text}" PARENT_SCOPE)
endfunction()

# Stage one build tree.
# @param name the tree's directory name under the scratch root
# @param manifest the manifest body, one `name|file|policy|reason` row per line
# @param registrations the top-level CTestTestfile.cmake body; `@TREE@` becomes the tree path
# @param nested a nested CTestTestfile.cmake body
# @param catchTests the body of the `_tests.cmake` a catch include names, or `<none>` for no file
# @param edges the build.ninja body, or `<none>` for no file
# @param outVar receives the tree's path
function(fc_stage name manifest registrations nested catchTests edges outVar)
    set(tree "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(MAKE_DIRECTORY "${tree}/src/apps/x")
    file(WRITE "${tree}/error-popups-Debug.manifest" "${manifest}")
    string(REPLACE "@TREE@" "${tree}" registrations "${registrations}")
    string(REPLACE "@TREE@" "${tree}" nested "${nested}")
    # Reached the way ctest reaches it, through `subdirs()`; a testfile nothing lists is not read.
    file(WRITE "${tree}/CTestTestfile.cmake" "${registrations}subdirs(\"src/apps/x\")\n")
    file(WRITE "${tree}/src/apps/x/CTestTestfile.cmake" "${nested}")
    file(WRITE "${tree}/src/apps/x/cli-tests-b12d07c_include.cmake"
        "if(EXISTS \"${tree}/src/apps/x/cli-tests-b12d07c_tests.cmake\")\n  include(\"${tree}/src/apps/x/cli-tests-b12d07c_tests.cmake\")\nelse()\n  add_test(cli-tests_NOT_BUILT-b12d07c cli-tests_NOT_BUILT-b12d07c)\nendif()\n")
    if(NOT catchTests STREQUAL "<none>")
        file(WRITE "${tree}/src/apps/x/cli-tests-b12d07c_tests.cmake" "${catchTests}")
    endif()
    if(NOT edges STREQUAL "<none>")
        file(WRITE "${tree}/build.ninja" "${edges}")
    endif()
    set(${outVar} "${tree}" PARENT_SCOPE)
endfunction()

# Run the check against a tree and record whether it matched expectations.
# @param label what the case establishes
# @param tree the staged tree
# @param want `pass`, `refuse` or `skip`
# @param phrase text the output must carry
function(fc_case label tree want phrase)
    math(EXPR count "${cases} + 1")
    set(cases ${count} PARENT_SCOPE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${tree}" -DCONFIG=Debug -DANCHOR=FastCacheTest
                "-DVARIABLE=${variable}" -P "${check}"
        OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE status)
    set(output "${out}${err}")
    string(REPLACE "\n" " " flat "${output}")
    set(got "pass")
    if(flat MATCHES "CMake Error|CMake Warning")
        set(got "refuse")
    elseif(flat MATCHES "SKIP: ")
        set(got "skip")
    endif()
    string(FIND "${flat}" "${phrase}" at)
    if(NOT got STREQUAL want)
        set(failures "${failures}\n  FAIL ${label}: expected ${want}, got ${got}:\n${output}" PARENT_SCOPE)
    elseif(at EQUAL -1)
        set(failures "${failures}\n  FAIL ${label}: ${want} as expected but without '${phrase}':\n${output}" PARENT_SCOPE)
    else()
        message("  ok   ${label}")
    endif()
endfunction()

set(t "D:/b/target")
set(anchorRow "FastCacheTest|${t}/FastCacheTest.exe|always|\n")
fc_registration(anchorTest "a case" "${t}/FastCacheTest.exe [=[a case]=]" TRUE)
set(anchorEdge "build target\\FastCacheTest.exe: CXX_EXECUTABLE_LINKER__FastCacheTest_Debug src\\tests\\CMakeFiles\\FastCacheTest.dir\\a.cpp.obj src\\tests\\CMakeFiles\\FastCacheTest.dir\\ErrorPopupsAtStartup.cpp.obj\n")
set(catchInclude "include(\"@TREE@/src/apps/x/cli-tests-b12d07c_include.cmake\")\n")
set(catchTestsMarked "add_test( [==[the table is well formed]==] ${t}/cli-tests.exe [==[the table is well formed]==]  )\nset_tests_properties( [==[the table is well formed]==] PROPERTIES WORKING_DIRECTORY D:/b SKIP_RETURN_CODE 4 ENVIRONMENT_MODIFICATION ${variable}=set:1)\n")
set(catchEdge "build target\\cli-tests.exe: CXX_EXECUTABLE_LINKER__cli-tests_Debug x.obj ErrorPopupsAtStartup.cpp.obj\n")

# ---- the passing direction --------------------------------------------------------------

fc_registration(ccE2e "cc-e2e" "\"bash\" \"e2e.sh\" \"--launcher\" \"${t}/fastcache-cc\"" TRUE)
fc_registration(canary "canary" "\"cmake\" \"-DCANARY=${t}/canary.exe\" \"-P\" \"gate.cmake\"" TRUE)
fc_stage(clean
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\nfastcache-cc|${t}/fastcache-cc|on-request|\ncanary|${t}/canary.exe|always|\nunlaunched|${t}/unlaunched.exe|always|\n"
    "${anchorTest}${ccE2e}${canary}"
    "${catchInclude}"
    "${catchTestsMarked}"
    "${anchorEdge}${catchEdge}build target/fastcache-cc: CXX_EXECUTABLE_LINKER__fastcache-cc_Debug y.o ErrorPopupsAtStartup.cpp.o\nbuild target\\canary.exe: CXX_EXECUTABLE_LINKER__canary_Debug ErrorPopupsAtStartup.cpp.obj\nbuild target\\unlaunched.exe: CXX_EXECUTABLE_LINKER__unlaunched_Debug z.obj\n"
    tree)
fc_case("a command, a catch_discover_tests include and a script argument all count as launched and covered" "${tree}" pass "4 of 5 executable(s) are launched by a test; 4 carry the suppression")
fc_case("every registered test, catch-discovered ones included, is read and sets the variable" "${tree}" pass "4 registered test(s) read, 0 without ${variable}")

fc_registration(odd "odd" "${t}/odd.exe" TRUE)
fc_stage(exempt
    "${anchorRow}odd|${t}/odd.exe|exempt|it is a probe that is linked and never run\n"
    "${anchorTest}${odd}" "" "<none>"
    "${anchorEdge}build target\\odd.exe: CXX_EXECUTABLE_LINKER__odd_Debug odd.obj\n"
    tree)
fc_case("a launched executable stating a reason passes and prints the reason" "${tree}" pass "exempt: odd -- it is a probe that is linked and never run")

# POSIX spells no `.exe`, so the launcher's path is a PREFIX of its test binary's. The
# uncovered launcher is not launched; its test binary is.
fc_registration(prefixTest "t" "/b/target/fastcache-cc-tests" TRUE)
fc_stage(prefix
    "${anchorRow}fastcache-cc|/b/target/fastcache-cc|on-request|\nfastcache-cc-tests|/b/target/fastcache-cc-tests|always|\n"
    "${anchorTest}${prefixTest}" "" "<none>"
    "${anchorEdge}build target/fastcache-cc: CXX_EXECUTABLE_LINKER__fastcache-cc_Debug a.o\nbuild target/fastcache-cc-tests: CXX_EXECUTABLE_LINKER__fastcache-cc-tests_Debug ErrorPopupsAtStartup.cpp.o\n"
    tree)
fc_case("a path that is only a PREFIX of a launched one is not launched" "${tree}" pass "2 of 3 executable(s) are launched")

# ---- the refusing direction: coverage ---------------------------------------------------

fc_registration(bare "bare" "${t}/bare.exe" TRUE)
fc_stage(command
    "${anchorRow}bare|${t}/bare.exe|always|\n"
    "${anchorTest}${bare}" "" "<none>"
    "${anchorEdge}build target\\bare.exe: CXX_EXECUTABLE_LINKER__bare_Debug bare.obj\n"
    tree)
fc_case("an uncovered test COMMAND is refused by name" "${tree}" refuse "bare is launched by a test and its 'Debug' link line carries no")

fc_registration(e2e "e2e" "\"bash\" \"e2e.sh\" \"--fastcached\" \"${t}/fastcached.exe\"" TRUE)
fc_stage(argument
    "${anchorRow}fastcached|${t}/fastcached.exe|on-request|\n"
    "${anchorTest}${e2e}" "" "<none>"
    "${anchorEdge}build target\\fastcached.exe: CXX_EXECUTABLE_LINKER__fastcached_Debug main.obj\n"
    tree)
fc_case("an uncovered binary handed to a script as an ARGUMENT is refused" "${tree}" refuse "fastcached is launched by a test")

fc_stage(catch
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" "" "${catchTestsMarked}"
    "${anchorEdge}build target\\cli-tests.exe: CXX_EXECUTABLE_LINKER__cli-tests_Debug t.obj\n"
    tree)
fc_case("an uncovered catch_discover_tests binary is refused, found through its include file" "${tree}" refuse "cli-tests is launched by a test")

fc_registration(rel "rel" "${t}/rel.exe" TRUE)
fc_stage(otherconfig
    "${anchorRow}rel|${t}/rel.exe|always|\n"
    "${anchorTest}${rel}" "" "<none>"
    "${anchorEdge}build target\\rel.exe: CXX_EXECUTABLE_LINKER__rel_Release ErrorPopupsAtStartup.cpp.obj\n"
    tree)
fc_case("an object on ANOTHER configuration's link line does not cover this one" "${tree}" refuse "rel is launched by a test and its 'Debug' link line")

fc_registration(blank "blank" "${t}/blank.exe" TRUE)
fc_stage(exemptnoreason
    "${anchorRow}blank|${t}/blank.exe|exempt|\n"
    "${anchorTest}${blank}" "" "<none>"
    "${anchorEdge}build target\\blank.exe: CXX_EXECUTABLE_LINKER__blank_Debug b.obj\n"
    tree)
fc_case("an exemption with an EMPTY reason is not an exemption" "${tree}" refuse "blank is launched by a test")

fc_stage(pipe
    "${anchorRow}piped|${t}/piped.exe|exempt|a reason | with a pipe\n"
    "${anchorTest}" "" "<none>" "${anchorEdge}" tree)
fc_case("a reason carrying `|` is refused rather than misread" "${tree}" refuse "does not have four `|` fields")

fc_stage(empty "" "${anchorTest}" "" "<none>" "${anchorEdge}" tree)
fc_case("an empty manifest is refused as vacuous" "${tree}" refuse "names no executable")

fc_registration(other "other" "D:/b/target/other.exe" TRUE)
fc_stage(noanchor "${anchorRow}" "${other}" "" "<none>" "${anchorEdge}" tree)
fc_case("an anchor that is not launched is refused" "${tree}" refuse "the anchor 'FastCacheTest' was not found launched AND covered")

fc_stage(nomanifest "" "${anchorTest}" "" "<none>" "${anchorEdge}" tree)
file(REMOVE "${tree}/error-popups-Debug.manifest")
fc_case("no manifest at all is refused, naming the walk" "${tree}" refuse "configured without that walk")

# ---- the refusing direction: the variable -----------------------------------------------

fc_registration(unmarked "an unmarked script test" "\"bash\" \"e2e.sh\"" FALSE)
fc_stage(unmarkedaddtest "${anchorRow}" "${anchorTest}" "${unmarked}" "<none>" "${anchorEdge}" tree)
fc_case("an add_test that does not set the variable is refused by name" "${tree}" refuse "1 of 2 registered test(s) do not set ${variable}")
fc_case("... and the refusal names the test" "${tree}" refuse "an unmarked script test")

fc_stage(unmarkedcatch
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" ""
    "add_test( [==[a discovered case]==] ${t}/cli-tests.exe [==[a discovered case]==]  )\nset_tests_properties( [==[a discovered case]==] PROPERTIES WORKING_DIRECTORY D:/b SKIP_RETURN_CODE 4)\n"
    "${anchorEdge}${catchEdge}" tree)
fc_case("a catch_discover_tests case that does not set the variable is refused" "${tree}" refuse "a discovered case")

fc_stage(notbuilt
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" "" "<none>"
    "${anchorEdge}${catchEdge}" tree)
fc_case("a catch binary whose tests file was never generated is refused, not skipped" "${tree}" refuse "was never generated")

fc_stage(unnamed "${anchorRow}" "${anchorTest}add_test([==[a bracket never closed D:/b/x.exe)\n" "" "<none>" "${anchorEdge}" tree)
fc_case("a registration the reader cannot name is refused rather than counted as set" "${tree}" refuse "a test registration this reader cannot name")

# The two name spellings catch_discover_tests really writes, measured in this build: a
# bracket argument whose body holds brackets, and a bare token. Both set the variable.
fc_stage(spellings
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" ""
    "add_test( [==[RESP: XRANGE returns entries as [id, [field, value]] arrays]==] ${t}/cli-tests.exe [==[RESP: XRANGE returns entries as [id, [field, value]] arrays]==]  )\nset_tests_properties( [==[RESP: XRANGE returns entries as [id, [field, value]] arrays]==] PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT_MODIFICATION ${variable}=set:1)\nadd_test( AvailableCodecsCoverEveryCodec ${t}/cli-tests.exe AvailableCodecsCoverEveryCodec  )\nset_tests_properties( AvailableCodecsCoverEveryCodec PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT_MODIFICATION ${variable}=set:1)\n"
    "${anchorEdge}${catchEdge}" tree)
fc_case("a name holding brackets, and a bare name, are both read and found set" "${tree}" pass "3 registered test(s) read, 0 without ${variable}")

fc_stage(bareunset
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|\n"
    "${anchorTest}${catchInclude}" ""
    "add_test( BareUnsetCase ${t}/cli-tests.exe BareUnsetCase  )\nset_tests_properties( BareUnsetCase PROPERTIES SKIP_RETURN_CODE 4)\n"
    "${anchorEdge}${catchEdge}" tree)
fc_case("a bare-named case that does not set the variable is refused" "${tree}" refuse "1 of 2 registered test(s) do not set ${variable}")
fc_case("... and that refusal names the bare-named case" "${tree}" refuse "The first of them:     BareUnsetCase")

# Two names sharing everything up to a `]` inside them, one set and one not. A reader that
# ends a bracket name at the first `]` reads both as the same name, and the set one hides
# the unset one.
fc_stage(sharedprefix
    "${anchorRow}cli-tests|${t}/cli-tests.exe|always|
"
    "${anchorTest}${catchInclude}" ""
    "add_test( [==[codec [zstd] round trips]==] ${t}/cli-tests.exe [==[codec [zstd] round trips]==]  )
set_tests_properties( [==[codec [zstd] round trips]==] PROPERTIES ENVIRONMENT_MODIFICATION ${variable}=set:1)
add_test( [==[codec [zstd] refuses garbage]==] ${t}/cli-tests.exe [==[codec [zstd] refuses garbage]==]  )
set_tests_properties( [==[codec [zstd] refuses garbage]==] PROPERTIES SKIP_RETURN_CODE 4)
"
    "${anchorEdge}${catchEdge}" tree)
fc_case("a name is read to its CLOSING bracket, so a set case cannot hide an unset one sharing its prefix" "${tree}" refuse "codec [zstd] refuses garbage")

# A testfile ctest never reads -- here, one no `subdirs()` lists, the shape this check's own
# staged trees have inside a real build directory -- is not a registration.
fc_registration(stray "a stray unmarked test" "D:/b/x.exe" FALSE)
fc_stage(unreached "${anchorRow}" "${anchorTest}" "" "<none>" "${anchorEdge}" tree)
file(MAKE_DIRECTORY "${tree}/staged/elsewhere")
file(WRITE "${tree}/staged/elsewhere/CTestTestfile.cmake" "${stray}")
fc_case("a testfile no subdirs() reaches is not read" "${tree}" pass "1 registered test(s) read, 0 without ${variable}")

fc_stage(notests "${anchorRow}" "# no registrations
" "" "<none>" "${anchorEdge}" tree)
fc_case("a build registering no test at all is refused as vacuous" "${tree}" refuse "no registered test was found")

# ---- neither ----------------------------------------------------------------------------

fc_stage(noninja "${anchorRow}" "${anchorTest}" "" "<none>" "<none>" tree)
fc_case("a build with no build.ninja is a SKIP naming why, not a pass" "${tree}" skip "has no build.ninja")

message("check-error-popups self-test: ${cases} case(s) ran")
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "check-error-popups self-test failed:${failures}")
endif()
message("check-error-popups self-test: all ${cases} passed")
