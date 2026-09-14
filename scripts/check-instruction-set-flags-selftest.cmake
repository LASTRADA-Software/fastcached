# SPDX-License-Identifier: Apache-2.0
#
# Drives scripts/check-instruction-set-flags.cmake over synthetic trees and compile databases, in
# both directions, so the check is seen to REFUSE every spelling it exists for and to ACCEPT every
# one it must leave alone. A guard nobody has watched accept is not known to work either.
#
#   accepted   a plain first-party unit; the Apple rows (-mmacosx-version-min=, -arch arm64,
#              -arch x86_64); a bracket-bearing define alone; a vendored unit carrying -msha,
#              declined; a module-map response file, declined; a readable response file with no
#              flag; a target pragma inside a comment; the per-function target attributes the
#              tree's hardware SHA-256 path uses; cl given a GCC -m flag it does not read; the plant
#              mode refusing its planted flag, on a GCC driver and on clang-cl
#   refused    -msha, -msse4.1, -march=native, -mcpu=, an -m flag no row judged, -arch x86_64h,
#              -Xclang -target-feature, /arch:, -arch:, /ARCH: in capitals, clang-cl -msha,
#              clang-cl /clang:-msha, a quoted flag, a flag behind a bracket-bearing define, a
#              versioned driver name, an unknown driver, a flag inside a response file, an
#              unreadable response file, a response file naming another, an empty database, no
#              first-party unit, a database that does not parse, an entry with no command, a
#              missing database, #pragma GCC target, #pragma clang attribute ... target, a src/
#              with no sources, and a plant unit the database does not hold
#
# The three bracket arms are deliberate: the violation alone, the violation behind a bracket, and
# the bracket alone -- without the last, a check that refused every bracket would pass the middle
# one for the wrong reason.
#
# Usage:
#   cmake -DFASTCACHED_SOURCE_DIR=<repo> -DFASTCACHED_SCRATCH_DIR=<dir> \
#         -P scripts/check-instruction-set-flags-selftest.cmake
#
# The verdict is `CMake Error` in the output, never the exit code.

cmake_minimum_required(VERSION 3.28)

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
    set(${out} "{\"directory\": \"${root}/build\", \"file\": \"${root}/${file}\", \"command\": \"${command}\"}" PARENT_SCOPE)
endfunction()

# The common case: a database of the first-party unit compiled by @p command, plus the vendored unit
# compiled cleanly.
function(fastcached_unit_database root command)
    fastcached_entry("${root}" "src/FastCache/Core/Unit.cpp" "${command}" unit)
    fastcached_entry("${root}" "vendor/endo/tui/V.cpp" "/usr/bin/c++ -O2 -c V.cpp" vendored)
    file(WRITE "${root}/build/compile_commands.json" "[\n${unit},\n${vendored}\n]\n")
endfunction()

# Run the check; @p refuses says which side it must land on, @p expected is text the output must hold.
# ARGN is passed to the check as extra -D arguments.
function(fastcached_judge name root refuses expected)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DFASTCACHED_SOURCE_DIR=${root}"
                "-DFASTCACHED_COMPILE_DATABASE=${root}/build/compile_commands.json" ${ARGN} -P "${check}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
    )
    string(APPEND output "${errors}")
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
    # Both words, as ctest's own `FASTCACHED_SCRIPT_CHECK_FAILED` reads a check.
    set(refused OFF)
    if(flat MATCHES "CMake Error|CMake Warning")
        set(refused ON)
    endif()
    string(FIND "${flat}" "${expected}" said)
    set(problem "")
    if(refuses AND NOT refused)
        set(problem "accepted, and must refuse")
    elseif(NOT refuses AND refused)
        set(problem "refused, and must accept")
    elseif(said EQUAL -1)
        set(problem "reached the right side for the wrong reason: no `${expected}`")
    endif()
    math(EXPR count "${ran} + 1")
    set(ran ${count} PARENT_SCOPE)
    if(NOT problem STREQUAL "")
        set(mismatches "${mismatches}\n  ${name}: ${problem}\n${output}" PARENT_SCOPE)
    endif()
endfunction()

set(gxx "/usr/bin/g++ -O2 -DNDEBUG")
set(tail "-c ../src/FastCache/Core/Unit.cpp")

# ---------------------------------------------------------------- accepted

fastcached_tree(clean root)
fastcached_unit_database("${root}" "${gxx} ${tail}")
fastcached_judge(clean "${root}" OFF
    "1 first-party unit(s) judged, none carries a global instruction-set flag; 1 unit(s) outside src/ declined")

fastcached_tree(appleRows root)
fastcached_unit_database("${root}" "/usr/bin/clang++ -mmacosx-version-min=13.0 -arch arm64 -arch x86_64 ${tail}")
fastcached_judge(appleRows "${root}" OFF "none carries a global instruction-set flag")

fastcached_tree(bracketAlone root)
fastcached_unit_database("${root}" "${gxx} -DWIDTHS=[a] ${tail}")
fastcached_judge(bracketAlone "${root}" OFF "none carries a global instruction-set flag")

fastcached_tree(vendoredDeclined root)
fastcached_entry("${root}" "src/FastCache/Core/Unit.cpp" "${gxx} ${tail}" unit)
fastcached_entry("${root}" "vendor/endo/tui/V.cpp" "/usr/bin/c++ -msha -c V.cpp" vendored)
file(WRITE "${root}/build/compile_commands.json" "[${unit},${vendored}]")
fastcached_judge(vendoredDeclined "${root}" OFF "1 unit(s) outside src/ declined (first: ${root}/vendor/endo/tui/V.cpp)")

fastcached_tree(modmapDeclined root)
fastcached_unit_database("${root}" "${gxx} @CMakeFiles/Core.dir/Unit.cpp.o.modmap ${tail}")
fastcached_judge(modmapDeclined "${root}" OFF "1 module-map response file(s) declined")

fastcached_tree(responseClean root)
file(WRITE "${root}/build/flags.rsp" "-O2 -DNDEBUG\n")
fastcached_unit_database("${root}" "${gxx} @flags.rsp ${tail}")
fastcached_judge(responseClean "${root}" OFF "none carries a global instruction-set flag")

fastcached_tree(pragmaInComment root)
file(WRITE "${root}/src/FastCache/Core/Notes.hpp" [==[
// Never write #pragma GCC target("sha") here: it reaches every function after it.
/* #pragma clang attribute push (__attribute__((target("avx2"))), apply_to = function) */
#pragma once
]==])
fastcached_unit_database("${root}" "${gxx} ${tail}")
fastcached_judge(pragmaInComment "${root}" OFF "2 source(s) under src/ carry no target pragma")

# The tree's one legitimate spelling, copied from the hardware SHA-256 path (#1420): each function
# asks for its instructions, a macro spells the attribute, and nothing is enabled for the unit.
# FAITHFUL COPY until #1420 merges; the rebase reads the real src/FastCache/Core/Sha256.cpp instead.
fastcached_tree(perFunctionAttributes root)
file(WRITE "${root}/src/FastCache/Core/Sha256.cpp" [==[
// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Sha256.hpp>

#if defined(_M_X64) || defined(__x86_64__)
    #include <immintrin.h>
#elif defined(__APPLE__) && defined(__aarch64__)
    #include <arm_neon.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
    // Asked for per function, never by a global -m flag, for the reason
    // .agent/rules/build-and-toolchain.md gives under instruction-set extensions. cl
    // needs no attribute.
    #if defined(__GNUC__) || defined(__clang__)
        #define FASTCACHED_SHA_NI_TARGET __attribute__((target("sha,ssse3,sse4.1")))
    #else
        #define FASTCACHED_SHA_NI_TARGET
    #endif

    [[nodiscard]] FASTCACHED_SHA_NI_TARGET __m128i LoadWordsX86(std::span<std::byte const> block, std::size_t index) noexcept
    {
    }

    FASTCACHED_SHA_NI_TARGET void CompressBlocksX86ShaNi(State& state, std::span<std::byte const> blocks) noexcept
    {
    }

    #undef FASTCACHED_SHA_NI_TARGET
#endif

#if defined(__APPLE__) && defined(__aarch64__)
    // Asked for although Apple's default target enables sha2: an explicit -march=armv8-a
    // refuses the intrinsics otherwise (.agent/rules/build-and-toolchain.md, the same
    // entry). Only clang compiles this block.
    #define FASTCACHED_ARM_SHA2_TARGET __attribute__((target("sha2")))

    [[nodiscard]] FASTCACHED_ARM_SHA2_TARGET uint32x4_t LoadWordsArm(std::span<std::byte const> block,
                                                                     std::size_t index) noexcept
    {
    }

    FASTCACHED_ARM_SHA2_TARGET void CompressBlocksArmSha2(State& state, std::span<std::byte const> blocks) noexcept
    {
    }

    #undef FASTCACHED_ARM_SHA2_TARGET
#endif
]==])
fastcached_unit_database("${root}" "${gxx} ${tail}")
fastcached_judge(perFunctionAttributes "${root}" OFF "2 source(s) under src/ carry no target pragma")

# cl does not read GCC's -m flags (it warns D9002 and ignores them), so a -m flag there enables nothing.
fastcached_tree(clIgnoresGccFlag root)
fastcached_unit_database("${root}" "C:/VS/bin/cl.exe /nologo /O2 -msha ${tail}")
fastcached_judge(clIgnoresGccFlag "${root}" OFF "none carries a global instruction-set flag")

set(plant "-DFASTCACHED_PLANT_UNIT=src/FastCache/Core/Unit.cpp")

fastcached_tree(plantRefusedGnu root)
fastcached_unit_database("${root}" "${gxx} ${tail}")
fastcached_judge(plantRefusedGnu "${root}" OFF
    "`-msha` planted into src/FastCache/Core/Unit.cpp (1 entr(y/ies)) refused as it must" "${plant}")

fastcached_tree(plantRefusedClangCl root)
fastcached_unit_database("${root}" "C:/LLVM/bin/clang-cl.exe /O2 ${tail}")
fastcached_judge(plantRefusedClangCl "${root}" OFF
    "`-msha`, `/arch:AVX2` planted into src/FastCache/Core/Unit.cpp (2 entr(y/ies)) refused as it must" "${plant}")

# ---------------------------------------------------------------- refused, one spelling each

foreach(spelling IN ITEMS "-msha" "-msse4.1" "-march=native" "-mcpu=apple-m1" "-mfuture-extension"
                          "-arch x86_64h" "-target-feature")
    string(MAKE_C_IDENTIFIER "gnu${spelling}" caseName)
    fastcached_tree(${caseName} root)
    fastcached_unit_database("${root}" "/usr/bin/clang++ -O2 ${spelling} ${tail}")
    fastcached_judge(${caseName} "${root}" ON "src/FastCache/Core/Unit.cpp: `${spelling}`, because")
endforeach()

fastcached_tree(xclangTargetFeature root)
fastcached_unit_database("${root}" "/usr/bin/clang++ -Xclang -target-feature -Xclang +sha ${tail}")
fastcached_judge(xclangTargetFeature "${root}" ON "`-target-feature`, because it is clang's own switch")

foreach(spelling IN ITEMS "/arch:AVX2" "-arch:AVX512" "/ARCH:AVX2")
    string(MAKE_C_IDENTIFIER "msvc${spelling}" caseName)
    fastcached_tree(${caseName} root)
    fastcached_unit_database("${root}" "C:/VS/bin/cl.exe /nologo /O2 ${spelling} ${tail}")
    fastcached_judge(${caseName} "${root}" ON "src/FastCache/Core/Unit.cpp: `${spelling}`, because")
endforeach()

fastcached_tree(clangClGccFlag root)
fastcached_unit_database("${root}" "C:/LLVM/bin/clang-cl.exe /O2 -msha ${tail}")
fastcached_judge(clangClGccFlag "${root}" ON "`-msha`, because it is an -m flag no row has judged")

fastcached_tree(clangClUnwrap root)
fastcached_unit_database("${root}" "C:/LLVM/bin/clang-cl.exe /O2 /clang:-msha ${tail}")
fastcached_judge(clangClUnwrap "${root}" ON "`/clang:-msha`, because it is an -m flag no row has judged")

fastcached_tree(quotedFlag root)
fastcached_unit_database("${root}" "${gxx} \"-msha\" ${tail}")
fastcached_judge(quotedFlag "${root}" ON "`-msha`, because")

fastcached_tree(behindBracket root)
fastcached_unit_database("${root}" "${gxx} -DWIDTHS=[a] -msha ${tail}")
fastcached_judge(behindBracket "${root}" ON "`-msha`, because")

# An unbalanced bracket INSIDE a candidate token. Without the blanking, CMake's list parser fuses the
# two candidates into one element and `-msha` is never named -- which is what the blanking is for.
fastcached_tree(bracketInCandidate root)
fastcached_unit_database("${root}" "${gxx} -mfoo=[x -msha ${tail}")
fastcached_judge(bracketInCandidate "${root}" ON "`-msha`, because")

fastcached_tree(versionedDriver root)
fastcached_unit_database("${root}" "/usr/bin/x86_64-linux-gnu-g++-14 -O2 -mavx2 ${tail}")
fastcached_judge(versionedDriver "${root}" ON "`-mavx2`, because")

fastcached_tree(unknownDriver root)
fastcached_unit_database("${root}" "/opt/intel/bin/icpx -O2 ${tail}")
fastcached_judge(unknownDriver "${root}" ON "compiled by `icpx`, a driver DriverRows does not name")

fastcached_tree(responseFlag root)
file(WRITE "${root}/build/flags.rsp" "-O2\n-msha\n")
fastcached_unit_database("${root}" "${gxx} @flags.rsp ${tail}")
fastcached_judge(responseFlag "${root}" ON "`-msha`, because")

fastcached_tree(responseUnreadable root)
fastcached_unit_database("${root}" "${gxx} @missing.rsp ${tail}")
fastcached_judge(responseUnreadable "${root}" ON "cannot read response file `@missing.rsp`")

fastcached_tree(responseNested root)
file(WRITE "${root}/build/outer.rsp" "-O2 @inner.rsp\n")
file(WRITE "${root}/build/inner.rsp" "-O2\n")
fastcached_unit_database("${root}" "${gxx} @outer.rsp ${tail}")
fastcached_judge(responseNested "${root}" ON "names `@inner.rsp` inside a response file")

fastcached_tree(emptyDatabase root)
file(WRITE "${root}/build/compile_commands.json" "[]\n")
fastcached_judge(emptyDatabase "${root}" ON "has no entries, so no flag has been judged")

fastcached_tree(noFirstParty root)
fastcached_entry("${root}" "vendor/endo/tui/V.cpp" "/usr/bin/c++ -O2 -c V.cpp" vendored)
file(WRITE "${root}/build/compile_commands.json" "[${vendored}]")
fastcached_judge(noFirstParty "${root}" ON "no entry compiles a unit under")

fastcached_tree(unparseable root)
file(WRITE "${root}/build/compile_commands.json" "[{\"directory\": \n")
fastcached_judge(unparseable "${root}" ON "cannot be parsed as a compile database")

fastcached_tree(noCommand root)
file(WRITE "${root}/build/compile_commands.json"
    "[{\"directory\": \"${root}/build\", \"file\": \"${root}/src/FastCache/Core/Unit.cpp\", \"arguments\": [\"g++\", \"-msha\"]}]")
fastcached_judge(noCommand "${root}" ON "its entry has no `command`")

fastcached_tree(missingDatabase root)
fastcached_judge(missingDatabase "${root}" ON "does not exist, so no flag has been judged")

fastcached_tree(pragmaGcc root)
file(WRITE "${root}/src/FastCache/Core/Fast.cpp" "#include <x.hpp>\n#pragma GCC target(\"sha\")\nint Fast() { return 3; }\n")
fastcached_unit_database("${root}" "${gxx} ${tail}")
fastcached_judge(pragmaGcc "${root}" ON "src/FastCache/Core/Fast.cpp:2: a target pragma enables an instruction set")

fastcached_tree(pragmaClang root)
file(WRITE "${root}/src/FastCache/Core/Fast.hpp" [==[
#pragma once
  #  pragma clang attribute push (__attribute__((target("avx2"))), apply_to = function)
]==])
fastcached_unit_database("${root}" "${gxx} ${tail}")
fastcached_judge(pragmaClang "${root}" ON "src/FastCache/Core/Fast.hpp:2: a target pragma")

fastcached_tree(noSources root)
file(REMOVE "${root}/src/FastCache/Core/Unit.cpp")
fastcached_unit_database("${root}" "${gxx} ${tail}")
fastcached_judge(noSources "${root}" ON "no C or C++ source under")

fastcached_tree(plantUnitAbsent root)
fastcached_unit_database("${root}" "${gxx} ${tail}")
fastcached_judge(plantUnitAbsent "${root}" ON "is not a first-party unit of this database, so the plant was never judged"
    "-DFASTCACHED_PLANT_UNIT=src/FastCache/Core/Elsewhere.cpp")

if(NOT mismatches STREQUAL "")
    message(FATAL_ERROR "instruction-set-flags-selftest: ${ran} case(s) ran, and these did not judge as they must:${mismatches}")
endif()
message(STATUS "instruction-set-flags-selftest: ${ran} case(s) ran, every verdict as it must be")
