# SPDX-License-Identifier: Apache-2.0
#
# A test may not decide anything from the ARCHITECTURE it was compiled for.
#
# `#if defined(_M_ARM64)` in a test picks an expected value from a fact about the
# BUILD. The implementation it is testing picks its answer from the same fact. So
# both flip together, they agree on every machine CI owns, and the case is green
# under every build while being unable to fail for the reason it exists. That is
# `.agent/rules/testing.md`'s *assert what DISTINGUISHES, not what both sides
# produce*, with the preprocessor as the mechanism -- which is what makes it
# invisible to review, since both arms are in the diff and each reads correctly.
#
# It is not hypothetical. #146 was the MSVC bindir chosen by the architecture the
# binary was COMPILED for rather than the machine it RUNS on, and
# `ToolchainDiscovery_test.cpp` re-spelled the implementation's own condition to
# decide what to expect. A gate-class defect with a test pointed straight at it,
# which could not have caught it at any point.
#
# ---------------------------------------------------------------------------
# Why the OPERATING SYSTEM is deliberately out of scope
# ---------------------------------------------------------------------------
#
# This is the part a later reader will otherwise re-derive, or "fix" by widening
# the scan -- so it is written here, in the file that does the refusing.
#
# `#if defined(_WIN32)` in a test is SOUND. A Windows build runs on Windows;
# build time and run time necessarily agree, so a per-platform expectation stated
# as a literal is a real fact about the arm it is compiled in, and inverting the
# implementation's arm breaks the test. Measured on this tree at the time this
# check was written: 35 of 215 test files carry a platform or architecture
# directive, and 33 of them are that legitimate shape -- per-platform fixtures,
# `int` versus `socklen_t` in a syscall, an error-code table, an environment
# variable the test itself SETS so the implementation must independently agree.
#
# `#if defined(_M_ARM64)` is NOT sound, and the difference is not stylistic. An
# x64 build runs on ARM64 Windows routinely, under emulation, so the condition is
# a PROXY for the machine rather than a fact about it. The test and the
# implementation then agree perfectly while both are wrong about the host they
# are running on.
#
# So a scan over platform directives generally would refuse 33 correct files, and
# a guard that refuses correct code is disabled -- taking the real finding with
# it. This one refuses none of them.
#
# The remedy exists only for architecture for the same reason: there is a runtime
# fact to inject a seam around (`IToolchainHost::NativeArchitecture()`), where for
# the operating system there is no build-versus-run gap to inject.
#
# ---------------------------------------------------------------------------
# What is deliberately NOT covered
# ---------------------------------------------------------------------------
#
# `Platform/HostInfo.cpp`'s `CompiledArchitecture()` is a documented OPPOSITE
# decision and must keep its `#if`: it answers *what can this process run*, for
# which the compiled answer is the correct one, because an x86-64 process under
# emulation executes x86-64 code however the kernel describes the machine. This
# check is scoped to `*_test.cpp` and so cannot see it, which is correct rather
# than an oversight -- do not widen the scope to implementation files.
#
# Inputs:
#   SOURCE_DIR - the repository's src/ directory.

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "[architecture-conditioned-tests] SOURCE_DIR is required")
endif()

# The macros that name a target ARCHITECTURE, in both spellings every compiler
# this project builds with uses. Operating-system macros are absent by
# construction and the header above says why.
set(architectureMacros "_M_ARM64|_M_X64|_M_IX86|_M_ARM|__aarch64__|__x86_64__|__i386__|__arm__")

# Anchored on the DIRECTIVE, never on the macro name alone. `_M_ARM64` appears in
# prose in the rulebook, in AGENT.md and in comments explaining this very hazard,
# so an unanchored match finds the DOCUMENTATION of the problem and reports it as
# an instance of the problem.
set(directivePattern "^[ \t]*#[ \t]*(if|ifdef|ifndef|elif)[^\n]*(${architectureMacros})")

# A GLOB, never a file list: a list is exact about the files it knows and silent
# about the ones it does not, and silence reads identically to complete coverage.
file(GLOB_RECURSE testSources LIST_DIRECTORIES false "${SOURCE_DIR}/*_test.cpp")

# Quoted, because `if(VAR STREQUAL "")` does not fire when VAR is UNSET and an
# empty glob unsets it -- CMake then compares the literal string `testSources`
# against the empty string and the guard can never fire. This is the one failure
# mode a check like this must not have: reporting a clean tree because it read no
# files at all.
if("${testSources}" STREQUAL "")
    message(FATAL_ERROR
        "[architecture-conditioned-tests] found no *_test.cpp under ${SOURCE_DIR}.\n"
        "That is this check failing, not a clean tree.")
endif()

list(LENGTH testSources testFileCount)

set(offenders "")
foreach(source IN LISTS testSources)
    # Only MATCHING lines enter the list, so the bracket hazard that breaks other
    # `file(STRINGS)` readers cannot reach this one: a preprocessor directive
    # naming an architecture macro carries no `]` to merge two elements on.
    file(STRINGS "${source}" hits REGEX "${directivePattern}")
    foreach(hit IN LISTS hits)
        file(RELATIVE_PATH shown "${SOURCE_DIR}" "${source}")
        string(STRIP "${hit}" hit)
        list(APPEND offenders "  ${shown}: ${hit}")
    endforeach()
endforeach()

# ---------------------------------------------------------------------------
# POSITIVE CONTROL
# ---------------------------------------------------------------------------
#
# An empty offender list is the expected answer, which is exactly when a broken
# pattern is indistinguishable from a clean tree. So the same pattern is asked a
# question it MUST answer, and the check refuses when it cannot answer it.
#
# The control states WHAT it must find and never HOW MANY. `HostInfo.cpp`'s
# `CompiledArchitecture()` is the deliberate, documented site that must keep its
# `#if`, so that is the thing that must go on matching. A COUNT is a property of
# today's arms: adding or removing one makes a written-down number wrong while
# the tree is perfectly correct, and the next reader takes the mismatch for the
# instrument failing rather than for the tree changing. The count is still
# PRINTED below, as evidence of what was observed -- which is a different thing
# from an expectation, and only one of the two can drift.
#
# `ToolchainDiscovery.cpp` is deliberately NOT a control subject, and this is the
# same hazard having already fired once. It carried four of these directives until
# #1133 replaced them with an `EnumTable` indexed by
# `IToolchainHost::NativeArchitecture()` -- the runtime seam this whole check
# exists to push people toward. A control naming a file that cannot match is
# quietly weaker than it reads, so re-adding it expecting hits is the mistake this
# paragraph exists to prevent.
#
# Three ways this stops being a control, wanting three different repairs, so they
# are three refusals rather than one: the file is gone, the documented site inside
# it is gone, or the pattern has stopped matching. Folded into one message, two of
# them send the reader off to re-derive a regex that was never broken.
set(controlFile "${SOURCE_DIR}/FastCache/Platform/HostInfo.cpp")
set(controlSubject "CompiledArchitecture")

if(NOT EXISTS "${controlFile}")
    message(FATAL_ERROR
        "[architecture-conditioned-tests] the positive control has no subject file:\n"
        "  ${controlFile}\n"
        "This check therefore has no control, which is not the same as a clean tree.\n"
        "The pattern is not implicated -- point the control at wherever\n"
        "`${controlSubject}()` lives now.")
endif()

# Emptiness only, never a count, so the `]]` in `[[nodiscard]]` merging two list
# elements cannot change the answer: a merge can never make a non-empty list
# empty.
file(STRINGS "${controlFile}" controlSubjectLines REGEX "${controlSubject}")
if("${controlSubjectLines}" STREQUAL "")
    message(FATAL_ERROR
        "[architecture-conditioned-tests] the positive control's subject is gone:\n"
        "  ${controlSubject}() in ${controlFile}\n"
        "That function is the documented site that must KEEP its architecture `#if`,\n"
        "so it is what this control watches. If it was renamed, rename it here; if it\n"
        "was converted to a runtime seam, this control needs a different subject and\n"
        "choosing one is part of that change. The pattern is not implicated.")
endif()

file(STRINGS "${controlFile}" controlHitLines REGEX "${directivePattern}")
list(LENGTH controlHitLines controlHits)

if(controlHits EQUAL 0)
    message(FATAL_ERROR
        "[architecture-conditioned-tests] the positive control found NOTHING.\n"
        "`${controlSubject}()` is still in ${controlFile}, and the pattern below\n"
        "matched none of the architecture directives it is built out of. So the\n"
        "pattern has stopped matching, and its clean verdict about the test files\n"
        "above says nothing at all.\n"
        "  pattern: ${directivePattern}")
endif()

if(NOT offenders STREQUAL "")
    list(JOIN offenders "\n" shownOffenders)
    message(FATAL_ERROR
        "[architecture-conditioned-tests] a test decides something from the architecture it was COMPILED for:\n"
        "${shownOffenders}\n"
        "\n"
        "The implementation under test picks its answer from the same fact, so both flip\n"
        "together and the case is green under every build -- it cannot fail for the reason\n"
        "it exists. An x64 build runs on ARM64 Windows routinely, so this condition is a\n"
        "PROXY for the machine rather than a fact about it (#146, #1129).\n"
        "\n"
        "The remedy is to take the expectation from an INJECTED answer instead:\n"
        "`IToolchainHost::NativeArchitecture()` is asked of the host, so one Linux runner\n"
        "can drive every architecture through `ScriptedToolchainHost::WithNativeArchitecture`.\n"
        "See ToolchainDiscovery_test.cpp for the shape.\n"
        "\n"
        "`#if defined(_WIN32)` is NOT this defect and is not refused here -- a Windows\n"
        "build runs on Windows, so that condition is a fact rather than a proxy. The\n"
        "header of this script carries the reasoning; do not widen it.")
endif()

message(STATUS
    "[architecture-conditioned-tests] ${testFileCount} test source(s) carry no architecture-conditioned "
    "directive (positive control: ${controlHits} directive(s) in ${controlSubject}())")
