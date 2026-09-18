# SPDX-License-Identifier: Apache-2.0
#
# The instruction-set baseline probe refuses what it exists to refuse, and clears what it must
# clear, with THIS build's compiler (#1447).
#
# `src/tests/InstructionSetBaseline.cpp` is attached to every executable, so every CI leg's
# build is already its control: a default build of each CI compiler passes, or the leg is red.
# What no build shows is the REFUSING direction -- a guard nobody has watched refuse is not
# known to work -- and the ticket names the routes it must refuse that #1442's database check
# cannot see: `CL=/arch:AVX2` for cl, `CCC_OVERRIDE_OPTIONS` and a driver configuration file
# for clang. So this compiles the probe directly, syntax only, once per case:
#
#   control     nothing planted                          passes
#   attribute   a function with a target attribute       passes -- it defines no unit-wide macro
#   flag        the instruction set on the command line  refused, naming the macro
#   environment the instruction set in the environment   refused, naming the macro
#   config      the instruction set in a --config file   refused, naming the macro
#
# A case this compiler cannot express (GCC has no environment route, cl needs no attribute) is
# SKIPPED BY NAME with its reason, and the number of cases that RAN is printed: a self-test that
# stops early must not look like one that judged something. A run that ran nothing is refused.
#
# Its verdict is its OUTPUT (a `CMake Error` fails the registered test through
# FAIL_REGULAR_EXPRESSION), never its exit code alone.
#
# Usage:
#   cmake -DCXX=<compiler> -DCOMPILER_ID=<id> -DFRONTEND=<GNU|MSVC> -DARCH=<processor>
#         -DPROBE=<InstructionSetBaseline.cpp> -DSCRATCH=<dir> -P check-instruction-set-probe-selftest.cmake

cmake_minimum_required(VERSION 3.28)

foreach(required CXX COMPILER_ID ARCH PROBE SCRATCH)
    if("${${required}}" STREQUAL "")
        message(FATAL_ERROR "instruction-set-probe-selftest: -D${required}= is required")
    endif()
endforeach()
if("${FRONTEND}" STREQUAL "")
    # CMake leaves the frontend variant empty for compilers that have only one.
    if(COMPILER_ID STREQUAL "MSVC")
        set(FRONTEND "MSVC")
    else()
        set(FRONTEND "GNU")
    endif()
endif()
if(NOT EXISTS "${PROBE}")
    message(FATAL_ERROR "instruction-set-probe-selftest: the probe ${PROBE} does not exist")
endif()

# Which baseline row this compiler lands in, spelled the way the probe's blocks are chosen.
string(TOLOWER "${ARCH}" arch)
if(arch MATCHES "^(x86_64|amd64|x64)$")
    set(row "x86-64")
elseif(arch MATCHES "^(aarch64|arm64)$")
    if(APPLE_HOST)
        set(row "apple")
    else()
        set(row "aarch64")
    endif()
else()
    message(FATAL_ERROR "instruction-set-probe-selftest: no plant is defined for architecture '${ARCH}'; the probe refuses it too, so add a row to both")
endif()

# ---------------------------------------------------------------------------------------------
# The plants, per baseline row: the flag that asks for an instruction set the row refuses, the
# macro the refusal must name, and a target attribute the attribute control compiles.
if(row STREQUAL "x86-64")
    set(plantFlag "-msha")
    set(plantMacro "__SHA__")
    set(clPlant "/arch:AVX2")
    # Not `__AVX2__`: cl's `#error` is FATAL, so it names the first row it reaches, and AVX2
    # implies AVX. clang reports every row and names both.
    set(clMacro "__AVX__")
    set(attribute "sha,ssse3,sse4.1")
elseif(row STREQUAL "aarch64")
    set(plantFlag "-march=armv8-a+crypto")
    set(plantMacro "__ARM_FEATURE_")
    set(clPlant "/arch:armv8.1")
    set(clMacro "__ARM_ARCH")
    if(COMPILER_ID STREQUAL "GNU")
        set(attribute "+sha2")
    else()
        set(attribute "sha2")
    endif()
else()
    set(plantFlag "-march=armv8-a+sve")
    set(plantMacro "__ARM_FEATURE_SVE")
    set(clPlant "")
    set(clMacro "")
    set(attribute "sha2")
endif()

file(MAKE_DIRECTORY "${SCRATCH}")
file(TO_CMAKE_PATH "${PROBE}" probePath)

set(ran 0)
set(skipped "")
set(failures "")

# Compile @p source syntax only, with @p environment (a list of VAR=value, or empty) and the
# extra @p arguments, and judge it against @p expect: "pass", or the macro a refusal must name.
function(probe_case name expect source environment)
    set(arguments ${ARGN})
    if(FRONTEND STREQUAL "MSVC")
        set(command "${CXX}" /nologo /Zs /TP ${arguments} "${source}")
    else()
        set(command "${CXX}" -fsyntax-only -x c++ ${arguments} "${source}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env ${environment} ${command}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    set(output "${out}${err}")
    string(REPLACE "\n" " " flat "${output}")
    if(expect STREQUAL "pass")
        if(status EQUAL 0 AND NOT flat MATCHES "#1447")
            message(STATUS "ok: ${name} -- passes")
        else()
            set(failures "${failures}\n  ${name}: expected to PASS, exit ${status}: ${flat}" PARENT_SCOPE)
        endif()
    else()
        string(FIND "${flat}" "${expect}" at)
        if(NOT status EQUAL 0 AND NOT at EQUAL -1 AND flat MATCHES "#1447")
            message(STATUS "ok: ${name} -- refused, naming ${expect}")
        else()
            set(failures "${failures}\n  ${name}: expected a refusal naming ${expect}, exit ${status}: ${flat}" PARENT_SCOPE)
        endif()
    endif()
    math(EXPR count "${ran} + 1")
    set(ran ${count} PARENT_SCOPE)
endfunction()

# control: the probe exactly as every executable compiles it.
probe_case("control: nothing planted" pass "${probePath}" "")

# attribute: a function asking for the instructions, which defines no unit-wide macro.
if(COMPILER_ID STREQUAL "MSVC")
    list(APPEND skipped "attribute: cl takes no target attribute, and needs none")
else()
    set(attributed "${SCRATCH}/attributed.cpp")
    file(WRITE "${attributed}" "__attribute__((target(\"${attribute}\"))) int Attributed(int x) { return x + 1; }\n#include \"${probePath}\"\n")
    probe_case("attribute: target(\"${attribute}\") on one function" pass "${attributed}" "")
endif()

# flag: the instruction set spelled on the command line.
if(FRONTEND STREQUAL "MSVC")
    if(clPlant STREQUAL "")
        list(APPEND skipped "flag: no cl spelling for this row")
    else()
        probe_case("flag: ${clPlant}" "${clMacro}" "${probePath}" "" "${clPlant}")
    endif()
else()
    probe_case("flag: ${plantFlag}" "${plantMacro}" "${probePath}" "" "${plantFlag}")
endif()

# environment: the route #1442's database cannot see.
if(COMPILER_ID STREQUAL "MSVC")
    if(clPlant STREQUAL "")
        list(APPEND skipped "environment: no cl spelling for this row")
    else()
        probe_case("environment: CL=${clPlant}" "${clMacro}" "${probePath}" "CL=${clPlant}")
    endif()
elseif(COMPILER_ID MATCHES "Clang")
    if(FRONTEND STREQUAL "MSVC")
        set(override "+/clang:${plantFlag}")
    else()
        set(override "+${plantFlag}")
    endif()
    probe_case("environment: CCC_OVERRIDE_OPTIONS=${override}" "${plantMacro}" "${probePath}" "CCC_OVERRIDE_OPTIONS=${override}")
else()
    list(APPEND skipped "environment: ${COMPILER_ID} reads no instruction set from its environment")
endif()

# config: a clang driver configuration file, the route the ticket names beside the environment.
if(COMPILER_ID MATCHES "Clang" AND FRONTEND STREQUAL "GNU")
    set(config "${SCRATCH}/planted.cfg")
    file(WRITE "${config}" "${plantFlag}\n")
    probe_case("config: --config=planted.cfg carrying ${plantFlag}" "${plantMacro}" "${probePath}" "" "--config=${config}")
else()
    list(APPEND skipped "config: only the clang driver in its GNU spelling reads --config")
endif()

foreach(reason IN LISTS skipped)
    message(STATUS "skipped: ${reason}")
endforeach()
message(STATUS "instruction-set-probe-selftest: ${ran} case(s) ran for ${COMPILER_ID} (${FRONTEND}) on the ${row} row")
if(ran LESS 2)
    message(FATAL_ERROR "instruction-set-probe-selftest: only ${ran} case(s) ran, so nothing was shown able to refuse")
endif()
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "instruction-set-probe-selftest: the probe did not decide as it must:${failures}")
endif()
