# SPDX-License-Identifier: Apache-2.0
# IncrementalLink.cmake - every MSVC-style link in a debug-info configuration is a full link.
#
# CMake's defaults for an MSVC-style toolchain link Debug and RelWithDebInfo with
# `/debug /INCREMENTAL`. Five incremental relinks of a test executable failed on one host with
# `LNK1163: invalid selection for COMDAT section` on the vftable of a header-inline class, and
# a plain retry linked each time. The cause is INCONCLUSIVE -- a standalone reproduction never
# failed, with the compiler cache or without it -- and what survives every reading is
# incremental-link state, which a link keeping no `.ilk` does not have. So this is NOT gated
# on the launcher. The evidence, the reasoning and the measured cost are in
# `.agent/rules/build-and-toolchain.md`, "An MSVC-style debug-info link is
# never incremental"; they are not restated here.
#
# `MSVC` covers cl and clang-cl. clang-cl links through lld-link, which never links
# incrementally and accepts `/INCREMENTAL:NO` silently, so one condition serves both. A
# GNU-style driver on Windows spells no such switch and is left alone.
#
# Directory-scoped and included before any dependency or target is added, so every executable
# and shared library in the build -- a fetched dependency's included -- reads the rewritten
# value. `ctest -R incremental-link` reads the GENERATED build.ninja rather than trusting this
# file: a module's configure output is its claim, the link line is the artefact.

include_guard(GLOBAL)

if(NOT MSVC)
    return()
endif()

# Remove every spelling of the switch the value already carries, then state the one we
# want. Removing rather than appending, because the answer must not depend on link.exe's
# last-one-wins; appending unconditionally, because `/debug` without any `/INCREMENTAL`
# links incrementally by default.
foreach(kind IN ITEMS EXE SHARED MODULE)
    foreach(config IN ITEMS DEBUG RELWITHDEBINFO)
        set(variable "CMAKE_${kind}_LINKER_FLAGS_${config}")
        separate_arguments(flags WINDOWS_COMMAND "${${variable}}")
        list(FILTER flags EXCLUDE REGEX "^[-/][Ii][Nn][Cc][Rr][Ee][Mm][Ee][Nn][Tt][Aa][Ll](:[Nn][Oo])?$")
        list(APPEND flags "/INCREMENTAL:NO")
        list(JOIN flags " " ${variable})
    endforeach()
endforeach()
