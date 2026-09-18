# SPDX-License-Identifier: Apache-2.0
# InstructionSetProbe.cmake - every executable compiles `src/tests/InstructionSetBaseline.cpp`,
# which refuses to compile when the compiler assumes an instruction set above the
# architecture's baseline (#1447).
#
# ## Why the BUILD compiles it
#
# The compiler's predefined macros are the one place every route to a wider instruction set
# arrives -- a flag, the build-time environment (`CL`, `_CL_`, `CCC_OVERRIDE_OPTIONS`), a
# driver configuration file, a compiler's built-in default -- and only the build compiles
# with the build's environment. A probe driven from ctest would ask ctest's environment,
# which is a different process's and need not agree. So the probe is a unit of each
# executable: compiled by the same driver, with that executable's own flags, at the moment
# its real units are, and a violation fails the build naming the macro.
#
# ## Why every EXECUTABLE, and not every target
#
# One probe object per executable is one per distinct link, compiled with that target's
# flags, and every route above reaches every compile of a build alike -- so it cannot miss
# them. What it can miss is a flag set on one LIBRARY target alone, and that is a flag on a
# command line, which `scripts/check-instruction-set-flags.cmake` (#1442) reads and names. A
# unit with no symbols in a STATIC library is also what the MSVC librarian warns about
# (LNK4221), which a warnings-as-errors build would turn into a failure of its own.
#
# Walks `src/` only: `vendor/` is another project's code, built here and shipped by nobody.
#
# Reads the build system as it stands, so it runs after every add_subdirectory() that
# defines a target, beside the error-popup and code-page attachments.

include(ProjectTargets)

set(FASTCACHED_INSTRUCTION_SET_PROBE "${CMAKE_SOURCE_DIR}/src/tests/InstructionSetBaseline.cpp")

function(fastcached_probe_instruction_set_baseline)
    fastcached_collect_executables("${CMAKE_SOURCE_DIR}" executables src)
    set(attached 0)
    foreach(executable IN LISTS executables)
        target_sources(${executable} PRIVATE "${FASTCACHED_INSTRUCTION_SET_PROBE}")
        math(EXPR attached "${attached} + 1")
    endforeach()
    # The count is printed because a walk that found nothing and a walk that attached
    # everywhere are otherwise the same silence. Zero is stated, not refused: a
    # configuration that builds no executable ships nothing this protects.
    if(attached EQUAL 0)
        message(STATUS "[InstructionSetProbe] probed no executable: this configuration builds none")
    else()
        message(STATUS "[InstructionSetProbe] instruction-set baseline probe attached to ${attached} executable(s)")
    endif()
endfunction()
