# SPDX-License-Identifier: Apache-2.0
#
# Drives scripts/check-instruction-set-flags.cmake over synthetic compile databases in both directions, so the
# check is seen to REFUSE every spelling it exists for and to ACCEPT every one it must leave alone: a guard
# nobody has watched accept is not known to work either. Each case is named by its first field or argument.
#
# The bracket cases are deliberate: the violation alone, the violation behind a bracket-bearing define, the
# define alone, and an unbalanced bracket inside a candidate token -- without the accepting arm, a check that
# refused every bracket would pass the middle one for the wrong reason, and only the last one needs the
# blanking at all.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-instruction-set-flags-selftest.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/lib/CheckCommon.cmake")

foreach(required IN ITEMS FASTCACHED_SOURCE_DIR FASTCACHED_SCRATCH_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} must be set")
    endif()
endforeach()

set(check "${FASTCACHED_SOURCE_DIR}/scripts/check-instruction-set-flags.cmake")
if(NOT EXISTS "${check}")
    message(FATAL_ERROR "the check under test is missing: ${check}")
endif()

set(ran 0)
set(mismatches "")

# A tree with one first-party source and one vendored source, and an empty build directory.
function(fastcached_tree name out)
    set(root "${FASTCACHED_SCRATCH_DIR}/${name}")
    file(REMOVE_RECURSE "${root}")
    file(WRITE "${root}/src/FastCache/Core/Unit.cpp" "int Unit() { return 1; }\n")
    file(WRITE "${root}/vendor/endo/tui/V.cpp" "int V() { return 2; }\n")
    file(MAKE_DIRECTORY "${root}/build")
    set(${out} "${root}" PARENT_SCOPE)
endfunction()

# One compile-database entry, built in @p root/build, compiling @p file (relative to @p root).
function(fastcached_entry root file command out)
    foreach(field IN ITEMS root file command)
        string(REPLACE "\\" "\\\\" ${field} "${${field}}")
        string(REPLACE "\"" "\\\"" ${field} "${${field}}")
    endforeach()
    set(${out} "{\n  \"directory\": \"${root}/build\",\n  \"command\": \"${command}\",\n  \"file\": \"${root}/${file}\"\n}" PARENT_SCOPE)
endfunction()

# A database of the first-party unit compiled by @p command, plus the vendored unit compiled cleanly.
function(fastcached_unit_database root command)
    fastcached_entry("${root}" "src/FastCache/Core/Unit.cpp" "${command}" unit)
    fastcached_entry("${root}" "vendor/endo/tui/V.cpp" "/usr/bin/c++ -O2 -c V.cpp" vendored)
    file(WRITE "${root}/build/compile_commands.json" "[\n${unit},\n${vendored}\n]\n")
endfunction()

# Run the check; @p refuses says which side it must land on, @p expected is text the output must hold.
# @p parse is the path the run must report taking -- `batched (N batch(es))`, `whole-document (fallback: <reason>`,
# or `unread` for a database refused before any path was chosen. A case states which path it exercised, or a fast
# path that silently stopped engaging would pass every case. ARGN is passed to the check as extra -D arguments.
function(fastcached_judge name root refuses parse expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${root}"
                "-DFASTCACHED_COMPILE_DATABASE=${root}/build/compile_commands.json" ${ARGN} -P "${check}"
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
    )
    string(APPEND output "${errors}")
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
    # Both words, as ctest's own `FASTCACHED_SCRIPT_CHECK_FAILED` reads a check: a sub-run that merely WARNS
    # must not be scored a clean pass here while ctest would refuse it.
    set(refused OFF)
    if(flat MATCHES "CMake Error|CMake Warning")
        set(refused ON)
    endif()
    string(FIND "${flat}" "${expected}" said)
    if(parse STREQUAL "unread")
        string(FIND "${flat}" "parse: " parsed)
        set(pathTaken TRUE)
        if(NOT parsed EQUAL -1)
            set(pathTaken FALSE)
        endif()
    else()
        string(FIND "${flat}" "parse: ${parse}" parsed)
        set(pathTaken FALSE)
        if(NOT parsed EQUAL -1)
            set(pathTaken TRUE)
        endif()
    endif()
    set(problem "")
    if(refuses AND NOT refused)
        set(problem "accepted, and must refuse")
    elseif(NOT refuses AND refused)
        set(problem "refused, and must accept")
    elseif(said EQUAL -1)
        set(problem "reached the right side for the wrong reason: no `${expected}`")
    elseif(NOT pathTaken)
        set(problem "reached the right verdict by another parse path: not `parse: ${parse}`")
    endif()
    math(EXPR ran "${ran} + 1")
    set(ran "${ran}" PARENT_SCOPE)
    if(NOT problem STREQUAL "")
        set(mismatches "${mismatches}\n  ${name}: ${problem}\n${output}" PARENT_SCOPE)
    endif()
endfunction()

set(gxx "/usr/bin/g++ -O2 -DNDEBUG")
set(clangxx "/usr/bin/clang++ -O2")
set(cl "C:/VS/bin/cl.exe /nologo /O2")
set(clangcl "C:/LLVM/bin/clang-cl.exe /O2")
set(tail "-c ../src/FastCache/Core/Unit.cpp")
set(plant "-DFASTCACHED_PLANT_UNIT=src/FastCache/Core/Unit.cpp")
set(judged "none carries a global instruction-set flag")

# name|refuses|expected|extra -D argument|command. One first-party unit compiled by the command, one vendored
# unit compiled cleanly. The command is last, so it may hold anything but a semicolon or an unbalanced bracket.
set(UnitCases
    "clean|OFF|1 first-party unit(s) judged, ${judged}||${gxx} ${tail}"
    "appleDeploymentTarget|OFF|${judged}||${clangxx} -mmacosx-version-min=13.3 ${tail}"
    "bracketAlone|OFF|${judged}||${gxx} -DWIDTHS=[a] ${tail}"
    "modmapDeclined|OFF|1 module-map response file(s) declined||${gxx} @CMakeFiles/Core.dir/Unit.cpp.o.modmap ${tail}"
    "clIgnoresGccFlag|OFF|${judged}||${cl} -msha ${tail}"
    "gccIgnoresClSpelling|OFF|${judged}||${gxx} -arch:AVX2 ${tail}"
    "plantRefusedGnu|OFF|`-msha` planted into src/FastCache/Core/Unit.cpp (1 entr(y/ies)) refused as it must|${plant}|${gxx} ${tail}"
    "plantRefusedClangCl|OFF|`-msha`, `/arch:AVX2` planted into src/FastCache/Core/Unit.cpp (1 entr(y/ies)) refused as it must|${plant}|${clangcl} ${tail}"
    "gnuSha|ON|src/FastCache/Core/Unit.cpp: `-msha`, because||${clangxx} -msha ${tail}"
    "gnuSse41|ON|`-msse4.1`, because||${clangxx} -msse4.1 ${tail}"
    "gnuMarchNative|ON|`-march=native`, because it selects a CPU||${gxx} -march=native ${tail}"
    "gnuMcpu|ON|`-mcpu=apple-m1`, because it selects a CPU||${clangxx} -mcpu=apple-m1 ${tail}"
    "gnuUnknownM|ON|`-mfuture-extension`, because it is an -m flag no row has judged||${gxx} -mfuture-extension ${tail}"
    "appleSliceHaswell|ON|`-arch x86_64h`, because it names an Apple slice||${clangxx} -arch x86_64h ${tail}"
    "appleSliceArm64|OFF|${judged}||${clangxx} -arch  arm64 ${tail}"
    "appleSliceX86_64|OFF|${judged}||${clangxx} -arch x86_64 ${tail}"
    "appleSliceArm64Quoted|OFF|${judged}||${clangxx} \"-arch\" \"arm64\" ${tail}"
    "appleSliceUnknown|ON|`-arch arm64e`, because it names an Apple slice||${clangxx} -arch arm64e ${tail}"
    "appleSliceNotASlice|ON|`-arch -c`, because it names an Apple slice||${clangxx} -arch ${tail}"
    "targetFeature|ON|`-target-feature`, because it is clang's own switch||${clangxx} -Xclang -target-feature -Xclang +sha ${tail}"
    "targetCpu|ON|`-target-cpu`, because it is clang's own switch for a CPU||${clangxx} -Xclang -target-cpu -Xclang haswell ${tail}"
    "cc1Triple|ON|`-triple`, because it is clang's own switch for a target triple||${clangxx} -Xclang -triple -Xclang x86_64h-apple-macosx ${tail}"
    "exactRowIsTheWholeToken|OFF|${judged}||${clangxx} -triple-probe ${tail}"
    "targetTripleJoined|ON|`--target=x86_64h-apple-macosx`, because it names a target triple||${clangxx} --target=x86_64h-apple-macosx ${tail}"
    "targetTripleSeparate|ON|`-target x86_64h-apple-macosx`, because it names a target triple||${clangxx} -target x86_64h-apple-macosx ${tail}"
    "msvcArch|ON|`/arch:AVX2`, because it enables an instruction set||${cl} /arch:AVX2 ${tail}"
    "msvcArchDash|ON|`-arch:AVX512`, because it is cl's other spelling||${cl} -arch:AVX512 ${tail}"
    "msvcArchCapitals|ON|`/ARCH:AVX2`, because it enables an instruction set||${cl} /ARCH:AVX2 ${tail}"
    "clangClGccFlag|ON|`-msha`, because it is an -m flag no row has judged||${clangcl} -msha ${tail}"
    "clangClForwarded|ON|`/clang:-msha`, because it is an -m flag no row has judged||${clangcl} /clang:-msha ${tail}"
    "clangClArchSeparate|ON|`/clang:-arch`, because it names an Apple slice||${clangcl} /clang:-arch /clang:x86_64h ${tail}"
    "clangClTargetSeparate|ON|`/clang:-target`, because it names a target triple||${clangcl} /clang:-target /clang:x86_64h-apple-macosx ${tail}"
    "clangClArchLookalike|OFF|${judged}||${clangcl} /clang:-archive-member ${tail}"
    "quotedFlag|ON|`-msha`, because||${gxx} \"-msha\" ${tail}"
    "quotedFlagValue|ON|`-arch x86_64h`, because it names an Apple slice||${clangxx} \"-arch\" \"x86_64h\" ${tail}"
    "clangClDashForwarded|ON|`-clang:-mavx2`, because it is an -m flag no row has judged||${clangcl} -clang:-mavx2 ${tail}"
    "xclangJoined|ON|`-Xclang=-target-cpu`, because it is clang's own switch for a CPU||${clangxx} -Xclang=-target-cpu -Xclang=haswell ${tail}"
    "gccMachineDash|ON|`--machine-sha`, because it is GCC's long spelling of -m||${gxx} --machine-sha ${tail}"
    "gccMachineEquals|ON|`--machine=avx2`, because it is GCC's long spelling of -m||${gxx} --machine=avx2 ${tail}"
    "gccMachineSeparate|ON|`--machine`, because it is GCC's long spelling of -m||${gxx} --machine avx2 ${tail}"
    "behindBracket|ON|`-msha`, because||${gxx} -DWIDTHS=[a] -msha ${tail}"
    "versionedDriver|ON|`-mavx2`, because||/usr/bin/x86_64-linux-gnu-g++-14 -O2 -mavx2 ${tail}"
    "unknownDriver|ON|compiled by `icpx`, a driver DriverRows does not name||/opt/intel/bin/icpx -O2 ${tail}"
    "responseUnreadable|ON|cannot read response file `@missing.rsp`||${gxx} @missing.rsp ${tail}"
    "plantUnitUnreadable|ON|is in this database, but none of its 1 entr(y/ies) could be read|${plant}|/opt/intel/bin/icpx -O2 ${tail}"
    "plantUnitAbsent|ON|so the plant was never judged The plant is a flag this check places itself and must refuse, so nothing in the tree needs changing|-DFASTCACHED_PLANT_UNIT=src/FastCache/Core/Elsewhere.cpp|${gxx} ${tail}"
)
foreach(row IN LISTS UnitCases)
    fastcached_row_fields("${row}" caseName caseRefuses caseExpected caseExtra caseCommand)
    fastcached_tree(${caseName} root)
    fastcached_unit_database("${root}" "${caseCommand}")
    fastcached_judge(${caseName} "${root}" ${caseRefuses} "batched (1 batch(es))" "${caseExpected}" ${caseExtra})
endforeach()

# ---------------------------------------------------------------- cases that need more than one command

fastcached_tree(vendoredDeclined root)
fastcached_entry("${root}" "src/FastCache/Core/Unit.cpp" "${gxx} ${tail}" unit)
fastcached_entry("${root}" "vendor/endo/tui/V.cpp" "/usr/bin/c++ -msha -c V.cpp" vendored)
file(WRITE "${root}/build/compile_commands.json" "[\n${unit},\n${vendored}\n]\n")
fastcached_judge(vendoredDeclined "${root}" OFF "batched (1 batch(es))" "1 unit(s) outside src/ declined (first: ${root}/vendor/endo/tui/V.cpp)")

# The plant run answers only whether a planted flag is still refused: a real violation on another unit is the
# unplanted run's red, and must not make the plant run red as well.
fastcached_tree(plantBesideRealViolation root)
file(WRITE "${root}/src/FastCache/Core/Other.cpp" "int Other() { return 3; }\n")
fastcached_entry("${root}" "src/FastCache/Core/Unit.cpp" "${gxx} ${tail}" unit)
fastcached_entry("${root}" "src/FastCache/Core/Other.cpp" "${gxx} -mavx2 -c ../src/FastCache/Core/Other.cpp" other)
file(WRITE "${root}/build/compile_commands.json" "[\n${unit},\n${other}\n]\n")
fastcached_judge(plantBesideRealViolation "${root}" OFF "batched (1 batch(es))"
    "`-msha` planted into src/FastCache/Core/Unit.cpp (1 entr(y/ies)) refused as it must; 2 first-party unit(s) judged; 1 unplanted problem(s) left to the unplanted run"
    "${plant}")
fastcached_judge(realViolationUnplanted "${root}" ON "batched (1 batch(es))" "src/FastCache/Core/Other.cpp: `-mavx2`, because")
# A refusal of the TREE sends its reader to the tree; the plant runs above send theirs to the check.
fastcached_judge(realViolationRemedy "${root}" ON "batched (1 batch(es))" "An instruction set is asked for per function")

fastcached_tree(responseClean root)
file(WRITE "${root}/build/flags.rsp" "-O2 -DNDEBUG\n")
fastcached_unit_database("${root}" "${gxx} @flags.rsp ${tail}")
fastcached_judge(responseClean "${root}" OFF "batched (1 batch(es))" "${judged}")

fastcached_tree(responseFlag root)
file(WRITE "${root}/build/flags.rsp" "-O2\n-msha\n")
fastcached_unit_database("${root}" "${gxx} @flags.rsp ${tail}")
fastcached_judge(responseFlag "${root}" ON "batched (1 batch(es))" "`-msha`, because")

# One argument per line, as a response file may be written: the flag and its value are still a pair.
fastcached_tree(responseArchOnTwoLines root)
file(WRITE "${root}/build/flags.rsp" "-O2\n-arch\nx86_64h\n")
fastcached_unit_database("${root}" "${gxx} @flags.rsp ${tail}")
fastcached_judge(responseArchOnTwoLines "${root}" ON "batched (1 batch(es))" "`-arch x86_64h`, because it names an Apple slice")

fastcached_tree(responseNested root)
file(WRITE "${root}/build/outer.rsp" "-O2 @inner.rsp\n")
file(WRITE "${root}/build/inner.rsp" "-O2\n")
fastcached_unit_database("${root}" "${gxx} @outer.rsp ${tail}")
fastcached_judge(responseNested "${root}" ON "batched (1 batch(es))" "names `@inner.rsp` inside a response file")

# An unbalanced bracket INSIDE a candidate token. Without the blanking, CMake's list parser fuses the two
# candidates into one element and `-msha` is never named -- which is what the blanking is for. Not a table
# row: the rows are a CMake list, and this bracket would fuse them.
fastcached_tree(bracketInCandidate root)
fastcached_unit_database("${root}" "${gxx} -mfoo=[x -msha ${tail}")
fastcached_judge(bracketInCandidate "${root}" ON "batched (1 batch(es))" "`-msha`, because")

fastcached_tree(emptyDatabase root)
file(WRITE "${root}/build/compile_commands.json" "[]\n")
fastcached_judge(emptyDatabase "${root}" ON unread "has no entries, so no flag has been judged")

fastcached_tree(noFirstParty root)
fastcached_entry("${root}" "vendor/endo/tui/V.cpp" "/usr/bin/c++ -O2 -c V.cpp" vendored)
file(WRITE "${root}/build/compile_commands.json" "[\n${vendored}\n]\n")
fastcached_judge(noFirstParty "${root}" ON "batched (1 batch(es))" "no entry compiles a unit under")

fastcached_tree(unparseable root)
file(WRITE "${root}/build/compile_commands.json" "[{\"directory\": \n")
fastcached_judge(unparseable "${root}" ON unread "cannot be parsed as a compile database")

fastcached_tree(noCommand root)
file(WRITE "${root}/build/compile_commands.json"
    "[\n{\n  \"directory\": \"${root}/build\",\n  \"arguments\": [\"g++\", \"-msha\"],\n  \"file\": \"${root}/src/FastCache/Core/Unit.cpp\"\n}\n]\n")
fastcached_judge(noCommand "${root}" ON "batched (1 batch(es))" "its entry has no `command`")

fastcached_tree(missingDatabase root)
fastcached_judge(missingDatabase "${root}" ON unread "does not exist, so no flag has been judged")

# ---------------------------------------------------------------- the parse paths
#
# A database CMake did not lay out is read whole, and must be judged exactly as the batched reading would: each
# layout below reaches the fallback for its own reason, refuses a real `-msha`, and passes a plant.

# @p layout names the fixture; @p flags go on the first-party command.
function(fastcached_layout root layout flags)
    set(command "${gxx} ${flags} ${tail}")
    set(unitPath "${root}/src/FastCache/Core/Unit.cpp")
    set(vendored "{\n  \"directory\": \"${root}/build\",\n  \"command\": \"/usr/bin/c++ -O2 -c V.cpp\",\n  \"file\": \"${root}/vendor/endo/tui/V.cpp\"\n}")
    if(layout STREQUAL "noLineBreak")
        set(text "[{\"directory\": \"${root}/build\", \"command\": \"${command}\", \"file\": \"${unitPath}\"}]")
    elseif(layout STREQUAL "entryEndsDisagree")
        # A nested object whose closing brace starts a line: one more line-leading `}` than entries.
        set(text "[\n{\n  \"directory\": \"${root}/build\",\n  \"extra\": {\n  \"a\": 1\n},\n  \"command\": \"${command}\",\n  \"file\": \"${unitPath}\"\n}\n]\n")
    elseif(layout STREQUAL "batchUnparseable")
        # The counts agree -- the nested object's `}` stands in for the unit's own, which ends its last line -- and
        # the padding carries the unit's real end past the first batch, so the cut lands inside the nested object.
        string(REPEAT "-DPAD=0123456789 " 6000 pad)
        set(text "[\n{\n  \"directory\": \"${root}/build\",\n  \"file\": \"${unitPath}\",\n  \"extra\": {\n  \"a\": 1\n},\n  \"command\": \"${gxx} ${pad}${flags} ${tail}\"},\n${vendored}\n]\n")
    else()
        message(FATAL_ERROR "no layout `${layout}`")
    endif()
    file(WRITE "${root}/build/compile_commands.json" "${text}")
endfunction()

# layout|reason the run must name
set(FallbackLayouts
    "noLineBreak|0 line(s) start with `}` against 1 entries"
    "entryEndsDisagree|2 line(s) start with `}` against 1 entries"
    "batchUnparseable|batch 1 does not parse on its own"
)
foreach(row IN LISTS FallbackLayouts)
    fastcached_row_fields("${row}" layout reason)
    fastcached_tree(${layout}Refused root)
    fastcached_layout("${root}" ${layout} "-msha")
    fastcached_judge(${layout}Refused "${root}" ON "whole-document (fallback: ${reason}" "src/FastCache/Core/Unit.cpp: `-msha`, because")
    fastcached_tree(${layout}Plant root)
    fastcached_layout("${root}" ${layout} "")
    fastcached_judge(${layout}Plant "${root}" OFF "whole-document (fallback: ${reason}"
        "`-msha` planted into src/FastCache/Core/Unit.cpp (1 entr(y/ies)) refused as it must" "${plant}")
endforeach()

# CMake's layout across real cuts: three entries of about 41 KB against a BatchBytes of 64 KB make three batches, and
# the flag sits in the last one, which only a cut that kept every entry whole can reach.
fastcached_tree(batchedAcrossCuts root)
string(REPEAT "-DPAD=0123456789 " 2400 pad)
fastcached_entry("${root}" "src/FastCache/Core/Unit.cpp" "${gxx} ${pad}${tail}" first)
fastcached_entry("${root}" "src/FastCache/Core/Second.cpp" "${gxx} ${pad}-c ../src/FastCache/Core/Second.cpp" second)
fastcached_entry("${root}" "src/FastCache/Core/Third.cpp" "${gxx} ${pad}-msha -c ../src/FastCache/Core/Third.cpp" third)
file(WRITE "${root}/build/compile_commands.json" "[\n${first},\n${second},\n${third}\n]\n")
fastcached_judge(batchedAcrossCuts "${root}" ON "batched (3 batch(es))" "src/FastCache/Core/Third.cpp: `-msha`, because")

if(NOT mismatches STREQUAL "")
    message(FATAL_ERROR "instruction-set-flags-selftest: ${ran} case(s) ran, and these did not judge as they must:${mismatches}")
endif()
message(STATUS "instruction-set-flags-selftest: ${ran} case(s) ran, every verdict as it must be")
